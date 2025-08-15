// SPDX-License-Identifier: GPL-2.0-only
/*
 * FDCA (Fangzheng Distributed Computing Architecture) RVV State Management
 * 
 * Copyright (C) 2024 Fangzheng Technology Co., Ltd.
 * 
 * RISC-V 向量扩展状态管理实现
 * 
 * 本模块负责：
 * 1. RVV CSR 状态的保存和恢复
 * 2. 向量寄存器的上下文切换
 * 3. 多进程 RVV 状态隔离
 * 4. RVV 配置验证和管理
 * 5. 性能优化和错误处理
 * 6. 调试和监控接口
 *
 * Author: FDCA Kernel Team
 * Date: 2024
 */

#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/ktime.h>
#include <linux/delay.h>

#include "fdca_drv.h"
#include "fdca_rvv_state.h"

/*
 * ============================================================================
 * 全局变量
 * ============================================================================
 */

/* RVV 状态管理器实例 */
static struct fdca_rvv_state_manager *g_rvv_manager = NULL;

/* 预分配缓冲区池大小 */
#define FDCA_RVV_BUFFER_POOL_SIZE   16

/*
 * ============================================================================
 * CSR 操作函数
 * ============================================================================
 */

/**
 * fdca_rvv_read_csr() - 读取 RVV CSR 寄存器
 * @csr_addr: CSR 地址
 * 
 * Return: CSR 值
 */
static u64 fdca_rvv_read_csr(u32 csr_addr)
{
    u64 value = 0;
    
    /* 这里需要通过 MMIO 或其他方式读取 CSR */
    /* 由于这是模拟实现，返回默认值 */
    switch (csr_addr) {
    case FDCA_CSR_VSTART:
        value = 0;  /* 通常初始为 0 */
        break;
    case FDCA_CSR_VXSAT:
        value = 0;  /* 未饱和 */
        break;
    case FDCA_CSR_VXRM:
        value = 0;  /* 舍入到最近偶数 */
        break;
    case FDCA_CSR_VCSR:
        value = 0;  /* 合成 vxsat 和 vxrm */
        break;
    case FDCA_CSR_VL:
        value = 0;  /* 当前向量长度 */
        break;
    case FDCA_CSR_VTYPE:
        value = FDCA_VTYPE_VILL;  /* 默认为非法值 */
        break;
    case FDCA_CSR_VLENB:
        if (g_rvv_manager && g_rvv_manager->hw_config) {
            value = g_rvv_manager->hw_config->vlenb;
        } else {
            value = 128;  /* 默认 1024 bits = 128 bytes */
        }
        break;
    default:
        value = 0;
        break;
    }
    
    return value;
}

/**
 * fdca_rvv_write_csr() - 写入 RVV CSR 寄存器
 * @csr_addr: CSR 地址
 * @value: 要写入的值
 */
static void fdca_rvv_write_csr(u32 csr_addr, u64 value)
{
    /* 这里需要通过 MMIO 或其他方式写入 CSR */
    /* 由于这是模拟实现，只做日志记录 */
    
    if (g_rvv_manager && g_rvv_manager->fdev) {
        fdca_dbg(g_rvv_manager->fdev, "写入 CSR[0x%03x] = 0x%llx\n", 
                 csr_addr, value);
    }
}

/**
 * fdca_rvv_csr_parse_vtype() - 解析 VTYPE 寄存器
 * @csr_ctx: CSR 上下文
 */
void fdca_rvv_csr_parse_vtype(struct fdca_rvv_csr_context *csr_ctx)
{
    u64 vtype = csr_ctx->vtype;
    
    /* 检查是否为非法值 */
    csr_ctx->parsed.vill = !!(vtype & FDCA_VTYPE_VILL);
    if (csr_ctx->parsed.vill) {
        return;
    }
    
    /* 解析各个字段 */
    csr_ctx->parsed.vlmul = (vtype >> FDCA_VTYPE_VLMUL_SHIFT) & FDCA_VTYPE_VLMUL_MASK;
    csr_ctx->parsed.vsew = (vtype >> FDCA_VTYPE_VSEW_SHIFT) & (FDCA_VTYPE_VSEW_MASK >> FDCA_VTYPE_VSEW_SHIFT);
    csr_ctx->parsed.vta = !!(vtype & FDCA_VTYPE_VTA);
    csr_ctx->parsed.vma = !!(vtype & FDCA_VTYPE_VMA);
    
    /* 计算 SEW 位数 */
    csr_ctx->parsed.sew_bits = fdca_rvv_get_sew_bits(csr_ctx->parsed.vsew);
    
    /* 计算 LMUL 分数 */
    fdca_rvv_get_lmul_fraction(csr_ctx->parsed.vlmul,
                              &csr_ctx->parsed.lmul_mul,
                              &csr_ctx->parsed.lmul_div);
}

