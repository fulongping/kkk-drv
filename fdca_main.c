// SPDX-License-Identifier: GPL-2.0-only
/*
 * FDCA (Fangzheng Distributed Computing Architecture) Main Driver
 * 
 * Copyright (C) 2024 Fangzheng Technology Co., Ltd.
 * 
 * 主驱动模块 - 模块加载、卸载和全局管理
 * 
 * 本模块负责：
 * 1. 内核模块的加载和卸载
 * 2. 全局资源的初始化和清理
 * 3. 模块参数的处理
 * 4. 驱动信息的导出
 * 5. 调试级别的控制
 *
 * Author: FDCA Kernel Team
 * Date: 2024
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/slab.h>

#include "fdca_drv.h"

/*
 * ============================================================================
 * 模块信息和参数
 * ============================================================================
 */

MODULE_AUTHOR("FDCA Kernel Team");
MODULE_DESCRIPTION(FDCA_DRIVER_DESC);
MODULE_VERSION(FDCA_DRIVER_VERSION);
MODULE_LICENSE("GPL v2");

/* 模块参数 */
static unsigned int debug_level = 0;
module_param(debug_level, uint, 0644);
MODULE_PARM_DESC(debug_level, "Debug level (0=off, 1=error, 2=warn, 3=info, 4=debug)");

static bool force_load = false;
module_param(force_load, bool, 0644);
MODULE_PARM_DESC(force_load, "Force module loading even if hardware is not detected");

static unsigned int max_devices = 8;
module_param(max_devices, uint, 0644);
MODULE_PARM_DESC(max_devices, "Maximum number of FDCA devices to support");

/*
 * ============================================================================
 * 全局变量
 * ============================================================================
 */

/* 全局设备列表 */
static LIST_HEAD(fdca_device_list);
static DEFINE_MUTEX(fdca_device_list_lock);
static atomic_t fdca_device_count = ATOMIC_INIT(0);

/* 驱动状态 */
static bool fdca_driver_loaded = false;

/*
 * ============================================================================
 * 设备管理函数
 * ============================================================================
 */

/**
 * fdca_add_device() - 添加设备到全局列表
 * @fdev: FDCA 设备
 * 
 * Return: 0 表示成功，负数表示错误
 */
int fdca_add_device(struct fdca_device *fdev)
{
    if (!fdev)
        return -EINVAL;
    
    mutex_lock(&fdca_device_list_lock);
    
    /* 检查设备数量限制 */
    if (atomic_read(&fdca_device_count) >= max_devices) {
        mutex_unlock(&fdca_device_list_lock);
        dev_err(fdev->dev, "已达到最大设备数量限制: %u\n", max_devices);
        return -ENOSPC;
    }
    
    /* 添加到列表 */
    list_add_tail(&fdev->device_list, &fdca_device_list);
    atomic_inc(&fdca_device_count);
    
    mutex_unlock(&fdca_device_list_lock);
    
    pr_info("FDCA 设备 %s 已添加 (总数: %d)\n", 
            fdev->chip_name, atomic_read(&fdca_device_count));
    
    return 0;
}

/**
 * fdca_remove_device() - 从全局列表移除设备
 * @fdev: FDCA 设备
 */
void fdca_remove_device(struct fdca_device *fdev)
{
    if (!fdev)
        return;
    
    mutex_lock(&fdca_device_list_lock);
    
    /* 从列表移除 */
    if (!list_empty(&fdev->device_list)) {
        list_del(&fdev->device_list);
        atomic_dec(&fdca_device_count);
    }
    
    mutex_unlock(&fdca_device_list_lock);
    
    pr_info("FDCA 设备 %s 已移除 (剩余: %d)\n", 
            fdev->chip_name, atomic_read(&fdca_device_count));
}

/**
 * fdca_find_device_by_id() - 根据设备 ID 查找设备
 * @device_id: 设备 ID
 * 
 * Return: 设备指针或 NULL
 */
struct fdca_device *fdca_find_device_by_id(u32 device_id)
{
    struct fdca_device *fdev, *found = NULL;
    
    mutex_lock(&fdca_device_list_lock);
    
    list_for_each_entry(fdev, &fdca_device_list, device_list) {
        if (fdev->device_id == device_id) {
            found = fdev;
            break;
        }
    }
    
    mutex_unlock(&fdca_device_list_lock);
    
    return found;
}

/**
 * fdca_get_device_count() - 获取设备数量
 * 
 * Return: 当前设备数量
 */
int fdca_get_device_count(void)
{
    return atomic_read(&fdca_device_count);
}

/*
 * ============================================================================
 * 调试和工具函数
 * ============================================================================
 */

