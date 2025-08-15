/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * FDCA (Fangzheng Distributed Computing Architecture) Kernel Mode Driver
 * 
 * Copyright (C) 2024 Fangzheng Technology Co., Ltd.
 * 
 * 昉擎分布式计算架构内核模式驱动 - 核心数据结构定义
 * 
 * 本文件是整个 FDCA KMD 的基石，定义了所有核心数据结构和常量。
 * 采用分布式异构计算架构，支持上下文相关单元（CAU）和上下文无关单元（CFU）
 * 的协同工作，并充分利用 RISC-V 向量扩展（RVV）的强大能力。
 *
 * Author: FDCA Kernel Team
 * Date: 2024
 */

#ifndef __FDCA_DRV_H__
#define __FDCA_DRV_H__

#include <linux/pci.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/idr.h>
#include <linux/kref.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>

#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_mm.h>
#include <drm/drm_buddy.h>
#include <drm/drm_gem.h>

/* 前向声明 - 避免循环依赖 */
struct fdca_device;
struct fdca_context;
struct fdca_queue;
struct fdca_scheduler;
struct fdca_memory_manager;
struct fdca_rvv_state;
struct fdca_sync_object;
struct fdca_vram_object;
struct fdca_vram_stats;
struct fdca_gtt_entry;
struct fdca_gtt_stats;
struct fdca_gem_object;
struct fdca_memory_total_stats;

/*
 * ============================================================================
 * 基础常量定义
 * ============================================================================
 */

/* 驱动版本信息 */
#define FDCA_DRIVER_NAME        "fdca"
#define FDCA_DRIVER_DESC        "Fangzheng Distributed Computing Architecture Driver"
#define FDCA_DRIVER_VERSION     "1.0.0"
#define FDCA_DRIVER_DATE        "2024"

/* 硬件架构常量 */
#define FDCA_MAX_LANES          16      /* 最大向量Lane数量，参考ara项目 */
#define FDCA_MAX_QUEUES         64      /* 每个单元最大队列数 */
#define FDCA_MAX_CONTEXTS       1024    /* 最大上下文数量 */
#define FDCA_MAX_SYNC_OBJECTS   4096    /* 最大同步对象数量 */

/* RISC-V 向量扩展常量 */
#define FDCA_RVV_MAX_VLEN       65536   /* 最大向量长度(bits) - RVV标准 */
#define FDCA_RVV_MAX_ELEN       64      /* 最大元素长度(bits) - 当前支持到64位 */
#define FDCA_RVV_NUM_VREGS      32      /* 向量寄存器数量 - RVV标准 */
#define FDCA_RVV_VMASK_REG      0       /* 掩码寄存器v0 */

/* 内存管理常量 */
#define FDCA_VRAM_SIZE_MAX      (16ULL << 30)    /* 最大16GB设备内存 */
#define FDCA_GTT_SIZE_MAX       (256ULL << 30)   /* 最大256GB虚拟地址空间 */
#define FDCA_PAGE_SIZE          4096              /* 基础页大小 */
#define FDCA_LARGE_PAGE_SIZE    (2 << 20)        /* 大页大小(2MB) */

/*
 * ============================================================================
 * 计算单元类型枚举
 * ============================================================================
 */

/**
 * enum fdca_unit_type - FDCA计算单元类型
 * 
 * 分布式架构的核心概念：两种不同特性的计算单元
 */
enum fdca_unit_type {
    FDCA_UNIT_CAU = 0,      /* Context-Aware Unit: 上下文相关单元(访存优化) */
    FDCA_UNIT_CFU = 1,      /* Context-Free Unit: 上下文无关单元(计算优化) */
    FDCA_UNIT_MAX
};

/**
 * enum fdca_queue_type - 队列类型
 * 
 * 不同队列类型针对不同的计算模式进行优化
 */
enum fdca_queue_type {
    FDCA_QUEUE_CAU_MEM = 0,     /* CAU内存访问队列 */
    FDCA_QUEUE_CAU_COMPUTE = 1, /* CAU计算队列 */
    FDCA_QUEUE_CFU_VECTOR = 2,  /* CFU向量计算队列 */
    FDCA_QUEUE_CFU_SCALAR = 3,  /* CFU标量计算队列 */
    FDCA_QUEUE_MAX
};

