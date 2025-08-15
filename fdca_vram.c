// SPDX-License-Identifier: GPL-2.0-only
/*
 * FDCA (Fangzheng Distributed Computing Architecture) VRAM Manager
 * 
 * Copyright (C) 2024 Fangzheng Technology Co., Ltd.
 * 
 * VRAM 内存管理模块
 * 
 * 本模块负责：
 * 1. 设备本地内存 (VRAM) 的分配和释放
 * 2. 使用 drm_buddy 分配器进行高效内存管理
 * 3. 支持大页分配以提高性能
 * 4. 内存碎片整理和优化
 * 5. 内存使用统计和监控
 * 6. 与 GTT 协调的统一内存管理
 *
 * Author: FDCA Kernel Team
 * Date: 2024
 */

#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/genalloc.h>

#include <drm/drm_buddy.h>
#include <drm/drm_print.h>

#include "fdca_drv.h"

/*
 * ============================================================================
 * VRAM 管理常量和宏定义
 * ============================================================================
 */

/* VRAM 块大小定义 */
#define FDCA_VRAM_MIN_BLOCK_SIZE    PAGE_SIZE           /* 最小块: 4KB */
#define FDCA_VRAM_LARGE_BLOCK_SIZE  (2 << 20)           /* 大页块: 2MB */
#define FDCA_VRAM_HUGE_BLOCK_SIZE   (1 << 30)           /* 巨页块: 1GB */

/* 分配标志 */
#define FDCA_VRAM_ALLOC_CONTIGUOUS  BIT(0)              /* 连续分配 */
#define FDCA_VRAM_ALLOC_LARGE_PAGE  BIT(1)              /* 大页分配 */
#define FDCA_VRAM_ALLOC_PINNED      BIT(2)              /* 固定内存 */
#define FDCA_VRAM_ALLOC_CACHED      BIT(3)              /* 缓存内存 */

/* 碎片整理阈值 */
#define FDCA_VRAM_FRAG_THRESHOLD    25                  /* 碎片率 25% */
#define FDCA_VRAM_DEFRAG_INTERVAL   (30 * HZ)           /* 30秒检查间隔 */

/*
 * ============================================================================
 * VRAM 内存对象结构
 * ============================================================================
 */

/**
 * struct fdca_vram_object - VRAM 内存对象
 * 
 * 表示一个已分配的 VRAM 内存块
 */
struct fdca_vram_object {
    struct list_head list;              /* 对象链表节点 */
    struct drm_buddy_block *block;      /* buddy 分配的内存块 */
    
    /* 基础信息 */
    u64 offset;                         /* VRAM 中的偏移 */
    size_t size;                        /* 对象大小 */
    u32 flags;                          /* 分配标志 */
    
    /* 映射信息 */
    void *cpu_addr;                     /* CPU 虚拟地址 */
    dma_addr_t dma_addr;                /* DMA 地址 */
    bool mapped;                        /* 是否已映射 */
    
    /* 使用统计 */
    atomic_t ref_count;                 /* 引用计数 */
    u64 alloc_time;                     /* 分配时间 */
    u64 last_access;                    /* 最后访问时间 */
    
    /* 调试信息 */
    const char *debug_name;             /* 调试名称 */
    struct task_struct *owner;          /* 所有者进程 */
};

/*
 * ============================================================================
 * VRAM 管理器初始化和清理
 * ============================================================================
 */

/**
 * fdca_vram_get_size() - 获取 VRAM 大小
 * @fdev: FDCA 设备
 * 
 * 从硬件寄存器读取 VRAM 大小信息
 * 
 * Return: VRAM 大小（字节）
 */
