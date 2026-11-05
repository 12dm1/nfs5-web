// nfs5-web WEB3: replays the frames runtime/web/r_web.c records. All drawing goes to one offscreen colour + depth pair,
// which a present blits to the canvas: a frame may take several segments.
//
// Ring (bytes): magic 0, seg_count 4, published 8, consumed 12, presents_pub 16, presents_done 20, segments 24 (32 bytes each:
// ready, cmd_words, vtx_bytes, up_bytes, has_present, cmd, vtx, up).
const C = { VIDEO: 1, TEX_CREATE: 2, TEX_DESTROY: 3, TEX_WRITE: 4, TEX_GENMIPS: 5, SCISSOR: 6, PIPE: 7, BIND: 8, UNIFORM: 9, DRAW: 10, PRESENT: 11, READRECT: 12 };
const VSIZE = 48, VERTEX_BUFFER = 24 << 20, UBO_ALIGN = 256, UBO_SLOTS = 65536 / 256 * 64;

const SHADER = /* wgsl */`
struct U { screen: vec2f, alphaRef: f32, depthBias: f32, fogColor: vec4f, fogDensity: f32, fogScale: f32, hard: f32, lodBias: f32 };
@group(0) @binding(0) var<uniform> u: U;
@group(1) @binding(0) var tx: texture_2d<f32>;
@group(1) @binding(1) var sm: sampler;
struct VI { @location(0) pos: vec4f, @location(1) uv: vec2f, @location(2) color: vec4f, @location(3) spec: vec4f, @location(4) rect: vec4f };
struct VO { @builtin(position) pos: vec4f, @location(0) uvq: vec3f, @location(1) color: vec4f, @interpolate(flat) @location(2) rect: vec4f };
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
  return o;
}
fn fog(rgb: vec3f, q: f32) -> vec3f {
  if (u.fogColor.w <= 0.0) { return rgb; }
  let w = 1.0 / max(q, 1e-6);
  let f = clamp((1.0 - exp(-u.fogDensity * w)) * u.fogScale, 0.0, 1.0);
  return mix(rgb, u.fogColor.rgb, f);
}
@fragment fn fs(i: VO) -> @location(0) vec4f {
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
  const cap = Math.min(2160, Number(q.get('rh') ?? 2160) || 2160);
  const h = Math.max(720, Math.min(cap, Math.round(Math.min(ph, pw / aspect)))) & ~1;
  return [Math.round(h * aspect / 8) * 8, h];
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
  const msaaParam = Number(new URLSearchParams(location.search).get('msaa') ?? 4);
  const SAMPLES = msaaParam === 1 ? 1 : 4;
  const makeTargets = () => {
    const colorTex = device.createTexture({ size: [W, H], format: 'rgba8unorm', usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_SRC });
    const msTex = SAMPLES > 1 ? device.createTexture({ size: [W, H], format: 'rgba8unorm', sampleCount: SAMPLES, usage: GPUTextureUsage.RENDER_ATTACHMENT }) : null;
    const depthTex = device.createTexture({ size: [W, H], format: 'depth24plus', sampleCount: SAMPLES, usage: GPUTextureUsage.RENDER_ATTACHMENT });
    const colorView = colorTex.createView();
    return { colorTex, msTex, depthTex, colorView, depthView: depthTex.createView(), drawView: msTex ? msTex.createView() : colorView };
  };
  let { colorTex, msTex, depthTex, colorView, depthView, drawView } = makeTargets();
  log(`[gpu] MSAA x${SAMPLES}`);
  // the multisampled target into colorTex: an empty pass whose end resolves
  const resolve = (enc: GPUCommandEncoder) => {
    if (!msTex) return;
    enc.beginRenderPass({ colorAttachments: [{ view: drawView, resolveTarget: colorView, loadOp: 'load', storeOp: 'store' }] }).end();
  };

  const module = device.createShaderModule({ code: SHADER });
  const blitModule = device.createShaderModule({ code: BLIT });
  const uboLayout = device.createBindGroupLayout({ entries: [{ binding: 0, visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT, buffer: { type: 'uniform', hasDynamicOffset: true } }] });
  const texLayout = device.createBindGroupLayout({ entries: [
    { binding: 0, visibility: GPUShaderStage.FRAGMENT, texture: {} }, { binding: 1, visibility: GPUShaderStage.FRAGMENT, sampler: {} }] });
  const pipeLayout = device.createPipelineLayout({ bindGroupLayouts: [uboLayout, texLayout] });
  const vbuf = device.createBuffer({ size: VERTEX_BUFFER, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
  const ubo = device.createBuffer({ size: UBO_SLOTS * UBO_ALIGN, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
  const uboGroup = device.createBindGroup({ layout: uboLayout, entries: [{ binding: 0, resource: { buffer: ubo, size: 64 } }] });
  const uboData = new ArrayBuffer(UBO_SLOTS * UBO_ALIGN);
  const uboF = new Float32Array(uboData);

  const pipes = new Map<number, GPURenderPipeline>();
  const pipeline = (key: number) => {
    let p = pipes.get(key);
    if (p) return p;
    const blend = BLEND[key & 7], zcmp = (key >> 3) & 7, zwrite = (key >> 6) & 1, cull = (key >> 7) & 3, topo = (key >> 9) & 3, cw = (key >> 11) & 1;
    p = device.createRenderPipeline({
      layout: pipeLayout,
      vertex: { module, entryPoint: 'vs', buffers: [{ arrayStride: VSIZE, attributes: [
        { shaderLocation: 0, offset: 0, format: 'float32x4' }, { shaderLocation: 1, offset: 16, format: 'float32x2' },
        { shaderLocation: 2, offset: 24, format: 'unorm8x4' }, { shaderLocation: 3, offset: 28, format: 'unorm8x4' },
        { shaderLocation: 4, offset: 32, format: 'float32x4' }] }] },
      fragment: { module, entryPoint: 'fs', targets: [{ format: 'rgba8unorm', blend, writeMask: cw ? GPUColorWrite.ALL : 0 }] },
      primitive: { topology: TOPO[topo], cullMode: topo === 0 ? CULL[cull] : 'none', frontFace: 'cw' },
      depthStencil: { format: 'depth24plus', depthWriteEnabled: !!zwrite, depthCompare: DEPTH[zcmp] },
      multisample: { count: SAMPLES },
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
  interface T { tex: GPUTexture; groups: Map<number, GPUBindGroup>; levels: number }
  const textures = new Map<number, T>();
  const whiteT: T = { tex: white, groups: new Map(), levels: 1 };
  const group = (t: T, key: number) => {
    let g = t.groups.get(key);
    if (!g) { g = device.createBindGroup({ layout: texLayout, entries: [{ binding: 0, resource: t.tex.createView() }, { binding: 1, resource: sampler(key) }] }); t.groups.set(key, g); }
    return g;
  };

  // the texture's lower levels from level 0 (a partial update of a streamed texture)
  const genSampler = device.createSampler({ magFilter: 'linear', minFilter: 'linear' });
  const blitLayout = device.createBindGroupLayout({ entries: [
    { binding: 0, visibility: GPUShaderStage.FRAGMENT, texture: {} }, { binding: 1, visibility: GPUShaderStage.FRAGMENT, sampler: {} }] });
  const blitPipeLayout = device.createPipelineLayout({ bindGroupLayouts: [blitLayout] });
  const blitPipe = (format: GPUTextureFormat) => device.createRenderPipeline({ layout: blitPipeLayout,
    vertex: { module: blitModule, entryPoint: 'vs' }, fragment: { module: blitModule, entryPoint: 'fs', targets: [{ format }] }, primitive: { topology: 'triangle-list' } });
  const genPipe = blitPipe('rgba8unorm'), presentPipe = blitPipe(canvasFormat);
  const makePresentGroup = () => device.createBindGroup({ layout: blitLayout, entries: [{ binding: 0, resource: colorView }, { binding: 1, resource: genSampler }] });
  let presentGroup = makePresentGroup();
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
  const putWindow = () => { const [w, h] = raceMode(); Atomics.store(ring, 30, w); Atomics.store(ring, 31, h); setShape(lw, lh); };
  putWindow();
  addEventListener('resize', putWindow);

  // a segment: its uploads first, then its draws in one pass
  let frameJs = 0, lastPresentAt = 0;
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
    let curTex: T = whiteT, curSampler = 0, bindDirty = true, pipeKey = -1;
    let sc = [0, 0, lw, lh], scDirty = true, uboOff = 0;
    const begin = () => {
      if (pass) return pass;
      pass = enc.beginRenderPass({
        colorAttachments: [{ view: drawView, loadOp: 'load', storeOp: 'store' }],
        depthStencilAttachment: { view: depthView, depthLoadOp: 'load', depthStoreOp: 'store' },
      });
      pass.setVertexBuffer(0, vbuf);
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
          // a race's mode is the render size: the race draws pixel for pixel. The menus keep the targets, they are scaled.
          if (nw > 640 && (nw !== W || nh !== H)) {
            if (pass) { (pass as GPURenderPassEncoder).end(); pass = null; }
            toFree.push(colorTex, depthTex); if (msTex) toFree.push(msTex);
            W = nw; H = nh; canvas.width = W; canvas.height = H;
            ({ colorTex, msTex, depthTex, colorView, depthView, drawView } = makeTargets());
            presentGroup = makePresentGroup();
            log(`[gpu] race ${W}x${H}`);
          }
          lw = nw; lh = nh; scDirty = true; break; }
        case C.TEX_CREATE: {
          const id = cmd[i], w = cmd[i + 1], h = cmd[i + 2], levels = cmd[i + 3]; i += 4;
          const old = textures.get(id);
          if (old) toFree.push(old.tex);
          textures.set(id, { tex: device.createTexture({ size: [w, h], format: 'rgba8unorm', mipLevelCount: levels,
            usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST | GPUTextureUsage.RENDER_ATTACHMENT }), groups: new Map(), levels });
          break; }
        case C.TEX_DESTROY: {
          const id = cmd[i++], t = textures.get(id);
          if (t) { toFree.push(t.tex); textures.delete(id); }
          break; }
        case C.TEX_WRITE: {
          const id = cmd[i], level = cmd[i + 1], x = cmd[i + 2], y = cmd[i + 3], w = cmd[i + 4], h = cmd[i + 5], ofs = cmd[i + 6]; i += 7;
          const t = textures.get(id);
          if (t) queue.writeTexture({ texture: t.tex, mipLevel: level, origin: [x, y] }, mem as unknown as ArrayBuffer, { offset: upPtr + ofs, bytesPerRow: w * 4, rowsPerImage: h }, [w, h]);
          break; }
        case C.TEX_GENMIPS: { const t = textures.get(cmd[i++]); if (t) genMips(t); break; }
        case C.SCISSOR: sc = [cmd[i], cmd[i + 1], cmd[i + 2], cmd[i + 3]]; i += 4; scDirty = true; break;
        case C.PIPE: { const k = cmd[i++]; if (k !== pipeKey) { pipeKey = k; begin().setPipeline(pipeline(k)); } break; }
        case C.BIND: { const id = cmd[i++]; curSampler = cmd[i++]; curTex = (id && textures.get(id)) || whiteT; bindDirty = true; break; }
        case C.UNIFORM: {
          if (ubos < UBO_SLOTS) {
            uboOff = ubos * UBO_ALIGN;
            uboF.set(cf.subarray(i, i + 16), uboOff / 4);
            ubos++;
            begin().setBindGroup(0, uboGroup, [uboOff]);
          }
          i += 16;
          break; }
        case C.DRAW: {
          const first = cmd[i++], count = cmd[i++], p = begin();
          if (bindDirty) { p.setBindGroup(1, group(curTex, curSampler)); bindDirty = false; }
          if (scDirty) {
            const x0 = Math.max(0, Math.floor(sc[0] * W / lw)), y0 = Math.max(0, Math.floor(sc[1] * H / lh));
            const x1 = Math.min(W, Math.floor((sc[2] | 0) * W / lw)), y1 = Math.min(H, Math.floor((sc[3] | 0) * H / lh));
            p.setScissorRect(Math.min(x0, W), Math.min(y0, H), Math.max(0, x1 - x0), Math.max(0, y1 - y0));
            scDirty = false;
          }
          p.draw(count, 1, first);
          draws++; verts += count;
          break; }
        case C.PRESENT: present = true; break;
        case C.READRECT: { read = { seq: cmd[i], x: cmd[i + 1] | 0, y: cmd[i + 2] | 0, w: cmd[i + 3], h: cmd[i + 4], px: cmd[i + 5], done: cmd[i + 6] }; i += 7; break; }
        default: log(`[gpu] unknown command ${op} at ${i - 1} of ${words}`); i = words; break;
      }
    }
    if (ubos) queue.writeBuffer(ubo, 0, uboData, 0, ubos * UBO_ALIGN);
    (pass as GPURenderPassEncoder | null)?.end();
    stats.segments++; stats.draws += draws; stats.lastDraws = draws; stats.lastVerts = verts;
    if (present || read) resolve(enc);
    if (present) {
      const frame = ++stats.frames;
      const view = ctx.getCurrentTexture().createView();
      const out = enc.beginRenderPass({ colorAttachments: [{ view, loadOp: 'clear', storeOp: 'store', clearValue: [0, 0, 0, 1] }] });
      out.setPipeline(presentPipe); out.setBindGroup(0, presentGroup); out.draw(3); out.end();
      if (shotSet.has(frame) || stats.shotNext) {
        stats.shotNext = false;
        const rb = device.createBuffer({ size: W * H * 4, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
        enc.copyTextureToBuffer({ texture: colorTex }, { buffer: rb, bytesPerRow: W * 4 }, [W, H]);
        void queue.onSubmittedWorkDone().then(async () => { await rb.mapAsync(GPUMapMode.READ); stats.shots.set(frame, rb.getMappedRange().slice(0)); rb.destroy(); });
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
    if (present) {
      stats.jsSum += frameJs; if (frameJs > stats.jsMax) stats.jsMax = frameJs; frameJs = 0;
      if (lastPresentAt) { const gap = te - lastPresentAt; if (gap > stats.gapMax) stats.gapMax = gap; if (gap > 25) stats.slow++; }
      lastPresentAt = te;
    }
    readBack?.();
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
