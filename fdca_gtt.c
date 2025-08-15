// SPDX-License-Identifier: GPL-2.0-only
/*
 * FDCA (Fangzheng Distributed Computing Architecture) GTT Manager
 * 
 * Copyright (C) 2024 Fangzheng Technology Co., Ltd.
 * 
 * GTT (Graphics Translation Table) 内存管理模块
 * 
 * 本模块负责：
 * 1. 虚拟地址空间的分配和管理
 * 2. 使用 drm_mm 进行地址空间管理
 * 3. 页表的创建、更新和清理
 * 4. 系统内存到设备地址空间的映射
 * 5. IOMMU 支持和地址转换
 * 6. 大页支持和地址空间优化
 *
 * Author: FDCA Kernel Team
 * Date: 2024
 */

#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/iommu.h>
#include <linux/mm.h>
#include <linux/highmem.h>

#include <drm/drm_mm.h>
#include <drm/drm_cache.h>

#include "fdca_drv.h"

/*
 * ============================================================================
 * GTT 管理常量和宏定义
 * ============================================================================
 */

/* GTT 页表项定义 */
#define FDCA_GTT_PTE_VALID      BIT_ULL(0)      /* 页表项有效 */
#define FDCA_GTT_PTE_READABLE   BIT_ULL(1)      /* 可读 */
#define FDCA_GTT_PTE_WRITABLE   BIT_ULL(2)      /* 可写 */
#define FDCA_GTT_PTE_CACHEABLE  BIT_ULL(3)      /* 可缓存 */
#define FDCA_GTT_PTE_LARGE      BIT_ULL(4)      /* 大页 */
#define FDCA_GTT_PTE_ADDR_MASK  GENMASK_ULL(51, 12) /* 地址掩码 */

/* GTT 地址空间配置 */
#define FDCA_GTT_START_ADDR     0x100000000ULL  /* 4GB 起始地址 */
#define FDCA_GTT_APERTURE_SIZE  (4ULL << 30)    /* 4GB 孔径大小 */
#define FDCA_GTT_MAX_SIZE       (256ULL << 30)  /* 最大 256GB */

/* 页表级别 */
#define FDCA_GTT_LEVEL_0        0               /* L0: 1GB 页 */
#define FDCA_GTT_LEVEL_1        1               /* L1: 2MB 页 */
#define FDCA_GTT_LEVEL_2        2               /* L2: 4KB 页 */
#define FDCA_GTT_MAX_LEVELS     3

/*
 * ============================================================================
 * GTT 页表项和映射结构
 * ============================================================================
 */

/**
 * struct fdca_gtt_entry - GTT 映射条目
 * 
 * 表示一个 GTT 地址空间映射
 */
struct fdca_gtt_entry {
    struct drm_mm_node node;            /* drm_mm 节点 */
    struct list_head list;              /* 映射列表节点 */
    
    /* 映射信息 */
    u64 gpu_addr;                       /* GPU 虚拟地址 */
    struct page **pages;                /* 页面数组 */
    dma_addr_t *dma_addrs;              /* DMA 地址数组 */
    u32 num_pages;                      /* 页面数量 */
    
    /* 属性 */
    u32 flags;                          /* 映射标志 */
    bool coherent;                      /* 是否一致性映射 */
    bool large_pages;                   /* 是否使用大页 */
    
    /* 页表信息 */
    u32 *pte_indices;                   /* 页表项索引数组 */
    
    /* 统计信息 */
    u64 map_time;                       /* 映射时间 */
    atomic_t access_count;              /* 访问计数 */
    
    /* 调试信息 */
    const char *debug_name;             /* 调试名称 */
    struct task_struct *owner;          /* 所有者进程 */
};

/*
 * ============================================================================
 * GTT 管理器初始化和清理
 * ============================================================================
 */

/**
 * fdca_gtt_get_aperture_size() - 获取 GTT 孔径大小
 * @fdev: FDCA 设备
 * 
 * Return: GTT 孔径大小（字节）
 */
