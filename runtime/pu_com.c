/* nfs5-web: PU's COM objects (WEB1, headless).
 *
 * The model is nfsu2-web's com.c: a guest object is [obj+0] = vtable, every
 * vtable entry is the bridge address COM_MAGIC | iface<<16 | method, and
 * dispatch.c sends calls to those addresses here.  All methods are stdcall
 * with 'this' first; the callee pops the arguments.
 *
 * PU (2000, DirectX 6/7 era) talks COM only to DirectSound (+ 3D buffers and
 * the listener) and DirectInput 5.  Direct3D/DirectDraw sit behind the THRASH
 * driver (thrash_hle.c).  WEB1 is headless: sound buffers play silently with
 * a play cursor on the real clock (the game's mixer thread and the movie
 * player pace themselves on it), input devices report "nothing pressed".
 *
 * Object layout (guest memory): 16-byte header in front (refcount, iface),
 * then [obj+0] vtable, [obj+4..] fields per interface (see the OBJ_* macros). */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void mem_commit(uint32_t, uint32_t);
void dispatch_indirect(CPU*, uint32_t);
void guest_backtrace(CPU*, int);
uint32_t gheap_alloc(int heap, uint32_t n, int zero);
void gheap_free(int heap, uint32_t p);
extern int g_gh_com, g_gh_gbuf;

#define COM_MAGIC 0xE0000000u
#define S_OK            0u
#define E_NOINTERFACE   0x80004002u
#define E_NOTIMPL       0x80004001u
#define E_FAIL          0x80004005u
#define DSERR_NODRIVER  0x88780078u
#define DIERR_DEVICENOTREG 0x80040154u
#define NEL(a) ((unsigned)(sizeof(a)/sizeof(*(a))))

typedef struct { const char *name; unsigned char nargs; } Method;

static const Method M_DI[] = {          /* IDirectInputA / IDirectInput2A */
 {"QueryInterface",3},{"AddRef",1},{"Release",1},
 {"CreateDevice",4},{"EnumDevices",5},{"GetDeviceStatus",2},{"RunControlPanel",3},
 {"Initialize",3},{"FindDevice",4},
};
static const Method M_DIDEV[] = {       /* IDirectInputDeviceA / 2A */
 {"QueryInterface",3},{"AddRef",1},{"Release",1},
 {"GetCapabilities",2},{"EnumObjects",4},{"GetProperty",3},{"SetProperty",3},
 {"Acquire",1},{"Unacquire",1},{"GetDeviceState",3},{"GetDeviceData",5},
 {"SetDataFormat",2},{"SetEventNotification",2},{"SetCooperativeLevel",3},
 {"GetObjectInfo",4},{"GetDeviceInfo",2},{"RunControlPanel",3},{"Initialize",4},
 {"CreateEffect",5},{"EnumEffects",4},{"GetEffectInfo",3},{"GetForceFeedbackState",2},
 {"SendForceFeedbackCommand",2},{"EnumCreatedEffectObjects",4},{"Escape",2},{"Poll",1},
 {"SendDeviceData",5},
};
static const Method M_DS[] = {
 {"QueryInterface",3},{"AddRef",1},{"Release",1},
 {"CreateSoundBuffer",4},{"GetCaps",2},{"DuplicateSoundBuffer",3},
 {"SetCooperativeLevel",3},{"Compact",1},{"GetSpeakerConfig",2},
 {"SetSpeakerConfig",2},{"Initialize",2},
};
static const Method M_DSBUF[] = {
 {"QueryInterface",3},{"AddRef",1},{"Release",1},
 {"GetCaps",2},{"GetCurrentPosition",3},{"GetFormat",4},{"GetVolume",2},
 {"GetPan",2},{"GetFrequency",2},{"GetStatus",2},{"Initialize",3},
 {"Lock",8},{"Play",4},{"SetCurrentPosition",2},{"SetFormat",2},
 {"SetVolume",2},{"SetPan",2},{"SetFrequency",2},{"Stop",1},{"Unlock",5},{"Restore",1},
};
static const Method M_DS3DBUF[] = {
 {"QueryInterface",3},{"AddRef",1},{"Release",1},
 {"GetAllParameters",2},{"GetConeAngles",3},{"GetConeOrientation",2},{"GetConeOutsideVolume",2},
 {"GetMaxDistance",2},{"GetMinDistance",2},{"GetMode",2},{"GetPosition",2},{"GetVelocity",2},
 {"SetAllParameters",3},{"SetConeAngles",4},{"SetConeOrientation",5},{"SetConeOutsideVolume",3},
 {"SetMaxDistance",3},{"SetMinDistance",3},{"SetMode",3},{"SetPosition",5},{"SetVelocity",5},
};
static const Method M_DS3DLIS[] = {
 {"QueryInterface",3},{"AddRef",1},{"Release",1},
 {"GetAllParameters",2},{"GetDistanceFactor",2},{"GetDopplerFactor",2},{"GetOrientation",3},
 {"GetPosition",2},{"GetRolloffFactor",2},{"GetVelocity",2},
 {"SetAllParameters",3},{"SetDistanceFactor",3},{"SetDopplerFactor",3},{"SetOrientation",8},
 {"SetPosition",5},{"SetRolloffFactor",3},{"SetVelocity",5},{"CommitDeferredSettings",1},
};

