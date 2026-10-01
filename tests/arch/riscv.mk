# tests/arch/riscv.mk — RISC-V RV32 integration-test toolchain

CC      := riscv-none-elf-gcc
AR      := riscv-none-elf-ar
QEMU    := qemu-system-riscv32
MACHINE := virt
QEMU_EXTRA := -bios none -m 16M
BOARD   := $(ROOT)/boards/qemu_riscv_virt
TEST_LD := boot_test_riscv.ld

ARCH_CFLAGS := \
	-march=rv32imac_zicsr_zifencei -mabi=ilp32

ifeq ($(ULMK_RISCV_IRQ_INTEG),1)
ARCH_CFLAGS += \
	-DULMK_ARCH_HAVE_CLINT=1 \
	-DULMK_BOARD_CLINT_BASE=0x02000000u
endif

ARCH_LDFLAGS := -march=rv32imac_zicsr_zifencei -mabi=ilp32

ARCH_KERNEL_SRCS := \
	$(ROOT)/arch/riscv/startup.S \
	$(ROOT)/arch/riscv/arch.c \
	$(ROOT)/arch/riscv/mpu_pmp.c \
	$(ROOT)/arch/riscv/smp.c \
	$(ROOT)/arch/riscv/irq.c \
	$(ROOT)/arch/riscv/irq_clint.c \
	$(ROOT)/arch/riscv/irq_clic.c \
	$(ROOT)/arch/riscv/irq_plic.c \
	$(ROOT)/arch/riscv/ctx_switch.S \
	$(ROOT)/arch/riscv/trap.S \
	$(ROOT)/boards/qemu_riscv_virt/qemu_printk_hook.c

ARCH_INCLUDE := -I$(ROOT)/arch/riscv/include

BOARD_EXTRA_SRCS :=

ARCH_TEST_IO := $(INTEG_DIR)arch/riscv_test_io.c

ifneq ($(ARCH_SKIP_BOARD_SVC),1)
INTEG_KERNEL_STUB :=
else
INTEG_KERNEL_STUB ?= $(ROOT)/stub/board_init_stub.c
endif
