#pragma once

// HLSL shaders for THRASH→DX11 rendering.
// Vertex layout matches THRASH_Vertex reordered to DrawVertex:
//   {x,y,z,rhw} → POSITION (float4), {u,v} → TEXCOORD0, color/specular → COLOR0/1 (R8G8B8A8_UNORM)
// Source: THRASH_Vertex struct from PORSCHE.EXE decompilation (thrash_api.h)
static const char g_shaderSource[] = R"HLSL(

struct VS_INPUT {
    float4 pos      : POSITION;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
    float4 specular : COLOR1;
    float4 rect     : TEXCOORD1;   // G10: per-triangle UV clamp rect, zero = off
};

struct PS_INPUT {
    float4 pos      : SV_POSITION;
    noperspective float3 uvq : TEXCOORD0;
    float4 color    : COLOR0;
    float4 specular : COLOR1;
    nointerpolation float4 rect : TEXCOORD1;
};

cbuffer ScreenConstants : register(b0) {
    float2 screenSize;
    float  alphaRef;     // alpha test threshold (0 = disabled)
    float  depthBias;    // Glide grDepthBiasLevel / 4096 (W-buffer LSB is relative to w)
    float4 clearColor;
    float4 fogColor;     // rgb, w = 1 when fog is on (Glide GR_FOG_WITH_TABLE_ON_Q)
    float4 fogParams;    // x = density, y = 1 / (1 - exp(-density * w63)), z = emissive gain, w = threshold
    float4 emissive;     // x = curve power; y = A2C; z = hard world cutout; w = MAT16 sun sprite in a capture
};

// AA1: with alpha-to-coverage the MSAA coverage follows the alpha, so the
// alpha test becomes a sharp ramp around its threshold (one texel wide in
// screen space) instead of a discard; fully transparent texels still go.
bool AlphaTestDiscards(float a) {
    if (alphaRef <= 0.0) return false;
    // Cutout A2C used to keep every texel with a > 0. GPU GenerateMips then
    // left a ring of a ≈ 0.1 around foliage; those samples mixed with the sky
    // and drew a cyan halo on trees. Discard below the game's own threshold
    // (0x80 for cutouts) and let CoverageAlpha soften only the surviving edge.
    return a < alphaRef;
}
float CoverageAlpha(float a) {
    // Discard alone still blends a surviving bilinear/mip edge with the sky
    // and writes depth before the rear tree. Make survivors opaque as well.
    if (emissive.z > 0.5) return 1.0;
    if (emissive.y <= 0.5 || alphaRef <= 0.0) return a;
    // Soft A2C coverage mixed the surviving edge with the framebuffer.
    // Against the sky that is a cyan rim; it shows on the front tree when
    // another cutout sits behind it. Fully cover samples that passed the test.
    return 1.0;
}

// G12: emissive gain for additive draws in an HDR capture: a threshold +
// power curve on the peak channel, so a sprite's dim halo and edges stay as
// they are and only the bright core is lifted above 1.0.
float3 EmissiveGain(float3 rgb, float3 vcol) {
    float gain = fogParams.z;
    if (gain <= 1.0) return rgb;
    float t = saturate(fogParams.w);
    float peak = max(rgb.r, max(rgb.g, rgb.b));
    float x = saturate((peak - t) / max(1.0 - t, 1e-4));
    // CAR2 fix 9: the opaque gain (power passed negative) lifts only
    // full-bright vertices: unlit lamp panels, not lit road markings.
    if (emissive.x < 0.0) x *= saturate((min(vcol.r, min(vcol.g, vcol.b)) - 0.9) / 0.08);
    return rgb * (1.0 + (gain - 1.0) * pow(x, max(abs(emissive.x), 0.01)));
}

// MAT16: a sun / lens-flare sprite inside a cubemap capture (emissive.w; the
// blend state adds ONE/ONE). The colour goes out premultiplied, and the alpha
// carries the peak of what the sprite adds, so the cube's alpha accumulates
// the sun's luminance next to its colour; PSMaterial takes that share out of
// the road reflection again.
float4 SunMaskOutput(float4 result) {
    if (emissive.w <= 0.5) return result;
    float3 added = result.rgb * result.a;
    return float4(added, max(added.r, max(added.g, added.b)));
}

// dx7z guFogGenerateExp: table[i] = (1 - exp(-d * w_i)) / (1 - exp(-d * w_63)),
// indexed by the per-pixel W = 1 / q, then clamped to 1.
float3 ApplyFog(float3 rgb, float q) {
    if (fogColor.w <= 0.0) return rgb;
    float w = 1.0 / max(q, 1e-6);
    float f = saturate((1.0 - exp(-fogParams.x * w)) * fogParams.y);
    return lerp(rgb, fogColor.rgb, f);
}

// Vertices arrive in screen space (pre-transformed), so we just convert to NDC.
// .bgra swizzle: THRASH_Vertex stores color as D3DCOLOR (BGRA byte order),
// but input layout reads it as R8G8B8A8_UNORM (RGBA), so we swizzle to correct channel mapping.
PS_INPUT VSMain(VS_INPUT input) {
    PS_INPUT output;
    float2 ndc;
    ndc.x = (input.pos.x / screenSize.x) * 2.0 - 1.0;
    ndc.y = 1.0 - (input.pos.y / screenSize.y) * 2.0;
    // dx7z grDepthBiasLevel on the 16-bit floating W-buffer: one level is one
    // mantissa LSB, i.e. dw = level * w / 4096. The game's depth is
    // z = A - B/w (P6 contract, near 1 / far 65536, A ~ B ~ 1), so
    // dz = B/w^2 * dw = level * (A - z) / 4096.
    float z = input.pos.z + depthBias * (1.0 - input.pos.z);
    output.pos = float4(ndc, saturate(z), 1.0);
    // Glide interpolates (u*q, v*q, q), then divides by q per pixel.
    // Keep screen position, Z and iterated colors linear as in the TL input.
    // UI/LFB vertices with zero or invalid RHW use the affine fallback.
    float q = isfinite(input.pos.w) && input.pos.w > 0.0 ? input.pos.w : 1.0;
    output.uvq = float3(input.uv * q, q);
    output.color = input.color.bgra;
    output.specular = input.specular.bgra;
    output.rect = input.rect;
    return output;
}

