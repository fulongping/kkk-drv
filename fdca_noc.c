// SPDX-License-Identifier: GPL-2.0-only
/*
 * FDCA Network-on-Chip (NoC) Communication Management
 */

#include <linux/slab.h>
#include <linux/delay.h>
#include "fdca_drv.h"

/* NoC 管理器 */
struct fdca_noc_manager {
    struct fdca_device *fdev;
    void __iomem *noc_base;
    
    /* 通信统计 */
    atomic64_t cau_to_cfu_msgs;
    atomic64_t cfu_to_cau_msgs;
    atomic64_t total_latency;
    atomic64_t min_latency;
    atomic64_t max_latency;
};

static struct fdca_noc_manager *noc_mgr = NULL;

/**
 * fdca_noc_manager_init() - 初始化 NoC 管理器
 */
int fdca_noc_manager_init(struct fdca_device *fdev)
{
    struct fdca_noc_manager *mgr;
    
    if (!fdev->units[FDCA_UNIT_NOC].present) {
        fdca_warn(fdev, "NoC 单元不可用\n");
        return -ENODEV;
    }
    
    mgr = kzalloc(sizeof(*mgr), GFP_KERNEL);
    if (!mgr)
        return -ENOMEM;
    
    mgr->fdev = fdev;
    mgr->noc_base = fdev->units[FDCA_UNIT_NOC].mmio_base;
    
    /* 初始化统计 */
    atomic64_set(&mgr->cau_to_cfu_msgs, 0);
    atomic64_set(&mgr->cfu_to_cau_msgs, 0);
    atomic64_set(&mgr->total_latency, 0);
    atomic64_set(&mgr->min_latency, LLONG_MAX);
    atomic64_set(&mgr->max_latency, 0);
    
    /* 配置 NoC 参数 */
    iowrite32(0x1, mgr->noc_base + 0x00);  /* 启用 NoC */
    iowrite32(0x10, mgr->noc_base + 0x04); /* 设置延迟阈值 */
    
    noc_mgr = mgr;
    
    fdca_info(fdev, "NoC 管理器初始化完成\n");
    return 0;
}

/**
 * fdca_noc_manager_fini() - 清理 NoC 管理器
 */
void fdca_noc_manager_fini(struct fdca_device *fdev)
{
    if (!noc_mgr)
        return;
    
    fdca_info(fdev, "NoC 统计: CAU->CFU %lld, CFU->CAU %lld, 平均延迟 %lld ns\n",
              atomic64_read(&noc_mgr->cau_to_cfu_msgs),
              atomic64_read(&noc_mgr->cfu_to_cau_msgs),
              atomic64_read(&noc_mgr->total_latency) / 
              max(1LL, atomic64_read(&noc_mgr->cau_to_cfu_msgs) + 
                       atomic64_read(&noc_mgr->cfu_to_cau_msgs)));
    
    kfree(noc_mgr);
    noc_mgr = NULL;
}

/**
 * fdca_noc_send_message() - 发送 NoC 消息
 */
int fdca_noc_send_message(int src_unit, int dst_unit, const void *data, size_t size)
{
    u64 start_time, end_time, latency;
    
    if (!noc_mgr || size > 64) /* 最大消息大小限制 */
        return -EINVAL;
    
    start_time = ktime_get_ns();
    
    /* 发送消息到硬件 */
    iowrite32(src_unit, noc_mgr->noc_base + 0x10);     /* 源单元 */
    iowrite32(dst_unit, noc_mgr->noc_base + 0x14);     /* 目标单元 */
    iowrite32(size, noc_mgr->noc_base + 0x18);         /* 消息大小 */
    
    /* 复制数据 */
    memcpy_toio(noc_mgr->noc_base + 0x100, data, size);
    
    /* 启动传输 */
    iowrite32(0x1, noc_mgr->noc_base + 0x1C);
    
    /* 等待完成 */
    while (ioread32(noc_mgr->noc_base + 0x20) & 0x1) {
        cpu_relax();
    }
    
    end_time = ktime_get_ns();
    latency = end_time - start_time;
    
    /* 更新统计 */
    if (src_unit == FDCA_UNIT_CAU && dst_unit == FDCA_UNIT_CFU)
        atomic64_inc(&noc_mgr->cau_to_cfu_msgs);
    else if (src_unit == FDCA_UNIT_CFU && dst_unit == FDCA_UNIT_CAU)
        atomic64_inc(&noc_mgr->cfu_to_cau_msgs);
    
    atomic64_add(latency, &noc_mgr->total_latency);
    
    /* 更新最小/最大延迟 */
    u64 min_lat = atomic64_read(&noc_mgr->min_latency);
    if (latency < min_lat)
        atomic64_set(&noc_mgr->min_latency, latency);
    
    u64 max_lat = atomic64_read(&noc_mgr->max_latency);
    if (latency > max_lat)
        atomic64_set(&noc_mgr->max_latency, latency);
    
    return 0;
}

EXPORT_SYMBOL_GPL(fdca_noc_manager_init);
EXPORT_SYMBOL_GPL(fdca_noc_manager_fini);
EXPORT_SYMBOL_GPL(fdca_noc_send_message);