typedef struct { const char *name; const Method *m; unsigned n; } Iface;
static const Iface g_if[] = {
    {"?", 0, 0},
    {"IDirectInputA",          M_DI,      NEL(M_DI)},
    {"IDirectInputDevice2A",   M_DIDEV,   NEL(M_DIDEV)},
    {"IDirectSound",           M_DS,      NEL(M_DS)},
    {"IDirectSoundBuffer",     M_DSBUF,   NEL(M_DSBUF)},
    {"IDirectSound3DBuffer",   M_DS3DBUF, NEL(M_DS3DBUF)},
    {"IDirectSound3DListener", M_DS3DLIS, NEL(M_DS3DLIS)},
};
enum { IF_DI = 1, IF_DIDEV, IF_DS, IF_DSBUF, IF_DS3DBUF, IF_DS3DLIS, IF__N };

static uint32_t g_vtab[IF__N];
static uint64_t g_calls[IF__N][32];
static uint64_t g_com_calls, g_com_live, g_com_bytes, g_buf_bytes;
static CPU *g_com_cpu;
static int g_trace = -1;

#define CH_REF(o)   MR32((o) - 16u)
#define CH_IFACE(o) MR32((o) - 12u)
#define CH_SIZE(o)  MR32((o) - 8u)

/* fields */
#define DEV_KIND(o)     MR32((o) + 4u)      /* DIDEV: 1 keyboard, 2 mouse */
#define BUF_SIZE(o)     MR32((o) + 4u)      /* DSBUF */
#define BUF_DATA(o)     MR32((o) + 8u)
#define BUF_FLAGS(o)    MR32((o) + 12u)     /* DSBUFFERDESC.dwFlags */
#define BUF_BPS(o)      MR32((o) + 16u)     /* bytes per second */
#define BUF_ALIGN(o)    MR32((o) + 20u)     /* nBlockAlign */
#define BUF_STATE(o)    MR32((o) + 24u)     /* bit0 playing, bit1 looping */
#define BUF_POS(o)      MR32((o) + 28u)     /* play cursor when stopped / at start */
#define BUF_T0LO(o)     MR32((o) + 32u)     /* start time, us (64-bit) */
#define BUF_T0HI(o)     MR32((o) + 36u)
#define BUF_OWNER(o)    MR32((o) + 40u)     /* duplicates share the original's data */
#define BUF_FMT(o)      ((o) + 44u)         /* WAVEFORMATEX copy, 20 bytes */
#define BUF_FREQ(o)     MR32((o) + 64u)
#define BUF_VOL(o)      MR32((o) + 68u)
#define BUF_PAN(o)      MR32((o) + 72u)
#define BUF_EXTRA       80u

