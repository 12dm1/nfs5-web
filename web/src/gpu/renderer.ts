// nfs5-web WEB3: replays the frames runtime/web/r_web.c records. All drawing goes to one offscreen colour + depth pair,
// which a present blits to the canvas: a frame may take several segments.
//
// Ring (bytes): magic 0, seg_count 4, published 8, consumed 12, presents_pub 16, presents_done 20, segments 24 (32 bytes each:
// ready, cmd_words, vtx_bytes, up_bytes, has_present, cmd, vtx, up).
const C = { VIDEO: 1, TEX_CREATE: 2, TEX_DESTROY: 3, TEX_WRITE: 4, TEX_GENMIPS: 5, SCISSOR: 6, PIPE: 7, BIND: 8, UNIFORM: 9, DRAW: 10, PRESENT: 11, READRECT: 12, TARGET: 13, CUBE_MIPS: 14, PAINT: 15 };
const VSIZE = 48, VERTEX_BUFFER = 24 << 20, UBO_ALIGN = 256, UBO_SLOTS = 65536 / 256 * 64;

import { gfxLines, gfxSamples } from '../quality';

const SHADER = /* wgsl */`
struct U { screen: vec2f, alphaRef: f32, depthBias: f32, fogColor: vec4f, fogDensity: f32, fogScale: f32, hard: f32, lodBias: f32, em: vec4f,
  camR: vec4f, camU: vec4f, camF: vec4f, proj: vec4f, vp: vec4f, sun: vec4f, x0: vec4f, x1: vec4f };
@group(0) @binding(0) var<uniform> u: U;
@group(1) @binding(0) var tx: texture_2d<f32>;
@group(1) @binding(1) var sm: sampler;
@group(1) @binding(2) var paintTx: texture_2d<f32>;
@group(1) @binding(3) var sm2: sampler;
@group(2) @binding(0) var cubeTx: texture_cube<f32>;
@group(2) @binding(1) var cubeSm: sampler;
struct VI { @location(0) pos: vec4f, @location(1) uv: vec2f, @location(2) color: vec4f, @location(3) spec: vec4f, @location(4) rect: vec4f };
struct VO { @builtin(position) pos: vec4f, @location(0) uvq: vec3f, @location(1) color: vec4f, @interpolate(flat) @location(2) rect: vec4f,
  @location(3) aux: vec4f, @location(4) nrm: vec4f, @interpolate(linear) @location(5) ray: vec3f };
fn finite(x: f32) -> bool { return x == x && abs(x) < 1e30; }
@vertex fn vs(i: VI) -> VO {
  var o: VO;
  let ndc = vec2f(i.pos.x / u.screen.x * 2.0 - 1.0, 1.0 - i.pos.y / u.screen.y * 2.0);
  let z = i.pos.z + u.depthBias * (1.0 - i.pos.z);
  o.pos = vec4f(ndc, clamp(z, 0.0, 1.0), 1.0);
  var q = 1.0;
  if (finite(i.pos.w) && i.pos.w > 0.0) { q = i.pos.w; }
  o.uvq = vec3f(i.uv * q, q);
  o.color = i.color.bgra;
  o.rect = i.rect;
  // a carrier of the car page's UVs (r_web.c, the game's reflection pass over a car page)
  o.aux = vec4f(i.rect.xy * q, q, 0.0);
  if (i.rect.w == 7.0) { o.rect = vec4f(0.0); }
  // car normals in the specular dword, one byte per axis (runtime/pu_paint.c), x q for perspective
  let sp = i.spec * 255.0;
  if (sp.x >= 1.0) { o.nrm = vec4f((((sp.xyz - 1.0) / 254.0) * 2.0 - 1.0) * q, 1.0); } else { o.nrm = vec4f(0.0); }
  let su = (i.pos.x - u.vp.x) / select(1.0, u.vp.z, abs(u.vp.z) > 1e-6);
  let sv = (i.pos.y - u.vp.y) / select(1.0, u.vp.w, abs(u.vp.w) > 1e-6);
  o.ray = vec3f((su - u.proj.z) / select(1.0, u.proj.x, abs(u.proj.x) > 1e-6), -(sv - u.proj.w) / select(1.0, u.proj.y, abs(u.proj.y) > 1e-6), -1.0);
  return o;
}
fn fog(rgb: vec3f, q: f32) -> vec3f {
  if (u.fogColor.w <= 0.0) { return rgb; }
  let w = 1.0 / max(q, 1e-6);
  let f = clamp((1.0 - exp(-u.fogDensity * w)) * u.fogScale, 0.0, 1.0);
  return mix(rgb, u.fogColor.rgb, f);
}
fn shade(i: VO) -> vec4f {
  var uv = i.uvq.xy / i.uvq.z;
  let dim = vec2f(textureDimensions(tx, 0));
  if (i.rect.z > i.rect.x && i.rect.w > i.rect.y) {
    let half_ = 0.5 / dim;
    let lo = i.rect.xy + half_; let hi = i.rect.zw - half_;
    if (lo.x <= hi.x && lo.y <= hi.y) { uv = clamp(uv, lo, hi); } else { uv = 0.5 * (i.rect.xy + i.rect.zw); }
  }
  if (u.hard > 0.5 && !(i.rect.z > i.rect.x)) {
    let h = 1.5 / dim;
    uv = clamp(uv, h, vec2f(1.0) - h);
  }
  let t = textureSampleBias(tx, sm, uv, u.lodBias);
  var r = t * i.color;
  if (u.alphaRef > 0.0 && r.a < u.alphaRef) { discard; }
  if (u.hard > 0.5) { r.a = 1.0; }
  return vec4f(fog(r.rgb, i.uvq.z), r.a);
}
@fragment fn fs(i: VO) -> @location(0) vec4f { return shade(i); }
// WHEEL1: the rim (the car's tpage4, its quarter u, v < 0.5, centre 0.25, 0.25) smeared round its centre by the wheels' turn in a
// frame (u.x1.x radians, runtime/pu_paint.c pu_wheel_spin): 12 samples over the arc. The tyre's tread and the rest of the page as is.
const P_WHEEL = 1.0;
@fragment fn fs_wheel(i: VO) -> @location(0) vec4f {
  let uv = i.uvq.xy / i.uvq.z;
  let gx = dpdx(uv); let gy = dpdy(uv);
  let d = uv - vec2f(0.25);
  let ang = min(u.x1.x * P_WHEEL, 2.6);
  var t: vec4f;
  if (ang > 0.02 && all(uv >= vec2f(0.0)) && all(uv <= vec2f(0.5)) && dot(d, d) < 0.0625) {
    var acc = vec4f(0.0);
    for (var k = 0; k < 12; k++) {
      let a = ang * (f32(k) / 11.0 - 0.5);
      let cs = cos(a); let sn = sin(a);
      let p = vec2f(0.25) + vec2f(d.x * cs - d.y * sn, d.x * sn + d.y * cs);
      acc += textureSampleGrad(tx, sm, clamp(p, vec2f(0.002), vec2f(0.498)), gx, gy);
    }
    t = acc / 12.0;
  } else {
    t = textureSampleGrad(tx, sm, uv, gx, gy);
  }
  var r = t * i.color;
  if (u.alphaRef > 0.0 && r.a < u.alphaRef) { discard; }
  if (u.hard > 0.5) { r.a = 1.0; }
  return vec4f(fog(r.rgb, i.uvq.z), r.a);
}
// ---- car paint (PC dx11_shaders.h PSMaterial car branch, shaders/material_core.inc mat_shade_car; [car_paint] values of dx11z.ini) ----
const PI = 3.14159265;
const P_METAL = 0.2; const P_GRAZING = 0.5; const P_FLAKE_SCALE = 1200.0; const P_FLAKE = 0.015; const P_SILVER = 0.5;
const P_COAT = 1.0; const P_COAT_GLOSS = 0.0; const P_PEEL = 0.5; const P_PEEL_SCALE = 48.0; const P_ROUGH = 0.15; const P_METAL_MAP = 0.0;
const P_SPEC = 20.0; const P_REFL = 1.0; const P_GLOW = 35.8; const P_GLOW_ANG = 15.0; const P_GLOW_SAT = 0.6;
const P_VIEW_LIGHT = 1.2; const P_VIEW_DARK = 0.25; const P_ENV_DIFFUSE = 0.3; const P_ENV_GLOW = 0.0;
const P_DMG_DARK = 0.55;
const P_CAR_BIAS = 0.0; const P_SEAM = 0.85;
const P_SUN_FOG = 150.0; const P_LIGHT_GAIN = 1.7; const P_DARK_MATTE = 1.0; const P_DARK_POWER = 4.0; const P_SUN_REFL = 0.1;
const MAXMIP = 8.0;
const SUN_COL = vec3f(1.0, 0.95, 0.85);
fn lum(c: vec3f) -> f32 { return dot(c, vec3f(0.2126, 0.7152, 0.0722)); }
fn D_GGX(nh: f32, r: f32) -> f32 { let a = r * r; let a2 = a * a; let d = nh * nh * (a2 - 1.0) + 1.0; return a2 / (PI * d * d + 1e-7); }
fn G1(nx: f32, k: f32) -> f32 { return nx / (nx * (1.0 - k) + k + 1e-7); }
fn G_Smith(nv: f32, nl: f32, r: f32) -> f32 { let k = (r + 1.0) * (r + 1.0) / 8.0; return G1(nv, k) * G1(nl, k); }
fn fresnel(F0: vec3f, c: f32) -> vec3f { let t = 1.0 - c; let t2 = t * t; return F0 + (vec3f(1.0) - F0) * (t2 * t2 * t); }
fn fresnel_rk(F0: vec3f, c: f32, r: f32, k: f32) -> vec3f { let t = 1.0 - c; let t2 = t * t; let g = 1.0 - r; let Fm = max(vec3f(g), F0); return F0 + (Fm - F0) * (t2 * t2 * t) * k; }
fn hash2(x: i32, y: i32) -> f32 { var h = u32(x) * 374761393u + u32(y) * 668265263u; h = (h ^ (h >> 13u)) * 1274126177u; h = h ^ (h >> 16u); return f32(h & 16777215u) / 16777215.0; }
fn flake_offset(uv: vec2f, s: f32) -> vec2f { let c = floor(uv * s); let x = i32(c.x); let y = i32(c.y); return vec2f(hash2(x, y), hash2(x + 7919, y + 104729)) * 2.0 - vec2f(1.0); }
// smooth value noise in [-1, 1]^2 (PC material_core.inc mat_peel_offset): the clear coat's orange-peel ripple
fn peel_offset(uv: vec2f, s: f32) -> vec2f {
  let p = uv * s; let c = floor(p); var f = fract(p); f = f * f * (vec2f(3.0) - 2.0 * f);
  let x = i32(c.x); let y = i32(c.y);
  let a = vec2f(hash2(x, y), hash2(x + 7919, y + 104729)); let b = vec2f(hash2(x + 1, y), hash2(x + 7920, y + 104729));
  let c0 = vec2f(hash2(x, y + 1), hash2(x + 7919, y + 104730)); let d = vec2f(hash2(x + 1, y + 1), hash2(x + 7920, y + 104730));
  return mix(mix(a, b, f.x), mix(c0, d, f.x), f.y) * 2.0 - vec2f(1.0);
}
fn tilt(T: mat3x3f, off: vec2f, s: f32) -> vec3f { return normalize(T * normalize(vec3f(off * s, 1.0))); }
fn cotangent(N: vec3f, p1: vec3f, p2: vec3f, uv1: vec2f, uv2: vec2f) -> mat3x3f {
  let a = cross(p2, N); let b = cross(N, p1);
  let T = a * uv1.x + b * uv2.x; let B = a * uv1.y + b * uv2.y;
  let im = inverseSqrt(max(max(dot(T, T), dot(B, B)), 1e-20));
  return mat3x3f(T * im, B * im, N);
}
fn toWorld(d: vec3f) -> vec3f { return d.x * u.camR.xyz + d.y * u.camU.xyz - d.z * u.camF.xyz; }
fn cube(d: vec3f, mip: f32) -> vec4f { return textureSampleLevel(cubeTx, cubeSm, toWorld(d), mip); }
fn envGain(c: vec3f, g: f32) -> vec3f { return min(c, vec3f(1.0)) + max(c - vec3f(1.0), vec3f(0.0)) * g; }
fn envK(Nr: vec3f, V: vec3f, mip: f32, keep: f32) -> vec3f {
  if (u.camR.w < 0.5) { return vec3f(0.0); }
  let e = cube(reflect(-V, Nr), mip); let pk = max(e.r, max(e.g, e.b));
  let sh = select(0.0, clamp(e.a / pk, 0.0, 1.0), pk > 1e-5);
  return min(e.rgb * (1.0 - (1.0 - keep) * sh), vec3f(64.0));
}
fn envCone(d: vec3f, cone: f32, g: f32) -> vec3f {
  if (u.camR.w < 0.5) { return vec3f(0.0); }
  let mip = clamp(log2(max(exp2(MAXMIP) * cone / 90.0, 1.0)), 0.0, MAXMIP);
  let e = cube(d, mip); let pk = max(e.r, max(e.g, e.b));
  let sh = select(0.0, clamp(e.a / pk, 0.0, 1.0), pk > 1e-5);
  return envGain(min(e.rgb * (1.0 - sh), vec3f(64.0)), g);
}
fn sunVisibility() -> f32 {
  if (u.camU.w < 0.5) { return 1.0; }
  let e = cube(u.sun.xyz, MAXMIP - 5.0); let pk = max(e.r, max(e.g, e.b));
  var vis = clamp(e.a / 0.5, 0.0, 1.0) * clamp(pk - 0.5, 0.0, 1.0);
  if (u.fogColor.w > 0.0) { vis *= 1.0 - clamp((1.0 - exp(-u.fogDensity * P_SUN_FOG)) * u.fogScale, 0.0, 1.0); }
  return vis;
}
fn shade_car(Nb: vec3f, Nc: vec3f, N: vec3f, V: vec3f, L: vec3f, base: vec3f, df: vec3f, sun: vec3f, env_base: vec3f, env_coat: vec3f,
             metallic: f32, roughness: f32, spec_s: f32, refl_s: f32, coat: f32, coat_r: f32, sun_glow: f32, view_light: f32,
             env_irr: vec3f, env_glow_col: vec3f, env_diffuse: f32, env_glow: f32, light_gain: f32, area: vec3f) -> vec3f {
  let lit = lum(df);
  let F0 = mix(vec3f(0.04), mix(base, vec3f(0.85), P_SILVER), metallic);
  let H = normalize(V + L); let VdotH = max(dot(V, H), 0.0);
  let NdotV = max(dot(Nb, V), 0.001); let NdotL = max(dot(Nb, L), 0.0); let NdotH = max(dot(Nb, H), 0.0);
  let spec = sun * spec_s * lit * (D_GGX(NdotH, roughness) * G_Smith(NdotV, NdotL, roughness) * fresnel(F0, VdotH)) / (4.0 * NdotV * NdotL + 0.001) * NdotL;
  let Fr = fresnel_rk(F0, NdotV, roughness, P_GRAZING);
  let env_w = clamp(2.0 * (1.0 - roughness), 0.0, 1.0);
  let refl = env_base * Fr * refl_s * env_w;
  let sNdotL = max(dot(N, L), 0.0); let sNdotV = max(dot(N, V), 0.0);
  let ang = clamp(P_GLOW_ANG, 5.0, 85.0);
  let gk = log(0.5) / log(cos(ang * 0.017453292));
  let lobe = sun_glow * pow(sNdotL, gk) * lit;
  let sun_lum = lum(sun);
  let glow_env = env_glow_col * env_glow; let glow_env_lum = lum(glow_env);
  let base_lum = lum(base);
  // the paint's colourfulness from the page around the texel (a far mip), not the texel's own: a grey damage scrape in blue paint
  // took the white-paint lift (the whole sun glow as brightness, the achromatic gain) and lit up like a glint
  let amax = max(area.r, max(area.g, area.b));
  let chroma = clamp((amax - min(area.r, min(area.g, area.b))) / max(amax, 1e-4), 0.0, 1.0);
  let sat = P_GLOW_SAT * chroma;
  let base_pp = max(vec3f(base_lum) + (base - vec3f(base_lum)) * (1.0 + lobe * sat * sun_lum + glow_env_lum * sat), vec3f(0.0));
  let wgt = (1.0 - metallic) * (vec3f(1.0) - Fr * env_w);
  let d0 = base_pp * df * wgt;
  let achroma = (1.0 - chroma) * (1.0 - chroma);
  let add = base_pp * sun * (lobe * (1.0 - sat)) * wgt + base_pp * (glow_env * (1.0 - sat) + env_irr * env_diffuse) * wgt + d0 * (view_light * sNdotV * sNdotV + max(light_gain - 1.0, 0.0) * achroma);
  let room = (vec3f(1.0) - d0) / max(add, vec3f(1e-4));
  let s_max = min(min(room.x, min(room.y, room.z)), 1e4);
  let knee = select(0.0, s_max * (1.0 - exp(-1.0 / s_max)), s_max > 0.0);
  let diffuse = (d0 + add * knee) * (1.0 - P_VIEW_DARK * (1.0 - sNdotV) * (1.0 - sNdotV));
  let under = diffuse + spec + refl;
  let cNdotV = max(dot(Nc, V), 0.001); let cNdotL = max(dot(Nc, L), 0.0); let cNdotH = max(dot(Nc, H), 0.0);
  let Fc = fresnel_rk(vec3f(0.04), cNdotV, coat_r, P_GRAZING) * coat;
  let Fcs = fresnel(vec3f(0.04), VdotH) * coat;
  let spec_coat = sun * spec_s * lit * (D_GGX(cNdotH, coat_r) * G_Smith(cNdotV, cNdotL, coat_r) * Fcs) / (4.0 * cNdotV * cNdotL + 0.001) * cNdotL;
  let refl_coat = env_coat * Fc * refl_s;
  return under * (vec3f(1.0) - Fc) + spec_coat + refl_coat;
}
@fragment fn fs_car(i: VO) -> @location(0) vec4f {
  let uv = i.uvq.xy / i.uvq.z;
  let albedo = textureSampleBias(tx, sm, uv, u.lodBias + P_CAR_BIAS);
  let pk = textureSample(paintTx, sm, uv).rg;
  let d = 1.0 / max(i.uvq.z, 1e-6);
  let P = i.ray * d;
  let p1 = dpdx(P); let p2 = dpdy(P);
  let uv1 = dpdx(uv); let uv2 = dpdy(uv);
  let fw = length(fwidth(uv));
  let a = albedo.a * i.color.a;
  if (u.alphaRef > 0.0 && a < u.alphaRef) { discard; }
  // runtime/pu_paint.c: G 0.5 neutral; above, paint darker than its band (seams: matte by the dark power); below, damage strength
  let gv = pk.g * 255.0 - 127.5;
  var keep = pow(1.0 - clamp(gv / 127.5, 0.0, 1.0), P_DARK_POWER);
  keep = clamp(1.0 - P_DARK_MATTE * (1.0 - keep), 0.0, 1.0);
  // damage: darker by its strength, and the gloss goes as much
  let dmg = clamp(-gv / 127.5, 0.0, 1.0);
  keep = keep * (1.0 - dmg);
  let base = albedo.rgb * (1.0 - P_DMG_DARK * dmg);
  let V = -normalize(P);
  var Ngeo = normalize(cross(p1, p2));
  if (dot(Ngeo, P) > 0.0) { Ngeo = -Ngeo; }
  if (i.nrm.w > 0.5) {
    var ns = i.nrm.xyz; let l2 = dot(ns, ns);
    if (l2 > 1e-24) {
      ns = ns * inverseSqrt(l2);
      if (dot(ns, Ngeo) < 0.0) { ns = -ns; }
      let nv = dot(ns, V);
      if (nv < 0.05) { ns = normalize(ns + V * (0.05 - nv)); }
      Ngeo = ns;
    }
  }
  let N = Ngeo;
  let chrome = P_METAL_MAP * keep;
  let metal_c = clamp(P_METAL * keep + chrome, 0.0, 1.0);
  let coat_c = clamp(P_COAT * (1.0 - chrome), 0.0, 1.0) * keep;
  let rough = mix(1.0, P_ROUGH, keep);
  let cg = 1.0 - P_COAT_GLOSS; let rough_coat = max(cg * cg, 0.02);
  let T = cotangent(N, p1, p2, uv1, uv2);
  let fade = clamp(2.0 - fw * P_FLAKE_SCALE, 0.0, 1.0);
  var Nb = N;
  if (P_FLAKE * keep * fade > 0.0) { Nb = tilt(T, flake_offset(uv, P_FLAKE_SCALE), P_FLAKE * keep * fade * 0.6); }
  // the coat's ripple, faded out where its cells get smaller than two pixels (the PC has no fade: a far car shimmered)
  let peel_fade = clamp(2.0 - fw * P_PEEL_SCALE * 4.0, 0.0, 1.0);
  var Nc = N;
  if (P_PEEL * peel_fade > 0.0) { Nc = tilt(T, peel_offset(uv, P_PEEL_SCALE), P_PEEL * peel_fade * 0.08); }
  let vis = sunVisibility();
  let sun = SUN_COL * P_SUN_REFL * vis;
  let env_b = envGain(envK(Nb, V, rough * MAXMIP, P_SUN_REFL * vis), u.x0.x);
  let env_c = envGain(envK(Nc, V, rough_coat * MAXMIP, P_SUN_REFL * vis), u.x0.x);
  let env_irr = envCone(N, 60.0, u.x0.x);
  let env_glow = envCone(N, P_GLOW_ANG, u.x0.x);
  // the light (sun lobe, view light, sky fill, gain) falls on every paint texel: it scales the texel's own colour, so seams stay
  // dark. Only the shine (sun spot, reflection, coat) follows the texel's darkness. Scaled by it too, a damage scrape (darker
  // paint texels) lost the light and showed as a grey hole in lit paint.
  var rgb = shade_car(Nb, Nc, N, V, u.sun.xyz, base, i.color.rgb, sun, env_b, env_c, metal_c, rough, P_SPEC * keep, P_REFL * keep,
                      coat_c, rough_coat, P_GLOW, P_VIEW_LIGHT, env_irr, env_glow, P_ENV_DIFFUSE, P_ENV_GLOW,
                      P_LIGHT_GAIN, textureSampleLevel(tx, sm, uv, 3.0).rgb);
  // the seams (paint darker than its band, G above 128) darker once more by that ratio: the light lifts (gain, view light, sky) and
  // the knee washed them out on light paint (the user picked 0.85, 2026-11-26; ?seam=0 = off, ?seam=<0..3> to try)
  if (P_SEAM > 0.0) { rgb *= max(mix(1.0, clamp(1.0 - gv / 127.5, 0.0, 1.0), P_SEAM), 0.0); }
  rgb = mix(albedo.rgb * i.color.rgb, rgb, pk.r);   // texels the game does not paint keep the stock look
  return vec4f(fog(rgb, i.uvq.z), a);
}
// ---- intact car glass (GLASS1; PC car editor's Glass material: roughness 0.02, specular 20, reflection 1, grazing 1, no env
// diffuse): the R1 cube's reflection by a strong Fresnel, the sun's spot as on the paint. A second pass over the glass page's draw,
// after the view's opaque part (runtime/web/r_web.c glass_defer), premultiplied: what is under it (the game's own glass, its tint
// and alpha, and what is seen through) keeps (1 - F), the reflection adds. paintTx.g = 1: intact see-through glass
// (runtime/pu_paint.c glass_map); elsewhere (seals, frame, broken glass) nothing is drawn.
const G_ROUGH = 0.02; const G_SPEC = 20.0; const G_REFL = 1.0; const G_F0 = 0.04;
@fragment fn fs_glass(i: VO) -> @location(0) vec4f {
  let uv = i.uvq.xy / i.uvq.z;
  let g = textureSample(paintTx, sm, uv).g * select(0.0, 1.0, u.camR.w > 0.5);
  let d = 1.0 / max(i.uvq.z, 1e-6);
  let P = i.ray * d;
  var N = normalize(cross(dpdx(P), dpdy(P)));
  if (dot(N, P) > 0.0) { N = -N; }
  let V = -normalize(P);
  if (i.nrm.w > 0.5) {
    var ns = i.nrm.xyz; let l2 = dot(ns, ns);
    if (l2 > 1e-24) {
      ns = ns * inverseSqrt(l2);
      if (dot(ns, N) < 0.0) { ns = -ns; }
      let nv = dot(ns, V);
      if (nv < 0.05) { ns = normalize(ns + V * (0.05 - nv)); }
      N = ns;
    }
  }
  if (g < 0.5) { discard; }
  let L = u.sun.xyz; let H = normalize(V + L);
  let NdotV = max(dot(N, V), 0.001); let NdotL = max(dot(N, L), 0.0); let NdotH = max(dot(N, H), 0.0); let VdotH = max(dot(V, H), 0.0);
  let t = 1.0 - NdotV; let t2 = t * t;
  let F = G_F0 + (1.0 - G_F0) * t2 * t2 * t;
  let vis = sunVisibility();
  let sun = SUN_COL * P_SUN_REFL * vis;
  let env = envGain(envK(N, V, G_ROUGH * MAXMIP, P_SUN_REFL * vis), u.x0.x);
  let spec = sun * G_SPEC * lum(i.color.rgb) * (D_GGX(NdotH, G_ROUGH) * G_Smith(NdotV, NdotL, G_ROUGH) * fresnel(vec3f(G_F0), VdotH)) / (4.0 * NdotV * NdotL + 0.001) * NdotL;
  let refl = env * F * G_REFL + spec;
  // the fog hides the reflection as it hides the glass under it (that one is fogged already)
  let clear = fog(vec3f(1.0), i.uvq.z).r - fog(vec3f(0.0), i.uvq.z).r;   // 1 - the fog's share
  let out_ = vec4f(refl * clear, F * clear);
  return out_;
}
// the game's reflection pass over a car page: off the paint, which has its own
@fragment fn fs_mask(i: VO) -> @location(0) vec4f {
  let pk = textureSample(paintTx, sm2, i.aux.xy / max(i.aux.z, 1e-6)).rg;
  var r = shade(i);
  // the intact glass (g) has the cube's reflection too: off it when the cube is there
  return vec4f(r.rgb, r.a * (1.0 - max(pk.r, select(select(0.0, 0.75, abs(pk.g - 100.0 / 255.0) < 0.03), 1.0, pk.g > 0.9) * select(0.0, 1.0, u.camR.w > 0.5))));
}
// a cube face (PC dx11_shaders.h EmissiveGain, SunMaskOutput): em = gain, threshold, power (below 0: full-bright vertices only), sun sprite
@fragment fn fs_cap(i: VO) -> @location(0) vec4f {
  var r = shade(i);
  if (u.em.x > 1.0) {
    let peak = max(r.r, max(r.g, r.b));
    var x = clamp((peak - u.em.y) / max(1.0 - u.em.y, 1e-4), 0.0, 1.0);
    if (u.em.z < 0.0) { x *= clamp((min(i.color.r, min(i.color.g, i.color.b)) - 0.9) / 0.08, 0.0, 1.0); }
    r = vec4f(r.rgb * (1.0 + (u.em.x - 1.0) * pow(x, max(abs(u.em.z), 0.01))), r.a);
  }
  if (u.em.w > 0.5) { let a = r.rgb * r.a; return vec4f(a, max(a.r, max(a.g, a.b))); }
  return r;
}`;
const BLIT = /* wgsl */`
@group(0) @binding(0) var tx: texture_2d<f32>;
@group(0) @binding(1) var sm: sampler;
struct VO { @builtin(position) pos: vec4f, @location(0) uv: vec2f };
@vertex fn vs(@builtin(vertex_index) i: u32) -> VO {
  var o: VO; let x = f32(i & 1u) * 2.0; let y = f32((i >> 1u) & 1u) * 2.0;
  o.pos = vec4f(x * 2.0 - 1.0, 1.0 - y * 2.0, 0.0, 1.0); o.uv = vec2f(x, y); return o;
}
@fragment fn fs(i: VO) -> @location(0) vec4f { return textureSampleLevel(tx, sm, i.uv, 0.0); }`;

