/**
 * @file webgpu_shaders.hpp
 *
 * WGSL sources for the 2.5D hybrid lighting passes. tools/wgsl_validate.py extracts
 * every R"WGSL(...)WGSL" string from this file, concatenates HybridPreludeWgsl (and the library strings
 * before the source, HdFloorWgsl) in front of each entry-point source exactly as the presenter does, and
 * validates the result with naga / tint.
 *
 * Source/engine/render/hybrid_reference.cpp is a line-parallel C++ transliteration of the three hybrid
 * strings; keep the two in sync function for function. The opt-in HD floor (HdFloorWgsl) is
 * not modelled there.
 *
 * Rules enforced by review: no select() with side-effecting or expensive operands (both
 * operands are always evaluated; use if/else), every shift amount and array index is u32, the DDA skip
 * test is `any(cell != skip)`, neither stage binds a storage buffer, every branch on light culling is
 * workgroup-uniform (uniforms and the workgroup light mask read after a barrier only), and a dynamic vector-component
 * index is only ever applied to a local copy.
 */
#pragma once

namespace devilution {

/** Shared prelude (kinds, plane decoding, height recovery), concatenated in front of both hybrid shaders. */
constexpr char HybridPreludeWgsl[] = R"WGSL(
// ---- hybrid prelude ----
const K_UNLIT: u32 = 0u; const K_FLOOR: u32 = 1u; const K_WALL_L: u32 = 2u; const K_WALL_R: u32 = 3u;
const K_CARD: u32 = 4u; const K_CARD_EMISSIVE: u32 = 5u; const K_WALL_BLENDED: u32 = 6u; const K_WALL_TOP: u32 = 7u;
const CELL_PICK_OFFSET: f32 = 0.35;   // tiles along the wall normal for own/skip cell picks (see hybridLightMain)
const V_FACE_SW: u32 = 256u; const V_FACE_SE: u32 = 512u;
const V_BLOCK: u32 = 1024u; const V_VISIBLE: u32 = 4096u; const V_EXPLORED: u32 = 8192u; const V_ARCH: u32 = 16384u;
const V_DOOR_OPEN: u32 = 32768u;
const MAX_WALL_ROWS: u32 = 224u;   // plane-B wrap guard

fn isWallKind(kind: u32) -> bool { return kind == K_WALL_L || kind == K_WALL_R || kind == K_WALL_BLENDED; }
fn hasHeight(kind: u32) -> bool { return isWallKind(kind) || kind == K_CARD || kind == K_WALL_TOP; }
// Rows above the anchor for a plane texel. B holds unzoomed rows and is never scaled up.
// `zoom` packs the zoom as HdWorldTransform maps it: bits 0..7 screen pixels per world pixel in quarters (4 = none,
// 5 = 1.25x), bits 8..15 minus the transform's offsetY (the first world row gets only the remainder).
fn zoomQuarters(zoom: u32) -> u32 { return max(zoom & 255u, 4u); }
fn zoomScaleOf(zoom: u32) -> f32 { return f32(zoomQuarters(zoom)) * 0.25; }
// The world row shown at screen row y (exactly Zoom()'s mapping).
fn zoomRow(y: u32, zoom: u32) -> u32 { return ((y + (zoom >> 8u)) * 4u) / zoomQuarters(zoom); }
fn heightRows(kind: u32, b: u32, y: u32, zoom: u32) -> u32 {
  if (!hasHeight(kind)) { return 0u; }
  return (b - zoomRow(y, zoom)) & 255u;
}
// Zoom-aware B fetch: the world blitters wrote B at world size starting at column 0; zoomOffsetX is the transform's
// offsetX (side panel and remainder). A negative x after the shift is out of range and reads 0 like WebGPU robust
// access (h wraps -> guard).
fn baseTexelAt(tex: texture_2d<u32>, px: vec2i, zoom: u32, zoomOffsetX: i32) -> u32 {
  if (zoomQuarters(zoom) == 4u) { return textureLoad(tex, px, 0).r; }
  let sx = px.x - zoomOffsetX;
  if (sx < 0) { return 0u; }
  return textureLoad(tex, vec2i((sx * 4) / i32(zoomQuarters(zoom)), i32(zoomRow(u32(max(px.y, 0)), zoom))), 0).r;
}
fn passThrough(kind: u32) -> bool { return kind == K_UNLIT || kind == K_CARD_EMISSIVE; }
)WGSL";

/** Compute pass `hybridLightMain`: ray-marched per-mask-texel lighting. Bindings: uniform + textures only. */
constexpr char HybridLightWgsl[] = R"WGSL(
// ---- hybrid light pass ----
struct HybridLight { positionRadius: vec4f, colorIntensity: vec4f };
struct HybridParams {
  screenOriginViewport: vec4f, screenToWorld: vec4f, counts: vec4u, shading: vec4f, tuning: vec4f, misc: vec4u,
  lights: array<HybridLight, 32>, normalLut: array<vec4f, 32>,
  ambientCurve: array<vec4f, 4>,   // classic darkening per dLight level 0..15 (gbuffer::BuildClassicAmbientCurve)
};
@group(0) @binding(0) var<uniform> params: HybridParams;
@group(0) @binding(1) var volumeTex: texture_2d<u32>;                    // r32uint 112x112 (layout)
@group(0) @binding(2) var outputMask: texture_storage_2d<rgba16float, write>;
@group(0) @binding(3) var auxTex: texture_2d<u32>;
@group(0) @binding(4) var baseTex: texture_2d<u32>;

