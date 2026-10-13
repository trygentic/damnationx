/**
 * @file webgpu_presenter.cpp
 *
 * WebGPU presenter: the legacy 2D ray-traced composite (incl. the upstream UI rects, town ambient, one-bounce
 * approximation and opt-in 4x Cathedral HD floor preview) plus the 2.5D hybrid
 * lighting passes. Platform-specific code (surface creation, waiting, launcher
 * notifications) lives behind WebGpuPlatform (webgpu_platform.hpp); this file includes no
 * <emscripten.h>.
 */
#include "engine/render/webgpu_presenter.hpp"

#include "config.h"

#if DEVILUTIONX_WEBGPU

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <SDL3/SDL_surface.h>
#include <webgpu/webgpu.h>

#include "engine/assets.hpp"
#include "engine/dx.h"
#include "engine/palette.h"
#include "engine/render/gbuffer.hpp"
#include "engine/render/hd_floor.hpp"
#include "engine/render/hd_graphics.hpp"
#include "engine/render/hd_sprites_webgpu.hpp"
#include "engine/render/hd_tiles_webgpu.hpp"
#include "engine/render/hd_ui_webgpu.hpp"
#include "engine/render/hd_view_transform.hpp"
#include "engine/render/webgpu_platform.hpp"
#include "engine/render/webgpu_shaders.hpp"
#include "controls/control_mode.hpp"
#include "game_mode.hpp"
#include "player.h"
#include "render3d/world_renderer.hpp"
#include "utils/log.hpp"
#include "utils/png.h"
#include "utils/sdl_ptrs.h"

namespace devilution {
namespace {

// The two legacy WGSL strings below are the upstream legacy path (e90b5e6de: UI rects, town ambient, one-bounce,
// HD floor); the hybrid work does not modify them. The composite is compiled after HdFloorWgsl
// (webgpu_shaders.hpp, bindings 16-22), which the hybrid present pass shares.
constexpr char ShaderSource[] = R"(
@group(0) @binding(0) var frameTexture: texture_2d<f32>;
@group(0) @binding(1) var frameSampler: sampler;
@group(0) @binding(2) var lightMask: texture_2d<f32>;
@group(0) @binding(6) var floorGain: texture_2d<f32>;
@group(0) @binding(7) var indexTex: texture_2d<u32>;     // HD floor frames: the palette index plane (viewport rows)
@group(0) @binding(8) var paletteTex: texture_2d<f32>;   // HD floor frames: the SDL palette BltFast expands with

struct VertexOutput {
  @builtin(position) position: vec4f,
  @location(0) uv: vec2f,
};

@vertex fn vertexMain(@builtin(vertex_index) index: u32) -> VertexOutput {
  let positions = array<vec2f, 3>(
    vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
  let coordinates = array<vec2f, 3>(
    vec2f(0.0, 1.0), vec2f(2.0, 1.0), vec2f(0.0, -1.0));
  var output: VertexOutput;
  output.position = vec4f(positions[index], 0.0, 1.0);
  output.uv = coordinates[index];
  return output;
}

// Classic local floor colour over the HD cell mean (the floor gain passes, FloorGainWgsl), one texel per 4x4 pixels,
// stored premultiplied as (gain / 2 * weight, weight): bilinear here, then divided by the weight. Weight 0 = no classic
// sample nearby, use the light-table exposure instead.
fn floorGainAt(screen: vec2f, fallback: f32) -> vec3f {
  let dims = vec2i(textureDimensions(floorGain));
  let p = screen / 4.0 - vec2f(0.5);
  let base = vec2i(floor(p));
  let f = p - floor(p);
  let s00 = textureLoad(floorGain, clamp(base, vec2i(0), dims - 1), 0);
  let s10 = textureLoad(floorGain, clamp(base + vec2i(1, 0), vec2i(0), dims - 1), 0);
  let s01 = textureLoad(floorGain, clamp(base + vec2i(0, 1), vec2i(0), dims - 1), 0);
  let s11 = textureLoad(floorGain, clamp(base + vec2i(1, 1), vec2i(0), dims - 1), 0);
  let s = mix(mix(s00, s10, f.x), mix(s01, s11, f.x), f.y);
  if (s.a < 0.02) {
    return vec3f(fallback);
  }
  return s.rgb * 2.0 / s.a;
}

const FLOOR_WORLD3D: i32 = 16; // FloorFlagWorld3D: the first-person 3D pass is under this composite

fn world3dInRect(p: vec2i, r: vec4i) -> bool {
  return r.z > 0 && r.w > 0 && p.x >= r.x && p.y >= r.y && p.x < r.x + r.z && p.y < r.y + r.w;
}

@fragment fn fragmentMain(input: VertexOutput) -> @location(0) vec4f {
  var frame = textureSample(frameTexture, frameSampler, input.uv);
  let screen = input.uv * vec2f(textureDimensions(frameTexture));
  let px = vec2i(screen);
  if ((floorParams.lattice.w & FLOOR_WORLD3D) != 0) {
    // First-person view (render3d/world_renderer.hpp): the world rows keep the 3D image wherever the UI left the
    // reserved transparent index (misc.y); the UI itself is shown unlit, without the 2D light mask.
    if (px.y < floorParams.misc.x) {
      let index = textureLoad(indexTex, px, 0).r;
      let panel = vec4i(floorParams.misc.w & 0xFFFF, floorParams.misc.z & 0xFFFF, floorParams.misc.w >> 16, floorParams.misc.z >> 16);
      if (index == u32(floorParams.misc.y) && !world3dInRect(px, panel) && !world3dInRect(px, floorParams.view)
          && !world3dInRect(px, floorParams.grid)) {
        discard;
      }
      frame = textureLoad(paletteTex, vec2i(i32(index), 0), 0);
    }
    return vec4f(frame.rgb, 1.0);
  }
  if ((floorParams.lattice.w & FLOOR_EXPAND) != 0 && px.y < floorParams.misc.x) {
    // In game the view rows come from the index plane (byte-identical to BltFast), so only the panel rows are uploaded
    // as RGBA; an HD floor pixel shows its classic colour under the HD albedo.
    frame = textureLoad(paletteTex, vec2i(i32(floorFrameIndex(px, textureLoad(indexTex, px, 0).r)), 0), 0);
  }
  if ((floorParams.lattice.w & FLOOR_ACTIVE) != 0) {
    let hd = floorSampleAt(screen, px, indexTex);
    if (hd.found) {
      // Neutral albedo (the Cathedral atlas is tinted toward the level palette on the CPU) at the classic local
      // brightness and tint (lightmap, night palette) at low frequency, so the 4x detail survives; the light mask below
      // still applies.
      let exposure = clamp(1.0 - f32(hd.light - 1u) / 15.0, 0.0, 1.0);
      frame = vec4f(mix(frame.rgb, hd.rgb * floorGainAt(screen, exposure), hd.weight), frame.a);
    }
  }
  let lighting = textureSample(lightMask, frameSampler, input.uv).rgb;
  return vec4f(frame.rgb * lighting, frame.a);
}
)";

constexpr char RayTracingShaderSource[] = R"(
struct RayLight {
  positionRadius: vec4f,
  colorIntensity: vec4f,
};

struct RayTracingParams {
  screenOriginViewportEnabled: vec4f,
  screenToWorld: vec4f,
  counts: vec4u,
  uiRects: array<vec4f, 5>,
  lights: array<RayLight, 8>,
};

@group(0) @binding(0) var<storage, read> params: RayTracingParams;
@group(0) @binding(1) var<storage, read> occluders: array<u32>;
@group(0) @binding(2) var outputMask: texture_storage_2d<rgba8unorm, write>;

fn screenToWorld(screen: vec2f) -> vec2f {
  let delta = screen - params.screenOriginViewportEnabled.xy;
  return vec2f(
    params.screenToWorld.x * delta.x + params.screenToWorld.y * delta.y,
    params.screenToWorld.z * delta.x + params.screenToWorld.w * delta.y);
}

fn isOccluder(cell: vec2i) -> bool {
  if (cell.x < 0 || cell.y < 0 || cell.x >= i32(params.counts.y) || cell.y >= i32(params.counts.z)) {
    return true;
  }
  return occluders[u32(cell.y) * params.counts.y + u32(cell.x)] != 0u;
}

// Trace a bounded ray through the 2.5D dungeon occupancy grid. Two samples per
// crossed tile prevent diagonal cracks while retaining predictable browser cost.
fn rayBlocked(receiver: vec2f, emitter: vec2f) -> bool {
  let delta = emitter - receiver;
  let sampleCount = min(64u, u32(ceil(max(abs(delta.x), abs(delta.y)) * 2.0)));
  if (sampleCount <= 1u) {
    return false;
  }
  for (var step = 1u; step < sampleCount; step++) {
    let position = receiver + delta * (f32(step) / f32(sampleCount));
    if (isOccluder(vec2i(floor(position)))) {
      return true;
    }
  }
  return false;
}

fn rayVisibility(receiver: vec2f, emitter: vec2f, highQuality: bool) -> f32 {
  if (!highQuality) {
    return select(1.0, 0.0, rayBlocked(receiver, emitter));
  }

  let delta = emitter - receiver;
  if (dot(delta, delta) < 0.0001) {
    return 1.0;
  }
  let tangent = normalize(vec2f(-delta.y, delta.x));
  var visibleSamples = 0.0;
  for (var sample = -1; sample <= 1; sample++) {
    let sampleEmitter = emitter + tangent * (f32(sample) * 0.22);
    if (!rayBlocked(receiver, sampleEmitter)) {
      visibleSamples += 1.0;
    }
  }
  return visibleSamples / 3.0;
}

// Approximate a single diffuse bounce from nearby dungeon geometry. The
// bounce is evaluated only after a direct path to the emitter is established,
// so it can enrich visible surfaces without carrying light around corners into
// rooms that the direct-light visibility test keeps dark.
fn nearbySurfaceCoverage(cell: vec2i) -> f32 {
  let offsets = array<vec2i, 8>(
    vec2i(-1, 0), vec2i(1, 0), vec2i(0, -1), vec2i(0, 1),
    vec2i(-1, -1), vec2i(1, -1), vec2i(-1, 1), vec2i(1, 1));
  var coverage = 0.0;
  for (var index = 0u; index < 8u; index++) {
    if (isOccluder(cell + offsets[index])) {
      coverage += select(0.7, 1.0, index < 4u);
    }
  }
  return clamp(coverage / 6.8, 0.0, 1.0);
}

fn isInsideRect(point: vec2f, rect: vec4f) -> bool {
  return rect.z > 0.0 && rect.w > 0.0
    && point.x >= rect.x && point.y >= rect.y
    && point.x < rect.x + rect.z && point.y < rect.y + rect.w;
}

@compute @workgroup_size(8, 8)
fn rayTraceMain(@builtin(global_invocation_id) id: vec3u) {
  let dimensions = textureDimensions(outputMask);
  if (id.x >= dimensions.x || id.y >= dimensions.y) {
    return;
  }

  let sourcePixel = vec2f(f32(id.x * 2u) + 1.0, f32(id.y * 2u) + 1.0);
  var isUiPixel = sourcePixel.y >= params.screenOriginViewportEnabled.z;
  for (var index = 0u; index < 5u; index++) {
    isUiPixel = isUiPixel || isInsideRect(sourcePixel, params.uiRects[index]);
  }
  if (params.screenOriginViewportEnabled.w < 0.5 || isUiPixel) {
    textureStore(outputMask, vec2i(id.xy), vec4f(1.0));
    return;
  }

  let receiver = screenToWorld(sourcePixel);
  let receiverCell = vec2i(floor(receiver));
  let flags = params.counts.w;
  let debugOccluders = (flags & 1u) != 0u;
  let bloomEnabled = (flags & 2u) != 0u;
  let highQuality = (flags & 4u) != 0u;
  let isTown = (flags & 8u) != 0u;
  if (debugOccluders && isOccluder(receiverCell)) {
    textureStore(outputMask, vec2i(id.xy), vec4f(1.0, 0.35, 0.35, 1.0));
    return;
  }

  var radiance = vec3f(0.0);
  var bouncedRadiance = vec3f(0.0);
  var peakContribution = 0.0;
  let surfaceCoverage = nearbySurfaceCoverage(receiverCell);
  for (var index = 0u; index < params.counts.x; index++) {
    let light = params.lights[index].positionRadius;
    let colorIntensity = params.lights[index].colorIntensity;
    let distance = length(light.xy - receiver);
    if (distance >= light.z) {
      continue;
    }
    let visibility = rayVisibility(receiver, light.xy, highQuality);
    let attenuation = 1.0 - distance / light.z;
    let contribution = pow(clamp(attenuation * attenuation * visibility * colorIntensity.w, 0.0, 1.0), 0.72);
    radiance += colorIntensity.rgb * contribution;
    let bounceStrength = pow(attenuation, 1.35) * visibility * colorIntensity.w;
    bouncedRadiance += colorIntensity.rgb * bounceStrength * (0.10 + 0.30 * surfaceCoverage);
    peakContribution = max(peakContribution, contribution);
  }

  // Town is an outdoor, globally visible level. Preserve its authored ambient
  // readability while allowing local emitters to add highlights; dungeon
  // levels retain the much darker shadow floor.
  let shadow = select(vec3f(0.38, 0.42, 0.52), vec3f(0.80, 0.82, 0.88), isTown);
  var lighting = shadow + radiance * 0.68 + bouncedRadiance * 0.42;
  if (bloomEnabled) {
    let bloom = smoothstep(0.45, 1.0, peakContribution);
    lighting += (radiance + bouncedRadiance * 0.65) * (0.12 * bloom);
  }
  textureStore(outputMask, vec2i(id.xy), vec4f(lighting, 1.0));
}
)";

// ---- HD floor compute passes (?hdFloor=1). One module per pass (auto layouts); each starts with FloorComputeCommon.
// Floor decals (blood pools, bone piles, the shadows at torch stand bases) are part of the classic Cathedral floor CEL
// frames, so the plain HD slabs would replace them. The light tables only move an index along its palette ramp (16
// entries; 8 for 128..159) or to 0, so the ramp of the classic, lit index still names the art colour family: plain
// Cathedral floor uses the grey (32..47) and brown (112..127) ramps with a few blue-grey highlights; blood is the red ramp
// 48..63 and its brown-gold rim 80..95; bones are the blue-grey ramps 1..31; shadows are runs of the darkest shades (or
// black). Per 4x4 world block the count pass counts the HD floor pixels of each family; a pixel keeps its classic colour
// where its family is dense in the surrounding 12x12 pixels (on dim floor shadows also need a lighter 36x36 surround, so a
// dark room is not a "shadow"), and its 3x3 neighbours blend softly (HdFloorWgsl floorKeep).
constexpr char FloorComputeCommon[] = R"(
struct FloorParams { lattice: vec4i, view: vec4i, grid: vec4i, misc: vec4i };   // HdFloorWgsl FloorParams
@group(0) @binding(0) var<uniform> floorParams: FloorParams;
@group(0) @binding(1) var floorMask: texture_2d<u32>;
@group(0) @binding(2) var indexTex: texture_2d<u32>;
@group(0) @binding(3) var floorTiles: texture_2d<u32>;
const FLOOR_DECALS: i32 = 4;
fn floorDivI(a: i32, b: i32) -> i32 {
  let q = a / b;
  if ((a % b) != 0 && ((a < 0) != (b < 0))) { return q - 1; }
  return q;
}
fn floorWorldPixel(px: vec2i) -> vec2i {
  let q = floorParams.lattice.z;
  return vec2i(floorDivI((px.x - floorParams.view.x) * 4, q), floorDivI((px.y - floorParams.view.y) * 4, q));
}
fn floorScreenPixel(w: vec2i) -> vec2i {
  let q = floorParams.lattice.z;
  return vec2i(floorParams.view.x - floorDivI(-w.x * q, 4), floorParams.view.y - floorDivI(-w.y * q, 4));
}
fn floorBit(px: vec2i) -> bool {
  if (px.x < 0 || px.y < 0 || px.x >= floorParams.grid.z || px.y >= floorParams.grid.w) { return false; }
  let word = textureLoad(floorMask, vec2i(px.x >> 5u, px.y), 0).r;
  return ((word >> (u32(px.x) & 31u)) & 1u) != 0u;
}
fn floorTileOf(w: vec2i) -> vec2i {
  let x = 2 * (w.x - floorParams.lattice.x) + 1 - 64;
  let y = 2 * (w.y - floorParams.lattice.y) + 1 + 32;
  return vec2i(floorDivI(x + 2 * y + 64, 128), floorDivI(2 * y - x + 64, 128));
}
fn floorTileInfo(t: vec2i) -> u32 {
  if (t.x < 0 || t.y < 0 || t.x >= 112 || t.y >= 112) { return 0u; }
  return textureLoad(floorTiles, t, 0).r;
}
fn floorClassicIndex(idx: u32) -> u32 {
  if (idx >= 2u) { return idx ^ 1u; }
  return idx;
}
fn floorFamilyBit(idx: u32) -> u32 {
  if ((idx >= 48u && idx < 64u) || (idx >= 80u && idx < 96u)) { return 2u; }
  if (idx >= 1u && idx < 32u) { return 4u; }
  if (idx == 0u || (idx < 128u && (idx & 15u) >= 14u)) { return 8u; }
  return 0u;
}
fn blockInGrid(b: vec2i) -> bool { return b.x >= 0 && b.y >= 0 && b.x < floorParams.grid.x && b.y < floorParams.grid.y; }
)";

/** Pass 1: per 4x4 world block, the HD floor pixels and their families and light levels, packed (5 bits each, levels 8). */
constexpr char FloorCountWgsl[] = R"(
@group(0) @binding(4) var blockCounts: texture_storage_2d<r32uint, write>;
@compute @workgroup_size(8, 8)
fn floorCountMain(@builtin(global_invocation_id) id: vec3u) {
  let b = vec2i(id.xy);
  if (!blockInGrid(b)) { return; }
  var total = 0u; var blood = 0u; var bone = 0u; var dark = 0u; var levels = 0u;
  for (var j = 0; j < 4; j++) {
    for (var i = 0; i < 4; i++) {
      let w = b * 4 + vec2i(i, j);
      if (w.x >= floorParams.view.z || w.y >= floorParams.view.w) { continue; }
      let s = floorScreenPixel(w);
      if (!floorBit(s)) { continue; }
      let light = (floorTileInfo(floorTileOf(w)) >> 16u) & 31u;
      if (light == 0u) { continue; }
      total += 1u;
      levels += light - 1u;
      let family = floorFamilyBit(floorClassicIndex(textureLoad(indexTex, s, 0).r));
      if (family == 2u) { blood += 1u; } else if (family == 4u) { bone += 1u; } else if (family == 8u) { dark += 1u; }
    }
  }
  textureStore(blockCounts, b, vec4u(total | (blood << 5u) | (bone << 10u) | (dark << 15u) | (levels << 20u), 0u, 0u, 1u));
}
)";

