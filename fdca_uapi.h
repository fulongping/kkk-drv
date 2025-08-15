/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * FDCA User API Interface
 * 
 * 定义 IOCTL 接口，暴露设备能力给用户态运行时
 * 包括向量配置查询和任务提交接口
 */

#ifndef __FDCA_UAPI_H__
#define __FDCA_UAPI_H__

#include <linux/types.h>

/* FDCA 设备参数 */
#define FDCA_PARAM_DEVICE_ID        0
#define FDCA_PARAM_REVISION_ID      1
#define FDCA_PARAM_VLEN             2    /* 向量长度 */
#define FDCA_PARAM_ELEN             3    /* 元素长度 */
#define FDCA_PARAM_NUM_LANES        4    /* Lane 数量 */
#define FDCA_PARAM_CAU_QUEUES       5    /* CAU 队列数 */
#define FDCA_PARAM_CFU_QUEUES       6    /* CFU 队列数 */
#define FDCA_PARAM_VRAM_SIZE        7    /* VRAM 大小 */
#define FDCA_PARAM_GTT_SIZE         8    /* GTT 大小 */
#define FDCA_PARAM_NOC_BANDWIDTH    9    /* NoC 带宽 */
#define FDCA_PARAM_MAX_CONTEXTS     10   /* 最大上下文数 */

/* GEM 对象创建标志 */
#define FDCA_GEM_CREATE_CACHED      BIT(0)
#define FDCA_GEM_CREATE_UNCACHED    BIT(1)
#define FDCA_GEM_CREATE_COHERENT    BIT(2)
#define FDCA_GEM_CREATE_LARGE_PAGE  BIT(3)

/* 任务提交标志 */
#define FDCA_SUBMIT_CAU             BIT(0)   /* 提交到 CAU */
#define FDCA_SUBMIT_CFU             BIT(1)   /* 提交到 CFU */
#define FDCA_SUBMIT_SYNC            BIT(2)   /* 同步提交 */
#define FDCA_SUBMIT_ASYNC           BIT(3)   /* 异步提交 */

/*
 * ============================================================================
 * IOCTL 数据结构
 * ============================================================================
 */

/**
 * struct drm_fdca_get_param - 获取设备参数
 */
struct drm_fdca_get_param {
    __u32 param;        /* 参数类型 */
    __u32 pad;
    __u64 value;        /* 返回值 */
};

/**
 * struct drm_fdca_gem_create - 创建 GEM 对象
 */
struct drm_fdca_gem_create {
    __u64 size;         /* 对象大小 */
    __u32 flags;        /* 创建标志 */
    __u32 handle;       /* 返回句柄 */
};

/**
 * struct drm_fdca_gem_mmap - 映射 GEM 对象
 */
struct drm_fdca_gem_mmap {
    __u32 handle;       /* GEM 句柄 */
    __u32 pad;
    __u64 offset;       /* 映射偏移 */
    __u64 size;         /* 映射大小 */
    __u64 addr_ptr;     /* 返回的用户地址 */
};

/**
 * struct drm_fdca_rvv_config - RVV 配置信息
 */
struct drm_fdca_rvv_config {
    __u32 vlen;         /* 向量长度 */
    __u32 elen;         /* 元素长度 */
    __u32 num_lanes;    /* Lane 数量 */
    __u32 vlenb;        /* 向量长度（字节） */
    __u32 num_vregs;    /* 向量寄存器数量 */
    __u32 pad;
};

/**
 * struct drm_fdca_context_create - 创建执行上下文
 */
struct drm_fdca_context_create {
    __u32 flags;        /* 上下文标志 */
    __u32 ctx_id;       /* 返回上下文 ID */
};

/**
 * struct drm_fdca_context_destroy - 销毁执行上下文
 */
struct drm_fdca_context_destroy {
    __u32 ctx_id;       /* 上下文 ID */
    __u32 pad;
};

/**
 * struct drm_fdca_command - 命令描述符
 */
struct drm_fdca_command {
    __u32 type;         /* 命令类型 */
    __u32 size;         /* 命令大小 */
    __u64 data_ptr;     /* 命令数据指针 */
    __u32 num_deps;     /* 依赖数量 */
    __u32 pad;
    __u64 deps_ptr;     /* 依赖数组指针 */
};

/**
 * struct drm_fdca_submit - 任务提交
 */