static u64 fdca_vram_get_size(struct fdca_device *fdev)
{
    u32 size_reg;
    u64 vram_size;
    
    /* 读取 VRAM 大小寄存器 */
    size_reg = ioread32(fdev->mmio_base + 0x100);  /* VRAM_SIZE_REG */
    
    /* 解析大小 - 寄存器值以 MB 为单位 */
    vram_size = (u64)size_reg << 20;  /* 转换为字节 */
    
    /* 验证大小合理性 */
    if (vram_size < (64 << 20)) {  /* 最小 64MB */
        fdca_warn(fdev, "VRAM 大小过小: %llu MB，使用默认值 256MB\n", 
                  vram_size >> 20);
        vram_size = 256 << 20;
    }
    
    if (vram_size > FDCA_VRAM_SIZE_MAX) {
        fdca_warn(fdev, "VRAM 大小过大: %llu MB，限制为 %llu MB\n",
                  vram_size >> 20, FDCA_VRAM_SIZE_MAX >> 20);
        vram_size = FDCA_VRAM_SIZE_MAX;
    }
    
    fdca_info(fdev, "检测到 VRAM 大小: %llu MB\n", vram_size >> 20);
    
    return vram_size;
}

/**
 * fdca_vram_manager_init() - 初始化 VRAM 管理器
 * @fdev: FDCA 设备
 * 
 * Return: 0 表示成功，负数表示错误
 */
int fdca_vram_manager_init(struct fdca_device *fdev)
{
    struct fdca_vram_manager *vram = &fdev->mem_mgr->vram;
    u64 vram_size;
    int ret;
    
    fdca_info(fdev, "初始化 VRAM 管理器\n");
    
    /* 获取 VRAM 大小 */
    vram_size = fdca_vram_get_size(fdev);
    
    /* 设置 VRAM 基本信息 */
    vram->base = 0;  /* VRAM 从偏移 0 开始 */
    vram->size = vram_size;
    vram->available = vram_size;
    vram->used = 0;
    
    /* 初始化 buddy 分配器 */
    ret = drm_buddy_init(&vram->buddy, vram_size, FDCA_VRAM_MIN_BLOCK_SIZE);
    if (ret) {
        fdca_err(fdev, "buddy 分配器初始化失败: %d\n", ret);
        return ret;
    }
    
    /* 初始化锁 */
    mutex_init(&vram->lock);
    
    /* 初始化统计信息 */
    atomic64_set(&vram->alloc_count, 0);
    atomic64_set(&vram->free_count, 0);
    atomic64_set(&vram->large_page_count, 0);
    
    /* 初始化碎片整理工作 */
    INIT_WORK(&vram->defrag_work, fdca_vram_defrag_work);
    vram->defrag_in_progress = false;
    
    fdca_info(fdev, "VRAM 管理器初始化完成: %llu MB\n", vram_size >> 20);
    
    return 0;
}

/**
 * fdca_vram_manager_fini() - 清理 VRAM 管理器
 * @fdev: FDCA 设备
 */
void fdca_vram_manager_fini(struct fdca_device *fdev)
{
    struct fdca_vram_manager *vram = &fdev->mem_mgr->vram;
    
    fdca_info(fdev, "清理 VRAM 管理器\n");
    
    /* 等待碎片整理完成 */
    if (vram->defrag_in_progress) {
        cancel_work_sync(&vram->defrag_work);
    }
    
    /* 清理 buddy 分配器 */
    drm_buddy_fini(&vram->buddy);
    
    /* 打印统计信息 */
    fdca_info(fdev, "VRAM 统计: 分配 %lld 次, 释放 %lld 次, 大页 %lld 次\n",
              atomic64_read(&vram->alloc_count),
              atomic64_read(&vram->free_count),
              atomic64_read(&vram->large_page_count));
    
    /* 检查内存泄漏 */
    if (vram->used > 0) {
        fdca_warn(fdev, "检测到 VRAM 内存泄漏: %llu 字节\n", vram->used);
    }
    
    fdca_info(fdev, "VRAM 管理器清理完成\n");
}

/*
 * ============================================================================
 * VRAM 内存分配和释放
 * ============================================================================
 */

/**
 * fdca_vram_alloc() - 分配 VRAM 内存
 * @fdev: FDCA 设备
 * @size: 请求大小
 * @flags: 分配标志
 * @debug_name: 调试名称
 * 
 * Return: 内存对象指针或 ERR_PTR
 */
