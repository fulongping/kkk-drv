// SPDX-License-Identifier: GPL-2.0-only
/*
 * FDCA Vector Memory Operations Support
 * 
 * 支持 unit-strided, strided, indexed 和 segment 内存操作
 * 实现高效的向量数据传输
 */

#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include "fdca_drv.h"
#include "fdca_rvv_instr.h"

/* 向量内存操作描述符 */
struct fdca_vector_mem_op {
    enum fdca_vmem_type type;
    u64 base_addr;                /* 基地址 */
    u32 stride;                   /* 步长 */
    u32 *indices;                 /* 索引数组 (indexed 模式) */
    u32 num_elements;             /* 元素数量 */
    u32 element_size;             /* 元素大小 */
    bool is_load;                 /* 是否为加载操作 */
    
    /* DMA 相关 */
    dma_addr_t dma_addr;
    void *cpu_addr;
    size_t total_size;
};

/**
 * fdca_vector_mem_prepare_dma() - 准备 DMA 传输
 */
static int fdca_vector_mem_prepare_dma(struct fdca_device *fdev,
                                      struct fdca_vector_mem_op *op)
{
    /* 计算总传输大小 */
    switch (op->type) {
    case FDCA_VMEM_UNIT_STRIDE:
        op->total_size = op->num_elements * op->element_size;
        break;
        
    case FDCA_VMEM_STRIDED:
        op->total_size = op->num_elements * op->stride;
        break;
        
    case FDCA_VMEM_INDEXED:
        /* 最大可能的大小 */
        op->total_size = op->num_elements * op->element_size;
        break;
        
    case FDCA_VMEM_SEGMENT:
        /* 分段操作的总大小 */
        op->total_size = op->num_elements * op->element_size;
        break;
        
    default:
        return -EINVAL;
    }
    
    /* 分配 DMA 一致性内存 */
    op->cpu_addr = dma_alloc_coherent(fdev->dev, op->total_size,
                                     &op->dma_addr, GFP_KERNEL);
    if (!op->cpu_addr) {
        fdca_err(fdev, "DMA 内存分配失败: %zu 字节\n", op->total_size);
        return -ENOMEM;
    }
    
    return 0;
}

/**
 * fdca_vector_mem_cleanup_dma() - 清理 DMA 资源
 */
static void fdca_vector_mem_cleanup_dma(struct fdca_device *fdev,
                                       struct fdca_vector_mem_op *op)
{
    if (op->cpu_addr) {
        dma_free_coherent(fdev->dev, op->total_size,
                         op->cpu_addr, op->dma_addr);
        op->cpu_addr = NULL;
    }
}

/**
 * fdca_vector_mem_unit_stride() - 执行 unit-strided 内存操作
 */
static int fdca_vector_mem_unit_stride(struct fdca_device *fdev,
                                      struct fdca_vector_mem_op *op)
{
    void __iomem *reg_base = fdev->units[FDCA_UNIT_VPU].mmio_base;
    u32 ctrl_reg;
    
    /* 配置 unit-strided 操作 */
    iowrite64(op->base_addr, reg_base + 0x100);          /* 基地址 */
    iowrite32(op->num_elements, reg_base + 0x108);       /* 元素数量 */
    iowrite32(op->element_size, reg_base + 0x10C);       /* 元素大小 */
    iowrite64(op->dma_addr, reg_base + 0x110);           /* DMA 地址 */
    
    /* 设置控制寄存器 */
    ctrl_reg = (op->is_load ? BIT(0) : 0) |               /* 加载/存储 */
               (FDCA_VMEM_UNIT_STRIDE << 4);              /* 操作类型 */
    iowrite32(ctrl_reg, reg_base + 0x118);
    
    /* 启动操作 */
    iowrite32(BIT(0), reg_base + 0x11C);                 /* 启动位 */
    
    fdca_dbg(fdev, "Unit-stride 操作: 基地址=0x%llx, 元素=%u, 大小=%u\n",
             op->base_addr, op->num_elements, op->element_size);
    
    return 0;
}

/**
 * fdca_vector_mem_strided() - 执行 strided 内存操作
 */
static int fdca_vector_mem_strided(struct fdca_device *fdev,
                                  struct fdca_vector_mem_op *op)
{
    void __iomem *reg_base = fdev->units[FDCA_UNIT_VPU].mmio_base;
    u32 ctrl_reg;
    
    /* 配置 strided 操作 */
    iowrite64(op->base_addr, reg_base + 0x120);          /* 基地址 */
    iowrite32(op->stride, reg_base + 0x128);             /* 步长 */
    iowrite32(op->num_elements, reg_base + 0x12C);       /* 元素数量 */
    iowrite32(op->element_size, reg_base + 0x130);       /* 元素大小 */
    iowrite64(op->dma_addr, reg_base + 0x134);           /* DMA 地址 */
    
