/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * PWM zstreamer node driver tests.
 *
 * Tests the pwm-sink driver using the fake PWM driver on native_sim.
 * The pipeline is: sine-src (2 Hz, 1 byte/buf) → pwm-sink.
 * The fake PWM's FFF instrumentation lets us inspect every duty-cycle
 * update and verify the sine→PWM mapping.
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/pwm/pwm_fake.h>
#include <zephyr/fff.h>
#include <zephyr/net_buf.h>
#include <zephyr/ztest.h>

#include <zstreamer/node.h>
#include <zstreamer/source.h>

#include <zstreamer_test/helpers.h>

DEFINE_FFF_GLOBALS;

#define GRAPH_NODE DT_NODELABEL(pwm_streaming_graph)
#define SRC_NODE   DT_NODELABEL(sine_source)
#define SINK_NODE  DT_NODELABEL(pwm_sinker)

static const struct device *graph_dev = DEVICE_DT_GET(GRAPH_NODE);
static const struct device *src_dev = DEVICE_DT_GET(SRC_NODE);
static const struct device *sink_dev = DEVICE_DT_GET(SINK_NODE);

/*
 * Expected PWM period in cycles.
 * fake-pwm frequency = 10 MHz, period from DTS = 20 ms (20000000 ns).
 * period_cycles = 20000000 * 10000000 / 1000000000 = 200000
 */
#define EXPECTED_PERIOD_CYCLES 200000

/* ------------------------------------------------------------------ */
/* Recording FFF custom fake                                           */
/* ------------------------------------------------------------------ */

#define MAX_RECORDED 512

static uint32_t recorded_pulses[MAX_RECORDED];
static uint32_t recorded_periods[MAX_RECORDED];
static uint32_t recorded_count;

static int recording_fake(const struct device *dev, uint32_t ch, uint32_t period, uint32_t pulse,
			  pwm_flags_t flags)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(ch);
	ARG_UNUSED(flags);

	if (recorded_count < MAX_RECORDED) {
		recorded_periods[recorded_count] = period;
		recorded_pulses[recorded_count] = pulse;
	}
	recorded_count++;

	return 0;
}

static void reset_recording(void)
{
	recorded_count = 0;
	memset(recorded_pulses, 0, sizeof(recorded_pulses));
	memset(recorded_periods, 0, sizeof(recorded_periods));
}

/* ------------------------------------------------------------------ */
/* Fixture                                                             */
/* ------------------------------------------------------------------ */

static void cleanup(void *fixture)
{
	ARG_UNUSED(fixture);

	zstreamer_source_stop(src_dev);
	/* Let the sink thread drain remaining buffers. */
	k_msleep(50);
}

ZTEST_SUITE(zstreamer_node_pwm, NULL, NULL, cleanup, cleanup, NULL);

/* ------------------------------------------------------------------ */
/* Basic tests                                                         */
/* ------------------------------------------------------------------ */

ZTEST(zstreamer_node_pwm, test_devices_ready)
{
	zassert_true(device_is_ready(graph_dev), "graph not ready");
	zassert_true(device_is_ready(src_dev), "src not ready");
	zassert_true(device_is_ready(sink_dev), "sink not ready");
}

ZTEST(zstreamer_node_pwm, test_start_stop)
{
	int ret;

	ret = zstreamer_source_start(src_dev);
	zassert_equal(ret, 0, "src start failed: %d", ret);

	ret = zstreamer_source_start(src_dev);
	zassert_equal(ret, -EALREADY, "double start: %d", ret);

	ret = zstreamer_source_stop(src_dev);
	zassert_equal(ret, 0, "src stop failed: %d", ret);

	ret = zstreamer_source_stop(src_dev);
	zassert_equal(ret, -EALREADY, "double stop: %d", ret);
}

ZTEST(zstreamer_node_pwm, test_buf_alloc)
{
	struct net_buf *buf;

	buf = zstreamer_node_alloc_buf(src_dev, K_MSEC(100));
	zassert_not_null(buf, "buf alloc failed");
	zassert_true(net_buf_tailroom(buf) > 0, "no tailroom");
	net_buf_unref(buf);
}

/* ------------------------------------------------------------------ */
/* PWM output verification                                             */
/* ------------------------------------------------------------------ */

ZTEST(zstreamer_node_pwm, test_pwm_called)
{
	int ret;

	reset_recording();
	fake_pwm_set_cycles_fake.custom_fake = recording_fake;

	ret = zstreamer_source_start(src_dev);
	zassert_equal(ret, 0);

	k_msleep(500);

	ret = zstreamer_source_stop(src_dev);
	zassert_equal(ret, 0);

	zassert_true(recorded_count > 0, "PWM was never called");
}

ZTEST(zstreamer_node_pwm, test_period_cycles_constant)
{
	int ret;

	reset_recording();
	fake_pwm_set_cycles_fake.custom_fake = recording_fake;

	ret = zstreamer_source_start(src_dev);
	zassert_equal(ret, 0);

	k_msleep(500);

	ret = zstreamer_source_stop(src_dev);
	zassert_equal(ret, 0);

	zassert_true(recorded_count > 5, "too few samples: %u", recorded_count);

	for (uint32_t i = 0; i < MIN(recorded_count, MAX_RECORDED); i++) {
		zassert_equal(recorded_periods[i], EXPECTED_PERIOD_CYCLES,
			      "sample %u: period %u != %u", i, recorded_periods[i],
			      EXPECTED_PERIOD_CYCLES);
	}
}