/**
 * fdca_rvv_csr_save() - 保存 CSR 状态
 * @csr_ctx: CSR 上下文
 * 
 * Return: 0 表示成功，负数表示错误
 */
int fdca_rvv_csr_save(struct fdca_rvv_csr_context *csr_ctx)
{
    u64 start_time, end_time;
    
    if (!csr_ctx) {
        return -EINVAL;
    }
    
    start_time = ktime_get_ns();
    
    /* 读取所有 RVV CSR */
    csr_ctx->vstart = fdca_rvv_read_csr(FDCA_CSR_VSTART);
    csr_ctx->vxsat = fdca_rvv_read_csr(FDCA_CSR_VXSAT);
    csr_ctx->vxrm = fdca_rvv_read_csr(FDCA_CSR_VXRM);
    csr_ctx->vcsr = fdca_rvv_read_csr(FDCA_CSR_VCSR);
    csr_ctx->vl = fdca_rvv_read_csr(FDCA_CSR_VL);
    csr_ctx->vtype = fdca_rvv_read_csr(FDCA_CSR_VTYPE);
    csr_ctx->vlenb = fdca_rvv_read_csr(FDCA_CSR_VLENB);
    
    /* 解析 VTYPE */
    fdca_rvv_csr_parse_vtype(csr_ctx);
    
    /* 更新状态 */
    csr_ctx->valid = true;
    csr_ctx->dirty = false;
    csr_ctx->save_time = ktime_get_boottime_seconds();
    csr_ctx->save_count++;
    
    end_time = ktime_get_ns();
    
    if (g_rvv_manager && g_rvv_manager->fdev) {
        fdca_dbg(g_rvv_manager->fdev, "CSR 保存完成，耗时 %llu ns\n",
                 end_time - start_time);
    }
    
    return 0;
}

/**
 * fdca_rvv_csr_restore() - 恢复 CSR 状态
 * @csr_ctx: CSR 上下文
 * 
 * Return: 0 表示成功，负数表示错误
 */
int fdca_rvv_csr_restore(struct fdca_rvv_csr_context *csr_ctx)
{
    u64 start_time, end_time;
    
    if (!csr_ctx || !csr_ctx->valid) {
        return -EINVAL;
    }
    
    start_time = ktime_get_ns();
    
    /* 恢复所有 RVV CSR */
    fdca_rvv_write_csr(FDCA_CSR_VSTART, csr_ctx->vstart);
    fdca_rvv_write_csr(FDCA_CSR_VXSAT, csr_ctx->vxsat);
    fdca_rvv_write_csr(FDCA_CSR_VXRM, csr_ctx->vxrm);
    fdca_rvv_write_csr(FDCA_CSR_VCSR, csr_ctx->vcsr);
    fdca_rvv_write_csr(FDCA_CSR_VL, csr_ctx->vl);
    fdca_rvv_write_csr(FDCA_CSR_VTYPE, csr_ctx->vtype);
    /* VLENB 是只读寄存器，不需要恢复 */
    
    end_time = ktime_get_ns();
    
    if (g_rvv_manager && g_rvv_manager->fdev) {
        fdca_dbg(g_rvv_manager->fdev, "CSR 恢复完成，耗时 %llu ns\n",
                 end_time - start_time);
    }
    
    return 0;
}

/**
 * fdca_rvv_csr_validate() - 验证 CSR 状态
 * @csr_ctx: CSR 上下文
 * 
 * Return: 0 表示有效，负数表示无效
 */
int fdca_rvv_csr_validate(struct fdca_rvv_csr_context *csr_ctx)
{
    if (!csr_ctx) {
        return -EINVAL;
    }
    
    /* 检查 VTYPE 是否为非法值 */
    if (csr_ctx->parsed.vill) {
        return -EINVAL;
    }
    
    /* 检查 VL 是否超出范围 */
    if (g_rvv_manager && g_rvv_manager->hw_config) {
        u32 max_vl = g_rvv_manager->hw_config->vlen / csr_ctx->parsed.sew_bits;
        if (csr_ctx->vl > max_vl) {
            return -ERANGE;
        }
    }
    
    /* 检查 VSTART 是否合理 */
    if (csr_ctx->vstart > csr_ctx->vl) {
        return -ERANGE;
    }
    
    return 0;
}