// LAMP1: light bloom. Every draw also writes a second target whose blend keeps only the additive draws (lamp and street
// light glows, head/brake lights, flares: the game's own light sprites), opaque draws clear it (no halo through a wall),
// alpha-blended ones dim it. At a present that target goes to a quarter-size chain: downsample, 9-tap blur both ways,
// a wide horizontal streak (lens), then the composite adds both to the frame. ?bloom=0 off (gfx low: off), ?bloom=<k>.
const BLOOM_WGSL = /* wgsl */`
@group(0) @binding(0) var tx: texture_2d<f32>;
@group(0) @binding(1) var sm: sampler;
@group(0) @binding(2) var tb: texture_2d<f32>;
@group(0) @binding(3) var ts: texture_2d<f32>;
struct VO { @builtin(position) pos: vec4f, @location(0) uv: vec2f };
const K_BLOOM = 1.0;
const K_GAIN = 2.2;
const K_STREAK = 0.6;
@vertex fn vs(@builtin(vertex_index) i: u32) -> VO {
  var o: VO; let x = f32(i & 1u) * 2.0; let y = f32((i >> 1u) & 1u) * 2.0;
  o.pos = vec4f(x * 2.0 - 1.0, 1.0 - y * 2.0, 0.0, 1.0); o.uv = vec2f(x, y); return o;
}
fn px(t: texture_2d<f32>) -> vec2f { return 1.0 / vec2f(textureDimensions(t)); }
// full-size glow -> quarter: 4 bilinear taps = a 4x4 box
@fragment fn fs_down(i: VO) -> @location(0) vec4f {
  let d = px(tx);
  let t0 = textureSampleLevel(tx, sm, i.uv + d * vec2f(-1.0, -1.0), 0.0).rgb; let t1 = textureSampleLevel(tx, sm, i.uv + d * vec2f(1.0, -1.0), 0.0).rgb;
  let t2 = textureSampleLevel(tx, sm, i.uv + d * vec2f(-1.0, 1.0), 0.0).rgb; let t3 = textureSampleLevel(tx, sm, i.uv + d * vec2f(1.0, 1.0), 0.0).rgb;
  // half mean, half max: a lamp a few pixels wide keeps its strength in the quarter image
  let c = 0.5 * (t0 + t1 + t2 + t3) * 0.25 + 0.5 * max(max(t0, t1), max(t2, t3));
  return vec4f(c, 1.0);
}
fn blur(uv: vec2f, dir: vec2f, step: f32) -> vec4f {
  let w = array<f32, 5>(0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
  var c = textureSampleLevel(tx, sm, uv, 0.0).rgb * w[0];
  for (var k = 1; k < 5; k++) {
    let o = dir * f32(k) * step;
    c += (textureSampleLevel(tx, sm, uv + o, 0.0).rgb + textureSampleLevel(tx, sm, uv - o, 0.0).rgb) * w[k];
  }
  return vec4f(c, 1.0);
}
@fragment fn fs_bh(i: VO) -> @location(0) vec4f { return blur(i.uv, vec2f(px(tx).x, 0.0), 1.2); }
@fragment fn fs_bv(i: VO) -> @location(0) vec4f { return blur(i.uv, vec2f(0.0, px(tx).y), 1.2); }
// the wide pass adds a soft halo to the tight core (keeps the core: c + wide)
@fragment fn fs_bh2(i: VO) -> @location(0) vec4f { return blur(i.uv, vec2f(px(tx).x, 0.0), 3.5); }
@fragment fn fs_bv2(i: VO) -> @location(0) vec4f { return blur(i.uv, vec2f(0.0, px(tx).y), 3.5); }
// the lens streak: a long thin horizontal smear of the brightest glow
@fragment fn fs_streak(i: VO) -> @location(0) vec4f {
  let d = px(tx).x;
  var c = vec3f(0.0); var wsum = 0.0;
  for (var k = -16; k <= 16; k++) {
    let w = exp(-abs(f32(k)) / 6.0);
    let t = textureSampleLevel(tx, sm, i.uv + vec2f(f32(k) * 3.0 * d, 0.0), 0.0).rgb;
    c += max(t - vec3f(0.08), vec3f(0.0)) * w; wsum += w;
  }
  return vec4f(c / wsum * 3.0 * vec3f(0.75, 0.85, 1.0), 1.0);
}
@fragment fn fs_comp(i: VO) -> @location(0) vec4f {
  let c = textureSampleLevel(tx, sm, i.uv, 0.0).rgb;
  let b = (textureSampleLevel(tb, sm, i.uv, 0.0).rgb * K_GAIN + textureSampleLevel(ts, sm, i.uv, 0.0).rgb * K_STREAK) * K_BLOOM;
  // screen-style add: bright light never clips to a flat white patch
  return vec4f(c + b * (vec3f(1.0) - c * 0.5), 1.0);
}`;

