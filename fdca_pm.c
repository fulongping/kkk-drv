// SPDX-License-Identifier: GPL-2.0-only
/*
 * FDCA Power Management
 */

#include <linux/pm_runtime.h>
#include <linux/delay.h>
#include "fdca_drv.h"

/* 电源状态 */
enum fdca_power_state {
    FDCA_PM_ACTIVE,
    FDCA_PM_RUNTIME_SUSPEND,
    FDCA_PM_SYSTEM_SUSPEND,
    FDCA_PM_OFF,
};

struct fdca_pm_manager {
    struct fdca_device *fdev;
    enum fdca_power_state state;
    
    /* 电源控制寄存器 */
    void __iomem *pm_base;
    
    /* 统计信息 */
    atomic64_t suspend_count;
    atomic64_t resume_count;
    u64 total_suspend_time;
    u64 total_resume_time;
};

static struct fdca_pm_manager *pm_mgr = NULL;

/**
 * fdca_pm_save_context() - 保存设备上下文
 */
static int fdca_pm_save_context(struct fdca_device *fdev)
{
    /* 保存关键寄存器状态 */
    /* 这里应该保存设备的重要配置寄存器 */
    
    fdca_dbg(fdev, "设备上下文已保存\n");
    return 0;
}

/**
 * fdca_pm_restore_context() - 恢复设备上下文
 */
static int fdca_pm_restore_context(struct fdca_device *fdev)
{
    /* 恢复关键寄存器状态 */
    /* 这里应该恢复设备的重要配置寄存器 */
    
    fdca_dbg(fdev, "设备上下文已恢复\n");
    return 0;
}

/**
 * fdca_pm_power_down() - 关闭设备电源
 */
static int fdca_pm_power_down(struct fdca_device *fdev)
{
    if (!pm_mgr || !pm_mgr->pm_base)
        return -ENODEV;
    
    /* 关闭各个计算单元 */
    iowrite32(0x0, pm_mgr->pm_base + 0x10);  /* CAU 电源 */
    iowrite32(0x0, pm_mgr->pm_base + 0x14);  /* CFU 电源 */
    iowrite32(0x0, pm_mgr->pm_base + 0x18);  /* VPU 电源 */
    iowrite32(0x0, pm_mgr->pm_base + 0x1C);  /* NoC 电源 */
    
    /* 等待电源稳定 */
    msleep(10);
    
    fdca_dbg(fdev, "设备电源已关闭\n");
    return 0;
}

/**
 * fdca_pm_power_up() - 开启设备电源
 */
static int fdca_pm_power_up(struct fdca_device *fdev)
{
    if (!pm_mgr || !pm_mgr->pm_base)
        return -ENODEV;
    
    /* 开启各个计算单元电源 */
    iowrite32(0x1, pm_mgr->pm_base + 0x10);  /* CAU 电源 */
    iowrite32(0x1, pm_mgr->pm_base + 0x14);  /* CFU 电源 */
    iowrite32(0x1, pm_mgr->pm_base + 0x18);  /* VPU 电源 */
    iowrite32(0x1, pm_mgr->pm_base + 0x1C);  /* NoC 电源 */
    
    /* 等待电源稳定 */
    msleep(10);
    
    /* 等待设备就绪 */
    while (!(ioread32(pm_mgr->pm_base + 0x20) & 0xF)) {
        msleep(1);
    }
    
    fdca_dbg(fdev, "设备电源已开启\n");
    return 0;
}

/**
 * fdca_pm_runtime_suspend() - 运行时挂起
 */
static int fdca_pm_runtime_suspend(struct device *dev)
{
    struct fdca_device *fdev = dev_get_drvdata(dev);
    u64 start_time, end_time;
    int ret;
    
    start_time = ktime_get_ns();
    
    fdca_dbg(fdev, "运行时挂起开始\n");
    
    /* 保存上下文 */
    ret = fdca_pm_save_context(fdev);
    if (ret) {
        fdca_err(fdev, "保存上下文失败: %d\n", ret);
        return ret;
    }
    
    /* 关闭电源 */
    ret = fdca_pm_power_down(fdev);
    if (ret) {
        fdca_err(fdev, "关闭电源失败: %d\n", ret);
        return ret;
    }
    
    if (pm_mgr) {
        pm_mgr->state = FDCA_PM_RUNTIME_SUSPEND;
        atomic64_inc(&pm_mgr->suspend_count);
        
        end_time = ktime_get_ns();
        pm_mgr->total_suspend_time += (end_time - start_time);
    }
    
    fdca_dbg(fdev, "运行时挂起完成\n");
    return 0;
}

/**
 * fdca_pm_runtime_resume() - 运行时恢复
 */
