/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024-2026 Felipe Neves
 *
 * Non-cascading hierarchical timing wheel — kernel/time/timer_wheel.c
 *
 * Inspired by Linux 4.8 timer wheel (Gleixner): timers stay in their level
 * bucket until that coarser clock reaches the slot — no cascade storms.
 * Insert/cancel use bitmask math + intrusive dlist (O(1)).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <ulmk/microkernel.h>
#include <kernel/include/ulmk_timer.h>
#include <kernel/include/ulmk_percpu.h>
#ifndef UL_UNIT_TEST
#include <kernel/include/ulmk_klock.h>
#include <ulmk_arch.h>
#endif

struct timer_wheel {
	sys_dlist_t buckets[ULMK_TIMER_LVL_DEPTH][ULMK_TIMER_LVL_SIZE];
	uint64_t    clk;
	/* Bit i set ⇒ bucket i of that level is non-empty. */
	uint64_t    pending[ULMK_TIMER_LVL_DEPTH];
};

/*
 * TriCore: keep the wheel out of early .kernel_bss — growing that region
 * pushes .isr_stack across ORIGIN(KERNEL_RAM)+0x8000 (_small_data_ / A0).
 * Other arches keep the wheel in normal BSS.
 */
#if defined(__tricore__) && !defined(UL_UNIT_TEST)
#define ULMK_TIMER_WHEEL_SECTION \
	__attribute__((section(".user_bss"), aligned(8)))
#else
#define ULMK_TIMER_WHEEL_SECTION
#endif

static struct timer_wheel g_wheels[ULMK_NR_CPUS] ULMK_TIMER_WHEEL_SECTION;

static struct timer_wheel *wheel_of(void)
{
	uint32_t cpu;

#ifndef UL_UNIT_TEST
	cpu = ulmk_arch_timer_wheel_cpu() % (uint32_t)ULMK_NR_CPUS;
#else
	cpu = 0u;
#endif
	return &g_wheels[cpu];
}

static unsigned int calc_index(uint64_t expires, unsigned int lvl, uint64_t clk)
{
	uint64_t idx;

	/*
	 * Round up expiry to the next level granularity so a timer never
	 * fires before its requested time (slack only afterward).
	 */
	expires = (expires + ULMK_TIMER_LVL_GRAN(lvl) - 1u) &
		  ~(uint64_t)(ULMK_TIMER_LVL_GRAN(lvl) - 1u);
	(void)clk;
	idx = (expires >> ULMK_TIMER_LVL_SHIFT(lvl)) & ULMK_TIMER_LVL_MASK;
	return (unsigned int)idx;
}

static unsigned int calc_wheel_index(uint64_t expires, uint64_t clk,
				    unsigned int *bucket)
{
	uint64_t delta = expires - clk;
	unsigned int lvl;

	if ((int64_t)delta < 0) {
		*bucket = (unsigned int)(clk & ULMK_TIMER_LVL_MASK);
		return 0u;
	}

	for (lvl = 0u; lvl < ULMK_TIMER_LVL_DEPTH; lvl++) {
		uint64_t cutoff;

		/*
		 * Level n covers deltas < (LVL_SIZE << LVL_SHIFT(n)).
		 * Level 0: < 64, level 1: < 512, … level 4: < 262144.
		 */
		cutoff = (uint64_t)ULMK_TIMER_LVL_SIZE <<
			 ULMK_TIMER_LVL_SHIFT(lvl);
		if (delta < cutoff) {
			*bucket = calc_index(expires, lvl, clk);
			return lvl;
		}
	}

	/* Should be rejected by ulmk_timer_add — park on outer level. */
	*bucket = calc_index(expires, ULMK_TIMER_LVL_DEPTH - 1u, clk);
	return ULMK_TIMER_LVL_DEPTH - 1u;
}

void ulmk_timer_init(void)
{
	uint32_t cpu;
	unsigned int lvl;
	unsigned int b;

	for (cpu = 0u; cpu < (uint32_t)ULMK_NR_CPUS; cpu++) {
		struct timer_wheel *w = &g_wheels[cpu];

		w->clk = 0u;
		for (lvl = 0u; lvl < ULMK_TIMER_LVL_DEPTH; lvl++) {
			w->pending[lvl] = 0u;
			for (b = 0u; b < ULMK_TIMER_LVL_SIZE; b++)
				sys_dlist_init(&w->buckets[lvl][b]);
		}
	}
}

int ulmk_timer_add(struct ulmk_timeout *to, uint32_t delta_ticks)
{
	struct timer_wheel *w;
	unsigned int lvl;
	unsigned int bucket;
#ifndef UL_UNIT_TEST
	ulmk_arch_irq_key_t key;
#endif

	if (!to || !to->cb)
		return ULMK_EINVAL;
	if (delta_ticks == 0u)
		delta_ticks = 1u;
	if (delta_ticks > ULMK_TIMER_TIMEOUT_MAX)
		return ULMK_EINVAL;

#ifndef UL_UNIT_TEST
	key = ulmk_arch_spin_lock_irqsave(&g_ulmk_lock_timer);
#endif
	w = wheel_of();

	if (sys_dnode_is_linked(&to->node))
		sys_dlist_remove(&to->node);

	to->expires = w->clk + (uint64_t)delta_ticks;
	lvl = calc_wheel_index(to->expires, w->clk, &bucket);
	sys_dlist_append(&w->buckets[lvl][bucket], &to->node);
	w->pending[lvl] |= (1ull << bucket);

#ifndef UL_UNIT_TEST
	ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_timer, key);