/*
 * ============================================================================
 * 寄存器状态管理
 * ============================================================================
 */

/**
 * fdca_rvv_regs_alloc() - 分配寄存器状态存储
 * @reg_state: 寄存器状态结构
 * @config: 硬件配置
 * 
 * Return: 0 表示成功，负数表示错误
 */
int fdca_rvv_regs_alloc(struct fdca_rvv_register_state *reg_state,
                        const struct fdca_rvv_config *config)
{
    size_t vregs_size, vmask_size;
    
    if (!reg_state || !config) {
        return -EINVAL;
    }
    
    /* 计算所需内存大小 */
    vregs_size = config->vlen / 8 * FDCA_RVV_NUM_VREGS;  /* 32 个向量寄存器 */
    vmask_size = config->vlen / 8;  /* 1 个掩码寄存器 */
    
    /* 分配向量寄存器存储 */
    reg_state->vregs_data = kzalloc(vregs_size, GFP_KERNEL);
    if (!reg_state->vregs_data) {
        return -ENOMEM;
    }
    
    /* 分配掩码寄存器存储 */
    reg_state->vmask_data = kzalloc(vmask_size, GFP_KERNEL);
    if (!reg_state->vmask_data) {
        kfree(reg_state->vregs_data);
        reg_state->vregs_data = NULL;
        return -ENOMEM;
    }
    
    /* 设置状态信息 */
    reg_state->vregs_size = vregs_size;
    reg_state->vmask_size = vmask_size;
    reg_state->num_vregs = FDCA_RVV_NUM_VREGS;
    reg_state->allocated = true;
    reg_state->saved = false;
    atomic_set(&reg_state->ref_count, 1);
    
    if (g_rvv_manager && g_rvv_manager->fdev) {
        fdca_dbg(g_rvv_manager->fdev, 
                 "分配寄存器存储: VREGS=%zu bytes, VMASK=%zu bytes\n",
                 vregs_size, vmask_size);
    }
    
    return 0;
}

/**
 * fdca_rvv_regs_free() - 释放寄存器状态存储
 * @reg_state: 寄存器状态结构
 */
void fdca_rvv_regs_free(struct fdca_rvv_register_state *reg_state)
{
    if (!reg_state || !reg_state->allocated) {
        return;
    }
    
    /* 检查引用计数 */
    if (atomic_dec_return(&reg_state->ref_count) > 0) {
        return;
    }
    
    /* 释放内存 */
    kfree(reg_state->vmask_data);
    kfree(reg_state->vregs_data);
    
    /* 清零状态 */
    memset(reg_state, 0, sizeof(*reg_state));
    
    if (g_rvv_manager && g_rvv_manager->fdev) {
        fdca_dbg(g_rvv_manager->fdev, "释放寄存器存储\n");
    }
}

/**
 * fdca_rvv_regs_save() - 保存寄存器状态
 * @reg_state: 寄存器状态结构
 * @config: 硬件配置
 * 
 * Return: 0 表示成功，负数表示错误
 */
int fdca_rvv_regs_save(struct fdca_rvv_register_state *reg_state,
                       const struct fdca_rvv_config *config)
{
    u64 start_time, end_time;
    
    if (!reg_state || !reg_state->allocated || !config) {
        return -EINVAL;
    }
    
    start_time = ktime_get_ns();
    
    /* 这里需要通过硬件接口保存向量寄存器 */
    /* 由于这是模拟实现，只是清零数据 */
    memset(reg_state->vregs_data, 0, reg_state->vregs_size);
    memset(reg_state->vmask_data, 0, reg_state->vmask_size);
    
    /* 更新状态 */
    reg_state->saved = true;
    reg_state->save_time = ktime_get_boottime_seconds();
    
    end_time = ktime_get_ns();
    
    if (g_rvv_manager && g_rvv_manager->fdev) {
        fdca_dbg(g_rvv_manager->fdev, "寄存器保存完成，耗时 %llu ns\n",
                 end_time - start_time);
    }
    
    return 0;
}

