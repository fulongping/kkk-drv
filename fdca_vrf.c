// SPDX-License-Identifier: GPL-2.0-only
/*
 * FDCA Vector Register File Management
 * 
 * 抽象向量寄存器文件访问，支持 32 个向量寄存器的分配和管理
 * 考虑多 lane 架构下的寄存器分布
 */

#include <linux/slab.h>
#include <linux/bitmap.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>

#include "fdca_drv.h"
#include "fdca_rvv_state.h"

/*
 * VRF 管理结构
 */
struct fdca_vrf_manager {
    struct fdca_device *fdev;
    
    /* 寄存器分配位图 */
    DECLARE_BITMAP(allocated_regs, FDCA_RVV_NUM_VREGS);
    spinlock_t alloc_lock;
    
    /* Lane 分布管理 */
    struct {
        u32 num_lanes;
        u32 lane_width;           /* 每个 lane 的位宽 */
        u32 regs_per_lane;        /* 每个 lane 的寄存器数 */
        void __iomem *lane_bases[16]; /* 最多 16 个 lane */
    } lane_info;
    
    /* 性能统计 */
    atomic64_t reg_allocations;
    atomic64_t reg_frees;
    atomic64_t lane_accesses;
};

static struct fdca_vrf_manager *vrf_mgr = NULL;

/**
 * fdca_vrf_init() - 初始化向量寄存器文件管理
 */
int fdca_vrf_init(struct fdca_device *fdev)
{
    struct fdca_vrf_manager *mgr;
    int i;
    
    mgr = kzalloc(sizeof(*mgr), GFP_KERNEL);
    if (!mgr)
        return -ENOMEM;
    
    mgr->fdev = fdev;
    spin_lock_init(&mgr->alloc_lock);
    
    /* 初始化 lane 信息 */
    mgr->lane_info.num_lanes = fdev->rvv_config.num_lanes;
    mgr->lane_info.lane_width = fdev->rvv_config.vlen / mgr->lane_info.num_lanes;
    mgr->lane_info.regs_per_lane = FDCA_RVV_NUM_VREGS;
    
    /* 映射 lane 基址 */
    for (i = 0; i < mgr->lane_info.num_lanes; i++) {
        mgr->lane_info.lane_bases[i] = fdev->units[FDCA_UNIT_VPU].mmio_base + 
                                      (i * 0x1000); /* 每个 lane 4KB 空间 */
    }
    
    /* 初始化统计 */
    atomic64_set(&mgr->reg_allocations, 0);
    atomic64_set(&mgr->reg_frees, 0);
    atomic64_set(&mgr->lane_accesses, 0);
    
    vrf_mgr = mgr;
    
    fdca_info(fdev, "VRF 管理器初始化: %u lanes, %u 寄存器\n",
              mgr->lane_info.num_lanes, FDCA_RVV_NUM_VREGS);
    
    return 0;
}

/**
 * fdca_vrf_fini() - 清理向量寄存器文件管理
 */
void fdca_vrf_fini(struct fdca_device *fdev)
{
    if (!vrf_mgr)
        return;
    
    fdca_info(fdev, "VRF 统计: 分配 %lld, 释放 %lld, Lane访问 %lld\n",
              atomic64_read(&vrf_mgr->reg_allocations),
              atomic64_read(&vrf_mgr->reg_frees),
              atomic64_read(&vrf_mgr->lane_accesses));
    
    kfree(vrf_mgr);
    vrf_mgr = NULL;
}

/**
 * fdca_vrf_alloc_reg() - 分配向量寄存器
 */
int fdca_vrf_alloc_reg(void)
{
    int reg;
    
    if (!vrf_mgr)
        return -ENODEV;
    
    spin_lock(&vrf_mgr->alloc_lock);
    reg = find_first_zero_bit(vrf_mgr->allocated_regs, FDCA_RVV_NUM_VREGS);
    if (reg < FDCA_RVV_NUM_VREGS) {
        set_bit(reg, vrf_mgr->allocated_regs);
        atomic64_inc(&vrf_mgr->reg_allocations);
    } else {
        reg = -ENOSPC;
    }
    spin_unlock(&vrf_mgr->alloc_lock);
    
    return reg;
}

/**
 * fdca_vrf_free_reg() - 释放向量寄存器
 */
void fdca_vrf_free_reg(int reg)
{
    if (!vrf_mgr || reg < 0 || reg >= FDCA_RVV_NUM_VREGS)
        return;
    
    spin_lock(&vrf_mgr->alloc_lock);
    if (test_and_clear_bit(reg, vrf_mgr->allocated_regs)) {
        atomic64_inc(&vrf_mgr->reg_frees);
    }
    spin_unlock(&vrf_mgr->alloc_lock);
}

/**
 * fdca_vrf_read_lane() - 读取指定 lane 的向量寄存器数据
 */
int fdca_vrf_read_lane(int reg, int lane, void *buffer, size_t size)
{
    void __iomem *lane_base;
    
    if (!vrf_mgr || reg < 0 || reg >= FDCA_RVV_NUM_VREGS || 
        lane >= vrf_mgr->lane_info.num_lanes)
        return -EINVAL;
    
    lane_base = vrf_mgr->lane_info.lane_bases[lane];
    if (!lane_base)
        return -ENODEV;
    
    /* 读取寄存器数据 */
    memcpy_fromio(buffer, lane_base + (reg * vrf_mgr->lane_info.lane_width / 8), size);
    
    atomic64_inc(&vrf_mgr->lane_accesses);
    return 0;
}

/**
 * fdca_vrf_write_lane() - 写入指定 lane 的向量寄存器数据
 */
int fdca_vrf_write_lane(int reg, int lane, const void *buffer, size_t size)
{
    void __iomem *lane_base;
    
    if (!vrf_mgr || reg < 0 || reg >= FDCA_RVV_NUM_VREGS || 
        lane >= vrf_mgr->lane_info.num_lanes)
        return -EINVAL;
    
    lane_base = vrf_mgr->lane_info.lane_bases[lane];
    if (!lane_base)
        return -ENODEV;
    
    /* 写入寄存器数据 */
    memcpy_toio(lane_base + (reg * vrf_mgr->lane_info.lane_width / 8), buffer, size);
    
    atomic64_inc(&vrf_mgr->lane_accesses);
    return 0;
}

EXPORT_SYMBOL_GPL(fdca_vrf_init);
EXPORT_SYMBOL_GPL(fdca_vrf_fini);
EXPORT_SYMBOL_GPL(fdca_vrf_alloc_reg);
EXPORT_SYMBOL_GPL(fdca_vrf_free_reg);
EXPORT_SYMBOL_GPL(fdca_vrf_read_lane);
EXPORT_SYMBOL_GPL(fdca_vrf_write_lane);
