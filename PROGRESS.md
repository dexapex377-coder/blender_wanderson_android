# PROGRESS — Blender Android Port (PowerVR BXM-8-256 / Dimensity 7060)

## Device Profile
- SoC: MediaTek Dimensity 7060 (mt6855)
- GPU: PowerVR BXM-8-256, vulkan.mtk.so 25.1
- Vulkan: apiVersion 1.3.30, physical api 1.3.30, conformant 1.4.10
- `maxComputeSharedMemorySize`: **16384 (16KB)** ← critical limit
- `maxComputeWorkGroupInvocations`: 1024
- `fillModeNonSolid`: false
- `textureCompressionBC`: false
- `shaderFloat64`: false
- Extensions OK: dynamic_rendering, push_descriptor, spirv_1_4, 16bit_storage, buffer_device_address, variable_pointers, scalar_block_layout, robustness2

---

## Fix 1: SPIR-V 1.3 for all Android [APPLIED, BUILD OK]

**File**: `source/blender/gpu/vulkan/vk_shader_compiler.cc`
**Commit**: `3897f55` + `7223b6c` (workflow fixes)
**Status**: BUILD SUCCESS (`34478454273`, 1h21m), APK installed
**Result**: SPIR-V 1.3 no resolvió el cubo negro — confirmado que el problema NO es la versión SPIR-V.

`compile_for_vulkan_11()` returns true under `#ifdef __ANDROID__` → vulkan_1_1 env / SPIR-V 1.3
for all Android devices regardless of reported apiVersion.

---

## Fix 2: Z-bin shared memory overflow [APPLIED, NOT YET BUILT]

**File**: `source/blender/draw/engines/eevee/eevee_light_shared.hh:265`
**Root cause**: `CULLING_ZBIN_COUNT=4096` → 2 arrays × 4096 × 4 bytes = **32KB shared memory**,
exceeding the device limit of 16KB by 2×. This is the FIRST compute pass of EEVEE (Z-bin
light culling). When `vkCreateComputePipelines` fails, EEVEE has no light culling → everything
renders black. The driver eventually reports device lost → app exits cleanly.

**Evidence**: [CONFIRMED — device dump maxComputeSharedMemorySize=16384 + shader code eevee_light_culling.bsl.hh:200-201]

**Fix**: `CULLING_ZBIN_COUNT` 4096 → 1024 (8KB shared memory, 50% margin). Must be multiple
of `CULLING_ZBIN_GROUP_SIZE=1024` for the iteration to work correctly (zbin_iter=1).
Only affects z-resolution for >1024 lights (rare on mobile).

**Risk**: LOW (loss of z-precision only with >1024 lights; scenes with >1024 point/spot lights
are extremely rare on mobile).

---

## Fix 3: IrradianceSum shared memory — EXACT 16KB [NOT YET BUILT]

**File**: `source/blender/draw/engines/eevee/shaders/eevee_lightprobe_sphere_bake.bsl.hh:404`
**Root cause**: `float4 local_sh_coefs[SPHERE_PROBE_SH_GROUP_SIZE][4]` with
`SPHERE_PROBE_SH_GROUP_SIZE=256` = 256 × 4 × 4 bytes = **16384 bytes = exactly the device
limit**. The sibling shader `remap` was already reduced from a similar 16KB layout due to
"loses the device" (eevee_defines.hh:37-44), but `irradiance_sum` was left at exactly the
limit. Risk of pipeline failure is HIGH.

**Evidence**: [CONFIRMED by code — 16KB = exact device limit; failure mode is hypothesis from
similar zbin pattern]

**Proposed fix**: Reduce `SPHERE_PROBE_SH_GROUP_SIZE` from 256 to 128 (8KB shared memory).
Update `iter_count` in `eevee_lightprobe_sphere_bake.bsl.hh:421` from
`SPHERE_PROBE_MAX_HARMONIC/group_size` accordingly.

**Risk**: LOW (halves SH reconstruction quality per pass; may need 2 passes instead of 1,
but correctness is preserved).

---

## Findings: Workbench 19fps [NO FIX — design limitation]

### Root cause (CONFIRMED): No mobile optimization whatsoever