// GLASS1: the glass pass is premultiplied (its colour is the reflection, its alpha the share of what is under it that goes)
const GLASS_BLEND: GPUBlendState = { color: { srcFactor: 'one', dstFactor: 'one-minus-src-alpha' }, alpha: { srcFactor: 'zero', dstFactor: 'one' } };
const BLEND: (GPUBlendState | undefined)[] = [
  { color: { srcFactor: 'src-alpha', dstFactor: 'one-minus-src-alpha' }, alpha: { srcFactor: 'one', dstFactor: 'one-minus-src-alpha' } },
  { color: { srcFactor: 'src-alpha', dstFactor: 'one' }, alpha: { srcFactor: 'one', dstFactor: 'one-minus-src-alpha' } },
  { color: { srcFactor: 'zero', dstFactor: 'one-minus-src-alpha' }, alpha: { srcFactor: 'one', dstFactor: 'one-minus-src-alpha' } },
  { color: { srcFactor: 'dst', dstFactor: 'zero' }, alpha: { srcFactor: 'one', dstFactor: 'one-minus-src-alpha' } },
  undefined,
];
const DEPTH: GPUCompareFunction[] = ['never', 'less', 'equal', 'less-equal', 'greater', 'not-equal', 'greater-equal', 'always'];
const ADDR: GPUAddressMode[] = ['clamp-to-edge', 'repeat', 'mirror-repeat'];
const CULL: GPUCullMode[] = ['none', 'back', 'front'];
const TOPO: GPUPrimitiveTopology[] = ['triangle-list', 'line-list', 'point-list'];

