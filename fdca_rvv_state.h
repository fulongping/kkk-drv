/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * FDCA (Fangzheng Distributed Computing Architecture) RVV State Management
 * 
 * Copyright (C) 2024 Fangzheng Technology Co., Ltd.
 * 
 * RISC-V 向量扩展状态管理头文件
 * 
 * 本文件定义了 RVV 状态管理的接口和数据结构，包括：
 * 1. 向量控制状态寄存器 (CSR) 的定义
 * 2. 向量寄存器状态的保存和恢复
 * 3. 上下文切换时的状态管理
 * 4. 向量配置的动态管理
 * 5. 错误处理和诊断接口
 *
 * Author: FDCA Kernel Team
 * Date: 2024
 */

#ifndef __FDCA_RVV_STATE_H__
#define __FDCA_RVV_STATE_H__

#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/atomic.h>

/*
 * ============================================================================
 * RVV CSR 寄存器定义
 * ============================================================================
 */

/* RISC-V 向量扩展 CSR 地址 - 基于 RISC-V 规范 */
#define FDCA_CSR_VSTART     0x008       /* Vector start index */
#define FDCA_CSR_VXSAT      0x009       /* Fixed-point saturation flag */
#define FDCA_CSR_VXRM       0x00A       /* Fixed-point rounding mode */
#define FDCA_CSR_VCSR       0x00F       /* Vector control and status */
#define FDCA_CSR_VL         0xC20       /* Vector length */
#define FDCA_CSR_VTYPE      0xC21       /* Vector data type */
#define FDCA_CSR_VLENB      0xC22       /* Vector length in bytes */

/* VTYPE 寄存器位域定义 */
#define FDCA_VTYPE_VLMUL_MASK   0x7     /* Vector register length multiplier */
#define FDCA_VTYPE_VSEW_MASK    0x38    /* Selected element width */
#define FDCA_VTYPE_VTA          BIT(6)  /* Vector tail agnostic */
#define FDCA_VTYPE_VMA          BIT(7)  /* Vector mask agnostic */
#define FDCA_VTYPE_VILL         BIT(63) /* Illegal value */

#define FDCA_VTYPE_VLMUL_SHIFT  0
#define FDCA_VTYPE_VSEW_SHIFT   3

/* VLMUL 值定义 */
#define FDCA_VLMUL_1            0x0     /* LMUL = 1 */
#define FDCA_VLMUL_2            0x1     /* LMUL = 2 */
#define FDCA_VLMUL_4            0x2     /* LMUL = 4 */
#define FDCA_VLMUL_8            0x3     /* LMUL = 8 */
#define FDCA_VLMUL_RESERVED     0x4     /* Reserved */
#define FDCA_VLMUL_1_8          0x5     /* LMUL = 1/8 */
#define FDCA_VLMUL_1_4          0x6     /* LMUL = 1/4 */
#define FDCA_VLMUL_1_2          0x7     /* LMUL = 1/2 */

/* VSEW 值定义 */
#define FDCA_VSEW_8             0x0     /* SEW = 8 */
#define FDCA_VSEW_16            0x1     /* SEW = 16 */
#define FDCA_VSEW_32            0x2     /* SEW = 32 */
#define FDCA_VSEW_64            0x3     /* SEW = 64 */
#define FDCA_VSEW_128           0x4     /* SEW = 128 */
#define FDCA_VSEW_256           0x5     /* SEW = 256 */
#define FDCA_VSEW_512           0x6     /* SEW = 512 */
#define FDCA_VSEW_1024          0x7     /* SEW = 1024 */

/*
 * ============================================================================
 * RVV 状态结构定义
 * ============================================================================
 */

/**
 * struct fdca_rvv_csr_context - RVV CSR 上下文
 * 
 * 保存和恢复 RVV 控制状态寄存器的完整状态
 */
struct fdca_rvv_csr_context {
    /* 向量配置 CSR */
    u64 vstart;                     /* 向量起始索引 */
    u64 vxsat;                      /* 定点饱和标志 */
    u64 vxrm;                       /* 定点舍入模式 */
    u64 vcsr;                       /* 向量控制状态 */
    u64 vl;                         /* 向量长度 */
    u64 vtype;                      /* 向量数据类型 */
    u64 vlenb;                      /* 向量长度（字节） */
    