Workbench renders at **full native resolution** (~1080×2340) with RGBA16F everywhere, no
dynamic resolution reduction, no mobile-specific quality branching. ~180-200MB of framebuffer
traffic per frame. The TBDR GPU is overwhelmed by bandwidth.

**Multiplicative factor — TAA×8**: Each "final frame" = 8 complete engine draws + 8 GPU
submits + 8 WM loop iterations (viewport_aa=8). During idle/editing, all 8 samples execute
full engine + generate_commands CPU loop + GPU upload per sample.

**CPU overhead on Android**: `generate_commands` CPU loop re-executes per sample (view
fingerprint changes with TAA jitter). `compute_visibility` GPU dispatch runs but results
are IGNORED (`UNUSED_VARS(visibility_buf)` in draw_command.cc:860) — wasted work every sample.

**Zero mobile path exists**: No `GPU_OS_ANDROID`, no quality branching, no resolution
scaling, no AA reduction in `draw/engines/workbench/`. Nomad Sculpt (PBR, 60fps) vs
Workbench (basic shading, 19fps) on same device → confirms the issue is Blender's pipeline
design, not GPU capability.

**Key files**:
- `draw/engines/workbench/workbench_engine.cc:523-525` (TAA redraw gate)
- `draw/intern/draw_command.cc:858-947` (CPU generate_commands, per-sample)
- `draw/intern/draw_view.cc:40-44` (view fingerprint changes per sample)
- `draw/intern/draw_manager.cc:426-429` (generate_commands dispatch per sample)

### Candidate fixes (require user approval — NOT applied):
1. **Reduce viewport_aa to 1-4** on Android (most impactful, reduces workload 2-8×)
2. **0.5× dynamic resolution** for mobile (halve framebuffer traffic)
3. **Skip compute_visibility** on Android when results are discarded anyway
4. **Skip generate_commands re-execution** when only the TAA jitter changed

---

## Findings: EEVEE Next additional [NO FIX — diagnosis only]

### SPHERE_PROBE_ATLAS hardcoded 4096² (CONFIRMED, MEDIUM risk)

`eevee_defines.hh:53`: `SPHERE_PROBE_ATLAS_RES = 4096` → RGBA16F × 4096² × 5 mips ≈ 134MB
per layer. World layer always allocated. No mobile cap.

**Proposed fix**: Clamp to 1024² on `__ANDROID__` (reduce to ~8MB per layer).

### GPU_finish() in bake path (CONFIRMED, MEDIUM risk — bake only)

`eevee_instance.cc:1015`: `GPU_finish()` inside irradiance/volume bake adaptive-timing loop.
Runs once per sample batch. Only affects bake operations, not runtime rendering.

### GPU_flush() per shadow update iteration (CONFIRMED, LOW risk)

`eevee_shadow.cc:1370,1380`: Only fires when `loop_count != 0` (≥2 iterations). Acceptable.

### fillModeNonSolid: false (CONFIRMED, NO RISK)

No EEVEE dependency on non-solid fill. Wireframe/overlay paths are outside this engine.

---

## Summary: Fixes for next build

| # | Fix | File | Risk | Status |
|---|-----|------|------|--------|
| 1 | SPIR-V 1.3 all Android | vk_shader_compiler.cc | LOW | Applied + built (not root cause) |
| 2 | CULLING_ZBIN_COUNT 4096→1024 | eevee_light_shared.hh | LOW | Build `34527206281` |
| 3 | SPHERE_PROBE_SH_GROUP_SIZE 256→128 + reduction loops 10→8 | eevee_defines.hh + eevee_lightprobe_sphere_bake.bsl.hh | LOW | Build `34527206281` |
| 4 | TAA clamp a 1 sample en `__ANDROID__` (idle fps) | workbench_state.cc:223-229 | LOW | Build `34527206281` |
| 5 | renderdiv fraccionario (float via atof) | GHOST_AndroidMemoryTier.hh + SystemAndroid + WindowAndroid | LOW | Build `34527206281` |
| 6 | `is_dirty=true` en discard_active_pool (fix null-deref MTK Sig11 +0x38) | vk_descriptor_pools.cc:68-75 | LOW | Build `34527206281` |

See section below: **Hito — 6-fix bundle `b22cc3e` (run `34527206281`)**