// The race's mode for this window: its real pixels (720 to 2160 lines), its shape from 4:3 to 32:9. ?ws=0 keeps 4:3, ?rh=<lines> caps the lines.
export function raceMode(): [number, number] {
  const q = new URLSearchParams(location.search);
  const dpr = devicePixelRatio || 1, pw = Math.max(1, innerWidth * dpr), ph = Math.max(1, innerHeight * dpr);
  const aspect = q.get('ws') === '0' ? 4 / 3 : Math.min(32 / 9, Math.max(4 / 3, pw / ph));
  const cap = Math.min(gfxLines(), Number(q.get('rh') ?? 2160) || 2160);
  const h = Math.max(Math.min(720, cap), Math.min(cap, Math.round(Math.min(ph, pw / aspect)))) & ~1;
  return [Math.round(h * aspect / 8) * 8, h];
}

// The pixels the game's screen (lw x lh) is drawn at: the largest of its shape inside the window's real pixels, at most 2160 lines
// (?rh=<lines> caps them). A window of another size changes only these; its shape changes the game's screen too (r_web.c r_reshape).
function fitTarget(lw: number, lh: number): [number, number] {
  const q = new URLSearchParams(location.search);
  const dpr = devicePixelRatio || 1, pw = Math.max(1, innerWidth * dpr), ph = Math.max(1, innerHeight * dpr);
  const aspect = lw / lh, cap = Math.min(gfxLines(), Number(q.get('rh') ?? 2160) || 2160);
  const h = Math.max(480, Math.min(cap, Math.round(Math.min(ph, pw / aspect)))) & ~1;
  return [Math.max(2, Math.round(h * aspect / 2) * 2), h];
}

export interface RendererStats { jsSum: number; jsMax: number; gapMax: number; slow: number; submitMax: number; frames: number; segments: number; draws: number; lastDraws: number; lastVerts: number; textures: number; shots: Map<number, ArrayBuffer>; shotNext: boolean; shotVideo: boolean; }

