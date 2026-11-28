/* Render interpolation (PC wrapper enhanced/subsystems/framerate/interpolation.cpp). The simulation runs 64 steps a second and the race
 * loop draws only after a new step, which judders on a 60 or 144 Hz display. Each step records the transforms of the cars and the race
 * cameras; a frame draws them blended at a time slightly behind the newest step and puts the real values back after the render call,
 * so the simulation never sees a blended value. The loop draws on every pass, r_pageflip paces it to the display.
 * ?env=INTERP=0 turns it off. Only in the browser: the native build has no display to pace it. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cpu.h"
#include "pu_cars.h"

#define RACE_SKIP_RENDER 0x00606AACu
#define RACE_MENU_RETURN 0x00606AD8u
#define RACE_PAUSED      0x00606A90u
#define RACE_SUSPENDED   0x00606A98u
#define RACE_EXIT        0x00606A8Cu
#define SIM_STEP_COUNT   0x00606AC4u
#define CAMERA_BLOCKS    0x00627E80u
#define CAMERA_STRIDE    268u

#define DEFAULT_RATE 64.0
#define PERIOD_MEASURE_STEPS 256
#define CLOCK_GAP_PERIODS 8.0              /* a longer gap restarts the clock: pause or loading */
#define LATENESS_RELAX (1.0 / 4096.0)      /* upward drift of the lateness floor per step, in periods */
#define RENDERER_OWNED_FRAMES 30           /* frames without a renderer write before a value is blended again */
#define DELAY_SHRINK_STEPS 256
#define TRACK_EXPIRY_STEPS 64
#define SNAP_ROT_DOT 0.5f                  /* a rotation change above 60 degrees is a cut */
#define SNAP_DISTANCE 25.0f
#define HISTORY 6
#define MAX_SPACING 4
#define TRACKS 512                         /* hash table, at most 16 + 16 + 128 cars and 4 camera tracks */

typedef struct { float pos[3], rot[9]; } Xf;
typedef struct { Xf state; int32_t step; } Sample;
typedef struct {
    uint32_t key, position, rotation;      /* guest addresses, 0 = this part is not tracked */
    Sample hist[HISTORY];
    int count, last_spacing, stable_spacing, reset_pending, owned_frames;
    int32_t last_seen;
} Track;
typedef struct { Track *track; Xf saved, written; int watch_only; } Applied;

static Track g_tracks[TRACKS];
static Applied g_applied[TRACKS];
static int g_napplied;
static double g_period_us, g_anchor_time, g_last_step_time, g_lateness_min, g_last_render_step;
static int32_t g_anchor_step, g_last_step, g_delay_needed_at;
static int g_render_delay = 1, g_clock_valid, g_period_locked, g_have_last_render, g_on = -1;

static double now_us(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}
static uint32_t rd32(uint32_t a){ uint32_t v; memcpy(&v, g_mem + a, 4); return v; }
static void wr32(uint32_t a, uint32_t v){ memcpy(g_mem + a, &v, 4); }
static int interp_on(void){
    if (g_on < 0){
#ifdef __wasm__
        const char *e = getenv("INTERP");
        g_on = !(e && *e == '0');
#else
        g_on = 0;
#endif
    }
    return g_on;
}
static int race_running(void){ return !rd32(RACE_PAUSED) && !rd32(RACE_SUSPENDED) && !rd32(RACE_EXIT); }