/**
 * fdca_rvv_regs_restore() - 恢复寄存器状态
 * @reg_state: 寄存器状态结构
 * @config: 硬件配置
 * 
 * Return: 0 表示成功，负数表示错误
 */
int fdca_rvv_regs_restore(struct fdca_rvv_register_state *reg_state,
                          const struct fdca_rvv_config *config)
{
    u64 start_time, end_time;
    
    if (!reg_state || !reg_state->allocated || !reg_state->saved || !config) {
        return -EINVAL;
    }
    
    start_time = ktime_get_ns();
    
    /* 这里需要通过硬件接口恢复向量寄存器 */
    /* 由于这是模拟实现，只做时间模拟 */
    
    end_time = ktime_get_ns();
    
    if (g_rvv_manager && g_rvv_manager->fdev) {
        fdca_dbg(g_rvv_manager->fdev, "寄存器恢复完成，耗时 %llu ns\n",
                 end_time - start_time);
    }
    
    return 0;
}

/*
 * ============================================================================
 * 上下文管理
 * ============================================================================
 */

/**
 * fdca_rvv_context_create() - 创建 RVV 上下文
 * @fdev: FDCA 设备
 * 
 * Return: 上下文指针或 ERR_PTR
 */
struct fdca_rvv_context *fdca_rvv_context_create(struct fdca_device *fdev)
{
    struct fdca_rvv_context *ctx;
    int ret;
    
    if (!fdev || !fdev->rvv_available) {
        return ERR_PTR(-ENODEV);
    }
    
    /* 分配上下文结构 */
    ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
    if (!ctx) {
        return ERR_PTR(-ENOMEM);
    }
    
    /* 初始化基础字段 */
    mutex_init(&ctx->lock);
    ctx->active = false;
    ctx->preempted = false;
    
    /* 初始化统计信息 */
    atomic64_set(&ctx->stats.save_count, 0);
    atomic64_set(&ctx->stats.restore_count, 0);
    ctx->stats.total_save_time = 0;
    ctx->stats.total_restore_time = 0;
    
    /* 设置调试信息 */
    ctx->owner_pid = current->pid;
    strncpy(ctx->comm, current->comm, sizeof(ctx->comm) - 1);
    ctx->create_time = ktime_get_boottime_seconds();
    ctx->last_use_time = ctx->create_time;
    
    /* 分配寄存器存储 */
    ret = fdca_rvv_regs_alloc(&ctx->regs, &fdev->rvv_config);
    if (ret) {
        fdca_err(fdev, "寄存器状态分配失败: %d\n", ret);
        kfree(ctx);
        return ERR_PTR(ret);
    }
    
    /* 初始化 CSR 状态 */
    memset(&ctx->csr, 0, sizeof(ctx->csr));
    ctx->csr.valid = false;
    ctx->csr.dirty = false;
    
    fdca_dbg(fdev, "RVV 上下文创建: PID=%d, 名称=%s\n",
             ctx->owner_pid, ctx->comm);
    
    return ctx;
}

/**
 * fdca_rvv_context_destroy() - 销毁 RVV 上下文
 * @ctx: RVV 上下文
 */
void fdca_rvv_context_destroy(struct fdca_rvv_context *ctx)
{
    if (!ctx) {
        return;
    }
    
    if (g_rvv_manager && g_rvv_manager->fdev) {
        fdca_dbg(g_rvv_manager->fdev, "销毁 RVV 上下文: PID=%d\n", 
                 ctx->owner_pid);
    }
    
    /* 释放寄存器存储 */
    fdca_rvv_regs_free(&ctx->regs);
    
    /* 释放上下文结构 */
    kfree(ctx);
}

/**
 * fdca_rvv_context_save() - 保存 RVV 上下文
 * @ctx: RVV 上下文
 * 
 * Return: 0 表示成功，负数表示错误
 */
int fdca_rvv_context_save(struct fdca_rvv_context *ctx)
{
    u64 start_time, end_time;
    int ret;
    
    if (!ctx) {
        return -EINVAL;
    }
    
    mutex_lock(&ctx->lock);
    
    start_time = ktime_get_ns();
    
    /* 保存 CSR 状态 */
    ret = fdca_rvv_csr_save(&ctx->csr);
    if (ret) {
        goto out_unlock;
    }
    
    /* 保存寄存器状态 */
    if (g_rvv_manager && g_rvv_manager->hw_config) {
        ret = fdca_rvv_regs_save(&ctx->regs, g_rvv_manager->hw_config);
        if (ret) {
            goto out_unlock;
        }
    }
    
    /* 更新统计信息 */
    end_time = ktime_get_ns();
    atomic64_inc(&ctx->stats.save_count);
    ctx->stats.total_save_time += (end_time - start_time);
    ctx->last_use_time = ktime_get_boottime_seconds();
    
    ctx->active = false;
    
out_unlock:
    mutex_unlock(&ctx->lock);
    return ret;
}