fn screenToWorld(screen: vec2f) -> vec2f {
  let d = screen - params.screenOriginViewport.xy;
  return vec2f(params.screenToWorld.x * d.x + params.screenToWorld.y * d.y,
               params.screenToWorld.z * d.x + params.screenToWorld.w * d.y);
}
fn cellRaw(c: vec2i) -> u32 {
  let w = i32(params.counts.y); let h = i32(params.counts.z);
  if (c.x < 0 || c.y < 0 || c.x >= w || c.y >= h) { return V_BLOCK | 255u; }   // out of grid: solid, tall
  return textureLoad(volumeTex, c, 0).r;
}
// Hidden-information rule: an unexplored cell exposes only its gameplay BlockLight bit with infinite height and no arch.
fn cellForMarch(c: vec2i) -> u32 {
  let v = cellRaw(c);
  if ((v & V_EXPLORED) == 0u) { return (v & V_BLOCK) | 255u; }
  return v;
}
// Classic light level of one cell as a float (0 bright .. 15 dark); an unexplored cell is fully dark.
fn levelOfCell(c: vec2i) -> f32 {
  let v = cellRaw(c);
  if ((v & V_EXPLORED) == 0u) { return 15.0; }
  return f32((v >> 17u) & 15u);
}
// What the level's light table leaves of a colour at a fractional level (params.ambientCurve, linear between entries).
fn ambientAtLevel(level: f32) -> f32 {
  let l = clamp(level, 0.0, 15.0);
  let i0 = u32(floor(l)); let i1 = min(i0 + 1u, 15u); let t = l - f32(i0);
  return mix(params.ambientCurve[i0 / 4u][i0 % 4u], params.ambientCurve[i1 / 4u][i1 % 4u], t);
}
// Classic ambient for one cell: the curve at its level; 0 when unexplored.
fn ambientOfCell(c: vec2i) -> f32 {
  if ((cellRaw(c) & V_EXPLORED) == 0u) { return 0.0; }
  return ambientAtLevel(levelOfCell(c));
}
// Bilinear over the 4 nearest tile centres, interpolating the light LEVEL and mapping afterwards, as the classic
// per-pixel lightmap does. Interpolating mapped brightness instead spread a lit cell's value halfway into its dark
// neighbours as diamond-shaped lobes.
fn levelBilinear(ground: vec2f) -> f32 {
  let q = ground - vec2f(0.5); let c0 = vec2i(floor(q)); let f = q - vec2f(c0);
  let l00 = levelOfCell(c0); let l10 = levelOfCell(c0 + vec2i(1, 0));
  let l01 = levelOfCell(c0 + vec2i(0, 1)); let l11 = levelOfCell(c0 + vec2i(1, 1));
  return mix(mix(l00, l10, f.x), mix(l01, l11, f.x), f.y);
}
fn ambientBilinear(ground: vec2f) -> f32 { return ambientAtLevel(levelBilinear(ground)); }
// Direct light reaches what classic lights: 1 up to level 14, fading to 0 at 15 along the interpolated level, so the
// cut follows the lightmap's bleed instead of stopping dead at every fully dark cell's edge.
fn lightGate(ground: vec2f) -> f32 { return clamp(15.0 - levelBilinear(ground), 0.0, 1.0); }
// Surfaces: bilinear like cards, zero only for an unexplored own cell. A fully dark but explored cell keeps the bilinear
// value, which is 0 at its centre and rises toward lit neighbours: the classic per-pixel lightmap bleeds the same way.
fn ambientBilinearClamped(ground: vec2f, own: vec2i) -> f32 {
  if ((cellRaw(own) & V_EXPLORED) == 0u) { return 0.0; }
  return ambientBilinear(ground);
}
// Gate cell of a wall face: its own cell (the wall cell its centre line runs through), or the cell in front of that line when
// the own cell carries no Explored bit. Classic marks the wall's own cell explored only from some approaches, and a visible face
// never reveals anything about the room behind it, so gating an explored face's pixels to black there only produced cell-shaped
// panels. The front pick reaches half a tile further than the own pick: the wall line is the cell's centre line.
fn wallGateCell(ground: vec2f, n: vec2f) -> vec2i {
  let own = vec2i(floor(ground - n * CELL_PICK_OFFSET));
  if ((cellRaw(own) & V_EXPLORED) != 0u) { return own; }
  return vec2i(floor(ground + n * (CELL_PICK_OFFSET + 0.5)));
}
// Own cell of a foot-less wall receiver (WallTop): u + v == 1.9375 on the base row.
fn wallOwnCell(ground: vec2f) -> vec2i {
  var c = vec2i(floor(ground)); let f = ground - vec2f(c);
  let s = i32(round(ground.x + ground.y - 1.9375));
  if (c.x + c.y > s) { if (f.x < f.y) { c.x -= 1; } else { c.y -= 1; } }
  return c;
}
// t range in which a + t * da > k (the ray is in front of a face plane at coordinate k).
fn frontSpan(a: f32, da: f32, k: f32) -> vec2f {
  if (abs(da) <= 1e-6) {
    if (a > k) { return vec2f(-1e9, 1e9); }
    return vec2f(1e9, -1e9);
  }
  let t = (k - a) / da;
  if (da > 0.0) { return vec2f(t, 1e9); }
  return vec2f(-1e9, t);
}
// True when the ray's z is below h somewhere on [ta, tb] (z is linear in t, so the ends bound it).
fn belowOn(p: vec3f, d: vec3f, ta: f32, tb: f32, h: f32) -> bool { return min(p.z + ta * d.z, p.z + tb * d.z) < h; }
// BlockLight test of one explored cell over [tIn, tOut]. A wall half stands on the cell's centre line (tile_geometry faces:
// FaceSE = plane x = cell.x + 0.5 facing +x, FaceSW = plane y = cell.y + 0.5 facing +y, both = inner corner), and the cell in
// front of that plane is the floor the art draws there, so only the part of the span behind the face planes occludes.
// Blocking the whole cell shadowed the floor half of every wall cell and cut cell-sized dark panels out of the faces along a
// wall wherever a ray grazed a neighbouring wall cell. A cell without faces occludes as a whole.
fn blockedInCell(p: vec3f, d: vec3f, cell: vec2i, v: u32, tIn: f32, tOut: f32, h: f32) -> bool {
  var lo = tIn; var hi = tOut;                                  // [lo, hi]: the open part in front of every face plane
  if ((v & V_FACE_SE) != 0u) { let r = frontSpan(p.x, d.x, f32(cell.x) + 0.5); lo = max(lo, r.x); hi = min(hi, r.y); }
  if ((v & V_FACE_SW) != 0u) { let r = frontSpan(p.y, d.y, f32(cell.y) + 0.5); lo = max(lo, r.x); hi = min(hi, r.y); }
  if ((v & (V_FACE_SW | V_FACE_SE)) == 0u || lo >= hi) { return belowOn(p, d, tIn, tOut, h); }
  return (lo > tIn && belowOn(p, d, tIn, lo, h)) || (hi < tOut && belowOn(p, d, hi, tOut, h));
}
// Visibility of light l from receiver p (tile space; z in world height units). 3D DDA over the 2D grid:
// an explored cell blocks when BlockLight is set and the ray's z is below the cell's height inside it, behind its face planes
// (blockedInCell); an explored Arch cell blocks when the ray is below its height AND rises above its lintel (independent of
// BlockLight); unexplored cells block by BlockLight with infinite height and no faces. Receiver and light cells are skipped (any(): sharing one coordinate is not enough).
fn visibility3d(p: vec3f, l: vec3f, skipA: vec2i, skipB: vec2i) -> f32 {
  let d = l - p;
  if (dot(d.xy, d.xy) < 1e-6) { return 1.0; }
  var cell = vec2i(floor(p.xy));
  let endCell = vec2i(floor(l.xy));
  let step = vec2i(select(-1, 1, d.x > 0.0), select(-1, 1, d.y > 0.0));
  var invD = vec2f(1e9, 1e9);
  if (abs(d.x) > 1e-6) { invD.x = 1.0 / abs(d.x); }
  if (abs(d.y) > 1e-6) { invD.y = 1.0 / abs(d.y); }
  var tMax = vec2f((p.x - f32(cell.x)) * invD.x, (p.y - f32(cell.y)) * invD.y);
  if (d.x > 0.0) { tMax.x = (f32(cell.x + 1) - p.x) * invD.x; }
  if (d.y > 0.0) { tMax.y = (f32(cell.y + 1) - p.y) * invD.y; }
  let hScale = params.shading.y;
  var tIn = 0.0;
  for (var i = 0u; i < 64u; i++) {
    let tOut = min(min(tMax.x, tMax.y), 1.0);
    if (any(cell != skipA) && any(cell != skipB)) {
      let v = cellForMarch(cell);
      let h = f32(v & 255u) / hScale;
      let zIn = p.z + tIn * d.z; let zOut = p.z + tOut * d.z;
      let zMin = min(zIn, zOut); let zMax = max(zIn, zOut);
      if ((v & V_ARCH) != 0u) {
        let lintel = f32((v >> 21u) & 127u) * 2.0 / hScale;
        if (zMin < h && zMax > lintel) { return 0.0; }
      } else if ((v & V_BLOCK) != 0u && blockedInCell(p, d, cell, v, tIn, tOut, h)) { return 0.0; }
    }
    if (all(cell == endCell) || tOut >= 1.0) { break; }
    tIn = tOut;
    if (tMax.x < tMax.y) { cell.x += step.x; tMax.x += invD.x; } else { cell.y += step.y; tMax.y += invD.y; }
  }
  return 1.0;
}
fn softVisibility3d(p: vec3f, l: vec3f, skipA: vec2i, skipB: vec2i) -> f32 {
  let d = l.xy - p.xy;
  if (dot(d, d) < 1e-4) { return visibility3d(p, l, skipA, skipB); }
  let t = normalize(vec2f(-d.y, d.x)) * params.tuning.z;
  let v = visibility3d(p, l, skipA, skipB)
        + visibility3d(p, vec3f(l.xy + t, l.z), skipA, skipB)
        + visibility3d(p, vec3f(l.xy - t, l.z), skipA, skipB);
  return v / 3.0;
}
// Per-workgroup light culling: the real world footprint of this 8x8 block's shaded receivers, reduced in workgroup
// memory, and the lights touching it as a bitmask built once per workgroup. WGSL zero-initialises workgroup memory, so a key of 0
// means "no receiver yet". Every texel reads the same mask after a barrier, so every branch on it is workgroup-uniform.
var<workgroup> wgGroundMax: array<atomic<u32>, 2>;              // orderedKey of the largest ground.x / ground.y
var<workgroup> wgGroundMinInv: array<atomic<u32>, 2>;           // ~orderedKey of the smallest ground.x / ground.y
var<workgroup> wgLightMask: atomic<u32>;                        // bit i: light i reaches the footprint
)WGSL" // Split: MSVC caps one string literal piece at 16 KB (C2026); adjacent pieces concatenate.
R"WGSL(
// f32 -> u32 key with the same order (and never 0 for a finite value), so atomicMax finds the float extremes exactly.
fn orderedKey(f: f32) -> u32 {
  let b = bitcast<u32>(f);
  if ((b & 0x80000000u) != 0u) { return ~b; }
  return b | 0x80000000u;
}
fn keyToF32(k: u32) -> f32 {
  if ((k & 0x80000000u) != 0u) { return bitcast<f32>(k & 0x7fffffffu); }
  return bitcast<f32>(~k);
}
struct Footprint { lo: vec2f, hi: vec2f };
// The receivers' ground grown by the shading nudge: p.xy = ground + n.xy * tuning.y with |n.x|, |n.y| <= 1. A light whose radius
// misses this box misses every receiver of the block (dist >= L.w in accumulateMasked), so culling it changes nothing.
fn workgroupFootprint() -> Footprint {
  let lo = vec2f(keyToF32(~atomicLoad(&wgGroundMinInv[0])), keyToF32(~atomicLoad(&wgGroundMinInv[1])));
  let hi = vec2f(keyToF32(atomicLoad(&wgGroundMax[0])), keyToF32(atomicLoad(&wgGroundMax[1])));
  return Footprint(lo - vec2f(params.tuning.y), hi + vec2f(params.tuning.y));
}
fn lightTouches(fp: Footprint, L: vec4f) -> bool {
  let q = clamp(L.xy, fp.lo, fp.hi) - L.xy;
  return dot(q, q) < L.w * L.w;
}
// Shared light loop over the workgroup's light mask (ascending index, the order of the full list).
// own: cell for ambient clamp + gate (surfaces); gated = false -> free bilinear ambient, no gate (cards).
fn accumulateMasked(p: vec3f, n: vec3f, ground: vec2f, own: vec2i, skipA: vec2i, wrap: f32, gated: bool, lightMask: u32) -> vec3f {
  let flags = params.counts.w;
  var ambient: f32;
  if (gated) { ambient = ambientBilinearClamped(ground, own); } else { ambient = ambientBilinear(ground); }
  ambient = ambient * params.shading.x;
  var gate = 1.0;
  if (gated) {                                                  // never light what classic never lit
    if ((cellRaw(own) & V_EXPLORED) == 0u) { gate = 0.0; } else { gate = lightGate(ground); }
  }
  var radiance = vec3f(0.0); var peak = 0.0;
  var pending = lightMask;                                      // workgroup-uniform
  if (gate <= 0.0) { pending = 0u; }                            // radiance * gate == 0: skip every light and its marches
  while (pending != 0u) {
    let i = countTrailingZeros(pending); pending = pending & (pending - 1u);
    let L = params.lights[i].positionRadius; let ci = params.lights[i].colorIntensity;
    let toL = L.xyz - p; let dist = length(toL);
    if (dist >= L.w) { continue; }
    let ldir = toL / max(dist, 1e-4);
    let ndl = clamp((dot(n, ldir) + wrap) / (1.0 + wrap), 0.0, 1.0);
    if (ndl <= 0.0) { continue; }
    let lightCell = vec2i(floor(L.xy));
    var vis: f32;
    if ((flags & 16u) != 0u) { vis = softVisibility3d(p, L.xyz, skipA, lightCell); }
    else { vis = visibility3d(p, L.xyz, skipA, lightCell); }
    let att = 1.0 - dist / L.w;
    let contribution = att * att * vis * ndl * ci.w;
    radiance += ci.rgb * contribution; peak = max(peak, contribution);
  }
  var lighting = vec3f(ambient) + radiance * gate;
  if ((flags & 2u) != 0u) { lighting += radiance * gate * (0.12 * smoothstep(0.45, 1.0, peak)); }   // bloom: additive only
  return lighting;
}
// Footprint form for callers without a workgroup (the HD sprite fragment shader, hd_sprites_webgpu.cpp).
fn footprintLightMask(fp: Footprint) -> u32 {
  var mask = 0u;
  for (var i = 0u; i < min(params.counts.x, 32u); i++) {
    if (lightTouches(fp, params.lights[i].positionRadius)) { mask = mask | (1u << i); }
  }
  return mask;
}
fn accumulate(p: vec3f, n: vec3f, ground: vec2f, own: vec2i, skipA: vec2i, wrap: f32, gated: bool, fp: Footprint) -> vec3f {
  return accumulateMasked(p, n, ground, own, skipA, wrap, gated, footprintLightMask(fp));
}
fn debugVolumeColor(cell: vec2i) -> vec3f {                     // Volume view, computed here so the fragment stage needs no volume binding
  let v = cellRaw(cell);
  if ((v & V_EXPLORED) == 0u) { return vec3f(1.0, 0.0, 1.0); }  // magenta = leak detector
  let hgt = f32(v & 255u) / 255.0;
  if ((v & V_ARCH) != 0u) { return vec3f(0.2, 0.6, 1.0) * (0.3 + hgt); }
  if ((v & V_DOOR_OPEN) != 0u) { return vec3f(0.2, 1.0, 0.4) * (0.3 + hgt); }
  if ((v & V_BLOCK) != 0u) { return vec3f(1.0, 0.35, 0.35) * (0.3 + hgt); } // existing red occluder tint (webgpu_presenter.cpp:137-140)
  return vec3f(0.15 + 0.5 * f32(15u - ((v >> 17u) & 15u)) / 15.0);
}

