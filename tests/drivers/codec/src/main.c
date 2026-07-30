/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * Opus codec node tests.
 *
 * Round-trip pipeline: sine -> opus_encode -> opus_decode -> count_sink
 * verifies PCM comes out in whole frames across full runs and restarts.
 *
 * File pipeline: sine -> opus_encode -> fs_sink verifies the on-disk
 * stream: finalized by the in-band STOP event, starts with a valid
 * stream header, and consists of exactly length-prefixed packets, so
 * every file is decodable on its own.
 */

#include <zephyr/device.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include <zstreamer/fs/fs_sink.h>
#include <zstreamer/node.h>
#include <zstreamer/source.h>
#include <zstreamer_test/helpers.h>

#include <zstreamer/test/count_sink.h>

#define GRAPH_NODE DT_NODELABEL(streaming_graph)

/* 20 ms @ 16 kHz mono, 16-bit */
#define FRAME_BYTES (320 * 2)

static const struct device *graph_dev = DEVICE_DT_GET(GRAPH_NODE);
static const struct device *src_dev = DEVICE_DT_GET(DT_NODELABEL(sine_source));
static const struct device *sink_dev = DEVICE_DT_GET(DT_NODELABEL(pcm_sinker));
static const struct device *file_src_dev = DEVICE_DT_GET(DT_NODELABEL(sine_file));
static const struct device *file_sink_dev = DEVICE_DT_GET(DT_NODELABEL(opus_fs_sinker));

static void delete_opus_files(void)
{
	struct fs_dirent entry;
	char path[32];

	for (int i = 0; i < 10; i++) {
		snprintf(path, sizeof(path), "/lfs/opus%05u.bin", i);
		if (fs_stat(path, &entry) == 0) {
			fs_unlink(path);
		}
	}
	if (fs_stat("/lfs/opusidx", &entry) == 0) {
		fs_unlink("/lfs/opusidx");
	}
}

static void codec_cleanup(void *fixture)
{
	ARG_UNUSED(fixture);

	zstreamer_source_stop(src_dev);
	zstreamer_source_stop(file_src_dev);
	k_msleep(100);
	count_sink_reset(sink_dev);
	fs_sink_close(file_sink_dev);
	delete_opus_files();
	fs_sink_reset(file_sink_dev);
}

ZTEST_SUITE(zstreamer_codec, NULL, NULL, codec_cleanup, codec_cleanup, NULL);

ZTEST(zstreamer_codec, test_devices_ready)
{
	zassert_true(device_is_ready(graph_dev), "graph not ready");
	zassert_true(device_is_ready(src_dev), "sine src not ready");
	zassert_true(device_is_ready(DEVICE_DT_GET(DT_NODELABEL(opus_enc))), "encoder not ready");
	zassert_true(device_is_ready(DEVICE_DT_GET(DT_NODELABEL(opus_dec))), "decoder not ready");
	zassert_true(device_is_ready(sink_dev), "sink not ready");
}

ZTEST(zstreamer_codec, test_roundtrip_decodes_frames)
{
	int ret;

	ret = zstreamer_source_start(src_dev);
	zassert_equal(ret, 0, "src start: %d", ret);

	k_msleep(300);

	ret = zstreamer_source_stop(src_dev);
	zassert_equal(ret, 0, "src stop: %d", ret);

	/* Let encode/decode/sink drain. */
	k_msleep(100);

	uint32_t bufs = count_sink_get_buf_count(sink_dev);
	uint32_t bytes = count_sink_get_byte_count(sink_dev);

	zassert_true(bufs > 0, "no decoded frames");
	zassert_equal(bytes, bufs * FRAME_BYTES,
		      "decoded output not whole frames: %u bytes / %u bufs", bytes, bufs);

	/* Events crossed both codec nodes in order. */
	zassert_equal(count_sink_get_start_event_count(sink_dev), 1, "start events");
	zassert_equal(count_sink_get_stop_event_count(sink_dev), 1, "stop events");

	assert_pool_free(graph_dev);
}