static uint64_t now_us(void){
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

static uint32_t com_new(uint32_t iface, uint32_t extra){
    if (!g_vtab[iface]){
        unsigned n = g_if[iface].n;
        uint32_t vt = gheap_alloc(g_gh_com, n * 4u, 1);
        for (unsigned i = 0; i < n; i++) MR32(vt + 4u * i) = COM_MAGIC | (iface << 16) | i;
        g_vtab[iface] = vt;
    }
    uint32_t blk = gheap_alloc(g_gh_com, 16u + 4u + extra, 1);
    if (!blk) return 0;
    uint32_t o = blk + 16u;
    CH_REF(o) = 1; CH_IFACE(o) = iface; CH_SIZE(o) = extra;
    MR32(o) = g_vtab[iface];
    g_com_live++; g_com_bytes += 20u + extra;
    return o;
}
static int com_is(uint32_t o, uint32_t iface){
    return o >= 0x10000u && mem_is_committed(o - 16u) && mem_is_committed(o + 3u)
        && MR32(o) == g_vtab[iface] && CH_IFACE(o) == iface;
}
static uint32_t com_release(uint32_t o){
    uint32_t r = CH_REF(o);
    if (r == 0) return 0;
    if (--r == 0){
        uint32_t iface = CH_IFACE(o);
        if (iface == IF_DSBUF && BUF_DATA(o) && !BUF_OWNER(o)){ gheap_free(g_gh_gbuf, BUF_DATA(o)); g_buf_bytes -= BUF_SIZE(o); }
        if (iface == IF_DSBUF && BUF_OWNER(o)) com_release(BUF_OWNER(o));
        g_com_bytes -= 20u + CH_SIZE(o);
        CH_REF(o) = 0; MR32(o) = 0;
        gheap_free(g_gh_com, o - 16u);
        g_com_live--;
        return 0;
    }
    CH_REF(o) = r;
    return r;
}

/* ---- guest callbacks ---------------------------------------------------- */
uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args){
    if (!fn) return 0;
    CPU save = *c;
    for (int i = nargs - 1; i >= 0; i--) PUSH32(args[i]);
    PUSH32(0xDEADBEEFu);                    /* fake return address */
    dispatch_indirect(c, fn);
    uint32_t ret = c->eax;
    *c = save;                              /* back to the pre-call state */
    return ret;
}
uint32_t com_scratch(uint32_t n){ return gheap_alloc(g_gh_gbuf, n, 1); }

/* ---- DirectInput -------------------------------------------------------- */
static void put_guid(uint32_t p, uint32_t d1, uint16_t d2, uint16_t d3, const uint8_t d4[8]){
    MR32(p) = d1; MR16(p + 4) = d2; MR16(p + 6) = d3;
    for (int i = 0; i < 8; i++) MR8(p + 8 + i) = d4[i];
}
static const uint8_t k_sys_tail[8] = {0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00};
#define GUID_SYSMOUSE    0x6F1D2B60u
#define GUID_SYSKEYBOARD 0x6F1D2B61u

/* DIDEVICEINSTANCEA (DX5: 580 bytes) for the system keyboard / mouse */
static uint32_t devinst(int keyboard){
    static uint32_t kb, ms;
    uint32_t *slot = keyboard ? &kb : &ms;
    if (*slot) return *slot;
    uint32_t p = gheap_alloc(g_gh_gbuf, 600, 1);
    MR32(p) = 580u;
    put_guid(p + 4,  keyboard ? GUID_SYSKEYBOARD : GUID_SYSMOUSE, 0xD5A0, 0x11CF, k_sys_tail);
    put_guid(p + 20, keyboard ? GUID_SYSKEYBOARD : GUID_SYSMOUSE, 0xD5A0, 0x11CF, k_sys_tail);
    MR32(p + 36) = keyboard ? 0x00000403u : 0x00000202u;   /* DIDEVTYPE_KEYBOARD/PCENH, MOUSE/TRADITIONAL */
    const char *nm = keyboard ? "Keyboard" : "Mouse";
    strcpy((char *)(g_mem + p + 40), nm);
    strcpy((char *)(g_mem + p + 300), nm);
    *slot = p;
    return p;
}

