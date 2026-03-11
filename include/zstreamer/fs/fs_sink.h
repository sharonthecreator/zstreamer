/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Public API for the file system sink zstreamer node driver
 */

#ifndef ZSTREAMER_FS_SINK_H_
#define ZSTREAMER_FS_SINK_H_

#include <stddef.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback type for generating filenames.
 *
 * @param dev         The sink-fs device.
 * @param buf         Buffer to write the filename into.
 * @param buf_size    Size of the buffer.
 * @param mount_path  Mount path from DTS (e.g. "/lfs").
 * @param prefix      Filename prefix from DTS (e.g. "data_"), or "".
 * @param file_index  Current file index (0-based, wraps at file-count).
 * @param user_data   User-provided context pointer.
 * @return 0 on success, negative errno on failure.
 */
typedef int (*fs_sink_filename_cb_t)(const struct device *dev, char *buf, size_t buf_size,
				     const char *mount_path, const char *prefix,
				     uint32_t file_index, void *user_data);

/**
 * @brief Set a custom filename handler for a sink-fs device.
 *
 * Must be called before starting the node. If not called, a default
 * handler is used that generates "<mount_path>/<prefix><index>.bin".
 *
 * @param dev        The sink-fs device.
 * @param cb         Filename callback function.
 * @param user_data  Opaque pointer passed to the callback.
 * @return 0 on success, negative errno on failure.
 */
int fs_sink_set_filename_handler(const struct device *dev, fs_sink_filename_cb_t cb,
				 void *user_data);

/**
 * @brief Close the current file.
 *
 * Closes the open file handle so the node opens a fresh file on
 * the next buffer received.  Call after stopping the upstream source.
 *
 * @param dev  The fs-sink device.
 * @return 0 on success, negative errno on failure.
 */
int fs_sink_close(const struct device *dev);

/**
 * @brief Reset driver state.
 *
 * Resets file_index and clears the file-opened flag so the next
 * buffer received starts a fresh sequence of files.
 * Must be called only while the pipeline is stopped.
 *
 * @param dev  The fs-sink device.
 */
void fs_sink_reset(const struct device *dev);

#ifdef __cplusplus
}
#endif

#endif /* ZSTREAMER_FS_SINK_H_ */