/**
 * fdca_set_debug_level() - 设置调试级别
 * @level: 调试级别
 */
void fdca_set_debug_level(unsigned int level)
{
    debug_level = level;
    pr_info("FDCA 调试级别设置为: %u\n", level);
}

/**
 * fdca_get_debug_level() - 获取调试级别
 * 
 * Return: 当前调试级别
 */
unsigned int fdca_get_debug_level(void)
{
    return debug_level;
}

/**
 * fdca_dump_devices() - 打印所有设备信息
 */
void fdca_dump_devices(void)
{
    struct fdca_device *fdev;
    int count = 0;
    
    pr_info("=== FDCA 设备列表 ===\n");
    
    mutex_lock(&fdca_device_list_lock);
    
    list_for_each_entry(fdev, &fdca_device_list, device_list) {
        pr_info("设备 %d: %s (ID=0x%08x, 版本=0x%08x)\n",
                count++, fdev->chip_name, fdev->device_id, fdev->revision);
        
        if (fdev->rvv_available) {
            pr_info("  RVV: VLEN=%u, ELEN=%u, Lanes=%u\n",
                    fdev->rvv_config.vlen,
                    fdev->rvv_config.elen,
                    fdev->rvv_config.num_lanes);
        }
        
        pr_info("  计算单元: CAU=%s, CFU=%s\n",
                fdev->units[FDCA_UNIT_CAU].present ? "是" : "否",
                fdev->units[FDCA_UNIT_CFU].present ? "是" : "否");
        
        pr_info("  状态: %s, 上下文数: %d\n",
                fdca_device_is_active(fdev) ? "活跃" : "非活跃",
                atomic_read(&fdev->ctx_count));
    }
    
    mutex_unlock(&fdca_device_list_lock);
    
    pr_info("总设备数: %d\n", count);
}

/*
 * ============================================================================
 * 模块初始化和清理
 * ============================================================================
 */

/**
 * fdca_driver_init() - 驱动初始化
 * 
 * Return: 0 表示成功，负数表示错误
 */
static int __init fdca_driver_init(void)
{
    int ret;
    
    pr_info("FDCA 驱动开始加载 v%s\n", FDCA_DRIVER_VERSION);
    pr_info("调试级别: %u, 最大设备数: %u\n", debug_level, max_devices);
    
    /* 验证参数 */
    if (max_devices == 0 || max_devices > 64) {
        pr_err("无效的最大设备数: %u (有效范围: 1-64)\n", max_devices);
        return -EINVAL;
    }
    
    /* 初始化 PCI 驱动 */
    ret = fdca_pci_init();
    if (ret) {
        pr_err("PCI 驱动初始化失败: %d\n", ret);
        return ret;
    }
    
    /* 标记驱动已加载 */
    fdca_driver_loaded = true;
    
    pr_info("FDCA 驱动加载完成\n");
    
    /* 如果强制加载但没有设备，发出警告 */
    if (force_load && atomic_read(&fdca_device_count) == 0) {
        pr_warn("强制加载模式，但未检测到任何设备\n");
    }
    
    return 0;
}

/**
 * fdca_driver_exit() - 驱动清理
 */
static void __exit fdca_driver_exit(void)
{
    pr_info("FDCA 驱动开始卸载\n");
    
    /* 标记驱动未加载 */
    fdca_driver_loaded = false;
    
    /* 清理 PCI 驱动 */
    fdca_pci_exit();
    
    /* 验证所有设备已清理 */
    if (atomic_read(&fdca_device_count) > 0) {
        pr_warn("驱动卸载时仍有 %d 个设备未清理\n",
                atomic_read(&fdca_device_count));
        fdca_dump_devices();
    }
    
    pr_info("FDCA 驱动卸载完成\n");
}

/*
 * ============================================================================
 * 导出函数供其他模块使用
 * ============================================================================
 */

/* 这些函数需要在 fdca_drv.h 中有对应声明 */
EXPORT_SYMBOL_GPL(fdca_add_device);
EXPORT_SYMBOL_GPL(fdca_remove_device);
EXPORT_SYMBOL_GPL(fdca_find_device_by_id);
EXPORT_SYMBOL_GPL(fdca_get_device_count);
EXPORT_SYMBOL_GPL(fdca_set_debug_level);
EXPORT_SYMBOL_GPL(fdca_get_debug_level);
EXPORT_SYMBOL_GPL(fdca_dump_devices);

/* PCI 子模块函数 */
extern int fdca_pci_init(void);
extern void fdca_pci_exit(void);

/*
 * ============================================================================
 * 模块入口点
 * ============================================================================
 */

module_init(fdca_driver_init);
module_exit(fdca_driver_exit);
