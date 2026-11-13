# AUD1: sound latency in the browser

2026-11-13. Goal: the least delay between what happens in the game and what is heard.

## What the delay was

Everything the game plays goes through one looping DirectSound buffer: 51200 bytes, 32 kHz 16-bit stereo (400 ms),
flags 0x18000, made at 0058F172. Its writer is `sub_0058F8F0`, run from the game's sound thread (loop at 0058F880:
callback, write, `Sleep([006B48E1])` = 8 ms). Each pass asks the play cursor (GetCurrentPosition from 0058F945) and
writes up to *play + lead*:

| Address | What |
|---|---|
| 006AD790 | lead, frames ahead of the play cursor; starts at 20 ms (rate/50) |
| 006AD794 | lead cap: 0.8 x the buffer = 320 ms |
| 006AD78C | write position, frames |
| 006AD77C / 006AD780 | frames written / played so far |
| 006AD7A4 / 006AD7A0 | passes since the last underrun / least margin seen |
| 006AEFF4 | passes with a cursor that did not move (over 100: Restore) |

`sub_0058FCD0` adapts the lead: when the cursor has passed the writes (an underrun) it adds the whole deficit (+15,
up to the cap) and resets the pass count; only after 3000 passes (~30-45 s) does it lower the lead, 16 frames
(0.5 ms) every 10 passes, while its least margin is over 16 frames. So one stall (a load, a slow frame) left the lead
at 100-250 ms for minutes. Measured in a browser race: 250 ms in one run, 112 ms falling to 54 ms in another.

On top of that the C mixer thread kept 50 ms in a ring for the worklet, and the browser's output buffer adds its own
(Chrome on Windows: base 10 ms + the device). Heard delay: about 110-310 ms.

## What it is now

- `web/src/audio/worklet.ts` mixes in the AudioWorklet, straight from guest memory (wasm memory is shared): the table
  `AudioShare` in `runtime/pu_com.c` (AUD2: buffer list under a seqlock, muted, cooperative level, wake word) and the
  buffers' own fields (`BUF_*`). No ring, no C mixer thread.
- A streamed buffer (looping, written in parts) is read just behind the game's last write: `Unlock` stores where the
  write ended (`BUF_WHEAD`, +80) and counts the partial writes (`BUF_WCNT`, +84). The reader keeps the least margin in
  each half second at 2 ms (+3 ms after each underrun, given back 1 ms per 5 s) by trimming its rate by at most 0.5 %;
  far behind (over 60 ms) it jumps; past the writes it goes silent until 12 ms are written again (no stale sound from
  400 ms before). Other buffers are read at their play cursors on the host clock, as the C mixer did.
- Because the reader follows the writes, the game's lead no longer adds delay. Its lowering is switched off
  (`MR32(006AD7A4) = 0` when the writer asks the cursor): lowering it made the writes slower than real time (2 % with
  the wake-ups below) and drained the margin. Raising it after a stall stays.
- The game's sound thread sleeps on the wake word (`pu_audio_wake`, Sleep from 0058F8B5 in `runtime/win32_ext.c`)
  and the worklet wakes it with `Atomics.notify` when the margin is under 16 ms. A timed wait alone woke up to 15-39 ms
  late (the Windows timer tick in Chrome), 20-27 times in 10 s of a race; the lock wait was not the cause (avg 0.1 ms).
- `AudioContext({ latencyHint: 0 })`.

## Measured (headless Chrome, fe=0 AI race, `?env=AUDIOSTAT=1`)

| | before | after |
|---|---|---|
| game lead (write - play) | 54-250 ms | not on the path |
| queued between game and output | ring ~45 ms | margin avg 16-18 ms (least 1-4 ms) |
| writer gaps over 15 ms per 10 s | 52-68 (longest 25-53 ms) | 0 (longest 11-13 ms) |
| underruns per 10 s after the start | - | 0 in most windows, 1-2 in some (machine under load) |
| output rms in a race | - | 0.13-0.18 |

Chrome renders 4 quanta (10.7 ms) at once, so the margin cannot go much below one burst plus the writer's response.
Left over: the output buffer of the browser and device, and the first seconds of a race (loads stall the writer).

## Diagnostics

- `?env=AUDIOSTAT=1`: every 10 s the game's write gaps (`[audiostat] game writes`) and the worklet's margins,
  underruns, wake-ups, rms and the context's latencies (`[audiostat] worklet`).
- `?env=SLEEPSTAT=1`: short sleeps of the guest, how late they wake and how long they wait for the lock.
- `?env=NFS5_AUDIOMIX=ring`: the previous ring path (AUDIOSTAT then prints the ring's fill and the game's lead).

A person has not listened to it yet; the tests are muted and read numbers.
