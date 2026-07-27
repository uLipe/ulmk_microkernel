/* SPDX-License-Identifier: MIT */
/*
 * C29 architecture constants — arch/c29/include/arch_config.h
 */

#ifndef ULMK_ARCH_C29_CONFIG_H
#define ULMK_ARCH_C29_CONFIG_H

#include <ulmk/platform.h>

#ifndef ULMK_ARCH_NUM_CPU
#define ULMK_ARCH_NUM_CPU	1
#endif

#ifndef ULMK_ARCH_HAVE_FPU
#define ULMK_ARCH_HAVE_FPU	1
#endif

/* APR / region alignment (SSU normal APR granularity). */
#define ULMK_ARCH_MAX_REGIONS	16
#define ULMK_ARCH_REGION_ALIGN	4096u

#define ULMK_ARCH_PRS_KERNEL	0u
#define ULMK_ARCH_PRS_USER	1u

#ifndef ULMK_ARCH_IDLE_IS_WFI
#define ULMK_ARCH_IDLE_IS_WFI	1
#endif

/*
 * Fabricate contexts on the affinity CPU so entry IDs resolve through the
 * core-local table (distinct LPA/CPA addresses).
 */
#define ULMK_ARCH_CTX_FABRICATE_ON_AFFINITY_CPU	1

/* C29 software stack grows upward (A15 increases). */
#define ULMK_ARCH_STACK_GROWS_UP	1

/*
 * Idle must run on the INTSP/runtime Stack so Supervisor INT can enter.
 * Default ports keep kernel-privileged idle; C29 overrides both.
 */
#ifndef ULMK_ARCH_IDLE_ENTRY
void ulmk_arch_idle_entry(void *arg);
#define ULMK_ARCH_IDLE_ENTRY	ulmk_arch_idle_entry
#endif

#ifndef ULMK_ARCH_IDLE_PRIVILEGE
#define ULMK_ARCH_IDLE_PRIVILEGE	ULMK_PRIV_USER
#endif

/* Full software INT frame: A14/RPC/DSTS/ESTS + A0-13 + D0-15 + M0-31. */
#define ULMK_ARCH_CTX_FRAME_SIZE	264u

/* PIPE / timer / IPC / SSU bases — board may override via platform.h. */
#ifndef ULMK_BOARD_PIPE_BASE
#define ULMK_BOARD_PIPE_BASE		0x30020000u
#endif
#ifndef ULMK_BOARD_CPUTIMER2_BASE
#define ULMK_BOARD_CPUTIMER2_BASE	0x3021A000u
#endif
#ifndef ULMK_BOARD_MEMSSMISCI_BASE
#define ULMK_BOARD_MEMSSMISCI_BASE	0x301D8E00u
#endif
#ifndef ULMK_BOARD_SSUGEN_BASE
#define ULMK_BOARD_SSUGEN_BASE		0x30080000u
#endif
#ifndef ULMK_BOARD_SSUCPU1CFG_BASE
#define ULMK_BOARD_SSUCPU1CFG_BASE	0x30081000u
#endif
#ifndef ULMK_BOARD_SSUCPU2CFG_BASE
#define ULMK_BOARD_SSUCPU2CFG_BASE	0x30082000u
#endif
#ifndef ULMK_BOARD_SSUCPU3CFG_BASE
#define ULMK_BOARD_SSUCPU3CFG_BASE	0x30083000u
#endif
#ifndef ULMK_BOARD_SSUCPU1AP_BASE
#define ULMK_BOARD_SSUCPU1AP_BASE	0x30087000u
#endif
#ifndef ULMK_BOARD_DEVCFG_BASE
#define ULMK_BOARD_DEVCFG_BASE		0x30180000u
#endif
#ifndef ULMK_BOARD_WD_DISABLE_ADDR
#define ULMK_BOARD_WD_DISABLE_ADDR	0x30208C52u
#endif
#ifndef ULMK_BOARD_WD_DISABLE_VAL
#define ULMK_BOARD_WD_DISABLE_VAL	0x68u
#endif

#ifndef ULMK_BOARD_TICK_CLOCK_HZ
#define ULMK_BOARD_TICK_CLOCK_HZ	200000000u
#endif

/* PIPE channel numbers (logical SRPN). */
#ifndef ULMK_BOARD_IRQ_TIMER2
#define ULMK_BOARD_IRQ_TIMER2		8u
#endif
#ifndef ULMK_BOARD_IRQ_SWINT_YIELD
#define ULMK_BOARD_IRQ_SWINT_YIELD	255u	/* INT_SW1 */
#endif
#ifndef ULMK_BOARD_IRQ_SWINT_SYSCALL
#define ULMK_BOARD_IRQ_SWINT_SYSCALL	254u	/* INT_SW2 */
#endif
#ifndef ULMK_BOARD_IRQ_IPC_1
#define ULMK_BOARD_IRQ_IPC_1		12u	/* INT_IPC_1_1 */
#endif
#ifndef ULMK_BOARD_IRQ_IPC_2
#define ULMK_BOARD_IRQ_IPC_2		16u	/* INT_IPC_2_1 */
#endif