/**
 * fdca_rvv_context_restore() - 恢复 RVV 上下文
 * @ctx: RVV 上下文
 * 
 * Return: 0 表示成功，负数表示错误
 */
int fdca_rvv_context_restore(struct fdca_rvv_context *ctx)
{
    u64 start_time, end_time;
    int ret;
    
    if (!ctx) {
        return -EINVAL;
    }
    
    mutex_lock(&ctx->lock);
    
    start_time = ktime_get_ns();
    
    /* 恢复 CSR 状态 */
    ret = fdca_rvv_csr_restore(&ctx->csr);
    if (ret) {
        goto out_unlock;
    }
    
    /* 恢复寄存器状态 */
    if (g_rvv_manager && g_rvv_manager->hw_config) {
        ret = fdca_rvv_regs_restore(&ctx->regs, g_rvv_manager->hw_config);
        if (ret) {
            goto out_unlock;
        }
    }
    
    /* 更新统计信息 */
    end_time = ktime_get_ns();
    atomic64_inc(&ctx->stats.restore_count);
    ctx->stats.total_restore_time += (end_time - start_time);
    ctx->last_use_time = ktime_get_boottime_seconds();
    
    ctx->active = true;
    ctx->preempted = false;
    
out_unlock:
    mutex_unlock(&ctx->lock);
    return ret;
}

/*
 * ============================================================================
 * 状态管理器实现
 * ============================================================================
 */

/**
 * fdca_rvv_state_manager_init() - 初始化 RVV 状态管理器
 * @fdev: FDCA 设备
 * 
 * Return: 0 表示成功，负数表示错误
 */
int fdca_rvv_state_manager_init(struct fdca_device *fdev)
{
    struct fdca_rvv_state_manager *mgr;
    
    if (!fdev) {
        return -EINVAL;
    }
    
    fdca_info(fdev, "初始化 RVV 状态管理器\n");
    
    /* 分配管理器结构 */
    mgr = kzalloc(sizeof(*mgr), GFP_KERNEL);
    if (!mgr) {
        fdca_err(fdev, "无法分配 RVV 状态管理器\n");
        return -ENOMEM;
    }
    
    /* 初始化基础字段 */
    mgr->fdev = fdev;
    mgr->hw_config = &fdev->rvv_config;
    mgr->hw_available = fdev->rvv_available;
    mgr->current_ctx = NULL;
    
    /* 初始化上下文管理 */
    INIT_LIST_HEAD(&mgr->context_list);
    mutex_init(&mgr->context_lock);
    atomic_set(&mgr->context_count, 0);
    
    /* 初始化缓冲区池 */
    mgr->buffer_pool.pool_size = FDCA_RVV_BUFFER_POOL_SIZE;
    mgr->buffer_pool.buffers = kcalloc(mgr->buffer_pool.pool_size,
                                      sizeof(void *), GFP_KERNEL);
    mgr->buffer_pool.used = kcalloc(mgr->buffer_pool.pool_size,
                                   sizeof(bool), GFP_KERNEL);
    if (!mgr->buffer_pool.buffers || !mgr->buffer_pool.used) {
        fdca_err(fdev, "缓冲区池初始化失败\n");
        kfree(mgr->buffer_pool.buffers);
        kfree(mgr->buffer_pool.used);
        kfree(mgr);
        return -ENOMEM;
    }
    mutex_init(&mgr->buffer_pool.pool_lock);
    
    /* 设置性能优化参数 */
    mgr->lazy_save = true;
    mgr->fast_switch = true;
    mgr->save_threshold = 10;  /* 10ms */
    
    /* 初始化统计信息 */
    atomic64_set(&mgr->stats.total_switches, 0);
    atomic64_set(&mgr->stats.lazy_saves, 0);
    atomic64_set(&mgr->stats.fast_switches, 0);
    mgr->stats.avg_save_time = 0;
    mgr->stats.avg_restore_time = 0;
    
    /* 初始化错误处理 */
    atomic_set(&mgr->error_handling.save_errors, 0);
    atomic_set(&mgr->error_handling.restore_errors, 0);
    atomic_set(&mgr->error_handling.corruption_detected, 0);
    mgr->error_handling.error_recovery_active = false;
    
    /* 设置全局管理器 */
    g_rvv_manager = mgr;
    
    fdca_info(fdev, "RVV 状态管理器初始化完成\n");
    
    return 0;
}

