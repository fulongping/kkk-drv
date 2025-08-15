// SPDX-License-Identifier: GPL-2.0-only
/*
 * FDCA Debug and Diagnostics Interface
 */

#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include "fdca_drv.h"

static struct dentry *fdca_debugfs_root = NULL;

/* 设备状态显示 */
static int fdca_debugfs_device_show(struct seq_file *m, void *data)
{
    struct fdca_device *fdev = m->private;
    
    seq_printf(m, "=== FDCA 设备状态 ===\n");
    seq_printf(m, "设备 ID: 0x%x\n", fdev->device_id);
    seq_printf(m, "版本: 0x%x\n", fdev->revision);
    seq_printf(m, "芯片名称: %s\n", fdev->chip_name);
    seq_printf(m, "状态: %s\n", 
               fdev->state == FDCA_DEV_STATE_ACTIVE ? "活跃" : "非活跃");
    
    seq_printf(m, "\n=== 计算单元状态 ===\n");
    seq_printf(m, "CAU: %s\n", fdev->units[FDCA_UNIT_CAU].present ? "存在" : "不存在");
    seq_printf(m, "CFU: %s\n", fdev->units[FDCA_UNIT_CFU].present ? "存在" : "不存在");
    seq_printf(m, "VPU: %s\n", fdev->units[FDCA_UNIT_VPU].present ? "存在" : "不存在");
    seq_printf(m, "NoC: %s\n", fdev->units[FDCA_UNIT_NOC].present ? "存在" : "不存在");
    
    seq_printf(m, "\n=== RVV 配置 ===\n");
    seq_printf(m, "VLEN: %u bits\n", fdev->rvv_config.vlen);
    seq_printf(m, "ELEN: %u bits\n", fdev->rvv_config.elen);
    seq_printf(m, "Lanes: %u\n", fdev->rvv_config.num_lanes);
    seq_printf(m, "VLENB: %u bytes\n", fdev->rvv_config.vlenb);
    
    return 0;
}

static int fdca_debugfs_device_open(struct inode *inode, struct file *file)
{
    return single_open(file, fdca_debugfs_device_show, inode->i_private);
}

static const struct file_operations fdca_debugfs_device_fops = {
    .owner = THIS_MODULE,
    .open = fdca_debugfs_device_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

/* 内存统计显示 */
static int fdca_debugfs_memory_show(struct seq_file *m, void *data)
{
    struct fdca_device *fdev = m->private;
    struct fdca_memory_total_stats stats;
    
    if (fdev->mem_mgr) {
        fdca_memory_get_total_stats(fdev, &stats);
        
        seq_printf(m, "=== 内存统计 ===\n");
        seq_printf(m, "VRAM 总量: %llu MB\n", stats.vram_total >> 20);
        seq_printf(m, "VRAM 使用: %llu MB (%.1f%%)\n", 
                   stats.vram_used >> 20,
                   (float)stats.vram_used * 100.0 / stats.vram_total);
        seq_printf(m, "VRAM 碎片率: %u%%\n", stats.vram_fragmentation);
        
        seq_printf(m, "\nGTT 总量: %llu MB\n", stats.gtt_total >> 20);
        seq_printf(m, "GTT 使用: %llu MB (%.1f%%)\n",
                   stats.gtt_used >> 20,
                   (float)stats.gtt_used * 100.0 / stats.gtt_total);
        
        seq_printf(m, "\n总分配: %lld 字节\n", stats.total_allocated);
        seq_printf(m, "峰值使用: %lld 字节\n", stats.peak_usage);
    } else {
        seq_printf(m, "内存管理器未初始化\n");
    }
    
    return 0;
}

static int fdca_debugfs_memory_open(struct inode *inode, struct file *file)
{
    return single_open(file, fdca_debugfs_memory_show, inode->i_private);
}

static const struct file_operations fdca_debugfs_memory_fops = {
    .owner = THIS_MODULE,
    .open = fdca_debugfs_memory_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

/* 寄存器转储 */
static int fdca_debugfs_regs_show(struct seq_file *m, void *data)
{
    struct fdca_device *fdev = m->private;
    int i;
    
    seq_printf(m, "=== 主寄存器 ===\n");
    for (i = 0; i < 16; i++) {
        u32 val = ioread32(fdev->mmio_base + i * 4);
        seq_printf(m, "0x%03x: 0x%08x\n", i * 4, val);
    }
    
    if (fdev->units[FDCA_UNIT_CAU].present) {
        seq_printf(m, "\n=== CAU 寄存器 ===\n");
        for (i = 0; i < 8; i++) {
            u32 val = ioread32(fdev->units[FDCA_UNIT_CAU].mmio_base + i * 4);
            seq_printf(m, "0x%03x: 0x%08x\n", i * 4, val);
        }
    }
    
    if (fdev->units[FDCA_UNIT_CFU].present) {
        seq_printf(m, "\n=== CFU 寄存器 ===\n");
        for (i = 0; i < 8; i++) {
            u32 val = ioread32(fdev->units[FDCA_UNIT_CFU].mmio_base + i * 4);
            seq_printf(m, "0x%03x: 0x%08x\n", i * 4, val);
        }
    }
    
    return 0;
}

static int fdca_debugfs_regs_open(struct inode *inode, struct file *file)
{
    return single_open(file, fdca_debugfs_regs_show, inode->i_private);
}

static const struct file_operations fdca_debugfs_regs_fops = {
    .owner = THIS_MODULE,
    .open = fdca_debugfs_regs_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

/**
 * fdca_debugfs_init() - 初始化 debugfs 接口
 */
int fdca_debugfs_init(struct fdca_device *fdev)
{
    char name[32];
    struct dentry *device_dir;
    
    /* 创建根目录 */
    if (!fdca_debugfs_root) {
        fdca_debugfs_root = debugfs_create_dir("fdca", NULL);
        if (IS_ERR(fdca_debugfs_root)) {
            fdca_err(fdev, "无法创建 debugfs 根目录\n");
            return PTR_ERR(fdca_debugfs_root);
        }
    }
    
    /* 创建设备目录 */
    snprintf(name, sizeof(name), "card%d", fdev->drm.primary->index);
    device_dir = debugfs_create_dir(name, fdca_debugfs_root);
    if (IS_ERR(device_dir)) {
        fdca_err(fdev, "无法创建设备 debugfs 目录\n");
        return PTR_ERR(device_dir);
    }
    
    /* 创建调试文件 */
    debugfs_create_file("device", 0444, device_dir, fdev, &fdca_debugfs_device_fops);
    debugfs_create_file("memory", 0444, device_dir, fdev, &fdca_debugfs_memory_fops);
    debugfs_create_file("registers", 0444, device_dir, fdev, &fdca_debugfs_regs_fops);
    
    fdca_info(fdev, "debugfs 接口初始化完成: /sys/kernel/debug/fdca/%s\n", name);
    
    return 0;
}

/**
 * fdca_debugfs_fini() - 清理 debugfs 接口
 */
void fdca_debugfs_fini(void)
{
    debugfs_remove_recursive(fdca_debugfs_root);
    fdca_debugfs_root = NULL;
}

EXPORT_SYMBOL_GPL(fdca_debugfs_init);
EXPORT_SYMBOL_GPL(fdca_debugfs_fini);