/**
 * enum fdca_sync_type - 同步对象类型
 */
enum fdca_sync_type {
    FDCA_SYNC_FENCE = 0,        /* 传统fence同步 */
    FDCA_SYNC_TIMELINE = 1,     /* 时间线同步 */
    FDCA_SYNC_CROSS_UNIT = 2,   /* 跨单元同步 */
    FDCA_SYNC_MAX
};

/*
 * ============================================================================
 * RISC-V 向量扩展相关结构
 * ============================================================================
 */

/**
 * struct fdca_rvv_config - RVV配置参数
 * 
 * 管理RISC-V向量扩展的关键配置参数，参考ara项目的设计
 */
struct fdca_rvv_config {
    /* 基础配置 */
    u32 vlen;                   /* 向量长度(bits) */
    u32 elen;                   /* 元素最大长度(bits) */
    u32 num_lanes;              /* 并行Lane数量 */
    u32 vlenb;                  /* VLEN的字节数 */
    
    /* 硬件能力 */
    bool fp_support;            /* 浮点支持 */
    bool fixed_point_support;   /* 定点支持 */
    bool segment_support;       /* 分段内存操作支持 */
    bool os_support;            /* 操作系统支持(MMU) */
    
    /* 性能参数 */
    u32 multiplier_latency[4];  /* 乘法器延迟[EW8/16/32/64] */
    u32 fpu_latency[5];         /* FPU延迟[comp/div/conv/noncomp/dotp] */
    
    /* Lane配置 */
    u32 vrf_size_per_lane;      /* 每个Lane的VRF大小(bytes) */
    u32 vrf_banks_per_lane;     /* 每个Lane的VRF存储体数量 */
};

/**
 * struct fdca_rvv_csr_state - RVV控制状态寄存器
 * 
 * 管理所有向量扩展相关的CSR状态，支持上下文切换
 */
struct fdca_rvv_csr_state {
    /* 向量配置状态 */
    u64 vstart;                 /* 向量起始索引 */
    u64 vxsat;                  /* 固定点饱和标志 */
    u64 vxrm;                   /* 固定点舍入模式 */
    u64 vcsr;                   /* 向量控制状态 */
    u64 vl;                     /* 向量长度 */
    u64 vtype;                  /* 向量类型 */
    u64 vlenb;                  /* 向量长度字节数 */
    
    /* 状态管理 */
    bool valid;                 /* 状态是否有效 */
    bool dirty;                 /* 状态是否被修改 */
    u64 last_update_time;       /* 最后更新时间戳 */
};

/*
 * ============================================================================
 * 内存管理结构
 * ============================================================================
 */

/**
 * struct fdca_vram_stats - VRAM 统计信息
 */
struct fdca_vram_stats {
    u64 total_size;         /* 总大小 */
    u64 used_size;          /* 已使用大小 */
    u64 available_size;     /* 可用大小 */
    u32 fragmentation;      /* 碎片率(%) */
    u64 alloc_count;        /* 分配次数 */
    u64 free_count;         /* 释放次数 */
    u64 large_page_count;   /* 大页分配次数 */
};

/**
 * struct fdca_gtt_stats - GTT 统计信息
 */
struct fdca_gtt_stats {
    u64 total_size;         /* 总大小 */
    u64 used_size;          /* 已使用大小 */
    u64 available_size;     /* 可用大小 */
    u32 num_entries;        /* 页表项数量 */
    u64 map_count;          /* 映射次数 */
    u64 unmap_count;        /* 解映射次数 */
};

/**
 * struct fdca_memory_total_stats - 总体内存统计
 */
struct fdca_memory_total_stats {
    u64 vram_total;         /* VRAM 总大小 */
    u64 vram_used;          /* VRAM 已使用 */
    u64 vram_available;     /* VRAM 可用 */
    u32 vram_fragmentation; /* VRAM 碎片率 */
    u64 gtt_total;          /* GTT 总大小 */
    u64 gtt_used;           /* GTT 已使用 */
    u64 gtt_available;      /* GTT 可用 */
    u64 total_allocated;    /* 总分配量 */
    u64 peak_usage;         /* 峰值使用量 */
};

