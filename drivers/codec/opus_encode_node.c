/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * Opus encoder processor node.
 *
 * Consumes 16-bit little-endian interleaved PCM, accumulates full
 * Opus frames in the driver data (input buffer boundaries need not
 * align with frame boundaries), and emits the self-delimiting stream
 * described in opus_common.h: one 12-byte header per stream, then
 * length-prefixed Opus packets.
 *
 * Stream lifecycle (in-band events):
 *   START — reset encoder state; the next packet is preceded by a
 *           fresh stream header, so the stream that lands at a sink
 *           (e.g. a file opened by fs_sink) is self-contained.
 *   STOP  — zero-pad and flush the partial frame before the event is
 *           forwarded, so no audio is lost and the STOP that
 *           finalizes downstream sinks stays behind all data.
 *
 * The node thread runs on a driver-supplied stack
 * (CONFIG_ZSTREAMER_OPUS_THREAD_STACK_SIZE): even with libopus'
 * scratch arena off-stack, encode calls need ~7.4 KB of real stack,
 * which cannot fit ZSTREAMER_THREAD_STACK_SIZE.
 */

#define DT_DRV_COMPAT zstreamer_opus_encode_node

#include <opus.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>

#include <zstreamer/node.h>

#include "opus_common.h"

LOG_MODULE_REGISTER(opus_encode_node, CONFIG_ZSTREAMER_LOG_LEVEL);

/* Worst-case OpusEncoder sizes (fixed-point build, measured 24548 /
 * 29292 on 32-bit x86) with headroom for other architectures.
 * opus_encoder_get_size() is verified against this at init. */
#define OPUS_ENC_STATE_SIZE(channels) ((channels) == 1 ? 26624 : 31744)

struct opus_encode_node_config {
	struct zstreamer_node_config common;
	uint32_t sample_rate;
	uint32_t bitrate;
	uint16_t frame_samples;
	uint8_t channels;
};

struct opus_encode_node_data {
	struct zstreamer_node_data common;
	/** OpusEncoder lives in this per-instance buffer. */
	uint8_t *enc_state;
	size_t enc_state_size;
	/** PCM frame accumulator (frame_samples * channels samples). */
	opus_int16 *pcm;
	size_t pcm_fill_bytes;
	size_t pcm_frame_bytes;
	bool header_sent;
};

static int opus_encode_node_reset(const struct device *dev)
{
	const struct opus_encode_node_config *cfg = dev->config;
	struct opus_encode_node_data *data = dev->data;
	int err;

	k_mutex_lock(&zstreamer_opus_lock, K_FOREVER);
	err = opus_encoder_init((OpusEncoder *)data->enc_state, cfg->sample_rate, cfg->channels,
				OPUS_APPLICATION_AUDIO);
	if (err == OPUS_OK) {
		opus_encoder_ctl((OpusEncoder *)data->enc_state, OPUS_SET_BITRATE(cfg->bitrate));
	}
	k_mutex_unlock(&zstreamer_opus_lock);

	data->pcm_fill_bytes = 0;
	data->header_sent = false;

	return (err == OPUS_OK) ? 0 : -EIO;
}

static int emit_header(const struct device *dev)
{
	const struct opus_encode_node_config *cfg = dev->config;
	struct opus_encode_node_data *data = dev->data;
	struct net_buf *buf = zstreamer_node_alloc_buf(dev, K_MSEC(100));

	if (buf == NULL) {
		return -ENOMEM;
	}

	opus_stream_header_write(net_buf_add(buf, OPUS_STREAM_HEADER_SIZE), cfg->channels,
				 cfg->frame_samples, cfg->sample_rate);
	zstreamer_node_distribute(dev, buf, cfg->common.children, cfg->common.num_children);
	data->header_sent = true;

	return 0;
}