static Xf xf_read(uint32_t position, uint32_t rotation){
    Xf r;
    memset(&r, 0, sizeof r);
    if (position) memcpy(r.pos, g_mem + position, sizeof r.pos);
    if (rotation) memcpy(r.rot, g_mem + rotation, sizeof r.rot);
    else r.rot[0] = r.rot[4] = r.rot[8] = 1.0f;
    return r;
}
static void xf_write(uint32_t position, uint32_t rotation, const Xf *x){
    if (position) memcpy(g_mem + position, x->pos, sizeof x->pos);
    if (rotation) memcpy(g_mem + rotation, x->rot, sizeof x->rot);
}
static int xf_finite(const Xf *x){
    for (int i = 0; i < 3; i++) if (!isfinite(x->pos[i])) return 0;
    for (int i = 0; i < 9; i++) if (!isfinite(x->rot[i])) return 0;
    return 1;
}
static int xf_same(const Xf *a, const Xf *b){ return memcmp(a, b, sizeof *a) == 0; }
static float v_dot(const float *a, const float *b){ return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float v_len(const float *a){ return sqrtf(v_dot(a, a)); }
static void v_norm(const float *a, float *o){
    const float l = v_len(a), f = l > 1e-6f ? 1.0f / l : 1.0f;
    o[0] = a[0] * f; o[1] = a[1] * f; o[2] = a[2] * f;
}
/* the cosine of the angle between matching rows, the smallest of the three */
static float rot_similarity(const float *a, const float *b){
    float res = 1.0f, na[3], nb[3];
    for (int r = 0; r < 3; r++){
        v_norm(a + r * 3, na); v_norm(b + r * 3, nb);
        const float d = v_dot(na, nb);
        if (d < res) res = d;
    }
    return res;
}
/* a linear blend and Gram-Schmidt keep the handedness and the row lengths of the source */
static void rot_blend(const float *from, const float *to, float t, float *out){
    float rows[3][3], lens[3];
    for (int r = 0; r < 3; r++){
        const float *a = from + r * 3, *b = to + r * 3;
        for (int i = 0; i < 3; i++) rows[r][i] = a[i] + (b[i] - a[i]) * t;
        const float la = v_len(a);
        lens[r] = la + (v_len(b) - la) * t;
    }
    float x[3], y[3], z[3], tmp[3];
    v_norm(rows[0], x);
    const float d = v_dot(rows[1], x);
    for (int i = 0; i < 3; i++) tmp[i] = rows[1][i] - x[i] * d;
    v_norm(tmp, y);
    z[0] = x[1] * y[2] - x[2] * y[1]; z[1] = x[2] * y[0] - x[0] * y[2]; z[2] = x[0] * y[1] - x[1] * y[0];
    const float s = v_dot(z, rows[2]) < 0.0f ? -1.0f : 1.0f;
    for (int i = 0; i < 3; i++){
        out[i] = x[i] * lens[0]; out[3 + i] = y[i] * lens[1]; out[6 + i] = z[i] * s * lens[2];
    }
}
static Xf xf_blend(const Xf *a, const Xf *b, float t){
    Xf r;
    for (int i = 0; i < 3; i++) r.pos[i] = a->pos[i] + (b->pos[i] - a->pos[i]) * t;
    rot_blend(a->rot, b->rot, t, r.rot);
    return r;
}
static float xf_distance(const Xf *a, const Xf *b){
    const float d[3] = { a->pos[0] - b->pos[0], a->pos[1] - b->pos[1], a->pos[2] - b->pos[2] };
    return v_len(d);
}

static Track *track_slot(uint32_t key){
    uint32_t h = (key * 2654435761u) >> 23;
    for (int n = 0; n < TRACKS; n++, h = (h + 1) & (TRACKS - 1))
        if (!g_tracks[h].key || g_tracks[h].key == key) return &g_tracks[h];
    return NULL;
}
static void tracks_clear(void){ memset(g_tracks, 0, sizeof g_tracks); }
/* no tombstones: the living tracks go back in a fresh table; called only between frames */
static void tracks_expire(int32_t step){
    static Track keep[TRACKS];
    int n = 0;
    for (int i = 0; i < TRACKS; i++)
        if (g_tracks[i].key && step - g_tracks[i].last_seen <= TRACK_EXPIRY_STEPS) keep[n++] = g_tracks[i];
    tracks_clear();
    for (int i = 0; i < n; i++) *track_slot(keep[i].key) = keep[i];
}
static void track_push(Track *t, const Sample *s){
    if (t->count == HISTORY){
        memmove(&t->hist[0], &t->hist[1], sizeof t->hist[0] * (HISTORY - 1));
        t->count--;
    }
    t->hist[t->count++] = *s;
}
static void track_reset(Track *t, const Sample *s){ t->hist[0] = *s; t->count = 1; }

static void clock_reset(int32_t step, double now){
    g_anchor_step = step; g_anchor_time = now; g_lateness_min = 0.0; g_clock_valid = 1; g_have_last_render = 0;
}
static void clock_update(int32_t step, double now){
    if (g_period_us <= 0.0) g_period_us = 1e6 / DEFAULT_RATE;
    const int continuous = g_clock_valid && step == g_last_step + 1 && now - g_last_step_time <= g_period_us * CLOCK_GAP_PERIODS;
    if (!continuous){
        if (g_clock_valid && step <= g_last_step) tracks_clear();   /* a new race or a restart */
        clock_reset(step, now);
    }
    else {
        /* steps never run before they are due, the earliest one marks the true schedule */
        const double lateness = now - (g_anchor_time + (double)(step - g_anchor_step) * g_period_us);
        const double relaxed = g_lateness_min + g_period_us * LATENESS_RELAX;
        g_lateness_min = lateness < relaxed ? lateness : relaxed;
        if (!g_period_locked && step - g_anchor_step >= PERIOD_MEASURE_STEPS){
            const double measured = (now - g_anchor_time) / (double)(step - g_anchor_step);
            const double rate = 1e6 / measured;
            /* the first steps of a race catch up with the clock (68 a second here): that window does not count, measure again */
            double locked = 0.0;
            static const double cand[3] = { 32.0, 64.0, 128.0 };
            for (int i = 0; i < 3; i++) if (fabs(rate - cand[i]) < cand[i] * 0.03) locked = cand[i];
            if (locked > 0.0){
                g_period_us = 1e6 / locked;
                g_period_locked = 1;
                fprintf(stderr, "[interp] measured %.2f simulation steps a second, using %.2f\n", rate, locked);
            }
            clock_reset(step, now);
        }
    }
    g_last_step = step; g_last_step_time = now;
}
static void delay_update(int32_t step){
    int needed = 1;
    for (int i = 0; i < TRACKS; i++){
        const Track *t = &g_tracks[i];
        if (t->key && t->last_seen == step && !t->owned_frames && t->stable_spacing > needed) needed = t->stable_spacing;
    }
    /* grow at once, shrink only after the larger delay was not needed for a while */
    if (needed >= g_render_delay) g_delay_needed_at = step;
    if (needed > g_render_delay || (needed < g_render_delay && step - g_delay_needed_at > DELAY_SHRINK_STEPS)){
        fprintf(stderr, "[interp] render delay %d steps\n", needed);
        g_render_delay = needed; g_delay_needed_at = step;
    }
}
static void capture(uint32_t position, uint32_t rotation, int32_t step){
    const Xf state = xf_read(position, rotation);
    if (!xf_finite(&state)) return;
    const uint32_t key = position ? position : rotation;
    Track *t = track_slot(key);
    if (!t) return;
    const Sample cur = { state, step };
    if (!t->key || t->rotation != rotation || t->position != position || !t->count || t->reset_pending){
        const int owned = t->key ? t->owned_frames : 0;
        memset(t, 0, sizeof *t);
        t->key = key; t->position = position; t->rotation = rotation; t->owned_frames = owned;
        track_reset(t, &cur);
        t->last_seen = step;
        return;
    }
    t->last_seen = step;
    const Sample *newest = &t->hist[t->count - 1];
    if (xf_same(&state, &newest->state)) return;
    /* teleports, resets and camera cuts are shown at once */
    if (xf_distance(&newest->state, &state) > SNAP_DISTANCE || rot_similarity(newest->state.rot, state.rot) < SNAP_ROT_DOT){
        track_reset(t, &cur);
        return;
    }
    const int spacing = step - newest->step;
    if (spacing <= MAX_SPACING){
        if (spacing == t->last_spacing) t->stable_spacing = spacing;
        t->last_spacing = spacing;
    }
    else {
        /* leaving a rest: the old state holds until just before the move */
        const Sample rest_end = { newest->state, step - g_render_delay };
        track_push(t, &rest_end);
    }
    track_push(t, &cur);
}
static void capture_cars(uint32_t list, uint32_t count_addr, int capacity, int32_t step){
    int count = (int)rd32(count_addr);
    if (count < 0) count = 0;
    if (count > capacity) count = capacity;
    for (int i = 0; i < count; i++){
        const uint32_t car = rd32(list + (uint32_t)i * 4u);
        if (car) capture(car + 0x330u, car + 0x364u, step);
    }
}
/* after 00414FF0, one simulation step */
void pu_interp_step(void){
    if (!interp_on()) return;
    const double now = now_us();
    const int32_t step = (int32_t)rd32(SIM_STEP_COUNT);
    clock_update(step, now);
    capture_cars(CARS_L_CARS, 0x005E8B58u, CARS_MAX, step);    /* racers (CARS1: moved table) */
    capture_cars(CARS_L_COPS, 0x005E8ABCu, CARS_MAX, step);    /* police */
    capture_cars(0x005E8C24u, 0x005E8B5Cu, 128, step);  /* traffic */
    for (uint32_t i = 0; i < 2; i++){
        const uint32_t b = CAMERA_BLOCKS + i * CAMERA_STRIDE;
        /* position and rotation are separate tracks: the chase cameras' rotation is rewritten by the race renderer each frame and
         * belongs to it then; the orbit and replay cameras set it on steps only and it is blended like the rest */
        if (rd32(b)){
            capture(b + 0x24u, 0, step);
            capture(0, b + 0x7Cu, step);
        }
    }
    if ((step & 63) == 0) tracks_expire(step);
    delay_update(step);
}
static void apply_all(double now){
    g_napplied = 0;
    /* one render time for all keeps cars and cameras consistent with each other */
    double render_step = (double)g_anchor_step + (now - g_anchor_time - g_lateness_min) / g_period_us - (double)g_render_delay;
    if (g_have_last_render && render_step < g_last_render_step) render_step = g_last_render_step;
    g_last_render_step = render_step; g_have_last_render = 1;
    for (int i = 0; i < TRACKS; i++){
        Track *t = &g_tracks[i];
        if (!t->key || t->last_seen != g_last_step) continue;
        if (t->owned_frames > 0){
            Applied *w = &g_applied[g_napplied++];
            w->track = t; w->watch_only = 1;
            w->saved = xf_read(t->position, t->rotation);
            continue;
        }
        if (t->count < 2) continue;
        /* something outside the step moved it (a collision fix-up, a reset): the new value is the newest state, the blend goes on from it */
        const Xf live = xf_read(t->position, t->rotation);
        if (!xf_same(&live, &t->hist[t->count - 1].state)){
            if (!xf_finite(&live)) continue;
            Sample *n = &t->hist[t->count - 1];
            if (n->step == g_last_step) n->state = live;
            else { const Sample s = { live, g_last_step }; track_push(t, &s); }
        }
        int k = t->count - 2;
        while (k > 0 && (double)t->hist[k].step > render_step) k--;
        const Sample *from = &t->hist[k], *to = &t->hist[k + 1];
        const int span = to->step - from->step;
        double a = (render_step - (double)from->step) / (double)(span > 1 ? span : 1);
        if (a < 0.0) a = 0.0;
        if (a > 1.0) a = 1.0;
        Applied *e = &g_applied[g_napplied];
        e->track = t; e->watch_only = 0; e->saved = live;
        e->written = a <= 0.0 ? from->state : a >= 1.0 ? to->state : xf_blend(&from->state, &to->state, (float)a);
        if (!xf_finite(&e->written) || xf_same(&e->written, &live)) continue;
        xf_write(t->position, t->rotation, &e->written);
        g_napplied++;
    }
}
/* PERFSTAT=1: how evenly the drawn camera moves, in both modes */
static void motion_stat(void){
    static int on = -1;
    if (on < 0){ const char *e = getenv("PERFSTAT"); on = e && *e && *e != '0'; }
    if (!on) return;
    static float last[3]; static double t_last, t0, sum_speed, sum_jerk, last_speed; static int n, nstill;
    const double t = now_us();
    float p[3], d[3];
    memcpy(p, g_mem + CAMERA_BLOCKS + 0x24u, sizeof p);
    for (int i = 0; i < 3; i++) d[i] = p[i] - last[i];
    memcpy(last, p, sizeof p);
    const double dt = t - t_last;
    t_last = t;
    if (dt <= 0.0 || dt > 200000.0) return;
    const double speed = v_len(d) / (dt / 1e6);
    if (speed == 0.0) nstill++;
    sum_speed += speed; sum_jerk += fabs(speed - last_speed); last_speed = speed; n++;
    if (!t0) t0 = t;
    if (t - t0 >= 5e6 && n){
        fprintf(stderr, "[perf interp] %.1f fps; camera speed %.2f, change frame to frame %.2f (%.1f %%), frames that did not move %d of %d; render delay %d\n",
                n * 1e6 / (t - t0), sum_speed / n, sum_jerk / n, sum_speed > 0 ? 100.0 * sum_jerk / sum_speed : 0.0, nstill, n, g_render_delay);
        t0 = t; n = nstill = 0; sum_speed = sum_jerk = 0;
    }
}
/* at 00415C6E, the race loop's render decision. 1 = this pass draws, so it must not wait for a tick */
int pu_interp_check(void){
    const int drawing = !rd32(RACE_SKIP_RENDER);
    if (interp_on() && g_clock_valid && race_running()){
        const double now = now_us();
        /* a restart after a pause or a load waits for the next step */
        if (now - g_last_step_time <= g_period_us * CLOCK_GAP_PERIODS){
            if (!drawing && !g_mem[RACE_MENU_RETURN]) wr32(RACE_SKIP_RENDER, 0);    /* no new step: draw anyway, the blend moves on */
            if (!rd32(RACE_SKIP_RENDER)){
                apply_all(now);
                motion_stat();
                return 1;
            }
        }
    }
    if (drawing) motion_stat();
    return drawing;
}
/* at 00415C7F, the first instruction after the render call, on both paths */
void pu_interp_restore(void){
    for (int i = 0; i < g_napplied; i++){
        const Applied *e = &g_applied[i];
        Track *t = e->track;
        const Xf after = xf_read(t->position, t->rotation);
        if (e->watch_only){
            /* still written by the renderer: keep it owned, otherwise count down */
            if (!xf_same(&after, &e->saved)) t->owned_frames = RENDERER_OWNED_FRAMES;
            else if (--t->owned_frames == 0) t->reset_pending = 1;      /* recorded states came from the renderer: start over from the simulation's */
            continue;
        }
        if (xf_same(&after, &e->written)) xf_write(t->position, t->rotation, &e->saved);
        else t->owned_frames = RENDERER_OWNED_FRAMES;                   /* the frame wrote this value itself: it stays, the renderer owns it for now */
    }
    g_napplied = 0;
}
/* the simulation's own transform of a blended part while a frame draws (HEAD1/WHEEL1 move points the frame does not blend, the wheel
 * hubs, with their car): 1 and pos/rot (rows +364, +370, +37C) when the part at that position address is drawn blended now */
int pu_interp_sim_xf(uint32_t position, float *pos, float *rot){
    for (int i = 0; i < g_napplied; i++){
        const Applied *e = &g_applied[i];
        if (e->watch_only || e->track->position != position) continue;
        memcpy(pos, e->saved.pos, 12); memcpy(rot, e->saved.rot, 36);
        return 1;
    }
    return 0;
}