    /* 设置控制寄存器 */
    ctrl_reg = (op->is_load ? BIT(0) : 0) |               /* 加载/存储 */
               (FDCA_VMEM_STRIDED << 4);                  /* 操作类型 */
    iowrite32(ctrl_reg, reg_base + 0x138);
    
    /* 启动操作 */
    iowrite32(BIT(0), reg_base + 0x13C);                 /* 启动位 */
    
    fdca_dbg(fdev, "Strided 操作: 基地址=0x%llx, 步长=%u, 元素=%u\n",
             op->base_addr, op->stride, op->num_elements);
    
    return 0;
}

/**
 * fdca_vector_mem_indexed() - 执行 indexed 内存操作
 */
static int fdca_vector_mem_indexed(struct fdca_device *fdev,
                                  struct fdca_vector_mem_op *op)
{
    void __iomem *reg_base = fdev->units[FDCA_UNIT_VPU].mmio_base;
    dma_addr_t indices_dma;
    void *indices_cpu;
    size_t indices_size;
    u32 ctrl_reg;
    int ret = 0;
    
    /* 准备索引数组的 DMA 传输 */
    indices_size = op->num_elements * sizeof(u32);
    indices_cpu = dma_alloc_coherent(fdev->dev, indices_size,
                                    &indices_dma, GFP_KERNEL);
    if (!indices_cpu) {
        fdca_err(fdev, "索引数组 DMA 分配失败\n");
        return -ENOMEM;
    }
    
    /* 复制索引数据 */
    memcpy(indices_cpu, op->indices, indices_size);
    
    /* 配置 indexed 操作 */
    iowrite64(op->base_addr, reg_base + 0x140);          /* 基地址 */
    iowrite64(indices_dma, reg_base + 0x148);            /* 索引数组地址 */
    iowrite32(op->num_elements, reg_base + 0x150);       /* 元素数量 */
    iowrite32(op->element_size, reg_base + 0x154);       /* 元素大小 */
    iowrite64(op->dma_addr, reg_base + 0x158);           /* DMA 地址 */
    
    /* 设置控制寄存器 */
    ctrl_reg = (op->is_load ? BIT(0) : 0) |               /* 加载/存储 */
               (FDCA_VMEM_INDEXED << 4);                  /* 操作类型 */
    iowrite32(ctrl_reg, reg_base + 0x160);
    
    /* 启动操作 */
    iowrite32(BIT(0), reg_base + 0x164);                 /* 启动位 */
    
    fdca_dbg(fdev, "Indexed 操作: 基地址=0x%llx, 元素=%u\n",
             op->base_addr, op->num_elements);
    
    /* 清理索引数组 */
    dma_free_coherent(fdev->dev, indices_size, indices_cpu, indices_dma);
    
    return ret;
}

/**
 * fdca_vector_mem_segment() - 执行 segment 内存操作
 */
static int fdca_vector_mem_segment(struct fdca_device *fdev,
                                  struct fdca_vector_mem_op *op)
{
    void __iomem *reg_base = fdev->units[FDCA_UNIT_VPU].mmio_base;
    u32 ctrl_reg;
    u32 num_fields = op->stride;  /* segment 操作中 stride 表示字段数 */
    
    /* 配置 segment 操作 */
    iowrite64(op->base_addr, reg_base + 0x180);          /* 基地址 */
    iowrite32(num_fields, reg_base + 0x188);             /* 字段数量 */
    iowrite32(op->num_elements, reg_base + 0x18C);       /* 元素数量 */
    iowrite32(op->element_size, reg_base + 0x190);       /* 元素大小 */
    iowrite64(op->dma_addr, reg_base + 0x194);           /* DMA 地址 */
    
    /* 设置控制寄存器 */
    ctrl_reg = (op->is_load ? BIT(0) : 0) |               /* 加载/存储 */
               (FDCA_VMEM_SEGMENT << 4);                  /* 操作类型 */
    iowrite32(ctrl_reg, reg_base + 0x198);
    
    /* 启动操作 */
    iowrite32(BIT(0), reg_base + 0x19C);                 /* 启动位 */
    
    fdca_dbg(fdev, "Segment 操作: 基地址=0x%llx, 字段=%u, 元素=%u\n",
             op->base_addr, num_fields, op->num_elements);
    
    return 0;
}

/**
 * fdca_vector_mem_wait_completion() - 等待内存操作完成
 */