/** Encode the accumulated frame and emit it as a length-prefixed packet. */
static int encode_frame(const struct device *dev)
{
	const struct opus_encode_node_config *cfg = dev->config;
	struct opus_encode_node_data *data = dev->data;
	struct net_buf *out;
	opus_int32 n;
	int ret;

	if (!data->header_sent) {
		ret = emit_header(dev);
		if (ret != 0) {
			LOG_ERR("[%s] header emit failed: %d", dev->name, ret);
			return ret;
		}
	}

	out = zstreamer_node_alloc_buf(dev, K_MSEC(100));
	if (out == NULL) {
		LOG_WRN("[%s] out buf alloc failed, frame dropped", dev->name);
		return -ENOMEM;
	}

	k_mutex_lock(&zstreamer_opus_lock, K_FOREVER);
	n = opus_encode((OpusEncoder *)data->enc_state, data->pcm, cfg->frame_samples,
			out->data + OPUS_STREAM_LEN_SIZE,
			net_buf_tailroom(out) - OPUS_STREAM_LEN_SIZE);
	k_mutex_unlock(&zstreamer_opus_lock);

	if (n < 0) {
		LOG_ERR("[%s] opus_encode failed: %d", dev->name, n);
		net_buf_unref(out);
		return -EIO;
	}

	sys_put_le16((uint16_t)n, net_buf_add(out, OPUS_STREAM_LEN_SIZE));
	net_buf_add(out, n);
	zstreamer_node_distribute(dev, out, cfg->common.children, cfg->common.num_children);

	return 0;
}

static int opus_encode_node_process(const struct device *dev, struct net_buf *buf)
{
	struct opus_encode_node_data *data = dev->data;
	const uint8_t *src = buf->data;
	size_t remaining = buf->len;

	while (remaining > 0) {
		size_t space = data->pcm_frame_bytes - data->pcm_fill_bytes;
		size_t chunk = MIN(space, remaining);

		memcpy((uint8_t *)data->pcm + data->pcm_fill_bytes, src, chunk);
		data->pcm_fill_bytes += chunk;
		src += chunk;
		remaining -= chunk;

		if (data->pcm_fill_bytes == data->pcm_frame_bytes) {
			encode_frame(dev);
			data->pcm_fill_bytes = 0;
		}
	}

	/* Input consumed; encoded packets were distributed manually. */
	return -EAGAIN;
}

static int opus_encode_node_handle_event(const struct device *dev, struct net_buf *buf)
{
	struct opus_encode_node_data *data = dev->data;

	switch (zstreamer_buf_type_get(buf)) {
	case ZSTREAMER_BUF_EVENT_START:
		opus_encode_node_reset(dev);
		break;
	case ZSTREAMER_BUF_EVENT_STOP:
		/* Zero-pad and flush the partial frame now — the framework
		 * forwards the STOP event after we return, so it stays ordered
		 * behind the flushed packet. */
		if (data->pcm_fill_bytes > 0) {
			memset((uint8_t *)data->pcm + data->pcm_fill_bytes, 0,
			       data->pcm_frame_bytes - data->pcm_fill_bytes);
			encode_frame(dev);
			data->pcm_fill_bytes = 0;
		}
		break;
	default:
		break;
	}

	return 0;
}

static int opus_encode_node_init(const struct device *dev)
{
	const struct opus_encode_node_config *cfg = dev->config;
	struct opus_encode_node_data *data = dev->data;
	int ret;

	if ((size_t)opus_encoder_get_size(cfg->channels) > data->enc_state_size) {
		LOG_ERR("[%s] encoder state %d > reserved %zu", dev->name,
			opus_encoder_get_size(cfg->channels), data->enc_state_size);
		return -ENOMEM;
	}

	ret = opus_encode_node_reset(dev);
	if (ret != 0) {
		LOG_ERR("[%s] encoder init failed", dev->name);
		return ret;
	}

	LOG_INF("[%s] opus enc: %u Hz, %u ch, %u samples/frame, %u bps", dev->name,
		cfg->sample_rate, cfg->channels, cfg->frame_samples, cfg->bitrate);

	return zstreamer_node_common_init_stack(dev, data->common.stack,
						CONFIG_ZSTREAMER_OPUS_THREAD_STACK_SIZE);
}

