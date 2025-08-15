// SPDX-License-Identifier: GPL-2.0-only
/*
 * FDCA (Fangzheng Distributed Computing Architecture) Memory Manager
 * 
 * Copyright (C) 2024 Fangzheng Technology Co., Ltd.
 * 
 * 统一内存管理模块
 * 
 * 本模块负责：
 * 1. 整合 VRAM 和 GTT 管理器
 * 2. 提供统一的内存分配接口
 * 3. 实现 GEM 对象管理
 * 4. 内存池管理和优化
 * 5. 缓存对象管理
 * 6. 内存使用监控和统计
 *
 * Author: FDCA Kernel Team
 * Date: 2024
 */

#include <linux/slab.h>
#include <linux/genalloc.h>
#include <linux/workqueue.h>
#include <linux/list.h>

#include <drm/drm_gem.h>
#include <drm/drm_prime.h>

#include "fdca_drv.h"

/*
 * ============================================================================
 * 内存池配置
 * ============================================================================
 */

/* 内存池大小配置 */
#define FDCA_SMALL_POOL_SIZE    (16 << 20)      /* 16MB 小内存池 */
#define FDCA_LARGE_POOL_SIZE    (64 << 20)      /* 64MB 大内存池 */
#define FDCA_SMALL_BLOCK_SIZE   4096            /* 4KB 小块 */
#define FDCA_LARGE_BLOCK_SIZE   (2 << 20)       /* 2MB 大块 */

/* 缓存管理配置 */
#define FDCA_CACHE_MAX_OBJECTS  256             /* 最大缓存对象数 */
#define FDCA_CACHE_CLEANUP_INTERVAL (10 * HZ)   /* 10秒清理间隔 */
#define FDCA_CACHE_EXPIRE_TIME  (60 * HZ)       /* 60秒过期时间 */

/*
 * ============================================================================
 * GEM 对象结构
 * ============================================================================
 */

/**
 * struct fdca_gem_object - FDCA GEM 对象
 * 
 * 扩展标准 DRM GEM 对象，添加 FDCA 特定功能
 */
struct fdca_gem_object {
    struct drm_gem_object base;         /* 基础 GEM 对象 */
    
    /* 内存类型 */
    enum {
        FDCA_MEM_TYPE_VRAM = 0,         /* VRAM 内存 */
        FDCA_MEM_TYPE_SYSTEM = 1,       /* 系统内存 */
        FDCA_MEM_TYPE_CACHED = 2,       /* 缓存内存 */
    } mem_type;
    
    /* VRAM 对象 */
    struct fdca_vram_object *vram_obj;  /* VRAM 对象 */
    
    /* GTT 映射 */
    struct fdca_gtt_entry *gtt_entry;   /* GTT 映射条目 */
    
    /* 系统内存 */
    struct page **pages;                /* 页面数组 */
    struct sg_table *sg_table;         /* scatter-gather 表 */
    
    /* 属性 */
    u32 flags;                          /* 创建标志 */
    bool coherent;                      /* 是否一致性 */
    bool pinned;                        /* 是否固定 */
    
    /* 同步 */
    struct mutex lock;                  /* 对象锁 */
    atomic_t pin_count;                 /* 固定计数 */
    
    /* 统计信息 */
    u64 create_time;                    /* 创建时间 */
    u64 last_access;                    /* 最后访问时间 */
    atomic64_t access_count;            /* 访问计数 */
    
    /* 调试信息 */
    const char *debug_name;             /* 调试名称 */
};

/**
 * struct fdca_cached_object - 缓存对象
 * 
 * 用于内存池的缓存管理
 */
struct fdca_cached_object {
    struct list_head list;              /* 链表节点 */
    void *ptr;                          /* 内存指针 */
    size_t size;                        /* 对象大小 */
    u64 expire_time;                    /* 过期时间 */
    atomic_t ref_count;                 /* 引用计数 */
};

/*
 * ============================================================================
 * 内存管理器实现
 * ============================================================================
 */

/**
 * fdca_memory_create_pools() - 创建内存池
 * @fdev: FDCA 设备
 * 
 * Return: 0 表示成功，负数表示错误
 */