static u64 fdca_gtt_get_aperture_size(struct fdca_device *fdev)
{
    u32 aperture_reg;
    u64 aperture_size;
    
    /* 读取孔径大小寄存器 */
    aperture_reg = ioread32(fdev->mmio_base + 0x104);  /* GTT_APERTURE_REG */
    
    /* 解析大小 - 寄存器值以 MB 为单位 */
    aperture_size = (u64)aperture_reg << 20;
    
    /* 验证大小合理性 */
    if (aperture_size < FDCA_GTT_APERTURE_SIZE) {
        fdca_warn(fdev, "GTT 孔径过小: %llu MB，使用默认值\n", 
                  aperture_size >> 20);
        aperture_size = FDCA_GTT_APERTURE_SIZE;
    }
    
    if (aperture_size > FDCA_GTT_MAX_SIZE) {
        fdca_warn(fdev, "GTT 孔径过大: %llu MB，限制为 %llu MB\n",
                  aperture_size >> 20, FDCA_GTT_MAX_SIZE >> 20);
        aperture_size = FDCA_GTT_MAX_SIZE;
    }
    
    fdca_info(fdev, "GTT 孔径大小: %llu MB\n", aperture_size >> 20);
    
    return aperture_size;
}

/**
 * fdca_gtt_init_page_table() - 初始化页表
 * @fdev: FDCA 设备
 * 
 * Return: 0 表示成功，负数表示错误
 */
static int fdca_gtt_init_page_table(struct fdca_device *fdev)
{
    struct fdca_gtt_manager *gtt = &fdev->mem_mgr->gtt;
    u64 pt_size;
    
    /* 计算页表大小 - 每个页表项 8 字节 */
    gtt->num_entries = gtt->size / gtt->page_size;
    pt_size = gtt->num_entries * sizeof(u64);
    
    fdca_info(fdev, "初始化页表: %u 项, 大小 %llu KB\n",
              gtt->num_entries, pt_size >> 10);
    
    /* 分配页表内存 */
    gtt->page_table = dma_alloc_coherent(fdev->dev, pt_size,
                                        &gtt->page_table_dma, GFP_KERNEL);
    if (!gtt->page_table) {
        fdca_err(fdev, "无法分配页表内存\n");
        return -ENOMEM;
    }
    
    /* 清零页表 */
    memset(gtt->page_table, 0, pt_size);
    
    /* 写入页表基址到硬件 */
    iowrite32(lower_32_bits(gtt->page_table_dma), 
              fdev->mmio_base + 0x108);  /* GTT_BASE_LOW */
    iowrite32(upper_32_bits(gtt->page_table_dma), 
              fdev->mmio_base + 0x10C);  /* GTT_BASE_HIGH */
    
    fdca_info(fdev, "页表基址: 物理=0x%llx, 虚拟=%p\n",
              gtt->page_table_dma, gtt->page_table);
    
    return 0;
}

/**
 * fdca_gtt_manager_init() - 初始化 GTT 管理器
 * @fdev: FDCA 设备
 * 
 * Return: 0 表示成功，负数表示错误
 */
int fdca_gtt_manager_init(struct fdca_device *fdev)
{
    struct fdca_gtt_manager *gtt = &fdev->mem_mgr->gtt;
    u64 aperture_size;
    int ret;
    
    fdca_info(fdev, "初始化 GTT 管理器\n");
    
    /* 获取孔径大小 */
    aperture_size = fdca_gtt_get_aperture_size(fdev);
    
    /* 设置 GTT 基本信息 */
    gtt->base = FDCA_GTT_START_ADDR;
    gtt->size = aperture_size;
    gtt->page_size = PAGE_SIZE;
    
    /* 初始化地址空间管理器 */
    drm_mm_init(&gtt->mm, gtt->base, gtt->size);
    
    /* 初始化锁 */
    mutex_init(&gtt->lock);
    
    /* 初始化页表 */
    ret = fdca_gtt_init_page_table(fdev);
    if (ret) {
        fdca_err(fdev, "页表初始化失败: %d\n", ret);
        goto err_cleanup_mm;
    }
    
    /* 初始化统计信息 */
    atomic64_set(&gtt->map_count, 0);
    atomic64_set(&gtt->unmap_count, 0);
    
    fdca_info(fdev, "GTT 管理器初始化完成: 基址=0x%llx, 大小=%llu MB\n",
              gtt->base, gtt->size >> 20);
    
    return 0;
    
err_cleanup_mm:
    drm_mm_takedown(&gtt->mm);
    return ret;
}

/**
 * fdca_gtt_manager_fini() - 清理 GTT 管理器
 * @fdev: FDCA 设备
 */