/**
 * struct fdca_vram_manager - VRAM内存管理器
 * 
 * 使用drm_buddy分配器管理设备本地内存
 */
struct fdca_vram_manager {
    struct drm_buddy buddy;     /* buddy分配器 */
    struct mutex lock;          /* 保护分配操作 */
    
    /* 内存区域信息 */
    resource_size_t base;       /* 物理基地址 */
    resource_size_t size;       /* 总大小 */
    resource_size_t available;  /* 可用大小 */
    resource_size_t used;       /* 已用大小 */
    
    /* 分配统计 */
    atomic64_t alloc_count;     /* 分配次数 */
    atomic64_t free_count;      /* 释放次数 */
    atomic64_t large_page_count;/* 大页分配次数 */
    
    /* 碎片管理 */
    struct work_struct defrag_work; /* 碎片整理工作 */
    bool defrag_in_progress;    /* 碎片整理进行中 */
};

/**
 * struct fdca_gtt_manager - GTT图形地址转换表管理器
 * 
 * 使用drm_mm管理虚拟地址空间
 */
struct fdca_gtt_manager {
    struct drm_mm mm;           /* 地址空间管理器 */
    struct mutex lock;          /* 保护映射操作 */
    
    /* 地址空间信息 */
    u64 base;                   /* 虚拟基地址 */
    u64 size;                   /* 地址空间大小 */
    u64 page_size;              /* 页大小 */
    
    /* 页表管理 */
    void __iomem *page_table;   /* 页表基址 */
    dma_addr_t page_table_dma;  /* 页表DMA地址 */
    u32 num_entries;            /* 页表项数量 */
    
    /* 映射统计 */
    atomic64_t map_count;       /* 映射次数 */
    atomic64_t unmap_count;     /* 解映射次数 */
};

/**
 * struct fdca_memory_manager - 统一内存管理器
 * 
 * 整合VRAM和GTT管理，提供统一的内存服务
 */
struct fdca_memory_manager {
    struct fdca_device *fdev;   /* 关联的设备 */
    
    /* 子管理器 */
    struct fdca_vram_manager vram;  /* VRAM管理器 */
    struct fdca_gtt_manager gtt;    /* GTT管理器 */
    
    /* 内存池 */
    struct gen_pool *small_pool;    /* 小块内存池 */
    struct gen_pool *large_pool;    /* 大块内存池 */
    
    /* 缓存管理 */
    struct list_head cached_objects;   /* 缓存对象列表 */
    struct work_struct cache_cleanup;   /* 缓存清理工作 */
    
    /* 统计信息 */
    atomic64_t total_allocated;    /* 总分配量 */
    atomic64_t peak_usage;         /* 峰值使用量 */
};

/*
 * ============================================================================
 * 队列和调度器结构
 * ============================================================================
 */

/**
 * struct fdca_queue_ops - 队列操作接口
 * 
 * 定义队列的通用操作，不同类型队列实现不同的优化策略
 */
struct fdca_queue_ops {
    int (*init)(struct fdca_queue *queue);
    void (*fini)(struct fdca_queue *queue);
    int (*submit)(struct fdca_queue *queue, void *cmd, size_t size);
    int (*wait_idle)(struct fdca_queue *queue, unsigned long timeout);
    void (*reset)(struct fdca_queue *queue);
    u64 (*get_timestamp)(struct fdca_queue *queue);
};

/**
 * struct fdca_queue - 通用队列结构
 * 
 * 所有队列类型的基础结构，支持不同的优化策略
 */
struct fdca_queue {
    /* 基础信息 */
    struct fdca_device *fdev;       /* 关联设备 */
    enum fdca_queue_type type;      /* 队列类型 */
    enum fdca_unit_type unit;       /* 所属计算单元 */
    u32 id;                         /* 队列ID */
    
    /* 操作接口 */
    const struct fdca_queue_ops *ops;   /* 操作函数 */
    
    /* 硬件接口 */
    void __iomem *mmio_base;        /* MMIO基址 */
    u32 mmio_size;                  /* MMIO大小 */
    int irq;                        /* 中断号 */
    
