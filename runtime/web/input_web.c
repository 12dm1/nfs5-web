/* nfs5-web WEB3: keyboard and mouse from the page (web/src/input.ts), the browser's side of what
 * renderer/thrash_dx11.cpp does with its window on Windows: a mailbox of window messages + DirectInput key events +
 * key/button state, drained into the guest window's queue by kbd_pump. */
#include "host_abi.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define WM_MOUSEMOVE 0x200
#define WM_LBUTTONDOWN 0x201
#define WM_LBUTTONUP 0x202
#define WM_LBUTTONDBLCLK 0x203
#define WM_RBUTTONDOWN 0x204
#define WM_RBUTTONUP 0x205
#define WM_MBUTTONDOWN 0x207
#define WM_MBUTTONUP 0x208
#define WM_MOUSEWHEEL 0x20A

typedef struct { uint32_t m, w, l; } Msg;
typedef struct { uint32_t ofs, data, ts, seq; } Key;
typedef struct {
    uint32_t magic;
    volatile int32_t msg_head, msg_tail, key_head, key_tail;
    volatile int32_t mx, my;            /* the cursor: 0..65535 of the picture */
    Msg msgs[256];
    Key keys[256];
    uint8_t dik[256];                   /* DirectInput keyboard state */
    uint8_t vk[256];                    /* GetAsyncKeyState */
    /* the pad (web/src/input.ts) and what the page has to know for it */
    volatile int32_t pad_on; volatile uint32_t pad_buttons;
    volatile float pad_lx, pad_rt, pad_lt;
    volatile int32_t pad_mode, rumble_seq;
    volatile float rumble_low, rumble_high;
    volatile int32_t rumble_ms;
} Mail;
static Mail g_mail = { .magic = 0x4B424D31u };

void metal_game_size(unsigned *w, unsigned *h);
long r_frames(void) __attribute__((weak));
void win32_post_message(uint32_t m, uint32_t wp, uint32_t lp);
int win32_msg_room(void);

int pu_pad_mode(void);
int host_pad_read(uint32_t *buttons, float *lx, float *rt, float *lt){
    if (!__atomic_load_n(&g_mail.pad_on, __ATOMIC_ACQUIRE)) return 0;
    *buttons = g_mail.pad_buttons; *lx = g_mail.pad_lx; *rt = g_mail.pad_rt; *lt = g_mail.pad_lt;
    return 1;
}
void host_pad_rumble(float low, float high, int ms){
    g_mail.rumble_low = low; g_mail.rumble_high = high; g_mail.rumble_ms = ms;
    __atomic_add_fetch(&g_mail.rumble_seq, 1, __ATOMIC_RELEASE);
}
static uint32_t g_mbtn;
static int g_lx = -1, g_ly = -1;

static void cursor(int *x, int *y){
    unsigned w = 640, h = 480; metal_game_size(&w, &h);
    int cx = (int)(((int64_t)__atomic_load_n(&g_mail.mx, __ATOMIC_RELAXED) * w) >> 16);
    int cy = (int)(((int64_t)__atomic_load_n(&g_mail.my, __ATOMIC_RELAXED) * h) >> 16);
    *x = cx < 0 ? 0 : cx >= (int)w ? (int)w - 1 : cx;
    *y = cy < 0 ? 0 : cy >= (int)h ? (int)h - 1 : cy;
}
void metal_get_cursor(int *x, int *y){ cursor(x, y); }
const uint8_t *host_dik_state(void){ return g_mail.dik; }
uint32_t host_mouse_buttons(void){ return g_mbtn; }
int host_vk_down(int vk){ return vk >= 0 && vk < 256 && g_mail.vk[vk] ? 1 : 0; }
void host_mouse_state(int32_t *dx, int32_t *dy, uint32_t *buttons){
    int x, y; cursor(&x, &y);
    if (g_lx < 0){ g_lx = x; g_ly = y; }
    *dx = x - g_lx; *dy = y - g_ly; g_lx = x; g_ly = y;
    *buttons = g_mbtn;
}
int host_key_event(int peek, uint32_t *ofs, uint32_t *data, uint32_t *ts, uint32_t *seq){
    int32_t head = __atomic_load_n(&g_mail.key_head, __ATOMIC_ACQUIRE), tail = __atomic_load_n(&g_mail.key_tail, __ATOMIC_ACQUIRE);
    int32_t i = head + (peek >= 0 ? peek : 0);
    if (i - tail >= 0) return 0;
    const Key *e = &g_mail.keys[i & 255];
    *ofs = e->ofs; *data = e->data; *ts = e->ts; *seq = e->seq;
    if (peek < 0) __atomic_store_n(&g_mail.key_head, head + 1, __ATOMIC_RELEASE);
    return 1;
}
static int is_mouse(uint32_t m){ return m >= WM_MOUSEMOVE && m <= WM_MOUSEWHEEL; }
/* called by PeekMessage/GetMessage with the guest lock held */
void kbd_pump(void){
    static int registered;
    if (!registered){ registered = 1; nfsu2_register(HOST_KIND_INPUT, &g_mail, sizeof g_mail); }
    { const int mode = pu_pad_mode();
      if (mode != g_mail.pad_mode){ __atomic_store_n(&g_mail.pad_mode, mode, __ATOMIC_RELEASE); fprintf(stderr, "[pad] mode %d\n", mode); } }
    static long s_moveframe = -1;
    const long frame = r_frames ? r_frames() : 0;
    for (;;){
        int32_t head = __atomic_load_n(&g_mail.msg_head, __ATOMIC_ACQUIRE), tail = __atomic_load_n(&g_mail.msg_tail, __ATOMIC_ACQUIRE);
        if (head == tail || win32_msg_room() <= 8) break;
        /* mouse moves: the latest position only, at most one a frame (thrash_dx11.cpp kbd_pump) */
        if (g_mail.msgs[head & 255].m == WM_MOUSEMOVE){
            while (tail - head > 1 && g_mail.msgs[(head + 1) & 255].m == WM_MOUSEMOVE) head++;
            __atomic_store_n(&g_mail.msg_head, head, __ATOMIC_RELEASE);
            if (s_moveframe == frame) break;
            s_moveframe = frame;
        }
        const Msg e = g_mail.msgs[head & 255];
        __atomic_store_n(&g_mail.msg_head, head + 1, __ATOMIC_RELEASE);
        uint32_t lp = e.l;
        if (is_mouse(e.m)){
            unsigned w = 640, h = 480; metal_game_size(&w, &h);
            uint32_t x = ((e.l & 0xFFFFu) * w) >> 16, y = ((e.l >> 16) * h) >> 16;
            if (x >= w) x = w - 1;
            if (y >= h) y = h - 1;
            lp = x | y << 16;
        }
        switch (e.m){
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: g_mbtn |= 1; break;
        case WM_LBUTTONUP: g_mbtn &= ~1u; break;
        case WM_RBUTTONDOWN: g_mbtn |= 2; break;
        case WM_RBUTTONUP: g_mbtn &= ~2u; break;
        case WM_MBUTTONDOWN: g_mbtn |= 4; break;
        case WM_MBUTTONUP: g_mbtn &= ~4u; break;
        }
        win32_post_message(e.m, e.w, lp);
    }
}