Texture2D    txDiffuse : register(t0);
SamplerState samLinear : register(s0);

// Textured pixel shader: tex * vertex_color, matching Glide grColorCombine(SCALE_OTHER, ONE, ITERATED)
// Alpha discard is conditional on alphaRef > 0 — without this guard, semi-transparent HUD elements
// (alpha blended with SrcAlpha/InvSrcAlpha) were incorrectly discarded.
// Discovered via dx7z.dll disassembly: state 0x15 is fog, not alpha test.
float4 PSMain(PS_INPUT input) : SV_Target {
    float2 uv = input.uvq.xy / input.uvq.z;
    // G10: magnified 2D art: keep the bilinear footprint inside the
    // triangle's own sub-image (half a texel in) so neighbouring atlas texels
    // never bleed into its edges.
    if (input.rect.z > input.rect.x && input.rect.w > input.rect.y) {
        float w, h;
        txDiffuse.GetDimensions(w, h);
        float2 halfTx = 0.5 / float2(w, h);
        float2 lo = input.rect.xy + halfTx, hi = input.rect.zw - halfTx;
        uv = (lo.x <= hi.x && lo.y <= hi.y) ? clamp(uv, lo, hi) : 0.5 * (input.rect.xy + input.rect.zw);
    }
    // Cutouts map the whole 0..1 quad. WRAP bilinear then samples the
    // opposite edge of the bitmap and draws a 1px strip. Stock PU UVs sit
    // half a texel in. Hard world cutouts (emissive.z = hardCutoutActive
    // from ScreenConstants: binary-keyed bitmaps only) also skip the outer
    // texel: converted cards put opaque crumbs on v=0/1, and anisotropy
    // stretches them into a hairline against the sky. A per-triangle rect
    // (G10 UI art, bar5 tiling cutouts) takes precedence: its clamp above
    // spans the triangle's own UV box, so a tiled quad stays unclamped.
    if (emissive.z > 0.5 && !(input.rect.z > input.rect.x)) {
        float tw, th;
        txDiffuse.GetDimensions(tw, th);
        float inset = (emissive.z > 0.5) ? 1.5 : 0.5;
        float2 halfTex = inset / float2(tw, th);
        uv = clamp(uv, halfTex, 1.0 - halfTex);
    }
    float4 texColor = txDiffuse.Sample(samLinear, uv);
    float4 result = texColor * input.color;
    if (AlphaTestDiscards(result.a))
        discard;
    result.a = CoverageAlpha(result.a);
    result.rgb = ApplyFog(result.rgb, input.uvq.z);
    result.rgb = EmissiveGain(result.rgb, input.color.rgb);   // G12: 1:1 outside HDR captures
    return SunMaskOutput(result);
}

// Vertex-color-only pixel shader, used when no texture is bound (handle=NULL).
float4 PSMainNoTex(PS_INPUT input) : SV_Target {
    if (AlphaTestDiscards(input.color.a))
        discard;
    float4 result = input.color;
    result.a = CoverageAlpha(result.a);
    result.rgb = ApplyFog(result.rgb, input.uvq.z);
    result.rgb = EmissiveGain(result.rgb, input.color.rgb);
    return SunMaskOutput(result);
}

)HLSL";

// R2 car reflection pass. Same vertex layout as the main shaders; the UV
// carries a stereographic camera-space normal written by the game-side hook
// (car_reflection.cpp), interpolated perspective-correctly like any UV. The
// view ray is linear in game pixels, so it is built per vertex from the
// camera's own projection coefficients and interpolated without perspective.
static const char g_reflectShaderSource[] = R"HLSL(

struct VS_INPUT {
    float4 pos      : POSITION;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
    float4 specular : COLOR1;
    float4 rect     : TEXCOORD1;
};

struct PS_INPUT {
    float4 pos      : SV_POSITION;
    noperspective float3 uvq : TEXCOORD0;
    noperspective float3 ray : TEXCOORD1;
    // R3: diffuse UV * q, q, mask enabled (rect.xy, rect.z of the input)
    noperspective float4 mask : TEXCOORD2;
    float4 color    : COLOR0;
};

cbuffer ScreenConstants : register(b0) {
    float2 screenSize;
    float  alphaRef;
    float  depthBias;
    float4 clearColor;
    float4 fogColor;
    float4 fogParams;
    float4 emissive;
};

cbuffer ReflectConstants : register(b1) {
    float4 camRight;
    float4 camUp;
    float4 camForward;
    float4 proj;       // sX, sY, oX, oY
    float4 viewport;   // centerX, centerY, halfW, halfH (game draw pixels)
    float4 params;     // strength, fresnelF0, alphaBlend, excessPass
};

PS_INPUT VSReflect(VS_INPUT input) {
    PS_INPUT output;
    float2 ndc;
    ndc.x = (input.pos.x / screenSize.x) * 2.0 - 1.0;
    ndc.y = 1.0 - (input.pos.y / screenSize.y) * 2.0;
    float z = input.pos.z + depthBias * (1.0 - input.pos.z);
    output.pos = float4(ndc, saturate(z), 1.0);
    float q = isfinite(input.pos.w) && input.pos.w > 0.0 ? input.pos.w : 1.0;
    output.uvq = float3(input.uv * q, q);
    // P6 section 3 inverted: u = sX*x/d + oX, v = -sY*y/d + oY, d = -z.
    float u = (input.pos.x - viewport.x) / viewport.z;
    float v = (input.pos.y - viewport.y) / viewport.w;
    output.ray = float3((u - proj.z) / proj.x, -(v - proj.w) / proj.y, -1.0);
    output.mask = float4(input.rect.xy * q, q, input.rect.z);
    output.color = input.color.bgra;
    return output;
}