static uint32_t di_call(CPU *c, uint32_t self, int m, const uint32_t *a){
    switch (m){
    case 3: {                                   /* CreateDevice(this, rguid, ppdev, outer) */
        uint32_t g = a[1], pp = a[2];
        uint32_t d1 = (g && mem_is_committed(g + 15)) ? MR32(g) : 0;
        int kind = d1 == GUID_SYSKEYBOARD ? 1 : d1 == GUID_SYSMOUSE ? 2 : 0;
        if (!kind){ fprintf(stderr, "[com] DirectInput CreateDevice: unknown device GUID %08x\n", d1);
                    if (pp) MR32(pp) = 0; return DIERR_DEVICENOTREG; }
        uint32_t d = com_new(IF_DIDEV, 16u);
        DEV_KIND(d) = (uint32_t)kind;
        if (pp) MR32(pp) = d;
        fprintf(stderr, "[com] DirectInput device %s 0x%08x\n", kind == 1 ? "keyboard" : "mouse", d);
        return S_OK; }
    case 4: {                                   /* EnumDevices(this, type, cb, ref, flags) */
        uint32_t type = a[1] & 0xFFu, cb = a[2], ref = a[3];
        for (int kb = 1; kb >= 0; kb--){
            if (type && type != (kb ? 3u : 2u)) continue;
            uint32_t args[2] = { devinst(kb), ref };
            if (call_guest(c, cb, 2, args) == 0) break;     /* DIENUM_STOP */
        }
        return S_OK; }
    case 5: return S_OK;                        /* GetDeviceStatus: attached */
    case 6: case 7: return S_OK;
    case 8: return DIERR_DEVICENOTREG;          /* FindDevice */
    }
    return S_OK;
}

/* the renderer window's keyboard (renderer/thrash_dx11.cpp); weak: headless = nothing pressed */
const uint8_t *host_dik_state(void) __attribute__((weak));
int host_key_event(int peek, uint32_t *ofs, uint32_t *data, uint32_t *ts, uint32_t *seq) __attribute__((weak));

static uint32_t didev_call(CPU *c, uint32_t self, int m, const uint32_t *a){
    (void)c;
    int kb = DEV_KIND(self) == 1;
    switch (m){
    case 3: {                                   /* GetCapabilities(this, DIDEVCAPS*) */
        uint32_t p = a[1];
        if (p){ uint32_t sz = MR32(p); if (sz < 24u || sz > 64u) sz = 44u;
                memset(g_mem + p + 4, 0, sz - 4);
                MR32(p + 4) = 1u;               /* DIDC_ATTACHED */
                MR32(p + 8) = kb ? 0x403u : 0x202u;
                MR32(p + 12) = kb ? 0u : 3u;    /* axes */
                MR32(p + 16) = kb ? 128u : 3u;  /* buttons */
        }
        return S_OK; }
    case 9: {                                   /* GetDeviceState(this, cb, data) */
        uint32_t n = a[1], p = a[2];
        if (p && n && n <= 4096u){ mem_commit(p, n); memset(g_mem + p, 0, n);
            /* WEB2: the renderer window's keys (DIK codes, 0x80 = down) */
            const uint8_t *k = kb && host_dik_state ? host_dik_state() : 0;
            if (k) memcpy(g_mem + p, k, n < 256u ? n : 256u); }
        return S_OK; }
    case 10: {                                  /* GetDeviceData(this, cbObj, rgdod, pdwInOut, flags) */
        uint32_t cb = a[1], dst = a[2], pio = a[3], fl = a[4];
        uint32_t want = pio ? MR32(pio) : 0, got = 0;
        if (kb && host_key_event){
            /* DIDEVICEOBJECTDATA: ofs (DIK), data (0x80 down), timestamp, sequence */
            uint32_t ofs, data, ts, seq;
            while (got < want && host_key_event((fl & 1u) ? (int)got : -1, &ofs, &data, &ts, &seq)){   /* DIGDD_PEEK */
                if (dst && cb >= 16u){ uint32_t e = dst + got * cb;
                    MR32(e) = ofs; MR32(e + 4) = data; MR32(e + 8) = ts; MR32(e + 12) = seq; }
                got++;
            }
        }
        if (pio) MR32(pio) = got;
        return S_OK; }
    case 5: return E_NOTIMPL;                   /* GetProperty: as for a device without the property */
    case 15: {                                  /* GetDeviceInfo(this, DIDEVICEINSTANCEA*) */
        uint32_t p = a[1];
        if (p){ uint32_t src = devinst(kb), sz = MR32(p); if (sz < 4u || sz > 580u) sz = 580u;
                memcpy(g_mem + p + 4, g_mem + src + 4, sz - 4); }
        return S_OK; }
    case 18: return E_NOTIMPL;                  /* CreateEffect: no force feedback */
    }
    return S_OK;                                /* Acquire, Unacquire, SetDataFormat, SetCooperativeLevel, ... */
}

