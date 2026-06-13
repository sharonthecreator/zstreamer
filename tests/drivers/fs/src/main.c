/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/ztest.h>

#include <zstreamer/fs/fs_sink.h>
#include <zstreamer/node.h>
#include <zstreamer/source.h>
#include <zstreamer_test/helpers.h>

/* ------------------------------------------------------------------ */
/* Pipeline: numgen → fs_sinker  (64-byte bufs, size-threshold=256)   */
/* ------------------------------------------------------------------ */

#define GRAPH_NODE DT_NODELABEL(streaming_graph)
#define SRC_NODE DT_NODELABEL(numgen_source)
#define SINK_NODE DT_NODELABEL(fs_sinker)

#define BUFFER_SIZE DT_PROP(GRAPH_NODE, buffer_size)

/* Delta pipeline: slow numgen (100ms) → fs_sinker_delta (delta-ms=200) */
#define DELTA_SRC_NODE DT_NODELABEL(numgen_delta)
#define DELTA_SINK_NODE DT_NODELABEL(fs_sinker_delta)

static const struct device *graph_dev = DEVICE_DT_GET(GRAPH_NODE);
static const struct device *src_dev = DEVICE_DT_GET(SRC_NODE);
static const struct device *sink_dev = DEVICE_DT_GET(SINK_NODE);
static const struct device *delta_src_dev = DEVICE_DT_GET(DELTA_SRC_NODE);
static const struct device *delta_sink_dev = DEVICE_DT_GET(DELTA_SINK_NODE);

static void delete_prefixed_files(const char *prefix) {
  char path[48];
  struct fs_dirent entry;
  int consecutive_missing = 0;

  for (int i = 0; i < 100 && consecutive_missing < 5; i++) {
    snprintf(path, sizeof(path), "/lfs/%s%05u.bin", prefix, i);
    if (fs_stat(path, &entry) == 0) {
      fs_unlink(path);
      consecutive_missing = 0;
    } else {
      consecutive_missing++;
    }
  }
}

static void cleanup(void *fixture) {
  struct fs_dirent idx_entry;

  ARG_UNUSED(fixture);

  /* Stop both pipelines */
  zstreamer_source_stop(src_dev);
  zstreamer_source_stop(delta_src_dev);
  k_msleep(50);

  /* Close and reset both sinks */
  fs_sink_close(sink_dev);
  fs_sink_close(delta_sink_dev);
  delete_prefixed_files("test");
  delete_prefixed_files("delta");
  if (fs_stat("/lfs/testidx", &idx_entry) == 0) {
    fs_unlink("/lfs/testidx");
  }
  if (fs_stat("/lfs/deltaidx", &idx_entry) == 0) {
    fs_unlink("/lfs/deltaidx");
  }
  fs_sink_reset(sink_dev);
  fs_sink_reset(delta_sink_dev);
}

ZTEST_SUITE(zstreamer_fs, NULL, NULL, cleanup, cleanup, NULL);

ZTEST(zstreamer_fs, test_devices_ready) {
  zassert_true(device_is_ready(graph_dev), "graph not ready");
  zassert_true(device_is_ready(src_dev), "src not ready");
  zassert_true(device_is_ready(sink_dev), "sink not ready");
}

ZTEST(zstreamer_fs, test_writes_data) {
  struct fs_dirent entry;
  int ret;

  ret = zstreamer_source_start(src_dev);
  zassert_equal(ret, 0, "src start: %d", ret);

  k_msleep(200);

  ret = zstreamer_source_stop(src_dev);
  zassert_equal(ret, 0, "src stop: %d", ret);

  ret = fs_stat("/lfs/test00000.bin", &entry);
  zassert_equal(ret, 0, "file not found: %d", ret);
  zassert_true(entry.size > 0, "file empty");

  assert_pool_free(graph_dev);
}