ZTEST(zstreamer_codec, test_roundtrip_restart)
{
	const int cycles = 3;

	for (int i = 0; i < cycles; i++) {
		int ret = zstreamer_source_start(src_dev);

		zassert_equal(ret, 0, "cycle %d start: %d", i, ret);
		k_msleep(150);
		ret = zstreamer_source_stop(src_dev);
		zassert_equal(ret, 0, "cycle %d stop: %d", i, ret);
		k_msleep(100);
	}

	uint32_t bufs = count_sink_get_buf_count(sink_dev);
	uint32_t bytes = count_sink_get_byte_count(sink_dev);

	zassert_true(bufs >= cycles, "expected >= %d frames, got %u", cycles, bufs);
	zassert_equal(bytes, bufs * FRAME_BYTES, "partial frames after restarts");
	zassert_equal(count_sink_get_start_event_count(sink_dev), cycles, "start events != cycles");
	zassert_equal(count_sink_get_stop_event_count(sink_dev), cycles, "stop events != cycles");

	assert_pool_free(graph_dev);
}

/**
 * Walk an encoded stream file: validate the header, then step through
 * the length-prefixed packets.  Returns the packet count, and asserts
 * the last packet ends exactly at end-of-file.
 */
static uint32_t verify_stream_file(const char *path)
{
	struct fs_dirent entry;
	struct fs_file_t f;
	uint8_t header[12];
	uint8_t len_buf[2];
	uint32_t packets = 0;
	size_t pos;
	int ret;

	ret = fs_stat(path, &entry);
	zassert_equal(ret, 0, "%s missing: %d", path, ret);
	zassert_true(entry.size > sizeof(header), "%s too small: %zu", path, entry.size);

	fs_file_t_init(&f);
	ret = fs_open(&f, path, FS_O_READ);
	zassert_equal(ret, 0, "open %s: %d", path, ret);

	ret = fs_read(&f, header, sizeof(header));
	zassert_equal(ret, sizeof(header), "header read: %d", ret);
	zassert_mem_equal(header, "ZOPS", 4, "bad magic");
	zassert_equal(header[4], 1, "bad version");
	zassert_equal(header[5], 1, "bad channels");
	zassert_equal(sys_get_le16(&header[6]), 320, "bad frame samples");
	zassert_equal(sys_get_le32(&header[8]), 16000, "bad sample rate");

	pos = sizeof(header);
	while (pos < entry.size) {
		uint16_t plen;

		ret = fs_read(&f, len_buf, sizeof(len_buf));
		zassert_equal(ret, sizeof(len_buf), "len read at %zu: %d", pos, ret);
		plen = sys_get_le16(len_buf);
		zassert_true(plen > 0 && plen <= 1500, "bad packet len %u at %zu", plen, pos);

		ret = fs_seek(&f, plen, FS_SEEK_CUR);
		zassert_equal(ret, 0, "seek past packet: %d", ret);
		pos += sizeof(len_buf) + plen;
		packets++;
	}

	zassert_equal(pos, entry.size, "trailing garbage: pos %zu size %zu", pos, entry.size);
	fs_close(&f);

	return packets;
}

ZTEST(zstreamer_codec, test_file_stream_format)
{
	uint32_t packets;
	int ret;

	ret = zstreamer_source_start(file_src_dev);
	zassert_equal(ret, 0, "file src start: %d", ret);

	k_msleep(300);

	ret = zstreamer_source_stop(file_src_dev);
	zassert_equal(ret, 0, "file src stop: %d", ret);

	/* The in-band STOP finalizes the file — no fs_sink_close() here. */
	k_msleep(100);

	packets = verify_stream_file("/lfs/opus00000.bin");
	zassert_true(packets >= 5, "expected >= 5 packets, got %u", packets);

	assert_pool_free(graph_dev);
}

ZTEST(zstreamer_codec, test_file_new_stream_per_run)
{
	int ret;

	for (int run = 0; run < 2; run++) {
		ret = zstreamer_source_start(file_src_dev);
		zassert_equal(ret, 0, "run %d start: %d", run, ret);
		k_msleep(200);
		ret = zstreamer_source_stop(file_src_dev);
		zassert_equal(ret, 0, "run %d stop: %d", run, ret);
		k_msleep(100);
	}

	/* Each run produced its own self-contained decodable file. */
	zassert_true(verify_stream_file("/lfs/opus00000.bin") > 0, "run 0 file");
	zassert_true(verify_stream_file("/lfs/opus00001.bin") > 0, "run 1 file");

	assert_pool_free(graph_dev);
}
