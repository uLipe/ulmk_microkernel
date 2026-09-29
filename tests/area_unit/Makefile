CC      := cc
ROOT    := ../..
STUBINC := include

CFLAGS := \
    -std=c99 \
    -Wall -Wextra \
    -Wno-unused-parameter \
    -I$(STUBINC) \
    -I$(ROOT) \
    -g -O0

SRCS := \
    area_unit_test.c \
    $(ROOT)/kernel/mem/area.c \
    $(ROOT)/kernel/percpu/klock.c \
    $(ROOT)/kernel/mem/tlsf.c

# One binary per window encoding the arch ports use:
#   napot  RISC-V PMP NAPOT      (pow2, 8 B)
#   v7m    ARMv7-M PMSAv7        (pow2, 32 B)
#   v8m    ARMv8-M PMSAv8        (granule, 32 B)
#   tc     TriCore DPR           (granule, 8 B)
VARIANTS := napot v7m v8m tc
napot_DEFS := -DULMK_ARCH_MPU_WIN_POW2=1 -DULMK_ARCH_MPU_WIN_MIN=8u
v7m_DEFS   := -DULMK_ARCH_MPU_WIN_POW2=1 -DULMK_ARCH_MPU_WIN_MIN=32u
v8m_DEFS   := -DULMK_ARCH_MPU_WIN_POW2=0 -DULMK_ARCH_MPU_WIN_MIN=32u
tc_DEFS    := -DULMK_ARCH_MPU_WIN_POW2=0 -DULMK_ARCH_MPU_WIN_MIN=8u

TARGETS := $(patsubst %,area_%_unit_test,$(VARIANTS))

.PHONY: all run clean

all: $(TARGETS)

area_%_unit_test: $(SRCS)
	$(CC) $(CFLAGS) $($*_DEFS) $^ -o $@

run: $(TARGETS)
	@echo "--- running area unit tests ---"
	@for t in $(TARGETS); do \
	    echo "== $$t"; \
	    ./$$t || { echo "AREA UNIT TEST: FAIL"; exit 1; }; \
	done; echo "AREA UNIT TEST: PASS"

clean:
	rm -f $(TARGETS)
