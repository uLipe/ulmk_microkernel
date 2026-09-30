/* boards/qemu_mps2_an500/memory.ld — Layer 3 chip input for QEMU mps2-an500. */
MEMORY {
	KERNEL_FLASH  (rx)  : ORIGIN = 0x00000000, LENGTH = 512K
	KERNEL_RAM    (rwx) : ORIGIN = 0x20000000, LENGTH = 1024K
	SHARED_RAM    (rwx) : ORIGIN = 0x20100000, LENGTH = 256K
	PERIPH        (rw)  : ORIGIN = 0x40000000, LENGTH = 256M
}

ULMK_KERNEL_STACK_SIZE = 0x1000;
ULMK_ISR_STACK_SIZE    = 0x0800;
ULMK_MPU_ALIGN         = 32;

/* PMSAv7 power-of-two regions: see linker/kernel/user_ram_start.ld.in. */
ULMK_USER_TEXT_ALIGN   = 0x8000;
ULMK_USER_RAM_ALIGN    = 0x8000;

HAVE_CSA        = 0;
HAVE_SMALL_DATA = 0;
HAVE_BMHD       = 0;