Texture2D    txSphereMap   : register(t0);   // the game's .env sphere map, still bound
TextureCube  txEnvironment : register(t1);
Texture2D    txDiffuse     : register(t2);   // R3: the material's diffuse page
Texture2D    txPaintMap    : register(t3);   // CP1: the page's paint map (R = paint)
SamplerState samLinear     : register(s0);

// R3: the diffuse texture's alpha at this pixel (1 where no mask was given).
// A decal quad is transparent where the sticker is not, so the reflection
// pass no longer doubles the body's reflection there. CP1: the paint shader
// reflected on the paint already (camRight.w = paint map bound).
float DiffuseMask(PS_INPUT input) {
    if (input.mask.w < 0.5) return 1.0;
    float2 uv = input.mask.xy / max(input.mask.z, 1e-6);
    float m = txDiffuse.Sample(samLinear, uv).a;
    if (camRight.w > 0.5) m *= 1.0 - txPaintMap.Sample(samLinear, uv).r;
    return m;
}

float4 PSReflect(PS_INPUT input) : SV_Target {
    float2 p = input.uvq.xy / input.uvq.z;
    float d = dot(p, p);
    float3 n = float3(2.0 * p, 1.0 - d) / (d + 1.0);   // -Z pole: facing normals at the origin
    // The game's own sphere-map lookup for this normal (00445520: fast inverse
    // square root of |nz| + 1, half scale, half offset). Its alpha is the
    // designed reflection intensity of this track/band; its color was the
    // static stand-in for the environment.
    float x = abs(n.z) + 1.0;
    float y0 = asfloat(0x5F400000 - (asint(x) >> 1));
    float y = y0 * (1.47 - x * y0 * y0 * 0.47);
    float4 sphere = txSphereMap.Sample(samLinear, n.xy * (y * 0.5) + 0.5);
    float3 v = normalize(input.ray);
    float3 r = reflect(v, n);
    float3 world = r.x * camRight.xyz + r.y * camUp.xyz - r.z * camForward.xyz;
    float4 env = txEnvironment.Sample(samLinear, world);
    env.rgb = min(env.rgb, 64.0);   // G12: HDR cube; emissive draws may exceed 1
    // Same combine as the textured main shader (texture * vertex color), with
    // the cube's color in place of the sphere map's and the sphere map's
    // alpha kept as the blend intensity. Strength and optional Schlick Fresnel
    // scale that intensity through alpha when the pass is alpha-blended
    // (params.z) and through color otherwise.
    float factor = params.x;
    if (params.y > 0.0) {
        float cosTheta = saturate(-dot(v, n));
        float fresnel = params.y + (1.0 - params.y) * pow(1.0 - cosTheta, 5.0);
        factor *= fresnel;
    }
    // G12: the second, additive pass (params.w > 0.5) adds only the HDR
    // excess above 1.0 (emissive light sources) weighted by the same
    // intensity, so a lamp or the sun can exceed the paint's own brightness
    // instead of being capped at intensity * white by the lerp of pass one.
    float mask = DiffuseMask(input);
    if (params.w > 0.5) {
        float3 excess = max(env.rgb - 1.0, 0.0) * sphere.a * input.color.a * factor * mask;
        if (fogColor.w > 0.0) {
            float w = 1.0 / max(input.uvq.z, 1e-6);
            float f = saturate((1.0 - exp(-fogParams.x * w)) * fogParams.y);
            excess *= 1.0 - f;
        }
        return float4(excess, 1.0);
    }
    float4 result = float4(min(env.rgb, 1.0), sphere.a) * input.color;
    if (params.z > 0.5) result.a *= factor * mask;
    else result.rgb *= factor * mask;
    if (alphaRef > 0.0 && result.a < alphaRef)
        discard;
    if (fogColor.w > 0.0) {
        float w = 1.0 / max(input.uvq.z, 1e-6);
        float f = saturate((1.0 - exp(-fogParams.x * w)) * fogParams.y);
        result.rgb = lerp(result.rgb, fogColor.rgb, f);
    }
    return result;
}

)HLSL";

// MB1 camera motion blur (motion_blur.cpp). Full-screen pass over a copy of
// the finished 3D scene: each pixel's camera distance comes from the depth
// buffer (P6 contract z = A - B/d), its world position from the current
// view's basis/projection (the same constants R2 uses), and its previous
// screen position from the previous frame's view. The difference is the
// per-pixel velocity; the colour is averaged along it. Everything closer than
// depthParams.z (the player's car, the bonnet) is kept sharp and excluded from
// the samples of the pixels behind it.
static const char g_motionBlurShaderSource[] = R"HLSL(

cbuffer MotionBlurConstants : register(b1) {
    float4 curRight;      // world-space camera basis rows (xyz)
    float4 curUp;
    float4 curForward;
    float4 curPos;
    float4 prevRight;
    float4 prevUp;
    float4 prevForward;
    float4 prevPos;
    float4 curProj;       // sX, sY, oX, oY
    float4 prevProj;
    float4 curViewport;   // centerX, centerY, halfW, halfH (logical pixels)
    float4 prevViewport;
    float4 depthParams;   // A, B (d = B / (A - z)), near mask start, near mask end
    float4 blurParams;    // strength, max radius (GPU px), sample count, debug mode
    float4 sizes;         // GPU w, GPU h, logical w, logical h
};

