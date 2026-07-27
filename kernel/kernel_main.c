/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024-2026 Felipe Neves
 *
 * Platform-independent kernel entry — kernel/kernel_main.c
 * Called by startup.S after ulmk_arch_init() fills the boot_info.
 * Does not return.
 */

#include <stdint.h>
#include <ulmk/microkernel.h>
#include <ulmk/config.h>
#include <ulmk/syscall_wcet.h>
#include <ulmk_arch.h>
#include <kernel/include/ulmk_sched.h>
#include <kernel/include/ulmk_thread_internal.h>
#include <kernel/include/ulmk_irq_internal.h>
#include <kernel/include/ulmk_mem_internal.h>
#include <kernel/include/ulmk_percpu.h>
#include <kernel/include/ulmk_printk.h>
#include <kernel/include/ulmk_syscall_wcet_internal.h>
#include <kernel/include/ulmk_timer.h>
#include <kernel/syscall/syscall_router.h>

/* Linker-provided user pool boundaries (defined in .user_pool section). */
extern uint8_t _ulmk_user_pool_start[];
extern uint8_t _ulmk_user_pool_end[];

/* =========================================================================
 * Arch callbacks — invoked from the arch layer (ISR/trap stubs)
 * ========================================================================= */

void ulmk_kern_sched_dispatch(bool from_isr)
{
	ulmk_sched_trap_dispatch(from_isr);
}

void ulmk_kern_ipi_resched(void)
{
	ulmk_sched_request_resched();
}

#if ULMK_CONFIG_ENABLE_SMP
void ulmk_kern_ipi_from_isr(void)
{
	ulmk_sched_request_resched();
	/*
	 * Early IPI before sched_start publishes current: arm needs_resched
	 * only.  sched_start must not clear it on SMP (see sched.c).
	 */
	if (!ulmk_percpu()->current)
		return;
	ulmk_arch_ipi_note_enter();
	ulmk_kern_sched_dispatch(true);
}
#endif

void ulmk_kern_timer_tick(void)
{
	ulmk_arch_tick_ack();
	ulmk_timer_tick();
	if (!ulmk_percpu()->current)
		return;
	ulmk_kern_sched_dispatch(true);
}

uint32_t ulmk_kern_tick_start(void)
{
	static bool started;

	if (!started) {
		started = true;
		ulmk_arch_tick_init(ULMK_CONFIG_TICK_HZ);
	}
	return (uint32_t)(int32_t)ULMK_OK;
}

/*
 * After trap-exit switch, pick up destroy errors / recv_or_notif notif rc
 * that were staged while this thread was blocked.
 */
uint32_t ulmk_kern_syscall_ret_resolve(uint32_t ret)
{
	ulmk_thread_t *cur = ulmk_sched_current();

	if (!cur)
		return ret;

	if (cur->block_status != 0) {
		ret = (uint32_t)(int32_t)cur->block_status;
		cur->block_status            = 0;
		cur->syscall_wake_ret_valid  = 0;
		cur->ipc_msg_outptr          = NULL;
		cur->ipc_sender_outptr       = NULL;
		cur->notif_bits_outptr       = NULL;
		cur->rn_result_outptr        = NULL;
		return ret;
	}

	if (cur->syscall_wake_ret_valid) {
		ret = (uint32_t)(int32_t)cur->syscall_wake_ret;
		cur->syscall_wake_ret_valid = 0;
	}
	return ret;
}

void ulmk_kern_trap_mpu_restore(void)
{
	ulmk_thread_t *cur = ulmk_sched_current();

	if (!cur)
		return;

	ulmk_arch_mpu_switch(cur->regions, cur->region_count,
			     cur->privilege == ULMK_PRIV_KERNEL ? 0u : 1u);
}

uint32_t ulmk_kern_trap_syscall(uint8_t tin, uint32_t args[4])
{
#if ULMK_CONFIG_SYSCALL_WCET
	uint32_t begin;
	uint32_t end;
	uint32_t blocked;
	uint32_t wall;
	uint32_t ret;

	ulmk_syscall_wcet_account_reset();
	begin = ulmk_arch_cycle_read();
	ret   = ulmk_syscall_router(tin, args[0], args[1], args[2], args[3]);
	end   = ulmk_arch_cycle_read();
	blocked = ulmk_syscall_wcet_blocked_cycles();
	wall = end - begin;
	if (blocked > wall)
		blocked = wall;

	{
		ulmk_thread_t *cur = ulmk_sched_current();
		uint32_t cpu = ulmk_arch_cpu_id();
		volatile struct ulmk_syscall_wcet_slot *slot;
		volatile struct ulmk_syscall_wcet_slot *out;

		if (cpu >= ULMK_SYSCALL_WCET_MAX_CPUS)
			cpu = 0u;
		slot = &g_ulmk_syscall_wcet[cpu];
		slot->magic   = ULMK_SYSCALL_WCET_MAGIC;
		slot->nr      = tin;
		slot->begin   = begin;
		slot->end     = end;
		slot->blocked = blocked;
		slot->delta   = wall - blocked;
		slot->seq++;

		out = cur ? cur->wcet_out : NULL;
		if (out && out != slot) {
			out->magic   = ULMK_SYSCALL_WCET_MAGIC;
			out->nr      = tin;
			out->begin   = begin;
			out->end     = end;
			out->blocked = blocked;
			out->delta   = wall - blocked;
			out->seq++;
		}
	}
	return ret;
#else
	return ulmk_syscall_router(tin, args[0], args[1], args[2], args[3]);
#endif
}

