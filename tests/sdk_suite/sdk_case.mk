# tests/sdk_suite/sdk_case.mk — shared SDK consumer test rules
#
# Each case Makefile sets before include:
#   CASE_NAME   — short name (used in logs / elf name)
#   CASE_SRCS   — C sources relative to the case dir (and optional _common)
#   SENTINELS   — strings that must appear in QEMU output
#
# Optional:
#   QEMU_TIMEOUT, FAIL_SENTINEL, EXTRA_CFLAGS
#
# Shared SDK cache: set SDK_CACHE to a directory (dev.py does this once per
# arch/board).  When KERNEL_A already exists there, sdk_build is skipped.

ARCH ?= tricore
# Case dir is tests/sdk_suite/<case>/ → repo root is ../../..
WS        := $(abspath $(CURDIR)/../../..)
SUITE_DIR := $(abspath $(CURDIR)/..)
COMMON    := $(SUITE_DIR)/_common

QEMU_TIMEOUT  ?= 30
FAIL_SENTINEL ?= "$(CASE_NAME): FAIL"

ifeq ($(ARCH),tricore)
CC         := tricore-elf-gcc
QEMU       := qemu-system-tricore
MACHINE    := KIT_AURIX_TC397B_TRB
QEMU_EXTRA :=
BOARD      := boards/qemu_tc3xx
ARCH_FLAGS := -mcpu=tc39xx
else ifeq ($(ARCH),riscv)
CC         := riscv-none-elf-gcc
QEMU       := qemu-system-riscv32
MACHINE    := virt
QEMU_EXTRA := -bios none -m 16M
BOARD      ?= boards/qemu_riscv_virt
ARCH_FLAGS := -march=rv32imac_zicsr_zifencei -mabi=ilp32
# Optional: CASE Makefile sets SMP=1 for multi-hart SDK + QEMU -smp $(SMP_N)
SMP        ?= 0
SMP_N      ?= 2
ifeq ($(SMP),1)
ifneq ($(ARCH),riscv)
$(error SMP sdk_suite cases require ARCH=riscv)
endif
QEMU_EXTRA += -smp $(SMP_N)
SDK_SMP_FLAG := --enable-smp
TAG_SUFFIX := _smp
else
SDK_SMP_FLAG :=
TAG_SUFFIX :=
endif
else ifeq ($(ARCH),arm)
ARM_BOARD ?= qemu_mps2_an500
CC         := arm-none-eabi-gcc
QEMU       := qemu-system-arm
BOARD      := boards/$(ARM_BOARD)
# Hard float, matching the SDK: ld rejects a softfp consumer linked against a
# hard-float archive.  Both suite boards have an FPU; a board without one turns
# ULMK_CONFIG_FPU off in its board.cmake and is not exercised here.
ifeq ($(ARM_BOARD),qemu_mps2_an505)
MACHINE    := mps2-an505
ARCH_FLAGS := -mthumb -mcpu=cortex-m33 -mfloat-abi=hard -mfpu=fpv5-sp-d16
else
MACHINE    := mps2-an500
ARCH_FLAGS := -mthumb -mcpu=cortex-m7 -mfloat-abi=hard -mfpu=fpv5-sp-d16
endif
QEMU_EXTRA := -semihosting
else
$(error unsupported ARCH '$(ARCH)' — use tricore, riscv or arm)
endif

BOARD_NAME := $(notdir $(BOARD))
# Non-riscv boards: TAG_SUFFIX / SDK_SMP_FLAG empty
TAG_SUFFIX   ?=
SDK_SMP_FLAG ?=
# Opt-in dangerous ISR userspace attach (separate SDK cache + kernel).
IRQ_ATTACH ?= 0
ifeq ($(IRQ_ATTACH),1)
SDK_IRQ_ATTACH_FLAG := --enable-irq-attach
TAG_SUFFIX := $(TAG_SUFFIX)_irqattach
else
SDK_IRQ_ATTACH_FLAG :=
endif
TAG        := $(ARCH)_$(BOARD_NAME)_gcc$(TAG_SUFFIX)
TOOLCHAIN  := $(WS)/cmake/toolchain-$(ARCH)-gcc.cmake

