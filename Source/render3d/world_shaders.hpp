#pragma once
/**
 * @file render3d/world_shaders.hpp
 *
 * WGSL for the first-person 3D world pass (world_gpu.cpp).
 *
 * kWorldCommonWgsl: the frame uniforms, the occluder grid and the lighting (worldShade): every light (up to 32) adds
 *   albedo * colour * falloff * Lambert, with a shadow ray traced through the 112 x 112 occluder grid (2D DDA over the
 *   cells, each cell blocking the height band [bottom, top]); light reaches black at its radius like the original.
 *   The skinned-mesh renderer can prepend this string to its own shaders to be lit identically (bindings 0 and 1).
 * kWorldMeshWgsl: the level/billboard vertex + fragment shaders (bindings 2, 3: the level and sprite atlases).
 * kWorldBlitWgsl: nearest-neighbour blit of the offscreen 3D image into the swapchain.
 */

namespace devilution::render3d {

constexpr char kWorldCommonWgsl[] = R"(
struct WorldLight {
  posRadius: vec4f,      // tile space (x, height, y), radius in tiles
  colorIntensity: vec4f, // rgb, intensity
};

struct WorldFrame {
  viewProj: mat4x4f,
  cameraPos: vec4f,     // xyz, time in seconds
  cameraForward: vec4f, // xyz, eye height
  params: vec4f,        // ambient, exposure, town (0/1), wall height
  sky: vec4f,           // town: direction toward the moon (xyz), strength
  counts: vec4u,        // light count, grid size, flags (1: soft shadows, 2: no shadows), frame counter
  lights: array<WorldLight, 32>,
};

@group(0) @binding(0) var<uniform> frame: WorldFrame;
@group(0) @binding(1) var<storage, read> occluders: array<u32>;

// (bottom, top) of the blocking band of a cell; outside the grid everything blocks.
fn occluderBand(cell: vec2i) -> vec2f {
  let size = i32(frame.counts.y);
  if (cell.x < 0 || cell.y < 0 || cell.x >= size || cell.y >= size) {
    return vec2f(0.0, 1000.0);
  }
  let w = occluders[u32(cell.y) * u32(size) + u32(cell.x)];
  return vec2f(f32(w >> 16u), f32(w & 0xFFFFu)) * 0.001;
}

// 1 when the segment p -> l crosses no occluder band. 2D DDA over the xz cells; inside each cell the ray's height
// range [min(y in), max(y out)] is tested against the cell's band. The cells of p and l themselves never block.
fn shadowRay(p: vec3f, l: vec3f) -> f32 {
  let d = l - p;
  var cell = vec2i(floor(p.xz));
  let goal = vec2i(floor(l.xz));
  if (all(cell == goal)) {
    return 1.0;
  }
  let stepDir = vec2i(sign(d.xz));
  let absD = abs(d.xz);
  let invD = vec2f(select(1e30, 1.0 / absD.x, absD.x > 1e-6), select(1e30, 1.0 / absD.y, absD.y > 1e-6));
  let frac = p.xz - floor(p.xz);
  var tMax = vec2f(
    select(frac.x, 1.0 - frac.x, stepDir.x > 0) * invD.x,
    select(frac.y, 1.0 - frac.y, stepDir.y > 0) * invD.y);
  let tDelta = invD;
  for (var i = 0; i < 48; i++) {
    var tEnter: f32;
    if (tMax.x < tMax.y) {
      tEnter = tMax.x;
      tMax.x += tDelta.x;
      cell.x += stepDir.x;
    } else {
      tEnter = tMax.y;
      tMax.y += tDelta.y;
      cell.y += stepDir.y;
    }
    if (tEnter >= 1.0 || all(cell == goal)) {
      return 1.0;
    }
    let band = occluderBand(cell);
    if (band.y > band.x) {
      let tExit = min(min(tMax.x, tMax.y), 1.0);
      let y0 = p.y + d.y * tEnter;
      let y1 = p.y + d.y * tExit;
      if (max(y0, y1) > band.x && min(y0, y1) < band.y) {
        return 0.0;
      }
    }
  }
  return 1.0;
}

fn hash12(p: vec2f) -> f32 {
  var p3 = fract(vec3f(p.xyx) * 0.1031);
  p3 += dot(p3, p3.yzx + 33.33);
  return fract((p3.x + p3.y) * p3.z);
}

// Flicker factor of light i (torches and braziers: the object lights, intensity < 0.95), smooth noise over time.
fn lightFlicker(i: u32) -> f32 {
  let l = frame.lights[i];
  if (l.colorIntensity.a >= 0.95) {
    return 1.0;
  }
  let t = frame.cameraPos.w * 7.0 + hash12(l.posRadius.xz) * 40.0;
  let a = sin(t) * 0.5 + sin(t * 2.31 + 1.7) * 0.3 + sin(t * 5.13 + 0.4) * 0.2;
  return 1.0 + 0.1 * a;
}

// Lit colour of a surface point. `billboard`: wrap lighting for camera-facing sprites (they have no real normal).
// Falloff follows the classic light radius: close to full bright over the inner part of the radius, a smooth fade to
// black at the radius; surfaces are lit mostly independent of the angle (like the original's per-tile light levels)
// with a Lambert term for shape. Shadows stay ray traced through the occluder grid.
fn worldShade(p: vec3f, n: vec3f, albedo: vec3f, billboard: bool) -> vec3f {
  let town = frame.params.z > 0.5;
  var light = vec3f(frame.params.x);
  if (town) {
    let moon = max(dot(n, frame.sky.xyz), 0.0);
    light += vec3f(0.62, 0.68, 0.9) * frame.sky.w * select(0.35 + 0.65 * moon, 0.75, billboard);
  }
  let view = normalize(frame.cameraPos.xyz - p);
  let origin = p + n * 0.02;
  let soft = (frame.counts.z & 1u) != 0u;
  let count = min(frame.counts.x, 32u);
  for (var i = 0u; i < count; i++) {
    let lp = frame.lights[i].posRadius.xyz;
    let radius = frame.lights[i].posRadius.w;
    let toLight = lp - p;
    let dist = length(toLight);
    if (dist >= radius) {
      continue;
    }
    let l = toLight / max(dist, 1e-4);
    var lambert: f32;
    if (billboard) {
      lambert = 0.8 + 0.2 * max(dot(n, l), 0.0);
    } else {
      let nl = dot(n, l);
      if (nl <= -0.05) {
        continue;
      }
      lambert = 0.55 + 0.45 * max(nl, 0.0);
    }
    let atten = 1.0 - smoothstep(0.3, 1.0, dist / radius);
    var visibility: f32;
    if ((frame.counts.z & 2u) != 0u) {
      visibility = 1.0; // developer knob DVL_W3D_NO_SHADOWS
    } else if (soft) {
      // Two jittered taps around the light point: penumbrae at wall edges.
      let j = hash12(p.xz * 37.0 + vec2f(f32(frame.counts.w % 64u))) - 0.5;
      let side = normalize(vec3f(-l.z, 0.0, l.x)) * 0.18;
      visibility = 0.5 * (shadowRay(origin, lp + side * (0.5 + j)) + shadowRay(origin, lp - side * (0.5 - j)));
    } else {
      visibility = shadowRay(origin, lp);
    }
    if (visibility <= 0.0) {
      continue;
    }
    let h = normalize(l + view);
    let spec = select(pow(max(dot(n, h), 0.0), 24.0) * 0.1, 0.0, billboard);
    let c = frame.lights[i].colorIntensity;
    light += c.rgb * (c.a * lightFlicker(i) * atten * visibility * (lambert + spec));
  }
  // Overlapping lights (a volley of glowing missiles) roll off softly instead of burning the art out to white.
  let knee = 0.85;
  let over = max(light - vec3f(knee), vec3f(0.0));
  let toned = min(light, vec3f(knee)) + 0.55 * (vec3f(1.0) - exp(-over / 0.55));
  return albedo * toned * frame.params.y;
}
)";