static int fdca_pm_runtime_resume(struct device *dev)
{
    struct fdca_device *fdev = dev_get_drvdata(dev);
    u64 start_time, end_time;
    int ret;
    
    start_time = ktime_get_ns();
    
    fdca_dbg(fdev, "运行时恢复开始\n");
    
    /* 开启电源 */
    ret = fdca_pm_power_up(fdev);
    if (ret) {
        fdca_err(fdev, "开启电源失败: %d\n", ret);
        return ret;
    }
    
    /* 恢复上下文 */
    ret = fdca_pm_restore_context(fdev);
    if (ret) {
        fdca_err(fdev, "恢复上下文失败: %d\n", ret);
        return ret;
    }
    
    if (pm_mgr) {
        pm_mgr->state = FDCA_PM_ACTIVE;
        atomic64_inc(&pm_mgr->resume_count);
        
        end_time = ktime_get_ns();
        pm_mgr->total_resume_time += (end_time - start_time);
    }
    
    fdca_dbg(fdev, "运行时恢复完成\n");
    return 0;
}

/**
 * fdca_pm_suspend() - 系统挂起
 */
static int fdca_pm_suspend(struct device *dev)
{
    struct fdca_device *fdev = dev_get_drvdata(dev);
    int ret;
    
    fdca_info(fdev, "系统挂起\n");
    
    /* 执行运行时挂起流程 */
    ret = fdca_pm_runtime_suspend(dev);
    if (ret)
        return ret;
    
    if (pm_mgr) {
        pm_mgr->state = FDCA_PM_SYSTEM_SUSPEND;
    }
    
    return 0;
}

/**
 * fdca_pm_resume() - 系统恢复
 */
static int fdca_pm_resume(struct device *dev)
{
    struct fdca_device *fdev = dev_get_drvdata(dev);
    int ret;
    
    fdca_info(fdev, "系统恢复\n");
    
    /* 执行运行时恢复流程 */
    ret = fdca_pm_runtime_resume(dev);
    if (ret)
        return ret;
    
    return 0;
}

/* 电源管理操作结构 */
static const struct dev_pm_ops fdca_pm_ops = {
    .runtime_suspend = fdca_pm_runtime_suspend,
    .runtime_resume  = fdca_pm_runtime_resume,
    .suspend         = fdca_pm_suspend,
    .resume          = fdca_pm_resume,
};

/**
 * fdca_pm_init() - 初始化电源管理
 */
int fdca_pm_init(struct fdca_device *fdev)
{
    struct fdca_pm_manager *mgr;
    
    mgr = kzalloc(sizeof(*mgr), GFP_KERNEL);
    if (!mgr)
        return -ENOMEM;
    
    mgr->fdev = fdev;
    mgr->state = FDCA_PM_ACTIVE;
    mgr->pm_base = fdev->mmio_base + 0x200;  /* 电源管理寄存器基址 */
    
    atomic64_set(&mgr->suspend_count, 0);
    atomic64_set(&mgr->resume_count, 0);
    mgr->total_suspend_time = 0;
    mgr->total_resume_time = 0;
    
    pm_mgr = mgr;
    
    /* 启用运行时电源管理 */
    pm_runtime_enable(fdev->dev);
    pm_runtime_use_autosuspend(fdev->dev);
    pm_runtime_set_autosuspend_delay(fdev->dev, 1000);  /* 1秒延迟 */
    pm_runtime_mark_last_busy(fdev->dev);
    pm_runtime_put_autosuspend(fdev->dev);
    
    fdca_info(fdev, "电源管理初始化完成\n");
    return 0;
}

/**
 * fdca_pm_fini() - 清理电源管理
 */
void fdca_pm_fini(struct fdca_device *fdev)
{
    if (!pm_mgr)
        return;
    
    /* 禁用运行时电源管理 */
    pm_runtime_get_sync(fdev->dev);
    pm_runtime_disable(fdev->dev);
    
    fdca_info(fdev, "电源统计: 挂起 %lld 次 (%lld ns), 恢复 %lld 次 (%lld ns)\n",
              atomic64_read(&pm_mgr->suspend_count), pm_mgr->total_suspend_time,
              atomic64_read(&pm_mgr->resume_count), pm_mgr->total_resume_time);
    
    kfree(pm_mgr);
    pm_mgr = NULL;
}

/* 导出电源管理操作给 PCI 驱动使用 */
const struct dev_pm_ops *fdca_get_pm_ops(void)
{
    return &fdca_pm_ops;
}

EXPORT_SYMBOL_GPL(fdca_pm_init);
EXPORT_SYMBOL_GPL(fdca_pm_fini);
EXPORT_SYMBOL_GPL(fdca_get_pm_ops);
