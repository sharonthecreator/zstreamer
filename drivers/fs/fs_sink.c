/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zstreamer_fs_sink

#include <stdio.h>
#include <zephyr/device.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>

#include <zstreamer/fs/fs_sink.h>
#include <zstreamer/sink.h>

LOG_MODULE_REGISTER(fs_sink, CONFIG_ZSTREAMER_LOG_LEVEL);

struct fs_sink_config {
	struct zstreamer_sink_config common;
	const char *mount_path;
	const char *filename_prefix;
	uint32_t delta_ms_threshold;
	uint32_t size_threshold;
	uint32_t file_count;
	uint32_t max_write_size;
};

struct fs_sink_data {
	struct zstreamer_sink_data common;
	struct fs_file_t current_file;
	/** Uptime (ms) when the current file was opened, for delta rotation. */
	int64_t file_open_time_ms;
	size_t current_file_size;
	/** Next file index to use. Wraps to 0 when it reaches file_count. */
	uint32_t file_index;
	/** True once a file has been opened in this run (lazy open on first buf). */
	bool file_opened;
	/**
	 * True once file_index has been loaded from the persisted .idx file (or
	 * explicitly set via fs_sink_reset).  Prevents re-reading .idx on every
	 * file open — only the very first process() call after boot loads it.
	 * Doesn't load the index in init as the filesystem might not be mounted.
	 */
	bool index_loaded;
	fs_sink_filename_cb_t filename_cb;
	void *filename_cb_user_data;
	/**
	 * Scratch buffers kept here instead of on the 2 KB thread stack to
	 * avoid overflow when FatFs LFN is enabled (MAX_FILE_NAME == 255).
	 */
	char path_buf[MAX_FILE_NAME + 1];
	struct fs_dirent dirent_buf;
};

/*
 * Index persistence — the file_index is saved to <mount_path>/.idx as a raw
 * uint32_t so the driver can resume at the correct file after a reboot.
 * Loaded lazily on the first process() call; saved after every rotation.
 */

/** Build the path to the .idx persistence file. */
static int idx_path(const struct device *dev, char *buf, size_t buf_size)
{
	const struct fs_sink_config *cfg = dev->config;
	int n = snprintf(buf, buf_size, "%s/%sidx", cfg->mount_path, cfg->filename_prefix);

	if (n < 0 || (size_t)n >= buf_size) {
		return -ENOMEM;
	}
	return 0;
}

/**
 * Read file_index from .idx on disk.  Falls back to 0 if the file is missing
 * or corrupt.  Clamps to file_count if the saved value exceeds the limit
 * (e.g. file_count was reduced since the last boot).
 */
static int load_index(const struct device *dev)
{
	struct fs_sink_data *data = dev->data;
	const struct fs_sink_config *cfg = dev->config;
	struct fs_file_t f;
	int ret;
	uint32_t idx;

	ret = idx_path(dev, data->path_buf, sizeof(data->path_buf));
	if (ret < 0) {
		return ret;
	}

	ret = fs_stat(data->path_buf, &data->dirent_buf);
	if (ret < 0) {
		/* No saved index file — start at 0 */
		data->file_index = 0;
		return 0;
	}

	fs_file_t_init(&f);
	ret = fs_open(&f, data->path_buf, FS_O_READ);
	if (ret < 0) {
		data->file_index = 0;
		return 0;
	}

	ret = fs_read(&f, &idx, sizeof(idx));
	fs_close(&f);
	/* Verify we read the entire uint32_t value. */
	if (ret == sizeof(idx)) {
		if (cfg->file_count && idx >= cfg->file_count) {
			idx = 0;
		}
		data->file_index = idx;
	} else {
		data->file_index = 0;
	}

	return 0;
}

/** Persist the current file_index to .idx so it survives reboot. */
static int save_index(const struct device *dev)
{
	struct fs_sink_data *data = dev->data;
	struct fs_file_t f;
	int ret;

	ret = idx_path(dev, data->path_buf, sizeof(data->path_buf));
	if (ret < 0) {
		return ret;
	}

	fs_file_t_init(&f);
	ret = fs_open(&f, data->path_buf, FS_O_CREATE | FS_O_WRITE);
	if (ret < 0) {
		LOG_ERR("failed to save index: %d", ret);
		return ret;
	}

	ret = fs_write(&f, &data->file_index, sizeof(data->file_index));
	fs_close(&f);

	return (ret == sizeof(data->file_index)) ? 0 : -EIO;
}