# Shared cache (preferred) or per-case _out/
SDK_CACHE ?= $(SUITE_DIR)/_sdk_cache/$(TAG)
OUT   := $(CURDIR)/_out
BUILD := $(SDK_CACHE)/build
SDK   := $(SDK_CACHE)/ulmk

KERNEL_A := $(SDK)/lib/ulmk_kernel_$(TAG).a
BOARD_A  := $(SDK)/lib/ulmk_board_$(TAG).a
LD       := $(SDK)/linker/linker_$(TAG).ld

TARGET := $(CASE_NAME).elf
LOG    := /tmp/sdk_$(CASE_NAME)_$(ARCH)_$(BOARD_NAME).log

CFLAGS := \
	$(ARCH_FLAGS) \
	-ffreestanding \
	-Wall -Wextra -Wno-unused-parameter \
	-O2 -g \
	-I$(SDK)/include \
	-I$(SDK)/include/board \
	-I$(COMMON) \
	$(EXTRA_CFLAGS)

LDFLAGS := \
	-T $(LD) \
	$(ARCH_FLAGS) \
	-nostartfiles \
	-Wl,--gc-sections \
	-Wl,--no-warn-rwx-segments

QEMU_RUNNER := python3 $(SUITE_DIR)/qemu_until_sentinels.py

.PHONY: all run clean gen_config sdk

gen_config:
	@true

sdk:
	@if [ -f "$(KERNEL_A)" ] && [ -f "$(BOARD_A)" ] && [ -f "$(LD)" ] && \
		[ "$(SDK_FORCE_REBUILD)" != "1" ]; then \
		echo "--- SDK cache hit ($(TAG)) ---"; \
	else \
		echo "--- building SDK ($(TAG)) ---"; \
		mkdir -p "$(SDK_CACHE)"; \
		bash $(WS)/tools/sdk_build.sh \
			--toolchain $(TOOLCHAIN) \
			--chip-dir $(WS)/$(BOARD) \
			--arch $(ARCH) \
			--board-name $(BOARD_NAME) \
			--build-dir $(BUILD) \
			--out-dir $(SDK) \
			$(SDK_SMP_FLAG) $(SDK_IRQ_ATTACH_FLAG); \
	fi

all: sdk $(TARGET)

$(TARGET): sdk $(KERNEL_A) $(BOARD_A) $(LD) $(CASE_SRCS) $(COMMON)/sdk_test_util.h
	$(CC) $(CFLAGS) $(LDFLAGS) \
		$(CASE_SRCS) \
		-Wl,--start-group $(BOARD_A) $(KERNEL_A) -Wl,--end-group \
		-lc -lgcc \
		-o $@

run: all
	@echo "--- running $(TARGET) (ARCH=$(ARCH), timeout=$(QEMU_TIMEOUT)s) ---"
	@set -- ; \
	for S in $(SENTINELS) ; do \
		set -- "$$@" --sentinel "$$S" ; \
	done ; \
	$(QEMU_RUNNER) \
	    --timeout $(QEMU_TIMEOUT) \
	    --log $(LOG) \
	    --fail-sentinel $(FAIL_SENTINEL) \
	    "$$@" \
	    -- $(QEMU) \
	    -machine $(MACHINE) \
	    $(QEMU_EXTRA) \
	    -kernel $(TARGET) \
	    -nographic ; \
	FAIL=0 ; \
	for S in $(SENTINELS) ; do \
	    if grep -q "$$S" $(LOG) ; then \
	        echo "  [PASS] found: $$S" ; \
	    else \
	        echo "  [FAIL] missing: $$S" ; \
	        FAIL=1 ; \
	    fi ; \
	done ; \
	if grep -qF $(FAIL_SENTINEL) $(LOG) ; then \
	    echo "  [FAIL] case reported FAIL" ; \
	    FAIL=1 ; \
	fi ; \
	if [ $$FAIL -eq 0 ]; then \
	    echo "SDK SUITE PASS: $(TARGET)" ; \
	else \
	    echo "SDK SUITE FAIL: $(TARGET)" ; \
	    cat $(LOG) ; \
	    exit 1 ; \
	fi

clean:
	rm -f $(TARGET) $(LOG)
	rm -rf $(OUT)
