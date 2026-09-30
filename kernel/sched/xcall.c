/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024-2026 Felipe Neves
 *
 * Cross-CPU calls — kernel/sched/xcall.c
 *
 * One request slot per CPU.  The request lives on the caller's kernel stack
 * and is only touched under the target's slot lock, so the caller may return
 * as soon as it sees it done.
 */

#include <stdbool.h>
#include <stddef.h>
#include <ulmk/config.h>
#include <kernel/include/ulmk_percpu.h>
#include <kernel/include/ulmk_xcall.h>
#include <ulmk_arch.h>

#if ULMK_CONFIG_ENABLE_SMP

struct ulmk_xcall {
	ulmk_xcall_fn_t	 fn;
	void		*arg;
	uint32_t	 rc;
	bool		 done;
};

void ulmk_xcall_service(void)
{
	struct ulmk_percpu *pc = ulmk_percpu();
	struct ulmk_xcall *x;
	uint32_t rc;

	ulmk_arch_spin_lock(&pc->xcall_lock);
	x = pc->xcall;
	pc->xcall = NULL;
	ulmk_arch_spin_unlock(&pc->xcall_lock);
	if (!x)
		return;

	rc = x->fn(x->arg);

	ulmk_arch_spin_lock(&pc->xcall_lock);
	x->rc   = rc;
	x->done = true;
	ulmk_arch_spin_unlock(&pc->xcall_lock);
}

uint32_t ulmk_xcall(uint32_t cpu, ulmk_xcall_fn_t fn, void *arg)
{
	struct ulmk_percpu *dst = ulmk_percpu_of(cpu);
	struct ulmk_xcall x;
	bool posted = false;
	bool done = false;

	/* An offline CPU runs nothing, so nothing there can race @fn. */
	if (cpu == ulmk_arch_cpu_id() || !dst->online)
		return fn(arg);

	x.fn   = fn;
	x.arg  = arg;
	x.rc   = 0u;
	x.done = false;

	/*
	 * IRQs are off, so while waiting this CPU must keep serving its own
	 * slot: two CPUs calling each other would otherwise spin forever.
	 */
	while (!posted) {
		ulmk_arch_spin_lock(&dst->xcall_lock);
		if (!dst->xcall) {
			dst->xcall = &x;
			posted = true;
		}
		ulmk_arch_spin_unlock(&dst->xcall_lock);
		if (!posted)
			ulmk_xcall_service();
	}

	ulmk_arch_send_ipi(cpu);

	while (!done) {
		ulmk_xcall_service();
		ulmk_arch_spin_lock(&dst->xcall_lock);
		done = x.done;
		ulmk_arch_spin_unlock(&dst->xcall_lock);
	}
	return x.rc;
}

#endif /* ULMK_CONFIG_ENABLE_SMP */
