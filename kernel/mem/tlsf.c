/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2024-2026 Felipe Neves
 *
 * Minimal TLSF (Two-Level Segregated Fit) heap — kernel/mem/tlsf.c
 *
 * O(1) alloc/free.  Supports pools up to ~1 GB.  Every allocation is
 * aligned to TLSF_HDR (64 bytes = ULMK_ARCH_REGION_ALIGN) so it can be
 * used as an MPU region without re-alignment.
 *
 * Block layout (header = 64 bytes, payload follows immediately):
 *
 *   offset  0: struct blk *prev_phys  — physical predecessor (NULL = first)
 *   offset  4: uint32_t   size        — payload bytes (excl. header), mult. of 64
 *   offset  8: uint32_t   flags       — BLKF_FREE (0x1)
 *   offset 12: struct blk *next_free  — segregated free-list link (free only)
 *   offset 16: struct blk *prev_free  — segregated free-list link (free only)
 *   offset 20: uint8_t    _pad[44]    — pad to 64 bytes
 *
 * A sentinel block (size=0, flags=0) is placed at the end of the pool to
 * prevent coalescing past the pool boundary.
 *
 * TLSF parameters:
 *   FL_BASE   = log2(TLSF_HDR) = 6     (minimum FL index)
 *   SL_BITS   = 2                       (4 sub-classes per FL level)
 *   FL_COUNT  = 24                      (pools up to 64×2^23 ≈ 512 MB)
 */

#include <stddef.h>
#include <stdint.h>
#include <ulmk_arch.h>
#include <kernel/include/ulmk_klock.h>
#include <string.h>
#include <kernel/include/ulmk_mem_internal.h>

/* ─── constants ──────────────────────────────────────────────────────────── */

#define TLSF_HDR   64u
#define BLKF_FREE  1u

#define SL_BITS    2u
#define SL_COUNT   (1u << SL_BITS)   /* 4 */
#define FL_BASE    6u                 /* log2(TLSF_HDR) */
#define FL_COUNT   24u

/* ─── block header ───────────────────────────────────────────────────────── */

/*
 * On 32-bit targets (embedded): 2 pointers = 8B, 2 uint32 = 8B, 2 pointers = 8B
 * total without pad = 24B → pad = 40B.
 * On 64-bit hosts (unit tests): 2 pointers = 16B, 2 uint32 = 8B, 2 pointers = 16B
 * total without pad = 40B → pad = 24B.
 * Use a helper to compute the right padding at compile time.
 */
struct _blk_noPad {
	struct _blk_noPad *prev_phys;
	uint32_t            size;
	uint32_t            flags;
	struct _blk_noPad  *next_free;
	struct _blk_noPad  *prev_free;
};
#define _TLSF_PAD ((TLSF_HDR) - sizeof(struct _blk_noPad))

typedef struct blk {
	struct blk *prev_phys;
	uint32_t    size;
	uint32_t    flags;
	struct blk *next_free;
	struct blk *prev_free;
	uint8_t     _pad[_TLSF_PAD];
} blk_t;

/* Compile-time check: header must be exactly TLSF_HDR bytes. */
typedef char _blk_size_check[(sizeof(blk_t) == TLSF_HDR) ? 1 : -1];

/* ─── control block (kernel BSS) ─────────────────────────────────────────── */

typedef struct {
	uint32_t fl_bitmap;
	uint32_t sl_bitmap[FL_COUNT];
	blk_t   *free_lists[FL_COUNT][SL_COUNT];
	size_t   free_bytes;
} tlsf_ctrl_t;

static tlsf_ctrl_t ctrl;

/* ─── helpers ─────────────────────────────────────────────────────────────── */

/* floor(log2(x)) — x must be > 0 */
#define FLS(x)  ((uint32_t)(31u - __builtin_clz((uint32_t)(x))))

static inline blk_t *blk_next(const blk_t *b)
{
	return (blk_t *)((uint8_t *)b + TLSF_HDR + b->size);
}

/* Map size → (fl, sl) — floor mapping (for insert). */
static void mapping_insert(uint32_t size, uint32_t *fl_out, uint32_t *sl_out)
{
	uint32_t f = FLS(size);
	uint32_t fl = (f >= FL_BASE) ? (f - FL_BASE) : 0u;

	if (fl >= FL_COUNT)
		fl = FL_COUNT - 1u;
	*fl_out = fl;
	*sl_out = (size >> (f - SL_BITS)) & (SL_COUNT - 1u);
}

