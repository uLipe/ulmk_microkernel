/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024-2026 Felipe Neves
 *
 * IRQ binding table and kernel IRQ handlers — kernel/irq/irq.c
 * Reference: docs/api_spec.md §10
 *
 * Routing model:
 *   ulmk_irq_bind()     → records irq_nr→(notif_obj, bit) in irq_table
 *   ulmk_irq_attach()   → callback fast-path + owned notif (bit 0)
 *   ulmk_irq_enable()   → arms the interrupt controller via arch layer
 *   ulmk_irq_ack()      → acknowledges the interrupt source
 *   ulmk_kern_irq_dispatch() → ISR: attach trampoline and/or notif_signal
 */

#include <stdint.h>
#include <string.h>
#include <ulmk/microkernel.h>
#include <ulmk/config.h>
#include <ulmk/board.h>
#include <kernel/include/ulmk_irq_internal.h>
#include <kernel/include/ulmk_notif_internal.h>
#include <kernel/include/ulmk_mem_internal.h>
#include <kernel/include/ulmk_klock.h>
#include <kernel/include/ulmk_percpu.h>
#include <kernel/include/ulmk_sched.h>
#include <kernel/include/ulmk_printk.h>
#include <kernel/syscall/syscall_router.h>
#include <ulmk_arch.h>

/* =========================================================================
 * Binding table — static, kernel-space, hardware-bounded size.
 * ========================================================================= */

static ulmk_irq_binding_t UL_KERNEL_BSS irq_table[ULMK_CONFIG_MAX_IRQ_BINDINGS];

/* =========================================================================
 * Board interrupt controller — optional routing stage ahead of the CPU's
 * own controller.  See include/ulmk/board.h.
 * ========================================================================= */

#if ULMK_CONFIG_BOARD_IRQ_CTRL

/*
 * No stub here on purpose: a board that turns this option on must define both
 * hooks, and a missing one has to fail the link.  A weak no-op would be picked
 * up ahead of a definition arriving later in the archive order, leaving the
 * routing silently undone.
 */
#define board_irq_connect(srpn)		ulmk_board_irq_connect(srpn)
#define board_irq_disconnect(srpn)	ulmk_board_irq_disconnect(srpn)

#else /* board has no routing stage: keep it out of the IRQ path entirely */

#define board_irq_connect(srpn)		((void)0)
#define board_irq_disconnect(srpn)	((void)0)

#endif

void ulmk_irq_table_init(void)
{
	memset(irq_table, 0, sizeof(irq_table));
}

bool ulmk_irq_in_attach(void)
{
	return ulmk_percpu()->in_irq_attach;
}

int ulmk_irq_binding_add(uint8_t srpn, ulmk_notif_obj_t *notif, uint32_t bit)
{
	ulmk_arch_irq_key_t key;
	int i;
	int ret = -1;

	key = ulmk_arch_spin_lock_irqsave(&g_ulmk_lock_irq);
	for (i = 0; i < ULMK_CONFIG_MAX_IRQ_BINDINGS; i++) {
		if (!irq_table[i].notif) {
			irq_table[i].srpn         = srpn;
			irq_table[i].notif        = notif;
			irq_table[i].bit          = bit;
			irq_table[i].enabled      = false;
			irq_table[i].attach_fn    = NULL;
			irq_table[i].attach_data  = NULL;
			irq_table[i].owner        = NULL;
			irq_table[i].owned_notif  = false;
			ret = i;
			break;
		}
	}
	ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_irq, key);
	return ret;
}

ulmk_irq_binding_t *ulmk_irq_binding_by_srpn(uint8_t srpn)
{
	int i;

	for (i = 0; i < ULMK_CONFIG_MAX_IRQ_BINDINGS; i++) {
		if (irq_table[i].notif && irq_table[i].srpn == srpn)
			return &irq_table[i];
	}
	return NULL;
}

static void irq_binding_clear(ulmk_irq_binding_t *b)
{
	memset(b, 0, sizeof(*b));
}

