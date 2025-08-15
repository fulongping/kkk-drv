// SPDX-License-Identifier: GPL-2.0-only
/*
 * FDCA RVV Instruction Type Processing Implementation
 */

#include <linux/kernel.h>
#include "fdca_drv.h"
#include "fdca_rvv_instr.h"

/* RVV 指令 opcode 定义 */
#define RVV_OPCODE_MASK     0x7F
#define RVV_LOAD_FP         0x07
#define RVV_STORE_FP        0x27
#define RVV_MADD            0x43
#define RVV_ARITH           0x57

/* 功能字段提取宏 */
#define EXTRACT_FUNCT3(opcode)  (((opcode) >> 12) & 0x7)
#define EXTRACT_FUNCT6(opcode)  (((opcode) >> 26) & 0x3F)
#define EXTRACT_VD(opcode)      (((opcode) >> 7) & 0x1F)
#define EXTRACT_VS1(opcode)     (((opcode) >> 15) & 0x1F)
#define EXTRACT_VS2(opcode)     (((opcode) >> 20) & 0x1F)
#define EXTRACT_VM(opcode)      (((opcode) >> 25) & 0x1)

/**
 * fdca_rvv_decode_instr_type() - 解码指令类型
 */
enum fdca_rvv_instr_type fdca_rvv_decode_instr_type(u32 opcode)
{
    u32 base_opcode = opcode & RVV_OPCODE_MASK;
    u32 funct3 = EXTRACT_FUNCT3(opcode);
    
    switch (base_opcode) {
    case RVV_LOAD_FP:
    case RVV_STORE_FP:
        return FDCA_RVV_INSTR_VMEM;
        
    case RVV_ARITH:
        switch (funct3) {
        case 0x0:  /* OPIVV */
        case 0x1:  /* OPFVV */
        case 0x2:  /* OPMVV */
        case 0x3:  /* OPIVI */
        case 0x4:  /* OPIVX */
        case 0x5:  /* OPFVF */
        case 0x6:  /* OPMVX */
            return FDCA_RVV_INSTR_VARITH;
        case 0x7:  /* OPCFG */
            return FDCA_RVV_INSTR_VSETVLI;
        default:
            return FDCA_RVV_INSTR_INVALID;
        }
        
    case RVV_MADD:
        return FDCA_RVV_INSTR_VARITH;
        
    default:
        return FDCA_RVV_INSTR_INVALID;
    }
}

/**
 * fdca_rvv_decode_vmem_type() - 解码向量内存指令类型
 */
static enum fdca_vmem_type fdca_rvv_decode_vmem_type(u32 opcode)
{
    u32 funct3 = EXTRACT_FUNCT3(opcode);
    u32 funct6 = EXTRACT_FUNCT6(opcode);
    
    switch (funct3) {
    case 0x0:  /* unit-stride */
        return FDCA_VMEM_UNIT_STRIDE;
    case 0x2:  /* strided */
        return FDCA_VMEM_STRIDED;
    case 0x3:  /* indexed */
        return FDCA_VMEM_INDEXED;
    case 0x1:  /* segment */
        return FDCA_VMEM_SEGMENT;
    case 0x4:  /* whole register */
        return FDCA_VMEM_WHOLE_REG;
    default:
        return FDCA_VMEM_UNIT_STRIDE;  /* 默认 */
    }
}

/**
 * fdca_rvv_decode_varith_type() - 解码向量算术指令类型
 */
static enum fdca_varith_type fdca_rvv_decode_varith_type(u32 opcode)
{
    u32 funct6 = EXTRACT_FUNCT6(opcode);
    
    switch (funct6) {
    case 0x00:  /* vadd */
        return FDCA_VARITH_ADD;
    case 0x02:  /* vsub */
        return FDCA_VARITH_SUB;
    case 0x25:  /* vmul */
        return FDCA_VARITH_MUL;
    case 0x20:  /* vdiv */
        return FDCA_VARITH_DIV;
    case 0x24:  /* vand */
        return FDCA_VARITH_AND;
    case 0x28:  /* vor */
        return FDCA_VARITH_OR;
    case 0x2C:  /* vxor */
        return FDCA_VARITH_XOR;
    case 0x30:  /* vsll */
    case 0x34:  /* vsrl */
    case 0x38:  /* vsra */
        return FDCA_VARITH_SHIFT;
    case 0x18:  /* vmseq */
    case 0x19:  /* vmsne */
    case 0x1A:  /* vmsltu */
    case 0x1B:  /* vmslt */
        return FDCA_VARITH_CMP;
    case 0x00:  /* vredsum (特殊编码) */
        return FDCA_VARITH_REDUCE;
    default:
        return FDCA_VARITH_ADD;  /* 默认 */
    }
}

