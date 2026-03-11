/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/ztest.h>

#include <zstreamer/graph.h>
#include <zstreamer/node.h>
#include <zstreamer_test/helpers.h>

/* ------------------------------------------------------------------ */
/* Graph device — provides the buffer pool                             */
/* ------------------------------------------------------------------ */

#define GRAPH_NODE  DT_NODELABEL(streaming_graph)
#define BUFFER_SIZE DT_PROP(GRAPH_NODE, buffer_size)

static const struct device *graph_dev = DEVICE_DT_GET(GRAPH_NODE);

/* ------------------------------------------------------------------ */
/* Fake child devices for direct distribute testing                    */
/* ------------------------------------------------------------------ */

struct fake_child {
	struct zstreamer_node_data data;
	struct zstreamer_node_config cfg;
	struct device_state state;
	struct device dev;
};

static void fake_child_init(struct fake_child *child, const char *name, bool readonly)
{
	memset(child, 0, sizeof(*child));
	k_fifo_init(&child->data.fifo);
	child->cfg.readonly = readonly;
	child->state.initialized = true;
	child->dev.name = name;
	child->dev.config = &child->cfg;
	child->dev.data = &child->data;
	child->dev.state = &child->state;
}

static struct net_buf *fake_child_get(struct fake_child *child)
{
	return k_fifo_get(&child->data.fifo, K_NO_WAIT);
}