static int default_filename_cb(const struct device *dev, char *buf, size_t buf_size,
			       const char *mount_path, const char *prefix, uint32_t file_index,
			       void *user_data)
{
	int n;

	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	n = snprintf(buf, buf_size, "%s/%s%05u.bin", mount_path, prefix, file_index);
	if (n < 0 || (size_t)n >= buf_size) {
		return -ENOMEM;
	}

	return 0;
}

/**
 * Open a new output file using the filename callback.  Records the open
 * time for delta-based rotation and resets the per-file byte counter.
 */
static int open_new_file(const struct device *dev)
{
	const struct fs_sink_config *cfg = dev->config;
	struct fs_sink_data *data = dev->data;
	int ret;

	ret = data->filename_cb(dev, data->path_buf, sizeof(data->path_buf), cfg->mount_path,
				cfg->filename_prefix, data->file_index,
				data->filename_cb_user_data);
	if (ret < 0) {
		LOG_ERR("filename callback failed: %d", ret);
		return ret;
	}

	fs_file_t_init(&data->current_file);

	ret = fs_open(&data->current_file, data->path_buf, FS_O_CREATE | FS_O_WRITE);
	if (ret != 0) {
		LOG_ERR("failed to open %s: %d", data->path_buf, ret);
		return ret;
	}

	data->file_open_time_ms = k_uptime_get();
	data->current_file_size = 0;
	data->file_opened = true;

	LOG_DBG("opened %s", data->path_buf);

	return 0;
}

/**
 * Close the current file and open the next one.  Advances file_index,
 * wrapping to 0 when file_count is reached (cyclic buffer of files).
 * Persists the new index to .idx so we resume correctly after reboot.
 */
static int rotate_file(const struct device *dev)
{
	const struct fs_sink_config *cfg = dev->config;
	struct fs_sink_data *data = dev->data;
	int ret;

	ret = fs_close(&data->current_file);
	if (ret != 0) {
		LOG_ERR("failed to close file: %d", ret);
		return ret;
	}

	data->file_index++;
	if (cfg->file_count && data->file_index >= cfg->file_count) {
		data->file_index = 0;
	}

	ret = save_index(dev);
	if (ret != 0) {
		LOG_WRN("failed to persist index: %d", ret);
	}

	return open_new_file(dev);
}

/** Check whether the current file should be rotated based on elapsed time
 *  (delta_ms_threshold) or accumulated size (size_threshold).  Either
 *  threshold being 0 disables that check; at least one must be non-zero
 *  (enforced by BUILD_ASSERT in the DEFINE macro). */
static bool should_rotate(const struct device *dev, size_t incoming_len)
{
	const struct fs_sink_config *cfg = dev->config;
	struct fs_sink_data *data = dev->data;

	if (cfg->delta_ms_threshold) {
		int64_t elapsed = k_uptime_get() - data->file_open_time_ms;

		if (elapsed >= cfg->delta_ms_threshold) {
			return true;
		}
	}

	if (cfg->size_threshold) {
		if (data->current_file_size + incoming_len > cfg->size_threshold) {
			return true;
		}
	}

	return false;
}

/**
 * Sink process callback — called for each incoming buffer.
 *
 * File opening is lazy: the first call loads the persisted index from .idx
 * and opens the initial file.  This avoids init-ordering issues with the
 * filesystem (LittleFS registers at POST_KERNEL priority 99, well after
 * device init at priority 40).  Subsequent calls check rotation thresholds
 * and write buffer data to the current file.
 */