/**
 * fdca_rvv_parse_instr() - 解析 RVV 指令
 */
int fdca_rvv_parse_instr(u32 opcode, struct fdca_rvv_instr *instr)
{
    if (!instr)
        return -EINVAL;
    
    memset(instr, 0, sizeof(*instr));
    instr->opcode = opcode;
    
    /* 解码指令类型 */
    instr->type = fdca_rvv_decode_instr_type(opcode);
    if (instr->type == FDCA_RVV_INSTR_INVALID)
        return -EINVAL;
    
    /* 提取通用字段 */
    instr->vd = EXTRACT_VD(opcode);
    instr->vs1 = EXTRACT_VS1(opcode);
    instr->vs2 = EXTRACT_VS2(opcode);
    instr->vm = !EXTRACT_VM(opcode);  /* vm=0 表示使用掩码 */
    
    /* 根据类型解析具体字段 */
    switch (instr->type) {
    case FDCA_RVV_INSTR_VMEM:
        instr->vmem_type = fdca_rvv_decode_vmem_type(opcode);
        instr->memory_access = true;
        instr->uses_mask = instr->vm;
        instr->latency = 10;  /* 内存访问延迟较高 */
        break;
        
    case FDCA_RVV_INSTR_VARITH:
        instr->varith_type = fdca_rvv_decode_varith_type(opcode);
        instr->memory_access = false;
        instr->uses_mask = instr->vm;
        instr->latency = (instr->varith_type == FDCA_VARITH_MUL || 
                         instr->varith_type == FDCA_VARITH_DIV) ? 5 : 2;
        break;
        
    case FDCA_RVV_INSTR_VSETVLI:
        instr->vl_setting = EXTRACT_VS1(opcode);  /* rs1 字段包含 AVL */
        instr->modifies_vl = true;
        instr->memory_access = false;
        instr->latency = 1;
        break;
        
    default:
        return -EINVAL;
    }
    
    return 0;
}

/**
 * fdca_rvv_validate_instr() - 验证 RVV 指令
 */
int fdca_rvv_validate_instr(const struct fdca_rvv_instr *instr)
{
    if (!instr)
        return -EINVAL;
    
    /* 检查寄存器编号范围 */
    if (instr->vd >= FDCA_RVV_NUM_VREGS ||
        instr->vs1 >= FDCA_RVV_NUM_VREGS ||
        instr->vs2 >= FDCA_RVV_NUM_VREGS)
        return -ERANGE;
    
    /* 检查指令类型特定的约束 */
    switch (instr->type) {
    case FDCA_RVV_INSTR_VMEM:
        /* 内存指令不能使用 v0 作为目标寄存器（如果使用掩码） */
        if (instr->uses_mask && instr->vd == 0)
            return -EINVAL;
        break;
        
    case FDCA_RVV_INSTR_VARITH:
        /* 某些算术指令有特殊约束 */
        if (instr->varith_type == FDCA_VARITH_REDUCE && instr->vd != instr->vs1)
            return -EINVAL;
        break;
        
    case FDCA_RVV_INSTR_VSETVLI:
        /* 配置指令的 VL 值检查 */
        if (instr->vl_setting > 1024)  /* 假设最大 VL */
            return -ERANGE;
        break;
        
    default:
        return -EINVAL;
    }
    
    return 0;
}

/**
 * fdca_rvv_instr_conflicts() - 检查指令冲突
 */
bool fdca_rvv_instr_conflicts(const struct fdca_rvv_instr *a, 
                             const struct fdca_rvv_instr *b)
{
    if (!a || !b)
        return false;
    
    /* 如果任一指令修改 VL，则冲突 */
    if (a->modifies_vl || b->modifies_vl)
        return true;
    
    /* 检查寄存器依赖 */
    /* WAW 冲突：两个指令写同一寄存器 */
    if (a->vd == b->vd)
        return true;
    
    /* RAW 冲突：第二个指令读第一个指令写的寄存器 */
    if (a->vd == b->vs1 || a->vd == b->vs2)
        return true;
    
    /* WAR 冲突：第二个指令写第一个指令读的寄存器 */
    if (b->vd == a->vs1 || b->vd == a->vs2)
        return true;
    
    /* 掩码寄存器冲突 */
    if ((a->uses_mask || b->uses_mask) && (a->vd == 0 || b->vd == 0))
        return true;
    
    /* 内存访问冲突（保守策略） */
    if (a->memory_access && b->memory_access)
        return true;
    
    return false;
}

EXPORT_SYMBOL_GPL(fdca_rvv_decode_instr_type);
EXPORT_SYMBOL_GPL(fdca_rvv_parse_instr);
EXPORT_SYMBOL_GPL(fdca_rvv_validate_instr);
EXPORT_SYMBOL_GPL(fdca_rvv_instr_conflicts);
