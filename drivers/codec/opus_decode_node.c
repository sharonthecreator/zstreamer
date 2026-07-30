/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * Opus decoder processor node.
 *
 * Parses the self-delimiting stream produced by opus_encode_node
 * (see opus_common.h): a 12-byte stream header, then length-prefixed
 * Opus packets.  The parser is a byte-wise state machine, so headers,
 * length prefixes, and packets may split across input buffers
 * arbitrarily (e.g. when replayed from files in fixed-size chunks).
 * Each decoded frame is emitted as one buffer of 16-bit little-endian
 * interleaved PCM.
 *
 * Every stream header re-initializes the decoder — each stream (each
 * file written by fs_sink) decodes independently.  The header's
 * channels/sample-rate must match this node's devicetree config; on
 * mismatch or framing corruption the node drops input until the next
 * stream start.
 *
 * Stream lifecycle (in-band events):
 *   START — expect a fresh stream header next.
 *   STOP  — drop any partial packet; forwarded downstream as usual.
 *
 * Runs on a driver-supplied stack, same reason as opus_encode_node.
 */

#define DT_DRV_COMPAT zstreamer_opus_decode_node

#include <opus.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>

#include <zstreamer/node.h>

#include "opus_common.h"

LOG_MODULE_REGISTER(opus_decode_node, CONFIG_ZSTREAMER_LOG_LEVEL);

/* Worst-case OpusDecoder sizes (fixed-point build, measured 17800 /
 * 26520 on 32-bit x86) with headroom for other architectures. */
#define OPUS_DEC_STATE_SIZE(channels) ((channels) == 1 ? 19968 : 28672)

/* Reassembly bound for one length-prefixed packet.  Generous for any
 * sane bitrate/frame combination (e.g. 510 kbps at 20 ms is ~1276
 * bytes); longer prefixes indicate stream corruption. */
#define OPUS_DEC_MAX_PACKET 1500

enum opus_parse_state {
	PARSE_HEADER,
	PARSE_LEN,
	PARSE_PACKET,
	/** Corrupt or mismatched stream: drop input until next START. */
	PARSE_SKIP,
};

struct opus_decode_node_config {
	struct zstreamer_node_config common;
	uint32_t sample_rate;
	uint8_t channels;
};

struct opus_decode_node_data {
	struct zstreamer_node_data common;
	/** OpusDecoder lives in this per-instance buffer. */
	uint8_t *dec_state;
	size_t dec_state_size;
	enum opus_parse_state state;
	uint8_t header[OPUS_STREAM_HEADER_SIZE];
	uint8_t len_prefix[OPUS_STREAM_LEN_SIZE];
	uint8_t packet[OPUS_DEC_MAX_PACKET];
	/** Bytes collected so far for the current state's object. */
	size_t fill;
	uint16_t packet_len;
	uint16_t frame_samples;
};

static int opus_decode_node_reset(const struct device *dev)
{
	const struct opus_decode_node_config *cfg = dev->config;
	struct opus_decode_node_data *data = dev->data;
	int err;

	k_mutex_lock(&zstreamer_opus_lock, K_FOREVER);
	err = opus_decoder_init((OpusDecoder *)data->dec_state, cfg->sample_rate, cfg->channels);
	k_mutex_unlock(&zstreamer_opus_lock);

	return (err == OPUS_OK) ? 0 : -EIO;
}