// One mask texel before lighting: `store` is its final value unless `shade` (pass-through, debug view 5).
struct Receiver { shade: bool, store: vec4f, kind: u32, nIdx: u32, ground: vec2f, hf: f32 };
fn receiverOf(id: vec2u) -> Receiver {
  var r = Receiver(false, vec4f(1.0, 1.0, 1.0, 0.0), K_UNLIT, 0u, vec2f(0.0), 0.0);
  let flags = params.counts.w;
  let maskScale = params.misc.x; let zoom = params.misc.y; let zoomOffsetX = i32(params.misc.z);
  let debugView = (flags >> 8u) & 15u;
  let outPixel = vec2i(id * maskScale + (maskScale / 2u));      // texel centre in output pixels
  let viewportH = i32(params.screenOriginViewport.z);
  if ((flags & 8u) == 0u || outPixel.y >= viewportH) { return r; }
  let a = textureLoad(auxTex, outPixel, 0).r;                   // odd-W last column: robust access returns 0 (Unlit) past the edge
  var kind = a >> 5u; let nIdx = a & 31u;
  let b = baseTexelAt(baseTex, outPixel, zoom, zoomOffsetX);
  var h = heightRows(kind, b, u32(outPixel.y), zoom);
  if (isWallKind(kind) && h > MAX_WALL_ROWS) { kind = K_FLOOR; h = 0u; }   // plane-B wrap guard
  r.kind = kind; r.nIdx = nIdx; r.hf = f32(h);
  let floorScreen = vec2f(f32(outPixel.x), f32(outPixel.y) + r.hf * zoomScaleOf(zoom));
  r.ground = screenToWorld(floorScreen);
  if (debugView == 5u) { r.store = vec4f(debugVolumeColor(vec2i(floor(r.ground))), 1.0); return r; }
  if (passThrough(kind)) { return r; }
  r.shade = true;
  return r;
}
fn shadeReceiver(r: Receiver, lightMask: u32) -> vec4f {
  let kind = r.kind; let ground = r.ground;
  let n = params.normalLut[r.nIdx].xyz;
  let z = r.hf / params.shading.y;
  let nudge = n.xy * params.tuning.y;
  // Cell picks use a wider offset than the shading nudge. A wall's foot line runs through its cell's centre but the
  // ground is quantised to rows and lands up to 1/32 tile either side of it, so 0.35 tiles along the normal keeps the march's
  // skip cell (and the gate's own pick) on the wall's own cell for every column.
  let cellNudge = n.xy * CELL_PICK_OFFSET;
  var lit: vec3f;
  // Foot-table walls, see-through walls and arches. A see-through (WallBlended) pixel is lit exactly like the opaque
  // face it continues, as classic's per-pixel lightmap does; lighting it from the floor under the pixel only hid the
  // swapped wall facings and left every blended piece a different value from its opaque neighbours.
  if (isWallKind(kind)) {
    let skipA = vec2i(floor(ground + cellNudge)); let own = wallGateCell(ground, n.xy);
    lit = accumulateMasked(vec3f(ground + nudge, z), n, ground, own, skipA, params.tuning.x, true, lightMask);
  } else if (kind == K_WALL_TOP) {
    let own = wallOwnCell(ground);
    lit = accumulateMasked(vec3f(ground, z), n, ground, own, own, params.tuning.x, true, lightMask);
  } else if (kind == K_CARD) {                                  // no gate, no clamp: CPU culling already enforces no-reveal
    let c = vec2i(floor(ground));
    lit = accumulateMasked(vec3f(ground, z), n, ground, c, c, params.shading.w, false, lightMask);
  } else {                                                      // K_FLOOR
    let c = vec2i(floor(ground));
    lit = accumulateMasked(vec3f(ground, 0.0), n, ground, c, c, params.tuning.x, true, lightMask);
  }
  return vec4f(lit, 1.0);                                       // debugView 4 (Lighting) reads this mask unchanged in the composite
}