static int fdca_memory_create_pools(struct fdca_device *fdev)
{
    struct fdca_memory_manager *mem_mgr = fdev->mem_mgr;
    
    /* 创建小内存池 */
    mem_mgr->small_pool = gen_pool_create(ilog2(FDCA_SMALL_BLOCK_SIZE), -1);
    if (!mem_mgr->small_pool) {
        fdca_err(fdev, "无法创建小内存池\n");
        return -ENOMEM;
    }
    
    /* 创建大内存池 */
    mem_mgr->large_pool = gen_pool_create(ilog2(FDCA_LARGE_BLOCK_SIZE), -1);
    if (!mem_mgr->large_pool) {
        fdca_err(fdev, "无法创建大内存池\n");
        gen_pool_destroy(mem_mgr->small_pool);
        return -ENOMEM;
    }
    
    fdca_info(fdev, "内存池创建完成: 小池=%d KB, 大池=%d MB\n",
              FDCA_SMALL_POOL_SIZE >> 10, FDCA_LARGE_POOL_SIZE >> 20);
    
    return 0;
}

/**
 * fdca_memory_destroy_pools() - 销毁内存池
 * @fdev: FDCA 设备
 */
static void fdca_memory_destroy_pools(struct fdca_device *fdev)
{
    struct fdca_memory_manager *mem_mgr = fdev->mem_mgr;
    
    if (mem_mgr->large_pool) {
        gen_pool_destroy(mem_mgr->large_pool);
        mem_mgr->large_pool = NULL;
    }
    
    if (mem_mgr->small_pool) {
        gen_pool_destroy(mem_mgr->small_pool);
        mem_mgr->small_pool = NULL;
    }
    
    fdca_info(fdev, "内存池销毁完成\n");
}

/**
 * fdca_cache_cleanup_work() - 缓存清理工作函数
 * @work: 工作结构
 */
static void fdca_cache_cleanup_work(struct work_struct *work)
{
    struct fdca_memory_manager *mem_mgr = 
        container_of(work, struct fdca_memory_manager, cache_cleanup.work);
    struct fdca_device *fdev = mem_mgr->fdev;
    struct fdca_cached_object *obj, *tmp;
    u64 current_time = jiffies;
    int cleaned = 0;
    
    /* 清理过期的缓存对象 */
    list_for_each_entry_safe(obj, tmp, &mem_mgr->cached_objects, list) {
        if (time_after64(current_time, obj->expire_time) &&
            atomic_read(&obj->ref_count) == 0) {
            list_del(&obj->list);
            kfree(obj->ptr);
            kfree(obj);
            cleaned++;
        }
    }
    
    if (cleaned > 0) {
        fdca_dbg(fdev, "缓存清理: 清理了 %d 个对象\n", cleaned);
    }
    
    /* 重新安排下次清理 */
    schedule_delayed_work(&mem_mgr->cache_cleanup, FDCA_CACHE_CLEANUP_INTERVAL);
}

/**
 * fdca_memory_manager_init() - 初始化内存管理器
 * @fdev: FDCA 设备
 * 
 * Return: 0 表示成功，负数表示错误
 */
int fdca_memory_manager_init(struct fdca_device *fdev)
{
    struct fdca_memory_manager *mem_mgr;
    int ret;
    
    fdca_info(fdev, "初始化内存管理器\n");
    
    /* 分配内存管理器结构 */
    mem_mgr = kzalloc(sizeof(*mem_mgr), GFP_KERNEL);
    if (!mem_mgr) {
        fdca_err(fdev, "无法分配内存管理器结构\n");
        return -ENOMEM;
    }
    
    mem_mgr->fdev = fdev;
    fdev->mem_mgr = mem_mgr;
    
    /* 初始化缓存对象列表 */
    INIT_LIST_HEAD(&mem_mgr->cached_objects);
    INIT_DELAYED_WORK(&mem_mgr->cache_cleanup, fdca_cache_cleanup_work);
    
    /* 初始化统计信息 */
    atomic64_set(&mem_mgr->total_allocated, 0);
    atomic64_set(&mem_mgr->peak_usage, 0);
    
    /* 初始化 VRAM 管理器 */
    ret = fdca_vram_manager_init(fdev);
    if (ret) {
        fdca_err(fdev, "VRAM 管理器初始化失败: %d\n", ret);
        goto err_free_mem_mgr;
    }
    
    /* 初始化 GTT 管理器 */
    ret = fdca_gtt_manager_init(fdev);
    if (ret) {
        fdca_err(fdev, "GTT 管理器初始化失败: %d\n", ret);
        goto err_fini_vram;
    }
    
    /* 创建内存池 */
    ret = fdca_memory_create_pools(fdev);
    if (ret) {
        fdca_err(fdev, "内存池创建失败: %d\n", ret);
        goto err_fini_gtt;
    }
    
    /* 启动缓存清理工作 */
    schedule_delayed_work(&mem_mgr->cache_cleanup, FDCA_CACHE_CLEANUP_INTERVAL);
    
    fdca_info(fdev, "内存管理器初始化完成\n");
    
    return 0;
    
err_fini_gtt:
    fdca_gtt_manager_fini(fdev);
err_fini_vram:
    fdca_vram_manager_fini(fdev);
err_free_mem_mgr:
    kfree(mem_mgr);
    fdev->mem_mgr = NULL;
    return ret;
}