void ulmk_kern_trap_recoverable(void)
{
	ulmk_thread_t *cur = ulmk_sched_current();

	if (ulmk_irq_in_attach()) {
		ulmk_kern_irq_attach_fault();
		for (;;)
			;
	}

	if (cur) {
		ulmk_printk("TRAP: killing thread tid=%u\n",
			  (unsigned)cur->tid);
		cur->state = UL_THREAD_STATE_DEAD;
		ulmk_sched_dequeue(cur);
		ulmk_sched_set_dead_for_cleanup(cur);
		ulmk_sched_resched();
	}

	for (;;)
		;
}

void ulmk_kern_trap_panic(void)
{
	ulmk_printk("KERNEL PANIC: unrecoverable trap\n");
	for (;;)
		;
}

/* =========================================================================
 * Thread entries
 * ========================================================================= */

#ifndef ULMK_ARCH_IDLE_ENTRY
#define ULMK_ARCH_IDLE_ENTRY	idle_thread_entry
#endif
#ifndef ULMK_ARCH_IDLE_PRIVILEGE
#define ULMK_ARCH_IDLE_PRIVILEGE	ULMK_PRIV_KERNEL
#endif

static void idle_thread_entry(void *arg)
{
	(void)arg;
	for (;;) {
		/*
		 * Idle may be resumed via ctx_switch from a path that left
		 * interrupts masked.  Without re-enabling, the tick never
		 * arrives and sleepers park forever.
		 */
		ulmk_arch_cpu_irq_enable();
		ulmk_arch_cpu_idle();
	}
}

/* =========================================================================
 * Static thread storage
 * ========================================================================= */

#ifndef ULMK_ARCH_KSTACK_SIZE
#define ULMK_ARCH_KSTACK_SIZE	0u
#endif
/*
 * Idle runs on its own stack and absorbs IPIs / ISRs.  Trap frame alone is
 * ~144 bytes on RISC-V; the C IRQ path needs several hundred more.  SMP
 * idle is hit by remote enqueue IPIs — 256 overflowed into mepc (INST_FAULT
 * mtval=0).  Concurrent wakes on 3+ harts still blew 2048 (adjacent idle
 * stacks corrupt each other → INST_FAULT at trap epilogue).
 */
#define ULMK_IDLE_STACK_SIZE	(4096u + ULMK_ARCH_KSTACK_SIZE)

static ulmk_thread_t idle_thread_g[ULMK_NR_CPUS] UL_KERNEL_BSS;
static uint8_t     idle_stack_g[ULMK_NR_CPUS][ULMK_IDLE_STACK_SIZE]
	__attribute__((aligned(8))) UL_KERNEL_BSS;

static ulmk_thread_t root_thread_g   UL_KERNEL_BSS;
static uint8_t     root_stack_g[4096]
	__attribute__((aligned(8), section(".user_bss")));

#if ULMK_CONFIG_ENABLE_SMP
/*
 * Secondary CPUs enter here after ulmk_arch_start_secondary().  Sched state
 * and idle threads were prepared on CPU0; we only run the local sched_start.
 */
void ulmk_kern_secondary_main(void)
{
	uint32_t cpu = ulmk_arch_cpu_id();

	ulmk_printk("ulmk: CPU%u secondary entry\n", (unsigned)cpu);
#if ULMK_CONFIG_SYSCALL_WCET
	/* CCNT/DWT is per-core — primary enable does not cover this CPU. */
	ulmk_arch_cycle_enable();
#endif
	ulmk_arch_secondary_init();
	ulmk_arch_tick_init(ULMK_CONFIG_TICK_HZ);
	/*
	 * online + secondary_mark_ready happen inside ulmk_sched_start() only
	 * after current is set — otherwise a remote IPI can run dispatch with
	 * current==NULL and only arm needs_resched.  Arch idle entry must
	 * accept IPIs (RISC-V kernel trampoline enables MIE; TriCore idle
	 * CSA has PIE=1 on RFE) so the GPSR/CLINT ISR can schedule.
	 */
	ulmk_printk("ulmk: CPU%u sched_start\n", (unsigned)cpu);
	(void)cpu;
	ulmk_sched_start();
	for (;;)
		;
}
#else
void ulmk_kern_secondary_main(void)
{
	for (;;)
		;
}
#endif

/* =========================================================================
 * Kernel main — does not return
 * ========================================================================= */