constexpr char kWorldMeshWgsl[] = R"(
@group(0) @binding(2) var levelAtlas: texture_2d<f32>;
@group(0) @binding(3) var spriteAtlas: texture_2d<f32>;

const FLAG_SPRITE: u32 = 1u;
const FLAG_UNLIT: u32 = 2u;
const FLAG_OUTLINE: u32 = 4u;
const FLAG_BILLBOARD: u32 = 8u;
const FLAG_WATER: u32 = 16u;
const FLAG_SOFT: u32 = 32u;

struct VertexIn {
  @location(0) position: vec3f,
  @location(1) uv: vec2f,
  @location(2) normal: vec3f,
  @location(3) flags: u32,
};

struct VertexOut {
  @builtin(position) clip: vec4f,
  @location(0) world: vec3f,
  @location(1) uv: vec2f,
  @location(2) normal: vec3f,
  @location(3) @interpolate(flat) flags: u32,
};

@vertex fn worldVertex(input: VertexIn) -> VertexOut {
  var output: VertexOut;
  output.clip = frame.viewProj * vec4f(input.position, 1.0);
  output.world = input.position;
  output.uv = input.uv;
  output.normal = input.normal;
  output.flags = input.flags;
  return output;
}

fn spriteTexel(coord: vec2i) -> vec4f {
  let dims = vec2i(textureDimensions(spriteAtlas));
  return textureLoad(spriteAtlas, clamp(coord, vec2i(0), dims - 1), 0);
}

// Nearest texel of one mip level of the level atlas. Every slot is 32-texel aligned and the mips are box-filtered per
// 2 x 2 block, so a level never mixes two slots (no bleeding) as long as uv stays inside the slot.
fn levelTexel(uv: vec2f, level: i32) -> vec4f {
  let dims = vec2i(textureDimensions(levelAtlas, level));
  let coord = clamp(vec2i(floor(uv * vec2f(dims))), vec2i(0), dims - 1);
  return textureLoad(levelAtlas, coord, level);
}