/**
 * fdca_memory_manager_fini() - 清理内存管理器
 * @fdev: FDCA 设备
 */
void fdca_memory_manager_fini(struct fdca_device *fdev)
{
    struct fdca_memory_manager *mem_mgr = fdev->mem_mgr;
    struct fdca_cached_object *obj, *tmp;
    
    if (!mem_mgr) {
        return;
    }
    
    fdca_info(fdev, "清理内存管理器\n");
    
    /* 停止缓存清理工作 */
    cancel_delayed_work_sync(&mem_mgr->cache_cleanup);
    
    /* 清理所有缓存对象 */
    list_for_each_entry_safe(obj, tmp, &mem_mgr->cached_objects, list) {
        list_del(&obj->list);
        kfree(obj->ptr);
        kfree(obj);
    }
    
    /* 销毁内存池 */
    fdca_memory_destroy_pools(fdev);
    
    /* 清理 GTT 管理器 */
    fdca_gtt_manager_fini(fdev);
    
    /* 清理 VRAM 管理器 */
    fdca_vram_manager_fini(fdev);
    
    /* 打印统计信息 */
    fdca_info(fdev, "内存统计: 总分配 %lld 字节, 峰值使用 %lld 字节\n",
              atomic64_read(&mem_mgr->total_allocated),
              atomic64_read(&mem_mgr->peak_usage));
    
    /* 释放内存管理器结构 */
    kfree(mem_mgr);
    fdev->mem_mgr = NULL;
    
    fdca_info(fdev, "内存管理器清理完成\n");
}

/*
 * ============================================================================
 * GEM 对象管理
 * ============================================================================
 */

/**
 * fdca_gem_object_create() - 创建 GEM 对象
 * @fdev: FDCA 设备
 * @size: 对象大小
 * @flags: 创建标志
 * 
 * Return: GEM 对象指针或 ERR_PTR
 */
struct fdca_gem_object *fdca_gem_object_create(struct fdca_device *fdev,
                                               size_t size, u32 flags)
{
    struct fdca_gem_object *obj;
    int ret;
    
    /* 分配对象结构 */
    obj = kzalloc(sizeof(*obj), GFP_KERNEL);
    if (!obj) {
        return ERR_PTR(-ENOMEM);
    }
    
    /* 初始化基础 GEM 对象 */
    ret = drm_gem_object_init(&fdev->drm, &obj->base, size);
    if (ret) {
        fdca_err(fdev, "GEM 对象初始化失败: %d\n", ret);
        kfree(obj);
        return ERR_PTR(ret);
    }
    
    /* 初始化 FDCA 特定字段 */
    obj->flags = flags;
    obj->mem_type = FDCA_MEM_TYPE_VRAM;  /* 默认使用 VRAM */
    obj->coherent = !!(flags & FDCA_VRAM_ALLOC_CACHED);
    obj->pinned = false;
    
    mutex_init(&obj->lock);
    atomic_set(&obj->pin_count, 0);
    atomic64_set(&obj->access_count, 0);
    obj->create_time = ktime_get_boottime_seconds();
    obj->last_access = obj->create_time;
    
    /* 分配 VRAM */
    obj->vram_obj = fdca_vram_alloc(fdev, size, flags, "GEM对象");
    if (IS_ERR(obj->vram_obj)) {
        fdca_err(fdev, "VRAM 分配失败\n");
        ret = PTR_ERR(obj->vram_obj);
        goto err_gem_free;
    }
    
    fdca_dbg(fdev, "GEM 对象创建: 大小=%zu, 标志=0x%x\n", size, flags);
    
    return obj;
    
err_gem_free:
    drm_gem_object_release(&obj->base);
    kfree(obj);
    return ERR_PTR(ret);
}

/**
 * fdca_gem_object_free() - 释放 GEM 对象
 * @obj: GEM 对象
 */