Texture2D        txScene   : register(t0);
// AA1: on an MSAA target the depth/stencil are multisampled; sample 0 is
// read (the colour is a resolved copy). MSAA = the sample count (ps_4_0
// needs sized multisampled textures).
#ifdef MSAA
Texture2DMS<float, MSAA> txDepth   : register(t1);
Texture2DMS<uint2, MSAA> txStencil : register(t2);
float LoadDepth(int2 p)   { return txDepth.Load(p, 0); }
uint  LoadStencil(int2 p) { return txStencil.Load(p, 0).g; }
#else
Texture2D<float> txDepth   : register(t1);
Texture2D<uint2> txStencil : register(t2);   // MB2: g = 1 on the player's car
float LoadDepth(int2 p)   { return txDepth.Load(int3(p, 0)); }
uint  LoadStencil(int2 p) { return txStencil.Load(int3(p, 0)).g; }
#endif

float4 VSMotionBlur(uint id : SV_VertexID) : SV_POSITION {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 1, 1);
}

float CameraDistance(float z) {
    return depthParams.y / max(depthParams.x - z, 1e-7);
}

// Camera-space position of a logical pixel at camera distance d (P6 section 3
// inverted, as in the R2 vertex shader). Forward is -Z.
float3 CameraPosition(float2 lp, float d) {
    float u = (lp.x - curViewport.x) / curViewport.z;
    float v = (lp.y - curViewport.y) / curViewport.w;
    float3 ray = float3((u - curProj.z) / curProj.x, -(v - curProj.w) / curProj.y, -1.0);
    return ray * d;
}

// Logical pixel of a world point in the previous view; w = 0 when it is
// behind the previous camera.
float3 PreviousPixel(float3 world) {
    float3 rel = world - prevPos.xyz;
    float x = dot(rel, prevRight.xyz);
    float y = dot(rel, prevUp.xyz);
    float d = dot(rel, prevForward.xyz);
    if (d <= 1e-3) return float3(0, 0, 0);
    float u = prevProj.x * x / d + prevProj.z;
    float v = -prevProj.y * y / d + prevProj.w;
    return float3(prevViewport.x + prevViewport.z * u, prevViewport.y + prevViewport.w * v, 1.0);
}

// 0 on the sharp foreground (the player's car by stencil, anything nearer
// than the distance mask), 1 on the scenery.
float ForegroundMask(int2 pix, float d) {
    if (LoadStencil(pix) != 0) return 0.0;
    return smoothstep(depthParams.z, depthParams.w, d);
}

// Interleaved gradient noise: a per-pixel offset of the tap positions hides
// the banding of a small sample count.
float Noise(float2 p) {
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

float4 PSMotionBlur(float4 pos : SV_POSITION) : SV_Target {
    float2 gpuSize = sizes.xy;
    float2 toLogical = sizes.zw / gpuSize;
    int2 pix = int2(pos.xy);
    float4 center = txScene.Load(int3(pix, 0));

    float z = LoadDepth(pix);
    float d = CameraDistance(z);
    float mask = ForegroundMask(pix, d);
    if (mask <= 0.0) return blurParams.w > 0.5 ? float4(0, 0, 1, 1) : center;

    float2 lp = pos.xy * toLogical;
    float3 posCam = CameraPosition(lp, d);
    float3 world = curPos.xyz + posCam.x * curRight.xyz + posCam.y * curUp.xyz - posCam.z * curForward.xyz;
    float3 prev = PreviousPixel(world);
    if (prev.z <= 0.0) return center;

    // Motion of this surface point from the previous frame to this one, in
    // GPU pixels. The blur spans that path (a shutter open for the frame).
    float2 velocity = (lp - prev.xy) / toLogical * blurParams.x * mask;
    float len = length(velocity);
    if (len < 0.5) return center;
    if (len > blurParams.y) velocity *= blurParams.y / len;

    if (blurParams.w > 0.5) {
        // Debug: velocity as colour (red = +x, green = +y, 1 = max radius).
        return float4(0.5 + 0.5 * velocity / blurParams.y, 0.0, 1.0);
    }

    int samples = (int)blurParams.z;
    float jitter = Noise(pos.xy) - 0.5;
    float3 sum = center.rgb;
    float weight = 1.0;
    for (int i = 0; i < samples; i++) {
        float t = ((i + 0.5 + jitter) / samples) - 0.5;
        if (abs(t) < 1e-4) continue;
        // Colour and depth of one and the same texel (a filtered colour tap
        // would mix in the neighbour the depth test rejected).
        int2 spix = clamp(int2(pos.xy + velocity * t), int2(0, 0), int2(gpuSize) - 1);
        float sd = CameraDistance(LoadDepth(spix));
        // A sample on the sharp foreground (the car) must not bleed into the
        // scenery behind it; foreground pixels themselves take every sample.
        float w = saturate(ForegroundMask(spix, sd) + 1.0 - mask);
        sum += txScene.Load(int3(spix, 0)).rgb * w;
        weight += w;
    }
    return float4(sum / weight, center.a);
}

)HLSL";

// MAT7 track material shading. The vertex shader is the same screen→NDC
// transform as VSMain, plus a noperspective camera-space ray for P
// reconstruction (as in the R2 reflect shader). The pixel shader calls
// mat_shade from material_core.inc (embedded by gen_material_shader.py).
// The prelude and core are prepended at runtime from material_shader_gen.h
// (or from disk when shader_dir is set); only the wrapper code is here.
static const char g_materialShaderWrapper[] = R"HLSL(

struct VS_INPUT {
    float4 pos      : POSITION;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
    float4 specular : COLOR1;
    float4 rect     : TEXCOORD1;
};

struct PS_INPUT {
    float4 pos      : SV_POSITION;
    noperspective float3 uvq : TEXCOORD0;
    float4 color    : COLOR0;
    noperspective float3 ray : TEXCOORD2;
    float4 nrm      : TEXCOORD3;   // MAT11: smooth camera-space normal, w = 1 when valid
};

cbuffer ScreenConstants : register(b0) {
    float2 screenSize;
    float  alphaRef;
    float  depthBias;
    float4 clearColor;
    float4 fogColor;
    float4 fogParams;
    float4 emissive;
};