    /* 命令缓冲区 */
    void *cmd_buffer;               /* 命令缓冲区虚拟地址 */
    dma_addr_t cmd_buffer_dma;      /* 命令缓冲区DMA地址 */
    size_t cmd_buffer_size;         /* 缓冲区大小 */
    u32 head;                       /* 队列头指针 */
    u32 tail;                       /* 队列尾指针 */
    
    /* 同步和状态 */
    spinlock_t lock;                /* 队列锁 */
    wait_queue_head_t wait_queue;   /* 等待队列 */
    bool active;                    /* 队列是否活跃 */
    u64 last_activity;              /* 最后活动时间 */
    
    /* 性能统计 */
    atomic64_t submit_count;        /* 提交计数 */
    atomic64_t complete_count;      /* 完成计数 */
    u64 total_exec_time;            /* 总执行时间 */
    
    /* RVV相关 */
    struct fdca_rvv_state *rvv_state;   /* 当前RVV状态 */
    u32 vector_length;              /* 当前向量长度 */
    u32 element_width;              /* 当前元素宽度 */
};

/**
 * struct fdca_scheduler_ops - 调度器操作接口
 */
struct fdca_scheduler_ops {
    int (*init)(struct fdca_scheduler *sched);
    void (*fini)(struct fdca_scheduler *sched);
    int (*schedule)(struct fdca_scheduler *sched, struct fdca_queue *queue);
    void (*preempt)(struct fdca_scheduler *sched, struct fdca_queue *queue);
    void (*priority_update)(struct fdca_scheduler *sched, struct fdca_queue *queue, int priority);
};

/**
 * struct fdca_scheduler - 调度器基础结构
 * 
 * 管理队列调度和资源分配，支持不同的调度策略
 */
struct fdca_scheduler {
    /* 基础信息 */
    struct fdca_device *fdev;       /* 关联设备 */
    enum fdca_unit_type unit;       /* 调度的计算单元 */
    const struct fdca_scheduler_ops *ops;   /* 操作接口 */
    
    /* 队列管理 */
    struct list_head active_queues; /* 活跃队列列表 */
    struct list_head pending_queues;/* 待调度队列列表 */
    spinlock_t queue_lock;          /* 队列列表锁 */
    
    /* 调度策略 */
    int default_priority;           /* 默认优先级 */
    u32 time_slice_us;              /* 时间片(微秒) */
    u32 preemption_threshold;       /* 抢占阈值 */
    
    /* 工作线程 */
    struct workqueue_struct *wq;    /* 工作队列 */
    struct work_struct schedule_work;   /* 调度工作 */
    bool schedule_pending;          /* 调度待处理 */
    
    /* 性能统计 */
    atomic64_t schedule_count;      /* 调度次数 */
    atomic64_t preemption_count;    /* 抢占次数 */
    u64 total_schedule_time;        /* 总调度时间 */
};

/*
 * ============================================================================
 * NoC (Network-on-Chip) 通信结构
 * ============================================================================
 */

/**
 * struct fdca_noc_node - NoC网络节点
 * 
 * 表示片上网络中的一个通信节点
 */
struct fdca_noc_node {
    u32 id;                         /* 节点ID */
    enum fdca_unit_type unit_type;  /* 关联的计算单元类型 */
    void __iomem *ctrl_regs;        /* 控制寄存器 */
    
    /* 路由信息 */
    u32 x_coord;                    /* X坐标 */
    u32 y_coord;                    /* Y坐标 */
    u32 routing_table[FDCA_UNIT_MAX]; /* 路由表 */
    
    /* 性能统计 */
    atomic64_t tx_packets;          /* 发送包数 */
    atomic64_t rx_packets;          /* 接收包数 */
    atomic64_t tx_bytes;            /* 发送字节数 */
    atomic64_t rx_bytes;            /* 接收字节数 */
    u64 max_latency;                /* 最大延迟 */
    u64 avg_latency;                /* 平均延迟 */
};

/**
 * struct fdca_noc_manager - NoC网络管理器
 * 
 * 管理整个片上网络的配置和监控
 */
struct fdca_noc_manager {
    struct fdca_device *fdev;       /* 关联设备 */
    
    /* 网络拓扑 */
    struct fdca_noc_node *nodes;    /* 节点数组 */
    u32 num_nodes;                  /* 节点数量 */
    u32 mesh_width;                 /* 网格宽度 */
    u32 mesh_height;                /* 网格高度 */
    