static void fdca_gem_object_free(struct drm_gem_object *gem_obj)
{
    struct fdca_gem_object *obj = container_of(gem_obj, struct fdca_gem_object, base);
    struct fdca_device *fdev = drm_to_fdca(gem_obj->dev);
    
    fdca_dbg(fdev, "GEM 对象释放: 大小=%zu\n", gem_obj->size);
    
    /* 解除 GTT 映射 */
    if (obj->gtt_entry) {
        fdca_gtt_unmap_pages(fdev, obj->gtt_entry, DMA_BIDIRECTIONAL);
        obj->gtt_entry = NULL;
    }
    
    /* 释放 VRAM */
    if (obj->vram_obj) {
        fdca_vram_free(fdev, obj->vram_obj);
        obj->vram_obj = NULL;
    }
    
    /* 释放系统内存页面 */
    if (obj->pages) {
        u32 num_pages = gem_obj->size >> PAGE_SHIFT;
        u32 i;
        
        for (i = 0; i < num_pages; i++) {
            if (obj->pages[i]) {
                put_page(obj->pages[i]);
            }
        }
        kfree(obj->pages);
    }
    
    /* 释放 scatter-gather 表 */
    if (obj->sg_table) {
        sg_free_table(obj->sg_table);
        kfree(obj->sg_table);
    }
    
    /* 释放基础对象 */
    drm_gem_object_release(gem_obj);
    kfree(obj);
}

/*
 * ============================================================================
 * 内存统计和监控
 * ============================================================================
 */

/**
 * fdca_memory_get_total_stats() - 获取总体内存统计
 * @fdev: FDCA 设备
 * @stats: 输出统计信息的结构
 */
void fdca_memory_get_total_stats(struct fdca_device *fdev,
                                struct fdca_memory_total_stats *stats)
{
    struct fdca_vram_stats vram_stats;
    struct fdca_gtt_stats gtt_stats;
    
    /* 获取 VRAM 统计 */
    fdca_vram_get_stats(fdev, &vram_stats);
    
    /* 获取 GTT 统计 */
    fdca_gtt_get_stats(fdev, &gtt_stats);
    
    /* 汇总统计信息 */
    stats->vram_total = vram_stats.total_size;
    stats->vram_used = vram_stats.used_size;
    stats->vram_available = vram_stats.available_size;
    stats->vram_fragmentation = vram_stats.fragmentation;
    
    stats->gtt_total = gtt_stats.total_size;
    stats->gtt_used = gtt_stats.used_size;
    stats->gtt_available = gtt_stats.available_size;
    
    stats->total_allocated = atomic64_read(&fdev->mem_mgr->total_allocated);
    stats->peak_usage = atomic64_read(&fdev->mem_mgr->peak_usage);
}

/**
 * fdca_memory_print_total_stats() - 打印总体内存统计
 * @fdev: FDCA 设备
 */
void fdca_memory_print_total_stats(struct fdca_device *fdev)
{
    struct fdca_memory_total_stats stats;
    
    fdca_memory_get_total_stats(fdev, &stats);
    
    fdca_info(fdev, "=== 内存管理统计 ===\n");
    fdca_info(fdev, "VRAM: %llu MB / %llu MB (%.1f%%, 碎片率 %u%%)\n",
              stats.vram_used >> 20, stats.vram_total >> 20,
              (float)stats.vram_used * 100.0 / stats.vram_total,
              stats.vram_fragmentation);
    fdca_info(fdev, "GTT: %llu MB / %llu MB (%.1f%%)\n",
              stats.gtt_used >> 20, stats.gtt_total >> 20,
              (float)stats.gtt_used * 100.0 / stats.gtt_total);
    fdca_info(fdev, "总分配: %lld 字节, 峰值: %lld 字节\n",
              stats.total_allocated, stats.peak_usage);
}

/*
 * ============================================================================
 * DRM GEM 函数表
 * ============================================================================
 */

static const struct drm_gem_object_funcs fdca_gem_object_funcs = {
    .free = fdca_gem_object_free,
    .print_info = drm_gem_print_info,
};

/*
 * ============================================================================
 * 导出符号
 * ============================================================================
 */

EXPORT_SYMBOL_GPL(fdca_memory_manager_init);
EXPORT_SYMBOL_GPL(fdca_memory_manager_fini);
EXPORT_SYMBOL_GPL(fdca_gem_object_create);
EXPORT_SYMBOL_GPL(fdca_memory_get_total_stats);
EXPORT_SYMBOL_GPL(fdca_memory_print_total_stats);