@compute @workgroup_size(8, 8)
fn hybridLightMain(@builtin(global_invocation_id) id: vec3u, @builtin(local_invocation_index) li: u32) {
  let dims = textureDimensions(outputMask);
  let inside = id.x < dims.x && id.y < dims.y;                  // dispatched over the FULL mask (rows >= viewport store pass-through)
  var r = Receiver(false, vec4f(1.0, 1.0, 1.0, 0.0), K_UNLIT, 0u, vec2f(0.0), 0.0);
  if (inside) { r = receiverOf(id.xy); }
  // Footprint reduction and light mask: no early return before the barriers (they must stay in uniform control flow).
  if (r.shade) {
    let kx = orderedKey(r.ground.x); let ky = orderedKey(r.ground.y);
    atomicMax(&wgGroundMax[0], kx); atomicMax(&wgGroundMax[1], ky);
    atomicMax(&wgGroundMinInv[0], ~kx); atomicMax(&wgGroundMinInv[1], ~ky);
  }
  workgroupBarrier();
  if (li < min(params.counts.x, 32u) && atomicLoad(&wgGroundMax[0]) != 0u) {   // one light per invocation; 0: nothing to shade
    if (lightTouches(workgroupFootprint(), params.lights[li].positionRadius)) { atomicOr(&wgLightMask, 1u << li); }
  }
  workgroupBarrier();
  let lightMask = atomicLoad(&wgLightMask);
  if (!inside) { return; }
  if (r.shade) { r.store = shadeReceiver(r, lightMask); }
  textureStore(outputMask, vec2i(id.xy), r.store);
}
)WGSL";