    /* 配置参数 */
    u32 packet_size;                /* 数据包大小 */
    u32 buffer_depth;               /* 缓冲区深度 */
    u32 clock_freq;                 /* 时钟频率 */
    
    /* 监控和统计 */
    struct mutex config_lock;       /* 配置锁 */
    bool monitoring_enabled;        /* 监控是否启用 */
    struct timer_list monitor_timer;/* 监控定时器 */
    
    /* 拥塞控制 */
    atomic_t congestion_level;      /* 拥塞级别 */
    u32 congestion_threshold;       /* 拥塞阈值 */
    struct work_struct congestion_work; /* 拥塞处理工作 */
};

/*
 * ============================================================================
 * 同步对象管理
 * ============================================================================
 */

/**
 * struct fdca_sync_object - 同步对象
 * 
 * 支持各种类型的同步操作，特别是跨计算单元的同步
 */
struct fdca_sync_object {
    struct kref ref;                /* 引用计数 */
    enum fdca_sync_type type;       /* 同步类型 */
    u32 id;                         /* 同步对象ID */
    
    /* 状态管理 */
    atomic_t signaled;              /* 信号状态 */
    u64 signal_value;               /* 信号值(timeline) */
    wait_queue_head_t wait_queue;   /* 等待队列 */
    
    /* 跨单元同步 */
    enum fdca_unit_type source_unit; /* 源计算单元 */
    enum fdca_unit_type target_unit; /* 目标计算单元 */
    u32 dependency_mask;            /* 依赖掩码 */
    
    /* 回调支持 */
    void (*callback)(struct fdca_sync_object *sync, void *data);
    void *callback_data;            /* 回调数据 */
    
    /* 调试信息 */
    u64 create_time;                /* 创建时间 */
    u64 signal_time;                /* 信号时间 */
    const char *debug_name;         /* 调试名称 */
};

/*
 * ============================================================================
 * 上下文管理
 * ============================================================================
 */

/**
 * struct fdca_context - 进程上下文
 * 
 * 管理单个进程在设备上的资源和状态
 */
struct fdca_context {
    struct kref ref;                /* 引用计数 */
    struct fdca_device *fdev;       /* 关联设备 */
    
    /* 进程信息 */
    struct pid *pid;                /* 进程PID */
    struct drm_file *file;          /* DRM文件 */
    u32 ctx_id;                     /* 上下文ID */
    
    /* 队列分配 */
    struct fdca_queue *queues[FDCA_QUEUE_MAX]; /* 队列数组 */
    struct mutex queue_lock;        /* 队列分配锁 */
    
    /* RVV状态 */
    struct fdca_rvv_csr_state rvv_state; /* RVV CSR状态 */
    bool rvv_enabled;               /* RVV是否启用 */
    u32 vector_context_id;          /* 向量上下文ID */
    
    /* 内存映射 */
    struct list_head vma_list;      /* VMA列表 */
    struct mutex vma_lock;          /* VMA锁 */
    
    /* 同步对象 */
    struct idr sync_idr;            /* 同步对象IDR */
    struct mutex sync_lock;         /* 同步对象锁 */
    
    /* 调试和统计 */
    atomic64_t submit_count;        /* 提交计数 */
    atomic64_t gpu_time_ns;         /* GPU时间(纳秒) */
    u64 create_time;                /* 创建时间 */
    u64 last_activity;              /* 最后活动时间 */
};

/*
 * ============================================================================
 * 主设备结构
 * ============================================================================
 */

/**
 * struct fdca_device - FDCA主设备结构
 * 
 * 这是整个驱动的核心结构，包含所有子系统的引用
 */
struct fdca_device {
    /* 基础设备信息 */
    struct drm_device drm;          /* DRM设备 */
    struct pci_dev *pdev;           /* PCI设备 */
    struct device *dev;             /* 通用设备 */
    struct list_head device_list;   /* 全局设备列表节点 */
    
    /* 硬件信息 */
    u32 device_id;                  /* 设备ID */
    u32 revision;                   /* 硬件版本 */
    char chip_name[32];             /* 芯片名称 */
    
    /* MMIO映射 */
    void __iomem *mmio_base;        /* MMIO基址 */
    resource_size_t mmio_size;      /* MMIO大小 */
    