/** Pass 2: which families count as decals around each block (3x3 blocks, and 9x9 for dim shadows). */
constexpr char FloorDecideWgsl[] = R"(
@group(0) @binding(5) var blockCountsIn: texture_2d<u32>;
@group(0) @binding(6) var decalFamilies: texture_storage_2d<r32uint, write>;
@compute @workgroup_size(8, 8)
fn floorDecideMain(@builtin(global_invocation_id) id: vec3u) {
  let b = vec2i(id.xy);
  if (!blockInGrid(b)) { return; }
  var total = 0u; var blood = 0u; var bone = 0u; var dark = 0u;
  for (var dy = -1; dy <= 1; dy++) {
    for (var dx = -1; dx <= 1; dx++) {
      let n = b + vec2i(dx, dy);
      if (!blockInGrid(n)) { continue; }
      let c = textureLoad(blockCountsIn, n, 0).r;
      total += c & 31u; blood += (c >> 5u) & 31u; bone += (c >> 10u) & 31u; dark += (c >> 15u) & 31u;
    }
  }
  var families = 0u;
  if (total >= 16u) {
    if (blood * 100u >= total * 8u) { families = families | 2u; }
    if (bone * 100u >= total * 25u) { families = families | 4u; }
    if (dark * 100u >= total * 60u) {
      // Dim floor (light level 4 and darker) turns its mortar black as well: there a shadow must be a blob in a lighter
      // surround. Lit and full-bright (hybrid) floor shows only the art's own dark shades.
      let own = textureLoad(blockCountsIn, b, 0).r;
      let marked = own & 31u;
      let dim = marked == 0u || (own >> 20u) >= 4u * marked;
      var wideTotal = 0u; var wideDark = 0u;
      if (dim) {
        for (var y = max(b.y - 4, 0); y < min(b.y + 5, floorParams.grid.y); y++) {
          for (var x = max(b.x - 4, 0); x < min(b.x + 5, floorParams.grid.x); x++) {
            let c = textureLoad(blockCountsIn, vec2i(x, y), 0).r;
            wideTotal += c & 31u; wideDark += (c >> 15u) & 31u;
          }
        }
      }
      if (!dim || wideDark * 100u <= wideTotal * 35u) { families = families | 8u; }
    }
  }
  textureStore(decalFamilies, b, vec4u(families, 0u, 0u, 1u));
}
)";

/**
 * Pass 3: every decal region grows by one block, so the decal's own pixels at its rim (where the window is half plain
 * floor) are kept too; plain pixels never match, so this does not spread into the floor. A shadow only grows into blocks
 * that are dark themselves (dim floor turns its mortar black too).
 */
constexpr char FloorGrowWgsl[] = R"(
@group(0) @binding(5) var blockCountsIn: texture_2d<u32>;
@group(0) @binding(7) var decalFamiliesIn: texture_2d<u32>;
@group(0) @binding(8) var floorFamilies: texture_storage_2d<r32uint, write>;
@compute @workgroup_size(8, 8)
fn floorGrowMain(@builtin(global_invocation_id) id: vec3u) {
  let b = vec2i(id.xy);
  if (!blockInGrid(b)) { return; }
  var families = 0u;
  for (var dy = -1; dy <= 1; dy++) {
    for (var dx = -1; dx <= 1; dx++) {
      let n = b + vec2i(dx, dy);
      if (blockInGrid(n)) { families = families | textureLoad(decalFamiliesIn, n, 0).r; }
    }
  }
  if ((families & 8u) != 0u && (textureLoad(decalFamiliesIn, b, 0).r & 8u) == 0u) {
    let own = textureLoad(blockCountsIn, b, 0).r;
    let total = own & 31u;
    if (total == 0u || ((own >> 15u) & 31u) * 100u < total * 40u) { families = families & ~8u; }
  }
  textureStore(floorFamilies, b, vec4u(families, 0u, 0u, 1u));
}
)";

/**
 * Legacy composite only. Pass 4: per 4x4 screen block, the classic colours of the HD floor pixels and their HD cells'
 * mean colours (alpha-weighted on town pages), skipping pixels a decal keeps. Pass 5: a 5x5 block box filter of both;
 * gain = classic / HD mean, so the HD floor carries the classic local brightness and tint (lightmap, night palette)
 * while keeping its own detail.
 */
constexpr char FloorGainSumWgsl[] = R"(
@group(0) @binding(9) var floorFamiliesIn: texture_2d<u32>;
@group(0) @binding(10) var paletteTex: texture_2d<f32>;
@group(0) @binding(11) var cellMeans: texture_2d<f32>;           // rgba32float: row 0 Cathedral variants, row 1 + page town cells (0..255)
@group(0) @binding(12) var townFloorAtlas: texture_2d_array<f32>;
@group(0) @binding(13) var classicSums: texture_storage_2d<rgba32float, write>;
@group(0) @binding(14) var meanSums: texture_storage_2d<rgba32float, write>;
fn floorDecalAt(w: vec2i) -> bool {
  if (w.x < 0 || w.y < 0 || w.x >= floorParams.view.z || w.y >= floorParams.view.w) { return false; }
  let families = textureLoad(floorFamiliesIn, vec2i(w.x >> 2u, w.y >> 2u), 0).r;
  if (families == 0u) { return false; }
  let s = floorScreenPixel(w);
  if (!floorBit(s)) { return false; }
  return (floorFamilyBit(floorClassicIndex(textureLoad(indexTex, s, 0).r)) & families) != 0u;
}
// A pixel the decals keep at least half of (a decal pixel, or seven decal neighbours) does not count.
fn floorMostlyKept(w: vec2i) -> bool {
  if ((floorParams.lattice.w & FLOOR_DECALS) == 0) { return false; }
  if (floorDecalAt(w)) { return true; }
  var neighbours = 0;
  for (var dy = -1; dy <= 1; dy++) {
    for (var dx = -1; dx <= 1; dx++) {
      if ((dx != 0 || dy != 0) && floorDecalAt(w + vec2i(dx, dy))) { neighbours++; }
    }
  }
  return neighbours >= 7;
}
@compute @workgroup_size(8, 8)
fn floorGainSumMain(@builtin(global_invocation_id) id: vec3u) {
  let b = vec2i(id.xy);
  if (!blockInGrid(b)) { return; }
  var classic = vec3f(0.0); var mean = vec3f(0.0); var weight = 0.0;
  let scale = f32(floorParams.lattice.z) * 0.25;
  for (var j = 0; j < 4; j++) {
    for (var i = 0; i < 4; i++) {
      let p = b * 4 + vec2i(i, j);
      if (!floorBit(p)) { continue; }
      let w = floorWorldPixel(p);
      let t = floorTileOf(w);
      let info = floorTileInfo(t);
      if (((info >> 16u) & 31u) == 0u || floorMostlyKept(w)) { continue; }
      let code = info & 0xFFFFu;
      var cellMean: vec3f;
      var wgt = 1.0;
      if (code >= 32768u) {
        // The pixel's place in its town cell, as HdFloorWgsl floorSampleAt computes it: count only HD ground there.
        let worldPos = (vec2f(p) + vec2f(0.5) - vec2f(floorParams.view.xy)) / scale;
        let centre = vec2f(f32(floorParams.lattice.x + 32 * (t.x - t.y) + 32), f32(floorParams.lattice.y + 16 * (t.x + t.y) - 16));
        let delta = (worldPos - centre) / vec2f(32.0, 16.0);
        let uv = clamp(vec2f((delta.x + delta.y + 1.0) * 0.5, (-delta.x + delta.y + 1.0) * 0.5), vec2f(0.0), vec2f(0.999));
        let cell = code & 511u;
        let page = (code >> 9u) & 63u;
        let texel = vec2i(i32(cell % 16u) * 256 + i32(uv.x * 256.0), i32(cell / 16u) * 128 + i32(uv.y * 128.0));
        wgt = textureLoad(townFloorAtlas, texel, page, 0).a;
        if (wgt <= 0.0) { continue; }
        cellMean = textureLoad(cellMeans, vec2i(i32(cell), i32(page) + 1), 0).rgb;
      } else {
        cellMean = textureLoad(cellMeans, vec2i(i32(code & 15u), 0), 0).rgb;
      }
      let colour = textureLoad(paletteTex, vec2i(i32(floorClassicIndex(textureLoad(indexTex, p, 0).r)), 0), 0).rgb * 255.0;
      classic += colour * wgt;
      mean += cellMean * wgt;
      weight += wgt;
    }
  }
  textureStore(classicSums, b, vec4f(classic, weight));
  textureStore(meanSums, b, vec4f(mean, 0.0));
}
)";

constexpr char FloorGainBoxWgsl[] = R"(
@group(0) @binding(15) var classicSumsIn: texture_2d<f32>;
@group(0) @binding(16) var meanSumsIn: texture_2d<f32>;
@group(0) @binding(17) var floorGainOut: texture_storage_2d<rgba8unorm, write>;
@compute @workgroup_size(8, 8)
fn floorGainBoxMain(@builtin(global_invocation_id) id: vec3u) {
  let b = vec2i(id.xy);
  if (!blockInGrid(b)) { return; }
  var classic = vec4f(0.0); var mean = vec3f(0.0);
  for (var y = max(b.y - 2, 0); y <= min(b.y + 2, floorParams.grid.y - 1); y++) {
    for (var x = max(b.x - 2, 0); x <= min(b.x + 2, floorParams.grid.x - 1); x++) {
      classic += textureLoad(classicSumsIn, vec2i(x, y), 0);
      mean += textureLoad(meanSumsIn, vec2i(x, y), 0).rgb;
    }
  }
  if (classic.a <= 0.0) {
    textureStore(floorGainOut, b, vec4f(0.0));
    return;
  }
  let weight = min(classic.a / 64.0, 1.0);   // a quarter of the 20x20 window marked
  let gain = clamp(classic.rgb / max(mean, vec3f(1.0)), vec3f(0.0), vec3f(2.0));
  textureStore(floorGainOut, b, vec4f(gain * 0.5 * weight, weight));
}
)";

struct alignas(16) GpuRayLight {
	std::array<float, 4> positionRadius;
	std::array<float, 4> colorIntensity;
};

struct alignas(16) GpuRayTracingParams {
	std::array<float, 4> screenOriginViewportEnabled;
	std::array<float, 4> screenToWorld;
	std::array<uint32_t, 4> counts;
	std::array<std::array<float, 4>, WebGpuUiRectCount> uiRects;
	std::array<GpuRayLight, WebGpuMaxRayTracedLights> lights;
};

static_assert(sizeof(GpuRayLight) == 32);
static_assert(sizeof(GpuRayTracingParams) == 384);

// GPU mirrors of the hybrid WGSL structs (webgpu_shaders.hpp HybridParams / PresentParams; field order identical).
struct alignas(16) GpuHybridLight {
	std::array<float, 4> positionRadius; // worldX, worldY, worldZ, radius
	std::array<float, 4> colorIntensity; // r, g, b, intensity
};

struct alignas(16) GpuHybridParams {
	std::array<float, 4> screenOriginViewport; // originX, originY, viewportHeight (output rows), 0
	std::array<float, 4> screenToWorld;        // XX, XY, YX, YY
	std::array<uint32_t, 4> counts;            // lightCount, gridW, gridH, flags
	std::array<float, 4> shading;              // ambientScale, heightPixelsPerWorldUnit, 0, wrapCard
	std::array<float, 4> tuning;               // wrapSurface, receiverNudge, softTapOffset, 0
	std::array<uint32_t, 4> misc;              // maskScale, zoom (quarters | -offsetY << 8), zoomOffsetX, frameCounter
	std::array<GpuHybridLight, WebGpuMaxHybridLights> lights;
	std::array<std::array<float, 4>, WebGpuNormalLutSize> normalLut;
	std::array<std::array<float, 4>, 4> ambientCurve; // classic darkening per dLight level 0..15
};

struct alignas(16) GpuPresentParams {
	std::array<uint32_t, 4> config;                         // hybrid, zoom, debugView, maskScale
	std::array<float, 4> values;                            // viewportHeight, emissiveScale, exposure, reserved
	std::array<uint32_t, 4> config2;                        // zoomOffsetX, frameHasUnlit, reserved, reserved
	std::array<std::array<float, 4>, 64> paletteEmissive;   // 256 weights
	std::array<std::array<float, 4>, WebGpuNormalLutSize> normalLut; // Normal debug view
};

static_assert(sizeof(GpuHybridLight) == 32);
static_assert(sizeof(GpuHybridParams) == 96 + 1024 + 512 + 64); // 1696, uniform buffer
static_assert(sizeof(GpuPresentParams) == 48 + 1024 + 512); // 1584, uniform buffer
static_assert(sizeof(GpuHybridParams) % 16 == 0 && sizeof(GpuPresentParams) % 16 == 0);

// HybridParams.counts.w / PresentParams.config.z flag layout (debug view in bits 8-11).
constexpr uint32_t FlagDebugOccluders = 1U;
constexpr uint32_t FlagBloom = 2U;
constexpr uint32_t FlagHighQuality = 4U;
constexpr uint32_t FlagHybrid = 8U;
constexpr uint32_t FlagSoftShadows = 16U;
constexpr uint32_t FlagDebugViewShift = 8U;
// Legacy RayTracingParams.counts.w only (rayTraceMain `isTown`); bit 8 means FlagHybrid in HybridParams.
constexpr uint32_t FlagLegacyTown = 8U;
constexpr uint32_t FlagDebugViewMask = 15U;

constexpr uint32_t PaletteTextureWidth = 256;
constexpr size_t OccluderBufferSize = WebGpuOccluderGridWidth * WebGpuOccluderGridHeight * sizeof(uint32_t);

WGPUStringView StringView(const char *string)
{
	return { string, WGPU_STRLEN };
}

// ---- HD floor CPU state (?hdFloor=1) ----
// The world pass marks the floor diamonds it replaces (in world-pass pixels) and the capture swaps their pixels' palette
// indices with a neighbour shade. When the frame is presented, the floor pixels still holding the swapped index were not
// drawn over: one bit per screen pixel tells the composite to show the HD floor there. The diamonds lie on the regular
// iso grid, so the shaders find each pixel's dungeon tile (and its cell and light in a 112 x 112 table) from its position
// (webgpu_shaders.hpp HdFloorWgsl); the decals and the legacy gain are found on the GPU (the Floor*Wgsl passes).
constexpr int FloorTileGrid = 112; // dungeon tiles per side (MAXDUNX / MAXDUNY)
static_assert(FloorTileGrid == static_cast<int>(WebGpuOccluderGridWidth) && FloorTileGrid == static_cast<int>(WebGpuOccluderGridHeight));
constexpr int32_t FloorFlagActive = 1;
constexpr int32_t FloorFlagTown = 2;
constexpr int32_t FloorFlagDecals = 4;
constexpr int32_t FloorFlagExpand = 8;
constexpr int32_t FloorFlagWorld3D = 16; // the composite runs over the first-person 3D pass (render3d/world_renderer.hpp)
/** This present encodes the first-person 3D pass (set by WebGpuPresent from render3d::WorldGpuPrepare). */
bool World3DFrame = false;
constexpr int FloorBlock = 4; // decal and gain blocks: 4x4 pixels

/** A rectangle [left, right) x [top, bottom). */
struct FloorRect {
	int left = 0;
	int top = 0;
	int right = 0;
	int bottom = 0;

	[[nodiscard]] bool empty() const { return left >= right || top >= bottom; }
	void Add(int addLeft, int addTop, int addRight, int addBottom)
	{
		if (addLeft >= addRight || addTop >= addBottom)
			return;
		if (empty()) {
			*this = { addLeft, addTop, addRight, addBottom };
			return;
		}
		left = std::min(left, addLeft);
		top = std::min(top, addTop);
		right = std::max(right, addRight);
		bottom = std::max(bottom, addBottom);
	}
};

/** HdFloorWgsl FloorParams (uniform). */
struct alignas(16) GpuFloorParams {
	std::array<int32_t, 4> lattice; // world-pass x, y of dungeon tile (0, 0)'s bottom-left corner; zoom quarters; FloorFlag*
	std::array<int32_t, 4> view;    // screen position of world pixel 0 (HdWorldTransform offset); world-pass width, height
	std::array<int32_t, 4> grid;    // 4x4 block grid width, height; screen width, height
	std::array<int32_t, 4> misc;    // viewport rows (the legacy composite expands them from the index plane)
};
static_assert(sizeof(GpuFloorParams) == 64);

int FloorWidth = 0; // the world pass (the back buffer width; Begin's height)
int FloorHeight = 0;
bool FloorActive = false;
bool FloorCaptured = false;
// The world pass's transform to the screen (zoom): the marks are in world-pass pixels, the shown bits in screen pixels.
HdWorldTransform FloorTransform;
// The world-pass position of dungeon tile (0, 0)'s bottom-left corner: every diamond of a world pass lies on this grid.
int FloorLatticeX = 0;
int FloorLatticeY = 0;
bool FloorLatticeValid = false;
// Per dungeon tile marked this world pass: bits 0..15 the cell (Cathedral variant 0..15, or town 0x8000 | page << 9 |
// cell), bits 16..20 the light level + 1. Entries of earlier passes may stay on the GPU: no shown pixel lies on them.
std::array<uint32_t, FloorTileGrid * FloorTileGrid> FloorTileTable {};
std::vector<uint16_t> FloorTileList;
FloorRect FloorTilesSet; // tile coordinates (x = tile x) of FloorTileList
bool FloorTownMarks = false;
// One bit per marked world-pass pixel (64 per word, FloorWords words per row); zero outside FloorMarked.
std::vector<uint64_t> FloorMarkedBits;
int FloorWords = 0;
FloorRect FloorMarked;
// The frame right after WebGpuCaptureCathedralFloor (FloorWidth pitch, valid inside FloorMarked): a marked pixel holds
// its swapped index there, so it was drawn over exactly where the presented frame differs from this copy.
std::vector<uint8_t> FloorCapturedFrame;
// One bit per screen pixel showing the HD floor (FloorWords words per row: the screen is as wide as the world pass),
// rebuilt by WebGpuFinishCathedralFloor. Rows outside [FloorShownTop, FloorShownBottom) are zero.
std::vector<uint64_t> FloorShownBits;
int FloorShownTop = 0;
int FloorShownBottom = 0;
bool FloorShownAny = false;
// Rows [top, bottom) of the GPU mask that may hold bits: the next upload covers them and the shown rows.
int FloorUploadedTop = 0;
int FloorUploadedBottom = 0;
// The SDL palette of the last finished frame (the legacy composite's expansion and gain, the Cathedral tint).
std::array<std::array<uint8_t, 4>, 256> FloorPalette {};
// Cathedral palette tint (WebGpuFinishCathedralFloor): the untinted atlas, the smoothed tint of the classic floor
// (average colour / its luminance) and the tint the uploaded atlas carries.
std::vector<uint8_t> FloorAtlasOriginal;
std::array<float, 3> FloorTint { 1.0F, 1.0F, 1.0F };
std::array<float, 3> FloorAppliedTint { 1.0F, 1.0F, 1.0F };
bool FloorTintValid = false;
// The 4x4 block grid of the decal and gain passes (ceil(screen / 4)).
int FloorGainWidth = 0;
int FloorGainHeight = 0;
// Mean sRGB colour of every HD cell: Cathedral variants (untinted; the applied tint is multiplied in) and town cells
// (page * HdFloorMaxCellsPerPage + cell), alpha-weighted. The gain pass reads them from cellMeansTexture.
std::array<std::array<float, 3>, 16> CathedralCellMean {};
std::vector<std::array<float, 3>> TownCellMean;
bool FloorCellMeansDirty = true;

std::string ToString(WGPUStringView string)
{
	if (string.data == nullptr)
		return {};
	const size_t length = string.length == WGPU_STRLEN ? std::char_traits<char>::length(string.data) : string.length;
	return { string.data, length };
}