/**
 * HD floor lookup (?hdFloor=1), shared by the hybrid present pass (concatenated in front of HybridPresentWgsl) and the
 * legacy composite (webgpu_presenter.cpp). The CPU uploads one bit per screen pixel (the floor pixels that show the HD
 * floor: marked by a replaced diamond and not drawn over later) and one texel per dungeon tile (cell and light); the
 * diamond of a pixel and its place in the HD cell follow from the screen position, because the floor diamonds lie on
 * the regular iso grid (webgpu_presenter.cpp FloorTileOf). Floor decals (blood, bones, torch shadows) keep the classic
 * pixel: a compute pass marks the 4x4 world blocks where a decal family is dense (floorFamilies), and floorKeep finds
 * the decal pixels in them from the classic index.
 */
constexpr char HdFloorWgsl[] = R"WGSL(
// ---- HD floor (webgpu_presenter.cpp, ?hdFloor=1) ----
struct FloorParams {
  lattice: vec4i,   // xy: world-pass pixel of dungeon tile (0, 0)'s bottom-left corner; z: zoom quarters; w: FLOOR_* flags
  view: vec4i,      // xy: screen position of world pixel 0 (HdWorldTransform offset); zw: world-pass size
  grid: vec4i,      // xy: 4x4 block grid (ceil(screen / 4)); zw: screen size
  misc: vec4i,      // x: viewport rows (the legacy composite expands them from the index plane), yzw: 0
};
@group(0) @binding(16) var floorMask: texture_2d<u32>;           // r32uint, ceil(W / 32) x H: bit = this screen pixel shows the HD floor
@group(0) @binding(17) var floorAtlas: texture_2d<f32>;          // 4x Cathedral hash-variant atlas (4x4 cells)
@group(0) @binding(18) var townFloorAtlas: texture_2d_array<f32>; // 4x town atlas pages (256x128 cells, 16 per row)
@group(0) @binding(19) var floorTiles: texture_2d<u32>;          // r32uint 112x112 per dungeon tile: bits 0..15 cell, 16..20 light level + 1
@group(0) @binding(20) var floorFamilies: texture_2d<u32>;       // r32uint per 4x4 world block: decal families kept there (bit 1 blood, 2 bone, 3 dark)
@group(0) @binding(21) var<uniform> floorParams: FloorParams;
@group(0) @binding(22) var floorSampler: sampler;