/* ---- DirectSound -------------------------------------------------------- */
static uint32_t ds_play_pos(uint32_t b){
    uint32_t sz = BUF_SIZE(b);
    if (!sz) return 0;
    if (!(BUF_STATE(b) & 1u)) return BUF_POS(b) % sz;
    uint64_t t0 = (uint64_t)BUF_T0HI(b) << 32 | BUF_T0LO(b);
    uint64_t bytes = (now_us() - t0) * (uint64_t)BUF_BPS(b) / 1000000u + BUF_POS(b);
    uint32_t al = BUF_ALIGN(b) ? BUF_ALIGN(b) : 1u;
    if (!(BUF_STATE(b) & 2u) && bytes >= sz){     /* one-shot buffer ran out */
        BUF_STATE(b) = 0; BUF_POS(b) = 0; return 0; }
    return (uint32_t)(bytes % sz) / al * al;
}
static void ds_set_format(uint32_t b, uint32_t wfx){
    if (!wfx || !mem_is_committed(wfx + 15)) return;
    memcpy(g_mem + BUF_FMT(b), g_mem + wfx, 16); MR32(BUF_FMT(b) + 16) = 0;   /* WAVEFORMATEX without extra */
    uint32_t bps = MR32(wfx + 8);
    BUF_BPS(b) = (bps >= 1000u && bps <= 2000000u) ? bps : 88200u;
    BUF_ALIGN(b) = MR16(wfx + 12) ? MR16(wfx + 12) : 1u;
    BUF_FREQ(b) = MR32(wfx + 4);
}

static uint32_t g_ds, g_ds_primary;
static uint32_t ds_call(CPU *c, uint32_t self, int m, const uint32_t *a){
    switch (m){
    case 3: {                                   /* CreateSoundBuffer(this, desc, ppbuf, outer) */
        uint32_t d = a[1], pp = a[2];
        uint32_t flags = d ? MR32(d + 4) : 0, bytes = d ? MR32(d + 8) : 0, wfx = d ? MR32(d + 16) : 0;
        uint32_t b = com_new(IF_DSBUF, BUF_EXTRA);
        BUF_FLAGS(b) = flags; BUF_BPS(b) = 88200u; BUF_ALIGN(b) = 4u; BUF_FREQ(b) = 22050u;
        if (flags & 1u){                        /* DSBCAPS_PRIMARYBUFFER */
            BUF_SIZE(b) = 0x8000u; g_ds_primary = b;
            static const uint8_t pcm[16] = {1,0, 2,0, 0x22,0x56,0,0, 0x88,0x58,1,0, 4,0, 16,0};  /* 22050 Hz 16-bit stereo */
            memcpy(g_mem + BUF_FMT(b), pcm, 16);
        } else {
            if (!bytes || bytes > 0x1000000u) bytes = 0x10000u;
            BUF_SIZE(b) = bytes; ds_set_format(b, wfx);
        }
        BUF_DATA(b) = gheap_alloc(g_gh_gbuf, BUF_SIZE(b) + 64u, 1); g_buf_bytes += BUF_SIZE(b);
        if (g_trace) fprintf(stderr, "[com] CreateSoundBuffer flags=0x%x bytes=%u bps=%u -> 0x%08x\n",
                             flags, BUF_SIZE(b), BUF_BPS(b), b);
        if (pp) MR32(pp) = b;
        return S_OK; }
    case 4: {                                   /* GetCaps(this, DSCAPS*) */
        uint32_t p = a[1];
        if (p){ uint32_t sz = MR32(p); if (sz < 8u || sz > 256u) sz = 96u;
                memset(g_mem + p + 4, 0, sz - 4);
                MR32(p + 4) = 0x00000F1Fu;      /* primary+secondary mono/stereo/8/16, CONTINUOUSRATE */
                MR32(p + 8) = 4000u; MR32(p + 12) = 48000u; MR32(p + 16) = 1u;
        }
        return S_OK; }
    case 5: {                                   /* DuplicateSoundBuffer(this, orig, ppdup) */
        uint32_t src = a[1], pp = a[2];
        if (!com_is(src, IF_DSBUF)){ if (pp) MR32(pp) = 0; return E_FAIL; }
        uint32_t b = com_new(IF_DSBUF, BUF_EXTRA);
        memcpy(g_mem + b + 4, g_mem + src + 4, BUF_EXTRA - 4);
        BUF_STATE(b) = 0; BUF_POS(b) = 0;
        uint32_t root = BUF_OWNER(src) ? BUF_OWNER(src) : src;
        BUF_OWNER(b) = root; CH_REF(root)++;
        if (pp) MR32(pp) = b;
        return S_OK; }
    case 8: { uint32_t p = a[1]; if (p) MR32(p) = 4u; return S_OK; }   /* GetSpeakerConfig: stereo */
    }
    (void)c; (void)self;
    return S_OK;                                /* SetCooperativeLevel, Compact, SetSpeakerConfig, Initialize */
}

