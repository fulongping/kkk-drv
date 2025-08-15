/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * FDCA RVV Instruction Type Processing
 * 
 * 处理不同类型的 RVV 指令 (vmem_type, vamo_type, varith_type, vsetvli_type)
 * 实现指令解析和验证
 */

#ifndef __FDCA_RVV_INSTR_H__
#define __FDCA_RVV_INSTR_H__

#include <linux/types.h>

/* RVV 指令类型定义 */
enum fdca_rvv_instr_type {
    FDCA_RVV_INSTR_VMEM,      /* 向量内存指令 */
    FDCA_RVV_INSTR_VAMO,      /* 向量原子指令 */
    FDCA_RVV_INSTR_VARITH,    /* 向量算术指令 */
    FDCA_RVV_INSTR_VSETVLI,   /* 向量配置指令 */
    FDCA_RVV_INSTR_INVALID,   /* 无效指令 */
};

/* 向量内存指令类型 */
enum fdca_vmem_type {
    FDCA_VMEM_UNIT_STRIDE,    /* unit-strided */
    FDCA_VMEM_STRIDED,        /* strided */
    FDCA_VMEM_INDEXED,        /* indexed */
    FDCA_VMEM_SEGMENT,        /* segment */
    FDCA_VMEM_WHOLE_REG,      /* whole register */
};

/* 向量算术指令类型 */
enum fdca_varith_type {
    FDCA_VARITH_ADD,          /* 加法 */
    FDCA_VARITH_SUB,          /* 减法 */
    FDCA_VARITH_MUL,          /* 乘法 */
    FDCA_VARITH_DIV,          /* 除法 */
    FDCA_VARITH_AND,          /* 逻辑与 */
    FDCA_VARITH_OR,           /* 逻辑或 */
    FDCA_VARITH_XOR,          /* 逻辑异或 */
    FDCA_VARITH_SHIFT,        /* 移位 */
    FDCA_VARITH_CMP,          /* 比较 */
    FDCA_VARITH_REDUCE,       /* 归约 */
};

/* RVV 指令描述符 */
struct fdca_rvv_instr {
    u32 opcode;                           /* 指令码 */
    enum fdca_rvv_instr_type type;        /* 指令类型 */
    
    union {
        enum fdca_vmem_type vmem_type;    /* 内存指令类型 */
        enum fdca_varith_type varith_type; /* 算术指令类型 */
    };
    
    /* 操作数 */
    u8 vd;                               /* 目标寄存器 */
    u8 vs1;                              /* 源寄存器1 */
    u8 vs2;                              /* 源寄存器2 */
    bool vm;                             /* 掩码位 */
    
    /* 立即数 */
    union {
        u32 imm;                         /* 立即数 */
        u32 stride;                      /* 步长 */
        u32 vl_setting;                 /* VL 设置 */
    };
    
    /* 指令属性 */
    bool uses_mask;                      /* 是否使用掩码 */
    bool modifies_vl;                    /* 是否修改 VL */
    bool memory_access;                  /* 是否访问内存 */
    u32 latency;                         /* 预期延迟 */
};

/* 函数声明 */
enum fdca_rvv_instr_type fdca_rvv_decode_instr_type(u32 opcode);
int fdca_rvv_parse_instr(u32 opcode, struct fdca_rvv_instr *instr);
int fdca_rvv_validate_instr(const struct fdca_rvv_instr *instr);
bool fdca_rvv_instr_conflicts(const struct fdca_rvv_instr *a, 
                             const struct fdca_rvv_instr *b);

#endif /* __FDCA_RVV_INSTR_H__ */