const FLOOR_ACTIVE: i32 = 1; const FLOOR_TOWN: i32 = 2; const FLOOR_DECALS: i32 = 4; const FLOOR_EXPAND: i32 = 8;

fn floorDivI(a: i32, b: i32) -> i32 {
  let q = a / b;
  if ((a % b) != 0 && ((a < 0) != (b < 0))) { return q - 1; }
  return q;
}
// World-pass pixel shown at screen pixel px (HdWorldTransform::WorldX / WorldY).
fn floorWorldPixel(px: vec2i) -> vec2i {
  let q = floorParams.lattice.z;
  return vec2i(floorDivI((px.x - floorParams.view.x) * 4, q), floorDivI((px.y - floorParams.view.y) * 4, q));
}
// First screen pixel of world pixel w (HdWorldTransform::ToScreenX / ToScreenY).
fn floorScreenPixel(w: vec2i) -> vec2i {
  let q = floorParams.lattice.z;
  return vec2i(floorParams.view.x - floorDivI(-w.x * q, 4), floorParams.view.y - floorDivI(-w.y * q, 4));
}
fn floorBit(px: vec2i) -> bool {
  if (px.x < 0 || px.y < 0 || px.x >= floorParams.grid.z || px.y >= floorParams.grid.w) { return false; }
  let word = textureLoad(floorMask, vec2i(px.x >> 5u, px.y), 0).r;
  return ((word >> (u32(px.x) & 31u)) & 1u) != 0u;
}
// The dungeon tile whose diamond holds world pixel w (the diamonds tile the plane exactly; webgpu_presenter.cpp FloorTileOf).
fn floorTileOf(w: vec2i) -> vec2i {
  let x = 2 * (w.x - floorParams.lattice.x) + 1 - 64;
  let y = 2 * (w.y - floorParams.lattice.y) + 1 + 32;
  return vec2i(floorDivI(x + 2 * y + 64, 128), floorDivI(2 * y - x + 64, 128));
}
fn floorTileInfo(t: vec2i) -> u32 {
  if (t.x < 0 || t.y < 0 || t.x >= 112 || t.y >= 112) { return 0u; }
  return textureLoad(floorTiles, t, 0).r;
}
// The classic index of a pixel that shows the HD floor: the capture swapped every index >= 2 with its ramp neighbour.
fn floorClassicIndex(idx: u32) -> u32 {
  if (idx >= 2u) { return idx ^ 1u; }
  return idx;
}
// webgpu_presenter.cpp FloorFamilyOf as a bit (blood 2, bone 4, dark 8, plain 0).
fn floorFamilyBit(idx: u32) -> u32 {
  if ((idx >= 48u && idx < 64u) || (idx >= 80u && idx < 96u)) { return 2u; }
  if (idx >= 1u && idx < 32u) { return 4u; }
  if (idx == 0u || (idx < 128u && (idx & 15u) >= 14u)) { return 8u; }
  return 0u;
}
// A classic decal pixel: an HD floor pixel of a family that is dense around it (floorFamilies).
fn floorDecalAt(w: vec2i, indexTex: texture_2d<u32>) -> bool {
  if (w.x < 0 || w.y < 0 || w.x >= floorParams.view.z || w.y >= floorParams.view.w) { return false; }
  let families = textureLoad(floorFamilies, vec2i(w.x >> 2u, w.y >> 2u), 0).r;
  if (families == 0u) { return false; }
  let s = floorScreenPixel(w);
  if (!floorBit(s)) { return false; }
  return (floorFamilyBit(floorClassicIndex(textureLoad(indexTex, s, 0).r)) & families) != 0u;
}
// How much of the classic pixel to keep: 1 on a decal pixel, 10/127 per decal neighbour (a soft edge).
fn floorKeep(w: vec2i, indexTex: texture_2d<u32>) -> f32 {
  if ((floorParams.lattice.w & FLOOR_DECALS) == 0) { return 0.0; }
  if (floorDecalAt(w, indexTex)) { return 1.0; }
  var neighbours = 0;
  for (var dy = -1; dy <= 1; dy++) {
    for (var dx = -1; dx <= 1; dx++) {
      if ((dx != 0 || dy != 0) && floorDecalAt(w + vec2i(dx, dy), indexTex)) { neighbours++; }
    }
  }
  return f32(neighbours * 10) / 127.0;
}
struct FloorSample { found: bool, rgb: vec3f, weight: f32, light: u32 };
// The HD floor albedo (sRGB) at screen pixel px and how much of it replaces the classic pixel; light is the classic light
// level + 1 the diamond was drawn at. textureSampleLevel (explicit LOD) is valid in this non-uniform code.
fn floorSampleAt(screen: vec2f, px: vec2i, indexTex: texture_2d<u32>) -> FloorSample {
  var r = FloorSample(false, vec3f(0.0), 0.0, 0u);
  if ((floorParams.lattice.w & FLOOR_ACTIVE) == 0 || !floorBit(px)) { return r; }
  let w = floorWorldPixel(px);
  let t = floorTileOf(w);
  let info = floorTileInfo(t);
  let light = (info >> 16u) & 31u;
  if (light == 0u) { return r; }
  // The pixel's place in its diamond: corners to cell corners (the diamond's bottom-left is lattice + (32, 16) per tile).
  let scale = f32(floorParams.lattice.z) * 0.25;
  let worldPos = (screen - vec2f(floorParams.view.xy)) / scale;
  let centre = vec2f(f32(floorParams.lattice.x + 32 * (t.x - t.y) + 32), f32(floorParams.lattice.y + 16 * (t.x + t.y) - 16));
  let delta = (worldPos - centre) / vec2f(32.0, 16.0);
  let tileUV = clamp(vec2f((delta.x + delta.y + 1.0) * 0.5, (-delta.x + delta.y + 1.0) * 0.5), vec2f(0.002), vec2f(0.998));
  let code = info & 0xFFFFu;
  var texel: vec4f;
  if (code >= 32768u) {                                         // town: bit 15, bits 9..14 page, bits 0..8 cell
    let cell = code & 511u;
    let townUV = (vec2f(f32(cell % 16u), f32(cell / 16u)) + tileUV) * vec2f(256.0, 128.0) / vec2f(textureDimensions(townFloorAtlas));
    texel = textureSampleLevel(townFloorAtlas, floorSampler, townUV, (code >> 9u) & 63u, 0.0);
  } else {
    let atlasUV = (vec2f(f32(code % 4u), f32(code / 4u)) + tileUV) / 4.0;
    texel = textureSampleLevel(floorAtlas, floorSampler, atlasUV, 0.0);
  }
  r.found = true;
  r.rgb = texel.rgb;
  r.weight = texel.a * (1.0 - floorKeep(w, indexTex));            // atlas alpha 0 (water, banks) and decals keep classic
  r.light = light;
  return r;
}
// The palette index to show at px: the classic one where the capture left its swapped index (HD floor pixels).
fn floorFrameIndex(px: vec2i, idx: u32) -> u32 {
  if ((floorParams.lattice.w & FLOOR_ACTIVE) != 0 && floorBit(px)) { return floorClassicIndex(idx); }
  return idx;
}
)WGSL";