Pending user approval for:
- Sphere probe atlas cap on Android
- Workbench dynamic resolution (renderdiv auto, ya existe vía getprop)

---

## Investigación: FPS del timeline vs. gfxinfo (bottleneck real) — [VERIFICADO]

### El "FPS" de Blender NO es render time real

El FPS del timeline/animation player es **tiempo de pared del game loop** (evaluate + Python +
physics + draw + swap + compositor + display pacing), no tiempo de GPU puro.

**Hallazgo crítico** — `workbench_state.cc:219-222`:
```cpp
if (is_navigating || is_playback) {
    /* Only draw using SMAA or no AA when navigating. */
    _samples_len = min_ii(_samples_len, 1);
}
```
Durante **playback TAA ya está capado a 1 sample** → el TAA×8 NO es el bottleneck del FPS en
reproducción. El 19fps es el draw de paso único a resolución full (RGB16F) + evaluar + swap +
SurfaceFlinger pacing. Reducir `viewport_aa` NO mejorará el FPS de playback (solo edición idle).

### Medición real (Android): comandos

```bash
adb shell dumpsys gfxinfo org.blender.blender framestats   # per-frame: draw/duration/ready/jank
adb shell dumpsys gfxinfo org.blender.blender              # resumen: Total frames, Janky frames, p95/p99
adb shell dumpsys SurfaceFlinger --latency                 # por-surface (cubre SurfaceView del GLNDK)
```
- `framestats`: per-frame (api≥26, ventanas del app). Útil para ver `DrawDuration`, `ReadyDuration`,
  `Janky frames`, p90/p95/p99.
- `SurfaceFlinger --latency`: mide el surface real presentado por SurfaceFlinger (mejor si Blender
  dibuja en SurfaceView/ANativeWindow vía GLNDK).
- **Lección**: si `dumpsys gfxinfo` muestra que el frame de la app es ~16ms pero el FPS del timeline
  dice 19, el cuello de botella es el LOOP de Blender (evaluate) o pacing de SurfaceFlinger, no el render.

---

## Investigación: ¿MSAA por hardware es viable en Workbench? — [NO RECOMENDADO]

### Soporte de device (CONFIRMADO — dump Vulkan)

- `framebufferColorSampleCounts: 15` → bits 0-3 = **soporta 1/2/4/8 samples** (MSAA 8x máximo).
- `shaderStorageImageMultisample: true`, `variableMultisampleRate: false`.
- `VK_EXT_multisampled_render_to_single_sampled` disponible + `GL_EXT_multisampled_render_to_texture`
  → la ruta eficiente TBDR (resolver en tile memory) existe.
- **El hardware NO es el problema.**

### El GPU module de Blender 4.x NO tiene MSAA — [CONFIRMADO por código]

- `gpu_framebuffer.hh`: `GPUAttachment {tex, layer, mip}` — **no existe parámetro de samples**.
- `GPU_texture.hh`: sin `samples` en la API de creación.
- `vk_render_pass_fallback.hh:11` (literal): *"All images in the Vulkan backend are single-sample,
  so there is no multisample..."*
- `vk_graphics_pipeline.hh:325`: `VkPipelineMultisampleStateCreateInfo` hardcodeado a
  `VK_SAMPLE_COUNT_1_BIT`.
- `vk_texture.cc:729`, `vk_texture_pool.cc:392`, `vk_render_pass_fallback.cc:44,63`,
  `vk_memory_pool.cc:37`: todo `VK_SAMPLE_COUNT_1_BIT`.
- `overlay_antialiasing.hh:14`: el engine Overlay usa jitter AA **explicitamente para evitar
  el coste de MSAA**.
- Blender **eliminó** MSAA del GPU module en el rewrite del draw-manager v2 (4.x). EEVEE Next
  usa solo TAA; la GUI/Overlay usa SMAA/jitter. No hay infraestructura que reutilizar.

### Coste de refactor para MSAA por hardware (estimación)