static uint32_t dsbuf_call(CPU *c, uint32_t b, int m, const uint32_t *a){
    (void)c;
    uint32_t sz = BUF_SIZE(b);
    switch (m){
    case 3: {                                   /* GetCaps(this, DSBCAPS*) */
        uint32_t p = a[1];
        if (p){ MR32(p + 4) = BUF_FLAGS(b) | 0x8u; MR32(p + 8) = sz; MR32(p + 12) = 0; MR32(p + 16) = 0; }
        return S_OK; }
    case 4: {                                   /* GetCurrentPosition(this, pplay, pwrite) */
        uint32_t pos = ds_play_pos(b);
        uint32_t ahead = BUF_BPS(b) / 100u;     /* the write cursor ~10 ms ahead */
        if (a[1]) MR32(a[1]) = pos;
        if (a[2]) MR32(a[2]) = sz ? (pos + ahead) % sz : 0;
        return S_OK; }
    case 5: {                                   /* GetFormat(this, wfx, size, pwritten) */
        uint32_t p = a[1], n = a[2];
        if (p && n){ uint32_t k = n < 18u ? n : 18u; memcpy(g_mem + p, g_mem + BUF_FMT(b), k); }
        if (a[3]) MR32(a[3]) = 18u;
        return S_OK; }
    case 6: if (a[1]) MR32(a[1]) = BUF_VOL(b); return S_OK;
    case 7: if (a[1]) MR32(a[1]) = BUF_PAN(b); return S_OK;
    case 8: if (a[1]) MR32(a[1]) = BUF_FREQ(b); return S_OK;
    case 9: {                                   /* GetStatus */
        if (BUF_STATE(b) & 1u) (void)ds_play_pos(b);
        if (a[1]) MR32(a[1]) = (BUF_STATE(b) & 1u) ? (1u | ((BUF_STATE(b) & 2u) ? 4u : 0u)) : 0u;
        return S_OK; }
    case 11: {                                  /* Lock(this, off, bytes, pp1, pn1, pp2, pn2, flags) */
        uint32_t off = a[1], want = a[2], fl = a[7];
        if (!sz) return E_FAIL;
        if (fl & 1u) off = ds_play_pos(b);      /* DSBLOCK_FROMWRITECURSOR (close enough) */
        if (fl & 2u){ off = 0; want = sz; }     /* DSBLOCK_ENTIREBUFFER */
        off %= sz; if (!want || want > sz) want = sz;
        uint32_t b1 = want <= sz - off ? want : sz - off, b2 = want - b1;
        if (a[3]) MR32(a[3]) = BUF_DATA(b) + off;
        if (a[4]) MR32(a[4]) = b1;
        if (a[5]) MR32(a[5]) = b2 ? BUF_DATA(b) : 0u;
        if (a[6]) MR32(a[6]) = b2;
        return S_OK; }
    case 12: {                                  /* Play(this, res1, prio, flags) */
        uint32_t pos = ds_play_pos(b);
        uint64_t t = now_us();
        BUF_POS(b) = pos; BUF_T0LO(b) = (uint32_t)t; BUF_T0HI(b) = (uint32_t)(t >> 32);
        BUF_STATE(b) = 1u | ((a[3] & 1u) ? 2u : 0u);
        return S_OK; }
    case 13: {                                  /* SetCurrentPosition */
        uint64_t t = now_us();
        BUF_POS(b) = sz ? a[1] % sz : 0; BUF_T0LO(b) = (uint32_t)t; BUF_T0HI(b) = (uint32_t)(t >> 32);
        return S_OK; }
    case 14: ds_set_format(b, a[1]); return S_OK;
    case 15: BUF_VOL(b) = a[1]; return S_OK;
    case 16: BUF_PAN(b) = a[1]; return S_OK;
    case 17: {                                  /* SetFrequency: the cursor speed follows */
        uint32_t f = a[1], pos = ds_play_pos(b), al = BUF_ALIGN(b) ? BUF_ALIGN(b) : 1u;
        if (f >= 100u && f <= 200000u){ BUF_FREQ(b) = f; BUF_BPS(b) = f * al; }
        uint64_t t = now_us();
        BUF_POS(b) = pos; BUF_T0LO(b) = (uint32_t)t; BUF_T0HI(b) = (uint32_t)(t >> 32);
        return S_OK; }
    case 18: BUF_POS(b) = ds_play_pos(b); BUF_STATE(b) = 0; return S_OK;   /* Stop */
    }
    return S_OK;                                /* Initialize, Unlock, Restore */
}