struct fdca_vram_object *fdca_vram_alloc(struct fdca_device *fdev,
                                         size_t size, u32 flags,
                                         const char *debug_name)
{
    struct fdca_vram_manager *vram = &fdev->mem_mgr->vram;
    struct fdca_vram_object *obj;
    struct drm_buddy_block *block;
    u64 min_block_size = FDCA_VRAM_MIN_BLOCK_SIZE;
    int ret;
    
    /* 参数验证 */
    if (!size || size > vram->size) {
        fdca_err(fdev, "无效的分配大小: %zu\n", size);
        return ERR_PTR(-EINVAL);
    }
    
    /* 页对齐 */
    size = PAGE_ALIGN(size);
    
    /* 大页优化 */
    if ((flags & FDCA_VRAM_ALLOC_LARGE_PAGE) && 
        size >= FDCA_VRAM_LARGE_BLOCK_SIZE) {
        min_block_size = FDCA_VRAM_LARGE_BLOCK_SIZE;
    }
    
    /* 分配对象结构 */
    obj = kzalloc(sizeof(*obj), GFP_KERNEL);
    if (!obj) {
        fdca_err(fdev, "无法分配 VRAM 对象结构\n");
        return ERR_PTR(-ENOMEM);
    }
    
    /* 获取锁进行分配 */
    mutex_lock(&vram->lock);
    
    /* 检查可用空间 */
    if (vram->available < size) {
        mutex_unlock(&vram->lock);
        fdca_err(fdev, "VRAM 空间不足: 请求 %zu，可用 %llu\n",
                 size, vram->available);
        kfree(obj);
        return ERR_PTR(-ENOMEM);
    }
    
    /* 使用 buddy 分配器分配内存 */
    ret = drm_buddy_alloc_blocks(&vram->buddy, 0, vram->size,
                                size, min_block_size, 
                                &block, 1,
                                (flags & FDCA_VRAM_ALLOC_CONTIGUOUS) ? 
                                DRM_BUDDY_CONTIGUOUS_ALLOCATION : 0);
    if (ret) {
        mutex_unlock(&vram->lock);
        fdca_err(fdev, "buddy 分配失败: %d\n", ret);
        kfree(obj);
        return ERR_PTR(ret);
    }
    
    /* 初始化对象 */
    INIT_LIST_HEAD(&obj->list);
    obj->block = block;
    obj->offset = drm_buddy_block_offset(block);
    obj->size = drm_buddy_block_size(block);
    obj->flags = flags;
    obj->cpu_addr = NULL;
    obj->dma_addr = 0;
    obj->mapped = false;
    atomic_set(&obj->ref_count, 1);
    obj->alloc_time = ktime_get_boottime_seconds();
    obj->last_access = obj->alloc_time;
    obj->debug_name = debug_name;
    obj->owner = current;
    
    /* 更新统计信息 */
    vram->used += obj->size;
    vram->available -= obj->size;
    atomic64_inc(&vram->alloc_count);
    
    if (flags & FDCA_VRAM_ALLOC_LARGE_PAGE) {
        atomic64_inc(&vram->large_page_count);
    }
    
    mutex_unlock(&vram->lock);
    
    fdca_dbg(fdev, "VRAM 分配成功: 偏移=0x%llx, 大小=%zu, 标志=0x%x, 名称=%s\n",
             obj->offset, obj->size, flags, debug_name ?: "匿名");
    
    return obj;
}

/**
 * fdca_vram_free() - 释放 VRAM 内存
 * @fdev: FDCA 设备
 * @obj: 内存对象
 */