/** Validate a completed stream header and re-init the decoder. */
static void handle_header(const struct device *dev)
{
	const struct opus_decode_node_config *cfg = dev->config;
	struct opus_decode_node_data *data = dev->data;
	uint8_t channels = data->header[5];
	uint16_t frame_samples = sys_get_le16(&data->header[6]);
	uint32_t sample_rate = sys_get_le32(&data->header[8]);

	if (memcmp(data->header, OPUS_STREAM_MAGIC, 4) != 0 ||
	    data->header[4] != OPUS_STREAM_VERSION) {
		LOG_ERR("[%s] bad stream header", dev->name);
		data->state = PARSE_SKIP;
		return;
	}

	if (channels != cfg->channels || sample_rate != cfg->sample_rate) {
		LOG_ERR("[%s] stream %u Hz/%u ch != node %u Hz/%u ch", dev->name, sample_rate,
			channels, cfg->sample_rate, cfg->channels);
		data->state = PARSE_SKIP;
		return;
	}

	if (opus_decode_node_reset(dev) != 0) {
		LOG_ERR("[%s] decoder re-init failed", dev->name);
		data->state = PARSE_SKIP;
		return;
	}

	data->frame_samples = frame_samples;
	data->state = PARSE_LEN;
	data->fill = 0;
}

/** Decode the completed packet and emit one PCM frame buffer. */
static void handle_packet(const struct device *dev)
{
	const struct opus_decode_node_config *cfg = dev->config;
	struct opus_decode_node_data *data = dev->data;
	struct net_buf *out = zstreamer_node_alloc_buf(dev, K_MSEC(100));
	int max_samples;
	int n;

	if (out == NULL) {
		LOG_WRN("[%s] out buf alloc failed, frame dropped", dev->name);
		return;
	}

	max_samples = net_buf_tailroom(out) / (2 * cfg->channels);

	k_mutex_lock(&zstreamer_opus_lock, K_FOREVER);
	n = opus_decode((OpusDecoder *)data->dec_state, data->packet, data->packet_len,
			(opus_int16 *)out->data, max_samples, 0);
	k_mutex_unlock(&zstreamer_opus_lock);

	if (n < 0) {
		LOG_ERR("[%s] opus_decode failed: %d", dev->name, n);
		net_buf_unref(out);
		return;
	}

	net_buf_add(out, (size_t)n * 2 * cfg->channels);
	zstreamer_node_distribute(dev, out, cfg->common.children, cfg->common.num_children);
}

static int opus_decode_node_process(const struct device *dev, struct net_buf *buf)
{
	struct opus_decode_node_data *data = dev->data;
	const uint8_t *src = buf->data;
	size_t remaining = buf->len;

	while (remaining > 0) {
		size_t want, chunk;
		uint8_t *dst;

		switch (data->state) {
		case PARSE_HEADER:
			want = OPUS_STREAM_HEADER_SIZE - data->fill;
			dst = &data->header[data->fill];
			break;
		case PARSE_LEN:
			want = OPUS_STREAM_LEN_SIZE - data->fill;
			dst = &data->len_prefix[data->fill];
			break;
		case PARSE_PACKET:
			want = data->packet_len - data->fill;
			dst = &data->packet[data->fill];
			break;
		case PARSE_SKIP:
		default:
			return -EAGAIN;
		}

		chunk = MIN(want, remaining);
		memcpy(dst, src, chunk);
		data->fill += chunk;
		src += chunk;
		remaining -= chunk;

		if (data->fill < (data->state == PARSE_HEADER ? OPUS_STREAM_HEADER_SIZE
				  : data->state == PARSE_LEN  ? OPUS_STREAM_LEN_SIZE
							      : data->packet_len)) {
			continue;
		}

		switch (data->state) {
		case PARSE_HEADER:
			handle_header(dev);
			break;
		case PARSE_LEN:
			data->packet_len = sys_get_le16(data->len_prefix);
			if (data->packet_len == 0 || data->packet_len > OPUS_DEC_MAX_PACKET) {
				LOG_ERR("[%s] bad packet length %u", dev->name, data->packet_len);
				data->state = PARSE_SKIP;
				break;
			}
			data->state = PARSE_PACKET;
			data->fill = 0;
			break;
		case PARSE_PACKET:
			handle_packet(dev);
			data->state = PARSE_LEN;
			data->fill = 0;
			break;
		default:
			break;
		}
	}

	/* Input consumed; decoded frames were distributed manually. */
	return -EAGAIN;
}