/** Present pass `fragmentMain`: composite the full-bright frame with the light mask. No storage buffers. */
constexpr char HybridPresentWgsl[] = R"WGSL(
// ---- hybrid present pass ----
struct PresentParams { config: vec4u, values: vec4f, config2: vec4u, paletteEmissive: array<vec4f, 64>, normalLut: array<vec4f, 32> };
@group(0) @binding(0) var frameTexture: texture_2d<f32>;
@group(0) @binding(1) var frameSampler: sampler;
@group(0) @binding(2) var lightMask: texture_2d<f32>;        // legacy 2D mask (rgba8unorm, half res)
@group(0) @binding(3) var hybridMask: texture_2d<f32>;       // rgba16float
@group(0) @binding(4) var auxTex: texture_2d<u32>;
@group(0) @binding(5) var baseTex: texture_2d<u32>;
@group(0) @binding(6) var indexTex: texture_2d<u32>;
@group(0) @binding(7) var<uniform> present: PresentParams;
@group(0) @binding(8) var paletteTex: texture_2d<f32>;       // rgba8unorm 256x1: the SDL palette
// Bindings 16-22: the HD floor (HdFloorWgsl, concatenated in front of this source).

// VertexOutput / vertexMain: verbatim copy of the legacy ShaderSource (webgpu_presenter.cpp:27-41).
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