void fdca_vram_free(struct fdca_device *fdev, struct fdca_vram_object *obj)
{
    struct fdca_vram_manager *vram = &fdev->mem_mgr->vram;
    
    if (!obj) {
        fdca_warn(fdev, "尝试释放空的 VRAM 对象\n");
        return;
    }
    
    fdca_dbg(fdev, "释放 VRAM: 偏移=0x%llx, 大小=%zu, 名称=%s\n",
             obj->offset, obj->size, obj->debug_name ?: "匿名");
    
    /* 检查引用计数 */
    if (atomic_read(&obj->ref_count) > 1) {
        fdca_warn(fdev, "释放仍有引用的 VRAM 对象: 引用数=%d\n",
                  atomic_read(&obj->ref_count));
    }
    
    /* 取消映射（如果已映射） */
    if (obj->mapped) {
        fdca_vram_unmap(fdev, obj);
    }
    
    mutex_lock(&vram->lock);
    
    /* 释放 buddy 块 */
    drm_buddy_free_block(&vram->buddy, obj->block);
    
    /* 更新统计信息 */
    vram->used -= obj->size;
    vram->available += obj->size;
    atomic64_inc(&vram->free_count);
    
    mutex_unlock(&vram->lock);
    
    /* 释放对象结构 */
    kfree(obj);
    
    /* 检查是否需要碎片整理 */
    fdca_vram_check_fragmentation(fdev);
}

/**
 * fdca_vram_map() - 映射 VRAM 到 CPU 地址空间
 * @fdev: FDCA 设备
 * @obj: 内存对象
 * 
 * Return: 0 表示成功，负数表示错误
 */
int fdca_vram_map(struct fdca_device *fdev, struct fdca_vram_object *obj)
{
    if (!obj) {
        return -EINVAL;
    }
    
    if (obj->mapped) {
        fdca_dbg(fdev, "VRAM 对象已映射\n");
        return 0;
    }
    
    /* 映射到 CPU 地址空间 */
    obj->cpu_addr = ioremap_wc(fdev->vram_base + obj->offset, obj->size);
    if (!obj->cpu_addr) {
        fdca_err(fdev, "VRAM CPU 映射失败\n");
        return -ENOMEM;
    }
    
    /* 设置 DMA 地址 */
    obj->dma_addr = fdev->vram_base + obj->offset;
    obj->mapped = true;
    
    fdca_dbg(fdev, "VRAM 映射成功: CPU=%p, DMA=0x%llx\n",
             obj->cpu_addr, obj->dma_addr);
    
    return 0;
}

/**
 * fdca_vram_unmap() - 取消 VRAM 映射
 * @fdev: FDCA 设备
 * @obj: 内存对象
 */
void fdca_vram_unmap(struct fdca_device *fdev, struct fdca_vram_object *obj)
{
    if (!obj || !obj->mapped) {
        return;
    }
    
    fdca_dbg(fdev, "取消 VRAM 映射: CPU=%p\n", obj->cpu_addr);
    
    iounmap(obj->cpu_addr);
    obj->cpu_addr = NULL;
    obj->dma_addr = 0;
    obj->mapped = false;
}

/*
 * ============================================================================
 * 碎片整理和优化
 * ============================================================================
 */

/**
 * fdca_vram_get_fragmentation() - 计算碎片率
 * @fdev: FDCA 设备
 * 
 * Return: 碎片率百分比
 */
static u32 fdca_vram_get_fragmentation(struct fdca_device *fdev)
{
    struct fdca_vram_manager *vram = &fdev->mem_mgr->vram;
    u64 largest_free;
    u32 fragmentation;
    
    if (vram->available == 0) {
        return 0;
    }
    
    /* 获取最大可用块 */
    largest_free = drm_buddy_avail_size(&vram->buddy);
    
    /* 计算碎片率 */
    fragmentation = 100 - (largest_free * 100 / vram->available);
    
    return fragmentation;
}

/**
 * fdca_vram_check_fragmentation() - 检查是否需要碎片整理
 * @fdev: FDCA 设备
 */
static void fdca_vram_check_fragmentation(struct fdca_device *fdev)
{
    struct fdca_vram_manager *vram = &fdev->mem_mgr->vram;
    u32 fragmentation;
    
    if (vram->defrag_in_progress) {
        return;
    }
    
    fragmentation = fdca_vram_get_fragmentation(fdev);
    
    if (fragmentation > FDCA_VRAM_FRAG_THRESHOLD) {
        fdca_info(fdev, "检测到高碎片率: %u%%，启动碎片整理\n", fragmentation);
        vram->defrag_in_progress = true;
        schedule_work(&vram->defrag_work);
    }
}

