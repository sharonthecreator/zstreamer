/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared definitions for the Opus encode/decode nodes.
 *
 * Stream framing
 * --------------
 * Raw Opus packets are variable-length and carry no boundaries, so
 * the encoder emits a self-delimiting byte stream that any sink can
 * store as-is (e.g. fs_sink) and the decoder can parse back:
 *
 *   stream header (12 bytes, little-endian, one per stream):
 *     offset 0: magic "ZOPS"
 *     offset 4: u8  version (OPUS_STREAM_VERSION)
 *     offset 5: u8  channels
 *     offset 6: u16 frame samples per channel
 *     offset 8: u32 sample rate in Hz
 *   then, repeated until end of stream:
 *     u16 packet length | raw Opus packet bytes
 *
 * A new header is emitted at every stream start (in-band START
 * event), so each file written by fs_sink begins with a header and
 * is decodable on its own.
 *
 * Locking
 * -------
 * libopus is built with NONTHREADSAFE_PSEUDOSTACK: scratch memory
 * lives in one global arena instead of the (2 KB, fixed) node thread
 * stacks, which makes every libopus call non-reentrant process-wide.
 * All encoder/decoder calls must hold zstreamer_opus_lock.
 */

#ifndef ZSTREAMER_DRIVERS_CODEC_OPUS_COMMON_H_
#define ZSTREAMER_DRIVERS_CODEC_OPUS_COMMON_H_

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

#define OPUS_STREAM_MAGIC       "ZOPS"
#define OPUS_STREAM_VERSION     1
#define OPUS_STREAM_HEADER_SIZE 12
#define OPUS_STREAM_LEN_SIZE    2

/** Serializes all libopus calls (see "Locking" above). */
extern struct k_mutex zstreamer_opus_lock;

static inline void opus_stream_header_write(uint8_t *out, uint8_t channels, uint16_t frame_samples,
					    uint32_t sample_rate)
{
	memcpy(&out[0], OPUS_STREAM_MAGIC, 4);
	out[4] = OPUS_STREAM_VERSION;
	out[5] = channels;
	sys_put_le16(frame_samples, &out[6]);
	sys_put_le32(sample_rate, &out[8]);
}

#endif /* ZSTREAMER_DRIVERS_CODEC_OPUS_COMMON_H_ */