static int opus_decode_node_handle_event(const struct device *dev, struct net_buf *buf)
{
	struct opus_decode_node_data *data = dev->data;

	switch (zstreamer_buf_type_get(buf)) {
	case ZSTREAMER_BUF_EVENT_START:
		data->state = PARSE_HEADER;
		data->fill = 0;
		break;
	case ZSTREAMER_BUF_EVENT_STOP:
		if (data->state == PARSE_PACKET && data->fill > 0) {
			LOG_WRN("[%s] stream stopped mid-packet, %zu bytes dropped", dev->name,
				data->fill);
		}
		data->state = PARSE_HEADER;
		data->fill = 0;
		break;
	default:
		break;
	}

	return 0;
}

static int opus_decode_node_init(const struct device *dev)
{
	const struct opus_decode_node_config *cfg = dev->config;
	struct opus_decode_node_data *data = dev->data;
	int ret;

	if ((size_t)opus_decoder_get_size(cfg->channels) > data->dec_state_size) {
		LOG_ERR("[%s] decoder state %d > reserved %zu", dev->name,
			opus_decoder_get_size(cfg->channels), data->dec_state_size);
		return -ENOMEM;
	}

	ret = opus_decode_node_reset(dev);
	if (ret != 0) {
		LOG_ERR("[%s] decoder init failed", dev->name);
		return ret;
	}

	LOG_INF("[%s] opus dec: %u Hz, %u ch", dev->name, cfg->sample_rate, cfg->channels);

	return zstreamer_node_common_init_stack(dev, data->common.stack,
						CONFIG_ZSTREAMER_OPUS_THREAD_STACK_SIZE);
}

static const struct zstreamer_node_driver_api opus_decode_node_api = {
	.process = opus_decode_node_process,
	.handle_event = opus_decode_node_handle_event,
};

#define OPUS_DECODE_NODE_DEFINE(inst)                                                              \
	BUILD_ASSERT(DT_INST_PROP(inst, sample_rate_hz) == 8000 ||                                 \
			     DT_INST_PROP(inst, sample_rate_hz) == 12000 ||                        \
			     DT_INST_PROP(inst, sample_rate_hz) == 16000 ||                        \
			     DT_INST_PROP(inst, sample_rate_hz) == 24000 ||                        \
			     DT_INST_PROP(inst, sample_rate_hz) == 48000,                          \
		     "opus: sample-rate-hz must be 8000/12000/16000/24000/48000");                 \
	BUILD_ASSERT(DT_INST_PROP(inst, channels) >= 1 && DT_INST_PROP(inst, channels) <= 2,       \
		     "opus: channels must be 1 or 2");                                             \
	Z_ZSTREAMER_CHILDREN_DEFINE(zstreamer_node, inst);                                         \
	static K_THREAD_STACK_DEFINE(opus_decode_big_stack_##inst,                                 \
				     CONFIG_ZSTREAMER_OPUS_THREAD_STACK_SIZE);                     \
	static uint8_t opus_decode_state_##inst[OPUS_DEC_STATE_SIZE(DT_INST_PROP(inst, channels))] \
		__aligned(8);                                                                      \
	static struct opus_decode_node_data opus_decode_node_data_##inst = {                       \
		.common = {.stack = opus_decode_big_stack_##inst},                                 \
		.dec_state = opus_decode_state_##inst,                                             \
		.dec_state_size = sizeof(opus_decode_state_##inst),                                \
		.state = PARSE_HEADER,                                                             \
	};                                                                                         \
	static const struct opus_decode_node_config opus_decode_node_config_##inst = {             \
		.common = ZSTREAMER_NODE_CONFIG_INIT(inst, true),                                  \
		.sample_rate = DT_INST_PROP(inst, sample_rate_hz),                                 \
		.channels = DT_INST_PROP(inst, channels),                                          \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, opus_decode_node_init, NULL, &opus_decode_node_data_##inst,    \
			      &opus_decode_node_config_##inst, POST_KERNEL,                        \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &opus_decode_node_api);

DT_INST_FOREACH_STATUS_OKAY(OPUS_DECODE_NODE_DEFINE)