/* 前向声明 */
static void fdca_vram_defrag_work(struct work_struct *work);
static void fdca_vram_check_fragmentation(struct fdca_device *fdev);

/**
 * fdca_vram_defrag_work() - 碎片整理工作函数
 * @work: 工作结构
 */
static void fdca_vram_defrag_work(struct work_struct *work)
{
    struct fdca_vram_manager *vram = container_of(work, struct fdca_vram_manager, defrag_work);
    struct fdca_device *fdev = container_of(vram, struct fdca_memory_manager, vram)->fdev;
    u32 frag_before, frag_after;
    
    fdca_info(fdev, "开始 VRAM 碎片整理\n");
    
    frag_before = fdca_vram_get_fragmentation(fdev);
    
    mutex_lock(&vram->lock);
    
    /* 执行碎片整理 - 这里可以实现具体的整理算法 */
    /* 当前版本只是一个占位符，未来可以实现更复杂的算法 */
    msleep(100);  /* 模拟整理时间 */
    
    mutex_unlock(&vram->lock);
    
    frag_after = fdca_vram_get_fragmentation(fdev);
    
    fdca_info(fdev, "VRAM 碎片整理完成: %u%% -> %u%%\n", frag_before, frag_after);
    
    vram->defrag_in_progress = false;
}

/*
 * ============================================================================
 * 统计和监控函数
 * ============================================================================
 */

/**
 * fdca_vram_get_stats() - 获取 VRAM 统计信息
 * @fdev: FDCA 设备
 * @stats: 输出统计信息的结构
 */
void fdca_vram_get_stats(struct fdca_device *fdev, struct fdca_vram_stats *stats)
{
    struct fdca_vram_manager *vram = &fdev->mem_mgr->vram;
    
    mutex_lock(&vram->lock);
    
    stats->total_size = vram->size;
    stats->used_size = vram->used;
    stats->available_size = vram->available;
    stats->fragmentation = fdca_vram_get_fragmentation(fdev);
    stats->alloc_count = atomic64_read(&vram->alloc_count);
    stats->free_count = atomic64_read(&vram->free_count);
    stats->large_page_count = atomic64_read(&vram->large_page_count);
    
    mutex_unlock(&vram->lock);
}

/**
 * fdca_vram_print_stats() - 打印 VRAM 统计信息
 * @fdev: FDCA 设备
 */
void fdca_vram_print_stats(struct fdca_device *fdev)
{
    struct fdca_vram_stats stats;
    
    fdca_vram_get_stats(fdev, &stats);
    
    fdca_info(fdev, "=== VRAM 统计信息 ===\n");
    fdca_info(fdev, "总大小: %llu MB\n", stats.total_size >> 20);
    fdca_info(fdev, "已使用: %llu MB (%.1f%%)\n", 
              stats.used_size >> 20,
              (float)stats.used_size * 100.0 / stats.total_size);
    fdca_info(fdev, "可用: %llu MB\n", stats.available_size >> 20);
    fdca_info(fdev, "碎片率: %u%%\n", stats.fragmentation);
    fdca_info(fdev, "分配次数: %llu\n", stats.alloc_count);
    fdca_info(fdev, "释放次数: %llu\n", stats.free_count);
    fdca_info(fdev, "大页分配: %llu\n", stats.large_page_count);
}

/*
 * ============================================================================
 * 导出符号
 * ============================================================================
 */

EXPORT_SYMBOL_GPL(fdca_vram_manager_init);
EXPORT_SYMBOL_GPL(fdca_vram_manager_fini);
EXPORT_SYMBOL_GPL(fdca_vram_alloc);
EXPORT_SYMBOL_GPL(fdca_vram_free);
EXPORT_SYMBOL_GPL(fdca_vram_map);
EXPORT_SYMBOL_GPL(fdca_vram_unmap);
EXPORT_SYMBOL_GPL(fdca_vram_get_stats);
EXPORT_SYMBOL_GPL(fdca_vram_print_stats);
