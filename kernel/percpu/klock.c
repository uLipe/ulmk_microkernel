/* SPDX-License-Identifier: MIT */

#include <kernel/include/ulmk_klock.h>

ulmk_spinlock_t g_ulmk_lock_thread = ULMK_SPINLOCK_INIT;
ulmk_spinlock_t g_ulmk_lock_ipc    = ULMK_SPINLOCK_INIT;
ulmk_spinlock_t g_ulmk_lock_irq    = ULMK_SPINLOCK_INIT;
ulmk_spinlock_t g_ulmk_lock_timer  = ULMK_SPINLOCK_INIT;
ulmk_spinlock_t g_ulmk_lock_area   = ULMK_SPINLOCK_INIT;
ulmk_spinlock_t g_ulmk_lock_mem    = ULMK_SPINLOCK_INIT;
