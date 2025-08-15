/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * FDCA Command Queue Abstraction Layer
 */

#ifndef __FDCA_QUEUE_H__
#define __FDCA_QUEUE_H__

#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/wait.h>

/* 队列类型 */
enum fdca_queue_type {
    FDCA_QUEUE_CAU,     /* 上下文相关队列 (访存优化) */
    FDCA_QUEUE_CFU,     /* 上下文无关队列 (计算优化) */
};

/* 命令状态 */
enum fdca_cmd_status {
    FDCA_CMD_PENDING,
    FDCA_CMD_RUNNING,
    FDCA_CMD_COMPLETED,
    FDCA_CMD_ERROR,
};

/* 命令描述符 */
struct fdca_command {
    struct list_head list;
    u32 cmd_id;
    enum fdca_cmd_status status;
    void *data;
    size_t data_size;
    u64 submit_time;
    u64 start_time;
    u64 end_time;
};

/* 队列管理器 */
struct fdca_queue_manager {
    struct fdca_device *fdev;
    enum fdca_queue_type type;
    
    /* 队列状态 */
    struct list_head pending_cmds;
    struct list_head running_cmds;
    struct mutex queue_lock;
    wait_queue_head_t wait_queue;
    
    /* 统计信息 */
    atomic64_t submitted_cmds;
    atomic64_t completed_cmds;
    atomic64_t failed_cmds;
    
    /* 队列特定操作 */
    int (*submit_cmd)(struct fdca_queue_manager *mgr, struct fdca_command *cmd);
    int (*wait_cmd)(struct fdca_queue_manager *mgr, u32 cmd_id);
    void (*cleanup)(struct fdca_queue_manager *mgr);
};

/* 函数声明 */
int fdca_queue_manager_init(struct fdca_device *fdev, enum fdca_queue_type type);
void fdca_queue_manager_fini(struct fdca_device *fdev, enum fdca_queue_type type);
int fdca_queue_submit_command(struct fdca_device *fdev, enum fdca_queue_type type,
                             struct fdca_command *cmd);
int fdca_queue_wait_command(struct fdca_device *fdev, enum fdca_queue_type type, u32 cmd_id);

#endif /* __FDCA_QUEUE_H__ */