export async function attachRenderer(memory: WebAssembly.Memory, ptr: number, canvas: HTMLCanvasElement, log: (s: string) => void, wantShots: number[] = []) {
  if (!navigator.gpu) throw new Error('this browser has no WebGPU');
  const adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
  if (!adapter) throw new Error('no WebGPU adapter');
  const device = await adapter.requestDevice({ requiredLimits: { maxBufferSize: VERTEX_BUFFER, maxTextureDimension2D: Math.min(8192, adapter.limits.maxTextureDimension2D), maxSamplersPerShaderStage: 16 } });
  const info = (adapter as unknown as { info?: { vendor?: string; architecture?: string; description?: string } }).info;
  log(`[gpu] WebGPU adapter: ${info?.vendor ?? '?'} ${info?.architecture ?? ''} ${info?.description ?? ''}`);
  device.addEventListener('uncapturederror', e => log('[gpu] error: ' + (e as GPUUncapturedErrorEvent).error.message));
  device.lost.then(i => log('[gpu] device lost: ' + i.message));
  const queue = device.queue;
  const ctx = canvas.getContext('webgpu')!;
  const canvasFormat = navigator.gpu.getPreferredCanvasFormat();
  ctx.configure({ device, format: canvasFormat, alphaMode: 'opaque' });

  let W = canvas.width, H = canvas.height;
  // AA1 (PC dx11z [display] msaa): the scene draws into a 4x multisampled colour + depth pair, resolved into colorTex before a
  // present, a shot or a pixel read. ?msaa=1 turns it off. The PC's alpha-to-coverage ended as "discard below the game's
  // threshold, survivors opaque", which the shader's hard cutout already does, so there is no coverage from alpha here.
  const msaaParam = Number(new URLSearchParams(location.search).get('msaa') ?? gfxSamples());
  const SAMPLES = msaaParam === 1 ? 1 : 4;
  const bloomParam = new URLSearchParams(location.search).get('bloom');
  const BLOOM = bloomParam === null ? gfxSamples() > 1 : Number(bloomParam) > 0;
  const BLOOM_K = bloomParam !== null && Number(bloomParam) > 0 ? Number(bloomParam) : 1.0;
  const makeTargets = () => {
    const colorTex = device.createTexture({ size: [W, H], format: 'rgba8unorm', usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_SRC });
    const msTex = SAMPLES > 1 ? device.createTexture({ size: [W, H], format: 'rgba8unorm', sampleCount: SAMPLES, usage: GPUTextureUsage.RENDER_ATTACHMENT }) : null;
    const depthTex = device.createTexture({ size: [W, H], format: 'depth24plus', sampleCount: SAMPLES, usage: GPUTextureUsage.RENDER_ATTACHMENT });
    const colorView = colorTex.createView();
    return { colorTex, msTex, depthTex, colorView, depthView: depthTex.createView(), drawView: msTex ? msTex.createView() : colorView };
  };
  let { colorTex, msTex, depthTex, colorView, depthView, drawView } = makeTargets();
  // LAMP1 targets: the glow (multisampled like the scene, resolved), the quarter chain, the composite the present shows
  const makeBloom = () => {
    if (!BLOOM) return null;
    const RT = GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING;
    const glowTex = device.createTexture({ size: [W, H], format: 'rgba8unorm', usage: RT });
    const msGlow = SAMPLES > 1 ? device.createTexture({ size: [W, H], format: 'rgba8unorm', sampleCount: SAMPLES, usage: GPUTextureUsage.RENDER_ATTACHMENT }) : null;
    const qw = Math.max(1, Math.ceil(W / 4)), qh = Math.max(1, Math.ceil(H / 4));
    const q = () => device.createTexture({ size: [qw, qh], format: 'rgba16float', usage: RT });
    const a = q(), b = q(), st = q(), w = q();
    const finalTex = device.createTexture({ size: [W, H], format: 'rgba8unorm', usage: RT | GPUTextureUsage.COPY_SRC });
    const glowView = glowTex.createView();
    return { glowTex, msGlow, a, b, st, finalTex, glowView, glowDraw: msGlow ? msGlow.createView() : glowView,
      av: a.createView(), bv: b.createView(), sv: st.createView(), wv: w.createView(), fv: finalTex.createView(),
      all: [glowTex, a, b, st, w, finalTex, ...(msGlow ? [msGlow] : [])] };
  };
  let bloom = makeBloom();
  let glowClear = true;
  if (BLOOM) log(`[gpu] LAMP1 light bloom x${BLOOM_K}`);
  log(`[gpu] MSAA x${SAMPLES}`);
  // the multisampled target into colorTex: an empty pass whose end resolves
  const resolve = (enc: GPUCommandEncoder) => {
    if (!msTex) return;
    const att: GPURenderPassColorAttachment[] = [{ view: drawView, resolveTarget: colorView, loadOp: 'load', storeOp: 'store' }];
    if (bloom && bloom.msGlow) att.push({ view: bloom.glowDraw, resolveTarget: bloom.glowView, loadOp: glowClear ? 'clear' : 'load', storeOp: 'store', clearValue: [0, 0, 0, 0] });
    enc.beginRenderPass({ colorAttachments: att }).end();
  };

  // ?paintdbg=1..6: what the paint shader sees (1 paint mask: magenta paint, green stock; 2 normal; 3 environment; 4 vertex light; 5 keep/paint/sun visibility; 6 albedo)
  const paintDbg = Number(new URLSearchParams(location.search).get('paintdbg') ?? 0);
  const PDBG = ['', 'select(vec3f(0.0, 1.0, 0.0), vec3f(1.0, 0.0, 1.0), pk.r > 0.5)', 'N * 0.5 + 0.5', 'env_b', 'i.color.rgb', 'vec3f(keep, pk.r, vis)', 'albedo.rgb'][paintDbg] ?? '';
  // ?peel=<0..2>: the orange peel's strength for a look (0 = off)
  const peelQ = Number(new URLSearchParams(location.search).get('peel') ?? NaN);
  let code = PDBG ? SHADER.replace('return vec4f(fog(rgb, i.uvq.z), a);', `return vec4f(${PDBG}, 1.0);`) : SHADER;
  // ?glassdbg=1: the glass shader's intact glass in magenta (2: its reflection alone), for counts in a test shot
  const glassDbg = Number(new URLSearchParams(location.search).get('glassdbg') ?? 0);
  if (glassDbg === 1) code = code.replace('  return out_;', '  return vec4f(1.0, 0.0, 1.0, 1.0);');
  if (glassDbg === 2) code = code.replace('  return out_;', '  return vec4f(refl, 1.0);');
  if (glassDbg === 3) code = code.replace('  if (g < 0.5) { discard; }', '').replace('  return out_;', '  return vec4f(1.0, 0.0, 1.0, 1.0);');
  // ?carbias=<-4..2>: the car page's mip bias (negative = sharper: thin seams survive the distance); ?seam=<0..3> (see fs_car)
  const carBias = Number(new URLSearchParams(location.search).get('carbias') ?? NaN);
  if (carBias >= -4 && carBias <= 2) code = code.replace('const P_CAR_BIAS = 0.0;', `const P_CAR_BIAS = ${carBias.toFixed(3)};`);
  const seamQ = Number(new URLSearchParams(location.search).get('seam') ?? NaN);
  if (seamQ >= 0 && seamQ <= 3) code = code.replace('const P_SEAM = 0.85;', `const P_SEAM = ${seamQ.toFixed(3)};`);
  // ?wheelblur=<k>: the rim smear x k (0 off)
  const wheelQ = Number(new URLSearchParams(location.search).get('wheelblur') ?? NaN);
  if (wheelQ >= 0 && wheelQ <= 4) code = code.replace('const P_WHEEL = 1.0;', `const P_WHEEL = ${wheelQ.toFixed(3)};`);
  // ?wheelspin=<radians>: a fixed smear for looks (the menus' cars stand still)
  const spinQ = Number(new URLSearchParams(location.search).get('wheelspin') ?? NaN);
  if (spinQ >= 0 && spinQ <= 3) code = code.replace('let ang = min(u.x1.x * P_WHEEL, 2.6);', `let ang = ${spinQ.toFixed(3)};`);
  if (peelQ >= 0 && peelQ <= 2) code = code.replace('const P_PEEL = 0.5;', `const P_PEEL = ${peelQ.toFixed(3)};`);
  if (BLOOM) {
    for (const n of ['fs', 'fs_car', 'fs_glass', 'fs_mask', 'fs_wheel']) code = code.replace(`@fragment fn ${n}(i: VO) -> @location(0) vec4f {`, `fn ${n}_b(i: VO) -> vec4f {`);
    code += `
struct MRT { @location(0) c: vec4f, @location(1) g: vec4f };
@fragment fn fs(i: VO) -> MRT { let c = fs_b(i); return MRT(c, c); }
@fragment fn fs_car(i: VO) -> MRT { let c = fs_car_b(i); return MRT(c, c); }
@fragment fn fs_glass(i: VO) -> MRT { let c = fs_glass_b(i); return MRT(c, c); }
@fragment fn fs_mask(i: VO) -> MRT { let c = fs_mask_b(i); return MRT(c, c); }
@fragment fn fs_wheel(i: VO) -> MRT { let c = fs_wheel_b(i); return MRT(c, c); }`;
  }
  const module = device.createShaderModule({ code });
  void module.getCompilationInfo().then(ci => { for (const m of ci.messages) log(`[gpu] shader ${m.type} line ${m.lineNum}: ${m.message}`); });
  const blitModule = device.createShaderModule({ code: BLIT });
  const uboLayout = device.createBindGroupLayout({ entries: [{ binding: 0, visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT, buffer: { type: 'uniform', hasDynamicOffset: true } }] });
  const texLayout = device.createBindGroupLayout({ entries: [
    { binding: 0, visibility: GPUShaderStage.FRAGMENT, texture: {} }, { binding: 1, visibility: GPUShaderStage.FRAGMENT, sampler: {} },
    { binding: 2, visibility: GPUShaderStage.FRAGMENT, texture: {} }, { binding: 3, visibility: GPUShaderStage.FRAGMENT, sampler: {} }] });
  const cubeLayout = device.createBindGroupLayout({ entries: [
    { binding: 0, visibility: GPUShaderStage.FRAGMENT, texture: { viewDimension: 'cube' } }, { binding: 1, visibility: GPUShaderStage.FRAGMENT, sampler: {} }] });
  const pipeLayout = device.createPipelineLayout({ bindGroupLayouts: [uboLayout, texLayout, cubeLayout] });
  const vbuf = device.createBuffer({ size: VERTEX_BUFFER, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
  const ubo = device.createBuffer({ size: UBO_SLOTS * UBO_ALIGN, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
  const uboGroup = device.createBindGroup({ layout: uboLayout, entries: [{ binding: 0, resource: { buffer: ubo, size: 192 } }] });
  const uboData = new ArrayBuffer(UBO_SLOTS * UBO_ALIGN);
  const uboF = new Float32Array(uboData);

  // The reflection cube (pu_cube.c): HDR, one mip chain, its alpha is the sun mask: the sun's sprites add their light, what stands in front clears it
  const CUBE_SIZE = 256, CUBE_FORMAT: GPUTextureFormat = 'rgba16float', CUBE_MIPS = 9;
  const cubeTex = device.createTexture({ size: [CUBE_SIZE, CUBE_SIZE, 6], format: CUBE_FORMAT, mipLevelCount: CUBE_MIPS, usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_SRC });
  const cubeDepth = device.createTexture({ size: [CUBE_SIZE, CUBE_SIZE], format: 'depth24plus', usage: GPUTextureUsage.RENDER_ATTACHMENT });
  const cubeDepthView = cubeDepth.createView();
  const cubeFace = (face: number, mip: number) => cubeTex.createView({ dimension: '2d', baseArrayLayer: face, arrayLayerCount: 1, baseMipLevel: mip, mipLevelCount: 1 });
  const cubeViews = Array.from({ length: 6 }, (_, f) => Array.from({ length: CUBE_MIPS }, (_, m) => cubeFace(f, m)));
  let capFace = -1, capClear = false, cubeFaces = 0;
  const capBlend = (key: number): GPUBlendState | undefined => {
    const base = BLEND[key & 7];
    if (key & (1 << 14)) return { color: { srcFactor: 'one', dstFactor: 'one' }, alpha: { srcFactor: 'one', dstFactor: 'one' } };
    const keep: GPUBlendComponent = key & (1 << 13) ? { srcFactor: 'zero', dstFactor: 'one-minus-src-alpha' } : { srcFactor: 'zero', dstFactor: 'one' };
    return { color: base ? base.color : { srcFactor: 'one', dstFactor: 'zero' }, alpha: keep };
  };
  // LAMP1: the glow target keeps the additive draws, opaque ones clear it, alpha-blended ones dim it
  const GLOW_KEEP: GPUBlendState = { color: { srcFactor: 'zero', dstFactor: 'one' }, alpha: { srcFactor: 'zero', dstFactor: 'one' } };
  const GLOW_CLEAR: GPUBlendState = { color: { srcFactor: 'zero', dstFactor: 'zero' }, alpha: { srcFactor: 'zero', dstFactor: 'zero' } };
  const GLOW_DIM: GPUBlendState = { color: { srcFactor: 'zero', dstFactor: 'one-minus-src-alpha' }, alpha: { srcFactor: 'zero', dstFactor: 'one' } };
  const glowBlend = (key: number): GPUBlendState => {
    if (key & (1 << 17)) return GLOW_KEEP;
    const m = key & 7;
    return m === 1 ? BLEND[1]! : m === 0 || m === 2 ? GLOW_DIM : m === 3 ? GLOW_KEEP : GLOW_CLEAR;
  };
  const pipes = new Map<number, GPURenderPipeline>();
  const pipeline = (key: number) => {
    let p = pipes.get(key);
    if (p) return p;
    const cap = (key >> 12) & 1;
    const blend = cap ? capBlend(key) : key & (1 << 17) ? GLASS_BLEND : BLEND[key & 7], zcmp = (key >> 3) & 7, zwrite = (key >> 6) & 1, cull = (key >> 7) & 3, topo = (key >> 9) & 3, cw = (key >> 11) & 1;
    p = device.createRenderPipeline({
      layout: pipeLayout,
      vertex: { module, entryPoint: 'vs', buffers: [{ arrayStride: VSIZE, attributes: [
        { shaderLocation: 0, offset: 0, format: 'float32x4' }, { shaderLocation: 1, offset: 16, format: 'float32x2' },
        { shaderLocation: 2, offset: 24, format: 'unorm8x4' }, { shaderLocation: 3, offset: 28, format: 'unorm8x4' },
        { shaderLocation: 4, offset: 32, format: 'float32x4' }] }] },
      fragment: { module, entryPoint: cap ? 'fs_cap' : key & (1 << 18) ? 'fs_wheel' : key & (1 << 15) ? 'fs_car' : key & (1 << 16) ? 'fs_mask' : key & (1 << 17) ? 'fs_glass' : 'fs', targets: [{ format: cap ? CUBE_FORMAT : 'rgba8unorm', blend, writeMask: cw ? GPUColorWrite.ALL : 0 },
        ...(BLOOM && !cap ? [{ format: 'rgba8unorm' as GPUTextureFormat, blend: glowBlend(key), writeMask: cw ? GPUColorWrite.ALL : 0 }] : [])] },
      primitive: { topology: TOPO[topo], cullMode: topo === 0 ? CULL[cull] : 'none', frontFace: 'cw' },
      depthStencil: { format: 'depth24plus', depthWriteEnabled: !!zwrite, depthCompare: DEPTH[zcmp] },
      multisample: { count: cap ? 1 : SAMPLES },
    });
    pipes.set(key, p);
    return p;
  };
  const samplers = new Map<number, GPUSampler>();
  const sampler = (key: number) => {
    let s = samplers.get(key);
    if (s) return s;
    const linear = key & 1, cs = (key >> 1) & 3, ct = (key >> 3) & 3, mips = (key >> 5) & 1;
    s = device.createSampler({
      addressModeU: ADDR[cs], addressModeV: ADDR[ct],
      magFilter: linear ? 'linear' : 'nearest', minFilter: linear ? 'linear' : 'nearest', mipmapFilter: linear ? 'linear' : 'nearest',
      lodMinClamp: 0, lodMaxClamp: mips ? 32 : 0, maxAnisotropy: linear ? 16 : 1,
    });
    samplers.set(key, s);
    return s;
  };

  const white = device.createTexture({ size: [1, 1], format: 'rgba8unorm', usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
  queue.writeTexture({ texture: white }, new Uint8Array([255, 255, 255, 255]), { bytesPerRow: 4 }, [1, 1]);
  interface T { id?: number; tex: GPUTexture; groups: Map<number, GPUBindGroup>; levels: number; paint?: GPUTexture }
  const textures = new Map<number, T>();
  const whiteT: T = { tex: white, groups: new Map(), levels: 1 };
  const noPaint = device.createTexture({ size: [1, 1], format: 'rg8unorm', usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
  const paintSampler = device.createSampler({ magFilter: 'linear', minFilter: 'linear', mipmapFilter: 'linear' });
  // pt: the texture whose paint map goes with it (the page itself, or the car page under the game's reflection pass)
  const group = (t: T, key: number, pt: T | undefined) => {
    const gk = key + (pt ? (pt === t ? 1 : 2) * 1024 : 0) + (pt && pt !== t ? 4096 * (pt.id ?? 0) : 0);
    let g = t.groups.get(gk);
    if (!g) {
      g = device.createBindGroup({ layout: texLayout, entries: [{ binding: 0, resource: t.tex.createView() }, { binding: 1, resource: sampler(key) },
        { binding: 2, resource: (pt?.paint ?? noPaint).createView() }, { binding: 3, resource: paintSampler }] });
      t.groups.set(gk, g);
    }
    return g;
  };
  const cubeSampler = device.createSampler({ magFilter: 'linear', minFilter: 'linear', mipmapFilter: 'linear' });
  const dummyCube = device.createTexture({ size: [1, 1, 6], format: CUBE_FORMAT, usage: GPUTextureUsage.TEXTURE_BINDING });
  const cubeGroup = device.createBindGroup({ layout: cubeLayout, entries: [{ binding: 0, resource: cubeTex.createView({ dimension: 'cube' }) }, { binding: 1, resource: cubeSampler }] });
  const dummyCubeGroup = device.createBindGroup({ layout: cubeLayout, entries: [{ binding: 0, resource: dummyCube.createView({ dimension: 'cube' }) }, { binding: 1, resource: cubeSampler }] });

  // the texture's lower levels from level 0 (a partial update of a streamed texture)
  const genSampler = device.createSampler({ magFilter: 'linear', minFilter: 'linear' });
  const blitLayout = device.createBindGroupLayout({ entries: [
    { binding: 0, visibility: GPUShaderStage.FRAGMENT, texture: {} }, { binding: 1, visibility: GPUShaderStage.FRAGMENT, sampler: {} }] });
  const blitPipeLayout = device.createPipelineLayout({ bindGroupLayouts: [blitLayout] });
  const blitPipe = (format: GPUTextureFormat) => device.createRenderPipeline({ layout: blitPipeLayout,
    vertex: { module: blitModule, entryPoint: 'vs' }, fragment: { module: blitModule, entryPoint: 'fs', targets: [{ format }] }, primitive: { topology: 'triangle-list' } });
  const genPipe = blitPipe('rgba8unorm'), presentPipe = blitPipe(canvasFormat), cubeMipPipe = blitPipe(CUBE_FORMAT);
  const makePresentGroup = () => device.createBindGroup({ layout: blitLayout, entries: [{ binding: 0, resource: colorView }, { binding: 1, resource: genSampler }] });
  let presentGroup = makePresentGroup();
  // ?bloomdbg=1: the composite shows the bloom alone (2: the glow target as the scene left it)
  const bloomDbg = Number(new URLSearchParams(location.search).get('bloomdbg') ?? 0);
  let bloomCode = BLOOM_WGSL.replace('const K_BLOOM = 1.0;', `const K_BLOOM = ${BLOOM_K.toFixed(3)};`);
  if (bloomDbg === 1) bloomCode = bloomCode.replace('return vec4f(c + b * (vec3f(1.0) - c * 0.5), 1.0);', 'return vec4f(b, 1.0);');
  const bloomModule = BLOOM ? device.createShaderModule({ code: bloomCode }) : null;
  if (bloomModule) void bloomModule.getCompilationInfo().then(ci => { for (const m of ci.messages) log(`[gpu] bloom shader ${m.type} line ${m.lineNum}: ${m.message}`); });
  const bloomLayout = device.createBindGroupLayout({ entries: [
    { binding: 0, visibility: GPUShaderStage.FRAGMENT, texture: {} }, { binding: 1, visibility: GPUShaderStage.FRAGMENT, sampler: {} },
    { binding: 2, visibility: GPUShaderStage.FRAGMENT, texture: {} }, { binding: 3, visibility: GPUShaderStage.FRAGMENT, texture: {} }] });
  const bloomPipeLayout = device.createPipelineLayout({ bindGroupLayouts: [bloomLayout] });
  const bloomPipe = (entry: string, format: GPUTextureFormat) => device.createRenderPipeline({ layout: bloomPipeLayout,
    vertex: { module: bloomModule!, entryPoint: 'vs' }, fragment: { module: bloomModule!, entryPoint: entry, targets: [{ format }] }, primitive: { topology: 'triangle-list' } });
  // the wide halo is added onto the tight blur (one, one)
  const bloomPipeAdd = () => device.createRenderPipeline({ layout: bloomPipeLayout,
    vertex: { module: bloomModule!, entryPoint: 'vs' }, fragment: { module: bloomModule!, entryPoint: 'fs_bv2', targets: [{ format: 'rgba16float',
      blend: { color: { srcFactor: 'one', dstFactor: 'one' }, alpha: { srcFactor: 'one', dstFactor: 'one' } } }] }, primitive: { topology: 'triangle-list' } });
  const bp = bloomModule ? { down: bloomPipe('fs_down', 'rgba16float'), bh: bloomPipe('fs_bh', 'rgba16float'), bv: bloomPipe('fs_bv', 'rgba16float'),
    bh2: bloomPipe('fs_bh2', 'rgba16float'), bv2: bloomPipe('fs_bv2', 'rgba16float'),
    streak: bloomPipe('fs_streak', 'rgba16float'), comp: bloomPipe('fs_comp', 'rgba8unorm'), add: bloomPipeAdd() } : null;
  const bg = (a: GPUTextureView, b?: GPUTextureView, c?: GPUTextureView) => device.createBindGroup({ layout: bloomLayout, entries: [
    { binding: 0, resource: a }, { binding: 1, resource: genSampler }, { binding: 2, resource: b ?? a }, { binding: 3, resource: c ?? a }] });
  const makeBloomGroups = () => bloom ? { down: bg(bloom.glowView), bh: bg(bloom.av), bv: bg(bloom.bv), streak: bg(bloom.av), wide: bg(bloom.wv),
    comp: bloomDbg === 2 ? bg(bloom.glowView, bloom.glowView, bloom.glowView) : bg(colorView, bloom.av, bloom.sv), show: device.createBindGroup({ layout: blitLayout, entries: [{ binding: 0, resource: bloom.fv }, { binding: 1, resource: genSampler }] }) } : null;
  let bloomGroups = makeBloomGroups();
  const runBloom = (enc: GPUCommandEncoder) => {
    if (!bloom || !bp || !bloomGroups) return;
    const step = (pipe: GPURenderPipeline, group: GPUBindGroup, view: GPUTextureView, load: GPULoadOp = 'clear') => {
      const p = enc.beginRenderPass({ colorAttachments: [{ view, loadOp: load, storeOp: 'store', clearValue: [0, 0, 0, 0] }] });
      p.setPipeline(pipe); p.setBindGroup(0, group); p.draw(3); p.end();
    };
    step(bp.down, bloomGroups.down, bloom.av);           // glow -> a (quarter)
    step(bp.streak, bloomGroups.streak, bloom.sv);       // a -> streak
    step(bp.bh2, bloomGroups.bh, bloom.wv);              // a -> w (wide, horizontal)
    step(bp.bh, bloomGroups.bh, bloom.bv);               // a -> b (tight, horizontal)
    step(bp.bv, bloomGroups.bv, bloom.av);               // b -> a (tight)
    step(bp.add, bloomGroups.wide, bloom.av, 'load');    // a += vertical(w) (wide halo)
    step(bp.comp, bloomGroups.comp, bloom.fv);
  };
  const genMips = (t: T) => {
    const enc = device.createCommandEncoder();
    for (let l = 1; l < t.levels; l++) {
      const pass = enc.beginRenderPass({ colorAttachments: [{ view: t.tex.createView({ baseMipLevel: l, mipLevelCount: 1 }), loadOp: 'clear', storeOp: 'store', clearValue: [0, 0, 0, 0] }] });
      pass.setPipeline(genPipe);
      pass.setBindGroup(0, device.createBindGroup({ layout: blitLayout, entries: [{ binding: 0, resource: t.tex.createView({ baseMipLevel: l - 1, mipLevelCount: 1 }) }, { binding: 1, resource: genSampler }] }));
      pass.draw(3);
      pass.end();
    }
    queue.submit([enc.finish()]);
  };

  const sab = memory.buffer as unknown as SharedArrayBuffer;
  const ring = new Int32Array(sab, ptr, 6 + 3 * 8 + 2);
  const segCount = ring[1];
  let nextSeg = 0;
  const stats: RendererStats = { jsSum: 0, jsMax: 0, gapMax: 0, slow: 0, submitMax: 0, frames: 0, segments: 0, draws: 0, lastDraws: 0, lastVerts: 0, textures: 0, shots: new Map(), shotNext: false, shotVideo: new URLSearchParams(location.search).has('shotvideo') };
  const shotSet = new Set(wantShots);
  let lw = 640, lh = 480;
  // the element takes the shape of the game's mode; the canvas behind it keeps its size, so a 4:3 menu fills the wide canvas and
  // shows undistorted, and the mouse and the focus outline keep mapping the element's box to the game's screen
  // a mode within 1 % of the window's shape fills it, the race then has no thin bars from rounding its width
  const setShape = (w: number, h: number) => {
    const fill = Math.abs(w / h / (innerWidth / Math.max(1, innerHeight)) - 1) < 0.01;
    canvas.style.aspectRatio = fill ? 'auto' : `${w} / ${h}`;
    canvas.style.width = fill ? '100%' : '';
  };
  setShape(lw, lh);
  // for the game: the mode its next race starts with (r_web.c r_window_mode)
  // while the window's edge is dragged the targets stay: new ones once its size has held for REFIT_MS, the picture is scaled till then
  const REFIT_MS = 200;
  let refit = true, refitAt = 0, frameStart = true;
  const putWindow = () => { const [w, h] = raceMode(); Atomics.store(ring, 30, w); Atomics.store(ring, 31, h); setShape(lw, lh); refit = true; refitAt = performance.now() + REFIT_MS; };
  putWindow();
  addEventListener('resize', putWindow);

  // a segment: its uploads first, then its draws in one pass
  let frameJs = 0, lastPresentAt = 0;
  const cubeLog = new URLSearchParams(location.search).has('cubelog');
  let cubeRead = -1;
  // new targets for the game's screen in this window; the old ones go after this segment's work
  const retarget = (toFree: GPUTexture[]) => {
    refit = false;
    const [nw, nh] = fitTarget(lw, lh);
    if (nw === W && nh === H) return;
    toFree.push(colorTex, depthTex); if (msTex) toFree.push(msTex);
    W = nw; H = nh; canvas.width = W; canvas.height = H;
    ({ colorTex, msTex, depthTex, colorView, depthView, drawView } = makeTargets());
    if (bloom) toFree.push(...bloom.all);
    bloom = makeBloom(); bloomGroups = makeBloomGroups(); glowClear = true;
    presentGroup = makePresentGroup();
    log(`[gpu] screen ${lw}x${lh} drawn at ${W}x${H}`);
  };
  const runSegment = (s: number) => {
    const t0 = performance.now();
    const b = 6 + s * 8;
    const words = ring[b + 1], vtxBytes = ring[b + 2], cmdPtr = ring[b + 5] >>> 0, vtxPtr = ring[b + 6] >>> 0, upPtr = ring[b + 7] >>> 0;
    const buffer = memory.buffer as unknown as SharedArrayBuffer;
    const cmd = new Uint32Array(buffer, cmdPtr, words), cf = new Float32Array(buffer, cmdPtr, words);
    const mem = new Uint8Array(buffer);
    if (vtxBytes) queue.writeBuffer(vbuf, 0, buffer as unknown as ArrayBuffer, vtxPtr, vtxBytes);
    const enc = device.createCommandEncoder();
    let pass: GPURenderPassEncoder | null = null;
    let present = false, draws = 0, verts = 0, ubos = 0;
    let read: { seq: number; x: number; y: number; w: number; h: number; px: number; done: number } | null = null;
    const toFree: GPUTexture[] = [];
    let curTex: T = whiteT, curSampler = 0, bindDirty = true, pipeKey = -1, curPaint: T | undefined;
    let sc = [0, 0, lw, lh], scDirty = true, uboOff = 0;
    if (refit && frameStart && t0 >= refitAt) retarget(toFree);   // a resized window: between two frames
    const begin = () => {
      if (pass) return pass;
      const load: GPULoadOp = capFace >= 0 && capClear ? 'clear' : 'load';
      capClear = false;
      const att: GPURenderPassColorAttachment[] = [{ view: capFace >= 0 ? cubeViews[capFace][0] : drawView, loadOp: load, storeOp: 'store', clearValue: [0, 0, 0, 0] }];
      if (bloom && capFace < 0) { att.push({ view: bloom.glowDraw, loadOp: glowClear ? 'clear' : 'load', storeOp: 'store', clearValue: [0, 0, 0, 0] }); glowClear = false; }
      pass = enc.beginRenderPass({
        colorAttachments: att,
        depthStencilAttachment: { view: capFace >= 0 ? cubeDepthView : depthView, depthLoadOp: load, depthClearValue: 1, depthStoreOp: 'store' },
      });
      pass.setVertexBuffer(0, vbuf);
      pass.setBindGroup(2, capFace >= 0 ? dummyCubeGroup : cubeGroup);
      pipeKey = -1; bindDirty = true; scDirty = true;
      return pass;
    };
    for (let i = 0; i < words;) {
      const op = cmd[i++];
      switch (op) {
        case C.VIDEO: {
          if (stats.shotVideo && cmd[i] > lw) for (const k of [1, 2, 3, 6, 12]) shotSet.add(stats.frames + k);
          const nw = cmd[i++], nh = cmd[i++];
          if (nw !== lw || nh !== lh) setShape(nw, nh);
          lw = nw; lh = nh; scDirty = true;
          if (pass) { (pass as GPURenderPassEncoder).end(); pass = null; }
          retarget(toFree);
          break; }
        case C.TEX_CREATE: {
          const id = cmd[i], w = cmd[i + 1], h = cmd[i + 2], levels = cmd[i + 3]; i += 4;
          const old = textures.get(id);
          if (old) toFree.push(old.tex);
          if (old?.paint) toFree.push(old.paint);
          textures.set(id, { id, tex: device.createTexture({ size: [w, h], format: 'rgba8unorm', mipLevelCount: levels,
            usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST | GPUTextureUsage.RENDER_ATTACHMENT }), groups: new Map(), levels });
          break; }
        case C.TEX_DESTROY: {
          const id = cmd[i++], t = textures.get(id);
          if (t) { toFree.push(t.tex); if (t.paint) toFree.push(t.paint); textures.delete(id); }
          break; }
        case C.TEX_WRITE: {
          const id = cmd[i], level = cmd[i + 1], x = cmd[i + 2], y = cmd[i + 3], w = cmd[i + 4], h = cmd[i + 5], ofs = cmd[i + 6]; i += 7;
          const t = textures.get(id);
          if (t) queue.writeTexture({ texture: t.tex, mipLevel: level, origin: [x, y] }, mem as unknown as ArrayBuffer, { offset: upPtr + ofs, bytesPerRow: w * 4, rowsPerImage: h }, [w, h]);
          break; }
        case C.PAINT: {
          const id = cmd[i], w = cmd[i + 1], h = cmd[i + 2], ofs = cmd[i + 3]; i += 4;
          const t = textures.get(id);
          if (t) {
            if (t.paint) toFree.push(t.paint);
            t.paint = undefined;
            if (w) {
              t.paint = device.createTexture({ size: [w, h], format: 'rg8unorm', usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
              queue.writeTexture({ texture: t.paint }, mem as unknown as ArrayBuffer, { offset: upPtr + ofs, bytesPerRow: w * 2, rowsPerImage: h }, [w, h]);
            }
            t.groups.clear();
            for (const o of textures.values()) if (o !== t) for (const k of [...o.groups.keys()]) if (k >= 4096) o.groups.delete(k);
          }
          break; }
        case C.TEX_GENMIPS: { const t = textures.get(cmd[i++]); if (t) genMips(t); break; }
        case C.SCISSOR: sc = [cmd[i], cmd[i + 1], cmd[i + 2], cmd[i + 3]]; i += 4; scDirty = true; break;
        case C.PIPE: { const k = cmd[i++]; if (k !== pipeKey) { pipeKey = k; begin().setPipeline(pipeline(k)); } break; }
        case C.BIND: { const id = cmd[i++]; curSampler = cmd[i++]; const pid = cmd[i++]; curTex = (id && textures.get(id)) || whiteT; curPaint = pid ? textures.get(pid) : undefined; bindDirty = true; break; }
        case C.UNIFORM: {
          if (ubos < UBO_SLOTS) {
            uboOff = ubos * UBO_ALIGN;
            uboF.set(cf.subarray(i, i + 48), uboOff / 4);
            ubos++;
            begin().setBindGroup(0, uboGroup, [uboOff]);
          }
          i += 48;
          break; }
        case C.DRAW: {
          const first = cmd[i++], count = cmd[i++], p = begin();
          if (bindDirty) { p.setBindGroup(1, group(curTex, curSampler, curPaint)); bindDirty = false; }
          if (scDirty) {
            const tw = capFace >= 0 ? CUBE_SIZE : W, th = capFace >= 0 ? CUBE_SIZE : H;
            const x0 = Math.max(0, Math.floor(sc[0] * tw / lw)), y0 = Math.max(0, Math.floor(sc[1] * th / lh));
            const x1 = Math.min(tw, Math.floor((sc[2] | 0) * tw / lw)), y1 = Math.min(th, Math.floor((sc[3] | 0) * th / lh));
            p.setScissorRect(Math.min(x0, tw), Math.min(y0, th), Math.max(0, x1 - x0), Math.max(0, y1 - y0));
            scDirty = false;
          }
          p.draw(count, 1, first);
          draws++; verts += count;
          break; }
        case C.PRESENT: present = true; break;
        case C.TARGET: {
          if (pass) { (pass as GPURenderPassEncoder).end(); pass = null; }
          capFace = cmd[i++] | 0; capClear = capFace >= 0; pipeKey = -1; scDirty = true;
          break; }
        case C.CUBE_MIPS: {
          const f = cmd[i++];
          for (let m = 1; m < CUBE_MIPS; m++) {
            const mp = enc.beginRenderPass({ colorAttachments: [{ view: cubeViews[f][m], loadOp: 'clear', storeOp: 'store', clearValue: [0, 0, 0, 0] }] });
            mp.setPipeline(cubeMipPipe);
            mp.setBindGroup(0, device.createBindGroup({ layout: blitLayout, entries: [{ binding: 0, resource: cubeViews[f][m - 1] }, { binding: 1, resource: genSampler }] }));
            mp.draw(3); mp.end();
          }
          cubeFaces++;
          if (cubeLog && cubeFaces % 360 < 6) cubeRead = f;
          break; }
        case C.READRECT: { read = { seq: cmd[i], x: cmd[i + 1] | 0, y: cmd[i + 2] | 0, w: cmd[i + 3], h: cmd[i + 4], px: cmd[i + 5], done: cmd[i + 6] }; i += 7; break; }
        default: log(`[gpu] unknown command ${op} at ${i - 1} of ${words}`); i = words; break;
      }
    }
    if (ubos) queue.writeBuffer(ubo, 0, uboData, 0, ubos * UBO_ALIGN);
    const cubeRb = cubeRead >= 0 && !pass ? { f: cubeRead, b: device.createBuffer({ size: 256 * 16, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ }) } : null;
    if (cubeRb) { enc.copyTextureToBuffer({ texture: cubeTex, mipLevel: 4, origin: [0, 0, cubeRb.f] }, { buffer: cubeRb.b, bytesPerRow: 256 }, [16, 16]); cubeRead = -1; }
    (pass as GPURenderPassEncoder | null)?.end();
    stats.segments++; stats.draws += draws; stats.lastDraws = draws; stats.lastVerts = verts;
    if (present || read) resolve(enc);
    if (present) {
      const frame = ++stats.frames;
      const view = ctx.getCurrentTexture().createView();
      // the race only (its mode is wider than the menus' 640 x 480): the showroom's light panels are large additive walls
      const bloomNow = !!bloomGroups && lw > 640;
      if (bloomNow) runBloom(enc);
      const out = enc.beginRenderPass({ colorAttachments: [{ view, loadOp: 'clear', storeOp: 'store', clearValue: [0, 0, 0, 1] }] });
      out.setPipeline(presentPipe); out.setBindGroup(0, bloomNow ? bloomGroups!.show : presentGroup); out.draw(3); out.end();
      glowClear = true;
      if (shotSet.has(frame) || stats.shotNext) {
        stats.shotNext = false;
        const sw = W, sh = H, row = ((sw * 4 + 255) >> 8) << 8;
        const rb = device.createBuffer({ size: row * sh, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
        enc.copyTextureToBuffer({ texture: bloomNow ? bloom!.finalTex : colorTex }, { buffer: rb, bytesPerRow: row }, [sw, sh]);
        void queue.onSubmittedWorkDone().then(async () => {
          await rb.mapAsync(GPUMapMode.READ);
          const src = new Uint8Array(rb.getMappedRange()), px = new Uint8Array(sw * sh * 4);
          for (let y = 0; y < sh; y++) px.set(src.subarray(y * row, y * row + sw * 4), y * sw * 4);
          stats.shots.set(frame, px.buffer); rb.destroy();
        });
      }
    }
    // the lens flare's pixels (r_readrect): the last command of its segment, after the pass; RGB565 like a Glide frame read
    let readBack: (() => void) | null = null;
    if (read) {
      const { seq, x, y, w, h, px, done } = read as { seq: number; x: number; y: number; w: number; h: number; px: number; done: number };
      const sx = (v: number) => Math.floor(v * W / lw), sy = (v: number) => Math.floor(v * H / lh);
      const x0 = Math.max(0, sx(x)), y0 = Math.max(0, sy(y)), x1 = Math.min(W, Math.max(sx(x + w), sx(x) + 1)), y1 = Math.min(H, Math.max(sy(y + h), sy(y) + 1));
      const finish = (data: Uint8Array | null, row: number) => {
        const out = new Uint16Array(memory.buffer as unknown as SharedArrayBuffer, px, w * h);
        for (let j = 0; j < h; j++) for (let k = 0; k < w; k++) {
          const tx = sx(x + k), ty = sy(y + j);
          if (!data || tx < x0 || tx >= x1 || ty < y0 || ty >= y1) { out[j * w + k] = 0; continue; }
          const o = (ty - y0) * row + (tx - x0) * 4;
          out[j * w + k] = (data[o] >> 3) << 11 | (data[o + 1] >> 2) << 5 | data[o + 2] >> 3;
        }
        const d = new Int32Array(memory.buffer as unknown as SharedArrayBuffer, done, 1);
        Atomics.store(d, 0, seq); Atomics.notify(d, 0);
      };
      if (x1 <= x0 || y1 <= y0) readBack = () => finish(null, 0);
      else {
        const row = (((x1 - x0) * 4 + 255) >> 8) << 8;
        const rb = device.createBuffer({ size: row * (y1 - y0), usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
        enc.copyTextureToBuffer({ texture: colorTex, origin: [x0, y0] }, { buffer: rb, bytesPerRow: row }, [x1 - x0, y1 - y0]);
        readBack = () => { void rb.mapAsync(GPUMapMode.READ).then(() => { finish(new Uint8Array(rb.getMappedRange()), row); rb.destroy(); }, () => { finish(null, 0); rb.destroy(); }); };
      }
    }
    const ts = performance.now();
    queue.submit([enc.finish()]);
    const te = performance.now();
    if (te - ts > stats.submitMax) stats.submitMax = te - ts;
    frameJs += te - t0;
    frameStart = present;
    if (present) {
      stats.jsSum += frameJs; if (frameJs > stats.jsMax) stats.jsMax = frameJs; frameJs = 0;
      if (lastPresentAt) { const gap = te - lastPresentAt; if (gap > stats.gapMax) stats.gapMax = gap; if (gap > 25) stats.slow++; }
      lastPresentAt = te;
    }
    readBack?.();
    if (cubeRb) void cubeRb.b.mapAsync(GPUMapMode.READ).then(() => {
      const h = new Uint16Array(cubeRb.b.getMappedRange());
      const half = (v: number) => { const e = (v >> 10) & 31, m = v & 1023, s = v & 0x8000 ? -1 : 1; return s * (e === 0 ? m / 16777216 : e === 31 ? 65536 : Math.pow(2, e - 15) * (1 + m / 1024)); };
      let r = 0, g = 0, b = 0, peak = 0, amax = 0, n = 0;
      for (let y = 0; y < 16; y++) for (let x = 0; x < 16; x++) {
        const o = (y * 256 + x * 8) / 2, R = half(h[o]), G = half(h[o + 1]), B = half(h[o + 2]), A = half(h[o + 3]);
        r += R; g += G; b += B; n++; peak = Math.max(peak, R, G, B); amax = Math.max(amax, A);
      }
      log(`[cube] face ${cubeRb.f}: mean ${(r / n).toFixed(3)} ${(g / n).toFixed(3)} ${(b / n).toFixed(3)}, peak ${peak.toFixed(2)}, sun alpha ${amax.toFixed(2)}, faces done ${cubeFaces}`);
      cubeRb.b.destroy();
    });
    for (const t of toFree) t.destroy();
    stats.textures = textures.size;
    return present;
  };

  // Segments that only upload run as soon as they come; one present per display frame.
  let inflight = 0;
  const drain = (mayPresent: boolean) => {
    for (;;) {
      const b = 6 + nextSeg * 8;
      if (Atomics.load(ring, b) !== 1) return;
      if (ring[b + 4] && !mayPresent) return;
      const present = runSegment(nextSeg);
      Atomics.store(ring, b, 0);
      Atomics.notify(ring, b);
      Atomics.add(ring, 3, 1);
      nextSeg = (nextSeg + 1) % segCount;
      if (present) {
        Atomics.add(ring, 5, 1);
        Atomics.notify(ring, 5);
        inflight++;
        void queue.onSubmittedWorkDone().then(() => { inflight--; });
        return;
      }
    }
  };
  const onFrame = () => { requestAnimationFrame(onFrame); if (inflight < 3) drain(true); };
  requestAnimationFrame(onFrame);
  // uploads while no display frame comes, and a page in the background: rAF stops there, the game must not
  let lastPresentCheck = performance.now();
  setInterval(() => {
    const now = performance.now();
    drain(document.hidden && now - lastPresentCheck > 100);
    if (now - lastPresentCheck > 100) lastPresentCheck = now;
  }, 4);
  return stats;
}