// Per-component sRGB <-> linear with if/else (a select() would evaluate the pow on every component regardless).
fn srgbToLinear1(c: f32) -> f32 {
  if (c > 0.04045) { return pow((c + 0.055) / 1.055, 2.4); }
  return c / 12.92;
}
fn linearToSrgb1(c: f32) -> f32 {
  if (c > 0.0031308) { return 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
  return c * 12.92;
}
fn srgbToLinear(c: vec3f) -> vec3f { return vec3f(srgbToLinear1(c.x), srgbToLinear1(c.y), srgbToLinear1(c.z)); }
fn linearToSrgb(c: vec3f) -> vec3f { return vec3f(linearToSrgb1(c.x), linearToSrgb1(c.y), linearToSrgb1(c.z)); }
fn paletteEmissiveAt(idx: u32) -> f32 { var v = present.paletteEmissive[idx >> 2u]; return v[idx & 3u]; }   // local copy: compiler-safe dynamic component index

// Bilateral upsample of the half-res mask: 4 taps weighted bilinearly, kept only when the tap's centre sample has the same
// kind, a height within 8 rows and lies inside the viewport; nearest (m0) when every weight is zero.
fn sampleLighting(px: vec2i, kind: u32, h: u32) -> vec3f {
  let s = i32(present.config.w);
  if (s == 1) { return textureLoad(hybridMask, px, 0).rgb; }
  let mf = (vec2f(px) - vec2f(1.0)) / 2.0; let m0 = vec2i(floor(mf)); let f = mf - vec2f(m0);
  let zoom = present.config.y; let zoomOffsetX = i32(present.config2.x);
  let viewportH = i32(present.values.x);
  var sum = vec3f(0.0); var wsum = 0.0;
  for (var j = 0; j < 2; j++) { for (var i = 0; i < 2; i++) {
    let m = m0 + vec2i(i, j);
    let sp = m * 2 + 1;                                         // negative sp -> textureLoad returns 0 (robust access): kind Unlit -> rejected
    if (sp.y >= viewportH) { continue; }                        // rows below the viewport hold pass-through, never lighting
    let ta = textureLoad(auxTex, sp, 0).r;
    let tb = baseTexelAt(baseTex, sp, zoom, zoomOffsetX);
    let th = heightRows(ta >> 5u, tb, u32(max(sp.y, 0)), zoom);
    var wx = 1.0 - f.x; if (i == 1) { wx = f.x; }
    var wy = 1.0 - f.y; if (j == 1) { wy = f.y; }
    let w = wx * wy;
    let ok = ((ta >> 5u) == kind) && (abs(i32(th) - i32(h)) < 8);
    if (ok) { sum += textureLoad(hybridMask, m, 0).rgb * w; wsum += w; }
  } }
  if (wsum <= 0.0) { return textureLoad(hybridMask, m0, 0).rgb; }
  return sum / wsum;
}
// Viewport rows: GPU palette expansion (byte-identical to BltFast); panel rows / gamepad: the RGBA frame texel sampled by the caller
// (sampled once in uniform control flow: textureSample must not sit under a per-pixel branch).
fn frameColor(px: vec2i, frameSample: vec4f, idx: u32) -> vec4f {
  if (px.y >= i32(present.values.x) || present.config2.y != 0u) { return frameSample; }
  return textureLoad(paletteTex, vec2i(i32(idx), 0), 0);
}
// HD floor albedo (sRGB) of a pixel that shows the HD floor (HdFloorWgsl), else the frame colour. The atlases hold neutral
// albedo (the Cathedral atlas is tinted toward the level palette on the CPU) and the frame is full-bright here, so the
// cell is used as is; the hybrid lighting then applies to the result.
fn hdFloorAlbedo(screen: vec2f, px: vec2i, frame: vec4f) -> vec4f {
  let hd = floorSampleAt(screen, px, indexTex);
  if (!hd.found) { return frame; }
  return vec4f(mix(frame.rgb, hd.rgb, hd.weight), frame.a);   // weight 0 (atlas alpha, decals) = keep classic
}
fn debugColor(view: u32, px: vec2i, a: u32, idx: u32, frame: vec4f) -> vec4f {
  let kind = a >> 5u; let nIdx = a & 31u;
  if (view == 1u) {                                             // Kind: 8-colour palette
    var kindColors = array<vec3f, 8>(
      vec3f(0.0, 0.0, 0.0), vec3f(0.25, 0.25, 0.25), vec3f(1.0, 0.0, 0.0), vec3f(0.0, 0.0, 1.0),
      vec3f(0.0, 1.0, 0.0), vec3f(1.0, 1.0, 0.0), vec3f(1.0, 0.0, 1.0), vec3f(0.0, 1.0, 1.0));
    return vec4f(kindColors[kind & 7u], 1.0);
  }
  if (view == 2u) {                                             // Height: grey h / 255
    let b = baseTexelAt(baseTex, px, present.config.y, i32(present.config2.x));
    let h = heightRows(kind, b, u32(px.y), present.config.y);
    return vec4f(vec3f(f32(h) / 255.0), 1.0);
  }
  if (view == 3u) { return vec4f(present.normalLut[nIdx].xyz * 0.5 + vec3f(0.5), 1.0); }   // Normal
  if (view == 4u || view == 5u) {                               // Lighting / Volume: the mask alone
    let m = px / i32(present.config.w);
    return vec4f(textureLoad(hybridMask, m, 0).rgb, 1.0);
  }
  if (view == 6u) { return vec4f(vec3f(paletteEmissiveAt(idx)), 1.0); }   // Emissive
  return frame;
}

@fragment fn fragmentMain(input: VertexOutput) -> @location(0) vec4f {
  if (present.config.x == 0u) {                                 // legacy path unchanged
    let frame = textureSample(frameTexture, frameSampler, input.uv);
    let m = textureSample(lightMask, frameSampler, input.uv).rgb; return vec4f(frame.rgb * m, frame.a);
  }
  let frameSample = textureSample(frameTexture, frameSampler, input.uv);   // uniform control flow (config.x is uniform)
  let screen = input.uv * vec2f(textureDimensions(frameTexture));
  let px = vec2i(screen);
  let a = textureLoad(auxTex, px, 0).r; let kind = a >> 5u;
  let idx = floorFrameIndex(px, textureLoad(indexTex, px, 0).r);   // HD floor pixels hold a swapped index (HdFloorWgsl)
  let frame = frameColor(px, frameSample, idx);
  if (present.config.z != 0u) { return debugColor(present.config.z, px, a, idx, frame); }
  if (passThrough(kind) || px.y >= i32(present.values.x)) { return frame; }
  let b = baseTexelAt(baseTex, px, present.config.y, i32(present.config2.x));
  var h = heightRows(kind, b, u32(px.y), present.config.y);
  var k = kind;
  if (isWallKind(k) && h > MAX_WALL_ROWS) { k = K_FLOOR; h = 0u; }
  let lighting = sampleLighting(px, k, h);
  let emissive = paletteEmissiveAt(idx);
  var surface = frame;
  if (kind == K_FLOOR) { surface = hdFloorAlbedo(screen, px, frame); }   // floors are flat: the G-buffer normal stays up
  let albedo = srgbToLinear(surface.rgb);
  var color = albedo * lighting + albedo * emissive * present.values.y;
  color = color * present.values.z;
  return vec4f(linearToSrgb(clamp(color, vec3f(0.0), vec3f(1.0))), frame.a);
}
)WGSL";

} // namespace devilution