#endif
	return ULMK_OK;
}

bool ulmk_timer_cancel(struct ulmk_timeout *to)
{
	bool removed = false;
#ifndef UL_UNIT_TEST
	ulmk_arch_irq_key_t key;
#endif

	if (!to)
		return false;

#ifndef UL_UNIT_TEST
	key = ulmk_arch_spin_lock_irqsave(&g_ulmk_lock_timer);
#endif
	if (sys_dnode_is_linked(&to->node)) {
		sys_dlist_remove(&to->node);
		sys_dnode_init(&to->node);
		removed = true;
		/*
		 * Leave pending bit set — empty buckets are cheap to scan;
		 * clearing would require a full bucket walk.
		 */
	}
#ifndef UL_UNIT_TEST
	ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_timer, key);
#endif
	return removed;
}

uint64_t ulmk_timer_jiffies(void)
{
	return wheel_of()->clk;
}

static void collect_bucket(struct timer_wheel *w, unsigned int lvl,
			   unsigned int bucket, sys_dlist_t *out)
{
	sys_dlist_t *src = &w->buckets[lvl][bucket];
	sys_dnode_t *node;
	sys_dnode_t *next;

	SYS_DLIST_FOR_EACH_NODE_SAFE(src, node, next) {
		sys_dlist_remove(node);
		sys_dlist_append(out, node);
	}
	w->pending[lvl] &= ~(1ull << bucket);
}

static void expire_list(sys_dlist_t *list)
{
	/*
	 * Drain by pop-head, not FOR_EACH_NODE_SAFE.  A callback may
	 * cancel another timer still on this pending list; SAFE keeps a
	 * stale "next" that becomes self-linked after cancel and loops
	 * forever with IRQs masked (kills the tick).
	 */
	while (!sys_dlist_is_empty(list)) {
		sys_dnode_t *node = sys_dlist_get(list);
		struct ulmk_timeout *to =
			SYS_DLIST_CONTAINER_OF(node, struct ulmk_timeout, node);
		void (*cb)(struct ulmk_timeout *) = to->cb;

		sys_dnode_init(node);
		if (cb)
			cb(to);
	}
}

void ulmk_timer_run(void)
{
	struct timer_wheel *w;
	sys_dlist_t pending;
	unsigned int lvl;
#ifndef UL_UNIT_TEST
	ulmk_arch_irq_key_t key;
#endif

	sys_dlist_init(&pending);

#ifndef UL_UNIT_TEST
	key = ulmk_arch_spin_lock_irqsave(&g_ulmk_lock_timer);
#endif
	w = wheel_of();

	for (lvl = 0u; lvl < ULMK_TIMER_LVL_DEPTH; lvl++) {
		uint64_t gran = ULMK_TIMER_LVL_GRAN(lvl);
		unsigned int bucket;

		/* Higher levels only fire on their coarser clock edge. */
		if (lvl > 0u && (w->clk & (gran - 1u)) != 0u)
			continue;

		bucket = (unsigned int)((w->clk >> ULMK_TIMER_LVL_SHIFT(lvl)) &
					ULMK_TIMER_LVL_MASK);
		if ((w->pending[lvl] & (1ull << bucket)) == 0u)
			continue;
		collect_bucket(w, lvl, bucket, &pending);
	}

#ifndef UL_UNIT_TEST
	ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_timer, key);
#endif

	/* Callbacks run with the timer lock released. */
	expire_list(&pending);
}

static void wheel_advance_collect(struct timer_wheel *w, sys_dlist_t *pending)
{
	unsigned int lvl;

	w->clk++;

	for (lvl = 0u; lvl < ULMK_TIMER_LVL_DEPTH; lvl++) {
		uint64_t gran = ULMK_TIMER_LVL_GRAN(lvl);
		unsigned int bucket;

		if (lvl > 0u && (w->clk & (gran - 1u)) != 0u)
			continue;

		bucket = (unsigned int)((w->clk >> ULMK_TIMER_LVL_SHIFT(lvl)) &
				       ULMK_TIMER_LVL_MASK);
		if ((w->pending[lvl] & (1ull << bucket)) == 0u)
			continue;
		collect_bucket(w, lvl, bucket, pending);
	}
}

void ulmk_timer_tick(void)
{
	sys_dlist_t pending;
#ifndef UL_UNIT_TEST
	ulmk_arch_irq_key_t key;
#endif

	sys_dlist_init(&pending);

#ifndef UL_UNIT_TEST
	key = ulmk_arch_spin_lock_irqsave(&g_ulmk_lock_timer);
#endif
	wheel_advance_collect(wheel_of(), &pending);
#ifndef UL_UNIT_TEST
	ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_timer, key);
#endif

	expire_list(&pending);
}
