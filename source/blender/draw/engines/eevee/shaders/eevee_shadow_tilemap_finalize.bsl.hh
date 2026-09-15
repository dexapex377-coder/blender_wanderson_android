/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "draw_shader_shared.hh"
#include "eevee_defines.hh"
#include "eevee_shadow_shared.hh"

#include "eevee_shadow_tilemap_lib.bsl.hh"
#include "gpu_shader_math_matrix_projection_lib.glsl"

namespace eevee::shadow {

/**
 * Select the smallest viewport that can contain the given rect of tiles to render.
 * Returns the viewport index.
 */
int viewport_select(int2 rect_size)
{
  /* TODO(fclem): Experiment with non squared viewports. */
  int max_dim = max(rect_size.x, rect_size.y);
  /* Assumes max_dim is non-null. */
  int power_of_two = findMSB(uint(max_dim));
  if ((1 << power_of_two) != max_dim) {
    power_of_two += 1;
  }
  return power_of_two;
}

struct TilemapFinalize {
  [[storage(0, read)]] const ShadowTileMapData (&tilemaps_buf)[];
  [[storage(1, read)]] const uint (&tiles_buf)[];
  [[storage(2, read_write)]] ShadowPagesInfoData &pages_infos_buf;
  [[storage(3, read_write)]] ShadowStatistics &statistics_buf;
  [[storage(4, write)]] ViewMatrices (&view_infos_buf)[SHADOW_VIEW_MAX];
  [[storage(5, write)]] ShadowRenderView (&render_view_buf)[SHADOW_VIEW_MAX];
  [[storage(6, read)]] const ShadowTileMapClip (&tilemaps_clip_buf)[];
  [[image(0, write, UINT_32)]] uimage2D tilemaps_img;

  [[shared]] int rect_min_x;
  [[shared]] int rect_min_y;
  [[shared]] int rect_max_x;
  [[shared]] int rect_max_y;
  [[shared]] uint lod_rendered;
};

/**
 * Virtual shadow-mapping: Tile-map to texture conversion.
 *
 * For all visible light tile-maps, copy page coordinate to a texture.
 * This avoids one level of indirection when evaluating shadows and allows
 * to use a sampler instead of a SSBO bind.
 */
[[compute, local_size(SHADOW_TILEMAP_RES, SHADOW_TILEMAP_RES)]]
void tilemap_finalize_main([[resource_table]] TilemapFinalize &srt,
                           [[global_invocation_id]] const uint3 global_id,
                           [[local_invocation_index]] const uint local_index)

{
  /* DOWNSTREAM (Android): TRIVIAL BODY probe. Finalize workgroups never start (finS=0 with
   * allocG>0 in prior builds). If this fires, the pipeline schedules fine and the full body
   * is what trips the driver. If it does not, the driver refuses to schedule this pipeline
   * even with an empty body. */
  if (local_index == 0u) {
    atomicAdd(srt.pages_infos_buf._pad2, 1);
    atomicAdd(srt.statistics_buf.diag_finalize_groups, 1);
  }
}


struct RendermapFinalize {
  [[storage(0, read_write)]] ShadowStatistics &statistics_buf;
  [[storage(1, read)]] const ShadowRenderView (&render_view_buf)[SHADOW_VIEW_MAX];
  [[storage(2, read_write)]] uint (&tiles_buf)[];
  [[storage(3, read_write)]] DispatchCommand &clear_dispatch_buf;
  [[storage(4, read_write)]] DrawCommandArray &tile_draw_buf;
  [[storage(5, write)]] uint (&dst_coord_buf)[SHADOW_RENDER_MAP_SIZE];
  [[storage(6, write)]] uint (&src_coord_buf)[SHADOW_RENDER_MAP_SIZE];
  [[storage(7, write)]] uint (&render_map_buf)[SHADOW_RENDER_MAP_SIZE];
};

/**
 * Virtual shadow-mapping: Tile-map to render-map conversion.
 *
 * For each shadow view, copy page atlas location to the indirection table before render.
 */
[[compute, local_size(SHADOW_TILEMAP_RES, SHADOW_TILEMAP_RES)]]
void rendermap_finalize_main([[resource_table]] RendermapFinalize &srt,
                             [[global_invocation_id]] const uint3 global_id)
{
  int view_index = int(global_id.z);
  /* Dispatch size if already bounded by SHADOW_VIEW_MAX. */
  if (view_index >= srt.statistics_buf.view_needed_count) {
    return;
  }

  int2 rect_min = srt.render_view_buf[view_index].rect_min;
  int tilemap_tiles_index = srt.render_view_buf[view_index].tilemap_tiles_index;
  int lod = srt.render_view_buf[view_index].tilemap_lod;
  int2 viewport_size = shadow_viewport_size_get(srt.render_view_buf[view_index].viewport_index);

  int2 tile_co = int2(global_id.xy);
  int2 tile_co_lod = tile_co >> lod;
  bool lod_valid_thread = all(equal(tile_co, tile_co_lod << lod));

  int tile_index = shadow_tile_offset(uint2(tile_co_lod), tilemap_tiles_index, lod);

  if (lod_valid_thread) {
    ShadowTileData tile = shadow_tile_unpack(srt.tiles_buf[tile_index]);
    /* Tile coordinate relative to chosen viewport origin. */
    int2 viewport_tile_co = tile_co_lod - rect_min;
    /* We need to add page indirection to the render map for the whole viewport even if this one
     * might extend outside of the shadow-map range. To this end, we need to wrap the threads to
     * always cover the whole mip. This is because the viewport cannot be bigger than the mip
     * level itself. */
    int lod_res = SHADOW_TILEMAP_RES >> lod;
    int2 relative_tile_co = (viewport_tile_co + lod_res) % lod_res;
    if (all(lessThan(relative_tile_co, viewport_size))) {
      bool do_page_render = tile.is_used && tile.do_update;
      uint page_packed = shadow_page_pack(tile.page);
      /* Add page to render map. */
      int render_page_index = shadow_render_page_index_get(view_index, relative_tile_co);
      srt.render_map_buf[render_page_index] = do_page_render ? page_packed : 0xFFFFFFFFu;

      if (do_page_render) {
        /* Add page to clear dispatch. */
        uint page_index = atomicAdd(srt.clear_dispatch_buf.num_groups_z, 1u);
        /* Add page to tile processing. */
        atomicAdd(srt.tile_draw_buf.vertex_len, 6u);
        /* Add page mapping for indexing the page position in atlas and in the frame-buffer. */
        srt.dst_coord_buf[page_index] = page_packed;
        srt.src_coord_buf[page_index] = packUvec4x8(
            uint4(int4(relative_tile_co.x, relative_tile_co.y, view_index, 0)));
        /* Tag tile as rendered. Should be safe since only one thread is reading and writing. */
        srt.tiles_buf[tile_index] |= SHADOW_IS_RENDERED;
        /* Statistics. */
        atomicAdd(srt.statistics_buf.page_rendered_count, 1);
      }
    }
  }
}

PipelineCompute tilemap_finalize(eevee::shadow::tilemap_finalize_main);
PipelineCompute tilemap_rendermap(eevee::shadow::rendermap_finalize_main);

}  // namespace eevee::shadow