static const struct zstreamer_node_driver_api opus_encode_node_api = {
	.process = opus_encode_node_process,
	.handle_event = opus_encode_node_handle_event,
};

#define OPUS_ENC_FRAME_SAMPLES(inst)                                                               \
	(DT_INST_PROP(inst, frame_ms) * DT_INST_PROP(inst, sample_rate_hz) / 1000)

#define OPUS_ENCODE_NODE_DEFINE(inst)                                                              \
	BUILD_ASSERT(DT_INST_PROP(inst, sample_rate_hz) == 8000 ||                                 \
			     DT_INST_PROP(inst, sample_rate_hz) == 12000 ||                        \
			     DT_INST_PROP(inst, sample_rate_hz) == 16000 ||                        \
			     DT_INST_PROP(inst, sample_rate_hz) == 24000 ||                        \
			     DT_INST_PROP(inst, sample_rate_hz) == 48000,                          \
		     "opus: sample-rate-hz must be 8000/12000/16000/24000/48000");                 \
	BUILD_ASSERT(DT_INST_PROP(inst, channels) >= 1 && DT_INST_PROP(inst, channels) <= 2,       \
		     "opus: channels must be 1 or 2");                                             \
	BUILD_ASSERT(DT_INST_PROP(inst, frame_ms) == 10 || DT_INST_PROP(inst, frame_ms) == 20 ||   \
			     DT_INST_PROP(inst, frame_ms) == 40 ||                                 \
			     DT_INST_PROP(inst, frame_ms) == 60,                                   \
		     "opus: frame-ms must be 10/20/40/60");                                        \
	BUILD_ASSERT(DT_PROP(DT_PARENT(DT_DRV_INST(inst)), buffer_size) >=                         \
			     OPUS_STREAM_HEADER_SIZE + OPUS_STREAM_LEN_SIZE + 16,                  \
		     "opus: graph buffer-size too small for encoded output");                      \
	Z_ZSTREAMER_CHILDREN_DEFINE(zstreamer_node, inst);                                         \
	static K_THREAD_STACK_DEFINE(opus_encode_big_stack_##inst,                                 \
				     CONFIG_ZSTREAMER_OPUS_THREAD_STACK_SIZE);                     \
	static uint8_t opus_encode_state_##inst[OPUS_ENC_STATE_SIZE(DT_INST_PROP(inst, channels))] \
		__aligned(8);                                                                      \
	static opus_int16 opus_encode_pcm_##inst[OPUS_ENC_FRAME_SAMPLES(inst) *                    \
						 DT_INST_PROP(inst, channels)];                    \
	static struct opus_encode_node_data opus_encode_node_data_##inst = {                       \
		.common = {.stack = opus_encode_big_stack_##inst},                                 \
		.enc_state = opus_encode_state_##inst,                                             \
		.enc_state_size = sizeof(opus_encode_state_##inst),                                \
		.pcm = opus_encode_pcm_##inst,                                                     \
		.pcm_frame_bytes = sizeof(opus_encode_pcm_##inst),                                 \
	};                                                                                         \
	static const struct opus_encode_node_config opus_encode_node_config_##inst = {             \
		.common = ZSTREAMER_NODE_CONFIG_INIT(inst, true),                                  \
		.sample_rate = DT_INST_PROP(inst, sample_rate_hz),                                 \
		.bitrate = DT_INST_PROP(inst, bitrate_bps),                                        \
		.frame_samples = OPUS_ENC_FRAME_SAMPLES(inst),                                     \
		.channels = DT_INST_PROP(inst, channels),                                          \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, opus_encode_node_init, NULL, &opus_encode_node_data_##inst,    \
			      &opus_encode_node_config_##inst, POST_KERNEL,                        \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &opus_encode_node_api);

DT_INST_FOREACH_STATUS_OKAY(OPUS_ENCODE_NODE_DEFINE)
