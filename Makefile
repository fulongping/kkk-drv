# SPDX-License-Identifier: GPL-2.0-only
#
# FDCA (Fangzheng Distributed Computing Architecture) Kernel Driver Makefile
# 
# Copyright (C) 2024 Fangzheng Technology Co., Ltd.
#

# 模块名称
obj-m := fdca.o

# 模块源文件
fdca-y := fdca_main.o \
          fdca_pci.o \
          fdca_drm.o \
          fdca_vram.o \
          fdca_gtt.o \
          fdca_memory.o \
          fdca_rvv_state.o \
          fdca_rvv_config.o \
          fdca_vrf.o \
          fdca_rvv_instr.o \
          fdca_vector_mem.o \
          fdca_queue.o \
          fdca_sync.o \
          fdca_noc.o \
          fdca_debug.o \
          fdca_pm.o

# 可选模块 (后续实现)
# fdca-y += fdca_vram.o fdca_gtt.o
# fdca-y += fdca_rvv_state.o fdca_rvv_config.o
# fdca-y += fdca_queue.o fdca_scheduler.o
# fdca-y += fdca_noc.o fdca_sync.o

# 编译标志
ccflags-y := -I$(src) -I$(src)/include
ccflags-y += -Wall -Wextra -Wno-unused-parameter
ccflags-y += -DDEBUG

# 内核版本检查
KERNEL_VERSION := $(shell uname -r)
KERNEL_DIR := /lib/modules/$(KERNEL_VERSION)/build

# 默认目标
all:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

# 清理目标
clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean
	rm -f *.ur-safe

# 安装目标
install: all
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules_install
	depmod -a

# 卸载目标
uninstall:
	rm -f /lib/modules/$(KERNEL_VERSION)/extra/fdca.ko
	depmod -a

# 加载模块
load: all
	sudo insmod fdca.ko

# 卸载模块
unload:
	sudo rmmod fdca || true

# 重新加载模块
reload: unload load

# 查看模块信息
info:
	modinfo fdca.ko

# 查看内核日志
dmesg:
	dmesg | grep -i fdca | tail -20

# 检查语法
check:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) C=1 modules

# 稀疏检查
sparse:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) C=2 modules

# 帮助信息
help:
	@echo "FDCA 内核驱动编译系统"
	@echo ""
	@echo "可用目标:"
	@echo "  all      - 编译模块 (默认)"
	@echo "  clean    - 清理编译文件"
	@echo "  install  - 安装模块到系统"
	@echo "  uninstall- 从系统卸载模块"
	@echo "  load     - 加载模块"
	@echo "  unload   - 卸载模块"
	@echo "  reload   - 重新加载模块"
	@echo "  info     - 显示模块信息"
	@echo "  dmesg    - 查看相关内核日志"
	@echo "  check    - 语法检查"
	@echo "  sparse   - 稀疏检查"
	@echo "  help     - 显示此帮助"

.PHONY: all clean install uninstall load unload reload info dmesg check sparse help