1. API `GPU_texture` + `gpu::Texture` con `samples` (interface + 3 backends) ~150 líneas
2. `GPU_framebuffer` con attachment MSAA + `GPU_framebuffer_blit`/resolve ~150 líneas
3. `GPUOffScreen` multisample ~50 líneas
4. `VKFrameBuffer`/`gl_framebuffer` crear attachments usados por el viewport + resolve state ~100 líneas
5. Draw manager: viewport fb MSAA ~50 líneas
6. Workbench: render a fb MSAA → resolve → TAA accumulation ~80 líneas (y el resto de engines)
7. **Riesgo**: tocar la abstracción central usada por TODOS los engines (compositor, GPencil,
   overlay, select, workbench, eevee) → alto riesgo de regresión en un runtime inestable de PowerVR.

**Total**: ~600+ líneas en ~15 archivos, semanas de trabajo, riesgo alto. No es "bajo riesgo".

### Por qué MSAA además NO resuelve el bottleneck

- El cuello de botella es **bandwidth/fill de fragmentos** en resolución full con RGBA16F.
  MSAA en PowerVR TBDR se resuelve en tile memory (poco DRAM extra) pero **multiplica el fragment
  shading × samples** — empeoraría un GPU ya al límite de fill.
- MSAA solo arregla **aliasing geométrico**. NO arregla aliasing de sombreado (texturas, normales
  sub-píxel, ruido de sombras) ni aliasing temporal → **TAA sigue siendo necesario encima**.
- Por eso los estudios reales usan TAA como pasada final, no MSAA.
- Además: durante playback TAA ya está a 1 sample → MSAA reemplazaría SMAA ahí, pero el problema
  del FPS de playback no es el AA.

### Veredicto

