# tests/integ_kernel.mk
# Build kernel sources as a static library so boot_test.ld can select
# sections with *libtest_kernel.a:(...) patterns (same model as production).

INTEG_KERNEL_LIB ?= libtest_kernel.a

INTEG_KERNEL_EXTRA_SRCS ?=

# Match production: kernel/arch at -Ofast -fno-inline (or -Os when
# ULMK_OPTIMIZE_SIZE=1).  Test harness objects keep integ_common.mk -O0.
# Deferred (=) so CFLAGS from integ_common.mk is visible when compiling.
# -fno-inline: TriCore CSA/PCXI breaks under GCC inlining at -O3/-Ofast.
INTEG_KERNEL_OPT ?= -Ofast -fno-inline
ifeq ($(ULMK_OPTIMIZE_SIZE),1)
INTEG_KERNEL_OPT := -Os
endif
INTEG_KERNEL_CFLAGS = $(filter-out -O0 -O1 -O2 -O3 -Os -Ofast,$(CFLAGS)) \
	$(INTEG_KERNEL_OPT)

# init.c copies .data / zeroes .bss before the C runtime is up — prevent GCC
# from turning those loops into memcpy/memset calls.
INTEG_KERNEL_EXTRA_CFLAGS += -fno-tree-loop-distribute-patterns

# ARCH_KERNEL_SRCS must be set by tests/arch/$(ARCH).mk before include.
INTEG_KERNEL_STUB ?= $(ROOT)/stub/board_init_stub.c

INTEG_KERNEL_SRCS ?= \
	$(ARCH_KERNEL_SRCS) \
	$(ROOT)/kernel/init/init.c \
	$(ROOT)/kernel/kernel_main.c \
	$(ROOT)/kernel/printk/ulmk_printk.c \
	$(ROOT)/kernel/percpu/percpu.c \
	$(ROOT)/kernel/percpu/klock.c \
	$(ROOT)/kernel/sched/sched.c \
	$(ROOT)/kernel/sched/xcall.c \
	$(ROOT)/kernel/sched/fifo_rt.c \
	$(ROOT)/kernel/sched/bitmap_rt.c \
	$(ROOT)/kernel/irq/irq.c \
	$(ROOT)/kernel/mem/area.c \
	$(ROOT)/kernel/mem/mem.c \
	$(ROOT)/kernel/mem/tlsf.c \
	$(ROOT)/kernel/thread/thread.c \
	$(ROOT)/kernel/ipc/ep.c \
	$(ROOT)/kernel/notif/notif.c \
	$(ROOT)/kernel/time/timer_wheel.c \
	$(ROOT)/kernel/time/sleep.c \
	$(ROOT)/kernel/syscall/syscall_router.c \
	$(ROOT)/kernel/syscall/syscall_wcet.c \
	$(INTEG_KERNEL_STUB) \
	$(INTEG_KERNEL_EXTRA_SRCS)

INTEG_KERNEL_OBJS := $(patsubst $(ROOT)/%,integ_kernel/%,$(INTEG_KERNEL_SRCS))
INTEG_KERNEL_OBJS := $(INTEG_KERNEL_OBJS:.c=.o)
INTEG_KERNEL_OBJS := $(INTEG_KERNEL_OBJS:.S=.o)

$(INTEG_KERNEL_LIB): $(INTEG_KERNEL_OBJS)
	$(AR) rcs $@ $^

integ_kernel/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(INTEG_KERNEL_CFLAGS) $(INTEG_KERNEL_EXTRA_CFLAGS) -c -o $@ $<

integ_kernel/%.o: $(ROOT)/%.S
	@mkdir -p $(dir $@)
	$(CC) $(INTEG_KERNEL_CFLAGS) -c -o $@ $<

.PHONY: integ_kernel_clean integ_kernel_rebuild
integ_kernel_clean:
	rm -rf integ_kernel $(INTEG_KERNEL_LIB)

integ_kernel_rebuild: integ_kernel_clean $(INTEG_KERNEL_LIB)
