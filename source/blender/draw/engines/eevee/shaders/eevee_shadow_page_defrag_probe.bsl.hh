/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * DOWNSTREAM (Android): Bisect probes for `eevee_shadow_page_defrag`.
 *
 * The real defrag pipeline fails to compile on some mobile GPUs
 * (powerVR BXM: vkCreateComputePipelines -> VK_ERROR_UNKNOWN) and Blender
 * substitutes it with a do-nothing pipeline, which leaks the shadow page pool.
 *
 * These probes replicate the defrag algorithm in 4 incremental stages so that
 * the exact construct rejected by the driver can be isolated from logcat:
 *   p0: resets only (stats + indirect command buffers).
 *   p1: p0 + free-from-cache pop over the "old" cached range.
 *   p2: p1 + cache ring compaction loop.
 *   p3: p2 + new-range pop + unsigned wrap-around = full defrag replication.
 *
 * They are Android-gated at the registration level (extra enum entries) and
 * produce no functional change when not built.
 */

#pragma once

#include "draw_shader_shared.hh"
#include "eevee_shadow_page_ops.bsl.hh"

namespace eevee::shadow {

using PageAllocator = eevee::shadow::PageAllocator;
using Statistics = eevee::shadow::Statistics;

struct ProbeCommands {
  [[storage(5, write)]] DispatchCommand &clear_dispatch_buf;
  [[storage(6, write)]] DrawCommandArray &tile_draw_buf;
};

/* `page_mask` from page_ops is undefined at the end of that file. */
#define SHADOW_MAX_PAGE_MASK uint(SHADOW_MAX_PAGE - 1)

/* P0: resets only. Same descriptor set as the real defrag. */
[[compute, local_size(1)]]
void defrag_p0([[resource_table]] PageAllocator &allocator,
               [[resource_table]] ProbeCommands &cmds,
               [[resource_table]] Statistics &stats)
{
  /* Touch allocator to avoid any dead-binding elision. */
  int dummy = int(allocator.pages_infos_buf.page_cached_start);
  stats.statistics_buf.page_used_count = dummy;
  stats.statistics_buf.page_update_count = 0;
  stats.statistics_buf.page_allocated_count = 0;
  stats.statistics_buf.page_rendered_count = 0;
  stats.statistics_buf.view_needed_count = 0;

  cmds.clear_dispatch_buf.num_groups_x = SHADOW_PAGE_RES / SHADOW_PAGE_CLEAR_GROUP_SIZE;
  cmds.clear_dispatch_buf.num_groups_y = SHADOW_PAGE_RES / SHADOW_PAGE_CLEAR_GROUP_SIZE;
  cmds.clear_dispatch_buf.num_groups_z = 0;

  cmds.tile_draw_buf.vertex_len = 0u;
  cmds.tile_draw_buf.instance_len = 1u;
  cmds.tile_draw_buf.vertex_first = 0u;
  cmds.tile_draw_buf.instance_first = 0u;
}

PipelineCompute page_defrag_p0(defrag_p0);

/* P1: p0 + free-from-cache pop over the "old" cached range. */
[[compute, local_size(1)]]
void defrag_p1([[resource_table]] PageAllocator &allocator,
               [[resource_table]] ProbeCommands &cmds,
               [[resource_table]] Statistics &stats)
{
  int additional_pages = allocator.pages_infos_buf.page_alloc_count -
                         allocator.pages_infos_buf.page_free_count;
  uint src = allocator.pages_infos_buf.page_cached_start;
  uint end = allocator.pages_infos_buf.page_cached_end;

  /* find_first_valid */
  while (src < end) {
    if (allocator.pages_cached_buf[src & SHADOW_MAX_PAGE_MASK].x != uint(-1u)) {
      break;
    }
    src++;
  }

  /* Free cached pages until the allocation requirement is met. */
  while (additional_pages > 0 && src < end) {
    uint cached_idx = src & SHADOW_MAX_PAGE_MASK;
    uint tile_index = allocator.pages_cached_buf[cached_idx].y;
    ShadowTileData tile = shadow_tile_unpack(allocator.tiles_buf[tile_index]);

    allocator.page_cache_remove(tile);
    allocator.page_free(tile);
    allocator.tiles_buf[tile_index] = shadow_tile_pack(tile);

    /* find_first_valid */
    src++;
    while (src < end) {
      if (allocator.pages_cached_buf[src & SHADOW_MAX_PAGE_MASK].x != uint(-1u)) {
        break;
      }
      src++;
    }
    additional_pages--;
  }

  allocator.pages_infos_buf.page_cached_start = src;
  allocator.pages_infos_buf.page_cached_end = end;
  allocator.pages_infos_buf.page_alloc_count = 0;

  stats.statistics_buf.page_used_count = 0;
  stats.statistics_buf.page_update_count = 0;
  stats.statistics_buf.page_allocated_count = 0;
  stats.statistics_buf.page_rendered_count = 0;
  stats.statistics_buf.view_needed_count = 0;

  cmds.clear_dispatch_buf.num_groups_x = SHADOW_PAGE_RES / SHADOW_PAGE_CLEAR_GROUP_SIZE;
  cmds.clear_dispatch_buf.num_groups_y = SHADOW_PAGE_RES / SHADOW_PAGE_CLEAR_GROUP_SIZE;
  cmds.clear_dispatch_buf.num_groups_z = 0;
  cmds.tile_draw_buf.vertex_len = 0u;
  cmds.tile_draw_buf.instance_len = 1u;
  cmds.tile_draw_buf.vertex_first = 0u;
  cmds.tile_draw_buf.instance_first = 0u;
}

PipelineCompute page_defrag_p1(defrag_p1);

/* P2: p1 + cache ring compaction loop. */
[[compute, local_size(1)]]
void defrag_p2([[resource_table]] PageAllocator &allocator,
               [[resource_table]] ProbeCommands &cmds,
               [[resource_table]] Statistics &stats)
{
  int additional_pages = allocator.pages_infos_buf.page_alloc_count -
                         allocator.pages_infos_buf.page_free_count;
  uint src = allocator.pages_infos_buf.page_cached_start;
  uint end = allocator.pages_infos_buf.page_cached_end;

  while (src < end) {
    if (allocator.pages_cached_buf[src & SHADOW_MAX_PAGE_MASK].x != uint(-1u)) {
      break;
    }
    src++;
  }

  while (additional_pages > 0 && src < end) {
    uint cached_idx = src & SHADOW_MAX_PAGE_MASK;
    uint tile_index = allocator.pages_cached_buf[cached_idx].y;
    ShadowTileData tile = shadow_tile_unpack(allocator.tiles_buf[tile_index]);

    allocator.page_cache_remove(tile);
    allocator.page_free(tile);
    allocator.tiles_buf[tile_index] = shadow_tile_pack(tile);

    src++;
    while (src < end) {
      if (allocator.pages_cached_buf[src & SHADOW_MAX_PAGE_MASK].x != uint(-1u)) {
        break;
      }
      src++;
    }
    additional_pages--;
  }

  /* Compaction: shift old-range pages backward to fill holes. */
  bool is_empty = (src == end);
  if (!is_empty) {
    for (uint dst = end - 1u; dst > src; dst--) {
      if (allocator.pages_cached_buf[dst & SHADOW_MAX_PAGE_MASK].x != uint(-1u)) {
        continue;
      }
      /* page_cache_update_page_ref */
      uint old_page_idx = src & SHADOW_MAX_PAGE_MASK;
      uint tile_index = allocator.pages_cached_buf[old_page_idx].y;
      allocator.pages_cached_buf[dst & SHADOW_MAX_PAGE_MASK].y = tile_index;
      ShadowTileData tile = shadow_tile_unpack(allocator.tiles_buf[tile_index]);
      tile.cache_index = dst & SHADOW_MAX_PAGE_MASK;
      allocator.tiles_buf[tile_index] = shadow_tile_pack(tile);

      /* Move page. */
      allocator.pages_cached_buf[dst & SHADOW_MAX_PAGE_MASK] =
          allocator.pages_cached_buf[src & SHADOW_MAX_PAGE_MASK];
      allocator.pages_cached_buf[src & SHADOW_MAX_PAGE_MASK] = uint2(~0u);

      /* find_first_valid */
      while (src < dst) {
        src++;
        if (allocator.pages_cached_buf[src & SHADOW_MAX_PAGE_MASK].x != uint(-1u)) {
          break;
        }
      }
    }
  }

  allocator.pages_infos_buf.page_cached_start = src;
  allocator.pages_infos_buf.page_cached_end = end;
  allocator.pages_infos_buf.page_alloc_count = 0;

  stats.statistics_buf.page_used_count = 0;
  stats.statistics_buf.page_update_count = 0;
  stats.statistics_buf.page_allocated_count = 0;
  stats.statistics_buf.page_rendered_count = 0;
  stats.statistics_buf.view_needed_count = 0;

  cmds.clear_dispatch_buf.num_groups_x = SHADOW_PAGE_RES / SHADOW_PAGE_CLEAR_GROUP_SIZE;
  cmds.clear_dispatch_buf.num_groups_y = SHADOW_PAGE_RES / SHADOW_PAGE_CLEAR_GROUP_SIZE;
  cmds.clear_dispatch_buf.num_groups_z = 0;
  cmds.tile_draw_buf.vertex_len = 0u;
  cmds.tile_draw_buf.instance_len = 1u;
  cmds.tile_draw_buf.vertex_first = 0u;
  cmds.tile_draw_buf.instance_first = 0u;
}

PipelineCompute page_defrag_p2(defrag_p2);

/* P2A: p2 but the compaction block uses scalar .x/.y SSBO accesses instead of uint2
 * vector loads/stores. If this compiles, the uint2 vector copy is the PowerVR-rejected op
 * and this exact code can be ported into `eevee_shadow_page_ops.bsl.hh`. */
[[compute, local_size(1)]]
void defrag_p2a([[resource_table]] PageAllocator &allocator,
                [[resource_table]] ProbeCommands &cmds,
                [[resource_table]] Statistics &stats)
{
  int additional_pages = allocator.pages_infos_buf.page_alloc_count -
                         allocator.pages_infos_buf.page_free_count;
  uint src = allocator.pages_infos_buf.page_cached_start;
  uint end = allocator.pages_infos_buf.page_cached_end;

  while (src < end) {
    if (allocator.pages_cached_buf[src & SHADOW_MAX_PAGE_MASK].x != uint(-1u)) {
      break;
    }
    src++;
  }

  while (additional_pages > 0 && src < end) {
    uint cached_idx = src & SHADOW_MAX_PAGE_MASK;
    uint tile_index = allocator.pages_cached_buf[cached_idx].y;
    ShadowTileData tile = shadow_tile_unpack(allocator.tiles_buf[tile_index]);
    allocator.page_cache_remove(tile);
    allocator.page_free(tile);
    allocator.tiles_buf[tile_index] = shadow_tile_pack(tile);

    src++;
    while (src < end) {
      if (allocator.pages_cached_buf[src & SHADOW_MAX_PAGE_MASK].x != uint(-1u)) {
        break;
      }
      src++;
    }
    additional_pages--;
  }

  /* Compaction (scalarized): shift old-range pages backward to fill holes. */
  bool is_empty = (src == end);
  if (!is_empty) {
    for (uint dst = end - 1u; dst > src; dst--) {
      if (allocator.pages_cached_buf[dst & SHADOW_MAX_PAGE_MASK].x != uint(-1u)) {
        continue;
      }
      uint old_page_idx = src & SHADOW_MAX_PAGE_MASK;
      uint dst_page_idx = dst & SHADOW_MAX_PAGE_MASK;
      uint page_coord = allocator.pages_cached_buf[old_page_idx].x;
      uint tile_index = allocator.pages_cached_buf[old_page_idx].y;
      /* Move page, one component at a time. */
      allocator.pages_cached_buf[dst_page_idx].x = page_coord;
      allocator.pages_cached_buf[dst_page_idx].y = tile_index;
      allocator.pages_cached_buf[old_page_idx].x = uint(-1u);
      allocator.pages_cached_buf[old_page_idx].y = uint(-1u);

      /* page_cache_update_page_ref */
      ShadowTileData tile = shadow_tile_unpack(allocator.tiles_buf[tile_index]);
      tile.cache_index = dst_page_idx;
      allocator.tiles_buf[tile_index] = shadow_tile_pack(tile);

      /* find_first_valid */
      while (src < dst) {
        src++;
        if (allocator.pages_cached_buf[src & SHADOW_MAX_PAGE_MASK].x != uint(-1u)) {
          break;
        }
      }
    }
  }

  allocator.pages_infos_buf.page_cached_start = src;
  allocator.pages_infos_buf.page_cached_end = end;
  allocator.pages_infos_buf.page_alloc_count = 0;

  stats.statistics_buf.page_used_count = 0;
  stats.statistics_buf.page_update_count = 0;
  stats.statistics_buf.page_allocated_count = 0;
  stats.statistics_buf.page_rendered_count = 0;
  stats.statistics_buf.view_needed_count = 0;

  cmds.clear_dispatch_buf.num_groups_x = SHADOW_PAGE_RES / SHADOW_PAGE_CLEAR_GROUP_SIZE;
  cmds.clear_dispatch_buf.num_groups_y = SHADOW_PAGE_RES / SHADOW_PAGE_CLEAR_GROUP_SIZE;
  cmds.clear_dispatch_buf.num_groups_z = 0;
  cmds.tile_draw_buf.vertex_len = 0u;
  cmds.tile_draw_buf.instance_len = 1u;
  cmds.tile_draw_buf.vertex_first = 0u;
  cmds.tile_draw_buf.instance_first = 0u;
}

PipelineCompute page_defrag_p2a(defrag_p2a);

/* P3: p2 + new-range pop + wrap-around = full defrag replication. */
[[compute, local_size(1)]]
void defrag_p3([[resource_table]] PageAllocator &allocator,
               [[resource_table]] ProbeCommands &cmds,
               [[resource_table]] Statistics &stats)
{
  int additional_pages = allocator.pages_infos_buf.page_alloc_count -
                         allocator.pages_infos_buf.page_free_count;
  uint src = allocator.pages_infos_buf.page_cached_start;
  uint end = allocator.pages_infos_buf.page_cached_end;

  while (src < end) {
    if (allocator.pages_cached_buf[src & SHADOW_MAX_PAGE_MASK].x != uint(-1u)) {
      break;
    }
    src++;
  }

  while (additional_pages > 0 && src < end) {
    uint cached_idx = src & SHADOW_MAX_PAGE_MASK;
    uint tile_index = allocator.pages_cached_buf[cached_idx].y;
    ShadowTileData tile = shadow_tile_unpack(allocator.tiles_buf[tile_index]);

    allocator.page_cache_remove(tile);
    allocator.page_free(tile);
    allocator.tiles_buf[tile_index] = shadow_tile_pack(tile);

    src++;
    while (src < end) {
      if (allocator.pages_cached_buf[src & SHADOW_MAX_PAGE_MASK].x != uint(-1u)) {
        break;
      }
      src++;
    }
    additional_pages--;
  }

  bool is_empty = (src == end);
  if (!is_empty) {
    for (uint dst = end - 1u; dst > src; dst--) {
      if (allocator.pages_cached_buf[dst & SHADOW_MAX_PAGE_MASK].x != uint(-1u)) {
        continue;
      }
      uint old_page_idx = src & SHADOW_MAX_PAGE_MASK;
      uint tile_index = allocator.pages_cached_buf[old_page_idx].y;
      allocator.pages_cached_buf[dst & SHADOW_MAX_PAGE_MASK].y = tile_index;
      ShadowTileData tile = shadow_tile_unpack(allocator.tiles_buf[tile_index]);
      tile.cache_index = dst & SHADOW_MAX_PAGE_MASK;
      allocator.tiles_buf[tile_index] = shadow_tile_pack(tile);

      allocator.pages_cached_buf[dst & SHADOW_MAX_PAGE_MASK] =
          allocator.pages_cached_buf[src & SHADOW_MAX_PAGE_MASK];
      allocator.pages_cached_buf[src & SHADOW_MAX_PAGE_MASK] = uint2(~0u);

      while (src < dst) {
        src++;
        if (allocator.pages_cached_buf[src & SHADOW_MAX_PAGE_MASK].x != uint(-1u)) {
          break;
        }
      }
    }
  }

  /* Pop from the (compact) new range. */
  end = allocator.pages_infos_buf.page_cached_next;
  while (additional_pages > 0 && src < end) {
    uint cached_idx = src & SHADOW_MAX_PAGE_MASK;
    uint tile_index = allocator.pages_cached_buf[cached_idx].y;
    ShadowTileData tile = shadow_tile_unpack(allocator.tiles_buf[tile_index]);

    allocator.page_cache_remove(tile);
    allocator.page_free(tile);
    allocator.tiles_buf[tile_index] = shadow_tile_pack(tile);
    additional_pages--;
    src++;
  }

  allocator.pages_infos_buf.page_cached_start = src;
  allocator.pages_infos_buf.page_cached_end = end;
  allocator.pages_infos_buf.page_alloc_count = 0;

  /* Wrap the cursor to avoid unsigned overflow. */
  const uint max_page = uint(SHADOW_MAX_PAGE);
  if (src > max_page) {
    allocator.pages_infos_buf.page_cached_next -= max_page;
    allocator.pages_infos_buf.page_cached_start -= max_page;
    allocator.pages_infos_buf.page_cached_end -= max_page;
  }

  stats.statistics_buf.page_used_count = 0;
  stats.statistics_buf.page_update_count = 0;
  stats.statistics_buf.page_allocated_count = 0;
  stats.statistics_buf.page_rendered_count = 0;
  stats.statistics_buf.view_needed_count = 0;

  cmds.clear_dispatch_buf.num_groups_x = SHADOW_PAGE_RES / SHADOW_PAGE_CLEAR_GROUP_SIZE;
  cmds.clear_dispatch_buf.num_groups_y = SHADOW_PAGE_RES / SHADOW_PAGE_CLEAR_GROUP_SIZE;
  cmds.clear_dispatch_buf.num_groups_z = 0;
  cmds.tile_draw_buf.vertex_len = 0u;
  cmds.tile_draw_buf.instance_len = 1u;
  cmds.tile_draw_buf.vertex_first = 0u;
  cmds.tile_draw_buf.instance_first = 0u;
}

PipelineCompute page_defrag_p3(defrag_p3);

}  // namespace eevee::shadow