    /* 解析后的配置信息 */
    struct {
        u32 vlmul;                  /* LMUL 值 */
        u32 vsew;                   /* SEW 值 */
        bool vta;                   /* 尾部不可知 */
        bool vma;                   /* 掩码不可知 */
        bool vill;                  /* 非法配置 */
        u32 sew_bits;               /* SEW 位数 */
        u32 lmul_mul;               /* LMUL 倍数 */
        u32 lmul_div;               /* LMUL 除数 */
    } parsed;
    
    /* 状态标志 */
    bool valid;                     /* 上下文是否有效 */
    bool dirty;                     /* 是否需要保存 */
    u64 save_time;                  /* 保存时间 */
    u32 save_count;                 /* 保存计数 */
};

/**
 * struct fdca_rvv_register_state - RVV 寄存器状态
 * 
 * 保存向量寄存器文件的完整状态
 */
struct fdca_rvv_register_state {
    /* 向量寄存器数据 */
    void *vregs_data;               /* 向量寄存器数据 */
    size_t vregs_size;              /* 数据大小 */
    u32 num_vregs;                  /* 寄存器数量 */
    
    /* 掩码寄存器 */
    void *vmask_data;               /* 掩码寄存器数据 */
    size_t vmask_size;              /* 掩码数据大小 */
    
    /* 状态信息 */
    bool allocated;                 /* 是否已分配 */
    bool saved;                     /* 是否已保存 */
    u64 save_time;                  /* 保存时间戳 */
    atomic_t ref_count;             /* 引用计数 */
};

/**
 * struct fdca_rvv_context - 完整的 RVV 上下文
 * 
 * 包含进程的完整 RVV 状态，用于上下文切换
 */
struct fdca_rvv_context {
    /* CSR 状态 */
    struct fdca_rvv_csr_context csr;       /* CSR 上下文 */
    
    /* 寄存器状态 */
    struct fdca_rvv_register_state regs;   /* 寄存器状态 */
    
    /* 上下文管理 */
    struct mutex lock;                      /* 上下文锁 */
    bool active;                            /* 上下文是否活跃 */
    bool preempted;                         /* 是否被抢占 */
    
    /* 性能统计 */
    struct {
        atomic64_t save_count;              /* 保存次数 */
        atomic64_t restore_count;           /* 恢复次数 */
        u64 total_save_time;                /* 总保存时间 */
        u64 total_restore_time;             /* 总恢复时间 */
    } stats;
    
    /* 调试信息 */
    pid_t owner_pid;                        /* 所有者进程 PID */
    char comm[TASK_COMM_LEN];               /* 进程名称 */
    u64 create_time;                        /* 创建时间 */
    u64 last_use_time;                      /* 最后使用时间 */
};

/**
 * struct fdca_rvv_state_manager - RVV 状态管理器
 * 
 * 管理系统中所有的 RVV 状态
 */
struct fdca_rvv_state_manager {
    struct fdca_device *fdev;               /* 关联设备 */
    
    /* 硬件配置 */
    struct fdca_rvv_config *hw_config;      /* 硬件配置 */
    bool hw_available;                      /* 硬件是否可用 */
    
    /* 上下文管理 */
    struct fdca_rvv_context *current_ctx;   /* 当前活跃上下文 */
    struct list_head context_list;          /* 上下文列表 */
    struct mutex context_lock;               /* 上下文列表锁 */
    atomic_t context_count;                  /* 上下文计数 */
    
    /* 预分配资源池 */
    struct {
        void **buffers;                     /* 预分配缓冲区 */
        bool *used;                         /* 使用标记 */
        u32 pool_size;                      /* 池大小 */
        struct mutex pool_lock;             /* 池锁 */
    } buffer_pool;
    
    /* 性能优化 */
    bool lazy_save;                         /* 延迟保存 */
    bool fast_switch;                       /* 快速切换 */
    u32 save_threshold;                     /* 保存阈值 */
    
    /* 统计信息 */
    struct {
        atomic64_t total_switches;          /* 总切换次数 */
        atomic64_t lazy_saves;              /* 延迟保存次数 */
        atomic64_t fast_switches;           /* 快速切换次数 */
        u64 avg_save_time;                  /* 平均保存时间 */
        u64 avg_restore_time;               /* 平均恢复时间 */
    } stats;
    