@fragment fn worldFragment(input: VertexOut) -> @location(0) vec4f {
  // Derivatives first (uniform control flow).
  let tc = input.uv * vec2f(textureDimensions(levelAtlas, 0));
  let rho = max(length(dpdx(tc)), length(dpdy(tc)));
  let sprite = (input.flags & FLAG_SPRITE) != 0u;
  var c: vec4f;
  if (sprite) {
    let coord = vec2i(floor(input.uv * vec2f(textureDimensions(spriteAtlas))));
    c = spriteTexel(coord);
    if (c.a < 0.5) {
      if ((input.flags & FLAG_OUTLINE) != 0u) {
        let a = spriteTexel(coord + vec2i(1, 0)).a + spriteTexel(coord - vec2i(1, 0)).a
              + spriteTexel(coord + vec2i(0, 1)).a + spriteTexel(coord - vec2i(0, 1)).a;
        if (a > 0.5) {
          return vec4f(0.85, 0.08, 0.05, 1.0);
        }
      }
      discard;
    }
  } else {
    // Nearest texels of the two closest mips, blended: the pixel art stays crisp up close while distant and grazing
    // surfaces use the box-filtered mips instead of shimmering. Soft surfaces (ceilings) lean to low frequencies.
    let maxLevel = f32(textureNumLevels(levelAtlas) - 1u);
    var lod = log2(max(rho, 1e-6)) - 0.25;
    if ((input.flags & FLAG_SOFT) != 0u) {
      // Ceilings: only the low frequencies of the floor art (no converging tile seams overhead).
      lod = max(lod + 1.5, 2.5);
    }
    lod = clamp(lod, 0.0, maxLevel);
    let l0 = i32(floor(lod));
    let l1 = min(l0 + 1, i32(maxLevel));
    let t = smoothstep(0.2, 0.8, fract(lod));
    c = levelTexel(input.uv, l0);
    if (t > 0.0) {
      c = mix(c, levelTexel(input.uv, l1), t);
    }
    if (c.a < 0.5) {
      discard;
    }
  }
  let albedo = c.rgb * (f32((input.flags >> 8u) & 255u) / 128.0);
  if ((input.flags & FLAG_UNLIT) != 0u) {
    return vec4f(albedo, 1.0);
  }
  let billboard = (input.flags & FLAG_BILLBOARD) != 0u;
  let n = normalize(input.normal);
  var color = worldShade(input.world, n, albedo, billboard);
  if (frame.params.z > 0.5) {
    // Tristram's art is dark: lift the mid tones the way the fully lit classic town reads on screen (world and
    // billboards only; skinned meshes and the viewmodel keep worldShade's plain result).
    color = pow(max(color, vec3f(0.0)), vec3f(0.8));
  }
  if ((input.flags & FLAG_WATER) != 0u) {
    // Moonlit sheen on the river: a slow ripple modulating a reflection of the night sky.
    let time = frame.cameraPos.w;
    let ripple = sin(input.world.x * 9.0 + time * 1.7) * sin(input.world.z * 7.0 - time * 1.3);
    let view = normalize(frame.cameraPos.xyz - input.world);
    let fresnel = pow(1.0 - max(view.y, 0.0), 3.0);
    color = color * (0.9 + 0.12 * ripple) + vec3f(0.10, 0.13, 0.22) * fresnel * (0.8 + 0.4 * ripple);
  }
  return vec4f(color, 1.0);
}

// Additive glow around fire / lightning missiles: uv in [0, 1] over the quad, premultiplied colour in the normal.
@fragment fn glowFragment(input: VertexOut) -> @location(0) vec4f {
  let r = length(input.uv * 2.0 - 1.0);
  let a = pow(clamp(1.0 - r, 0.0, 1.0), 2.2);
  return vec4f(input.normal * a, 1.0);
}
)";

constexpr char kWorldBlitWgsl[] = R"(
@group(0) @binding(0) var image: texture_2d<f32>;

struct BlitOut {
  @builtin(position) position: vec4f,
  @location(0) uv: vec2f,
};

@vertex fn blitVertex(@builtin(vertex_index) index: u32) -> BlitOut {
  let positions = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
  let uvs = array<vec2f, 3>(vec2f(0.0, 1.0), vec2f(2.0, 1.0), vec2f(0.0, -1.0));
  var output: BlitOut;
  output.position = vec4f(positions[index], 0.0, 1.0);
  output.uv = uvs[index];
  return output;
}

@fragment fn blitFragment(input: BlitOut) -> @location(0) vec4f {
  let dims = vec2i(textureDimensions(image));
  let coord = clamp(vec2i(floor(input.uv * vec2f(dims))), vec2i(0), dims - 1);
  return vec4f(textureLoad(image, coord, 0).rgb, 1.0);
}
)";

} // namespace devilution::render3d