static int fs_sink_process(const struct device *dev, struct net_buf *buf)
{
	const struct fs_sink_config *cfg = dev->config;
	struct fs_sink_data *data = dev->data;
	int ret;

	if (!data->file_opened) {
		if (!data->index_loaded) {
			ret = load_index(dev);
			if (ret != 0) {
				return ret;
			}
			data->index_loaded = true;
		}
		ret = open_new_file(dev);
		if (ret != 0) {
			return ret;
		}
	} else if (should_rotate(dev, buf->len)) {
		ret = rotate_file(dev);
		if (ret != 0) {
			return ret;
		}
	}

	const uint8_t *src = buf->data;
	size_t remaining = buf->len;
	size_t chunk_size = cfg->max_write_size ? cfg->max_write_size : remaining;

	while (remaining > 0) {
		size_t to_write = MIN(remaining, chunk_size);
		ssize_t written = fs_write(&data->current_file, src, to_write);

		if (written < 0) {
			LOG_ERR("fs_write failed: %zd", written);
			fs_close(&data->current_file);
			data->file_opened = false;
			return (int)written;
		}

		data->current_file_size += written;
		src += written;
		remaining -= written;
	}

	/* Sync file entry so file size is visible if power is lost. */
	fs_sync(&data->current_file);

	return 0;
}

static int fs_sink_init(const struct device *dev)
{
	struct fs_sink_data *data = dev->data;

	data->file_index = 0;
	data->file_opened = false;
	data->index_loaded = false;

	if (data->filename_cb == NULL) {
		data->filename_cb = default_filename_cb;
	}

	return zstreamer_sink_common_init(dev);
}

static const struct zstreamer_node_driver_api fs_sink_api = {
	.process = fs_sink_process,
};

int fs_sink_set_filename_handler(const struct device *dev, fs_sink_filename_cb_t cb,
				 void *user_data)
{
	struct fs_sink_data *data = dev->data;

	data->filename_cb = cb;
	data->filename_cb_user_data = user_data;

	return 0;
}

int fs_sink_close(const struct device *dev)
{
	struct fs_sink_data *data = dev->data;

	if (!data->file_opened) {
		return 0;
	}

	int ret = fs_close(&data->current_file);

	data->file_opened = false;

	return ret;
}

/**
 * Reset all mutable driver state to defaults.  Clears index_loaded so the
 * next process() call reloads from .idx (or defaults to 0 if absent).
 * Call only while the pipeline is stopped and after fs_sink_close().
 */
void fs_sink_reset(const struct device *dev)
{
	struct fs_sink_data *data = dev->data;

	data->file_index = 0;
	data->file_opened = false;
	data->index_loaded = false;
	data->current_file_size = 0;
	data->filename_cb = default_filename_cb;
	data->filename_cb_user_data = NULL;
}

#define FS_SINK_DEFINE(inst)                                                                       \
	BUILD_ASSERT(DT_INST_PROP(inst, delta_ms_threshold) != 0 ||                                \
			     DT_INST_PROP(inst, size_threshold) != 0,                              \
		     "fs-sink: at least one rotation threshold must be "                           \
		     "non-zero");                                                                  \
	BUILD_ASSERT(DT_INST_PROP(inst, file_count) <= 100000,                                     \
		     "fs-sink: file-count must be <= 100000 "                                      \
		     "(filenames use 5 digits)");                                                  \
	ZSTREAMER_SINK_DT_INST_PRE_DEFINE(inst);                                                   \
	static struct fs_sink_data fs_sink_data_##inst = {                                         \
		.common = ZSTREAMER_SINK_DATA_INIT(inst),                                          \
		.file_open_time_ms = 0,                                                            \
		.current_file_size = 0,                                                            \
		.file_index = 0,                                                                   \
		.index_loaded = false,                                                             \
		.filename_cb = NULL,                                                               \
		.filename_cb_user_data = NULL,                                                     \
	};                                                                                         \
	static const struct fs_sink_config fs_sink_config_##inst = {                               \
		.common = ZSTREAMER_SINK_CONFIG_INIT(inst),                                        \
		.mount_path = DT_INST_PROP(inst, mount_path),                                      \
		.filename_prefix = DT_INST_PROP(inst, filename_prefix),                            \
		.delta_ms_threshold = DT_INST_PROP(inst, delta_ms_threshold),                      \
		.size_threshold = DT_INST_PROP(inst, size_threshold),                              \
		.file_count = DT_INST_PROP(inst, file_count),                                      \
		.max_write_size = DT_INST_PROP(inst, max_write_size),                              \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, fs_sink_init, NULL, &fs_sink_data_##inst,                      \
			      &fs_sink_config_##inst, POST_KERNEL,                                 \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &fs_sink_api);

DT_INST_FOREACH_STATUS_OKAY(FS_SINK_DEFINE)