    /* VRAM信息 */
    resource_size_t vram_base;      /* VRAM物理基址 */
    resource_size_t vram_size;      /* VRAM大小 */
    
    /* 计算单元 */
    struct {
        bool present;               /* 单元是否存在 */
        void __iomem *mmio_base;    /* 单元MMIO基址 */
        u32 mmio_size;              /* 单元MMIO大小 */
        int irq;                    /* 中断号 */
        u32 num_queues;             /* 队列数量 */
        u32 compute_units;          /* 计算单元数量 */
    } units[FDCA_UNIT_MAX];
    
    /* RVV配置 */
    struct fdca_rvv_config rvv_config; /* RVV硬件配置 */
    bool rvv_available;             /* RVV是否可用 */
    
    /* 子系统管理器 */
    struct fdca_memory_manager *mem_mgr;    /* 内存管理器 */
    struct fdca_scheduler *schedulers[FDCA_UNIT_MAX]; /* 调度器数组 */
    struct fdca_noc_manager *noc_mgr;       /* NoC管理器 */
    
    /* 上下文管理 */
    struct idr ctx_idr;             /* 上下文IDR */
    struct mutex ctx_lock;          /* 上下文锁 */
    atomic_t ctx_count;             /* 活跃上下文数量 */
    
    /* 电源管理 */
    struct {
        atomic_t usage_count;       /* 使用计数 */
        bool runtime_suspended;     /* 运行时挂起状态 */
        struct mutex lock;          /* 电源锁 */
        struct work_struct suspend_work; /* 挂起工作 */
    } pm;
    
    /* 固件管理 */
    struct {
        const struct firmware *fw;  /* 固件数据 */
        bool loaded;                /* 固件是否已加载 */
        u32 version;                /* 固件版本 */
        char version_string[64];    /* 固件版本字符串 */
    } firmware;
    
    /* 调试和监控 */
    struct {
        struct dentry *root;        /* debugfs根目录 */
        bool enabled;               /* 调试是否启用 */
        u32 debug_level;            /* 调试级别 */
    } debug;
    
    /* 性能统计 */
    struct {
        atomic64_t total_commands;  /* 总命令数 */
        atomic64_t total_interrupts;/* 总中断数 */
        u64 uptime_start;           /* 启动时间 */
        u64 total_compute_time;     /* 总计算时间 */
    } stats;
    
    /* 错误恢复 */
    struct {
        struct work_struct reset_work;  /* 重置工作 */
        atomic_t reset_count;       /* 重置计数 */
        bool recovery_active;       /* 恢复是否进行中 */
        struct mutex recovery_lock; /* 恢复锁 */
    } recovery;
    
    /* 设备状态 */
    enum {
        FDCA_DEV_STATE_INIT = 0,    /* 初始化中 */
        FDCA_DEV_STATE_ACTIVE,      /* 活跃状态 */
        FDCA_DEV_STATE_SUSPENDED,   /* 挂起状态 */
        FDCA_DEV_STATE_ERROR,       /* 错误状态 */
        FDCA_DEV_STATE_RESETTING,   /* 重置中 */
    } state;
    
    /* 全局锁 */
    struct mutex device_lock;       /* 设备级锁 */
    spinlock_t irq_lock;           /* 中断锁 */
};

/*
 * ============================================================================
 * 辅助宏定义
 * ============================================================================
 */

/* 从DRM设备获取FDCA设备 */
#define drm_to_fdca(d) container_of(d, struct fdca_device, drm)

/* 从PCI设备获取FDCA设备 */
#define pci_to_fdca(p) pci_get_drvdata(p)

