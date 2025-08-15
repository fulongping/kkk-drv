// SPDX-License-Identifier: GPL-2.0-only
/*
 * FDCA Synchronization Object Management
 */

#include <linux/slab.h>
#include <linux/completion.h>
#include "fdca_drv.h"

/* 同步对象 */
struct fdca_sync_obj {
    u32 fence_id;
    struct completion completion;
    struct list_head list;
    atomic_t ref_count;
    bool signaled;
};

static struct list_head sync_objects = LIST_HEAD_INIT(sync_objects);
static DEFINE_MUTEX(sync_lock);
static atomic_t fence_counter = ATOMIC_INIT(0);

/**
 * fdca_sync_create_fence() - 创建同步栅栏
 */
u32 fdca_sync_create_fence(void)
{
    struct fdca_sync_obj *obj;
    u32 fence_id;
    
    obj = kzalloc(sizeof(*obj), GFP_KERNEL);
    if (!obj)
        return 0;
    
    fence_id = atomic_inc_return(&fence_counter);
    obj->fence_id = fence_id;
    init_completion(&obj->completion);
    INIT_LIST_HEAD(&obj->list);
    atomic_set(&obj->ref_count, 1);
    obj->signaled = false;
    
    mutex_lock(&sync_lock);
    list_add_tail(&obj->list, &sync_objects);
    mutex_unlock(&sync_lock);
    
    return fence_id;
}

/**
 * fdca_sync_signal_fence() - 触发同步栅栏
 */
int fdca_sync_signal_fence(u32 fence_id)
{
    struct fdca_sync_obj *obj;
    int ret = -ENOENT;
    
    mutex_lock(&sync_lock);
    list_for_each_entry(obj, &sync_objects, list) {
        if (obj->fence_id == fence_id) {
            obj->signaled = true;
            complete(&obj->completion);
            ret = 0;
            break;
        }
    }
    mutex_unlock(&sync_lock);
    
    return ret;
}

/**
 * fdca_sync_wait_fence() - 等待同步栅栏
 */
int fdca_sync_wait_fence(u32 fence_id, unsigned long timeout_ms)
{
    struct fdca_sync_obj *obj;
    int ret = -ENOENT;
    
    mutex_lock(&sync_lock);
    list_for_each_entry(obj, &sync_objects, list) {
        if (obj->fence_id == fence_id) {
            mutex_unlock(&sync_lock);
            if (timeout_ms) {
                ret = wait_for_completion_timeout(&obj->completion, 
                                                 msecs_to_jiffies(timeout_ms));
                ret = ret ? 0 : -ETIMEDOUT;
            } else {
                wait_for_completion(&obj->completion);
                ret = 0;
            }
            return ret;
        }
    }
    mutex_unlock(&sync_lock);
    
    return ret;
}

EXPORT_SYMBOL_GPL(fdca_sync_create_fence);
EXPORT_SYMBOL_GPL(fdca_sync_signal_fence);
EXPORT_SYMBOL_GPL(fdca_sync_wait_fence);
