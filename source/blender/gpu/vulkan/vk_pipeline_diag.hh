/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Diagnostic logging for Vulkan pipeline/module compilation on Android.
 *
 * Every module/pipeline compile result is appended to a file in the caches dir
 * (`vk-pipeline-diag.txt`) and echoed to logcat under the tag `blender-pipe-diag`.
 * This lets a build run unattended and dump every shader/pipeline the driver
 * accepts or rejects (the Android driver refuses several with VK_ERROR_UNKNOWN
 * and no diagnostics of its own).
 */

#pragma once

#include "BKE_appdir.hh"

#include "BLI_fileops.hh"
#include "BLI_path_utils.hh"

#include "vk_common.hh"

#include <cstdarg>
#include <cstdio>
#include <string>

#ifdef __ANDROID__
#  include <android/log.h>
#endif

namespace blender::gpu {

inline void vk_pipeline_diag_log_line(const std::string &line)
{
#ifdef __ANDROID__
  __android_log_print(ANDROID_LOG_INFO, "blender-pipe-diag", "%s", line.c_str());
#else
  fprintf(stderr, "%s\n", line.c_str());
#endif

  static char diag_dir[FILE_MAX];
  if (diag_dir[0] == '\0') {
    BKE_appdir_folder_caches(diag_dir, sizeof(diag_dir));
    BLI_dir_create_recursive(diag_dir);
  }
  const std::string diag_file = std::string(diag_dir) + SEP_STR + "vk-pipeline-diag.txt";
  FILE *fp = BLI_fopen(diag_file.c_str(), "a");
  if (fp) {
    fprintf(fp, "%s\n", line.c_str());
    fclose(fp);
  }
}

inline void vk_pipeline_diag_logf(const char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  char buf[2048];
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  vk_pipeline_diag_log_line(buf);
}

}  // namespace blender::gpu