/* 日志宏 */
#define fdca_err(fdev, fmt, args...) \
    dev_err(fdev->dev, "[FDCA] " fmt, ##args)

#define fdca_warn(fdev, fmt, args...) \
    dev_warn(fdev->dev, "[FDCA] " fmt, ##args)

#define fdca_info(fdev, fmt, args...) \
    dev_info(fdev->dev, "[FDCA] " fmt, ##args)

#define fdca_dbg(fdev, fmt, args...) \
    dev_dbg(fdev->dev, "[FDCA] " fmt, ##args)

/* 性能统计宏 */
#define fdca_stats_inc(fdev, field) \
    atomic64_inc(&(fdev)->stats.field)

#define fdca_stats_add(fdev, field, val) \
    atomic64_add(val, &(fdev)->stats.field)

/* 错误处理宏 */
#define fdca_check_ret(expr) \
    do { \
        int __ret = (expr); \
        if (__ret) \
            return __ret; \
    } while (0)

/*
 * ============================================================================
 * 外部函数声明
 * ============================================================================
 */

/* 核心初始化函数 */
int fdca_device_init(struct fdca_device *fdev);
void fdca_device_fini(struct fdca_device *fdev);

/* 子系统初始化函数 - 这些将在后续模块中实现 */
int fdca_memory_manager_init(struct fdca_device *fdev);
void fdca_memory_manager_fini(struct fdca_device *fdev);

/* VRAM 管理函数 */
int fdca_vram_manager_init(struct fdca_device *fdev);
void fdca_vram_manager_fini(struct fdca_device *fdev);
struct fdca_vram_object *fdca_vram_alloc(struct fdca_device *fdev,
                                         size_t size, u32 flags,
                                         const char *debug_name);
void fdca_vram_free(struct fdca_device *fdev, struct fdca_vram_object *obj);
int fdca_vram_map(struct fdca_device *fdev, struct fdca_vram_object *obj);
void fdca_vram_unmap(struct fdca_device *fdev, struct fdca_vram_object *obj);
void fdca_vram_get_stats(struct fdca_device *fdev, struct fdca_vram_stats *stats);
void fdca_vram_print_stats(struct fdca_device *fdev);

/* GTT 管理函数 */
int fdca_gtt_manager_init(struct fdca_device *fdev);
void fdca_gtt_manager_fini(struct fdca_device *fdev);
struct fdca_gtt_entry *fdca_gtt_map_pages(struct fdca_device *fdev,
                                          struct page **pages, u32 num_pages,
                                          enum dma_data_direction direction,
                                          const char *debug_name);
void fdca_gtt_unmap_pages(struct fdca_device *fdev, struct fdca_gtt_entry *entry,
                         enum dma_data_direction direction);
void fdca_gtt_get_stats(struct fdca_device *fdev, struct fdca_gtt_stats *stats);
void fdca_gtt_print_stats(struct fdca_device *fdev);

/* 统一内存管理函数 */
struct fdca_gem_object *fdca_gem_object_create(struct fdca_device *fdev,
                                               size_t size, u32 flags);
void fdca_memory_get_total_stats(struct fdca_device *fdev,
                                struct fdca_memory_total_stats *stats);
void fdca_memory_print_total_stats(struct fdca_device *fdev);

int fdca_scheduler_init(struct fdca_device *fdev);
void fdca_scheduler_fini(struct fdca_device *fdev);

int fdca_noc_manager_init(struct fdca_device *fdev);
void fdca_noc_manager_fini(struct fdca_device *fdev);

int fdca_rvv_state_init(struct fdca_device *fdev);
void fdca_rvv_state_fini(struct fdca_device *fdev);

/* RVV 状态管理函数 */
int fdca_rvv_state_manager_init(struct fdca_device *fdev);
void fdca_rvv_state_manager_fini(struct fdca_device *fdev);

/* PCI 子系统函数 */
int fdca_pci_init(void);
void fdca_pci_exit(void);

/* 设备管理函数 */
int fdca_add_device(struct fdca_device *fdev);
void fdca_remove_device(struct fdca_device *fdev);
struct fdca_device *fdca_find_device_by_id(u32 device_id);
int fdca_get_device_count(void);

/* 调试函数 */
void fdca_set_debug_level(unsigned int level);
unsigned int fdca_get_debug_level(void);
void fdca_dump_devices(void);

/* DRM 驱动结构 - 在 fdca_drm.c 中定义 */
extern const struct drm_driver fdca_drm_driver;

/* 工具函数 */
static inline bool fdca_device_is_active(struct fdca_device *fdev)
{
    return fdev->state == FDCA_DEV_STATE_ACTIVE;
}

static inline void fdca_device_set_state(struct fdca_device *fdev, int state)
{
    fdev->state = state;
}

#endif /* __FDCA_DRV_H__ */
