/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/init.h>
#include <zephyr/storage/flash_map.h>

#define TEST_PARTITION lfs_partition
#define TEST_PARTITION_ID FIXED_PARTITION_ID(TEST_PARTITION)

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(lfs_storage);

static struct fs_mount_t lfs_mnt = {
    .type = FS_LITTLEFS,
    .fs_data = &lfs_storage,
    .storage_dev = (void *)TEST_PARTITION_ID,
    .mnt_point = "/lfs",
};

static int mount_littlefs(void) {
  int ret;

  ret = fs_mount(&lfs_mnt);
  if (ret == -EINVAL) {
    /* Unformatted — format and retry */
    ret = fs_mkfs(FS_LITTLEFS, (uintptr_t)TEST_PARTITION_ID, NULL, 0);
    if (ret != 0) {
      return ret;
    }
    ret = fs_mount(&lfs_mnt);
  }

  return ret;
}

/* Run at APPLICATION level, after flash + LittleFS registration (POST_KERNEL)
 */
SYS_INIT(mount_littlefs, APPLICATION, 0);