void fdca_gtt_manager_fini(struct fdca_device *fdev)
{
    struct fdca_gtt_manager *gtt = &fdev->mem_mgr->gtt;
    u64 pt_size;
    
    fdca_info(fdev, "清理 GTT 管理器\n");
    
    /* 释放页表内存 */
    if (gtt->page_table) {
        pt_size = gtt->num_entries * sizeof(u64);
        dma_free_coherent(fdev->dev, pt_size, gtt->page_table, 
                         gtt->page_table_dma);
        gtt->page_table = NULL;
    }
    
    /* 清理地址空间管理器 */
    drm_mm_takedown(&gtt->mm);
    
    /* 打印统计信息 */
    fdca_info(fdev, "GTT 统计: 映射 %lld 次, 解映射 %lld 次\n",
              atomic64_read(&gtt->map_count),
              atomic64_read(&gtt->unmap_count));
    
    fdca_info(fdev, "GTT 管理器清理完成\n");
}

/*
 * ============================================================================
 * GTT 地址空间分配和释放
 * ============================================================================
 */

/**
 * fdca_gtt_alloc_space() - 分配 GTT 地址空间
 * @fdev: FDCA 设备
 * @size: 请求大小
 * @alignment: 对齐要求
 * 
 * Return: GTT 映射条目指针或 ERR_PTR
 */
static struct fdca_gtt_entry *fdca_gtt_alloc_space(struct fdca_device *fdev,
                                                   u64 size, u64 alignment)
{
    struct fdca_gtt_manager *gtt = &fdev->mem_mgr->gtt;
    struct fdca_gtt_entry *entry;
    int ret;
    
    /* 分配条目结构 */
    entry = kzalloc(sizeof(*entry), GFP_KERNEL);
    if (!entry) {
        return ERR_PTR(-ENOMEM);
    }
    
    /* 初始化条目 */
    INIT_LIST_HEAD(&entry->list);
    atomic_set(&entry->access_count, 0);
    entry->map_time = ktime_get_boottime_seconds();
    entry->owner = current;
    
    mutex_lock(&gtt->lock);
    
    /* 分配地址空间 */
    ret = drm_mm_insert_node_generic(&gtt->mm, &entry->node, size, alignment,
                                    0, DRM_MM_SEARCH_DEFAULT);
    if (ret) {
        mutex_unlock(&gtt->lock);
        fdca_err(fdev, "GTT 地址空间分配失败: %d\n", ret);
        kfree(entry);
        return ERR_PTR(ret);
    }
    
    entry->gpu_addr = entry->node.start;
    
    mutex_unlock(&gtt->lock);
    
    fdca_dbg(fdev, "GTT 地址分配: 0x%llx, 大小=%llu\n",
             entry->gpu_addr, size);
    
    return entry;
}

/**
 * fdca_gtt_free_space() - 释放 GTT 地址空间
 * @fdev: FDCA 设备
 * @entry: GTT 映射条目
 */
static void fdca_gtt_free_space(struct fdca_device *fdev, 
                                struct fdca_gtt_entry *entry)
{
    struct fdca_gtt_manager *gtt = &fdev->mem_mgr->gtt;
    
    if (!entry) {
        return;
    }
    
    fdca_dbg(fdev, "GTT 地址释放: 0x%llx\n", entry->gpu_addr);
    
    mutex_lock(&gtt->lock);
    drm_mm_remove_node(&entry->node);
    mutex_unlock(&gtt->lock);
    
    kfree(entry);
}

/*
 * ============================================================================
 * 页表操作函数
 * ============================================================================
 */

/**
 * fdca_gtt_get_pte_index() - 获取页表项索引
 * @gtt: GTT 管理器
 * @gpu_addr: GPU 地址
 * 
 * Return: 页表项索引
 */
static u32 fdca_gtt_get_pte_index(struct fdca_gtt_manager *gtt, u64 gpu_addr)
{
    return (gpu_addr - gtt->base) / gtt->page_size;
}

/**
 * fdca_gtt_set_pte() - 设置页表项
 * @fdev: FDCA 设备
 * @index: 页表项索引
 * @dma_addr: DMA 地址
 * @flags: 标志
 */