cbuffer MaterialConstants : register(b2) {
    float4 matCamRight;
    float4 matCamUp;
    float4 matCamForward;
    float4 matProj;       // sX, sY, oX, oY
    float4 matViewport;   // centerX, centerY, halfW, halfH
    float4 matBaseColor;
    float4 matParams1;    // metallic, roughness, occlusion, specular
    float4 matParams2;    // reflection, normalScale, bumpDiffuse, fogResist
    float4 matEmissive;   // r, g, b, unused
    float4 matUvScale;    // x, y, unused, unused
    float4 matSunDir;     // camera-space, normalised
    float4 matSunColour;
    float4 matFlags;      // maxMip, flipY, hasNormal, hasOrm
    float4 matPlanar;     // MAT14: 1/targetW, 1/targetH, enabled, mip scale (max mip * blur)
    float4 matPlanarN;    // MAT14: world up in camera space, w = distortion
    float4 matPlanarD;    // MAT14: x = gain of the car layer; MAT14d: y = sun scale (cube alpha mask)
    float4 matSurface;    // MAT19: gloss min, gloss max, surface map flag, debug view
    float4 matCar;        // CAR1: car material, zone map bound; CAR2: coat mask, sun mask in the cube (fix 7)
    float4 matZoneAlpha[2]; // CAR1: the 8 zone alpha values (0..1) of the material's classes
    float4 matTint[8];    // CAR1: paint tint per zone class (rgb); [0].w = sun_fog distance (CAR2 fix 7), [1].w = light_gain (fix 10)
                          // CP1: [2].x = paint map bound (t8), [2].y = dark_matte (0 off .. 1 full), [2].z = dark_power
    float4 matPaint0;     // CAR2: paint_metallic, grazing (0 = 1), flake_scale, flake_intensity
    float4 matPaint1;     // CAR2: metal_silver, coat, coat_gloss, orange_peel
    float4 matPaint2;     // CAR2: sun_glow, sun_glow_angle, sun_glow_sat, view_light
    float4 matPaint3;     // CAR2: view_dark, env_diffuse, env_glow, env_hdr (fix 8)
};

PS_INPUT VSMaterial(VS_INPUT input) {
    PS_INPUT output;
    float2 ndc;
    ndc.x = (input.pos.x / screenSize.x) * 2.0 - 1.0;
    ndc.y = 1.0 - (input.pos.y / screenSize.y) * 2.0;
    float z = input.pos.z + depthBias * (1.0 - input.pos.z);
    output.pos = float4(ndc, saturate(z), 1.0);
    float q = isfinite(input.pos.w) && input.pos.w > 0.0 ? input.pos.w : 1.0;
    output.uvq = float3(input.uv * q, q);
    output.color = input.color.bgra;
    // Camera-space ray from NDC + projection inverse.
    output.ray = float3((ndc.x - matProj.z) / matProj.x, (ndc.y + matProj.w) / matProj.y, -1.0);
    // CP1: pos.w is 1, so the rasterizer interpolates in screen space; the
    // normal is taken x q (= rhw) like the UVs and renormalised in the PS, i.e.
    // perspective-correct. Affine normals bent the paint / reflection of large
    // polygons that reach behind the camera (depth changes tens of times).
    output.nrm = float4(input.rect.xyz * q, input.rect.w);
    return output;
}

Texture2D    txDiffuse     : register(t0);
TextureCube  txEnvironment : register(t1);
SamplerState samLinear     : register(s0);
Texture2D    txNormal      : register(t3);
Texture2D    txOrm         : register(t4);
Texture2D    txPlanar      : register(t5);   // MAT14: car mirror layer (premultiplied)
SamplerState samClamp      : register(s1);
Texture2D<float> txPlanarDepth : register(t6);   // MAT14: its depth (game z)
Texture2D<float> txZones       : register(t7);   // CAR1: paint zone map (source alpha, R8)
Texture2D<float2> txPaint      : register(t8);   // CP1: paint map (R = body / stripe paint, G = keep x R)

// MAT14: cars mirrored in the road. The extra car pass drew every car
// mirrored in the plane of the road under it, with the main camera, so a road
// pixel finds the reflection of the cars at its own screen position; the
// layer is black with alpha 0 where no car is (premultiplied). The mirror
// image of an object above the road lies behind the road surface: a layer
// pixel closer than the road belongs to another surface (the lower leg of a
// switchback) and is skipped. Game depth: z = A - B/w, range 1..65536 (MB1).
static const float kPlanarDepthA = 65536.0 / 65535.0;
static const float kPlanarDepthB = 65536.0 / 65535.0;

float PlanarSceneDepth(float2 uv) {
    float2 size;
    txPlanarDepth.GetDimensions(size.x, size.y);
    int2 px = int2(clamp(uv * size, float2(0.0, 0.0), size - 1.0));
    float z = txPlanarDepth.Load(int3(px, 0));
    return kPlanarDepthB / max(kPlanarDepthA - z, 1e-7);
}

// CAR2: environment along the reflection of V about Nrefl, with the MAT16
// sun-mask share taken down to sun_reflection (same as the track path).
float3 MatSampleEnvK(float3 Nrefl, float3 V, float mipLevel, float sunKeep) {
    float3 R = reflect(-V, Nrefl);
    float3 world = R.x * matCamRight.xyz + R.y * matCamUp.xyz - R.z * matCamForward.xyz;
    float4 env = txEnvironment.SampleLevel(samLinear, world, mipLevel);
    float envPeak = max(env.r, max(env.g, env.b));
    float sunShare = envPeak > 1e-5 ? saturate(env.a / envPeak) : 0.0;
    return min(env.rgb * (1.0 - (1.0 - sunKeep) * sunShare), 64.0);
}
float3 MatSampleEnv(float3 Nrefl, float3 V, float mipLevel) { return MatSampleEnvK(Nrefl, V, mipLevel, matPlanarD.y); }

