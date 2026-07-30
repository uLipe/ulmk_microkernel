/* SPDX-License-Identifier: MIT */
/*
 * Host unit tests for the non-cascading timing wheel.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define UL_UNIT_TEST 1

#include <ulmk/microkernel.h>
#include <kernel/include/ulmk_timer.h>

static int tests_run;
static int tests_failed;

#define CHECK(cond, msg) \
	do { \
		tests_run++; \
		if (!(cond)) { \
			tests_failed++; \
			printf("  FAIL [line %d] %s\n", __LINE__, (msg)); \
		} else { \
			printf("  PASS %s\n", (msg)); \
		} \
	} while (0)

static int g_fire_count;
static struct ulmk_timeout *g_last_fired;

static void fire_cb(struct ulmk_timeout *to)
{
	g_fire_count++;
	g_last_fired = to;
}

static void advance(uint32_t n)
{
	uint32_t i;

	for (i = 0u; i < n; i++)
		ulmk_timer_tick();
}

static void reset(void)
{
	g_fire_count = 0;
	g_last_fired = NULL;
	ulmk_timer_init();
}

static void test_add_fire_l0(void)
{
	struct ulmk_timeout to;

	reset();
	sys_dnode_init(&to.node);
	to.cb = fire_cb;
	CHECK(ulmk_timer_add(&to, 5u) == ULMK_OK, "add 5 ticks");
	advance(4u);
	CHECK(g_fire_count == 0, "not yet at t=4");
	advance(1u);
	CHECK(g_fire_count == 1, "fires at t=5");
	CHECK(g_last_fired == &to, "callback got correct object");
}

static void test_cancel(void)
{
	struct ulmk_timeout to;

	reset();
	sys_dnode_init(&to.node);
	to.cb = fire_cb;
	CHECK(ulmk_timer_add(&to, 10u) == ULMK_OK, "add for cancel");
	CHECK(ulmk_timer_cancel(&to) == true, "cancel returns true");
	advance(20u);
	CHECK(g_fire_count == 0, "cancelled timer never fires");
	CHECK(ulmk_timer_cancel(&to) == false, "second cancel is false");
}

static void test_overflow_einval(void)
{
	struct ulmk_timeout to;

	reset();
	sys_dnode_init(&to.node);
	to.cb = fire_cb;
	CHECK(ulmk_timer_add(&to, ULMK_TIMER_TIMEOUT_MAX + 1u) == ULMK_EINVAL,
	      "delta > MAX → EINVAL");
	CHECK(ulmk_timer_add(&to, ULMK_TIMER_TIMEOUT_MAX) == ULMK_OK,
	      "delta == MAX accepted");
	CHECK(ulmk_timer_cancel(&to) == true, "cleanup max timer");
}

static void test_level1_boundary(void)
{
	struct ulmk_timeout to;

	/*
	 * Delta 70 lands in level 1 (gran=8).  Non-cascading expiry rounds
	 * up to the next multiple of 8 → fires at clk where L1 bucket hits,
	 * never before the requested time.
	 */
	reset();
	sys_dnode_init(&to.node);
	to.cb = fire_cb;
	CHECK(ulmk_timer_add(&to, 70u) == ULMK_OK, "add L1 delta=70");
	advance(69u);
	CHECK(g_fire_count == 0, "no early fire before 70");
	advance(16u); /* allow slack within L1 gran */
	CHECK(g_fire_count == 1, "fires within L1 slack window");
}

static void test_batch_same_bucket(void)
{
	struct ulmk_timeout a, b, c;

	reset();
	sys_dnode_init(&a.node);
	sys_dnode_init(&b.node);
	sys_dnode_init(&c.node);
	a.cb = b.cb = c.cb = fire_cb;
	CHECK(ulmk_timer_add(&a, 3u) == ULMK_OK, "batch a");
	CHECK(ulmk_timer_add(&b, 3u) == ULMK_OK, "batch b");
	CHECK(ulmk_timer_add(&c, 3u) == ULMK_OK, "batch c");
	advance(3u);
	CHECK(g_fire_count == 3, "all three fire in same bucket");
}

static void test_zero_becomes_one(void)
{
	struct ulmk_timeout to;

	reset();
	sys_dnode_init(&to.node);
	to.cb = fire_cb;
	CHECK(ulmk_timer_add(&to, 0u) == ULMK_OK, "delta 0 accepted as 1");
	advance(1u);
	CHECK(g_fire_count == 1, "fires after one tick");
}

/*
 * Regression: HIL SPI-on-ESP32-P4 hung in ulmk_timer_tick with IRQs
 * masked.  Root cause — expire_list used FOR_EACH_NODE_SAFE; a callback
 * that cancels a peer still on the pending list left "next" self-linked,
 * so the walk never terminated.  Repro values: two timers, same bucket
 * (delta=3), first cb cancels the second.
 */
static struct ulmk_timeout *g_peer;

static void cancel_peer_cb(struct ulmk_timeout *to)
{
	(void)to;
	g_fire_count++;
	if (g_peer)
		(void)ulmk_timer_cancel(g_peer);
}

static void test_cancel_peer_same_bucket(void)
{
	struct ulmk_timeout a, b;

	reset();
	sys_dnode_init(&a.node);
	sys_dnode_init(&b.node);
	a.cb = cancel_peer_cb;
	b.cb = fire_cb;
	g_peer = &b;
	CHECK(ulmk_timer_add(&a, 3u) == ULMK_OK, "peer-cancel a");
	CHECK(ulmk_timer_add(&b, 3u) == ULMK_OK, "peer-cancel b");
	advance(3u);
	CHECK(g_fire_count == 1, "only a fires; b cancelled in-cb");
	CHECK(!sys_dnode_is_linked(&b.node), "b left unlinked");
	g_peer = NULL;
}

/* Re-add while still linked must detach first (no bucket corruption). */
static void test_readd_while_linked(void)
{
	struct ulmk_timeout to;

	reset();
	sys_dnode_init(&to.node);
	to.cb = fire_cb;
	CHECK(ulmk_timer_add(&to, 10u) == ULMK_OK, "first add");
	CHECK(ulmk_timer_add(&to, 3u) == ULMK_OK, "re-add shorter");
	advance(3u);
	CHECK(g_fire_count == 1, "fires at new expiry");
	advance(20u);
	CHECK(g_fire_count == 1, "old slot did not double-fire");
}

int main(void)
{
	printf("timer_unit:\n");
	test_add_fire_l0();
	test_cancel();
	test_overflow_einval();
	test_level1_boundary();
	test_batch_same_bucket();
	test_zero_becomes_one();
	test_cancel_peer_same_bucket();
	test_readd_while_linked();
	printf("timer_unit: %d run, %d failed\n", tests_run, tests_failed);
	return tests_failed ? 1 : 0;
}