/*
 * Secondary reset vectors (TI affinity: CPU2 fetches LPA, CPU3 fetches CPA).
 * Release always targets LPA1 / CPA0.  Flash builds keep stub LMAs in the
 * contiguous CPU1 FLASH_RP0 image (symbols ulmk_c29_cpu{2,3}_start); CPU1
 * copies them into those banks before release (CPU2 has no flash XIP).
 */
#define ULMK_C29_CPU2_RESET_ADDR	0x20108000u
#define ULMK_C29_CPU3_RESET_ADDR	0x20110000u
#if defined(ULMK_C29_FLASH) && ULMK_C29_FLASH
#define ULMK_C29_SECONDARY_STUB_BYTES	0x80u
#endif

/* IPC FLAG0 is the only flag that vectors. */
#define ULMK_ARCH_IPC_FLAG0		0x1u

/*
 * PIPE register map (SPRUJ79 / hw_pipe.h):
 *   GLOBAL_EN        +0x08
 *   RTINT_THRESHOLD  +0x00
 *   INTSP            +0x6C
 *   TASK_CTRL        +0x90
 *   INT_CTL_L(n)     +0x1000 + n*4   EN / FLAG
 *   INT_CTL_H(n)     +0x2000 + n*4   FLAG_FRC / FLAG_CLR
 *   INT_CONFIG(n)    +0x3000 + n*4   PRI_LEVEL / CONTEXT_ID
 *   INT_LINK_OWNER   +0x4000 + n*4
 *   INT_VECT_ADDR(n) +0x5000 + n*4
 */
#define ULMK_ARCH_PIPE_MMR_CLR \
	(ULMK_BOARD_PIPE_BASE + 0xA0u)
#define ULMK_ARCH_PIPE_MEM_INIT \
	(ULMK_BOARD_PIPE_BASE + 0x44u)
#define ULMK_ARCH_PIPE_MEM_INIT_STS \
	(ULMK_BOARD_PIPE_BASE + 0x48u)
#define ULMK_ARCH_PIPE_MEM_INIT_KEY	0x5A5A0000u
#define ULMK_ARCH_PIPE_GLOBAL_EN \
	(ULMK_BOARD_PIPE_BASE + 0x08u)
#define ULMK_ARCH_PIPE_RTINT_THRESHOLD \
	(ULMK_BOARD_PIPE_BASE + 0x00u)
#define ULMK_ARCH_PIPE_INTSP \
	(ULMK_BOARD_PIPE_BASE + 0x6Cu)
#define ULMK_ARCH_PIPE_TASK_CTRL \
	(ULMK_BOARD_PIPE_BASE + 0x90u)
#define ULMK_ARCH_PIPE_INT_CTL_L(n) \
	(ULMK_BOARD_PIPE_BASE + 0x1000u + ((uint32_t)(n) * 4u))
#define ULMK_ARCH_PIPE_INT_CTL_H(n) \
	(ULMK_BOARD_PIPE_BASE + 0x2000u + ((uint32_t)(n) * 4u))
#define ULMK_ARCH_PIPE_INT_CONFIG(n) \
	(ULMK_BOARD_PIPE_BASE + 0x3000u + ((uint32_t)(n) * 4u))
#define ULMK_ARCH_PIPE_INT_LINK_OWNER(n) \
	(ULMK_BOARD_PIPE_BASE + 0x4000u + ((uint32_t)(n) * 4u))
#define ULMK_ARCH_PIPE_INT_VECT(n) \
	(ULMK_BOARD_PIPE_BASE + 0x5000u + ((uint32_t)(n) * 4u))

#define ULMK_ARCH_PIPE_GLOBAL_EN_KEY	0xFACE0000u
#define ULMK_ARCH_PIPE_GLOBAL_EN_VAL	0x3u
#define ULMK_ARCH_PIPE_TASK_CTRL_KEY	0xCAFE0000u
#define ULMK_ARCH_PIPE_SUP_IGN_INTE_EN	0x100u

#define ULMK_ARCH_PIPE_CTL_L_EN		0x1u
#define ULMK_ARCH_PIPE_FLAG_FRC		0x1u
#define ULMK_ARCH_PIPE_FLAG_CLR		0x2u

#define ULMK_ARCH_PIPE_OWNER_LINK2	2u
#define ULMK_ARCH_PIPE_SUP_PRI		255u
#define ULMK_ARCH_PIPE_INTSP_STACK	2u	/* SSU_STACK2 — matches TI init + ESTS */

#define ULMK_ARCH_MEM_DLB_CONFIG \
	(ULMK_BOARD_MEMSSMISCI_BASE + 0x0u)
#define ULMK_ARCH_MEM_DLB_CPU1_EN	(1u << 0)
#define ULMK_ARCH_MEM_DLB_CPU2_EN	(1u << 1)
#define ULMK_ARCH_MEM_DLB_CPU3_EN	(1u << 2)

#define ULMK_ARCH_SYSCTL_LSEN \
	(ULMK_BOARD_DEVCFG_BASE + 0x348u)

#endif /* ULMK_ARCH_C29_CONFIG_H */