/* A fake "caller" device — distribute only uses dev->name for logging. */
static struct device_state caller_state;
static struct device caller_dev = {
	.name = "test-caller",
	.state = &caller_state,
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static struct net_buf *alloc_test_buf(void)
{
	const struct zstreamer_graph_config *cfg = graph_dev->config;
	struct net_buf *buf = net_buf_alloc_fixed(cfg->pool, K_NO_WAIT);

	zassert_not_null(buf, "pool alloc failed");
	/* Fill with known pattern. */
	memset(net_buf_add(buf, BUFFER_SIZE), 0xAB, BUFFER_SIZE);
	return buf;
}

/* Drain and unref any leftover buffers from a fake child's fifo. */
static void fake_child_drain(struct fake_child *child)
{
	struct net_buf *buf;

	while ((buf = k_fifo_get(&child->data.fifo, K_NO_WAIT)) != NULL) {
		net_buf_unref(buf);
	}
}

/* ------------------------------------------------------------------ */
/* Fixtures                                                            */
/* ------------------------------------------------------------------ */

static struct fake_child child_a;
static struct fake_child child_b;

static void distribute_cleanup(void *fixture)
{
	ARG_UNUSED(fixture);

	fake_child_drain(&child_a);
	fake_child_drain(&child_b);
	/* Let any pending refs settle, then verify pool is clean. */
	k_msleep(10);
}

ZTEST_SUITE(zstreamer_distribute, NULL, NULL, NULL, distribute_cleanup, NULL);

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

/**
 * Single readwrite child — the "last child with ref==1" optimisation
 * means the buffer is passed by ref, never cloned.
 */
ZTEST(zstreamer_distribute, test_single_child_no_clone)
{
	fake_child_init(&child_a, "rw-only", false);

	struct net_buf *orig = alloc_test_buf();
	const struct device *children[] = {&child_a.dev};

	zstreamer_node_distribute(&caller_dev, orig, children, 1);

	struct net_buf *got = fake_child_get(&child_a);

	zassert_not_null(got, "child received nothing");
	zassert_equal_ptr(got, orig,
			  "single child should get ref (same ptr), "
			  "not clone");

	net_buf_unref(got);
	assert_pool_free(graph_dev);
}

/**
 * Two readonly children — both get refs to the same buffer, no clones.
 */
ZTEST(zstreamer_distribute, test_two_readonly_no_clone)
{
	fake_child_init(&child_a, "ro-a", true);
	fake_child_init(&child_b, "ro-b", true);

	struct net_buf *orig = alloc_test_buf();
	const struct device *children[] = {&child_a.dev, &child_b.dev};

	zstreamer_node_distribute(&caller_dev, orig, children, 2);

	struct net_buf *got_a = fake_child_get(&child_a);
	struct net_buf *got_b = fake_child_get(&child_b);

	zassert_not_null(got_a, "child_a received nothing");
	zassert_not_null(got_b, "child_b received nothing");
	zassert_equal_ptr(got_a, got_b,
			  "both readonly children should get "
			  "same buffer (ref, not clone)");
	zassert_equal_ptr(got_a, orig,
			  "readonly children should get the "
			  "original buffer");

	net_buf_unref(got_a);
	net_buf_unref(got_b);
	assert_pool_free(graph_dev);
}

/**
 * Two readwrite children — first gets a clone, second gets the original
 * via the last-child optimisation.
 */
ZTEST(zstreamer_distribute, test_two_readwrite_clones_first)
{
	fake_child_init(&child_a, "rw-a", false);
	fake_child_init(&child_b, "rw-b", false);

	struct net_buf *orig = alloc_test_buf();
	const struct device *children[] = {&child_a.dev, &child_b.dev};

	zstreamer_node_distribute(&caller_dev, orig, children, 2);

	struct net_buf *got_a = fake_child_get(&child_a);
	struct net_buf *got_b = fake_child_get(&child_b);

	zassert_not_null(got_a, "child_a received nothing");
	zassert_not_null(got_b, "child_b received nothing");

	/* First child must get a clone (different pointer). */
	zassert_not_equal((uintptr_t)got_a, (uintptr_t)orig,
			  "first rw child should get a clone, not the original");

	/* Last child gets the original via ref (same pointer). */
	zassert_equal_ptr(got_b, orig, "last rw child should get original buffer (ref)");

	net_buf_unref(got_a);
	net_buf_unref(got_b);
	assert_pool_free(graph_dev);
}

/**
 * After cloning, the clone must contain identical data.
 */
ZTEST(zstreamer_distribute, test_clone_data_integrity)
{
	fake_child_init(&child_a, "rw-a", false);
	fake_child_init(&child_b, "rw-b", false);

	struct net_buf *orig = alloc_test_buf();
	const struct device *children[] = {&child_a.dev, &child_b.dev};

	zstreamer_node_distribute(&caller_dev, orig, children, 2);

	struct net_buf *clone = fake_child_get(&child_a);
	struct net_buf *ref = fake_child_get(&child_b);

	zassert_not_null(clone, "child_a received nothing");
	zassert_not_null(ref, "child_b received nothing");

	/* Clone must have same length and data as original. */
	zassert_equal(clone->len, BUFFER_SIZE, "clone len %u != %u", clone->len, BUFFER_SIZE);
	zassert_equal(ref->len, BUFFER_SIZE, "ref len %u != %u", ref->len, BUFFER_SIZE);
	zassert_mem_equal(clone->data, ref->data, BUFFER_SIZE, "clone data differs from original");

	/* Verify the known pattern. */
	for (size_t i = 0; i < BUFFER_SIZE; i++) {
		zassert_equal(clone->data[i], 0xAB, "clone byte %zu = 0x%02x", i, clone->data[i]);
	}

	net_buf_unref(clone);
	net_buf_unref(ref);
	assert_pool_free(graph_dev);
}

/**
 * Mixed: first child readonly, second readwrite.
 * The readonly child gets a ref; the readwrite child must get a clone
 * because the ref count was bumped by the first child.
 */
ZTEST(zstreamer_distribute, test_readonly_then_readwrite)
{
	fake_child_init(&child_a, "ro", true);
	fake_child_init(&child_b, "rw", false);

	struct net_buf *orig = alloc_test_buf();
	const struct device *children[] = {&child_a.dev, &child_b.dev};

	zstreamer_node_distribute(&caller_dev, orig, children, 2);

	struct net_buf *got_a = fake_child_get(&child_a);
	struct net_buf *got_b = fake_child_get(&child_b);

	zassert_not_null(got_a, "readonly child received nothing");
	zassert_not_null(got_b, "readwrite child received nothing");

	/* Readonly child gets the original (ref). */
	zassert_equal_ptr(got_a, orig, "readonly child should get original (ref)");

	/* Readwrite child must get a clone because ref count > 1
	 * after the readonly child's ref. */
	zassert_not_equal((uintptr_t)got_b, (uintptr_t)orig,
			  "rw child after ro should get clone, not original");

	/* Data still matches. */
	zassert_mem_equal(got_b->data, got_a->data, BUFFER_SIZE, "clone data differs from ref");

	net_buf_unref(got_a);
	net_buf_unref(got_b);
	assert_pool_free(graph_dev);
}

/**
 * Mixed: first child readwrite, second readonly.
 * The readwrite child gets a clone; the readonly child gets the original
 * via ref (readonly flag bypasses the ref-count check).
 */
ZTEST(zstreamer_distribute, test_readwrite_then_readonly)
{
	fake_child_init(&child_a, "rw", false);
	fake_child_init(&child_b, "ro", true);

	struct net_buf *orig = alloc_test_buf();
	const struct device *children[] = {&child_a.dev, &child_b.dev};

	zstreamer_node_distribute(&caller_dev, orig, children, 2);

	struct net_buf *got_a = fake_child_get(&child_a);
	struct net_buf *got_b = fake_child_get(&child_b);

	zassert_not_null(got_a, "readwrite child received nothing");
	zassert_not_null(got_b, "readonly child received nothing");

	/* First rw child gets clone. */
	zassert_not_equal((uintptr_t)got_a, (uintptr_t)orig, "first rw child should get a clone");

	/* Second ro child gets original via ref. */
	zassert_equal_ptr(got_b, orig, "readonly child should get original (ref)");

	net_buf_unref(got_a);
	net_buf_unref(got_b);
	assert_pool_free(graph_dev);
}

/**
 * Two readwrite children, but the incoming buffer already has ref == 2
 * (someone else holds a reference).  The last-child optimisation cannot
 * fire because handing the original away would steal it from the other
 * owner.  Both children must therefore receive clones.
 */
ZTEST(zstreamer_distribute, test_two_readwrite_extra_ref)
{
	fake_child_init(&child_a, "rw-a", false);
	fake_child_init(&child_b, "rw-b", false);

	struct net_buf *orig = alloc_test_buf();

	/* Simulate an external owner by bumping the ref count. */
	struct net_buf *orig_ref = net_buf_ref(orig);
	zassert_not_null(orig_ref, "failed the to add ref to original buffer");
	zassert_equal(orig->ref, 2, "expected ref == 2 before distribute");

	const struct device *children[] = {&child_a.dev, &child_b.dev};

	zstreamer_node_distribute(&caller_dev, orig, children, 2);

	struct net_buf *got_a = fake_child_get(&child_a);
	struct net_buf *got_b = fake_child_get(&child_b);

	zassert_not_null(got_a, "child_a received nothing");
	zassert_not_null(got_b, "child_b received nothing");

	/* Both must be clones — neither pointer equals orig. */
	zassert_not_equal((uintptr_t)got_a, (uintptr_t)orig,
			  "first child should get clone when buf ref > 1");
	zassert_not_equal((uintptr_t)got_b, (uintptr_t)orig,
			  "last child should also get clone when buf ref > 1");

	/* The two clones are distinct buffers. */
	zassert_not_equal((uintptr_t)got_a, (uintptr_t)got_b,
			  "each child should get its own clone");

	/* Data integrity. */
	zassert_mem_equal(got_a->data, got_b->data, BUFFER_SIZE, "clone data mismatch");
	zassert_equal(got_a->data[0], 0xAB, "unexpected clone content");

	/* Release the external ref we added, plus the two clones. */
	net_buf_unref(orig);
	net_buf_unref(got_a);
	net_buf_unref(got_b);
	assert_pool_free(graph_dev);
}

/**
 * After distribute + unref of all received buffers, the pool must be
 * completely free — no leaks regardless of clone/ref mix.
 */
ZTEST(zstreamer_distribute, test_pool_no_leak)
{
	fake_child_init(&child_a, "rw-a", false);
	fake_child_init(&child_b, "rw-b", false);

	/* Run distribute multiple times to stress ref-counting. */
	for (int i = 0; i < 8; i++) {
		struct net_buf *buf = alloc_test_buf();
		const struct device *children[] = {&child_a.dev, &child_b.dev};

		zstreamer_node_distribute(&caller_dev, buf, children, 2);

		struct net_buf *a = fake_child_get(&child_a);
		struct net_buf *b = fake_child_get(&child_b);

		zassert_not_null(a, "iter %d: child_a empty", i);
		zassert_not_null(b, "iter %d: child_b empty", i);

		net_buf_unref(a);
		net_buf_unref(b);
	}

	assert_pool_free(graph_dev);
}