struct drm_fdca_submit {
    __u32 ctx_id;       /* 上下文 ID */
    __u32 flags;        /* 提交标志 */
    __u32 num_cmds;     /* 命令数量 */
    __u32 fence_out;    /* 输出栅栏 ID */
    __u64 cmds_ptr;     /* 命令数组指针 */
    __u64 fence_in;     /* 输入栅栏 ID */
};

/**
 * struct drm_fdca_wait - 等待操作
 */
struct drm_fdca_wait {
    __u32 ctx_id;       /* 上下文 ID */
    __u32 fence_id;     /* 栅栏 ID */
    __u64 timeout_ns;   /* 超时时间（纳秒） */
    __u32 result;       /* 等待结果 */
    __u32 pad;
};

/**
 * struct drm_fdca_memory_stats - 内存统计信息
 */
struct drm_fdca_memory_stats {
    __u64 vram_total;       /* VRAM 总大小 */
    __u64 vram_used;        /* VRAM 已使用 */
    __u64 vram_available;   /* VRAM 可用 */
    __u32 vram_fragmentation; /* VRAM 碎片率 */
    __u32 pad;
    __u64 gtt_total;        /* GTT 总大小 */
    __u64 gtt_used;         /* GTT 已使用 */
    __u64 gtt_available;    /* GTT 可用 */
};

/**
 * struct drm_fdca_performance_info - 性能信息
 */
struct drm_fdca_performance_info {
    __u64 cau_utilization;  /* CAU 利用率 */
    __u64 cfu_utilization;  /* CFU 利用率 */
    __u64 noc_bandwidth;    /* NoC 带宽利用率 */
    __u64 avg_latency;      /* 平均延迟 */
    __u64 peak_bandwidth;   /* 峰值带宽 */
    __u64 total_operations; /* 总操作数 */
};

/*
 * ============================================================================
 * IOCTL 定义
 * ============================================================================
 */

#define DRM_FDCA_GET_PARAM          0x00
#define DRM_FDCA_GEM_CREATE         0x01
#define DRM_FDCA_GEM_MMAP           0x02
#define DRM_FDCA_GET_RVV_CONFIG     0x03
#define DRM_FDCA_CONTEXT_CREATE     0x04
#define DRM_FDCA_CONTEXT_DESTROY    0x05
#define DRM_FDCA_SUBMIT             0x06
#define DRM_FDCA_WAIT               0x07
#define DRM_FDCA_GET_MEMORY_STATS   0x08
#define DRM_FDCA_GET_PERF_INFO      0x09

#define DRM_IOCTL_FDCA_GET_PARAM    DRM_IOWR(DRM_COMMAND_BASE + DRM_FDCA_GET_PARAM, struct drm_fdca_get_param)
#define DRM_IOCTL_FDCA_GEM_CREATE   DRM_IOWR(DRM_COMMAND_BASE + DRM_FDCA_GEM_CREATE, struct drm_fdca_gem_create)
#define DRM_IOCTL_FDCA_GEM_MMAP     DRM_IOWR(DRM_COMMAND_BASE + DRM_FDCA_GEM_MMAP, struct drm_fdca_gem_mmap)
#define DRM_IOCTL_FDCA_GET_RVV_CONFIG DRM_IOR(DRM_COMMAND_BASE + DRM_FDCA_GET_RVV_CONFIG, struct drm_fdca_rvv_config)
#define DRM_IOCTL_FDCA_CONTEXT_CREATE DRM_IOWR(DRM_COMMAND_BASE + DRM_FDCA_CONTEXT_CREATE, struct drm_fdca_context_create)
#define DRM_IOCTL_FDCA_CONTEXT_DESTROY DRM_IOW(DRM_COMMAND_BASE + DRM_FDCA_CONTEXT_DESTROY, struct drm_fdca_context_destroy)
#define DRM_IOCTL_FDCA_SUBMIT       DRM_IOWR(DRM_COMMAND_BASE + DRM_FDCA_SUBMIT, struct drm_fdca_submit)
#define DRM_IOCTL_FDCA_WAIT         DRM_IOWR(DRM_COMMAND_BASE + DRM_FDCA_WAIT, struct drm_fdca_wait)
#define DRM_IOCTL_FDCA_GET_MEMORY_STATS DRM_IOR(DRM_COMMAND_BASE + DRM_FDCA_GET_MEMORY_STATS, struct drm_fdca_memory_stats)
#define DRM_IOCTL_FDCA_GET_PERF_INFO DRM_IOR(DRM_COMMAND_BASE + DRM_FDCA_GET_PERF_INFO, struct drm_fdca_performance_info)

#endif /* __FDCA_UAPI_H__ */