static void fdca_gtt_set_pte(struct fdca_device *fdev, u32 index,
                             dma_addr_t dma_addr, u32 flags)
{
    struct fdca_gtt_manager *gtt = &fdev->mem_mgr->gtt;
    u64 pte_value = 0;
    
    if (index >= gtt->num_entries) {
        fdca_err(fdev, "页表项索引越界: %u >= %u\n", index, gtt->num_entries);
        return;
    }
    
    /* 构造页表项 */
    pte_value = (dma_addr & FDCA_GTT_PTE_ADDR_MASK) | FDCA_GTT_PTE_VALID;
    
    if (flags & DMA_TO_DEVICE) {
        pte_value |= FDCA_GTT_PTE_READABLE;
    }
    if (flags & DMA_FROM_DEVICE) {
        pte_value |= FDCA_GTT_PTE_WRITABLE;
    }
    if (flags & DMA_BIDIRECTIONAL) {
        pte_value |= FDCA_GTT_PTE_READABLE | FDCA_GTT_PTE_WRITABLE;
    }
    
    /* 写入页表项 */
    ((u64 *)gtt->page_table)[index] = pte_value;
    
    /* 确保写入完成 */
    wmb();
    
    fdca_dbg(fdev, "设置 PTE[%u] = 0x%llx (DMA=0x%llx)\n",
             index, pte_value, dma_addr);
}

/**
 * fdca_gtt_clear_pte() - 清除页表项
 * @fdev: FDCA 设备
 * @index: 页表项索引
 */
static void fdca_gtt_clear_pte(struct fdca_device *fdev, u32 index)
{
    struct fdca_gtt_manager *gtt = &fdev->mem_mgr->gtt;
    
    if (index >= gtt->num_entries) {
        fdca_err(fdev, "页表项索引越界: %u >= %u\n", index, gtt->num_entries);
        return;
    }
    
    /* 清除页表项 */
    ((u64 *)gtt->page_table)[index] = 0;
    
    /* 确保写入完成 */
    wmb();
    
    fdca_dbg(fdev, "清除 PTE[%u]\n", index);
}

/*
 * ============================================================================
 * GTT 映射和解映射
 * ============================================================================
 */

/**
 * fdca_gtt_map_pages() - 映射页面到 GTT
 * @fdev: FDCA 设备
 * @pages: 页面数组
 * @num_pages: 页面数量
 * @direction: DMA 方向
 * @debug_name: 调试名称
 * 
 * Return: GTT 映射条目指针或 ERR_PTR
 */
struct fdca_gtt_entry *fdca_gtt_map_pages(struct fdca_device *fdev,
                                          struct page **pages, u32 num_pages,
                                          enum dma_data_direction direction,
                                          const char *debug_name)
{
    struct fdca_gtt_manager *gtt = &fdev->mem_mgr->gtt;
    struct fdca_gtt_entry *entry;
    u64 size = num_pages * PAGE_SIZE;
    u32 i, pte_index;
    int ret;
    
    fdca_dbg(fdev, "GTT 映射: %u 页, 方向=%d, 名称=%s\n",
             num_pages, direction, debug_name ?: "匿名");
    
    /* 分配地址空间 */
    entry = fdca_gtt_alloc_space(fdev, size, PAGE_SIZE);
    if (IS_ERR(entry)) {
        return entry;
    }
    
    /* 设置条目信息 */
    entry->pages = pages;
    entry->num_pages = num_pages;
    entry->debug_name = debug_name;
    
    /* 分配 DMA 地址数组 */
    entry->dma_addrs = kcalloc(num_pages, sizeof(dma_addr_t), GFP_KERNEL);
    if (!entry->dma_addrs) {
        ret = -ENOMEM;
        goto err_free_space;
    }
    
    /* 分配页表项索引数组 */
    entry->pte_indices = kcalloc(num_pages, sizeof(u32), GFP_KERNEL);
    if (!entry->pte_indices) {
        ret = -ENOMEM;
        goto err_free_dma_addrs;
    }
    
    /* 映射每个页面 */
    for (i = 0; i < num_pages; i++) {
        /* 创建 DMA 映射 */
        entry->dma_addrs[i] = dma_map_page(fdev->dev, pages[i], 0, PAGE_SIZE,
                                          direction);
        if (dma_mapping_error(fdev->dev, entry->dma_addrs[i])) {
            fdca_err(fdev, "DMA 映射失败: 页 %u\n", i);
            ret = -ENOMEM;
            goto err_unmap_pages;
        }
        
        /* 设置页表项 */
        pte_index = fdca_gtt_get_pte_index(gtt, entry->gpu_addr + i * PAGE_SIZE);
        entry->pte_indices[i] = pte_index;
        fdca_gtt_set_pte(fdev, pte_index, entry->dma_addrs[i], direction);
    }
    
    /* 更新统计信息 */
    atomic64_inc(&gtt->map_count);
    
    fdca_dbg(fdev, "GTT 映射完成: GPU=0x%llx, 页数=%u\n",
             entry->gpu_addr, num_pages);
    
    return entry;
    
err_unmap_pages:
    /* 解映射已经映射的页面 */
    while (i-- > 0) {
        dma_unmap_page(fdev->dev, entry->dma_addrs[i], PAGE_SIZE, direction);
        fdca_gtt_clear_pte(fdev, entry->pte_indices[i]);
    }
    kfree(entry->pte_indices);
err_free_dma_addrs:
    kfree(entry->dma_addrs);
err_free_space:
    fdca_gtt_free_space(fdev, entry);
    return ERR_PTR(ret);
}