static int fdca_vector_mem_wait_completion(struct fdca_device *fdev, 
                                          unsigned long timeout_ms)
{
    void __iomem *reg_base = fdev->units[FDCA_UNIT_VPU].mmio_base;
    unsigned long timeout = jiffies + msecs_to_jiffies(timeout_ms);
    u32 status;
    
    do {
        status = ioread32(reg_base + 0x1A0);  /* 状态寄存器 */
        
        if (status & BIT(0)) {  /* 完成标志 */
            /* 检查错误状态 */
            if (status & BIT(1)) {
                fdca_err(fdev, "向量内存操作错误: 状态=0x%x\n", status);
                return -EIO;
            }
            return 0;  /* 成功完成 */
        }
        
        if (time_after(jiffies, timeout)) {
            fdca_err(fdev, "向量内存操作超时\n");
            return -ETIMEDOUT;
        }
        
        cpu_relax();
        
    } while (1);
}

/**
 * fdca_vector_mem_execute() - 执行向量内存操作
 */
int fdca_vector_mem_execute(struct fdca_device *fdev,
                           struct fdca_vector_mem_op *op)
{
    int ret;
    
    if (!fdev || !op) {
        return -EINVAL;
    }
    
    /* 检查硬件是否可用 */
    if (!fdev->units[FDCA_UNIT_VPU].present) {
        fdca_err(fdev, "向量处理单元不可用\n");
        return -ENODEV;
    }
    
    /* 准备 DMA 传输 */
    ret = fdca_vector_mem_prepare_dma(fdev, op);
    if (ret) {
        return ret;
    }
    
    /* 根据类型执行相应的内存操作 */
    switch (op->type) {
    case FDCA_VMEM_UNIT_STRIDE:
        ret = fdca_vector_mem_unit_stride(fdev, op);
        break;
        
    case FDCA_VMEM_STRIDED:
        ret = fdca_vector_mem_strided(fdev, op);
        break;
        
    case FDCA_VMEM_INDEXED:
        ret = fdca_vector_mem_indexed(fdev, op);
        break;
        
    case FDCA_VMEM_SEGMENT:
        ret = fdca_vector_mem_segment(fdev, op);
        break;
        
    default:
        fdca_err(fdev, "不支持的向量内存操作类型: %d\n", op->type);
        ret = -EINVAL;
        goto cleanup;
    }
    
    if (ret) {
        goto cleanup;
    }
    
    /* 等待操作完成 */
    ret = fdca_vector_mem_wait_completion(fdev, 1000);  /* 1秒超时 */
    
cleanup:
    /* 清理 DMA 资源 */
    fdca_vector_mem_cleanup_dma(fdev, op);
    
    return ret;
}

/**
 * fdca_vector_mem_create_op() - 创建向量内存操作描述符
 */
struct fdca_vector_mem_op *fdca_vector_mem_create_op(enum fdca_vmem_type type,
                                                    u64 base_addr,
                                                    u32 num_elements,
                                                    u32 element_size,
                                                    bool is_load)
{
    struct fdca_vector_mem_op *op;
    
    op = kzalloc(sizeof(*op), GFP_KERNEL);
    if (!op) {
        return NULL;
    }
    
    op->type = type;
    op->base_addr = base_addr;
    op->num_elements = num_elements;
    op->element_size = element_size;
    op->is_load = is_load;
    
    return op;
}

/**
 * fdca_vector_mem_destroy_op() - 销毁向量内存操作描述符
 */
void fdca_vector_mem_destroy_op(struct fdca_vector_mem_op *op)
{
    if (op) {
        kfree(op->indices);
        kfree(op);
    }
}

/**
 * fdca_vector_mem_set_stride() - 设置步长（用于 strided 操作）
 */
void fdca_vector_mem_set_stride(struct fdca_vector_mem_op *op, u32 stride)
{
    if (op) {
        op->stride = stride;
    }
}

/**
 * fdca_vector_mem_set_indices() - 设置索引数组（用于 indexed 操作）
 */
int fdca_vector_mem_set_indices(struct fdca_vector_mem_op *op, 
                               const u32 *indices, u32 count)
{
    if (!op || !indices) {
        return -EINVAL;
    }
    
    /* 释放旧的索引数组 */
    kfree(op->indices);
    
    /* 分配新的索引数组 */
    op->indices = kmalloc(count * sizeof(u32), GFP_KERNEL);
    if (!op->indices) {
        return -ENOMEM;
    }
    
    /* 复制索引数据 */
    memcpy(op->indices, indices, count * sizeof(u32));
    op->num_elements = count;
    
    return 0;
}

EXPORT_SYMBOL_GPL(fdca_vector_mem_execute);
EXPORT_SYMBOL_GPL(fdca_vector_mem_create_op);
EXPORT_SYMBOL_GPL(fdca_vector_mem_destroy_op);
EXPORT_SYMBOL_GPL(fdca_vector_mem_set_stride);
EXPORT_SYMBOL_GPL(fdca_vector_mem_set_indices);
