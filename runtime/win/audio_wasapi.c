/* nfs5-web WEB2: sound output on Windows (WASAPI, shared mode).
 *
 * A host thread pulls stereo float frames from the mixer (pu_audio_render in
 * pu_com.c) whenever the device asks for more (event-driven, ~10 ms period).
 * 48 kHz float, Windows converts to the device's format (AUTOCONVERTPCM).
 * Off with NFS5_NOAUDIO=1 and in headless runs (NFS5_HEADLESS=1).
 * NFS5_AUDIODUMP=<file.wav>: no device, the mix goes to a 16-bit WAV file
 * at real-time pace (autotests; also works headless). */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

void pu_audio_render(float *out, int frames, int rate, int64_t t_ahead_us);

DEFINE_GUID(NFS_CLSID_MMDeviceEnumerator, 0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(NFS_IID_IMMDeviceEnumerator,  0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
DEFINE_GUID(NFS_IID_IAudioClient,         0x1CB9AD4C, 0xDBFA, 0x4C32, 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
DEFINE_GUID(NFS_IID_IAudioRenderClient,   0xF294ACFC, 0x3146, 0x4483, 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2);
DEFINE_GUID(NFS_KSDATAFORMAT_FLOAT,       0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);

#define RATE 48000

static DWORD WINAPI audio_thread(LPVOID unused){
    (void)unused;
    IMMDeviceEnumerator *en = 0; IMMDevice *dev = 0; IAudioClient *ac = 0; IAudioRenderClient *rc = 0;
    HANDLE ev = 0; HRESULT hr;
    CoInitializeEx(0, COINIT_MULTITHREADED);
    hr = CoCreateInstance(&NFS_CLSID_MMDeviceEnumerator, 0, CLSCTX_ALL, &NFS_IID_IMMDeviceEnumerator, (void**)&en);
    if (FAILED(hr)) goto fail;
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    if (FAILED(hr)) goto fail;
    hr = IMMDevice_Activate(dev, &NFS_IID_IAudioClient, CLSCTX_ALL, 0, (void**)&ac);
    if (FAILED(hr)) goto fail;
    WAVEFORMATEXTENSIBLE wf = {0};
    wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE; wf.Format.nChannels = 2; wf.Format.nSamplesPerSec = RATE;
    wf.Format.wBitsPerSample = 32; wf.Format.nBlockAlign = 8; wf.Format.nAvgBytesPerSec = RATE * 8;
    wf.Format.cbSize = 22; wf.Samples.wValidBitsPerSample = 32; wf.dwChannelMask = 3;
    wf.SubFormat = NFS_KSDATAFORMAT_FLOAT;
    hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | 0x80000000u /* AUTOCONVERTPCM */ | 0x08000000u /* SRC_DEFAULT_QUALITY */,
            300000 /* 30 ms */, 0, (WAVEFORMATEX*)&wf, 0);
    if (FAILED(hr)) goto fail;
    UINT32 nbuf = 0; IAudioClient_GetBufferSize(ac, &nbuf);
    ev = CreateEventA(0, FALSE, FALSE, 0); IAudioClient_SetEventHandle(ac, ev);
    hr = IAudioClient_GetService(ac, &NFS_IID_IAudioRenderClient, (void**)&rc);
    if (FAILED(hr)) goto fail;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    fprintf(stderr, "[audio] WASAPI shared, %d Hz float stereo, buffer %u frames\n", RATE, nbuf);
    IAudioClient_Start(ac);
    for (;;){
        WaitForSingleObject(ev, 100);
        UINT32 pad = 0; if (FAILED(IAudioClient_GetCurrentPadding(ac, &pad))) break;
        UINT32 n = nbuf - pad; if (!n) continue;
        BYTE *p = 0; if (FAILED(IAudioRenderClient_GetBuffer(rc, n, &p))) break;
        /* these frames are heard after the ones still queued */
        pu_audio_render((float*)p, (int)n, RATE, (int64_t)pad * 1000000 / RATE);
        IAudioRenderClient_ReleaseBuffer(rc, n, 0);
    }
    fprintf(stderr, "[audio] device lost, sound off\n");
    return 0;
fail:
    fprintf(stderr, "[audio] WASAPI unavailable (hr 0x%08lx), sound off\n", (unsigned long)hr);
    return 0;
}

static DWORD WINAPI dump_thread(LPVOID path){
    FILE *f = fopen((const char*)path, "wb"); if (!f) return 0;
    uint8_t hdr[44] = {0}; fwrite(hdr, 1, 44, f);
    static float buf[2 * 480]; static int16_t pcm[2 * 480];
    uint32_t total = 0; LARGE_INTEGER fq, t0, t; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0);
    for (uint64_t done = 0;;){
        Sleep(5); QueryPerformanceCounter(&t);
        const uint64_t due = (uint64_t)(t.QuadPart - t0.QuadPart) * RATE / (uint64_t)fq.QuadPart;
        while (done + 480 <= due){
            pu_audio_render(buf, 480, RATE, 0);
            for (int i = 0; i < 960; i++) pcm[i] = (int16_t)(buf[i] * 32767.f);
            fwrite(pcm, 2, 960, f); done += 480; total += 1920;
            /* keep the header valid all the time (the process may be killed) */
            const uint32_t riff = 36 + total, rate = RATE, bps = RATE * 4;
            long here = ftell(f); fseek(f, 0, SEEK_SET);
            fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
            const uint32_t n16 = 16; const uint16_t one = 1, two = 2, four = 4, sixteen = 16;
            fwrite(&n16, 4, 1, f); fwrite(&one, 2, 1, f); fwrite(&two, 2, 1, f); fwrite(&rate, 4, 1, f);
            fwrite(&bps, 4, 1, f); fwrite(&four, 2, 1, f); fwrite(&sixteen, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&total, 4, 1, f);
            fseek(f, here, SEEK_SET); fflush(f);
        }
    }
}

void audio_host_start(void){
    static int started;
    if (started) return; started = 1;
    const char *h = getenv("NFS5_HEADLESS"), *na = getenv("NFS5_NOAUDIO"), *dump = getenv("NFS5_AUDIODUMP");
    if (dump && *dump){ fprintf(stderr, "[audio] mix -> %s (no device)\n", dump); CloseHandle(CreateThread(0, 0, dump_thread, (LPVOID)dump, 0, 0)); return; }
    if ((h && *h && *h != '0') || (na && *na && *na != '0')){ fprintf(stderr, "[audio] off (headless / NFS5_NOAUDIO)\n"); return; }
    CloseHandle(CreateThread(0, 0, audio_thread, 0, 0, 0));
}