struct WebGpuPresenter {
	WGPUInstance instance = nullptr;
	WGPUSurface surface = nullptr;
	WGPUAdapter adapter = nullptr;
	WGPUDevice device = nullptr;
	WGPUQueue queue = nullptr;
	// Sized textures (recreated by WebGpuResize).
	WGPUTexture frameTexture = nullptr;
	WGPUTextureView frameTextureView = nullptr;
	WGPUTexture lightMaskTexture = nullptr;
	WGPUTextureView lightMaskTextureView = nullptr;
	WGPUTexture auxTexture = nullptr;
	WGPUTextureView auxTextureView = nullptr;
	WGPUTexture baseTexture = nullptr;
	WGPUTextureView baseTextureView = nullptr;
	WGPUTexture indexTexture = nullptr;
	WGPUTextureView indexTextureView = nullptr;
	WGPUTexture hybridMaskTexture = nullptr;
	WGPUTextureView hybridMaskTextureView = nullptr;
	WGPUTexture paletteTexture = nullptr;
	WGPUTextureView paletteTextureView = nullptr;
	WGPUTexture volumeTexture = nullptr;
	WGPUTextureView volumeTextureView = nullptr;
	// HD floor (HdFloorWgsl bindings 16-22 and the Floor*Wgsl passes), sized with the source.
	WGPUTexture floorMaskTexture = nullptr; // r32uint ceil(W / 32) x H: one bit per screen pixel showing the HD floor
	WGPUTextureView floorMaskTextureView = nullptr;
	WGPUTexture floorCountsTexture = nullptr; // r32uint per 4x4 block: HD floor pixels per decal family (pass 1)
	WGPUTextureView floorCountsTextureView = nullptr;
	WGPUTexture floorDecalTexture = nullptr; // r32uint per block: decal families around it (pass 2)
	WGPUTextureView floorDecalTextureView = nullptr;
	WGPUTexture floorFamiliesTexture = nullptr; // r32uint per block: decal families kept there (pass 3, binding 20)
	WGPUTextureView floorFamiliesTextureView = nullptr;
	WGPUTexture floorClassicSumTexture = nullptr; // rgba32float per block: classic colour sums (pass 4)
	WGPUTextureView floorClassicSumTextureView = nullptr;
	WGPUTexture floorMeanSumTexture = nullptr; // rgba32float per block: HD cell mean sums (pass 4)
	WGPUTextureView floorMeanSumTextureView = nullptr;
	WGPUTexture floorGainTexture = nullptr; // HD floor classic brightness (legacy composite binding 6, pass 5), ceil(size / 4)
	WGPUTextureView floorGainTextureView = nullptr;
	// Buffers, sampler, pipelines and the HD floor atlases (size independent).
	WGPUTexture floorAtlasTexture = nullptr; // 4x Cathedral atlas (binding 17)
	WGPUTextureView floorAtlasTextureView = nullptr;
	bool floorAtlasAvailable = false;
	// HD town floor pages (binding 18): a 1x1 placeholder until the first town frame with ?hdFloor=1.
	WGPUTexture townFloorTexture = nullptr;
	WGPUTextureView townFloorTextureView = nullptr;
	bool townFloorAvailable = false;
	bool townFloorLoadAttempted = false;
	WGPUTexture floorTilesTexture = nullptr; // r32uint 112 x 112 (binding 19)
	WGPUTextureView floorTilesTextureView = nullptr;
	WGPUTexture cellMeansTexture = nullptr; // rgba32float 512 x (1 + town pages): HD cell mean colours (pass 4)
	WGPUTextureView cellMeansTextureView = nullptr;
	uint32_t cellMeansRows = 0;
	WGPUBuffer floorParamsBuffer = nullptr; // GpuFloorParams (binding 21, compute binding 0)
	std::array<WGPUComputePipeline, 5> floorPipelines {}; // count, decide, grow, gain sum, gain box
	std::array<WGPUBindGroup, 5> floorBindGroups {};
	WGPUBuffer rayTracingParamsBuffer = nullptr;
	WGPUBuffer occluderBuffer = nullptr;
	WGPUBuffer hybridParamsBuffer = nullptr;
	WGPUBuffer presentParamsBuffer = nullptr;
	WGPUSampler sampler = nullptr;
	WGPURenderPipeline pipeline = nullptr;
	WGPUComputePipeline rayTracingPipeline = nullptr;
	WGPURenderPipeline hybridPresentPipeline = nullptr;
	WGPUComputePipeline hybridLightPipeline = nullptr;
	// Bind groups (recreated with the textures).
	WGPUBindGroup bindGroup = nullptr;
	WGPUBindGroup rayTracingBindGroup = nullptr;
	WGPUBindGroup hybridLightBindGroup = nullptr;
	WGPUBindGroup hybridPresentBindGroup = nullptr;
	WGPUTextureFormat surfaceFormat = WGPUTextureFormat_Undefined;
	int sourceWidth = 0;
	int sourceHeight = 0;
	int drawableWidth = 0;
	int drawableHeight = 0;
	uint32_t hybridMaskScale = 2; // 1 (High) or 2 (Low); the live mask is ceil(W/s) x ceil(H/s)
	uint32_t hybridMaskWidth = 0;
	uint32_t hybridMaskHeight = 0;
	bool volumeUploaded = false;
	uint32_t lastVolumeGeneration = 0;
	bool loggedUnpresentableHybrid = false; // edge-triggered: one log per hybrid.enabled-but-not-presentable episode
	bool surfaceLost = false;
	bool adapterRequestFinished = false;
	bool deviceRequestFinished = false;
	bool deviceLost = false;
	bool errorNotified = false; // OnUncapturedError notifies the launcher once per device

	void ReleaseBindGroups()
	{
		if (hybridPresentBindGroup != nullptr) wgpuBindGroupRelease(hybridPresentBindGroup);
		if (hybridLightBindGroup != nullptr) wgpuBindGroupRelease(hybridLightBindGroup);
		if (rayTracingBindGroup != nullptr) wgpuBindGroupRelease(rayTracingBindGroup);
		if (bindGroup != nullptr) wgpuBindGroupRelease(bindGroup);
		hybridPresentBindGroup = nullptr;
		hybridLightBindGroup = nullptr;
		rayTracingBindGroup = nullptr;
		bindGroup = nullptr;
		ReleaseFloorBindGroups();
	}

	void ReleaseFloorBindGroups()
	{
		for (WGPUBindGroup &group : floorBindGroups) {
			if (group != nullptr) wgpuBindGroupRelease(group);
			group = nullptr;
		}
	}

	void ReleaseHybridBindGroups()
	{
		if (hybridPresentBindGroup != nullptr) wgpuBindGroupRelease(hybridPresentBindGroup);
		if (hybridLightBindGroup != nullptr) wgpuBindGroupRelease(hybridLightBindGroup);
		hybridPresentBindGroup = nullptr;
		hybridLightBindGroup = nullptr;
	}

	static void ReleaseTexture(WGPUTexture &texture, WGPUTextureView &view)
	{
		if (view != nullptr) wgpuTextureViewRelease(view);
		if (texture != nullptr) {
			wgpuTextureDestroy(texture);
			wgpuTextureRelease(texture);
		}
		view = nullptr;
		texture = nullptr;
	}

	void ReleaseHybridMask()
	{
		ReleaseTexture(hybridMaskTexture, hybridMaskTextureView);
		hybridMaskWidth = 0;
		hybridMaskHeight = 0;
	}

	void ReleaseTextures()
	{
		ReleaseTexture(volumeTexture, volumeTextureView);
		ReleaseTexture(paletteTexture, paletteTextureView);
		ReleaseHybridMask();
		ReleaseTexture(floorMaskTexture, floorMaskTextureView);
		ReleaseTexture(floorCountsTexture, floorCountsTextureView);
		ReleaseTexture(floorDecalTexture, floorDecalTextureView);
		ReleaseTexture(floorFamiliesTexture, floorFamiliesTextureView);
		ReleaseTexture(floorClassicSumTexture, floorClassicSumTextureView);
		ReleaseTexture(floorMeanSumTexture, floorMeanSumTextureView);
		ReleaseTexture(floorGainTexture, floorGainTextureView);
		ReleaseTexture(indexTexture, indexTextureView);
		ReleaseTexture(baseTexture, baseTextureView);
		ReleaseTexture(auxTexture, auxTextureView);
		ReleaseTexture(lightMaskTexture, lightMaskTextureView);
		ReleaseTexture(frameTexture, frameTextureView);
		volumeUploaded = false;
	}

	static void ReleaseBuffer(WGPUBuffer &buffer)
	{
		if (buffer != nullptr) {
			wgpuBufferDestroy(buffer);
			wgpuBufferRelease(buffer);
		}
		buffer = nullptr;
	}

	void ReleaseFrameResources()
	{
		ReleaseBindGroups();
		if (hybridLightPipeline != nullptr) wgpuComputePipelineRelease(hybridLightPipeline);
		if (hybridPresentPipeline != nullptr) wgpuRenderPipelineRelease(hybridPresentPipeline);
		if (rayTracingPipeline != nullptr) wgpuComputePipelineRelease(rayTracingPipeline);
		if (pipeline != nullptr) wgpuRenderPipelineRelease(pipeline);
		if (sampler != nullptr) wgpuSamplerRelease(sampler);
		for (WGPUComputePipeline &floorPipeline : floorPipelines) {
			if (floorPipeline != nullptr) wgpuComputePipelineRelease(floorPipeline);
			floorPipeline = nullptr;
		}
		ReleaseBuffer(floorParamsBuffer);
		ReleaseBuffer(presentParamsBuffer);
		ReleaseBuffer(hybridParamsBuffer);
		ReleaseBuffer(occluderBuffer);
		ReleaseBuffer(rayTracingParamsBuffer);
		ReleaseTextures();
		ReleaseTexture(floorAtlasTexture, floorAtlasTextureView);
		floorAtlasAvailable = false;
		ReleaseTexture(townFloorTexture, townFloorTextureView);
		townFloorAvailable = false;
		townFloorLoadAttempted = false;
		ReleaseTexture(floorTilesTexture, floorTilesTextureView);
		ReleaseTexture(cellMeansTexture, cellMeansTextureView);
		cellMeansRows = 0;
		hybridLightPipeline = nullptr;
		hybridPresentPipeline = nullptr;
		rayTracingPipeline = nullptr;
		pipeline = nullptr;
		sampler = nullptr;
	}

	void Shutdown()
	{
		HdTilesGpuRelease();
		HdSpritesGpuRelease();
		render3d::WorldGpuRelease();
		HdUiGpuRelease();
		ReleaseFrameResources();
		// The surface goes before the device: a configured surface keeps a swap chain whose teardown uses the device's
		// internals (Dawn/Vulkan: FencedDeleter), and releasing the last device reference destroys those. Released in
		// the other order, ~Surface touched the destroyed device (segfault on exit with the HD art loaded, Linux lavapipe).
		if (surface != nullptr && device != nullptr) wgpuSurfaceUnconfigure(surface);
		if (surface != nullptr) wgpuSurfaceRelease(surface);
		if (queue != nullptr) wgpuQueueRelease(queue);
		if (device != nullptr) wgpuDeviceRelease(device);
		if (adapter != nullptr) wgpuAdapterRelease(adapter);
		if (instance != nullptr) wgpuInstanceRelease(instance);
		queue = nullptr;
		device = nullptr;
		adapter = nullptr;
		surface = nullptr;
		instance = nullptr;
		surfaceFormat = WGPUTextureFormat_Undefined;
		drawableWidth = 0;
		drawableHeight = 0;
		surfaceLost = false;
		deviceLost = false;
		errorNotified = false;
	}

	[[nodiscard]] bool HasSizedTextures() const
	{
		return frameTextureView != nullptr && lightMaskTextureView != nullptr && auxTextureView != nullptr
		    && baseTextureView != nullptr && indexTextureView != nullptr && hybridMaskTextureView != nullptr
		    && paletteTextureView != nullptr && volumeTextureView != nullptr && floorMaskTextureView != nullptr
		    && floorCountsTextureView != nullptr && floorDecalTextureView != nullptr && floorFamiliesTextureView != nullptr
		    && floorClassicSumTextureView != nullptr && floorMeanSumTextureView != nullptr && floorGainTextureView != nullptr;
	}

	[[nodiscard]] bool HasPipelines() const
	{
		return pipeline != nullptr && rayTracingPipeline != nullptr && hybridPresentPipeline != nullptr
		    && hybridLightPipeline != nullptr && sampler != nullptr && rayTracingParamsBuffer != nullptr
		    && occluderBuffer != nullptr && hybridParamsBuffer != nullptr && presentParamsBuffer != nullptr
		    && floorAtlasTextureView != nullptr && townFloorTextureView != nullptr && floorTilesTextureView != nullptr
		    && cellMeansTextureView != nullptr && floorParamsBuffer != nullptr
		    && std::all_of(floorPipelines.begin(), floorPipelines.end(), [](WGPUComputePipeline floorPipeline) { return floorPipeline != nullptr; });
	}

	[[nodiscard]] bool HasBindGroups() const
	{
		return bindGroup != nullptr && rayTracingBindGroup != nullptr && hybridLightBindGroup != nullptr
		    && hybridPresentBindGroup != nullptr
		    && std::all_of(floorBindGroups.begin(), floorBindGroups.end(), [](WGPUBindGroup group) { return group != nullptr; });
	}
};

WebGpuPresenter Presenter;

void OnAdapterRequest(WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message, void *userdata, void *)
{
	auto &presenter = *static_cast<WebGpuPresenter *>(userdata);
	if (status == WGPURequestAdapterStatus_Success)
		presenter.adapter = adapter;
	else
		LogError("WebGPU adapter request failed: {}", ToString(message));
	presenter.adapterRequestFinished = true;
}

// Device loss and uncaptured errors are counted in RendererDiagnostics and surfaced to the
// launcher as a retryable renderer failure. A loss we caused ourselves (WebGpuShutdown destroying the
// device) is neither a failure nor allowed to poison a presenter re-initialized afterwards.
void OnDeviceLost(WGPUDevice const *, WGPUDeviceLostReason reason, WGPUStringView message, void *userdata, void *)
{
	if (reason == WGPUDeviceLostReason_Destroyed || reason == WGPUDeviceLostReason_CallbackCancelled)
		return;
	auto &presenter = *static_cast<WebGpuPresenter *>(userdata);
	presenter.deviceLost = true;
	++MutableRendererDiagnostics().deviceLossCount;
	LogError("WebGPU device lost ({}): {}", static_cast<int>(reason), ToString(message));
	GetWebGpuPlatform().notifyState("renderer-failure", "The WebGPU device was lost. Retry to restart the renderer.", true, true);
}

void OnUncapturedError(WGPUDevice const *, WGPUErrorType type, WGPUStringView message, void *userdata, void *)
{
	++MutableRendererDiagnostics().validationErrorCount;
	const std::string detail = ToString(message);
	LogError("WebGPU validation error ({}): {}", static_cast<int>(type), detail);
	// A per-frame error would otherwise re-enter the launcher every frame: report and notify once per device.
	auto &presenter = *static_cast<WebGpuPresenter *>(userdata);
	if (presenter.errorNotified)
		return;
	presenter.errorNotified = true;
	GetWebGpuPlatform().reportError(detail.c_str());
	GetWebGpuPlatform().notifyState("renderer-failure", "The WebGPU renderer reported an error. Retry to restart the renderer.", true, true);
}

void OnDeviceRequest(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message, void *userdata, void *)
{
	auto &presenter = *static_cast<WebGpuPresenter *>(userdata);
	if (status == WGPURequestDeviceStatus_Success)
		presenter.device = device;
	else
		LogError("WebGPU device request failed: {}", ToString(message));
	presenter.deviceRequestFinished = true;
}

bool ConfigureSurface(WebGpuPresenter &presenter, int width, int height)
{
	if (width <= 0 || height <= 0)
		return false;
	if (presenter.drawableWidth == width && presenter.drawableHeight == height)
		return true;

	WGPUSurfaceConfiguration configuration = WGPU_SURFACE_CONFIGURATION_INIT;
	configuration.device = presenter.device;
	configuration.format = presenter.surfaceFormat;
	configuration.usage = WGPUTextureUsage_RenderAttachment;
	configuration.width = static_cast<uint32_t>(width);
	configuration.height = static_cast<uint32_t>(height);
	configuration.presentMode = WGPUPresentMode_Fifo;
	configuration.alphaMode = WGPUCompositeAlphaMode_Auto;
	wgpuSurfaceConfigure(presenter.surface, &configuration);
	presenter.drawableWidth = width;
	presenter.drawableHeight = height;
	return true;
}

bool CreateTexture2D(WebGpuPresenter &presenter, const char *label, WGPUTextureUsage usage, WGPUTextureFormat format,
    uint32_t width, uint32_t height, WGPUTexture &texture, WGPUTextureView &view)
{
	WGPUTextureDescriptor descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
	descriptor.label = StringView(label);
	descriptor.usage = usage;
	descriptor.dimension = WGPUTextureDimension_2D;
	descriptor.size = { width, height, 1 };
	descriptor.format = format;
	texture = wgpuDeviceCreateTexture(presenter.device, &descriptor);
	if (texture == nullptr)
		return false;
	view = wgpuTextureCreateView(texture, nullptr);
	return view != nullptr;
}

WGPUBuffer CreateBuffer(WebGpuPresenter &presenter, const char *label, WGPUBufferUsage usage, size_t size)
{
	WGPUBufferDescriptor descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
	descriptor.label = StringView(label);
	descriptor.usage = usage;
	descriptor.size = size;
	return wgpuDeviceCreateBuffer(presenter.device, &descriptor);
}

WGPUShaderModule CreateShaderModule(WebGpuPresenter &presenter, const char *label, const char *code)
{
	WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
	source.code = StringView(code);
	WGPUShaderModuleDescriptor descriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
	descriptor.nextInChain = &source.chain;
	descriptor.label = StringView(label);
	return wgpuDeviceCreateShaderModule(presenter.device, &descriptor);
}

WGPURenderPipeline CreatePresentPipeline(WebGpuPresenter &presenter, const char *label, WGPUShaderModule shader)
{
	WGPUColorTargetState colorTarget = WGPU_COLOR_TARGET_STATE_INIT;
	colorTarget.format = presenter.surfaceFormat;
	WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
	fragment.module = shader;
	fragment.entryPoint = StringView("fragmentMain");
	fragment.targetCount = 1;
	fragment.targets = &colorTarget;
	WGPURenderPipelineDescriptor descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
	descriptor.label = StringView(label);
	descriptor.vertex.module = shader;
	descriptor.vertex.entryPoint = StringView("vertexMain");
	descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
	descriptor.fragment = &fragment;
	return wgpuDeviceCreateRenderPipeline(presenter.device, &descriptor);
}

WGPUComputePipeline CreateComputePipeline(WebGpuPresenter &presenter, const char *label, WGPUShaderModule shader, const char *entryPoint)
{
	WGPUComputePipelineDescriptor descriptor = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
	descriptor.label = StringView(label);
	descriptor.compute.module = shader;
	descriptor.compute.entryPoint = StringView(entryPoint);
	return wgpuDeviceCreateComputePipeline(presenter.device, &descriptor);
}

