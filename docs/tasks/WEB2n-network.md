# WEB2n: network smoke test (native)

Goal: the game's own network code works through the runtime: two native instances in one race.
Status: **IP mode works** (two headless instances on 127.0.0.1, one race, both cars on both sides).
The N1 entry server (`_mod/netserver/server.js`) needs the wrapper's `net_entry` patches, so it comes with the
feature port / online step.

## What was needed

- **`runtime/win/winsock_win.c`**: WSOCK32 (27 imports, by ordinal) on host Winsock 2. Guest SOCKETs are small
  handles; sockaddr_in, fd_set (32-bit sockets) and timeval have the same layout; buffers are used in place. Calls
  that can block run without the guest lock. Winsock 1's IPPROTO_IP option numbers are mapped to Winsock 2's. The
  browser build needs its own version of this file. `NETLOG=1|2` logs calls.
- **Named mutexes / semaphores** (`win32_ext.c`, `named_open`): the client inside the host instance talks to its own
  server through EA's local queues (0x582CF0..: named semaphore + mutex + file mapping, ring buffer at 0x5831F0 /
  0x583300). `OpenSemaphoreA` / `OpenMutexA` were missing (3000 failed polls, the host never connected to itself). One
  process here, so a name is looked up in the object table; two instances are separate processes, no collision.
- **Semaphore wait took the whole count.** `WaitForSingleObject` / `WaitForMultipleObjects` zeroed `signaled` as for
  an auto-reset event, so a burst of queue messages lost its wake-ups. The ring reader peeks (wait + release) and pops
  (wait 0), so the last message stayed unread until the next one came: the host's `C6 0E` (logged in) was the last
  message, the game waited for it forever. Now a wait decrements. Worth remembering if other PU queues looked stuck.

## Findings

- `hostgame=1` / `joingame=<ip>.<port>` + `fe_player_filename=<profile>` are game arguments (`NFS5_ARGS`), the game
  connects before the frontend and opens `creategame` / `joingame` directly. Details in
  `_mod/docs/findings/NET0-mp-frontend-automation.md`.
- The joiner's frontend connects a second time and hangs (known from NET0): `nettest.c` skips it at 0x4D3D00 for the
  test only. A player using the menus (peer to peer, IP, Connect) does not take that path; **not tested yet**.
- A `hostgame=1` start without `SaveData/player.lst` crashes in sub_004def60: the accessor `UserList.UserList` does not
  exist, the code does not check. Test setup issue (the test copies a whole SaveData), not seen in the menus.
- Traffic in a 2-player race: ~10 packets/s each way, ~45 B each (UDP, port 9442, the same socket for everything).
  Useful for the browser relay: small and steady.
- When the joiner quits, the host keeps running; the remote car stops.

## Test

`tools/net_test.sh <work dir> [seconds] [SaveData folder]`: two headless instances with `NETTEST=host|join`, own save
folders, AI drives (`NETTEST_AI=1`). `runtime/nettest.c`, hooked at the frontend frame 0x4D2B80 (`gen.sh` HOOKS), presses
READY / PLAYGAME through the command dispatcher 0x50D6D0, a monitor thread logs comms state, sim tick and the car slices
every 3 s. Prints PASS when both got the race start and both cars move on both sides. Without `NETTEST` the hooks return
at once.

## Progress

- 2026-11-05: PASS (110 s and 170 s runs). Host / joiner at about the same sim tick show the same cars within 1-4
  slices (a slice = 4-5 m on stock tracks): host sees the joiner's car 1-4 slices back, joiner the host's 0-2 back. No
  exceptions. Host 1484 packets sent / 1234 received over 130 s of race.
- Not tested: two real PCs, the menu path, more than 2 players, end of a race (a network race never ends by itself,
  every player leaves through the race menu), human driving.