/**
 * fdca_rvv_state_manager_fini() - 清理 RVV 状态管理器
 * @fdev: FDCA 设备
 */
void fdca_rvv_state_manager_fini(struct fdca_device *fdev)
{
    struct fdca_rvv_state_manager *mgr = g_rvv_manager;
    int i;
    
    if (!mgr || mgr->fdev != fdev) {
        return;
    }
    
    fdca_info(fdev, "清理 RVV 状态管理器\n");
    
    /* 清理缓冲区池 */
    if (mgr->buffer_pool.buffers) {
        for (i = 0; i < mgr->buffer_pool.pool_size; i++) {
            kfree(mgr->buffer_pool.buffers[i]);
        }
        kfree(mgr->buffer_pool.buffers);
        kfree(mgr->buffer_pool.used);
    }
    
    /* 打印统计信息 */
    fdca_info(fdev, "RVV 统计: 切换 %lld 次, 保存错误 %d 次, 恢复错误 %d 次\n",
              atomic64_read(&mgr->stats.total_switches),
              atomic_read(&mgr->error_handling.save_errors),
              atomic_read(&mgr->error_handling.restore_errors));
    
    /* 清理管理器结构 */
    kfree(mgr);
    g_rvv_manager = NULL;
    
    fdca_info(fdev, "RVV 状态管理器清理完成\n");
}

/*
 * ============================================================================
 * 调试和监控函数
 * ============================================================================
 */

/**
 * fdca_rvv_print_csr_state() - 打印 CSR 状态
 * @csr_ctx: CSR 上下文
 */
void fdca_rvv_print_csr_state(const struct fdca_rvv_csr_context *csr_ctx)
{
    if (!csr_ctx || !g_rvv_manager || !g_rvv_manager->fdev) {
        return;
    }
    
    fdca_info(g_rvv_manager->fdev, "=== RVV CSR 状态 ===\n");
    fdca_info(g_rvv_manager->fdev, "VSTART: 0x%llx\n", csr_ctx->vstart);
    fdca_info(g_rvv_manager->fdev, "VXSAT:  0x%llx\n", csr_ctx->vxsat);
    fdca_info(g_rvv_manager->fdev, "VXRM:   0x%llx\n", csr_ctx->vxrm);
    fdca_info(g_rvv_manager->fdev, "VCSR:   0x%llx\n", csr_ctx->vcsr);
    fdca_info(g_rvv_manager->fdev, "VL:     0x%llx\n", csr_ctx->vl);
    fdca_info(g_rvv_manager->fdev, "VTYPE:  0x%llx\n", csr_ctx->vtype);
    fdca_info(g_rvv_manager->fdev, "VLENB:  0x%llx\n", csr_ctx->vlenb);
    
    if (!csr_ctx->parsed.vill) {
        fdca_info(g_rvv_manager->fdev, "SEW: %u bits, LMUL: %u/%u\n",
                  csr_ctx->parsed.sew_bits,
                  csr_ctx->parsed.lmul_mul,
                  csr_ctx->parsed.lmul_div);
    } else {
        fdca_info(g_rvv_manager->fdev, "VTYPE: 非法值\n");
    }
}

/*
 * ============================================================================
 * 导出符号
 * ============================================================================
 */

EXPORT_SYMBOL_GPL(fdca_rvv_context_create);
EXPORT_SYMBOL_GPL(fdca_rvv_context_destroy);
EXPORT_SYMBOL_GPL(fdca_rvv_context_save);
EXPORT_SYMBOL_GPL(fdca_rvv_context_restore);
EXPORT_SYMBOL_GPL(fdca_rvv_csr_save);
EXPORT_SYMBOL_GPL(fdca_rvv_csr_restore);
EXPORT_SYMBOL_GPL(fdca_rvv_csr_validate);
EXPORT_SYMBOL_GPL(fdca_rvv_csr_parse_vtype);
EXPORT_SYMBOL_GPL(fdca_rvv_print_csr_state);