ZTEST(zstreamer_node_pwm, test_reaches_peak)
{
	/*
	 * With a 2 Hz sine, a full cycle is 500ms. Over 1000ms we should
	 * see the peak (100% duty) at least once.
	 */
	int ret;
	uint32_t max_pulse = 0;

	reset_recording();
	fake_pwm_set_cycles_fake.custom_fake = recording_fake;

	ret = zstreamer_source_start(src_dev);
	zassert_equal(ret, 0);

	k_msleep(1000);

	ret = zstreamer_source_stop(src_dev);
	zassert_equal(ret, 0);

	for (uint32_t i = 0; i < MIN(recorded_count, MAX_RECORDED); i++) {
		if (recorded_pulses[i] > max_pulse) {
			max_pulse = recorded_pulses[i];
		}
	}

	uint32_t max_pct = max_pulse * 100 / EXPECTED_PERIOD_CYCLES;

	zassert_true(max_pct >= 95, "peak duty only %u%% (pulse=%u)", max_pct, max_pulse);
}

ZTEST(zstreamer_node_pwm, test_reaches_trough)
{
	/*
	 * Over a full cycle the sine trough (~0% duty) should appear.
	 */
	int ret;
	uint32_t min_pulse = UINT32_MAX;

	reset_recording();
	fake_pwm_set_cycles_fake.custom_fake = recording_fake;

	ret = zstreamer_source_start(src_dev);
	zassert_equal(ret, 0);

	k_msleep(1000);

	ret = zstreamer_source_stop(src_dev);
	zassert_equal(ret, 0);

	for (uint32_t i = 0; i < MIN(recorded_count, MAX_RECORDED); i++) {
		if (recorded_pulses[i] < min_pulse) {
			min_pulse = recorded_pulses[i];
		}
	}

	uint32_t min_pct = min_pulse * 100 / EXPECTED_PERIOD_CYCLES;

	zassert_true(min_pct <= 5, "trough duty %u%% (pulse=%u)", min_pct, min_pulse);
}

ZTEST(zstreamer_node_pwm, test_duty_range)
{
	/*
	 * Over two full cycles, the duty should span from near-0% to near-100%.
	 */
	int ret;
	uint32_t min_pulse = UINT32_MAX;
	uint32_t max_pulse = 0;

	reset_recording();
	fake_pwm_set_cycles_fake.custom_fake = recording_fake;

	ret = zstreamer_source_start(src_dev);
	zassert_equal(ret, 0);

	k_msleep(1500);

	ret = zstreamer_source_stop(src_dev);
	zassert_equal(ret, 0);

	for (uint32_t i = 0; i < MIN(recorded_count, MAX_RECORDED); i++) {
		if (recorded_pulses[i] < min_pulse) {
			min_pulse = recorded_pulses[i];
		}
		if (recorded_pulses[i] > max_pulse) {
			max_pulse = recorded_pulses[i];
		}
	}

	uint32_t range = max_pulse - min_pulse;
	uint32_t range_pct = range * 100 / EXPECTED_PERIOD_CYCLES;

	zassert_true(range_pct >= 90, "duty range only %u%% (min=%u max=%u)", range_pct, min_pulse,
		     max_pulse);
}

ZTEST(zstreamer_node_pwm, test_sine_oscillation)
{
	/*
	 * Verify the output oscillates: there should be at least one
	 * transition from above-midpoint to below-midpoint (or vice versa)
	 * within one cycle.
	 */
	int ret;
	uint32_t midpoint = EXPECTED_PERIOD_CYCLES / 2;
	uint32_t crossings = 0;

	reset_recording();
	fake_pwm_set_cycles_fake.custom_fake = recording_fake;

	ret = zstreamer_source_start(src_dev);
	zassert_equal(ret, 0);

	k_msleep(1000);

	ret = zstreamer_source_stop(src_dev);
	zassert_equal(ret, 0);

	uint32_t n = MIN(recorded_count, MAX_RECORDED);

	for (uint32_t i = 1; i < n; i++) {
		bool prev_above = recorded_pulses[i - 1] > midpoint;
		bool curr_above = recorded_pulses[i] > midpoint;

		if (prev_above != curr_above) {
			crossings++;
		}
	}

	/* 2 Hz over 1 second = 2 cycles, each crosses midpoint 4 times. */
	zassert_true(crossings >= 4, "only %u midpoint crossings (expected >= 4)", crossings);
}

ZTEST(zstreamer_node_pwm, test_restart_cycle)
{
	for (int cycle = 0; cycle < 5; cycle++) {
		int ret;

		reset_recording();
		fake_pwm_set_cycles_fake.custom_fake = recording_fake;

		ret = zstreamer_source_start(src_dev);
		zassert_equal(ret, 0, "cycle %d start", cycle);

		k_msleep(300);

		ret = zstreamer_source_stop(src_dev);
		zassert_equal(ret, 0, "cycle %d stop", cycle);

		zassert_true(recorded_count > 0, "cycle %d: no PWM calls", cycle);

		k_msleep(50);
	}
}

ZTEST(zstreamer_node_pwm, test_pool_free)
{
	int ret;

	ret = zstreamer_source_start(src_dev);
	zassert_equal(ret, 0);

	k_msleep(300);

	ret = zstreamer_source_stop(src_dev);
	zassert_equal(ret, 0);

	assert_pool_free(graph_dev);
}