/** The hybrid mask (rgba16float, ceil(W/s) x ceil(H/s)) for the current presenter.hybridMaskScale. */
bool CreateHybridMask(WebGpuPresenter &presenter)
{
	const uint32_t scale = presenter.hybridMaskScale;
	const uint32_t width = (static_cast<uint32_t>(presenter.sourceWidth) + scale - 1) / scale;
	const uint32_t height = (static_cast<uint32_t>(presenter.sourceHeight) + scale - 1) / scale;
	if (!CreateTexture2D(presenter, "DevilutionX hybrid light mask", WGPUTextureUsage_StorageBinding | WGPUTextureUsage_TextureBinding,
	        WGPUTextureFormat_RGBA16Float, width, height, presenter.hybridMaskTexture, presenter.hybridMaskTextureView))
		return false;
	presenter.hybridMaskWidth = width;
	presenter.hybridMaskHeight = height;
	return true;
}

/** Every sized texture: frame, legacy mask, aux/base/index planes, hybrid mask, palette, volume and HD floor metadata. */
bool CreateTextures(WebGpuPresenter &presenter)
{
	const auto width = static_cast<uint32_t>(presenter.sourceWidth);
	const auto height = static_cast<uint32_t>(presenter.sourceHeight);
	constexpr WGPUTextureUsage UploadedUsage = WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding;
	if (!CreateTexture2D(presenter, "DevilutionX frame texture", UploadedUsage, WGPUTextureFormat_RGBA8Unorm, width, height,
	        presenter.frameTexture, presenter.frameTextureView))
		return false;
	if (!CreateTexture2D(presenter, "DevilutionX ray traced light mask", WGPUTextureUsage_StorageBinding | WGPUTextureUsage_TextureBinding,
	        WGPUTextureFormat_RGBA8Unorm, (width + 1) / 2, (height + 1) / 2, presenter.lightMaskTexture, presenter.lightMaskTextureView))
		return false;
	if (!CreateTexture2D(presenter, "DevilutionX G-buffer aux plane", UploadedUsage, WGPUTextureFormat_R8Uint, width, height,
	        presenter.auxTexture, presenter.auxTextureView))
		return false;
	if (!CreateTexture2D(presenter, "DevilutionX G-buffer base plane", UploadedUsage, WGPUTextureFormat_R8Uint, width, height,
	        presenter.baseTexture, presenter.baseTextureView))
		return false;
	if (!CreateTexture2D(presenter, "DevilutionX palette index plane", UploadedUsage, WGPUTextureFormat_R8Uint, width, height,
	        presenter.indexTexture, presenter.indexTextureView))
		return false;
	if (!CreateHybridMask(presenter))
		return false;
	if (!CreateTexture2D(presenter, "DevilutionX palette", UploadedUsage, WGPUTextureFormat_RGBA8Unorm, PaletteTextureWidth, 1,
	        presenter.paletteTexture, presenter.paletteTextureView))
		return false;
	if (!CreateTexture2D(presenter, "DevilutionX occluder volume", UploadedUsage, WGPUTextureFormat_R32Uint,
	        WebGpuOccluderGridWidth, WebGpuOccluderGridHeight, presenter.volumeTexture, presenter.volumeTextureView))
		return false;
	if (!CreateTexture2D(presenter, "HD floor shown pixels", UploadedUsage, WGPUTextureFormat_R32Uint, (width + 31) / 32, height,
	        presenter.floorMaskTexture, presenter.floorMaskTextureView))
		return false;
	FloorGainWidth = static_cast<int>((width + FloorBlock - 1) / FloorBlock);
	FloorGainHeight = static_cast<int>((height + FloorBlock - 1) / FloorBlock);
	const auto blocksWide = static_cast<uint32_t>(FloorGainWidth);
	const auto blocksHigh = static_cast<uint32_t>(FloorGainHeight);
	constexpr WGPUTextureUsage PassUsage = WGPUTextureUsage_StorageBinding | WGPUTextureUsage_TextureBinding;
	if (!CreateTexture2D(presenter, "HD floor block counts", PassUsage, WGPUTextureFormat_R32Uint, blocksWide, blocksHigh,
	        presenter.floorCountsTexture, presenter.floorCountsTextureView)
	    || !CreateTexture2D(presenter, "HD floor decal families", PassUsage, WGPUTextureFormat_R32Uint, blocksWide, blocksHigh,
	        presenter.floorDecalTexture, presenter.floorDecalTextureView)
	    || !CreateTexture2D(presenter, "HD floor kept families", PassUsage, WGPUTextureFormat_R32Uint, blocksWide, blocksHigh,
	        presenter.floorFamiliesTexture, presenter.floorFamiliesTextureView)
	    || !CreateTexture2D(presenter, "HD floor classic sums", PassUsage, WGPUTextureFormat_RGBA32Float, blocksWide, blocksHigh,
	        presenter.floorClassicSumTexture, presenter.floorClassicSumTextureView)
	    || !CreateTexture2D(presenter, "HD floor cell mean sums", PassUsage, WGPUTextureFormat_RGBA32Float, blocksWide, blocksHigh,
	        presenter.floorMeanSumTexture, presenter.floorMeanSumTextureView)
	    || !CreateTexture2D(presenter, "HD floor classic brightness", PassUsage, WGPUTextureFormat_RGBA8Unorm, blocksWide, blocksHigh,
	        presenter.floorGainTexture, presenter.floorGainTextureView))
		return false;
	// New textures start zeroed, and the grids are reallocated at the new size.
	FloorUploadedTop = 0;
	FloorUploadedBottom = 0;
	FloorShownTop = 0;
	FloorShownBottom = 0;
	FloorShownAny = false;
	FloorShownBits.clear();
	FloorMarkedBits.clear();
	FloorMarked = {};
	FloorActive = false;
	presenter.volumeUploaded = false;
	return true;
}

WGPUBindGroupEntry TextureEntry(uint32_t binding, WGPUTextureView view)
{
	WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
	entry.binding = binding;
	entry.textureView = view;
	return entry;
}

WGPUBindGroupEntry BufferEntry(uint32_t binding, WGPUBuffer buffer, uint64_t size)
{
	WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
	entry.binding = binding;
	entry.buffer = buffer;
	entry.size = size;
	return entry;
}

WGPUBindGroupEntry SamplerEntry(uint32_t binding, WGPUSampler sampler)
{
	WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
	entry.binding = binding;
	entry.sampler = sampler;
	return entry;
}

/** HdFloorWgsl bindings 16-22, shared by the legacy composite and the hybrid present pass. */
void AppendFloorPresentEntries(const WebGpuPresenter &presenter, std::vector<WGPUBindGroupEntry> &entries)
{
	entries.push_back(TextureEntry(16, presenter.floorMaskTextureView));
	entries.push_back(TextureEntry(17, presenter.floorAtlasTextureView));
	entries.push_back(TextureEntry(18, presenter.townFloorTextureView));
	entries.push_back(TextureEntry(19, presenter.floorTilesTextureView));
	entries.push_back(TextureEntry(20, presenter.floorFamiliesTextureView));
	entries.push_back(BufferEntry(21, presenter.floorParamsBuffer, sizeof(GpuFloorParams)));
	entries.push_back(SamplerEntry(22, presenter.sampler));
}

WGPUBindGroup CreateBindGroup(WebGpuPresenter &presenter, WGPUBindGroupLayout layout, const char *label, const std::vector<WGPUBindGroupEntry> &entries)
{
	WGPUBindGroupDescriptor descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
	descriptor.label = StringView(label);
	descriptor.layout = layout;
	descriptor.entryCount = entries.size();
	descriptor.entries = entries.data();
	WGPUBindGroup group = wgpuDeviceCreateBindGroup(presenter.device, &descriptor);
	wgpuBindGroupLayoutRelease(layout);
	return group;
}

/** The bind groups of the HD floor passes (FloorCountWgsl ... FloorGainBoxWgsl), exactly the bindings each one uses. */
bool CreateFloorBindGroups(WebGpuPresenter &presenter)
{
	presenter.ReleaseFloorBindGroups();
	const WGPUBindGroupEntry params = BufferEntry(0, presenter.floorParamsBuffer, sizeof(GpuFloorParams));
	const WGPUBindGroupEntry mask = TextureEntry(1, presenter.floorMaskTextureView);
	const WGPUBindGroupEntry index = TextureEntry(2, presenter.indexTextureView);
	const WGPUBindGroupEntry tiles = TextureEntry(3, presenter.floorTilesTextureView);
	const std::array<std::vector<WGPUBindGroupEntry>, 5> passEntries {
		std::vector<WGPUBindGroupEntry> { params, mask, index, tiles, TextureEntry(4, presenter.floorCountsTextureView) },
		std::vector<WGPUBindGroupEntry> { params, TextureEntry(5, presenter.floorCountsTextureView), TextureEntry(6, presenter.floorDecalTextureView) },
		std::vector<WGPUBindGroupEntry> { params, TextureEntry(5, presenter.floorCountsTextureView), TextureEntry(7, presenter.floorDecalTextureView),
		    TextureEntry(8, presenter.floorFamiliesTextureView) },
		std::vector<WGPUBindGroupEntry> { params, mask, index, tiles, TextureEntry(9, presenter.floorFamiliesTextureView),
		    TextureEntry(10, presenter.paletteTextureView), TextureEntry(11, presenter.cellMeansTextureView), TextureEntry(12, presenter.townFloorTextureView),
		    TextureEntry(13, presenter.floorClassicSumTextureView), TextureEntry(14, presenter.floorMeanSumTextureView) },
		std::vector<WGPUBindGroupEntry> { params, TextureEntry(15, presenter.floorClassicSumTextureView), TextureEntry(16, presenter.floorMeanSumTextureView),
		    TextureEntry(17, presenter.floorGainTextureView) },
	};
	for (size_t pass = 0; pass < passEntries.size(); ++pass) {
		presenter.floorBindGroups[pass] = CreateBindGroup(presenter, wgpuComputePipelineGetBindGroupLayout(presenter.floorPipelines[pass], 0),
		    "DamnationX HD floor pass bind group", passEntries[pass]);
		if (presenter.floorBindGroups[pass] == nullptr)
			return false;
	}
	return true;
}

bool CreateLegacyBindGroups(WebGpuPresenter &presenter)
{
	std::vector<WGPUBindGroupEntry> entries {
		TextureEntry(0, presenter.frameTextureView),
		SamplerEntry(1, presenter.sampler),
		TextureEntry(2, presenter.lightMaskTextureView),
		TextureEntry(6, presenter.floorGainTextureView),
		TextureEntry(7, presenter.indexTextureView),
		TextureEntry(8, presenter.paletteTextureView),
	};
	AppendFloorPresentEntries(presenter, entries);
	presenter.bindGroup = CreateBindGroup(presenter, wgpuRenderPipelineGetBindGroupLayout(presenter.pipeline, 0), "DevilutionX composite bind group", entries);

	WGPUBindGroupLayout rayTracingLayout = wgpuComputePipelineGetBindGroupLayout(presenter.rayTracingPipeline, 0);
	WGPUBindGroupEntry rayTracingEntries[3] = { WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT };
	rayTracingEntries[0].binding = 0;
	rayTracingEntries[0].buffer = presenter.rayTracingParamsBuffer;
	rayTracingEntries[0].size = sizeof(GpuRayTracingParams);
	rayTracingEntries[1].binding = 1;
	rayTracingEntries[1].buffer = presenter.occluderBuffer;
	rayTracingEntries[1].size = OccluderBufferSize;
	rayTracingEntries[2].binding = 2;
	rayTracingEntries[2].textureView = presenter.lightMaskTextureView;
	WGPUBindGroupDescriptor rayTracingBindGroupDescriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
	rayTracingBindGroupDescriptor.layout = rayTracingLayout;
	rayTracingBindGroupDescriptor.entryCount = 3;
	rayTracingBindGroupDescriptor.entries = rayTracingEntries;
	presenter.rayTracingBindGroup = wgpuDeviceCreateBindGroup(presenter.device, &rayTracingBindGroupDescriptor);
	wgpuBindGroupLayoutRelease(rayTracingLayout);
	return presenter.bindGroup != nullptr && presenter.rayTracingBindGroup != nullptr;
}

/** The two hybrid bind groups; both reference the hybrid mask, so they are rebuilt whenever it is. */
bool CreateHybridBindGroups(WebGpuPresenter &presenter)
{
	WGPUBindGroupLayout lightLayout = wgpuComputePipelineGetBindGroupLayout(presenter.hybridLightPipeline, 0);
	std::array<WGPUBindGroupEntry, 5> lightEntries {};
	lightEntries.fill(WGPU_BIND_GROUP_ENTRY_INIT);
	lightEntries[0].binding = 0;
	lightEntries[0].buffer = presenter.hybridParamsBuffer;
	lightEntries[0].size = sizeof(GpuHybridParams);
	lightEntries[1].binding = 1;
	lightEntries[1].textureView = presenter.volumeTextureView;
	lightEntries[2].binding = 2;
	lightEntries[2].textureView = presenter.hybridMaskTextureView;
	lightEntries[3].binding = 3;
	lightEntries[3].textureView = presenter.auxTextureView;
	lightEntries[4].binding = 4;
	lightEntries[4].textureView = presenter.baseTextureView;
	WGPUBindGroupDescriptor lightDescriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
	lightDescriptor.label = StringView("DevilutionX hybrid light bind group");
	lightDescriptor.layout = lightLayout;
	lightDescriptor.entryCount = lightEntries.size();
	lightDescriptor.entries = lightEntries.data();
	presenter.hybridLightBindGroup = wgpuDeviceCreateBindGroup(presenter.device, &lightDescriptor);
	wgpuBindGroupLayoutRelease(lightLayout);

	std::vector<WGPUBindGroupEntry> presentEntries {
		TextureEntry(0, presenter.frameTextureView),
		SamplerEntry(1, presenter.sampler),
		TextureEntry(2, presenter.lightMaskTextureView),
		TextureEntry(3, presenter.hybridMaskTextureView),
		TextureEntry(4, presenter.auxTextureView),
		TextureEntry(5, presenter.baseTextureView),
		TextureEntry(6, presenter.indexTextureView),
		BufferEntry(7, presenter.presentParamsBuffer, sizeof(GpuPresentParams)),
		TextureEntry(8, presenter.paletteTextureView),
	};
	AppendFloorPresentEntries(presenter, presentEntries); // HD floor (HdFloorWgsl bindings 16-22)
	presenter.hybridPresentBindGroup = CreateBindGroup(presenter, wgpuRenderPipelineGetBindGroupLayout(presenter.hybridPresentPipeline, 0),
	    "DevilutionX hybrid present bind group", presentEntries);
	return presenter.hybridLightBindGroup != nullptr && presenter.hybridPresentBindGroup != nullptr;
}

bool CreateBindGroups(WebGpuPresenter &presenter)
{
	return CreateLegacyBindGroups(presenter) && CreateHybridBindGroups(presenter) && CreateFloorBindGroups(presenter);
}

void WriteWholeTexture(WebGpuPresenter &presenter, WGPUTexture texture, const void *data, uint32_t bytesPerRow, uint32_t width, uint32_t height)
{
	WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	destination.texture = texture;
	destination.aspect = WGPUTextureAspect_All;
	WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
	layout.bytesPerRow = bytesPerRow;
	layout.rowsPerImage = height;
	const WGPUExtent3D extent = { width, height, 1 };
	wgpuQueueWriteTexture(presenter.queue, &destination, data, static_cast<size_t>(bytesPerRow) * height, &layout, &extent);
}

/** The opt-in 4x Cathedral floor atlas (hd\cathedral-floor-atlas.png, 1024x512), or a 1x1 placeholder when absent. */
bool CreateFloorAtlas(WebGpuPresenter &presenter)
{
	constexpr char FloorAsset[] = "hd\\cathedral-floor-atlas.png";
	SDLSurfaceUniquePtr floorImage;
	AssetRef floorReference = FindAsset(FloorAsset);
	const size_t floorAssetSize = floorReference.ok() ? floorReference.size() : 0;
	if (floorReference.ok())
		floorImage.reset(LoadPNG(FloorAsset));
	presenter.floorAtlasAvailable = false;
	if (floorImage != nullptr && floorImage->w == 1024 && floorImage->h == 512) {
		floorImage.reset(SDL_ConvertSurface(floorImage.get(), SDL_PIXELFORMAT_RGBA32));
		presenter.floorAtlasAvailable = floorImage != nullptr;
		if (presenter.floorAtlasAvailable)
			Log("Loaded 4x Cathedral floor atlas");
	} else {
		LogError("Cathedral floor atlas unavailable or not 1024x512 (asset bytes {}, {}x{}, {}); using classic floor",
		    floorAssetSize, floorImage != nullptr ? floorImage->w : 0, floorImage != nullptr ? floorImage->h : 0, SDL_GetError());
	}
	const uint32_t atlasWidth = presenter.floorAtlasAvailable ? 1024U : 1U;
	const uint32_t atlasHeight = presenter.floorAtlasAvailable ? 512U : 1U;
	if (!CreateTexture2D(presenter, "4x Cathedral floor atlas", WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding, WGPUTextureFormat_RGBA8Unorm,
	        atlasWidth, atlasHeight, presenter.floorAtlasTexture, presenter.floorAtlasTextureView))
		return false;
	if (presenter.floorAtlasAvailable) {
		FloorAtlasOriginal.resize(static_cast<size_t>(atlasWidth) * atlasHeight * 4);
		for (uint32_t row = 0; row < atlasHeight; ++row) {
			std::copy_n(static_cast<const uint8_t *>(floorImage->pixels) + static_cast<size_t>(row) * floorImage->pitch, atlasWidth * 4,
			    FloorAtlasOriginal.data() + static_cast<size_t>(row) * atlasWidth * 4);
		}
		FloorAppliedTint = { 1.0F, 1.0F, 1.0F };
		FloorTintValid = false;
		for (size_t variant = 0; variant < CathedralCellMean.size(); ++variant) {
			std::array<double, 3> sum {};
			double weight = 0;
			for (uint32_t y = (variant / 4) * 128; y < (variant / 4 + 1) * 128; y += 2) {
				for (uint32_t x = (variant % 4) * 256; x < (variant % 4 + 1) * 256; x += 2) {
					const uint8_t *texel = FloorAtlasOriginal.data() + (static_cast<size_t>(y) * atlasWidth + x) * 4;
					for (size_t c = 0; c < 3; ++c)
						sum[c] += texel[c] * texel[3];
					weight += texel[3];
				}
			}
			for (size_t c = 0; c < 3; ++c)
				CathedralCellMean[variant][c] = weight > 0 ? static_cast<float>(sum[c] / weight) : 128.0F;
		}
		FloorCellMeansDirty = true;
		WriteWholeTexture(presenter, presenter.floorAtlasTexture, FloorAtlasOriginal.data(), atlasWidth * 4, atlasWidth, atlasHeight);
	} else {
		const std::array<uint8_t, 4> emptyFloor { 0, 0, 0, 255 };
		WriteWholeTexture(presenter, presenter.floorAtlasTexture, emptyFloor.data(), 4, 1, 1);
	}
	return true;
}

/** An RGBA8 2D array texture; the view is explicitly 2DArray because the WGSL binding is texture_2d_array. */
bool CreateTextureArray(WebGpuPresenter &presenter, const char *label, uint32_t width, uint32_t height, uint32_t layers,
    WGPUTexture &texture, WGPUTextureView &view)
{
	WGPUTextureDescriptor descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
	descriptor.label = StringView(label);
	descriptor.usage = WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding;
	descriptor.dimension = WGPUTextureDimension_2D;
	descriptor.size = { width, height, layers };
	descriptor.format = WGPUTextureFormat_RGBA8Unorm;
	texture = wgpuDeviceCreateTexture(presenter.device, &descriptor);
	if (texture == nullptr)
		return false;
	WGPUTextureViewDescriptor viewDescriptor = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
	viewDescriptor.dimension = WGPUTextureViewDimension_2DArray;
	viewDescriptor.arrayLayerCount = layers;
	view = wgpuTextureCreateView(texture, &viewDescriptor);
	return view != nullptr;
}

