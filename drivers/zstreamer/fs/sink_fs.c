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

#include <zstreamer/node.h>
#include <zstreamer/sink_fs.h>

LOG_MODULE_REGISTER(sink_fs, CONFIG_ZSTREAMER_LOG_LEVEL);

struct sink_fs_config {
  struct zstreamer_node_config common;
  const char *mount_path;
  uint32_t delta_ms_threshold;
  uint32_t size_threshold;
};

struct sink_fs_data {
  struct zstreamer_node_data common;
  struct fs_file_t current_file;
  int64_t file_open_time;
  size_t current_file_size;
  uint32_t file_index;
  sink_fs_filename_cb_t filename_cb;
  void *filename_cb_user_data;
};

static int default_filename_cb(const struct device *dev, char *buf,
                               size_t buf_size, void *user_data) {
  const struct sink_fs_config *cfg = dev->config;
  struct sink_fs_data *data = dev->data;
  int n;

  ARG_UNUSED(user_data);

  n = snprintf(buf, buf_size, "%s/%05u.bin", cfg->mount_path, data->file_index);
  if (n < 0 || (size_t)n >= buf_size) {
    return -ENOMEM;
  }

  return 0;
}

static int open_new_file(const struct device *dev) {
  struct sink_fs_data *data = dev->data;
  char filename[MAX_FILE_NAME + 1];
  int ret;

  ret = data->filename_cb(dev, filename, sizeof(filename),
                          data->filename_cb_user_data);
  if (ret < 0) {
    LOG_ERR("filename callback failed: %d", ret);
    return ret;
  }

  fs_file_t_init(&data->current_file);

  ret = fs_open(&data->current_file, filename, FS_O_CREATE | FS_O_WRITE);
  if (ret != 0) {
    LOG_ERR("failed to open %s: %d", filename, ret);
    return ret;
  }

  data->file_open_time = k_uptime_get();
  data->current_file_size = 0;

  LOG_DBG("opened %s", filename);

  return 0;
}

static int rotate_file(const struct device *dev) {
  struct sink_fs_data *data = dev->data;
  int ret;

  ret = fs_close(&data->current_file);
  if (ret != 0) {
    LOG_ERR("failed to close file: %d", ret);
    return ret;
  }

  data->file_index++;

  return open_new_file(dev);
}

static bool should_rotate(const struct device *dev, size_t incoming_len) {
  const struct sink_fs_config *cfg = dev->config;
  struct sink_fs_data *data = dev->data;

  if (cfg->delta_ms_threshold > 0) {
    int64_t elapsed = k_uptime_get() - data->file_open_time;

    if (elapsed >= cfg->delta_ms_threshold) {
      return true;
    }
  }

  if (cfg->size_threshold > 0) {
    if (data->current_file_size + incoming_len > cfg->size_threshold) {
      return true;
    }
  }

  return false;
}

static int sink_fs_open(const struct device *dev) {
  struct sink_fs_data *data = dev->data;

  data->file_index = 0;

  if (data->filename_cb == NULL) {
    data->filename_cb = default_filename_cb;
  }

  return open_new_file(dev);
}

static int sink_fs_process(const struct device *dev, struct net_buf *buf) {
  struct sink_fs_data *data = dev->data;
  int ret;

  if (should_rotate(dev, buf->len)) {
    ret = rotate_file(dev);
    if (ret != 0) {
      return ret;
    }
  }

  ssize_t written = fs_write(&data->current_file, buf->data, buf->len);

  if (written < 0) {
    LOG_ERR("fs_write failed: %zd", written);
    return (int)written;
  }

  data->current_file_size += written;

  return 0;
}

static int sink_fs_close(const struct device *dev) {
  struct sink_fs_data *data = dev->data;

  return fs_close(&data->current_file);
}

static const struct zstreamer_node_driver_api sink_fs_api = {
    .open = sink_fs_open,
    .process = sink_fs_process,
    .close = sink_fs_close,
};

int sink_fs_set_filename_handler(const struct device *dev,
                                 sink_fs_filename_cb_t cb, void *user_data) {
  struct sink_fs_data *data = dev->data;

  data->filename_cb = cb;
  data->filename_cb_user_data = user_data;

  return 0;
}

#define SINK_FS_DEFINE(inst)                                                   \
  BUILD_ASSERT(DT_INST_PROP(inst, delta_ms_threshold) != 0 ||                  \
                   DT_INST_PROP(inst, size_threshold) != 0,                    \
               "sink-fs: at least one rotation threshold must be "             \
               "non-zero");                                                    \
  Z_ZSTREAMER_NODE_CHILDREN_DEFINE(inst, DT_DRV_INST(inst));                   \
  static K_THREAD_STACK_DEFINE(zstreamer_node_stack_##inst,                    \
                               DT_INST_PROP(inst, thread_stack_size));         \
  static struct sink_fs_data sink_fs_data_##inst = {                           \
      .common = Z_ZSTREAMER_NODE_DATA_INIT(inst, zstreamer_node_stack_##inst), \
  };                                                                           \
  static const struct sink_fs_config sink_fs_config_##inst = {                 \
      .common = {Z_ZSTREAMER_NODE_CONFIG_INIT(                                 \
          inst, DT_DRV_INST(inst), DT_INST_PROP(inst, thread_stack_size),      \
          DT_INST_PROP(inst, thread_priority))},                               \
      .mount_path = DT_INST_PROP(inst, mount_path),                            \
      .delta_ms_threshold = DT_INST_PROP(inst, delta_ms_threshold),            \
      .size_threshold = DT_INST_PROP(inst, size_threshold),                    \
  };                                                                           \
  Z_ZSTREAMER_NODE_INIT_WRAPPER_DEFINE(inst, NULL)                             \
  DEVICE_DT_INST_DEFINE(inst, zstreamer_node_init_##inst, NULL,                \
                        &sink_fs_data_##inst, &sink_fs_config_##inst,          \
                        POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,       \
                        &sink_fs_api);

DT_INST_FOREACH_STATUS_OKAY(SINK_FS_DEFINE)