static uint32_t irq_attach_common(uint32_t srpn, uint32_t fn_addr,
				  uint32_t data_addr, uint32_t src_reg,
				  bool have_src)
{
#if !ULMK_CONFIG_IRQ_ATTACH
	(void)srpn;
	(void)fn_addr;
	(void)data_addr;
	(void)src_reg;
	(void)have_src;
	return (uint32_t)(int32_t)ULMK_ENOTSUP;
#else
	ulmk_notif_obj_t *notif;
	ulmk_thread_t *cur;
	ulmk_irq_attach_fn_t fn;
	int ret;
	uint32_t nid;

	if (srpn == 0u || srpn >= 256u || fn_addr == 0u)
		return (uint32_t)ULMK_NOTIF_INVALID;
	if (have_src && src_reg == 0u)
		return (uint32_t)ULMK_NOTIF_INVALID;

	if (ulmk_irq_binding_by_srpn((uint8_t)srpn))
		return (uint32_t)ULMK_NOTIF_INVALID;

	cur = ulmk_sched_current();
	if (!cur)
		return (uint32_t)ULMK_NOTIF_INVALID;

	nid = ulmk_kern_notif_create();
	if (nid == (uint32_t)ULMK_NOTIF_INVALID)
		return (uint32_t)ULMK_NOTIF_INVALID;

	notif = ulmk_notif_by_id((ulmk_notif_t)nid);
	if (!notif) {
		(void)ulmk_kern_notif_destroy(nid);
		return (uint32_t)ULMK_NOTIF_INVALID;
	}

	ret = ulmk_irq_binding_add((uint8_t)srpn, notif, 0u);
	if (ret < 0) {
		(void)ulmk_kern_notif_destroy(nid);
		return (uint32_t)ULMK_NOTIF_INVALID;
	}

	fn = (ulmk_irq_attach_fn_t)(uintptr_t)fn_addr;
	irq_table[ret].attach_fn   = fn;
	irq_table[ret].attach_data = (void *)(uintptr_t)data_addr;
	irq_table[ret].owner       = cur;
	irq_table[ret].owned_notif = true;

	board_irq_connect((uint8_t)srpn);
	if (have_src)
		ulmk_arch_irq_src_register((uint8_t)srpn, src_reg);
	else
		ulmk_arch_irq_src_configure((uint8_t)srpn, (uint8_t)srpn, 0u);

	return nid;
#endif
}

/* =========================================================================
 * Kernel syscall handlers
 * ========================================================================= */

uint32_t ulmk_kern_irq_bind(uint32_t srpn, uint32_t notif_id, uint32_t bit)
{
	ulmk_notif_obj_t *notif;
	int             ret;

	if (srpn == 0u || srpn >= 256u || bit > 31u)
		return (uint32_t)(int32_t)ULMK_EINVAL;

	if (ulmk_irq_binding_by_srpn((uint8_t)srpn))
		return (uint32_t)(int32_t)ULMK_EINVAL;

	notif = ulmk_notif_by_id((ulmk_notif_t)notif_id);
	if (!notif)
		return (uint32_t)(int32_t)ULMK_EINVAL;

	ret = ulmk_irq_binding_add((uint8_t)srpn, notif, bit);
	if (ret < 0)
		return (uint32_t)(int32_t)ULMK_ENOSPC;

	board_irq_connect((uint8_t)srpn);
	ulmk_arch_irq_src_configure((uint8_t)srpn, (uint8_t)srpn, 0u);
	return 0u;
}

uint32_t ulmk_kern_irq_bind_hw(uint32_t srpn, uint32_t notif_id,
			     uint32_t bit, uint32_t src_reg)
{
	ulmk_notif_obj_t *notif;
	int             ret;

	if (srpn == 0u || srpn >= 256u || bit > 31u || src_reg == 0u)
		return (uint32_t)(int32_t)ULMK_EINVAL;

	if (ulmk_irq_binding_by_srpn((uint8_t)srpn))
		return (uint32_t)(int32_t)ULMK_EINVAL;

	notif = ulmk_notif_by_id((ulmk_notif_t)notif_id);
	if (!notif)
		return (uint32_t)(int32_t)ULMK_EINVAL;

	ret = ulmk_irq_binding_add((uint8_t)srpn, notif, bit);
	if (ret < 0)
		return (uint32_t)(int32_t)ULMK_ENOSPC;

	board_irq_connect((uint8_t)srpn);
	ulmk_arch_irq_src_register((uint8_t)srpn, src_reg);
	return 0u;
}

uint32_t ulmk_kern_irq_attach(uint32_t srpn, uint32_t fn, uint32_t data)
{
	return irq_attach_common(srpn, fn, data, 0u, false);
}

uint32_t ulmk_kern_irq_attach_hw(uint32_t srpn, uint32_t fn, uint32_t data,
				 uint32_t src_reg)
{
	return irq_attach_common(srpn, fn, data, src_reg, true);
}

uint32_t ulmk_kern_irq_detach(uint32_t srpn)
{
#if !ULMK_CONFIG_IRQ_ATTACH
	(void)srpn;
	return (uint32_t)(int32_t)ULMK_ENOTSUP;
#else
	ulmk_irq_binding_t *b;
	ulmk_notif_t nid;
	bool owned;

	if (srpn == 0u || srpn >= 256u)
		return (uint32_t)(int32_t)ULMK_EINVAL;

	b = ulmk_irq_binding_by_srpn((uint8_t)srpn);
	if (!b || !b->attach_fn)
		return (uint32_t)(int32_t)ULMK_EINVAL;

	b->enabled = false;
	ulmk_arch_irq_src_disable((uint8_t)srpn);
	ulmk_arch_irq_src_ack((uint8_t)srpn);
	board_irq_disconnect((uint8_t)srpn);

	owned = b->owned_notif;
	nid   = b->notif ? b->notif->id : ULMK_NOTIF_INVALID;
	irq_binding_clear(b);

	if (owned && nid != ULMK_NOTIF_INVALID)
		(void)ulmk_kern_notif_destroy((uint32_t)nid);

	return 0u;
#endif
}