/** The town floor binding before (or without) the HD pages: one black 1x1 layer. */
bool CreateTownFloorPlaceholder(WebGpuPresenter &presenter)
{
	presenter.townFloorAvailable = false;
	if (!CreateTextureArray(presenter, "4x town floor atlas placeholder", 1, 1, 1, presenter.townFloorTexture, presenter.townFloorTextureView))
		return false;
	const std::array<uint8_t, 4> emptyFloor { 0, 0, 0, 255 };
	WriteWholeTexture(presenter, presenter.townFloorTexture, emptyFloor.data(), 4, 1, 1);
	return true;
}

HdFloorManifest TownFloorManifest;

/** The HD cell mean colours of the gain pass: 512 x rows rgba32float (row 0 the Cathedral variants, row 1 + p town page p). */
bool CreateCellMeans(WebGpuPresenter &presenter, uint32_t rows)
{
	WebGpuPresenter::ReleaseTexture(presenter.cellMeansTexture, presenter.cellMeansTextureView);
	presenter.cellMeansRows = 0;
	if (!CreateTexture2D(presenter, "HD floor cell means", WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding, WGPUTextureFormat_RGBA32Float,
	        HdFloorMaxCellsPerPage, rows, presenter.cellMeansTexture, presenter.cellMeansTextureView))
		return false;
	presenter.cellMeansRows = rows;
	FloorCellMeansDirty = true;
	return true;
}

/** Uploads the cell means when the atlases or the Cathedral tint changed (FloorCellMeansDirty). */
void UploadCellMeans(WebGpuPresenter &presenter)
{
	if (!FloorCellMeansDirty || presenter.cellMeansRows == 0)
		return;
	FloorCellMeansDirty = false;
	static std::vector<std::array<float, 4>> Texels;
	Texels.assign(static_cast<size_t>(HdFloorMaxCellsPerPage) * presenter.cellMeansRows, { 128.0F, 128.0F, 128.0F, 0.0F });
	for (size_t variant = 0; variant < CathedralCellMean.size(); ++variant) {
		for (size_t c = 0; c < 3; ++c)
			Texels[variant][c] = std::min(CathedralCellMean[variant][c] * FloorAppliedTint[c], 255.0F);
	}
	for (size_t cell = 0; cell < TownCellMean.size() && cell + HdFloorMaxCellsPerPage < Texels.size(); ++cell) {
		for (size_t c = 0; c < 3; ++c)
			Texels[cell + HdFloorMaxCellsPerPage][c] = TownCellMean[cell][c];
	}
	WriteWholeTexture(presenter, presenter.cellMeansTexture, Texels.data(), HdFloorMaxCellsPerPage * sizeof(Texels[0]), HdFloorMaxCellsPerPage, presenter.cellMeansRows);
}

/**
 * Loads hd\town-floor.txt and its pages into one 4096 x maxHeight x pages texture array, decoding one page at a
 * time so only a single page is in memory. A missing or malformed file logs an error and keeps the classic floor.
 */
bool LoadTownFloorPages(WebGpuPresenter &presenter, WGPUTexture &texture, WGPUTextureView &view)
{
	constexpr char ManifestAsset[] = "hd\\town-floor.txt";
	std::expected<AssetData, std::string> manifestData = LoadAsset(ManifestAsset);
	if (!manifestData.has_value()) {
		LogError("HD town floor unavailable ({}); using classic floor", manifestData.error());
		return false;
	}
	std::expected<HdFloorManifest, std::string> manifest = ParseHdFloorManifest(static_cast<std::string_view>(*manifestData));
	if (!manifest.has_value()) {
		LogError("HD town floor manifest malformed ({}); using classic floor", manifest.error());
		return false;
	}
	const auto layers = static_cast<uint32_t>(manifest->pages.size());
	const auto maxHeight = static_cast<uint32_t>(manifest->MaxPageHeight());
	if (!CreateTextureArray(presenter, "4x town floor atlas", HdFloorPageWidth, maxHeight, layers, texture, view)) {
		LogError("HD town floor: could not create a {}x{}x{} texture array; using classic floor", HdFloorPageWidth, maxHeight, layers);
		return false;
	}
	for (uint32_t layer = 0; layer < layers; ++layer) {
		const HdFloorPage &page = manifest->pages[layer];
		SDLSurfaceUniquePtr image;
		if (FindAsset(page.path).ok())
			image.reset(LoadPNG(page.path.c_str()));
		if (image == nullptr || image->w != page.width || image->h != page.height) {
			LogError("HD town floor page {} ({}) missing or not {}x{} (got {}x{}, {}); using classic floor", layer, page.path,
			    page.width, page.height, image != nullptr ? image->w : 0, image != nullptr ? image->h : 0, SDL_GetError());
			return false;
		}
		if (image->format != SDL_PIXELFORMAT_RGBA32)
			image.reset(SDL_ConvertSurface(image.get(), SDL_PIXELFORMAT_RGBA32));
		if (image == nullptr) {
			LogError("HD town floor page {} ({}) could not be converted ({}); using classic floor", layer, page.path, SDL_GetError());
			return false;
		}
		// Alpha-weighted mean colour of every cell (the gain pass divides the classic colour by it).
		TownCellMean.resize(static_cast<size_t>(layers) * HdFloorMaxCellsPerPage, { 128.0F, 128.0F, 128.0F });
		for (int cell = 0; cell < HdFloorCellsPerRow * (image->h / HdFloorCellHeight); ++cell) {
			const int cellX = (cell % HdFloorCellsPerRow) * HdFloorCellWidth;
			const int cellY = (cell / HdFloorCellsPerRow) * HdFloorCellHeight;
			std::array<double, 3> sum {};
			double weight = 0;
			for (int y = cellY; y < cellY + HdFloorCellHeight; y += 2) {
				const auto *row = static_cast<const uint8_t *>(image->pixels) + static_cast<size_t>(y) * image->pitch;
				for (int x = cellX; x < cellX + HdFloorCellWidth; x += 2) {
					const uint8_t *texel = row + static_cast<size_t>(x) * 4;
					for (size_t c = 0; c < 3; ++c)
						sum[c] += texel[c] * texel[3];
					weight += texel[3];
				}
			}
			if (weight > 0) {
				for (size_t c = 0; c < 3; ++c)
					TownCellMean[static_cast<size_t>(layer) * HdFloorMaxCellsPerPage + cell][c] = static_cast<float>(sum[c] / weight);
			}
		}
		WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
		destination.texture = texture;
		destination.origin = { 0, 0, layer };
		destination.aspect = WGPUTextureAspect_All;
		WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
		layout.bytesPerRow = static_cast<uint32_t>(image->pitch);
		layout.rowsPerImage = static_cast<uint32_t>(image->h);
		const WGPUExtent3D extent = { static_cast<uint32_t>(image->w), static_cast<uint32_t>(image->h), 1 };
		wgpuQueueWriteTexture(presenter.queue, &destination, image->pixels, static_cast<size_t>(image->pitch) * image->h, &layout, &extent);
	}
	TownFloorManifest = *std::move(manifest);
	FloorCellMeansDirty = true;
	Log("Loaded 4x town floor atlas: {} pages, {} piece slots", layers, TownFloorManifest.pieces.size());
	return true;
}

/** Swaps the placeholder for the HD town pages and rebuilds the bind groups that reference them (legacy 5, hybrid 18). */
void LoadTownFloor(WebGpuPresenter &presenter)
{
	WGPUTexture texture = nullptr;
	WGPUTextureView view = nullptr;
	if (!LoadTownFloorPages(presenter, texture, view)) {
		WebGpuPresenter::ReleaseTexture(texture, view);
		return;
	}
	WebGpuPresenter::ReleaseTexture(presenter.townFloorTexture, presenter.townFloorTextureView);
	presenter.townFloorTexture = texture;
	presenter.townFloorTextureView = view;
	// The cell means grow by a row per page; every bind group that samples the town pages is rebuilt.
	presenter.ReleaseBindGroups();
	const bool means = CreateCellMeans(presenter, 1 + static_cast<uint32_t>(TownFloorManifest.pages.size())) || CreateCellMeans(presenter, 1);
	presenter.townFloorAvailable = CreateBindGroups(presenter) && means && presenter.cellMeansRows > 1;
	if (!presenter.townFloorAvailable)
		LogError("HD town floor: could not rebuild the bind groups");
}

/** Size-independent resources: buffers, sampler, the HD floor atlas and the four pipelines. */
bool CreatePipelineResources(WebGpuPresenter &presenter)
{
	presenter.rayTracingParamsBuffer = CreateBuffer(presenter, "DevilutionX ray tracing parameters", WGPUBufferUsage_CopyDst | WGPUBufferUsage_Storage, sizeof(GpuRayTracingParams));
	presenter.occluderBuffer = CreateBuffer(presenter, "DevilutionX ray tracing occluders", WGPUBufferUsage_CopyDst | WGPUBufferUsage_Storage, OccluderBufferSize);
	presenter.hybridParamsBuffer = CreateBuffer(presenter, "DevilutionX hybrid light parameters", WGPUBufferUsage_CopyDst | WGPUBufferUsage_Uniform, sizeof(GpuHybridParams));
	presenter.presentParamsBuffer = CreateBuffer(presenter, "DevilutionX hybrid present parameters", WGPUBufferUsage_CopyDst | WGPUBufferUsage_Uniform, sizeof(GpuPresentParams));
	presenter.floorParamsBuffer = CreateBuffer(presenter, "DamnationX HD floor parameters", WGPUBufferUsage_CopyDst | WGPUBufferUsage_Uniform, sizeof(GpuFloorParams));
	if (presenter.rayTracingParamsBuffer == nullptr || presenter.occluderBuffer == nullptr || presenter.hybridParamsBuffer == nullptr || presenter.presentParamsBuffer == nullptr
	    || presenter.floorParamsBuffer == nullptr)
		return false;

	WGPUSamplerDescriptor samplerDescriptor = WGPU_SAMPLER_DESCRIPTOR_INIT;
	samplerDescriptor.addressModeU = WGPUAddressMode_ClampToEdge;
	samplerDescriptor.addressModeV = WGPUAddressMode_ClampToEdge;
	samplerDescriptor.addressModeW = WGPUAddressMode_ClampToEdge;
	samplerDescriptor.magFilter = WGPUFilterMode_Nearest;
	samplerDescriptor.minFilter = WGPUFilterMode_Nearest;
	samplerDescriptor.mipmapFilter = WGPUMipmapFilterMode_Nearest;
	presenter.sampler = wgpuDeviceCreateSampler(presenter.device, &samplerDescriptor);
	if (presenter.sampler == nullptr)
		return false;
	if (!CreateFloorAtlas(presenter) || !CreateTownFloorPlaceholder(presenter) || !CreateCellMeans(presenter, 1))
		return false;
	if (!CreateTexture2D(presenter, "HD floor tiles", WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding, WGPUTextureFormat_R32Uint,
	        FloorTileGrid, FloorTileGrid, presenter.floorTilesTexture, presenter.floorTilesTextureView))
		return false;

	// HD floor passes: FloorComputeCommon in front of each pass.
	const std::array<std::pair<const char *, const char *>, 5> floorPasses { {
	    { FloorCountWgsl, "floorCountMain" },
	    { FloorDecideWgsl, "floorDecideMain" },
	    { FloorGrowWgsl, "floorGrowMain" },
	    { FloorGainSumWgsl, "floorGainSumMain" },
	    { FloorGainBoxWgsl, "floorGainBoxMain" },
	} };
	for (size_t pass = 0; pass < floorPasses.size(); ++pass) {
		const std::string source = std::string(FloorComputeCommon) + floorPasses[pass].first;
		WGPUShaderModule floorShader = CreateShaderModule(presenter, "DamnationX HD floor pass shader", source.c_str());
		if (floorShader == nullptr)
			return false;
		presenter.floorPipelines[pass] = CreateComputePipeline(presenter, "DamnationX HD floor pass pipeline", floorShader, floorPasses[pass].second);
		wgpuShaderModuleRelease(floorShader);
		if (presenter.floorPipelines[pass] == nullptr)
			return false;
	}

	// The composite and the hybrid present pass share the HD floor lookup (HdFloorWgsl, bindings 16-22).
	const std::string compositeSource = std::string(HdFloorWgsl) + ShaderSource;
	WGPUShaderModule shader = CreateShaderModule(presenter, "DevilutionX present shader", compositeSource.c_str());
	if (shader == nullptr)
		return false;
	presenter.pipeline = CreatePresentPipeline(presenter, "DevilutionX present pipeline", shader);
	wgpuShaderModuleRelease(shader);
	if (presenter.pipeline == nullptr)
		return false;

	WGPUShaderModule rayTracingShader = CreateShaderModule(presenter, "DevilutionX portable ray tracing shader", RayTracingShaderSource);
	if (rayTracingShader == nullptr)
		return false;
	presenter.rayTracingPipeline = CreateComputePipeline(presenter, "DevilutionX portable ray tracing pipeline", rayTracingShader, "rayTraceMain");
	wgpuShaderModuleRelease(rayTracingShader);
	if (presenter.rayTracingPipeline == nullptr)
		return false;

	// The prelude is concatenated in front of each hybrid entry-point source (tools/wgsl_validate.py does the same).
	const std::string hybridLightSource = std::string(HybridPreludeWgsl) + HybridLightWgsl;
	WGPUShaderModule hybridLightShader = CreateShaderModule(presenter, "DevilutionX hybrid light shader", hybridLightSource.c_str());
	if (hybridLightShader == nullptr)
		return false;
	presenter.hybridLightPipeline = CreateComputePipeline(presenter, "DevilutionX hybrid light pipeline", hybridLightShader, "hybridLightMain");
	wgpuShaderModuleRelease(hybridLightShader);
	if (presenter.hybridLightPipeline == nullptr)
		return false;

	const std::string hybridPresentSource = std::string(HybridPreludeWgsl) + HdFloorWgsl + HybridPresentWgsl;
	WGPUShaderModule hybridPresentShader = CreateShaderModule(presenter, "DevilutionX hybrid present shader", hybridPresentSource.c_str());
	if (hybridPresentShader == nullptr)
		return false;
	presenter.hybridPresentPipeline = CreatePresentPipeline(presenter, "DevilutionX hybrid present pipeline", hybridPresentShader);
	wgpuShaderModuleRelease(hybridPresentShader);
	return presenter.hybridPresentPipeline != nullptr;
}

bool CreateFrameResources(WebGpuPresenter &presenter)
{
	return CreatePipelineResources(presenter) && CreateTextures(presenter) && CreateBindGroups(presenter);
}

/** Recreate the mask and both hybrid bind groups when the live mask size differs from ceil(W/s) x ceil(H/s). */
bool EnsureHybridMask(WebGpuPresenter &presenter, uint32_t maskScale)
{
	const uint32_t width = (static_cast<uint32_t>(presenter.sourceWidth) + maskScale - 1) / maskScale;
	const uint32_t height = (static_cast<uint32_t>(presenter.sourceHeight) + maskScale - 1) / maskScale;
	if (presenter.hybridMaskTextureView != nullptr && presenter.hybridMaskWidth == width && presenter.hybridMaskHeight == height) {
		presenter.hybridMaskScale = maskScale;
		return true;
	}
	const uint32_t previousScale = presenter.hybridMaskScale;
	presenter.ReleaseHybridBindGroups();
	presenter.ReleaseHybridMask();
	presenter.hybridMaskScale = maskScale;
	if (CreateHybridMask(presenter) && CreateHybridBindGroups(presenter))
		return true;
	LogError("WebGPU hybrid mask recreation failed ({}x{}, scale {}); restoring scale {}", width, height, maskScale, previousScale);
	// WebGpuIsReady() requires the mask and both hybrid bind groups, so rebuild them at the previous scale rather than
	// leaving them null: the caller then keeps presenting through the legacy path instead of dropping every frame.
	presenter.ReleaseHybridBindGroups();
	presenter.ReleaseHybridMask();
	presenter.hybridMaskScale = previousScale;
	if (!CreateHybridMask(presenter) || !CreateHybridBindGroups(presenter))
		LogError("WebGPU hybrid mask restore at scale {} failed; the presenter is not ready until the next resize", previousScale);
	return false;
}

#ifdef __EMSCRIPTEN__
// Browsers copy every write into shared memory themselves; Chrome measured no gain from smaller writes.
constexpr size_t MaxWriteBytes = SIZE_MAX;
#else
// Dawn stages a write of up to 4 MiB in a ring buffer and a larger one in a buffer of its own, created (and zeroed) per
// write: frame-sized writes go in bands below that.
constexpr size_t MaxWriteBytes = size_t { 4 } << 20;
#endif

/** Uploads `rows` rows of `data` (bytesPerRow apart) into `texture` (width texels) from row `firstRow`, in bands of at most MaxWriteBytes. */
void WriteTextureRows(WebGpuPresenter &presenter, WGPUTexture texture, const uint8_t *data, uint32_t bytesPerRow, uint32_t width, uint32_t firstRow, uint32_t rows)
{
	const auto bandRows = static_cast<uint32_t>(std::max<size_t>(MaxWriteBytes / std::max<uint32_t>(bytesPerRow, 1), 1));
	for (uint32_t row = 0; row < rows; row += bandRows) {
		const uint32_t count = std::min(bandRows, rows - row);
		WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
		destination.texture = texture;
		destination.aspect = WGPUTextureAspect_All;
		destination.origin = { 0, firstRow + row, 0 };
		WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
		layout.bytesPerRow = bytesPerRow;
		layout.rowsPerImage = count;
		const WGPUExtent3D extent = { width, count, 1 };
		wgpuQueueWriteTexture(presenter.queue, &destination, data + static_cast<size_t>(row) * bytesPerRow, static_cast<size_t>(bytesPerRow) * count, &layout, &extent);
	}
}

/** Uploads `rows` rows of a byte plane (bytesPerRow = pitch) into an r8uint W x H texture starting at row `firstRow`. */
void WritePlaneRows(WebGpuPresenter &presenter, WGPUTexture texture, const uint8_t *data, uint32_t pitch, uint32_t firstRow, uint32_t rows)
{
	WriteTextureRows(presenter, texture, data, pitch, static_cast<uint32_t>(presenter.sourceWidth), firstRow, rows);
}

/** What the HD floor needs from this present (PrepareFloorFrame). */
struct FloorFrame {
	bool active = false; // the composite shows the HD floor
	bool decals = false; // Cathedral: the decal passes run
	bool gain = false;   // legacy composite: the gain passes run
};

/**
 * Uploads the HD floor of this present: the rows of the shown-pixel mask that can differ from the GPU copy (this frame's
 * and the last upload's; every other row is zero on both sides), the tile entries of this world pass and the parameters.
 * That is W / 8 bytes per row of the view plus a few KiB, instead of a metadata texel per pixel. `indexRows`: the rows
 * of the palette index plane this present uploads (the shaders read it at the shown pixels).
 */