void ulmk_kern_main(const ulmk_boot_info_t *info)
{
	ulmk_thread_attr_t attr = {0};
	uint32_t           cpu;

	ulmk_printk("ulmk: kernel entry\n");

	ulmk_sched_init();
	ulmk_sched_set_class(&ulmk_bitmap_rt_class);
	UL_LOG_DBG("sched init done");

	ulmk_timer_init();
	UL_LOG_DBG("timer init done");

	ulmk_irq_table_init();
	UL_LOG_DBG("irq table init done");

#if ULMK_CONFIG_SYSCALL_WCET
	ulmk_arch_cycle_enable();
	for (cpu = 0u; cpu < ULMK_SYSCALL_WCET_MAX_CPUS; cpu++) {
		g_ulmk_syscall_wcet[cpu].magic = ULMK_SYSCALL_WCET_MAGIC;
		g_ulmk_syscall_wcet[cpu].seq   = 0u;
	}
#endif

	ulmk_arch_mpu_init();
	ulmk_arch_cpu_irq_enable();

	ulmk_heap_init((uintptr_t)_ulmk_user_pool_start,
		     (uintptr_t)_ulmk_user_pool_end - (uintptr_t)_ulmk_user_pool_start);
	ulmk_printk("ulmk: heap %u bytes available\n",
		  (unsigned)ulmk_heap_free_bytes());

	/*
	 * One idle thread per CPU — always present in that CPU's runqueue so
	 * pick_next never returns NULL after start.
	 */
	for (cpu = 0u; cpu < (uint32_t)ULMK_NR_CPUS; cpu++) {
		attr.name       = "idle";
		attr.entry      = ULMK_ARCH_IDLE_ENTRY;
		attr.arg        = NULL;
		attr.priority   = 255u;
		attr.stack_size = ULMK_IDLE_STACK_SIZE;
		attr.privilege  = ULMK_ARCH_IDLE_PRIVILEGE;
		attr.cpu        = (uint8_t)cpu;
		ulmk_thread_init(&idle_thread_g[cpu], &attr, idle_stack_g[cpu]);
		ulmk_percpu_of(cpu)->idle = &idle_thread_g[cpu];
		ulmk_sched_enqueue(&idle_thread_g[cpu]);
	}

	attr.name       = "root";
	attr.entry      = (void (*)(void *))ulmk_root_thread;
	attr.arg        = (void *)info;
	attr.priority   = 0u;
	attr.stack_size = sizeof(root_stack_g);
	attr.privilege  = ULMK_PRIV_DRIVER;
	attr.cpu        = 0u;
	ulmk_thread_init(&root_thread_g, &attr, root_stack_g);
	root_thread_g.cap_flags = ULMK_CAP_ALL;
	ulmk_sched_enqueue(&root_thread_g);

#ifdef ULMK_CSA_CTX_EARLY_TEST
	extern void csa_ctx_run_early(void);

	csa_ctx_run_early();
#endif

#ifdef UL_KERNEL_PRE_ROOT_HOOK
	{
		extern void ulmk_kernel_pre_root_hook(void);
		ulmk_kernel_pre_root_hook();
	}
#endif

	ulmk_arch_smp_mark_ready();

#if ULMK_CONFIG_ENABLE_SMP
	/*
	 * Bring up secondaries before MPU enforce so arch boot stubs can
	 * still write the ready handshake under a permissive protection
	 * window.
	 */
	for (cpu = 1u; cpu < (uint32_t)ULMK_NR_CPUS; cpu++) {
		ulmk_printk("ulmk: starting CPU%u\n", (unsigned)cpu);
		ulmk_arch_start_secondary(cpu, ulmk_kern_secondary_main);
	}
	{
		uint32_t want = (1u << ULMK_NR_CPUS) - 1u;

		ulmk_arch_smp_wait_ready(want);
		ulmk_printk("ulmk: smp ready mask=0x%x\n",
			    (unsigned)ulmk_arch_smp_ready_mask());
		{
			extern volatile uint32_t g_c29_smp_diag[6]
				__attribute__((weak));

			if (&g_c29_smp_diag[0] != NULL &&
			    g_c29_smp_diag[0] != 0u) {
				ulmk_printk(
					"ulmk: c29 smp diag cpu=%u rststat=0x%x rstctrl=0x%x vect=0x%x mem=0x%x ready=0x%x\n",
					(unsigned)g_c29_smp_diag[0],
					(unsigned)g_c29_smp_diag[1],
					(unsigned)g_c29_smp_diag[2],
					(unsigned)g_c29_smp_diag[3],
					(unsigned)g_c29_smp_diag[4],
					(unsigned)g_c29_smp_diag[5]);
			}
		}
	}
#endif

	ulmk_arch_mpu_enable();
	UL_LOG_DBG("mpu enabled");

	ulmk_printk("ulmk: switching to root thread\n");

	/*
	 * ulmk_sched_start() picks the highest-priority ready thread
	 * (root_thread_g, prio=0) and performs the initial context switch.
	 * It does not return.
	 */
	ulmk_sched_start();

	for (;;)
		;
}
