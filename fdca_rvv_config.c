// SPDX-License-Identifier: GPL-2.0-only
/*
 * FDCA RVV Configuration Management
 * 
 * 管理向量长度配置、元素宽度、LMUL 设置和动态 VL 调整
 * 参考 ara_pkg.sv 的配置参数
 */

#include <linux/slab.h>
#include "fdca_drv.h"
#include "fdca_rvv_state.h"

/**
 * fdca_rvv_config_validate() - 验证 RVV 配置
 */
int fdca_rvv_config_validate(struct fdca_device *fdev)
{
    struct fdca_rvv_config *config = &fdev->rvv_config;
    
    /* 验证 VLEN */
    if (config->vlen < 128 || config->vlen > 65536) {
        fdca_err(fdev, "无效的 VLEN: %u\n", config->vlen);
        return -EINVAL;
    }
    
    /* 验证 ELEN */
    if (config->elen > 64) {
        fdca_err(fdev, "无效的 ELEN: %u\n", config->elen);
        return -EINVAL;
    }
    
    /* 验证 Lane 数量 */
    if (config->num_lanes == 0 || config->num_lanes > 16) {
        fdca_err(fdev, "无效的 Lane 数量: %u\n", config->num_lanes);
        return -EINVAL;
    }
    
    fdca_info(fdev, "RVV 配置验证通过\n");
    return 0;
}

/**
 * fdca_rvv_config_init() - 初始化 RVV 配置
 */
int fdca_rvv_config_init(struct fdca_device *fdev)
{
    return fdca_rvv_config_validate(fdev);
}

EXPORT_SYMBOL_GPL(fdca_rvv_config_validate);
EXPORT_SYMBOL_GPL(fdca_rvv_config_init);