/* Map size → (fl, sl) — ceiling mapping (for search: find block at least this big). */
static void mapping_search(uint32_t size, uint32_t *fl_out, uint32_t *sl_out)
{
	uint32_t f = FLS(size);
	uint32_t round;

	if (f >= SL_BITS) {
		round = (1u << (f - SL_BITS)) - 1u;
		size  = (size + round) & ~round;
		f     = FLS(size);
	}

	uint32_t fl = (f >= FL_BASE) ? (f - FL_BASE) : 0u;

	if (fl >= FL_COUNT)
		fl = FL_COUNT - 1u;
	*fl_out = fl;
	*sl_out = (size >> (f - SL_BITS)) & (SL_COUNT - 1u);
}

static void insert_free(blk_t *b)
{
	uint32_t fl, sl;

	mapping_insert(b->size, &fl, &sl);
	b->next_free = ctrl.free_lists[fl][sl];
	b->prev_free = NULL;
	if (ctrl.free_lists[fl][sl])
		ctrl.free_lists[fl][sl]->prev_free = b;
	ctrl.free_lists[fl][sl] = b;
	ctrl.fl_bitmap        |= (1u << fl);
	ctrl.sl_bitmap[fl]    |= (1u << sl);
	b->flags              |= BLKF_FREE;
	ctrl.free_bytes       += b->size;
}

static void remove_free(blk_t *b)
{
	uint32_t fl, sl;

	mapping_insert(b->size, &fl, &sl);
	if (b->prev_free)
		b->prev_free->next_free = b->next_free;
	else
		ctrl.free_lists[fl][sl] = b->next_free;
	if (b->next_free)
		b->next_free->prev_free = b->prev_free;
	if (!ctrl.free_lists[fl][sl]) {
		ctrl.sl_bitmap[fl] &= ~(1u << sl);
		if (!ctrl.sl_bitmap[fl])
			ctrl.fl_bitmap &= ~(1u << fl);
	}
	b->flags       &= ~BLKF_FREE;
	b->next_free    = NULL;
	b->prev_free    = NULL;
	ctrl.free_bytes -= b->size;
}

static blk_t *find_free_block(uint32_t size)
{
	uint32_t fl, sl;
	uint32_t sl_map;
	uint32_t fl_map;

	mapping_search(size, &fl, &sl);

	sl_map = ctrl.sl_bitmap[fl] & (~0u << sl);
	if (sl_map) {
		sl = __builtin_ctz(sl_map);
		return ctrl.free_lists[fl][sl];
	}

	/* No suitable block at this FL level; try a higher level. */
	if (fl + 1u < FL_COUNT) {
		fl_map = ctrl.fl_bitmap & (~0u << (fl + 1u));
		if (fl_map) {
			fl = __builtin_ctz(fl_map);
			sl = __builtin_ctz(ctrl.sl_bitmap[fl]);
			return ctrl.free_lists[fl][sl];
		}
	}
	return NULL;
}

/* ─── public API ─────────────────────────────────────────────────────────── */

void ulmk_heap_init(uintptr_t base, size_t size)
{
	uint8_t *aligned;
	size_t   total;
	blk_t   *first;
	blk_t   *sentinel;
	uintptr_t end = base + size;

	if (size == 0u || base >= end)
		return;

	memset(&ctrl, 0, sizeof(ctrl));

	aligned = (uint8_t *)(((uintptr_t)base + TLSF_HDR - 1u) & ~((uintptr_t)TLSF_HDR - 1u));
	total   = (size_t)(end - (uintptr_t)aligned);

	/* Minimum: one header (64) + one min payload (64) + sentinel header (64). */
	if (total < 3u * TLSF_HDR)
		return;

	first             = (blk_t *)(void *)aligned;
	first->prev_phys  = NULL;
	first->size       = (uint32_t)(total - 2u * TLSF_HDR);
	first->flags      = 0u;
	first->next_free  = NULL;
	first->prev_free  = NULL;

	sentinel            = blk_next(first);
	sentinel->prev_phys = first;
	sentinel->size      = 0u;
	sentinel->flags     = 0u;
	sentinel->next_free = NULL;
	sentinel->prev_free = NULL;

	insert_free(first);
}