/* IIDs the game may ask a sound buffer / the device for (Data1 is enough) */
#define IID_IDirectSound3DListener 0x279AFA84u
#define IID_IDirectSound3DBuffer   0x279AFA86u
#define IID_IDirectSoundBuffer     0x279AFA85u
#define IID_IDirectSound           0x279AFA83u
#define IID_IDirectInputDevice2A   0x5944E682u
#define IID_IDirectInputDeviceA    0x5944E680u
#define IID_IDirectInput2A         0x5944E662u
#define IID_IDirectInputA          0x89521360u

static uint32_t query_interface(uint32_t self, uint32_t iface, uint32_t riid, uint32_t ppv){
    uint32_t d1 = (riid && mem_is_committed(riid + 15)) ? MR32(riid) : 0;
    uint32_t r = 0;
    if ((iface == IF_DSBUF && d1 == IID_IDirectSoundBuffer) || (iface == IF_DS && d1 == IID_IDirectSound)
        || (iface == IF_DIDEV && (d1 == IID_IDirectInputDevice2A || d1 == IID_IDirectInputDeviceA))
        || (iface == IF_DI && (d1 == IID_IDirectInput2A || d1 == IID_IDirectInputA)))
        { CH_REF(self)++; r = self; }
    else if (iface == IF_DSBUF && d1 == IID_IDirectSound3DBuffer){
        r = com_new(IF_DS3DBUF, 4u); MR32(r + 4) = self; CH_REF(self)++; }
    else if (iface == IF_DSBUF && d1 == IID_IDirectSound3DListener){
        r = com_new(IF_DS3DLIS, 4u); MR32(r + 4) = self; CH_REF(self)++; }
    if (ppv) MR32(ppv) = r;
    if (!r){ static int shown; if (shown++ < 16)
        fprintf(stderr, "[com] %s::QueryInterface({%08x-...}) -> E_NOINTERFACE\n", g_if[iface].name, d1); }
    return r ? S_OK : E_NOINTERFACE;
}