// CAR2 fix 8: the HDR excess of a cube sample (lamps, sky peaks) scaled for cars.
float3 CarEnvGain(float3 c, float gain) {
    return min(c, 1.0) + max(c - 1.0, 0.0) * gain;
}
// CAR2 fix 8: the cube around a camera-space direction as a light source:
// the mip whose texel spans the cone; the sun sprite is masked out (it has
// its own analytic lobe).
float3 MatSampleEnvCone(float3 dirCam, float coneDeg, float hdrGain) {
    float3 world = dirCam.x * matCamRight.xyz + dirCam.y * matCamUp.xyz - dirCam.z * matCamForward.xyz;
    float size = exp2(matFlags.x);
    float mip = clamp(log2(max(size * coneDeg / 90.0, 1.0)), 0.0, matFlags.x);
    float4 env = txEnvironment.SampleLevel(samLinear, world, mip);
    float envPeak = max(env.r, max(env.g, env.b));
    float sunShare = envPeak > 1e-5 ? saturate(env.a / envPeak) : 0.0;
    return CarEnvGain(min(env.rgb * (1.0 - sunShare), 64.0), hdrGain);
}
// CAR2 fix 7: how much of the sun disc the capture sees in the sun
// direction, from the car's own position. Inside a capture the opaque
// track / car draws clear the alpha they cover and the sun sprite adds its
// luminance (~1.5), so the alpha at the sun's direction is the disc where
// nothing stands in front of it. mip = 32 texels per face (~3 deg).
float MatSunVisibility() {
    if (matCar.w <= 0.5) return 1.0;
    float3 world = matSunDir.x * matCamRight.xyz + matSunDir.y * matCamUp.xyz - matSunDir.z * matCamForward.xyz;
    float mip = clamp(matFlags.x - 5.0, 0.0, matFlags.x);
    float4 env = txEnvironment.SampleLevel(samLinear, world, mip);
    // A dim disc is a dim sun: Industrial draws the sprite at night at 0.4,
    // Coastal at sunset reads 3.0 (additive gain), so peak 1.0 = full sun.
    float peak = max(env.r, max(env.g, env.b));
    float vis = saturate(env.a / 0.5) * saturate(peak - 0.5);
    // The sky pass draws the disc unfogged; in the game's fog the sun is as
    // hidden as an object matTint[0].w units away ([car_tune] sun_fog).
    if (fogColor.w > 0.0 && matTint[0].w > 0.0) {
        float f = saturate((1.0 - exp(-fogParams.x * matTint[0].w)) * fogParams.y);
        vis *= 1.0 - f;
    }
    return vis;
}

bool MatAlphaTestDiscards(float a) {
    if (alphaRef <= 0.0) return false;
    return a < alphaRef;
}
float MatCoverageAlpha(float a) {
    if (emissive.z > 0.5) return 1.0;
    if (emissive.y <= 0.5 || alphaRef <= 0.0) return a;
    return 1.0;
}

float3 MatApplyFog(float3 rgb, float q) {
    if (fogColor.w <= 0.0) return rgb;
    float w = 1.0 / max(q, 1e-6);
    float f = saturate((1.0 - exp(-fogParams.x * w)) * fogParams.y);
    return lerp(rgb, fogColor.rgb, f);
}

