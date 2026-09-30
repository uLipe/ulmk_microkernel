CC      := cc
ROOT    := ../..
STUBINC := include

CFLAGS := \
    -std=c99 \
    -Wall -Wextra \
    -Wno-unused-parameter \
    -DUL_UNIT_TEST \
    -I$(STUBINC) \
    -I$(ROOT) \
    -g -O0

# The linker-script symbols mmu_sv32.c reads, laid out like qemu_riscv_virt:
# PERIPH in the first 512 MiB, user text/RAM and the heap in one megapage.
LDFLAGS := \
    -no-pie \
    -Wl,--defsym=_ulmk_user_text_start=0x80001000 \
    -Wl,--defsym=_ulmk_user_text_end=0x80003000 \
    -Wl,--defsym=_ulmk_user_ram_start=0x80010000 \
    -Wl,--defsym=_ulmk_user_pool_start=0x80012000 \
    -Wl,--defsym=_ulmk_mem_periph_base=0x00000000 \
    -Wl,--defsym=_ulmk_mem_periph_end=0x20000000

TARGET := sv32_unit_test

.PHONY: all gen_config run clean

all: $(TARGET)

gen_config:

$(TARGET): sv32_unit_test.c $(ROOT)/arch/riscv/mmu_sv32.c
	$(CC) $(CFLAGS) sv32_unit_test.c $(LDFLAGS) -o $@

run: $(TARGET)
	@echo "--- running sv32 unit tests ---"
	@./$(TARGET) && echo "SV32 UNIT TEST: PASS" || \
	    { echo "SV32 UNIT TEST: FAIL"; exit 1; }

clean:
	rm -f $(TARGET)