ZTEST(zstreamer_fs, test_data_integrity) {
  struct fs_file_t f;
  uint8_t buf[BUFFER_SIZE];
  ssize_t nread;
  int ret;

  ret = zstreamer_source_start(src_dev);
  zassert_equal(ret, 0, "src start: %d", ret);

  k_msleep(200);

  ret = zstreamer_source_stop(src_dev);
  zassert_equal(ret, 0, "src stop: %d", ret);

  fs_file_t_init(&f);
  ret = fs_open(&f, "/lfs/test00000.bin", FS_O_READ);
  zassert_equal(ret, 0, "open: %d", ret);

  nread = fs_read(&f, buf, sizeof(buf));
  zassert_true(nread > 0, "read failed: %zd", nread);

  /* numgen produces sequential bytes (wrapping at 255) */
  uint8_t expected = buf[0];

  for (ssize_t i = 0; i < nread; i++) {
    zassert_equal(buf[i], expected, "byte %zd: got 0x%02x expected 0x%02x", i,
                  buf[i], expected);
    expected++;
  }

  fs_close(&f);
  assert_pool_free(graph_dev);
}

ZTEST(zstreamer_fs, test_size_rotation) {
  struct fs_dirent entry;
  int ret;

  /* size-threshold=256, bufs=64 bytes -> rotation after 4 bufs */
  ret = zstreamer_source_start(src_dev);
  zassert_equal(ret, 0, "src start: %d", ret);

  /* Run long enough for multiple rotations */
  k_msleep(500);

  ret = zstreamer_source_stop(src_dev);
  zassert_equal(ret, 0, "src stop: %d", ret);

  /* At least 2 files should exist */
  ret = fs_stat("/lfs/test00000.bin", &entry);
  zassert_equal(ret, 0, "file 0 missing: %d", ret);
  zassert_true(entry.size > 0, "file 0 empty");

  ret = fs_stat("/lfs/test00001.bin", &entry);
  zassert_equal(ret, 0, "file 1 missing: %d", ret);
  zassert_true(entry.size > 0, "file 1 empty");

  assert_pool_free(graph_dev);
}

ZTEST(zstreamer_fs, test_file_count_wraparound) {
  struct fs_dirent entry;
  int ret;

  /*
   * file-count=5, size-threshold=256, buf=64 → rotation every 4 bufs.
   * ~1 buf/20ms → 6 rotations in ~500ms, wrapping index past 4 → 0.
   * Run 800ms to be safe.
   */
  ret = zstreamer_source_start(src_dev);
  zassert_equal(ret, 0, "src start: %d", ret);

  k_msleep(800);

  ret = zstreamer_source_stop(src_dev);
  zassert_equal(ret, 0, "src stop: %d", ret);

  /* Files 0-4 should exist (overwritten by wrap) */
  for (int i = 0; i < 5; i++) {
    char path[32];

    snprintf(path, sizeof(path), "/lfs/test%05u.bin", i);
    ret = fs_stat(path, &entry);
    zassert_equal(ret, 0, "file %d missing: %d", i, ret);
    zassert_true(entry.size > 0, "file %d empty", i);
  }

  /* File 5 must NOT exist — index wraps at file-count */
  ret = fs_stat("/lfs/test00005.bin", &entry);
  zassert_not_equal(ret, 0, "file 5 should not exist (wrap failed)");

  assert_pool_free(graph_dev);
}

ZTEST(zstreamer_fs, test_index_persistence) {
  struct fs_dirent entry;
  struct fs_file_t f;
  uint32_t saved_idx;
  ssize_t nread;
  int ret;

  /* Run long enough for a few rotations to trigger save_index */
  ret = zstreamer_source_start(src_dev);
  zassert_equal(ret, 0, "src start: %d", ret);

  k_msleep(500);

  ret = zstreamer_source_stop(src_dev);
  zassert_equal(ret, 0, "src stop: %d", ret);

  fs_sink_close(sink_dev);

  /* .idx file should exist after rotations */
  ret = fs_stat("/lfs/testidx", &entry);
  zassert_equal(ret, 0, ".idx missing: %d", ret);
  zassert_equal(entry.size, sizeof(uint32_t), ".idx wrong size");

  /* Read and validate the persisted index */
  fs_file_t_init(&f);
  ret = fs_open(&f, "/lfs/testidx", FS_O_READ);
  zassert_equal(ret, 0, "open .idx: %d", ret);

  nread = fs_read(&f, &saved_idx, sizeof(saved_idx));
  fs_close(&f);
  zassert_equal(nread, sizeof(saved_idx), "read .idx: %zd", nread);
  zassert_true(saved_idx > 0, "saved index should be > 0");
  zassert_true(saved_idx < 5, "saved index should be < file-count");

  /*
   * The file at saved_idx should exist on disk — it's the file the
   * driver was about to write next (index is bumped before opening).
   * Verify the corresponding file was created.
   */
  char path[32];

  snprintf(path, sizeof(path), "/lfs/test%05u.bin", saved_idx - 1);
  ret = fs_stat(path, &entry);
  zassert_equal(ret, 0, "file at saved_idx-1 missing: %d", ret);

  assert_pool_free(graph_dev);
}