void *ulmk_heap_alloc(size_t size)
{
	ulmk_arch_irq_key_t key;
	uint32_t  rounded;
	blk_t    *blk;
	blk_t    *rem;
	blk_t    *nxt;
	void     *ret;

	if (size == 0u)
		return NULL;

	key = ulmk_arch_spin_lock_irqsave(&g_ulmk_lock_mem);
	rounded = (uint32_t)(((size + TLSF_HDR - 1u) / TLSF_HDR) * TLSF_HDR);

	blk = find_free_block(rounded);
	if (!blk) {
		ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_mem, key);
		return NULL;
	}

	remove_free(blk);

	/*
	 * Split if the remainder can hold a full free block
	 * (one header + one minimum payload = 2 × TLSF_HDR).
	 */
	if (blk->size >= rounded + 2u * TLSF_HDR) {
		rem             = (blk_t *)((uint8_t *)blk + TLSF_HDR + rounded);
		rem->prev_phys  = blk;
		rem->size       = blk->size - rounded - TLSF_HDR;
		rem->flags      = 0u;
		rem->next_free  = NULL;
		rem->prev_free  = NULL;

		nxt             = blk_next(rem);
		nxt->prev_phys  = rem;
		blk->size       = rounded;
		insert_free(rem);
	}

	ret = (uint8_t *)blk + TLSF_HDR;
	ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_mem, key);
	return ret;
}

void ulmk_heap_free(void *ptr)
{
	ulmk_arch_irq_key_t key;
	blk_t *blk;
	blk_t *nxt;
	blk_t *prv;

	if (!ptr)
		return;

	key = ulmk_arch_spin_lock_irqsave(&g_ulmk_lock_mem);
	blk = (blk_t *)((uint8_t *)ptr - TLSF_HDR);

	/* Coalesce forward. */
	nxt = blk_next(blk);
	if (nxt->size > 0u && (nxt->flags & BLKF_FREE)) {
		blk_t *after_nxt = blk_next(nxt);

		remove_free(nxt);
		blk->size       += TLSF_HDR + nxt->size;
		after_nxt->prev_phys = blk;
	}

	/* Coalesce backward. */
	prv = blk->prev_phys;
	if (prv && (prv->flags & BLKF_FREE)) {
		blk_t *after_blk = blk_next(blk);

		remove_free(prv);
		prv->size       += TLSF_HDR + blk->size;
		after_blk->prev_phys = prv;
		blk              = prv;
	}

	insert_free(blk);
	ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_mem, key);
}

/*
 * Aligned allocation.  Every TLSF block is 64-byte aligned; alignments ≤ 64
 * are satisfied by the base allocator at no extra cost.
 *
 * For align > TLSF_HDR the payload is carved out of a larger free block: the
 * lead in front of it becomes a free block of its own (so it needs room for
 * a header plus a minimum payload) and the tail past it is split off as in
 * ulmk_heap_alloc().  The result is an ordinary block for ulmk_heap_free().
 */
void *ulmk_heap_aligned_alloc(size_t align, size_t size)
{
	ulmk_arch_irq_key_t key;
	blk_t    *blk;
	blk_t    *nb;
	blk_t    *rem;
	uintptr_t payload;
	uintptr_t aligned;
	uint32_t  rounded;

	if (size == 0u)
		return NULL;
	if (align <= TLSF_HDR)
		return ulmk_heap_alloc(size);
	if (align & (align - 1u))
		return NULL;

	rounded = (uint32_t)(((size + TLSF_HDR - 1u) / TLSF_HDR) * TLSF_HDR);

	key = ulmk_arch_spin_lock_irqsave(&g_ulmk_lock_mem);
	/* Payloads are HDR-aligned, so the lead is at most align + HDR. */
	blk = find_free_block(rounded + (uint32_t)align + TLSF_HDR);
	if (!blk) {
		ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_mem, key);
		return NULL;
	}
	remove_free(blk);

	payload = (uintptr_t)blk + TLSF_HDR;
	aligned = (payload + align - 1u) & ~(uintptr_t)(align - 1u);
	if (aligned != payload && aligned - payload < 2u * TLSF_HDR)
		aligned += align;

	if (aligned != payload) {
		nb            = (blk_t *)(aligned - TLSF_HDR);
		nb->prev_phys = blk;
		nb->size      = blk->size - (uint32_t)(aligned - payload);
		nb->flags     = 0u;
		nb->next_free = NULL;
		nb->prev_free = NULL;
		blk_next(nb)->prev_phys = nb;

		blk->size = (uint32_t)(aligned - payload) - TLSF_HDR;
		insert_free(blk);
		blk = nb;
	}

	if (blk->size >= rounded + 2u * TLSF_HDR) {
		rem            = (blk_t *)((uint8_t *)blk + TLSF_HDR + rounded);
		rem->prev_phys = blk;
		rem->size      = blk->size - rounded - TLSF_HDR;
		rem->flags     = 0u;
		rem->next_free = NULL;
		rem->prev_free = NULL;
		blk_next(rem)->prev_phys = rem;
		blk->size = rounded;
		insert_free(rem);
	}

	ulmk_arch_spin_unlock_irqrestore(&g_ulmk_lock_mem, key);
	return (void *)aligned;
}

size_t ulmk_heap_free_bytes(void)
{
	return ctrl.free_bytes;
}