void com_call(CPU *c, uint32_t magic){
    uint32_t iface = (magic >> 16) & 0x1FFFu, m = magic & 0xFFFFu;
    g_com_calls++; g_com_cpu = c;
    if (g_trace < 0) g_trace = getenv("COMTRACE") ? 1 : 0;
    POP32(c->eip);
    if (iface == 0 || iface >= IF__N || m >= g_if[iface].n){
        fprintf(stderr, "\n*** COM call to unknown bridge 0x%08x (ret 0x%08x)\n", magic, c->eip);
        guest_backtrace(c, 12);
        exit(6);
    }
    int nargs = g_if[iface].m[m].nargs;
    uint32_t a[8] = {0};
    for (int i = 0; i < nargs && i < 8; i++) a[i] = MR32(c->esp + 4u * (uint32_t)i);
    if (m < 32) g_calls[iface][m]++;
    uint32_t self = a[0], r;
    if (m == 0) r = query_interface(self, iface, a[1], a[2]);
    else if (m == 1) r = ++CH_REF(self);
    else if (m == 2){
        uint32_t parent = (iface == IF_DS3DBUF || iface == IF_DS3DLIS) ? MR32(self + 4) : 0;
        if (iface == IF_DSBUF && CH_REF(self) == 1) BUF_STATE(self) = 0;   /* the last Release stops it */
        r = com_release(self);
        if (!r && parent) com_release(parent);
    }
    else switch (iface){
        case IF_DI:     r = di_call(c, self, (int)m, a); break;
        case IF_DIDEV:  r = didev_call(c, self, (int)m, a); break;
        case IF_DS:     r = ds_call(c, self, (int)m, a); break;
        case IF_DSBUF:  r = dsbuf_call(c, self, (int)m, a); break;
        default:        r = S_OK;              /* 3D buffer / listener: parameters are not used headless */
                        if (iface == IF_DS3DBUF && m == 9 && a[1]) MR32(a[1]) = 0;   /* GetMode: NORMAL */
                        break;
    }
    if (g_trace && !(iface == IF_DSBUF && (m == 4 || m == 9 || m == 11 || m == 19)))
        fprintf(stderr, "  [COM] %s::%s -> 0x%08x\n", g_if[iface].name, g_if[iface].m[m].name, r);
    c->eax = r;
    c->esp += 4u * (uint32_t)nargs;
}

/* ---- creation (from pu_call in win32.c's import chain) ------------------ */
static uint32_t g_di;
uint32_t pu_directinputcreate(uint32_t ver, uint32_t pp){
    if (!com_is(g_di, IF_DI) || !CH_REF(g_di)){ g_di = com_new(IF_DI, 4u); fprintf(stderr, "[com] DirectInputCreateA(version 0x%x) -> 0x%08x\n", ver, g_di); }
    else CH_REF(g_di)++;
    if (pp) MR32(pp) = g_di;
    return S_OK;
}
uint32_t pu_directsoundcreate(uint32_t pp){
    if (!com_is(g_ds, IF_DS) || !CH_REF(g_ds)){ g_ds = com_new(IF_DS, 4u); fprintf(stderr, "[com] DirectSoundCreate -> 0x%08x (silent, real-time cursors)\n", g_ds); }
    else CH_REF(g_ds)++;
    if (pp) MR32(pp) = g_ds;
    return S_OK;
}
uint32_t pu_directsoundcapturecreate(uint32_t pp){ if (pp) MR32(pp) = 0; return DSERR_NODRIVER; }

void com_init(void){ }
void com_forget_cpu(CPU *c){ if (g_com_cpu == c) g_com_cpu = 0; }
void com_mem_stats(uint64_t *com_bytes, uint64_t *buf_bytes, uint64_t *objs){
    *com_bytes = g_com_bytes; *buf_bytes = g_buf_bytes; *objs = g_com_live;
}
void com_report(void){
    fprintf(stderr, "\n=== COM: %llu calls, %llu live objects ===\n",
            (unsigned long long)g_com_calls, (unsigned long long)g_com_live);
    for (unsigned i = 1; i < IF__N; i++)
        for (unsigned m = 0; m < g_if[i].n && m < 32; m++)
            if (g_calls[i][m]) fprintf(stderr, "    %-24s %-26s %llu\n", g_if[i].name, g_if[i].m[m].name,
                                       (unsigned long long)g_calls[i][m]);
}