ZTEST(zstreamer_fs, test_invalid_index_resets_to_zero) {
  struct fs_dirent entry;
  struct fs_file_t f;
  int ret;

  /*
   * Write an out-of-range index to .idx.  file-count is 5, so any
   * value >= 5 is invalid.  load_index should clamp it back to 0.
   */
  uint32_t bad_idx = 9999;

  fs_file_t_init(&f);
  ret = fs_open(&f, "/lfs/testidx", FS_O_CREATE | FS_O_WRITE);
  zassert_equal(ret, 0, "create .idx: %d", ret);
  ret = fs_write(&f, &bad_idx, sizeof(bad_idx));
  fs_close(&f);
  zassert_equal(ret, sizeof(bad_idx), "write .idx failed");

  /*
   * Reset clears index_loaded, so the next process() call will run
   * load_index which reads the bad value and clamps it to 0.
   */
  fs_sink_reset(sink_dev);

  ret = zstreamer_source_start(src_dev);
  zassert_equal(ret, 0, "src start: %d", ret);

  k_msleep(200);

  ret = zstreamer_source_stop(src_dev);
  zassert_equal(ret, 0, "src stop: %d", ret);

  fs_sink_close(sink_dev);

  /* Driver should have started at index 0 after clamping */
  ret = fs_stat("/lfs/test00000.bin", &entry);
  zassert_equal(ret, 0, "file 00000.bin missing: %d", ret);
  zassert_true(entry.size > 0, "file 00000.bin empty");

  assert_pool_free(graph_dev);
}

ZTEST(zstreamer_fs, test_restart_cycle) {
  for (int cycle = 0; cycle < 5; cycle++) {
    struct fs_dirent entry;
    int ret;

    ret = zstreamer_source_start(src_dev);
    zassert_equal(ret, 0, "cycle %d start: %d", cycle, ret);

    k_msleep(200);

    ret = zstreamer_source_stop(src_dev);
    zassert_equal(ret, 0, "cycle %d stop: %d", cycle, ret);

    ret = fs_stat("/lfs/test00000.bin", &entry);
    zassert_equal(ret, 0, "cycle %d: file missing: %d", cycle, ret);
    zassert_true(entry.size > 0, "cycle %d: file empty", cycle);

    assert_pool_free(graph_dev);
    fs_sink_close(sink_dev);
    delete_prefixed_files("test");
    if (fs_stat("/lfs/testidx", &entry) == 0) {
      fs_unlink("/lfs/testidx");
    }
    fs_sink_reset(sink_dev);
  }
}

/* ------------------------------------------------------------------ */
/* Delta-ms rotation tests — use slow numgen (100ms) + delta sink     */
/* (delta-ms-threshold=200, no size-threshold).  With 1 buf/100ms,    */
/* each file gets ~2 buffers (128 bytes) before time-based rotation.  */
/* ------------------------------------------------------------------ */

ZTEST(zstreamer_fs, test_delta_devices_ready) {
  zassert_true(device_is_ready(delta_src_dev), "delta src not ready");
  zassert_true(device_is_ready(delta_sink_dev), "delta sink not ready");
}