    /* 错误处理 */
    struct {
        atomic_t save_errors;               /* 保存错误计数 */
        atomic_t restore_errors;            /* 恢复错误计数 */
        atomic_t corruption_detected;       /* 检测到损坏 */
        bool error_recovery_active;         /* 错误恢复激活 */
    } error_handling;
};

/*
 * ============================================================================
 * 函数声明
 * ============================================================================
 */

/* 状态管理器初始化和清理 */
int fdca_rvv_state_manager_init(struct fdca_device *fdev);
void fdca_rvv_state_manager_fini(struct fdca_device *fdev);

/* 上下文管理 */
struct fdca_rvv_context *fdca_rvv_context_create(struct fdca_device *fdev);
void fdca_rvv_context_destroy(struct fdca_rvv_context *ctx);
int fdca_rvv_context_save(struct fdca_rvv_context *ctx);
int fdca_rvv_context_restore(struct fdca_rvv_context *ctx);

/* CSR 操作 */
int fdca_rvv_csr_save(struct fdca_rvv_csr_context *csr_ctx);
int fdca_rvv_csr_restore(struct fdca_rvv_csr_context *csr_ctx);
int fdca_rvv_csr_validate(struct fdca_rvv_csr_context *csr_ctx);
void fdca_rvv_csr_parse_vtype(struct fdca_rvv_csr_context *csr_ctx);

/* 寄存器操作 */
int fdca_rvv_regs_save(struct fdca_rvv_register_state *reg_state, 
                       const struct fdca_rvv_config *config);
int fdca_rvv_regs_restore(struct fdca_rvv_register_state *reg_state,
                          const struct fdca_rvv_config *config);
int fdca_rvv_regs_alloc(struct fdca_rvv_register_state *reg_state,
                        const struct fdca_rvv_config *config);
void fdca_rvv_regs_free(struct fdca_rvv_register_state *reg_state);

/* 上下文切换 */
int fdca_rvv_switch_context(struct fdca_device *fdev,
                           struct fdca_rvv_context *new_ctx);
void fdca_rvv_preempt_context(struct fdca_device *fdev);
int fdca_rvv_resume_context(struct fdca_device *fdev,
                           struct fdca_rvv_context *ctx);

/* 配置管理 */
bool fdca_rvv_config_is_valid(const struct fdca_rvv_csr_context *csr_ctx,
                             const struct fdca_rvv_config *hw_config);
u32 fdca_rvv_get_vreg_size(const struct fdca_rvv_csr_context *csr_ctx,
                          const struct fdca_rvv_config *hw_config);
u32 fdca_rvv_get_total_regs_size(const struct fdca_rvv_csr_context *csr_ctx,
                                const struct fdca_rvv_config *hw_config);

/* 调试和监控 */
void fdca_rvv_print_csr_state(const struct fdca_rvv_csr_context *csr_ctx);
void fdca_rvv_print_context_stats(const struct fdca_rvv_context *ctx);
void fdca_rvv_print_manager_stats(const struct fdca_rvv_state_manager *mgr);
int fdca_rvv_verify_context_integrity(const struct fdca_rvv_context *ctx);

/* 错误处理 */
int fdca_rvv_handle_save_error(struct fdca_rvv_context *ctx, int error);
int fdca_rvv_handle_restore_error(struct fdca_rvv_context *ctx, int error);
void fdca_rvv_reset_error_state(struct fdca_device *fdev);

/* 内联辅助函数 */
static inline bool fdca_rvv_context_is_valid(const struct fdca_rvv_context *ctx)
{
    return ctx && ctx->csr.valid && ctx->regs.allocated;
}

static inline bool fdca_rvv_context_is_dirty(const struct fdca_rvv_context *ctx)
{
    return ctx && (ctx->csr.dirty || ctx->active);
}

static inline u32 fdca_rvv_get_sew_bits(u32 vsew)
{
    return 8 << vsew;  /* SEW = 8 * 2^vsew */
}

static inline void fdca_rvv_get_lmul_fraction(u32 vlmul, u32 *mul, u32 *div)
{
    if (vlmul <= 3) {
        *mul = 1 << vlmul;      /* 1, 2, 4, 8 */
        *div = 1;
    } else if (vlmul >= 5 && vlmul <= 7) {
        *mul = 1;
        *div = 1 << (8 - vlmul); /* 1/8, 1/4, 1/2 */
    } else {
        *mul = 1;               /* Reserved, default to 1 */
        *div = 1;
    }
}

#endif /* __FDCA_RVV_STATE_H__ */
