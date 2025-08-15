// SPDX-License-Identifier: GPL-2.0-only
/*
 * FDCA Command Queue Implementation
 */

#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/atomic.h>
#include "fdca_drv.h"
#include "fdca_queue.h"

static struct fdca_queue_manager *cau_queue_mgr = NULL;
static struct fdca_queue_manager *cfu_queue_mgr = NULL;

/* CAU 队列操作 */
static int fdca_cau_submit_cmd(struct fdca_queue_manager *mgr, struct fdca_command *cmd)
{
    /* CAU 队列优化低延迟提交 */
    cmd->start_time = ktime_get_ns();
    cmd->status = FDCA_CMD_RUNNING;
    
    /* 移动到运行队列 */
    list_move_tail(&cmd->list, &mgr->running_cmds);
    
    /* 立即提交到硬件 */
    /* TODO: 实际的硬件提交逻辑 */
    
    return 0;
}

/* CFU 队列操作 */
static int fdca_cfu_submit_cmd(struct fdca_queue_manager *mgr, struct fdca_command *cmd)
{
    /* CFU 队列优化高吞吐量 */
    cmd->start_time = ktime_get_ns();
    cmd->status = FDCA_CMD_RUNNING;
    
    /* 批量处理优化 */
    list_move_tail(&cmd->list, &mgr->running_cmds);
    
    return 0;
}

/* 通用等待函数 */
static int fdca_queue_wait_cmd(struct fdca_queue_manager *mgr, u32 cmd_id)
{
    struct fdca_command *cmd;
    int ret = 0;
    
    /* 查找命令 */
    mutex_lock(&mgr->queue_lock);
    list_for_each_entry(cmd, &mgr->running_cmds, list) {
        if (cmd->cmd_id == cmd_id) {
            while (cmd->status == FDCA_CMD_RUNNING) {
                mutex_unlock(&mgr->queue_lock);
                wait_event_interruptible(mgr->wait_queue, 
                                        cmd->status != FDCA_CMD_RUNNING);
                mutex_lock(&mgr->queue_lock);
            }
            ret = (cmd->status == FDCA_CMD_COMPLETED) ? 0 : -EIO;
            break;
        }
    }
    mutex_unlock(&mgr->queue_lock);
    
    return ret;
}

/**
 * fdca_queue_manager_init() - 初始化队列管理器
 */
int fdca_queue_manager_init(struct fdca_device *fdev, enum fdca_queue_type type)
{
    struct fdca_queue_manager *mgr;
    
    mgr = kzalloc(sizeof(*mgr), GFP_KERNEL);
    if (!mgr)
        return -ENOMEM;
    
    mgr->fdev = fdev;
    mgr->type = type;
    
    INIT_LIST_HEAD(&mgr->pending_cmds);
    INIT_LIST_HEAD(&mgr->running_cmds);
    mutex_init(&mgr->queue_lock);
    init_waitqueue_head(&mgr->wait_queue);
    
    atomic64_set(&mgr->submitted_cmds, 0);
    atomic64_set(&mgr->completed_cmds, 0);
    atomic64_set(&mgr->failed_cmds, 0);
    
    /* 设置类型特定的操作 */
    switch (type) {
    case FDCA_QUEUE_CAU:
        mgr->submit_cmd = fdca_cau_submit_cmd;
        cau_queue_mgr = mgr;
        break;
    case FDCA_QUEUE_CFU:
        mgr->submit_cmd = fdca_cfu_submit_cmd;
        cfu_queue_mgr = mgr;
        break;
    default:
        kfree(mgr);
        return -EINVAL;
    }
    
    mgr->wait_cmd = fdca_queue_wait_cmd;
    
    fdca_info(fdev, "%s 队列管理器初始化完成\n", 
              (type == FDCA_QUEUE_CAU) ? "CAU" : "CFU");
    
    return 0;
}

/**
 * fdca_queue_manager_fini() - 清理队列管理器
 */
void fdca_queue_manager_fini(struct fdca_device *fdev, enum fdca_queue_type type)
{
    struct fdca_queue_manager *mgr = (type == FDCA_QUEUE_CAU) ? cau_queue_mgr : cfu_queue_mgr;
    
    if (!mgr)
        return;
    
    fdca_info(fdev, "%s 队列统计: 提交 %lld, 完成 %lld, 失败 %lld\n",
              (type == FDCA_QUEUE_CAU) ? "CAU" : "CFU",
              atomic64_read(&mgr->submitted_cmds),
              atomic64_read(&mgr->completed_cmds),
              atomic64_read(&mgr->failed_cmds));
    
    kfree(mgr);
    
    if (type == FDCA_QUEUE_CAU)
        cau_queue_mgr = NULL;
    else
        cfu_queue_mgr = NULL;
}

/**
 * fdca_queue_submit_command() - 提交命令到队列
 */
int fdca_queue_submit_command(struct fdca_device *fdev, enum fdca_queue_type type,
                             struct fdca_command *cmd)
{
    struct fdca_queue_manager *mgr = (type == FDCA_QUEUE_CAU) ? cau_queue_mgr : cfu_queue_mgr;
    int ret;
    
    if (!mgr || !cmd)
        return -EINVAL;
    
    cmd->submit_time = ktime_get_ns();
    cmd->status = FDCA_CMD_PENDING;
    
    mutex_lock(&mgr->queue_lock);
    list_add_tail(&cmd->list, &mgr->pending_cmds);
    atomic64_inc(&mgr->submitted_cmds);
    
    /* 立即尝试提交 */
    ret = mgr->submit_cmd(mgr, cmd);
    
    mutex_unlock(&mgr->queue_lock);
    
    return ret;
}

EXPORT_SYMBOL_GPL(fdca_queue_manager_init);
EXPORT_SYMBOL_GPL(fdca_queue_manager_fini);
EXPORT_SYMBOL_GPL(fdca_queue_submit_command);
EXPORT_SYMBOL_GPL(fdca_queue_wait_command);