float4 PSMaterial(PS_INPUT input) : SV_Target {
    float2 uv = input.uvq.xy / input.uvq.z;
    float4 albedo = txDiffuse.Sample(samLinear, uv);
    // CAR1: the game bakes the paint into its 16-bit page per zone (the source
    // image alpha); the pack's albedo is the untinted page, so tint it here by
    // the zone class of the texel (point lookup: classes must not blend).
    float3 dbgZone = float3(0.0, 0.0, 0.0);
    if (matCar.y > 0.5) {
        float2 zsize;
        txZones.GetDimensions(zsize.x, zsize.y);
        float2 zuv = frac(uv);
        int2 zp = int2(clamp(zuv * zsize, float2(0.0, 0.0), zsize - 1.0));
        float zone = txZones.Load(int3(zp, 0)) * 255.0;
        float3 tint = float3(1.0, 1.0, 1.0);
        float zalpha[8] = {matZoneAlpha[0].x, matZoneAlpha[0].y, matZoneAlpha[0].z, matZoneAlpha[0].w,
                           matZoneAlpha[1].x, matZoneAlpha[1].y, matZoneAlpha[1].z, matZoneAlpha[1].w};
        [unroll] for (int k = 1; k < 8; ++k) {
            if (zalpha[k] > 0.0 && abs(zone - zalpha[k] * 255.0) < 0.5) tint = matTint[k].rgb;
        }
        albedo.rgb *= tint;
        dbgZone = float3(zone / 255.0, tint.g, tint.b);
    }
    // CP1: where the game painted the page (paint) and how much of the shine a
    // texel keeps: its source red over the band's panel level, so door seams
    // and grilles (darker than the panel) lose gloss linearly, black = matte.
    float paintMask = 1.0, keep = 1.0;
    if (matTint[2].x > 0.5) {
        float2 pk = txPaint.Sample(samLinear, uv);
        paintMask = pk.r;
        keep = pk.r > 1e-3 ? saturate(pk.g / pk.r) : 1.0;
        keep = pow(keep, matTint[2].z > 0.05 ? matTint[2].z : 1.0);   // dark_power: greys fade faster
        keep = saturate(1.0 - matTint[2].y * (1.0 - keep));
        dbgZone = float3(paintMask, keep, 0.0);
    }
    float4 result_color;
    result_color.a = albedo.a * matBaseColor.a * input.color.a;
    if (MatAlphaTestDiscards(result_color.a))
        discard;

    // Camera-space position from ray and depth.
    float d = 1.0 / max(input.uvq.z, 1e-6);
    float3 P = input.ray * d;
    float3 V = -normalize(P);

    // Faceted normal from screen-space derivatives.
    float3 ddP1 = ddx(P);
    float3 ddP2 = ddy(P);
    float3 Ngeo = normalize(cross(ddP1, ddP2));
    Ngeo = mat_orient_normal(Ngeo, P);
    // MAT11: smooth vertex normal welded on the CPU (rect.w = 1), else faceted.
    // CP1: a smooth normal of the other side (dot with the face < 0) flips; one
    // of this side that still looks away from the eye (grazing faces close to
    // the camera, the screen edges of a wide view) is turned towards it until
    // N.V = 0.05. The paint shades N.V <= 0 black (full Fresnel: no diffuse,
    // the reflection from under the car); flipping it mirrored the normal.
    if (input.nrm.w > 0.5) {
        float3 ns = input.nrm.xyz;
        float l2 = dot(ns, ns);   // x q (rhw): far vertices are short, only the direction counts
        if (l2 > 1e-24) {
            ns *= rsqrt(l2);
            if (dot(ns, Ngeo) < 0.0) ns = -ns;
            float nv = dot(ns, V);
            if (nv < 0.05) ns = normalize(ns + V * (0.05 - nv));
            Ngeo = ns;
        }
    }

    float3 N = Ngeo;
    bool hasNormal = matFlags.z > 0.5;
    if (hasNormal) {
        float2 uv_detail = uv * matUvScale.xy;
        float2 ddUV1 = ddx(uv_detail);
        float2 ddUV2 = ddy(uv_detail);
        MAT3 TBN = mat_cotangent_frame(Ngeo, ddP1, ddP2, ddUV1, ddUV2);
        float3 n_ts = txNormal.Sample(samLinear, uv_detail).rgb;
        N = mat_perturb_normal(TBN, n_ts, matParams2.y, (int)matFlags.y);
    }

    float ao_sample = 1.0;
    float metal_map = matParams1.x;
    float rough_map = matParams1.y;
    float coat_map = 1.0;   // CAR2: surface.A when the material says so
    bool hasOrm = matFlags.w > 0.5;
    if (hasOrm) {
        float4 orm = txOrm.Sample(samLinear, uv * matUvScale.xy);
        ao_sample = orm.r;
        if (matCar.z > 0.5) coat_map = orm.a;
        if (matSurface.z > 0.5) {   // MAT19 surface map: G = gloss mask
            // perceptual: roughness = (1 - gloss)^2, gloss 0.5 = rough 0.25
            float gl = 1.0 - lerp(matSurface.x, matSurface.y, orm.g);
            rough_map = gl * gl;
        }
        else
            rough_map *= orm.g;
        metal_map *= orm.b;
    }

    // Environment reflection from R1 cubemap.
    float3 R = reflect(-V, N);
    float3 world = R.x * matCamRight.xyz + R.y * matCamUp.xyz - R.z * matCamForward.xyz;
    float mipLevel = rough_map * matFlags.x;
    float4 env = txEnvironment.SampleLevel(samLinear, world, mipLevel);
    // MAT16: the cube's alpha is the luminance the sun / lens-flare sprites
    // added to this texel (captured through the MAT16 hooks). Take that share
    // of the colour down to sun_reflection, and the analytic sun highlight
    // with it, so a wet road does not glare while the cars keep their sun.
    float envPeak = max(env.r, max(env.g, env.b));
    float sunShare = envPeak > 1e-5 ? saturate(env.a / envPeak) : 0.0;
    float3 env_col = min(env.rgb * (1.0 - (1.0 - matPlanarD.y) * sunShare), 64.0);
    float dbgCover = 0.0;   // MAT19 debug view: car layer coverage
    float3 sunColour = matSunColour.rgb * matPlanarD.y;

    // MAT14: up-facing surfaces take the car mirror layer at their own screen
    // position; the normal map ripples the lookup.
    if (matPlanar.z > 0.5 && matCar.x < 0.5) {   // CAR1: not on car materials
        float wUp = saturate((dot(Ngeo, matPlanarN.xyz) - 0.5) * 4.0);
        if (wUp > 0.0) {
            float2 puv = input.pos.xy * matPlanar.xy;
            float3 dn = N - Ngeo;
            puv = saturate(puv + clamp(float2(dn.x, -dn.y) * matPlanarN.w, -0.05, 0.05));
            float4 car = txPlanar.SampleLevel(samClamp, puv, rough_map * matPlanar.w);
            float behind = saturate((PlanarSceneDepth(puv) - d + 1.0) * 2.0);
            float cov = saturate(car.a) * wUp * behind;
            dbgCover = cov;
            env_col = env_col * (1.0 - cov) + min(car.rgb * matPlanarD.x, 64.0) * wUp * behind;
        }
    }

    float4 shaded;
    if (matCar.x > 0.5) {
        // CAR2: car paint. Base lobe (paint metallic + chrome map, procedural
        // flakes) under a clear coat with an orange-peel ripple; the frame for
        // the tilts comes from the page UV derivatives.
        // CP1: keep scales everything shiny and pushes the roughness to 1
        // (as the car editor's paint depth with depth_matte 1).
        float chrome = metal_map * keep;
        float metal_c = saturate(matPaint0.x * keep + chrome);
        float coat_c = saturate(matPaint1.y * coat_map * (1.0 - chrome)) * keep;
        rough_map = lerp(1.0, rough_map, keep);
        float cg = 1.0 - matPaint1.z;
        float rough_coat = max(cg * cg, 0.02);
        float2 dUV1 = ddx(uv), dUV2 = ddy(uv);
        MAT3 TBNc = mat_cotangent_frame(N, ddP1, ddP2, dUV1, dUV2);
        float flake_fade = saturate(2.0 - length(fwidth(uv)) * matPaint0.z);
        float3 Nb = mat_car_base_normal(TBNc, N, uv, matPaint0.z, matPaint0.w * keep, flake_fade);
        float3 Nc = mat_car_coat_normal(TBNc, N, uv, 48.0, matPaint1.w);
        // CAR2 fix 7/8: the sun through the capture's sun mask; the cube as a light.
        float sunVis = MatSunVisibility();
        float3 sunCar = sunColour * sunVis;
        float3 env_b = CarEnvGain(MatSampleEnvK(Nb, V, rough_map * matFlags.x, matPlanarD.y * sunVis), matPaint3.w);
        float3 env_c = CarEnvGain(MatSampleEnvK(Nc, V, rough_coat * matFlags.x, matPlanarD.y * sunVis), matPaint3.w);
        float3 env_irr = MatSampleEnvCone(N, 60.0, matPaint3.w);
        float3 env_glow = MatSampleEnvCone(N, matPaint2.y > 0.0 ? matPaint2.y : 45.0, matPaint3.w);
        shaded = mat_shade_car(Nb, Nc, N, Ngeo, V, matSunDir.xyz,
                               albedo, matBaseColor, input.color.rgb, sunCar, env_b, env_c,
                               metal_c, rough_map, matParams1.z, ao_sample,
                               matParams1.w * keep, matParams2.x * keep, coat_c, rough_coat, matEmissive.rgb, matPaint1.x,
                               matPaint0.y > 0.0 ? matPaint0.y : 1.0, matPaint2.x * keep, matPaint2.y, matPaint2.z, matPaint2.w * keep, matPaint3.x,
                               env_irr, env_glow, matPaint3.y * keep, matPaint3.z * keep,
                               1.0 + ((matTint[1].w > 0.0 ? matTint[1].w : 1.0) - 1.0) * keep);
        // CP1: texels the game does not paint keep the stock look (texture x
        // the game light); its reflection pass draws on them, masked off the paint.
        if (matTint[2].x > 0.5)
            shaded.rgb = lerp(albedo.rgb * input.color.rgb, shaded.rgb, paintMask);
    } else {
        shaded = mat_shade(
            N, Ngeo, V, matSunDir.xyz,
            albedo, matBaseColor, input.color.rgb, sunColour, env_col,
            metal_map, rough_map, matParams1.z, ao_sample,
            matParams1.w, matParams2.x,
            matParams2.z, matParams2.w,
            matEmissive.rgb
        );
    }

    // MAT19 debug view ([materials] debug_view): what the reflection gets.
    // 1 = environment after sun mask and car layer, 2 = R sun-mask share,
    // G car-layer cover, 3 = roughness, 4 = reflection the shading added.
    int dbgView = (int)(matSurface.w + 0.5);
    // CP1 diagnostics (debug_view 8): magenta = NaN / Inf in the shading,
    // cyan = a car paint texel shaded black where the stock look is not.
    if (dbgView == 8) {
        if (any(isnan(shaded.rgb)) || any(isinf(shaded.rgb)) || any(isnan(N)))
            return float4(1.0, 0.0, 1.0, 1.0);
        float3 stock = albedo.rgb * input.color.rgb;
        if (matCar.x > 0.5 && paintMask > 0.5 && max(shaded.r, max(shaded.g, shaded.b)) < 0.004 &&
            max(stock.r, max(stock.g, stock.b)) > 0.03)
            return float4(0.0, 1.0, 1.0, 1.0);
        dbgView = 0;
    }
    if (dbgView > 0) {
        float3 d = env_col;
        if (dbgView == 2) d = float3(sunShare, dbgCover, 0.0);
        else if (dbgView == 3) d = rough_map.xxx;
        else if (dbgView == 5) d = N * 0.5 + 0.5;          // CAR1: shading normal
        else if (dbgView == 6) d = dbgZone;                 // CAR1: zone / tint
        else if (dbgView == 7) {                             // CAR1: quarters: albedo | normal | env | vertex colour
            float fx = input.pos.x / screenSize.x;
            if (fx < 0.25) d = albedo.rgb;
            else if (fx < 0.5) d = N * 0.5 + 0.5;
            else if (fx < 0.75) d = env_col;
            else d = input.color.rgb;
        }
        else if (dbgView == 4) {
            float4 dry = mat_shade_ex(N, Ngeo, V, matSunDir.xyz, albedo, matBaseColor, input.color.rgb,
                                      sunColour, env_col, metal_map, rough_map, matParams1.z, ao_sample,
                                      matParams1.w, 0.0, matParams2.z, matParams2.w, matEmissive.rgb, matCar.x);
            d = max(shaded.rgb - dry.rgb, 0.0);
        }
        return float4(d, result_color.a);
    }

    float3 rgb = shaded.rgb;
    float3 unfogged = rgb;
    rgb = MatApplyFog(rgb, input.uvq.z);
    // Emissive portion resists fog.
    float fogResist = matParams2.w;
    if (fogResist > 0.0 && any(matEmissive.rgb > 0.0)) {
        float3 emPortion = matEmissive.rgb * fogResist;
        rgb += emPortion * (unfogged - rgb);
    }

    result_color.rgb = rgb;
    result_color.a = MatCoverageAlpha(result_color.a);
    return result_color;
}

)HLSL";

// A draw-based clear honors the current scissor (unlike D3D11 Clear*View).
// It does not consume game vertices, textures, alphaRef or other material state.
static const char g_clearShaderSource[] = R"HLSL(
cbuffer ScreenConstants : register(b0) {
    float2 screenSize;
    float  alphaRef;
    float  depthBias;
    float4 clearColor;
    float4 fogColor;
    float4 fogParams;
    float4 emissive;
};
float4 VSClear(uint id : SV_VertexID) : SV_POSITION {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 1, 1);
}
float4 PSClear() : SV_Target {
    return clearColor;
}
)HLSL";