ZTEST(zstreamer_fs, test_delta_rotation) {
  struct fs_dirent entry;
  int ret;

  ret = zstreamer_source_start(delta_src_dev);
  zassert_equal(ret, 0, "delta start: %d", ret);

  /* 800ms with 200ms threshold → expect at least 3 rotations */
  k_msleep(800);

  ret = zstreamer_source_stop(delta_src_dev);
  zassert_equal(ret, 0, "delta stop: %d", ret);

  fs_sink_close(delta_sink_dev);

  /* At least files 0, 1, 2 should exist */
  for (int i = 0; i < 3; i++) {
    char path[48];

    snprintf(path, sizeof(path), "/lfs/delta%05u.bin", i);
    ret = fs_stat(path, &entry);
    zassert_equal(ret, 0, "delta file %d missing: %d", i, ret);
    zassert_true(entry.size > 0, "delta file %d empty", i);
  }

  /*
   * Each file should be small (~128 bytes = 2 bufs of 64).
   * Verify file 0 is well under the size-rotation range to confirm
   * rotation was triggered by time, not size.
   */
  ret = fs_stat("/lfs/delta00000.bin", &entry);
  zassert_equal(ret, 0, "stat delta 0: %d", ret);
  zassert_true(entry.size <= 256,
               "delta file 0 too large (%zu) — size rotation?", entry.size);

  assert_pool_free(graph_dev);
}

ZTEST(zstreamer_fs, test_delta_wraparound) {
  struct fs_dirent entry;
  int ret;

  /*
   * file-count=5, delta-ms=200.  Run 1200ms → ~6 rotations,
   * wrapping past index 4 back to 0.
   */
  ret = zstreamer_source_start(delta_src_dev);
  zassert_equal(ret, 0, "delta start: %d", ret);

  k_msleep(1200);

  ret = zstreamer_source_stop(delta_src_dev);
  zassert_equal(ret, 0, "delta stop: %d", ret);

  fs_sink_close(delta_sink_dev);

  /* Files 0-4 should all exist */
  for (int i = 0; i < 5; i++) {
    char path[48];

    snprintf(path, sizeof(path), "/lfs/delta%05u.bin", i);
    ret = fs_stat(path, &entry);
    zassert_equal(ret, 0, "delta file %d missing: %d", i, ret);
  }

  /* File 5 must not exist */
  ret = fs_stat("/lfs/delta00005.bin", &entry);
  zassert_not_equal(ret, 0, "delta file 5 should not exist");

  assert_pool_free(graph_dev);
}

/* Keep custom_filename last — it permanently changes the callback */

static int custom_filename_cb(const struct device *dev, char *buf,
                              size_t buf_size, const char *mount_path,
                              const char *prefix, uint32_t file_index,
                              void *user_data) {
  ARG_UNUSED(dev);
  ARG_UNUSED(prefix);
  ARG_UNUSED(file_index);
  ARG_UNUSED(user_data);

  /* Use mount_path from the driver, but our own naming scheme */
  int n;

  n = snprintf(buf, buf_size, "%s/custom_%u.bin", mount_path, file_index);
  if (n < 0 || (size_t)n >= buf_size) {
    return -ENOMEM;
  }

  return 0;
}

static void delete_custom_files(void) {
  for (int i = 0; i < 10; i++) {
    char path[32];
    struct fs_dirent e;

    snprintf(path, sizeof(path), "/lfs/custom_%u.bin", i);
    if (fs_stat(path, &e) == 0) {
      fs_unlink(path);
    }
  }
}

ZTEST(zstreamer_fs, test_custom_filename) {
  struct fs_dirent entry;
  int ret;

  delete_custom_files();

  ret = fs_sink_set_filename_handler(sink_dev, custom_filename_cb, NULL);
  zassert_equal(ret, 0, "set handler: %d", ret);

  ret = zstreamer_source_start(src_dev);
  zassert_equal(ret, 0, "src start: %d", ret);

  k_msleep(500);

  ret = zstreamer_source_stop(src_dev);
  zassert_equal(ret, 0, "src stop: %d", ret);

  ret = fs_stat("/lfs/custom_0.bin", &entry);
  zassert_equal(ret, 0, "custom file 0 missing: %d", ret);
  zassert_true(entry.size > 0, "custom file 0 empty");

  delete_custom_files();

  assert_pool_free(graph_dev);
}
