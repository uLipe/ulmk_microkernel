/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024-2026 Felipe Neves
 *
 * Board supplements to the arch layer.
 *
 * An arch port covers what the ISA defines.  Everything a particular SoC bolts
 * on around it — a routing stage ahead of the interrupt controller, a timer
 * that is not the architectural one, cache maintenance the ISA does not
 * specify, extra memory-protection entries — is declared here and implemented
 * by the board.
 *
 * Every hook is mandatory whenever its feature is selected, and none of them
 * has a fallback definition.  A board that selects a feature and forgets a
 * symbol fails to link, naming what is missing.  Weak stubs are forbidden
 * project-wide precisely for this: the linker resolves a reference from the
 * first archive member that satisfies it, so a stub would quietly win over the
 * board's own definition — or over one supplied by an SDK consumer — and the
 * result would be silence at runtime rather than an error at build time.
 * See README.md and docs/build_system_spec.md §9.3.
 *
 * Selection is per feature:
 *
 *   ULMK_CONFIG_BOARD_IRQ_CTRL    routing stage ahead of the CPU controller
 *   ULMK_CONFIG_BOARD_IRQ_CLAIM   board claims IRQs before generic dispatch
 *   ULMK_CONFIG_BOARD_PMP_EXTRA   board adds memory-protection entries
 *   ULMK_CONFIG_SIM_EXIT          board can stop the simulator it runs on
 *   ULMK_ARCH_HAS_CACHE           board owns cache range maintenance
 *   !ULMK_ARCH_HAVE_CLINT         board owns the tick timer
 */

#ifndef UL_BOARD_H
#define UL_BOARD_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <ulmk/config.h>
#include <arch_config.h>

#if ULMK_CONFIG_BOARD_IRQ_CTRL
/*
 * Connect @srpn to the upstream source the board assigned to it.  Called from
 * the bind path, before the source is enabled, so a line is routed when its
 * driver claims it rather than all of them up front from board init.
 */
void ulmk_board_irq_connect(uint8_t srpn);

/* Release the routing established by ulmk_board_irq_connect(). */
void ulmk_board_irq_disconnect(uint8_t srpn);
#endif

#if ULMK_CONFIG_BOARD_IRQ_CLAIM
/*
 * Offered every interrupt before the generic dispatch path.  Return true if
 * the board fully handled it (the tick, typically), false to let the kernel
 * dispatch and acknowledge it as a driver binding.
 */
bool ulmk_board_irq_claim(uint32_t irq);
#endif

#if ULMK_CONFIG_BOARD_PMP_EXTRA
/*
 * Add board-specific memory-protection entries.  Called whenever the kernel
 * rebuilds the protection state, so it must be idempotent.
 */
void ulmk_board_pmp_extra(void);
#endif

#if ULMK_CONFIG_SIM_EXIT
/*
 * Stop the simulator and hand it @code as the process exit status.
 *
 * Unlike the hooks above this one runs in userspace, at driver privilege, and
 * so lives in the board archive rather than the kernel's: a demo that has said
 * what it came to say calls it instead of falling through to the idle thread,
 * which on a simulator means hanging until something outside kills it.  Real
 * silicon has nowhere to exit to and leaves the feature off.
 */
__attribute__((noreturn)) void ulmk_board_sim_exit(int code);
#endif

/*
 * Not here yet: TriCore's ulmk_board_cpu_endinit_clear/set.  They sit on the
 * boot path of a safety part that cannot be exercised on the bench right now,
 * and moving them would trade a weak symbol that defaults to working for a
 * config gate that defaults to a dead boot.  They stay weak in
 * arch/tricore/arch.c until that board can be tested.
 */

#if ULMK_ARCH_HAS_CACHE
/*
 * Range maintenance: the ISA has no cache-op instructions here, so the SoC
 * owns the recipe.  Both run in the kernel's privileged context.
 */
void ulmk_board_dcache_clean(void *addr, size_t len);
void ulmk_board_dcache_invalidate(void *addr, size_t len);
#endif

#if !ULMK_ARCH_HAVE_CLINT
/* Tick timer, when the architectural one is absent. */
void ulmk_board_tick_init(uint32_t tick_hz);
void ulmk_board_tick_ack(void);
#endif

#if ULMK_ARCH_HAVE_BOARD_CPU_START
/*
 * Release @cpu_id so it begins executing at @entry (typically _start → park).
 * Called from ulmk_arch_start_secondary() when there is no CLINT MSIP wake.
 */
void ulmk_board_cpu_start(uint32_t cpu_id, void (*entry)(void));
#endif

#if ULMK_ARCH_HAVE_BOARD_IPI
/* Soft IPI: arm on this CPU, raise on @cpu_id, clear the local pending bit. */
void ulmk_board_ipi_arm_self(void);
void ulmk_board_ipi_send(uint32_t cpu_id);
void ulmk_board_ipi_clear_self(void);
#endif

#endif /* UL_BOARD_H */