FloorFrame PrepareFloorFrame(WebGpuPresenter &presenter, bool legacy, const WebGpuLightingData &lighting, int indexRows, int expandRows)
{
	FloorFrame frame;
	const bool atlas = FloorTownMarks ? presenter.townFloorAvailable : presenter.floorAtlasAvailable;
	frame.active = !World3DFrame && FloorShownAny && atlas && FloorLatticeValid && FloorShownBottom <= indexRows && !(legacy && lighting.hybrid.enabled)
	    && FloorWords * 64 >= presenter.sourceWidth && FloorShownBits.size() >= static_cast<size_t>(FloorWords) * presenter.sourceHeight;
	frame.decals = frame.active && !FloorTownMarks;
	frame.gain = frame.active && legacy;
	GpuFloorParams params {};
	if (frame.active) {
		int top = FloorShownTop;
		int bottom = FloorShownBottom;
		if (FloorUploadedTop < FloorUploadedBottom) {
			top = std::min(top, FloorUploadedTop);
			bottom = std::max(bottom, FloorUploadedBottom);
		}
		FloorUploadedTop = FloorShownTop;
		FloorUploadedBottom = FloorShownBottom;
		bottom = std::min(bottom, presenter.sourceHeight);
		if (top < bottom) {
			WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
			destination.texture = presenter.floorMaskTexture;
			destination.aspect = WGPUTextureAspect_All;
			destination.origin = { 0, static_cast<uint32_t>(top), 0 };
			WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
			layout.bytesPerRow = static_cast<uint32_t>(FloorWords * sizeof(uint64_t));
			layout.rowsPerImage = static_cast<uint32_t>(bottom - top);
			const WGPUExtent3D extent = { static_cast<uint32_t>((presenter.sourceWidth + 31) / 32), static_cast<uint32_t>(bottom - top), 1 };
			const size_t bytes = static_cast<size_t>(layout.bytesPerRow) * (bottom - top);
			wgpuQueueWriteTexture(presenter.queue, &destination, FloorShownBits.data() + static_cast<size_t>(top) * FloorWords, bytes, &layout, &extent);
		}
		// Entries of earlier world passes stay on the GPU: no shown pixel lies on them.
		if (!FloorTilesSet.empty()) {
			WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
			destination.texture = presenter.floorTilesTexture;
			destination.aspect = WGPUTextureAspect_All;
			destination.origin = { static_cast<uint32_t>(FloorTilesSet.left), static_cast<uint32_t>(FloorTilesSet.top), 0 };
			WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
			layout.bytesPerRow = FloorTileGrid * sizeof(uint32_t);
			layout.rowsPerImage = static_cast<uint32_t>(FloorTilesSet.bottom - FloorTilesSet.top);
			const WGPUExtent3D extent = { static_cast<uint32_t>(FloorTilesSet.right - FloorTilesSet.left), layout.rowsPerImage, 1 };
			const size_t bytes = static_cast<size_t>(layout.bytesPerRow) * (layout.rowsPerImage - 1) + extent.width * sizeof(uint32_t);
			wgpuQueueWriteTexture(presenter.queue, &destination, &FloorTileTable[static_cast<size_t>(FloorTilesSet.top) * FloorTileGrid + FloorTilesSet.left],
			    bytes, &layout, &extent);
		}
		if (frame.gain)
			UploadCellMeans(presenter);
		const HdWorldTransform &transform = FloorTransform;
		const bool zoomed = !transform.IsIdentity();
		params.lattice = { FloorLatticeX, FloorLatticeY, std::max(transform.quarters, 4),
			FloorFlagActive | (FloorTownMarks ? FloorFlagTown : 0) | (frame.decals ? FloorFlagDecals : 0) | (expandRows > 0 ? FloorFlagExpand : 0) };
		params.view = { zoomed ? transform.offsetX : 0, zoomed ? transform.offsetY : 0,
			zoomed ? transform.world.x + transform.world.width : FloorWidth, zoomed ? transform.world.y + transform.world.height : FloorHeight };
		params.grid = { FloorGainWidth, FloorGainHeight, presenter.sourceWidth, presenter.sourceHeight };
		params.misc = { expandRows, 0, 0, 0 };
	} else {
		params.lattice = { 0, 0, 4, expandRows > 0 ? FloorFlagExpand : 0 };
		params.misc = { expandRows, 0, 0, 0 };
	}
	if (World3DFrame) {
		// The HD floor is off in first person: view/grid/misc.zw carry the UI rectangles whose transparent-index pixels
		// are UI black, not 3D (main panel, left and right panels; x, y, width, height).
		const auto rect = [&](size_t i) {
			const WebGpuUiRect &r = lighting.uiRects[i];
			return std::array<int32_t, 4> { static_cast<int32_t>(r.x), static_cast<int32_t>(r.y), static_cast<int32_t>(r.width), static_cast<int32_t>(r.height) };
		};
		const std::array<int32_t, 4> panel = rect(0);
		params.lattice[3] = (params.lattice[3] & ~FloorFlagActive) | FloorFlagWorld3D;
		params.view = rect(1);
		params.grid = rect(2);
		params.misc[1] = render3d::kUiTransparentIndex;
		params.misc[2] = panel[1] | (panel[3] << 16);
		params.misc[3] = panel[0] | (panel[2] << 16);
	}
	wgpuQueueWriteBuffer(presenter.queue, presenter.floorParamsBuffer, 0, &params, sizeof(params));
	return frame;
}

/** Encodes the HD floor passes of this frame: the decals (Cathedral), then the legacy composite's gain. */
void EncodeFloorPasses(WebGpuPresenter &presenter, WGPUCommandEncoder encoder, const FloorFrame &floor)
{
	if (!floor.decals && !floor.gain)
		return;
	WGPUComputePassDescriptor descriptor = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
	descriptor.label = StringView("DamnationX HD floor passes");
	WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(encoder, &descriptor);
	const auto groupsX = static_cast<uint32_t>((FloorGainWidth + 7) / 8);
	const auto groupsY = static_cast<uint32_t>((FloorGainHeight + 7) / 8);
	const auto dispatch = [&](size_t index) {
		wgpuComputePassEncoderSetPipeline(pass, presenter.floorPipelines[index]);
		wgpuComputePassEncoderSetBindGroup(pass, 0, presenter.floorBindGroups[index], 0, nullptr);
		wgpuComputePassEncoderDispatchWorkgroups(pass, groupsX, groupsY, 1);
	};
	if (floor.decals) {
		dispatch(0); // count
		dispatch(1); // decide
		dispatch(2); // grow
	}
	if (floor.gain) {
		dispatch(3); // gain sums
		dispatch(4); // gain box filter
	}
	wgpuComputePassEncoderEnd(pass);
	wgpuComputePassEncoderRelease(pass);
}

/** Full RGBA frame upload from row `firstRow` (the panel rows below the view and the virtual gamepad live here). */
void WriteFrameRows(WebGpuPresenter &presenter, const SDL_Surface *surface, int firstRow)
{
	if (firstRow >= surface->h)
		return;
	const auto *pixels = static_cast<const uint8_t *>(surface->pixels) + static_cast<size_t>(surface->pitch) * firstRow;
	WriteTextureRows(presenter, presenter.frameTexture, pixels, static_cast<uint32_t>(surface->pitch), static_cast<uint32_t>(surface->w),
	    static_cast<uint32_t>(firstRow), static_cast<uint32_t>(surface->h - firstRow));
}

void FillHybridParams(const WebGpuLightingData &lighting, uint32_t maskScale, GpuHybridParams &params, GpuPresentParams &present)
{
	const WebGpuHybridData &hybrid = lighting.hybrid;
	const auto viewportRows = static_cast<float>(hybrid.planes.rows);
	const auto debugView = static_cast<uint32_t>(hybrid.debugView) & FlagDebugViewMask;
	const uint32_t flags = (lighting.debugOccluders ? FlagDebugOccluders : 0U)
	    | (lighting.bloom ? FlagBloom : 0U)
	    | (lighting.highQuality ? FlagHighQuality : 0U)
	    | FlagHybrid
	    | (hybrid.softShadows ? FlagSoftShadows : 0U)
	    | (debugView << FlagDebugViewShift);

	// Integers travel only through `counts` and `misc`; the float slots marked 0 stay 0.
	params.screenOriginViewport = { lighting.screenOriginX, lighting.screenOriginY, viewportRows, 0.0F };
	params.screenToWorld = { lighting.screenToWorldXX, lighting.screenToWorldXY, lighting.screenToWorldYX, lighting.screenToWorldYY };
	params.counts = { std::min<uint32_t>(hybrid.lightCount, WebGpuMaxHybridLights), WebGpuOccluderGridWidth, WebGpuOccluderGridHeight, flags };
	params.shading = { hybrid.ambientScale, hybrid.heightPixelsPerWorldUnit, 0.0F, hybrid.wrapCard };
	params.tuning = { hybrid.wrapSurface, hybrid.receiverNudge, hybrid.softTapOffset, 0.0F };
	params.misc = { maskScale, hybrid.zoom, hybrid.zoomOffsetX, hybrid.frameCounter };
	for (uint32_t i = 0; i < params.counts[0]; ++i) {
		const WebGpuHybridLight &light = hybrid.lights[i];
		params.lights[i].positionRadius = { light.worldX, light.worldY, light.worldZ, light.radius };
		params.lights[i].colorIntensity = { light.red, light.green, light.blue, light.intensity };
	}
	params.normalLut = hybrid.normalLut;
	for (size_t i = 0; i < hybrid.ambientCurve.size(); ++i)
		params.ambientCurve[i / 4][i % 4] = hybrid.ambientCurve[i];

	present.config = { 1U, hybrid.zoom, debugView, maskScale };
	present.values = { viewportRows, hybrid.emissiveScale, hybrid.exposure, 0.0F };
	present.config2 = { hybrid.zoomOffsetX, hybrid.frameHasUnlit ? 1U : 0U, 0U, 0U };
	for (size_t i = 0; i < hybrid.paletteEmissive.size(); ++i)
		present.paletteEmissive[i / 4][i % 4] = hybrid.paletteEmissive[i];
	present.normalLut = hybrid.normalLut;
}

/** True when the hybrid data describes planes this presenter can upload (preconditions). */
bool HybridFrameIsPresentable(const WebGpuPresenter &presenter, const WebGpuHybridData &hybrid)
{
	const WebGpuGBufferPlanes &planes = hybrid.planes;
	return hybrid.enabled
	    && planes.aux != nullptr && planes.base != nullptr && planes.index != nullptr && hybrid.volume != nullptr
	    && planes.rows > 0 && planes.rows <= static_cast<uint32_t>(presenter.sourceHeight)
	    && planes.pitch >= static_cast<uint32_t>(presenter.sourceWidth);
}

/** Acquires the swapchain texture, encodes compute + present and submits. Shared by the legacy and hybrid paths. */
bool EncodeAndSubmit(WebGpuPresenter &presenter, int drawableWidth, int drawableHeight,
    WGPUComputePipeline computePipeline, WGPUBindGroup computeBindGroup, uint32_t dispatchX, uint32_t dispatchY, const char *computeLabel,
    WGPURenderPipeline renderPipeline, WGPUBindGroup renderBindGroup, const FloorFrame &floor)
{
	RendererDiagnostics &diagnostics = MutableRendererDiagnostics();
	WGPUSurfaceTexture surfaceTexture = WGPU_SURFACE_TEXTURE_INIT;
	wgpuSurfaceGetCurrentTexture(presenter.surface, &surfaceTexture);
	if (surfaceTexture.status == WGPUSurfaceGetCurrentTextureStatus_Outdated || surfaceTexture.status == WGPUSurfaceGetCurrentTextureStatus_Lost) {
		presenter.drawableWidth = 0;
		presenter.drawableHeight = 0;
		presenter.surfaceLost = true;
		++diagnostics.surfaceLossCount;
		return false;
	}
	if (surfaceTexture.texture == nullptr)
		return false;
	if (presenter.surfaceLost) {
		presenter.surfaceLost = false;
		++diagnostics.recoveryCount;
	}
	WGPUTextureView targetView = wgpuTextureCreateView(surfaceTexture.texture, nullptr);

	WGPUCommandEncoderDescriptor encoderDescriptor = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
	WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(presenter.device, &encoderDescriptor);
	EncodeFloorPasses(presenter, encoder, floor);
	const float scale = std::min(
	    static_cast<float>(drawableWidth) / presenter.sourceWidth,
	    static_cast<float>(drawableHeight) / presenter.sourceHeight);
	const float width = presenter.sourceWidth * scale;
	const float height = presenter.sourceHeight * scale;
	if (World3DFrame) // first-person 3D world into the swapchain; the composite below keeps it where the UI is transparent
		render3d::WorldGpuEncode(encoder, targetView, (drawableWidth - width) / 2.0F, (drawableHeight - height) / 2.0F, width, height);
	WGPUComputePassDescriptor computePassDescriptor = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
	computePassDescriptor.label = StringView(computeLabel);
	WGPUComputePassEncoder computePass = wgpuCommandEncoderBeginComputePass(encoder, &computePassDescriptor);
	wgpuComputePassEncoderSetPipeline(computePass, computePipeline);
	wgpuComputePassEncoderSetBindGroup(computePass, 0, computeBindGroup, 0, nullptr);
	wgpuComputePassEncoderDispatchWorkgroups(computePass, dispatchX, dispatchY, 1);
	wgpuComputePassEncoderEnd(computePass);
	wgpuComputePassEncoderRelease(computePass);

	WGPURenderPassColorAttachment colorAttachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
	colorAttachment.view = targetView;
	colorAttachment.loadOp = World3DFrame ? WGPULoadOp_Load : WGPULoadOp_Clear;
	colorAttachment.storeOp = WGPUStoreOp_Store;
	colorAttachment.clearValue = { 0.0, 0.0, 0.0, 1.0 };
	WGPURenderPassDescriptor passDescriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
	passDescriptor.colorAttachmentCount = 1;
	passDescriptor.colorAttachments = &colorAttachment;
	WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &passDescriptor);
	wgpuRenderPassEncoderSetPipeline(pass, renderPipeline);
	wgpuRenderPassEncoderSetBindGroup(pass, 0, renderBindGroup, 0, nullptr);

	wgpuRenderPassEncoderSetViewport(pass, (drawableWidth - width) / 2.0F, (drawableHeight - height) / 2.0F, width, height, 0.0F, 1.0F);
	wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
	if (!World3DFrame) {        // the first-person view has no classic world pass to put HD tiles or sprites on
		HdTilesGpuEncode(pass);   // opt-in HD level tiles over the composite (hd_tiles_webgpu.cpp)
		HdSpritesGpuEncode(pass); // opt-in HD sprites over the composite, same viewport (hd_sprites_webgpu.cpp)
	}
	HdUiGpuEncode(pass);      // opt-in HD UI over everything, same viewport (hd_ui_webgpu.cpp)
	wgpuRenderPassEncoderEnd(pass);
	wgpuRenderPassEncoderRelease(pass);

	WGPUCommandBuffer commandBuffer = wgpuCommandEncoderFinish(encoder, nullptr);
	wgpuCommandEncoderRelease(encoder);
	wgpuQueueSubmit(presenter.queue, 1, &commandBuffer);
	wgpuCommandBufferRelease(commandBuffer);
	GetWebGpuPlatform().presentSurface(presenter.surface);
	wgpuTextureViewRelease(targetView);
	wgpuTextureRelease(surfaceTexture.texture);
	return true;
}

/** The legacy 2D path, unchanged: full frame upload, params + occluders every frame, rayTraceMain, legacy composite. */
bool PresentLegacy(WebGpuPresenter &presenter, SDL_Surface *surface, int drawableWidth, int drawableHeight, const WebGpuLightingData &lighting)
{
	// In game the composite expands the view rows from the palette index plane (as the hybrid present pass does), so
	// only the panel rows below are uploaded as RGBA (2 instead of 8 MB per frame at 1080p), unless the virtual gamepad
	// was drawn into the RGBA frame. The HD floor reads the same plane (the decals, the shown pixels' classic colour).
	// In the browser only HD floor frames do this: Chrome measured the 8 MB RGBA write cheaper than the 2 MB index plane.
	// A hybrid frame that falls back to this path is full-bright: no HD floor then.
	const int viewportRows = gbRunGame && MyPlayer != nullptr ? std::clamp<int>(static_cast<int>(lighting.viewportHeight), 0, presenter.sourceHeight) : 0;
	const bool indexPlane = PalSurface != nullptr && PalSurface->pixels != nullptr && PalSurface->w == presenter.sourceWidth
	    && PalSurface->h == presenter.sourceHeight && viewportRows > 0;
	const int indexRows = indexPlane ? std::max(viewportRows, std::min(FloorShownBottom, presenter.sourceHeight)) : 0;
#ifdef __EMSCRIPTEN__
	const bool expandView = FloorShownAny;
#else
	const bool expandView = true;
#endif
	// First-person view: the composite needs the index plane of the world rows to find the UI's transparent pixels.
	const bool expand = indexPlane && (World3DFrame || (expandView && ControlMode != ControlTypes::VirtualGamepad));
	const FloorFrame floor = PrepareFloorFrame(presenter, /*legacy=*/true, lighting, indexRows, expand ? viewportRows : 0);
	WriteFrameRows(presenter, surface, expand ? viewportRows : 0);
	if (expand || floor.active) {
		WritePlaneRows(presenter, presenter.indexTexture, static_cast<const uint8_t *>(PalSurface->pixels), static_cast<uint32_t>(PalSurface->pitch), 0,
		    static_cast<uint32_t>(indexRows));
		WriteWholeTexture(presenter, presenter.paletteTexture, FloorPalette.data(), PaletteTextureWidth * 4, PaletteTextureWidth, 1);
	}

	GpuRayTracingParams gpuParams {};
	gpuParams.screenOriginViewportEnabled = {
		lighting.screenOriginX,
		lighting.screenOriginY,
		static_cast<float>(lighting.viewportHeight),
		lighting.enabled ? 1.0F : 0.0F,
	};
	gpuParams.screenToWorld = {
		lighting.screenToWorldXX,
		lighting.screenToWorldXY,
		lighting.screenToWorldYX,
		lighting.screenToWorldYY,
	};
	gpuParams.counts = {
		std::min<uint32_t>(lighting.lightCount, WebGpuMaxRayTracedLights),
		WebGpuOccluderGridWidth,
		WebGpuOccluderGridHeight,
		(lighting.debugOccluders ? FlagDebugOccluders : 0U)
		    | (lighting.bloom ? FlagBloom : 0U)
		    | (lighting.highQuality ? FlagHighQuality : 0U)
		    | (lighting.isTown ? FlagLegacyTown : 0U),
	};
	for (size_t i = 0; i < WebGpuUiRectCount; ++i) {
		const WebGpuUiRect &rect = lighting.uiRects[i];
		gpuParams.uiRects[i] = { rect.x, rect.y, rect.width, rect.height };
	}
	for (uint32_t i = 0; i < gpuParams.counts[0]; ++i) {
		const WebGpuRayTracedLight &light = lighting.lights[i];
		gpuParams.lights[i].positionRadius = { light.worldX, light.worldY, light.radius, 0.0F };
		gpuParams.lights[i].colorIntensity = { light.red, light.green, light.blue, light.intensity };
	}
	wgpuQueueWriteBuffer(presenter.queue, presenter.rayTracingParamsBuffer, 0, &gpuParams, sizeof(gpuParams));
	wgpuQueueWriteBuffer(presenter.queue, presenter.occluderBuffer, 0, lighting.occluders.data(), lighting.occluders.size() * sizeof(uint32_t));

	const uint32_t lightMaskWidth = static_cast<uint32_t>((presenter.sourceWidth + 1) / 2);
	const uint32_t lightMaskHeight = static_cast<uint32_t>((presenter.sourceHeight + 1) / 2);
	return EncodeAndSubmit(presenter, drawableWidth, drawableHeight,
	    presenter.rayTracingPipeline, presenter.rayTracingBindGroup, (lightMaskWidth + 7) / 8, (lightMaskHeight + 7) / 8, "DevilutionX portable ray tracing pass",
	    presenter.pipeline, presenter.bindGroup, floor);
}