/**
 * fdca_gtt_unmap_pages() - 解映射 GTT 页面
 * @fdev: FDCA 设备
 * @entry: GTT 映射条目
 * @direction: DMA 方向
 */
void fdca_gtt_unmap_pages(struct fdca_device *fdev, struct fdca_gtt_entry *entry,
                         enum dma_data_direction direction)
{
    struct fdca_gtt_manager *gtt = &fdev->mem_mgr->gtt;
    u32 i;
    
    if (!entry) {
        fdca_warn(fdev, "尝试解映射空的 GTT 条目\n");
        return;
    }
    
    fdca_dbg(fdev, "GTT 解映射: GPU=0x%llx, 页数=%u, 名称=%s\n",
             entry->gpu_addr, entry->num_pages, 
             entry->debug_name ?: "匿名");
    
    /* 解映射所有页面 */
    for (i = 0; i < entry->num_pages; i++) {
        /* 清除页表项 */
        fdca_gtt_clear_pte(fdev, entry->pte_indices[i]);
        
        /* 解映射 DMA */
        dma_unmap_page(fdev->dev, entry->dma_addrs[i], PAGE_SIZE, direction);
    }
    
    /* 释放数组 */
    kfree(entry->pte_indices);
    kfree(entry->dma_addrs);
    
    /* 释放地址空间 */
    fdca_gtt_free_space(fdev, entry);
    
    /* 更新统计信息 */
    atomic64_inc(&gtt->unmap_count);
}

/*
 * ============================================================================
 * 统计和监控函数
 * ============================================================================
 */

/**
 * fdca_gtt_get_stats() - 获取 GTT 统计信息
 * @fdev: FDCA 设备
 * @stats: 输出统计信息的结构
 */
void fdca_gtt_get_stats(struct fdca_device *fdev, struct fdca_gtt_stats *stats)
{
    struct fdca_gtt_manager *gtt = &fdev->mem_mgr->gtt;
    
    mutex_lock(&gtt->lock);
    
    stats->total_size = gtt->size;
    stats->used_size = drm_mm_total_size(&gtt->mm) - drm_mm_avail_size(&gtt->mm);
    stats->available_size = drm_mm_avail_size(&gtt->mm);
    stats->num_entries = gtt->num_entries;
    stats->map_count = atomic64_read(&gtt->map_count);
    stats->unmap_count = atomic64_read(&gtt->unmap_count);
    
    mutex_unlock(&gtt->lock);
}

/**
 * fdca_gtt_print_stats() - 打印 GTT 统计信息
 * @fdev: FDCA 设备
 */
void fdca_gtt_print_stats(struct fdca_device *fdev)
{
    struct fdca_gtt_stats stats;
    
    fdca_gtt_get_stats(fdev, &stats);
    
    fdca_info(fdev, "=== GTT 统计信息 ===\n");
    fdca_info(fdev, "总大小: %llu MB\n", stats.total_size >> 20);
    fdca_info(fdev, "已使用: %llu MB (%.1f%%)\n", 
              stats.used_size >> 20,
              (float)stats.used_size * 100.0 / stats.total_size);
    fdca_info(fdev, "可用: %llu MB\n", stats.available_size >> 20);
    fdca_info(fdev, "页表项数: %u\n", stats.num_entries);
    fdca_info(fdev, "映射次数: %llu\n", stats.map_count);
    fdca_info(fdev, "解映射次数: %llu\n", stats.unmap_count);
}

/*
 * ============================================================================
 * 导出符号
 * ============================================================================
 */

EXPORT_SYMBOL_GPL(fdca_gtt_manager_init);
EXPORT_SYMBOL_GPL(fdca_gtt_manager_fini);
EXPORT_SYMBOL_GPL(fdca_gtt_map_pages);
EXPORT_SYMBOL_GPL(fdca_gtt_unmap_pages);
EXPORT_SYMBOL_GPL(fdca_gtt_get_stats);
EXPORT_SYMBOL_GPL(fdca_gtt_print_stats);