uint32_t ulmk_kern_irq_enable(uint32_t srpn)
{
	ulmk_irq_binding_t *b;

	if (srpn == 0u || srpn >= 256u)
		return (uint32_t)(int32_t)ULMK_EINVAL;

	b = ulmk_irq_binding_by_srpn((uint8_t)srpn);
	if (!b)
		return (uint32_t)(int32_t)ULMK_EINVAL;
	b->enabled = true;

	ulmk_arch_irq_src_enable((uint8_t)srpn);
	return 0u;
}

uint32_t ulmk_kern_irq_disable(uint32_t srpn)
{
	ulmk_irq_binding_t *b;

	if (srpn == 0u || srpn >= 256u)
		return (uint32_t)(int32_t)ULMK_EINVAL;

	b = ulmk_irq_binding_by_srpn((uint8_t)srpn);
	if (!b)
		return (uint32_t)(int32_t)ULMK_EINVAL;
	b->enabled = false;

	ulmk_arch_irq_src_disable((uint8_t)srpn);
	ulmk_arch_irq_src_ack((uint8_t)srpn);
	return 0u;
}

uint32_t ulmk_kern_irq_ack(uint32_t srpn)
{
	ulmk_irq_binding_t *b;

	if (srpn == 0u || srpn >= 256u)
		return (uint32_t)(int32_t)ULMK_EINVAL;

	b = ulmk_irq_binding_by_srpn((uint8_t)srpn);
	if (!b)
		return (uint32_t)(int32_t)ULMK_EINVAL;

	ulmk_arch_irq_src_ack((uint8_t)srpn);
	/*
	 * Undoes the mask applied when the notification was signalled; see
	 * ulmk_kern_irq_dispatch().  Enabling an already-unmasked source is a
	 * no-op, so drivers that ack outside a wait are unaffected.
	 */
	if (b->enabled && !b->attach_fn)
		ulmk_arch_irq_src_enable((uint8_t)srpn);
	return 0u;
}

void ulmk_kern_irq_attach_fault(void)
{
	struct ulmk_percpu *pc = ulmk_percpu();
	ulmk_thread_t *owner = pc->irq_attach_owner;
	uint8_t srpn = pc->irq_attach_srpn;
	ulmk_irq_binding_t *b;
	ulmk_notif_t nid;
	bool owned;

	pc->in_irq_attach = false;
	pc->irq_attach_owner = NULL;
	pc->irq_attach_srpn = 0u;

	b = ulmk_irq_binding_by_srpn(srpn);
	if (b) {
		b->enabled = false;
		ulmk_arch_irq_src_disable(srpn);
		ulmk_arch_irq_src_ack(srpn);
		owned = b->owned_notif;
		nid   = b->notif ? b->notif->id : ULMK_NOTIF_INVALID;
		irq_binding_clear(b);
		if (owned && nid != ULMK_NOTIF_INVALID)
			(void)ulmk_kern_notif_destroy((uint32_t)nid);
	}

	if (owner && owner->state != UL_THREAD_STATE_DEAD) {
		ulmk_printk("TRAP: irq attach fault - killing tid=%u\n",
			    (unsigned)owner->tid);
		owner->state = UL_THREAD_STATE_DEAD;
		ulmk_sched_dequeue(owner);
		ulmk_sched_set_dead_for_cleanup(owner);
	}

	ulmk_arch_mpu_switch(NULL, 0u, 0u);
	ulmk_sched_resched();
}

/* =========================================================================
 * ISR dispatch — called from the arch generic ISR handler.
 * ========================================================================= */

void ulmk_kern_irq_dispatch(uint8_t srpn)
{
	ulmk_irq_binding_t *b = ulmk_irq_binding_by_srpn(srpn);

	if (!b || !b->enabled || !b->notif)
		return;

	if (b->attach_fn) {
#if ULMK_CONFIG_IRQ_ATTACH
		struct ulmk_percpu *pc = ulmk_percpu();
		bool do_notify;

		pc->irq_attach_owner = b->owner;
		pc->irq_attach_srpn  = srpn;
		pc->in_irq_attach    = true;
		do_notify = ulmk_arch_irq_attach_call(
			b->attach_fn, b->attach_data,
			b->owner ? b->owner->regions : NULL,
			b->owner ? b->owner->region_count : 0u);
		pc->in_irq_attach    = false;
		pc->irq_attach_owner = NULL;
		pc->irq_attach_srpn  = 0u;
		if (do_notify) {
			ulmk_arch_irq_src_ack(srpn);
			notif_signal_impl(b->notif->id, 1u << b->bit);
		}
#else
		/* Config off: treat as legacy notif-only if somehow bound. */
		notif_signal_impl(b->notif->id, 1u << b->bit);
#endif
		return;
	}

	/*
	 * Threaded delivery: the condition that raised the line is cleared by
	 * the server thread, not here, so a level-triggered source would keep
	 * re-entering this handler and the thread would never get to run.
	 * Mask it and let ulmk_irq_ack() from the server reopen it.
	 */
	ulmk_arch_irq_src_disable(srpn);
	notif_signal_impl(b->notif->id, 1u << b->bit);
}