/** The hybrid present sequence. */
bool PresentHybrid(WebGpuPresenter &presenter, SDL_Surface *surface, int drawableWidth, int drawableHeight, const WebGpuLightingData &lighting)
{
	const WebGpuHybridData &hybrid = lighting.hybrid;
	const WebGpuGBufferPlanes &planes = hybrid.planes;
	const uint32_t maskScale = hybrid.fullResolution ? 1U : 2U;
	if (!EnsureHybridMask(presenter, maskScale)) {
		// Keep the frame: the legacy composite only needs the resources WebGpuIsReady() already checked.
		return presenter.HasSizedTextures() && presenter.HasBindGroups()
		    && PresentLegacy(presenter, surface, drawableWidth, drawableHeight, lighting);
	}

	// 2. RGBA frame: the viewport rows are palette-expanded on the GPU (indexTexture + paletteTexture), so only the
	//    panel strip below the viewport is uploaded, unless the virtual gamepad was drawn into the frame.
	if (hybrid.frameHasUnlit)
		WriteFrameRows(presenter, surface, 0);
	else
		WriteFrameRows(presenter, surface, static_cast<int>(planes.rows));

	// 3. Planes: aux/index rows [0, rows), base rows [0, world rows), bytesPerRow = pitch. Under zoom the shaders read B
	//    at the world row of every viewport row (zoomRow, Zoom()'s rounded-up source).
	const uint32_t zoomQuarters = std::max(hybrid.zoom & 255U, 4U);
	const uint32_t baseRows = planes.rows == 0 ? 0 : ((planes.rows - 1 + (hybrid.zoom >> 8)) * 4) / zoomQuarters + 1;
	WritePlaneRows(presenter, presenter.auxTexture, planes.aux, planes.pitch, 0, planes.rows);
	WritePlaneRows(presenter, presenter.indexTexture, planes.index, planes.pitch, 0, planes.rows);
	WritePlaneRows(presenter, presenter.baseTexture, planes.base, planes.pitch, 0, baseRows);
	WriteWholeTexture(presenter, presenter.paletteTexture, hybrid.palette.data(), PaletteTextureWidth * 4, PaletteTextureWidth, 1);
	// HD floor (HdFloorWgsl): the shown-pixel mask and the tile entries of this world pass. The present shader swaps the
	// HD albedo in for those Floor-kind pixels before lighting. Without ?hdFloor=1 nothing is uploaded.
	const FloorFrame floor = PrepareFloorFrame(presenter, /*legacy=*/false, lighting, static_cast<int>(planes.rows), 0);

	// 4. Volume only when its generation changed (counted as an upload); params every frame.
	if (!presenter.volumeUploaded || presenter.lastVolumeGeneration != hybrid.volumeGeneration) {
		WriteWholeTexture(presenter, presenter.volumeTexture, hybrid.volume, WebGpuOccluderGridWidth * sizeof(uint32_t), WebGpuOccluderGridWidth, WebGpuOccluderGridHeight);
		presenter.volumeUploaded = true;
		presenter.lastVolumeGeneration = hybrid.volumeGeneration;
		++MutableRendererDiagnostics().uploadCount;
	}
	static GpuHybridParams HybridParams;
	static GpuPresentParams PresentParams;
	HybridParams = {};
	PresentParams = {};
	FillHybridParams(lighting, maskScale, HybridParams, PresentParams);
	wgpuQueueWriteBuffer(presenter.queue, presenter.hybridParamsBuffer, 0, &HybridParams, sizeof(HybridParams));
	wgpuQueueWriteBuffer(presenter.queue, presenter.presentParamsBuffer, 0, &PresentParams, sizeof(PresentParams));

	// 5./6. Full-mask dispatch (rows below the viewport store pass-through), then the hybrid composite.
	return EncodeAndSubmit(presenter, drawableWidth, drawableHeight,
	    presenter.hybridLightPipeline, presenter.hybridLightBindGroup, (presenter.hybridMaskWidth + 7) / 8, (presenter.hybridMaskHeight + 7) / 8, "DevilutionX hybrid light pass",
	    presenter.hybridPresentPipeline, presenter.hybridPresentBindGroup, floor);
}

} // namespace

bool WebGpuInitialize(const char *canvasSelector, int sourceWidth, int sourceHeight)
{
	const WebGpuPlatform &platform = GetWebGpuPlatform();
	RendererDiagnostics &diagnostics = MutableRendererDiagnostics();
	Presenter.Shutdown();
	Presenter.sourceWidth = sourceWidth;
	Presenter.sourceHeight = sourceHeight;
	Presenter.instance = wgpuCreateInstance(nullptr);
	if (Presenter.instance == nullptr)
		return false;

	Presenter.surface = platform.createSurface(Presenter.instance, canvasSelector);
	if (Presenter.surface == nullptr)
		return false;

	WGPURequestAdapterOptions adapterOptions = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
	adapterOptions.compatibleSurface = Presenter.surface;
	WGPURequestAdapterCallbackInfo adapterCallback = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
	adapterCallback.mode = WGPUCallbackMode_AllowSpontaneous;
	adapterCallback.callback = OnAdapterRequest;
	adapterCallback.userdata1 = &Presenter;
	Presenter.adapterRequestFinished = false;
	wgpuInstanceRequestAdapter(Presenter.instance, &adapterOptions, adapterCallback);
	platform.waitUntil(&Presenter.adapterRequestFinished);
	if (Presenter.adapter == nullptr)
		return false;

	WGPUDeviceDescriptor deviceDescriptor = WGPU_DEVICE_DESCRIPTOR_INIT;
	deviceDescriptor.label = StringView("DevilutionX WebGPU device");
	deviceDescriptor.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
	deviceDescriptor.deviceLostCallbackInfo.callback = OnDeviceLost;
	deviceDescriptor.deviceLostCallbackInfo.userdata1 = &Presenter;
	deviceDescriptor.uncapturedErrorCallbackInfo.callback = OnUncapturedError;
	deviceDescriptor.uncapturedErrorCallbackInfo.userdata1 = &Presenter;
	WGPURequestDeviceCallbackInfo deviceCallback = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
	deviceCallback.mode = WGPUCallbackMode_AllowSpontaneous;
	deviceCallback.callback = OnDeviceRequest;
	deviceCallback.userdata1 = &Presenter;
	Presenter.deviceRequestFinished = false;
	wgpuAdapterRequestDevice(Presenter.adapter, &deviceDescriptor, deviceCallback);
	platform.waitUntil(&Presenter.deviceRequestFinished);
	if (Presenter.device == nullptr)
		return false;
	Presenter.queue = wgpuDeviceGetQueue(Presenter.device);

	WGPUSurfaceCapabilities capabilities = WGPU_SURFACE_CAPABILITIES_INIT;
	if (wgpuSurfaceGetCapabilities(Presenter.surface, Presenter.adapter, &capabilities) != WGPUStatus_Success || capabilities.formatCount == 0) {
		++diagnostics.capabilityFailureCount;
		return false;
	}
	Presenter.surfaceFormat = capabilities.formats[0];
	for (size_t i = 0; i < capabilities.formatCount; ++i) {
		if (capabilities.formats[i] == WGPUTextureFormat_BGRA8Unorm) {
			Presenter.surfaceFormat = capabilities.formats[i];
			break;
		}
	}
	wgpuSurfaceCapabilitiesFreeMembers(capabilities);

	if (!CreateFrameResources(Presenter)) {
		Presenter.Shutdown();
		return false;
	}
	Log("WebGPU presenter initialized ({}x{})", sourceWidth, sourceHeight);
	platform.notifyState("ready", "Ready to play.", false, false);
	return true;
}

bool WebGpuResize(int sourceWidth, int sourceHeight)
{
	if (!WebGpuIsReady() || sourceWidth <= 0 || sourceHeight <= 0)
		return false;
	Presenter.ReleaseBindGroups();
	Presenter.ReleaseTextures();
	Presenter.sourceWidth = sourceWidth;
	Presenter.sourceHeight = sourceHeight;
	if (!CreateTextures(Presenter) || !CreateBindGroups(Presenter)) {
		++MutableRendererDiagnostics().resizeFailureCount;
		LogError("WebGPU resize to {}x{} failed", sourceWidth, sourceHeight);
		return false;
	}
	return true;
}

bool WebGpuPresent(SDL_Surface *surface, int drawableWidth, int drawableHeight, const WebGpuLightingData &lighting)
{
	if (!WebGpuIsReady() || surface == nullptr || !ConfigureSurface(Presenter, drawableWidth, drawableHeight))
		return false;

	World3DFrame = render3d::WorldPassActive()
	    && render3d::WorldGpuPrepare(render3d::WorldGpuFrame {
	        .device = Presenter.device,
	        .queue = Presenter.queue,
	        .colorFormat = Presenter.surfaceFormat,
	        .sourceWidth = Presenter.sourceWidth,
	        .sourceHeight = Presenter.sourceHeight,
	        .drawableWidth = drawableWidth,
	        .drawableHeight = drawableHeight,
	        .viewportRows = static_cast<int>(lighting.viewportHeight),
	        .lights = lighting.worldLights.data(),
	        .lightCount = lighting.worldLightCount,
	    });
	const WebGpuHybridData &hybrid = lighting.hybrid;
	// The first-person view composites through the legacy path (the hybrid planes describe no world pass then).
	const bool hybridPresentable = !World3DFrame && HybridFrameIsPresentable(Presenter, hybrid);
	if (hybrid.enabled && !hybridPresentable && !World3DFrame) {
		if (!Presenter.loggedUnpresentableHybrid) {
			LogError("WebGPU hybrid frame is not presentable (planes aux={} base={} index={} volume={}, pitch {} rows {} for {}x{}); using the legacy path",
			    hybrid.planes.aux != nullptr, hybrid.planes.base != nullptr, hybrid.planes.index != nullptr, hybrid.volume != nullptr,
			    hybrid.planes.pitch, hybrid.planes.rows, Presenter.sourceWidth, Presenter.sourceHeight);
			Presenter.loggedUnpresentableHybrid = true;
		}
	} else {
		Presenter.loggedUnpresentableHybrid = false;
	}
	HdTilesGpuPrepare(HdTilesGpuFrame {
	    .device = Presenter.device,
	    .queue = Presenter.queue,
	    .surfaceFormat = Presenter.surfaceFormat,
	    .sourceWidth = Presenter.sourceWidth,
	    .sourceHeight = Presenter.sourceHeight,
	    .legacyComposite = !hybridPresentable,
	    .lightMaskView = Presenter.lightMaskTextureView,
	    .hybridMaskView = Presenter.hybridMaskTextureView,
	    .auxView = Presenter.auxTextureView,
	    .baseView = Presenter.baseTextureView,
	    .indexView = Presenter.indexTextureView,
	    .paletteView = Presenter.paletteTextureView,
	    .presentParamsBuffer = Presenter.presentParamsBuffer,
	    .presentParamsSize = sizeof(GpuPresentParams),
	});
	HdSpritesGpuPrepare(HdSpritesGpuFrame {
	    .device = Presenter.device,
	    .queue = Presenter.queue,
	    .surfaceFormat = Presenter.surfaceFormat,
	    .lightMaskView = Presenter.lightMaskTextureView,
	    .sourceWidth = Presenter.sourceWidth,
	    .sourceHeight = Presenter.sourceHeight,
	    .drawableWidth = drawableWidth,
	    .drawableHeight = drawableHeight,
	    .legacyComposite = !hybridPresentable,
	    .hybridParamsBuffer = Presenter.hybridParamsBuffer,
	    .hybridParamsSize = sizeof(GpuHybridParams),
	    .hybridVolumeView = Presenter.volumeTextureView,
	    .hybridExposure = hybrid.exposure,
	});
	HdUiGpuPrepare(HdUiGpuFrame {
	    .device = Presenter.device,
	    .queue = Presenter.queue,
	    .surfaceFormat = Presenter.surfaceFormat,
	    .sourceWidth = Presenter.sourceWidth,
	    .sourceHeight = Presenter.sourceHeight,
	    .drawableWidth = drawableWidth,
	    .drawableHeight = drawableHeight,
	});
	const bool presented = hybridPresentable
	    ? PresentHybrid(Presenter, surface, drawableWidth, drawableHeight, lighting)
	    : PresentLegacy(Presenter, surface, drawableWidth, drawableHeight, lighting);
	if (!presented)
		return false;

	++MutableRendererDiagnostics().frameCount;
	return true;
}

void WebGpuShutdown()
{
	Presenter.Shutdown();
}

bool WebGpuIsReady()
{
	return Presenter.device != nullptr && Presenter.queue != nullptr && !Presenter.deviceLost
	    && Presenter.HasPipelines() && Presenter.HasSizedTextures() && Presenter.HasBindGroups();
}

static bool HdFloorUserEnabled()
{
	// The HD floor is an opt-in preview, so the classic floor stays the default; Graphics > HD Graphics switches it off.
	static const bool UserEnabled = GetWebGpuPlatform().queryFlag("hdFloor");
	return UserEnabled && HdGraphicsEnabled();
}