| Opción | Esfuerzo | Riesgo | Impacto en FPS playback |
|--------|----------|--------|--------------------------|
| Reducir viewport_aa 8→4 (candidato #1) | ~5 líneas | nulo | Ninguno (playback ya es 1 sample) |
| MSAA 4x por hardware | ~600+ líneas, 15 archivos | alto (GPU module core) | Negativo (más fill) |
| **Resolution scaling** (`debug.blender.renderdiv 2` o auto low-mem) | **YA EXISTE** en `GHOST_AndroidMemoryTier.hh` | nulo | **El fix real** |

**Recomendación**: verificar primero con `dumpsys gfxinfo` si el bottleneck es render GPU o loop CPU.
Luego, si es render: usar resolution scaling (ya implementado, `debug.blender.renderdiv 2` en el
moto g56 y auto-divisor 2 si el device tiene <6GB RAM). El `viewport_aa` se puede bajar como bonus
para la edición idle, pero NO tocará el frame pacing de playback. Descartar MSAA por hardware:
no hay plumbing en el GPU module, costaría semanas y empeoraría el fill-bottleneck.

**Doc confirmación**: `vk_render_pass_fallback.hh:11` (single-sample), Device dump
`framebufferColorSampleCounts: 15`, `workbench_state.cc:219-222` (TAA cap en playback).

---

## Investigación: FPS bajo en vista normal — TAA×8 idle [CAUSA RAÍZ ENCONTRADA]

### Prueba empírica del usuario (device, renderdiv=2 activo, addon viewport fps)
- Vista normal idle: **<20fps, degradando hasta <20**
- Durante **box select**: **sube a ~50fps**
- `dumpsys SurfaceFlinger`: frame counter presentando 42-67fps (Android recibe más frames
  de los que el render produce — triple buffering amortigua; el addon es ground truth)

### Mecanismo (CONFIRMADO por código)
- `workbench_state.cc:212`: `_samples_len = U.viewport_aa` (default 8).
- `workbench_state.cc:261-267` + `workbench_engine.cc:523`: en idle, cada frame presentado
  corre **8 renders completos del engine** (`scene_state_.sample + 1 < samples_len` →
  `DRW_viewport_request_redraw()`).
- `workbench_state.cc:219-222`: durante navegación/playback/box select (modal), `_samples_len`
  se capea a 1 → **1 solo render por frame** → 50fps.
- CONCLUSIÓN: la GPU SÍ puede renderear la escena a ~50fps (box select lo prueba). El 8×
  de TAA idle es la amplificación que mata el FPS. NO es el loop CPU ni fill de fragmentos.

### Fix aplicado (candidate #1, ~5 líneas)
- **File**: `workbench_state.cc:219-225` → clamp `__ANDROID__`: `_samples_len = min_ii(_samples_len, 1)`.
- Efecto: idle == perfil de navegación (1 sample + SMAA final como AA).
- Estado: **APLICADO, NO BUILT** (pendiente build con los fixes EEVEE).

### Estatus del protocolo del usuario
1. ✅ `getprop debug.blender.lowmem` / `debug.blender.renderdiv` → vacíos (default)
2. ✅ Device 7.8GB RAM → auto-divisor <6GB NO dispara → divisor 1 por defecto (confirmado)
3. ✅ renderdiv=2 activado manualmente (setprop OK, buffers 1142x540 confirmados en SF)
4. ✅ Medición div=2: addon <20fps idle, 50fps box select; SF present 42-67fps
5. ⚠️ divisor 2 por sí solo NO movió el FPS idle del addon (bottleneck no era fill)
6. → Bottleneck real: **TAA×8 idle** (8 renders por presentado) — fix: clamp viewport_aa Android

---

## HITO — 6-fix bundle `b22cc3e` (run `34527206281`, 2026-09-10)

Build consolidado que junta todas las rondas de diagnóstico anteriores: EEVEE (cubo negro) +
Workbench (fps idle) + MemoryTier (renderdiv) + el fix de raíz del **crash null-deref del driver
MTK** (SIGSEGV +0x38). Commit `b22cc3e` en `main`, push a `fork` OK, build disparado
`gh workflow run build-android.yml --ref main`.

### Fix 1 — SPIR-V 1.3 Android [YA APLICADO previamente, build `34478454273`]
Neutral en este build (ya funcionaba). Confirmado en `vk_shader_compiler.cc` git-residente.

### Fix 2 — `CULLING_ZBIN_COUNT` 4096→1024 [fix cubo negro EEVEE]
- **File**: `eevee_light_shared.hh` → `#define CULLING_ZBIN_COUNT 1024`
- 2 arrays × 4096 × 4B = 32KB shared mem > límite 16KB del device → `vkCreateComputePipelines`
  falla → EEVEE sin light culling → **todo negro**. Con 1024 → 8KB (50% margen).
- Verificado en árbol: `grep -c "#define CULLING_ZBIN_COUNT 1024"` = 1. ✅

### Fix 3 — `SPHERE_PROBE_SH_GROUP_SIZE` 256→128 [fix preventivo EEVEE]
- **Files**: `eevee_defines.hh` (`#define SPHERE_PROBE_SH_GROUP_SIZE 128`) +
  `eevee_lightprobe_sphere_bake.bsl.hh` (group_size 128, loop de suma paralela 10→8 iteraciones).
- 256×4×4B = 16384B = **exacto al límite** del device (spec: shared mem DEBE ser < límite;
  = límite es defecto). Con 128 → 8KB seguro.
- Verificado: `grep -c "#define SPHERE_PROBE_SH_GROUP_SIZE 128"` = 1. ✅

### Fix 4 — TAA clamp a 1 sample en `__ANDROID__` [fix fps Workbench idle]
- **File**: `workbench_state.cc:223-229` — `_samples_len = min_ii(_samples_len, 1)` bajo
  `#ifdef __ANDROID__`. Idle == perfil de navegación (1 render por frame presentado).
- Justificado por la prueba empírica (box select 50fps vs idle <20fps) → la GPU SÍ renderiza
  la escena a ~50fps; el 8× de TAA idle era la amplificación.
- Verificado: `grep -c "__ANDROID__" workbench_state.cc` = 1. ✅

### Fix 5 — renderdiv fraccionario (1.5 float) [nitidez/media res]
- **Files**: `GHOST_AndroidMemoryTier.hh` (parseo `atof`, rango 1.0-4.0), `GHOST_SystemAndroid.cc` +
  `GHOST_WindowAndroid.cc` (usar el float, no int).
- Permite `setprop debug.blender.renderdiv 1.5`: nitidez intermedia entre 1 y 2. Antes solo int.
- Verificado: `grep -c "atof"` = 1. ✅
- Default sin getprop: divisor 1 (sin cambio de comportamiento) / auto <6GB RAM → 2 (sin cambio).

### Fix 6 — `is_dirty=true` en `discard_active_pool` [CRASH ROOT-CAUSE FIX]
- **File**: `source/blender/gpu/vulkan/vk_descriptor_pools.cc:68-75`
```cpp
void VKDescriptorPools::discard_active_pool(VKContext &context)
{
  context.discard_pool.discard_descriptor_pool_for_reuse(vk_descriptor_pool_, this);
  vk_descriptor_pool_ = VK_NULL_HANDLE;
  /* All descriptor sets of the discarded pool (including the one VKDescriptorSetTracker caches
   * for reuse) become invalid once this pool is recycled and reset by the submission thread.
   * Force a state refresh so the next draw re-allocates instead of reusing a stale handle. */
  context.state_manager_get().is_dirty = true;
}
```
- **Crash confirmado por traza real** (sesión larga, 13:39:11.5): `signal 11 (SIGSEGV)`,
  `fault addr 0x0000000000000038`, null deref en `vulkan.mtk.so`, muere en `vkUpdateDescriptorSets`
  dentro de `VKDescriptorSetPoolUpdator::upload_descriptor_sets()+312` ← `flush_render_graph` ←
  `swap_buffer_draw_handler` ← `GHOST_ContextVK::swapBufferRelease` ← `wm_draw_update` ← `android_main`.
  Firma **idéntica** a la documentada por Wanderson.
- **Root cause**: `descriptor_sets.vk_descriptor_set` se cachea (vk_descriptor_set.cc:51-59, reuse si
  `!is_dirty && layout ==`) pero el pool del que se alocó se descarta y **resetea asíncronamente**
  (`vkResetDescriptorPool` desde el thread de submission vía `recycle()` → vk_descriptor_pools.cc:74-80,
  disparado por el timeline en `vk_resource_pool.cc:168-171`). El handle cacheado queda stale → el
  próximo draw reusa un descriptor set inexistente → driver MTK null-deref.
- **Por qué la Opción A es suficiente** (vs. version counter en el tracker):
  - El tracker es **singleton por contexto** y cachea **un solo set**.
  - `discard_active_pool` es la **única** ruta hacia el reciclaje/reset (solo se llama desde
    `allocate()` cuando el pool llena 250 sets).
  - Coste = **1 `vkAllocateDescriptorSets` extra** tras cada pool-swap (~µs CPU), no escala con draws.
  - `is_dirty` solo se lee para decidir el reuse (vk_descriptor_set.cc:53) → cero efectos colaterales
    en state binding de otras plataformas.
  - Fix genérico (no `__ANDROID__`): el mismo código Vulkan corre en desktop; los drivers de PC
    simplemente toleran el handle stale. Válido para upstream.
- **Chequeo de sanidad**: aplicado tras verificar acceso (`state_manager_get()` no-const +
  `is_dirty` público). Verificado en árbol. ✅

### Chequeo de sanidad de los 6 (grep, pre-build)
| Fix | verificador | resultado |
|-----|-------------|-----------|
| 1 SPIR-V 1.3 | git show `3897f55` vk_shader_compiler.cc | ✅ residente |
| 2 zbin 1024 | grep `#define CULLING_ZBIN_COUNT 1024` | ✅ |
| 3 sphere 128 | grep `#define SPHERE_PROBE_SH_GROUP_SIZE 128` | ✅ |
| 4 TAA clamp | grep `__ANDROID__` en workbench_state.cc | ✅ |
| 5 renderdiv float | grep `atof` en GHOST_AndroidMemoryTier.hh | ✅ |
| 6 is_dirty discard | grep A2 en vk_descriptor_pools.cc | ✅ |

### Protocolo de prueba post-instalación
1. **Workbench idle con renderdiv=1.5**: `setprop debug.blender.renderdiv 1.5` → fps + nitidez visual.
2. **EEVEE**: confirmar que ya no está negro; evaluar sombras/probes.
3. **Preferencias**: confirmar que sigue sin el bug de 4GB RAM.
4. **Sesión larga (20-30+ min)**: mezcla idle + interacción activa, logcat capturando en background
   (el crash previo tardaba 1.5-20+ min en aparecer, no determinista). Verificar que el SIGSEGV +0x38
   ya NO ocurre.
  
_Pendiente: resultado del build + prueba en device → actualizar Status a BUILT/PASS/FAIL._
