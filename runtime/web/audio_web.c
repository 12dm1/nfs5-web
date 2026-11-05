/* nfs5-web WEB3: sound in the browser. A thread keeps a ring of stereo floats filled from the mixer (pu_audio_render,
 * runtime/pu_com.c); the page's AudioWorklet (web/src/audio/worklet.ts) plays from it. */
#include "host_abi.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void pu_audio_render(float *out, int frames, int rate, int64_t t_ahead_us);

#define RATE 48000
#define CAP (1u << 14)                  /* frames, a power of two: 340 ms */
#define TARGET 2400u                    /* the frames kept queued: 50 ms */
static struct {
    uint32_t magic, cap, rate;
    volatile uint32_t wr, rd;           /* frame counters, wrap freely */
    float data[CAP * 2];
} g_ring = { 0x41554431u, CAP, RATE, 0, 0, { 0 } };
static float g_tmp[480 * 2];

static void *audio_thread(void *arg){
    (void)arg;
    for (;;){
        const uint32_t wr = g_ring.wr, rd = __atomic_load_n(&g_ring.rd, __ATOMIC_ACQUIRE);
        const uint32_t queued = wr - rd;
        if (queued + 480 > TARGET){ usleep(3000); continue; }
        pu_audio_render(g_tmp, 480, RATE, (int64_t)queued * 1000000 / RATE);
        for (uint32_t i = 0; i < 480; i++){
            const uint32_t k = (wr + i) & (CAP - 1);
            g_ring.data[k * 2] = g_tmp[i * 2]; g_ring.data[k * 2 + 1] = g_tmp[i * 2 + 1];
        }
        __atomic_store_n(&g_ring.wr, wr + 480, __ATOMIC_RELEASE);
    }
    return 0;
}
void audio_host_start(void){
    static int started;
    if (started) return;
    started = 1;
    const char *na = getenv("NFS5_NOAUDIO");
    if (na && *na && *na != '0'){ fprintf(stderr, "[audio] off (NFS5_NOAUDIO)\n"); return; }
    nfsu2_register(HOST_KIND_AUDIO, &g_ring, sizeof g_ring);
    pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 256 * 1024);
    if (pthread_create(&t, &a, audio_thread, 0)) fprintf(stderr, "[audio] no thread\n");
    else fprintf(stderr, "[audio] page mixer, %d Hz stereo\n", RATE);
}