namespace {

/** 0x01 in byte i for every set bit i: eight marked pixels to a byte mask. */
const std::array<uint64_t, 256> FloorByteMask = [] {
	std::array<uint64_t, 256> masks {};
	for (size_t bits = 0; bits < masks.size(); ++bits) {
		for (size_t i = 0; i < 8; ++i) {
			if (((bits >> i) & 1U) != 0)
				masks[bits] |= uint64_t { 1 } << (8 * i);
		}
	}
	return masks;
}();

/**
 * 0x01 in every byte of `v` that is 2 or more: the indices the capture swaps with their ramp neighbour (index ^ 1). 0
 * and 1 stay: per-pixel lighting darkens floor pixels to black, and 1 is a bright colour, which blended (TransList)
 * walls and pillars over a swapped black pixel would mix in as blue-white speckles.
 */
inline uint64_t SwappableBytes(uint64_t v)
{
	constexpr uint64_t Low7 = 0x7F7F7F7F7F7F7F7FULL;
	const uint64_t high = v & 0xFEFEFEFEFEFEFEFEULL;
	return ((((high & Low7) + Low7) | high) >> 7) & 0x0101010101010101ULL;
}

/** Bit i set where byte i of `diff` is nonzero. */
inline uint64_t NonzeroByteBits(uint64_t diff)
{
	diff |= diff >> 4;
	diff |= diff >> 2;
	diff |= diff >> 1;
	return ((diff & 0x0101010101010101ULL) * 0x0102040810204080ULL) >> 56;
}

inline uint8_t SwapFloorIndex(uint8_t index)
{
	return index >= 2 ? static_cast<uint8_t>(index ^ 1U) : index;
}

/** Sets bits [first, last) of a bit row. */
void SetFloorBits(uint64_t *row, int first, int last)
{
	if (first >= last)
		return;
	const int firstWord = first >> 6;
	const int lastWord = (last - 1) >> 6;
	const uint64_t firstMask = ~uint64_t { 0 } << (first & 63);
	const uint64_t lastMask = ~uint64_t { 0 } >> (63 - ((last - 1) & 63));
	if (firstWord == lastWord) {
		row[firstWord] |= firstMask & lastMask;
		return;
	}
	row[firstWord] |= firstMask;
	std::fill(row + firstWord + 1, row + lastWord, ~uint64_t { 0 });
	row[lastWord] |= lastMask;
}

uint64_t *FloorMarkedRow(int y)
{
	return FloorMarkedBits.data() + static_cast<size_t>(y) * FloorWords;
}

uint64_t *FloorShownRow(int y)
{
	return FloorShownBits.data() + static_cast<size_t>(y) * FloorWords;
}

/** The tile table entry of the diamond holding world pixel (wx, wy) (HdFloorWgsl floorTileOf). */
uint32_t FloorTileInfoAt(int wx, int wy)
{
	const int x = 2 * (wx - FloorLatticeX) + 1 - 64;
	const int y = 2 * (wy - FloorLatticeY) + 1 + 32;
	const int tileX = HdWorldTransform::FloorDiv(x + 2 * y + 64, 128);
	const int tileY = HdWorldTransform::FloorDiv(2 * y - x + 64, 128);
	if (tileX < 0 || tileY < 0 || tileX >= FloorTileGrid || tileY >= FloorTileGrid)
		return 0;
	return FloorTileTable[static_cast<size_t>(tileY) * FloorTileGrid + tileX];
}

/** Records a dungeon tile's cell and light for the shaders; false when it is not on this world pass's grid. */
bool SetFloorTile(int x, int y, int tileX, int tileY, uint32_t code, int lightTableIndex)
{
	if (tileX < 0 || tileY < 0 || tileX >= FloorTileGrid || tileY >= FloorTileGrid)
		return false;
	// Every diamond of a world pass lies on one grid: tile (x + 1, y) is 32 pixels right and 16 down, (x, y + 1) 32 left.
	const int latticeX = x - 32 * (tileX - tileY);
	const int latticeY = y - 16 * (tileX + tileY);
	if (!FloorLatticeValid) {
		FloorLatticeX = latticeX;
		FloorLatticeY = latticeY;
		FloorLatticeValid = true;
	} else if (latticeX != FloorLatticeX || latticeY != FloorLatticeY) {
		return false;
	}
	const size_t index = static_cast<size_t>(tileY) * FloorTileGrid + tileX;
	if (FloorTileTable[index] == 0)
		FloorTileList.push_back(static_cast<uint16_t>(index));
	FloorTileTable[index] = code | (static_cast<uint32_t>(std::clamp(lightTableIndex, 0, 15) + 1) << 16);
	FloorTilesSet.Add(tileX, tileY, tileX + 1, tileY + 1);
	return true;
}

/** Clears the marks of the last world pass (its marked bit rows and tile entries). */
void ClearFloorMarks()
{
	if (!FloorMarkedBits.empty()) {
		const int bottom = std::min<int>(FloorMarked.bottom, static_cast<int>(FloorMarkedBits.size() / std::max(FloorWords, 1)));
		for (int y = FloorMarked.top; y < bottom; ++y)
			std::fill_n(FloorMarkedRow(y), FloorWords, uint64_t { 0 });
	}
	FloorMarked = {};
	for (const uint16_t index : FloorTileList)
		FloorTileTable[index] = 0;
	FloorTileList.clear();
	FloorTilesSet = {};
	FloorLatticeValid = false;
}

/** Zeroes the shown bits (the rows that hold any). */
void ClearFloorShown()
{
	for (int y = FloorShownTop; y < FloorShownBottom; ++y)
		std::fill_n(FloorShownRow(y), FloorWords, uint64_t { 0 });
	FloorShownTop = 0;
	FloorShownBottom = 0;
	FloorShownAny = false;
}

/** Records one floor diamond (or its halves, bit 0 left / bit 1 right) at x, y (bottom-left of the 64x32 diamond). */
void MarkFloorDiamond(int x, int y, int tileX, int tileY, uint32_t code, int lightTableIndex, uint8_t halves)
{
	const int left = std::max(0, (halves & 1U) != 0 ? x : x + 32);
	const int right = std::min(FloorWidth, (halves & 2U) != 0 ? x + 64 : x + 32);
	const int top = std::max(0, y - 32);
	const int bottom = std::min(FloorHeight, y);
	if (left >= right || top >= bottom || !SetFloorTile(x, y, tileX, tileY, code, lightTableIndex))
		return;
	// A pixel is inside when |px + 0.5 - x - 32| / 32 + |py + 0.5 - y + 16| / 16 <= 1. Both terms are multiples of 1/64
	// and their sum is never exactly 1, so the diamonds tile the plane: row py holds the columns
	// [x + 32 - reach, x + 32 + reach), reach = 32 - |2 py + 33 - 2 y|.
	FloorRect marked;
	for (int py = top; py < bottom; ++py) {
		const int reach = 32 - std::abs(2 * py + 33 - 2 * y);
		const int first = std::max(left, x + 32 - reach);
		const int last = std::min(right, x + 32 + reach);
		if (first >= last)
			continue;
		SetFloorBits(FloorMarkedRow(py), first, last);
		marked.Add(first, py, last, py + 1);
	}
	FloorMarked.Add(marked.left, marked.top, marked.right, marked.bottom);
}

/**
 * The shown bits of one screen row: the marked pixels whose frame index still equals the captured one. `marked` and
 * `captured` are this row's marks and capture in screen columns.
 */
bool DiffFloorRow(const uint64_t *marked, const uint8_t *captured, const uint8_t *frame, uint64_t *shown, int left, int right)
{
	bool any = false;
	for (int word = left >> 6; word <= (right - 1) >> 6; ++word) {
		const uint64_t bits = marked[word];
		if (bits == 0)
			continue;
		const int base = word * 64;
		uint64_t kept = bits;
		if (base + 64 <= FloorWidth) {
			// Eight-byte compares (a byte-wise memcmp in the browser's libc was the slow part): most words are unchanged.
			std::array<uint64_t, 8> diff;
			uint64_t any = 0;
			for (size_t chunk = 0; chunk < diff.size(); ++chunk) {
				uint64_t now;
				uint64_t then;
				std::memcpy(&now, frame + base + 8 * chunk, sizeof(now));
				std::memcpy(&then, captured + base + 8 * chunk, sizeof(then));
				diff[chunk] = now ^ then;
				any |= diff[chunk];
			}
			if (any != 0) {
				uint64_t changed = 0;
				for (size_t chunk = 0; chunk < diff.size(); ++chunk) {
					if (diff[chunk] != 0)
						changed |= NonzeroByteBits(diff[chunk]) << (8 * chunk);
				}
				kept &= ~changed;
			}
		} else {
			for (int x = base; x < FloorWidth; ++x) {
				if (frame[x] != captured[x])
					kept &= ~(uint64_t { 1 } << (x - base));
			}
		}
		shown[word] = kept;
		any = any || kept != 0;
	}
	return any;
}

/**
 * Rebuilds the shown bits from the presented frame: a marked pixel that still holds its swapped index was not drawn
 * over (a wall, sprite, shadow or UI drawn later changes it), so it shows the HD floor. Under zoom the marks and the
 * capture are resampled to the screen exactly as Zoom() resampled the frame.
 */
void BuildFloorShown(const uint8_t *pixels, int pitch)
{
	ClearFloorShown();
	const int markedBottom = std::min(FloorMarked.bottom, FloorHeight);
	if (FloorMarked.empty() || markedBottom <= FloorMarked.top)
		return;
	const HdWorldTransform &transform = FloorTransform;
	const auto noteRow = [](int y) {
		if (FloorShownTop >= FloorShownBottom)
			FloorShownTop = y;
		FloorShownBottom = y + 1;
		FloorShownAny = true;
	};
	if (transform.IsIdentity()) {
		for (int y = FloorMarked.top; y < markedBottom; ++y) {
			const size_t offset = static_cast<size_t>(y) * FloorWidth;
			if (DiffFloorRow(FloorMarkedRow(y), FloorCapturedFrame.data() + offset, pixels + static_cast<size_t>(y) * pitch, FloorShownRow(y),
			        FloorMarked.left, FloorMarked.right))
				noteRow(y);
		}
		return;
	}
	const HdRect &viewport = transform.viewport;
	const int viewLeft = std::max(viewport.x, 0);
	const int viewRight = std::min(viewport.x + viewport.width, FloorWidth);
	if (viewLeft >= viewRight)
		return;
	static std::vector<int> SourceColumn;
	static std::vector<uint64_t> ZoomedMarks;
	static std::vector<uint8_t> ZoomedCapture;
	SourceColumn.resize(static_cast<size_t>(FloorWidth));
	for (int x = viewLeft; x < viewRight; ++x)
		SourceColumn[static_cast<size_t>(x)] = std::clamp(transform.WorldX(x), transform.world.x, transform.world.x + transform.world.width - 1);
	ZoomedMarks.assign(static_cast<size_t>(FloorWords), 0);
	ZoomedCapture.assign(static_cast<size_t>(FloorWords) * 64 + 8, 0);
	int zoomedRow = -1;
	const int screenBottom = std::min(viewport.y + viewport.height, Presenter.sourceHeight);
	for (int y = std::max(viewport.y, 0); y < screenBottom; ++y) {
		const int worldY = std::clamp(transform.WorldY(y), transform.world.y, transform.world.y + transform.world.height - 1);
		if (worldY < FloorMarked.top || worldY >= markedBottom)
			continue;
		if (worldY != zoomedRow) {
			zoomedRow = worldY;
			std::fill(ZoomedMarks.begin(), ZoomedMarks.end(), uint64_t { 0 });
			const uint64_t *marks = FloorMarkedRow(worldY);
			const uint8_t *capture = FloorCapturedFrame.data() + static_cast<size_t>(worldY) * FloorWidth;
			for (int x = viewLeft; x < viewRight; ++x) {
				const int source = SourceColumn[static_cast<size_t>(x)];
				if (((marks[source >> 6] >> (source & 63)) & 1U) == 0)
					continue;
				ZoomedMarks[static_cast<size_t>(x >> 6)] |= uint64_t { 1 } << (x & 63);
				ZoomedCapture[static_cast<size_t>(x)] = capture[source];
			}
		}
		if (DiffFloorRow(ZoomedMarks.data(), ZoomedCapture.data(), pixels + static_cast<size_t>(y) * pitch, FloorShownRow(y), viewLeft, viewRight))
			noteRow(y);
	}
}

/**
 * Tints the Cathedral atlas toward the level palette: the average colour of well-lit HD floor pixels divided by its
 * luminance (a hue, not the classic brick pattern), smoothed over frames; the atlas is re-uploaded only when the
 * smoothed tint moves by more than 2%. The samples are every 8th pixel of every 8th row, without the decal colours.
 */
void UpdateCathedralFloorTint(const uint8_t *pixels, int pitch)
{
	if (FloorAtlasOriginal.empty() || !WebGpuIsReady())
		return;
	std::array<uint32_t, 256> histogram {};
	size_t count = 0;
	for (int y = (FloorShownTop + 7) & ~7; y < FloorShownBottom; y += 8) {
		const uint64_t *shown = FloorShownRow(y);
		const uint8_t *row = pixels + static_cast<size_t>(y) * pitch;
		const int worldY = FloorTransform.IsIdentity() ? y : FloorTransform.WorldY(y);
		for (int word = 0; word < FloorWords; ++word) {
			const uint64_t bits = shown[word];
			if (bits == 0)
				continue;
			for (int i = 0; i < 64; i += 8) {
				if (((bits >> i) & 1U) == 0)
					continue;
				const int x = word * 64 + i;
				const uint8_t index = SwapFloorIndex(row[x]);
				if ((index >= 48 && index < 64) || (index >= 80 && index < 96) || (index >= 1 && index < 32))
					continue; // blood and bones (decals)
				const int worldX = FloorTransform.IsIdentity() ? x : FloorTransform.WorldX(x);
				const uint32_t light = (FloorTileInfoAt(worldX, worldY) >> 16) & 31U;
				if (light == 0 || light > 4)
					continue; // light level 4 and darker: the light tables crush the hue
				++histogram[index];
				++count;
			}
		}
	}
	if (count < 64)
		return;
	std::array<double, 3> sum {};
	for (size_t index = 0; index < histogram.size(); ++index) {
		const uint32_t samples = histogram[index];
		if (samples == 0)
			continue;
		for (size_t c = 0; c < 3; ++c)
			sum[c] += static_cast<double>(FloorPalette[index][c]) * samples;
	}
	const double luminance = (0.2126 * sum[0] + 0.7152 * sum[1] + 0.0722 * sum[2]) / static_cast<double>(count);
	if (luminance < 4.0)
		return;
	for (size_t c = 0; c < 3; ++c) {
		const auto tint = static_cast<float>(std::clamp(sum[c] / static_cast<double>(count) / luminance, 0.4, 2.0));
		FloorTint[c] = FloorTintValid ? FloorTint[c] * 0.9F + tint * 0.1F : tint;
	}
	FloorTintValid = true;
	float change = 0.0F;
	for (size_t c = 0; c < 3; ++c)
		change = std::max(change, std::abs(FloorTint[c] - FloorAppliedTint[c]));
	if (change <= 0.02F)
		return;
	FloorAppliedTint = FloorTint;
	FloorCellMeansDirty = true;
	Log("HD Cathedral floor tint {:.2f} {:.2f} {:.2f} ({} samples)", FloorAppliedTint[0], FloorAppliedTint[1], FloorAppliedTint[2], count);
	static std::vector<uint8_t> Tinted;
	Tinted.resize(FloorAtlasOriginal.size());
	for (size_t i = 0; i < FloorAtlasOriginal.size(); i += 4) {
		for (size_t c = 0; c < 3; ++c)
			Tinted[i + c] = static_cast<uint8_t>(std::clamp(FloorAtlasOriginal[i + c] * FloorAppliedTint[c] + 0.5F, 0.0F, 255.0F));
		Tinted[i + 3] = FloorAtlasOriginal[i + 3];
	}
	WriteWholeTexture(Presenter, Presenter.floorAtlasTexture, Tinted.data(), 1024 * 4, 1024, 512);
}

} // namespace

/**
 * The world pass marks (and captures) floor pixels in world-pass pixels: `width` x `height` is the world pass's view,
 * the top-left quarter under zoom. HdCurrentWorldTransform() (set by DrawGame) maps it to the screen; under zoom
 * WebGpuFinishCathedralFloor resamples the marks to screen pixels and the shaders map every screen pixel back.
 */
void WebGpuBeginCathedralFloor(int width, int height, bool enabled)
{
	const bool UserEnabled = HdFloorUserEnabled();
	ClearFloorMarks();
	FloorWidth = width;
	FloorHeight = height;
	FloorTransform = HdCurrentWorldTransform();
	FloorCaptured = false;
	FloorActive = enabled && UserEnabled && width == Presenter.sourceWidth && height <= Presenter.sourceHeight
	    && FloorTransform.viewport.x + FloorTransform.viewport.width <= Presenter.sourceWidth
	    && FloorTransform.viewport.y + FloorTransform.viewport.height <= Presenter.sourceHeight;
	if (!FloorActive)
		return;
	const int words = (width + 63) / 64;
	const size_t size = static_cast<size_t>(words) * Presenter.sourceHeight;
	if (words != FloorWords || FloorMarkedBits.size() != size || FloorShownBits.size() != size) {
		FloorWords = words;
		FloorMarkedBits.assign(size, 0);
		FloorShownBits.assign(size, 0);
		FloorShownTop = 0;
		FloorShownBottom = 0;
		FloorShownAny = false;
		// The GPU mask keeps its rows; the next upload covers them.
		FloorUploadedTop = 0;
		FloorUploadedBottom = Presenter.sourceHeight;
	}
	// The capture copy, padded for the eight-byte compares at the end of the last row.
	FloorCapturedFrame.resize(static_cast<size_t>(width) * Presenter.sourceHeight + 64);
}

void WebGpuMarkCathedralFloor(int x, int y, int tileX, int tileY, int lightTableIndex, uint8_t halves)
{
	if (!FloorActive || lightTableIndex >= 12 || !Presenter.floorAtlasAvailable || halves == 0)
		return;
	uint32_t variant = static_cast<uint32_t>(tileX) * 0x9E3779B1U ^ static_cast<uint32_t>(tileY) * 0x85EBCA77U;
	variant ^= variant >> 16;
	FloorTownMarks = false;
	MarkFloorDiamond(x, y, tileX, tileY, variant & 15, lightTableIndex, halves);
}

bool WebGpuPrepareTownFloor()
{
	if (!HdFloorUserEnabled() || !WebGpuIsReady())
		return false;
	if (!Presenter.townFloorLoadAttempted) {
		Presenter.townFloorLoadAttempted = true;
		LoadTownFloor(Presenter);
	}
	return Presenter.townFloorAvailable;
}

void WebGpuMarkTownFloor(int x, int y, int tileX, int tileY, int pieceId, int lightTableIndex)
{
	if (!FloorActive || lightTableIndex >= 12 || !Presenter.townFloorAvailable || pieceId < 0)
		return;
	const std::optional<HdFloorCell> cell = TownFloorManifest.Lookup(static_cast<size_t>(pieceId));
	if (!cell.has_value())
		return;
	// Bit 15 marks a town cell for the shaders; bits 9..14 page, bits 0..8 cell.
	FloorTownMarks = true;
	MarkFloorDiamond(x, y, tileX, tileY, 0x8000U | (static_cast<uint32_t>(cell->page) << 9) | cell->cell, lightTableIndex, 3);
}

bool WebGpuTownSolidFloorPiece(int pieceId)
{
	return FloorActive && FloorCaptured && Presenter.townFloorAvailable && pieceId >= 0
	    && TownFloorManifest.Lookup(static_cast<size_t>(pieceId)).has_value();
}

void WebGpuCaptureTownSolidFloor(uint8_t *pixels, int pitch, int x, int y, int tileX, int tileY, int pieceId, int lightTableIndex, const uint8_t *written)
{
	if (!WebGpuTownSolidFloorPiece(pieceId) || lightTableIndex >= 12 || pixels == nullptr || written == nullptr)
		return;
	const std::optional<HdFloorCell> cell = TownFloorManifest.Lookup(static_cast<size_t>(pieceId));
	const uint32_t code = 0x8000U | (static_cast<uint32_t>(cell->page) << 9) | cell->cell;
	if (!SetFloorTile(x, y, tileX, tileY, code, lightTableIndex))
		return;
	FloorTownMarks = true;
	FloorRect marked;
	for (int py = std::max(0, y - 32); py < std::min(FloorHeight, y); ++py) {
		// The diamond's columns in this row (the same test as MarkFloorDiamond), clipped to the world pass.
		const int reach = 32 - std::abs(2 * py + 33 - 2 * y);
		const int first = std::max({ 0, x, x + 32 - reach });
		const int last = std::min({ FloorWidth, x + 64, x + 32 + reach });
		if (first >= last)
			continue;
		const uint8_t *writtenRow = written + static_cast<size_t>(py - (y - 32)) * 64;
		uint8_t *row = pixels + static_cast<size_t>(py) * pitch;
		uint8_t *captured = FloorCapturedFrame.data() + static_cast<size_t>(py) * FloorWidth;
		uint64_t *bits = FloorMarkedRow(py);
		int rowFirst = last;
		int rowLast = first - 1;
		for (int px = first; px < last; ++px) {
			if (writtenRow[px - x] == 0)
				continue;
			row[px] = SwapFloorIndex(row[px]);
			captured[px] = row[px];
			bits[px >> 6] |= uint64_t { 1 } << (px & 63);
			rowFirst = std::min(rowFirst, px);
			rowLast = px;
		}
		marked.Add(rowFirst, py, rowLast + 1, py + 1);
	}
	FloorMarked.Add(marked.left, marked.top, marked.right, marked.bottom);
}

/**
 * Swaps every marked pixel's index with its ramp neighbour (SwappableBytes) and keeps a copy of the frame, so a later
 * draw over a marked pixel is detected even where the light tables collapse different art to the same dark index. The
 * swapped index stays in the frame: blended draws over it see an almost classic colour, the HD sprites' occlusion
 * snapshots see the same values, and the shaders swap it back where they show the classic colour.
 */
void WebGpuCaptureCathedralFloor(uint8_t *pixels, int pitch)
{
	if (!FloorActive || pixels == nullptr)
		return;
	FloorCaptured = true;
	const int left = FloorMarked.left;
	const int right = FloorMarked.right;
	const int bottom = std::min(FloorMarked.bottom, FloorHeight);
	for (int y = FloorMarked.top; y < bottom; ++y) {
		uint8_t *row = pixels + static_cast<size_t>(y) * pitch;
		const uint64_t *bits = FloorMarkedRow(y);
		for (int word = left >> 6; word <= (right - 1) >> 6; ++word) {
			const uint64_t marks = bits[word];
			if (marks == 0)
				continue;
			const int base = word * 64;
			if (base + 64 <= FloorWidth) {
				for (int chunk = 0; chunk < 8; ++chunk) {
					const auto chunkMarks = static_cast<uint8_t>(marks >> (8 * chunk));
					if (chunkMarks == 0)
						continue;
					uint64_t value;
					std::memcpy(&value, row + base + 8 * chunk, sizeof(value));
					value ^= FloorByteMask[chunkMarks] & SwappableBytes(value);
					std::memcpy(row + base + 8 * chunk, &value, sizeof(value));
				}
			} else {
				for (int x = base; x < FloorWidth; ++x) {
					if (((marks >> (x - base)) & 1U) != 0)
						row[x] = SwapFloorIndex(row[x]);
				}
			}
		}
		std::memcpy(FloorCapturedFrame.data() + static_cast<size_t>(y) * FloorWidth + left, row + left, static_cast<size_t>(right - left));
	}
}

void WebGpuFinishCathedralFloor(const uint8_t *pixels, int pitch, const uint8_t *paletteRgba)
{
	if (paletteRgba != nullptr)
		std::memcpy(FloorPalette.data(), paletteRgba, sizeof(FloorPalette));
	if (!FloorActive || !FloorCaptured || pixels == nullptr) {
		ClearFloorShown();
		return;
	}
	BuildFloorShown(pixels, pitch);
	if (!FloorShownAny) {
		// Nothing shows the HD floor (a loading screen over the last world pass): later presents skip the floor until the
		// next world pass marks it again.
		FloorActive = false;
		return;
	}
	if (!FloorTownMarks)
		UpdateCathedralFloorTint(pixels, pitch);
}

} // namespace devilution

#else

namespace devilution {

bool WebGpuInitialize(const char *, int, int) { return false; }
bool WebGpuResize(int, int) { return false; }
bool WebGpuPresent(SDL_Surface *, int, int, const WebGpuLightingData &) { return false; }
void WebGpuShutdown() { }
bool WebGpuIsReady() { return false; }

void WebGpuBeginCathedralFloor(int, int, bool) { }
void WebGpuMarkCathedralFloor(int, int, int, int, int, uint8_t) { }
void WebGpuCaptureCathedralFloor(uint8_t *, int) { }
void WebGpuFinishCathedralFloor(const uint8_t *, int, const uint8_t *) { }
bool WebGpuPrepareTownFloor() { return false; }
void WebGpuMarkTownFloor(int, int, int, int, int, int) { }
bool WebGpuTownSolidFloorPiece(int) { return false; }
void WebGpuCaptureTownSolidFloor(uint8_t *, int, int, int, int, int, int, int, const uint8_t *) { }

} // namespace devilution

#endif
