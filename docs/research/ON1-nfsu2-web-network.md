# ON1: nfsu2-web's network stack, and how PU's Internet mode reaches N1 in the browser

Research 2026-11-13, read-only. Reference: github.com/th0mk/nfsu2-web at **d59318902112eb7bc5fd9b1be65266ac0352990f**
(the same revision as `_mod/web/upstream/REVISION`). It was a full clone with that commit checked out, not HEAD. Their
**old Node server** (before they moved to Rust) is in their history at `6826c08d` (the parent of `dbd9c999` "Server: the
Rust one only"): `web/server/lan.mjs` (731 lines), `web/server/online/vhost.mjs` (200), `online/index.mjs` (65). Those
are closer to our Node `web/server.mjs` than the current Rust crates. All of it is **MIT** ("Copyright (c) 2026 th0mk,
portions Talha Müderrisoğlu"). Our `LICENSE` already credits them and points to `licenses/nfsu2-web.txt`, so copying
is fine as long as the notice stays.

## 1. Their design, end to end

```
guest thread (lifted DirtySock)        -- ws2_32 import -->  runtime/winsock.c   (Windows semantics, SOCKET table)
   vlan_* / vfd_* backend                                    runtime/web/net_lan.c (virtual IP stack, per browser)
   TX ring --(shared memory, Atomics bell)-->                web/src/net/worker.ts + index.ts Link (net worker)
   --> one WebSocket  wss://<origin>/lan  -->                switch (web/server-rs/crates/lan; was web/server/lan.mjs)
       routes by destination vip; 10.77.0.1 -> online host   (crates/online vhost.rs; was online/vhost.mjs)
   RX ring <--(page writes, rings rx_bell)--                 net_lan.c LAN thread (pthread, never holds the guest lock)
```

### 1.1 C side

| Piece | File / function | What it does |
|---|---|---|
| Winsock front end | `runtime/winsock.c` (2196 lines) | Every ws2_32/wsock32 import (by name or ordinal, `k_ords`, which are the same numbers as WSOCK32 1.1), `WsSock` table (handles `0x1000+4n`), Windows error codes, Windows bind / connect / select / close semantics. Modes `off`, `vlan` in wasm (`lan`/`loopback` are Mac-only BSD paths, `#ifndef __wasm__`). On wasm it `#include "web/lan_vfd.h"` and `#define`s `poll/send/recv/shutdown/close` to `vfd_*`, so the vlan code paths stay the same as on the Mac. |
| Blocking | `swait()` (l.735), `ws_select()` (l.1749), blocking `ws_connect` | Every wait is `guest_wait_begin()` + `poll()` in slices of 50 ms or less + `guest_wait_end()`. `closesocket` from another thread sets `closing` and wakes the waiter with WSAEINTR. Non-blocking connect always returns WSAEWOULDBLOCK and finishes through select (writable, or exceptfds on failure). |
| DNS | `ws_gethostbyname()` (l.1909) | own name / `localhost` / dotted quad answered locally; `ONLINE_HOST` (`pcnfs05.ea.com`) -> `10.77.0.1` when `NFSU2_ONLINE` is on; anything else -> `WSAHOST_NOT_FOUND` (logged as BLOCKED). The `hostent` is built in guest memory at `GBUF_BASE 0x00BE0000` (one 512-byte slot per thread). |
| Game patch for online | `online_patch()` (l.300) | In guest memory `"*pcnfs05.ea.com"` at 0x7FBDBC becomes `"pcnfs05.ea.com"`, so DirtySock uses plain TCP 20920 instead of SSL. That is their only patch to reach the lobby. |
| Virtual stack | `runtime/web/net_lan.c` (1337 lines) | Sockets `VSock[1024]` (tcp/udp/raw-icmp), connections `VConn`, virtual fds `VFD_BASE+n`. UDP: unicast by port, broadcast to every bound socket including the sender's own, "port unreachable" back to the socket that sent. TCP over the switch with no retransmission: `SYN -> SYNACK/RST, DATA, FIN, RST`. Each frame carries the 4-tuple and the connector's random `cid`. Connect timeout 3 s, host unreachable when the vip is not in `peer_bits`. ICMP: a raw socket sees every ICMP message, and an echo request is answered by the receiving browser (`rx_frame`). Loopback and own-vip traffic never leaves the browser. Flow control: `TX_RESERVE` 4 slots, a control-frame queue `g_ctlq`, and the page never drops TCP. Token-bucket limits for auto-answers (`auto_ok`). |
| Wake-ups | `bump()` / `g_seq` | One event word. `vfd_poll` waits on it with `memory.atomic.wait32` (lock released by the caller). The LAN thread `lan_main` waits on `rx_bell` (50 s timeout, 2 s while control frames wait). |
| Address | `vlan_hello()` / `vlan_alive()` / `ident_from_page()` | The first WSAStartup registers the mailbox (`HOST_KIND_LAN`), starts the LAN thread, sets `want=1` and waits up to 5 s (guest lock released) for `page_state==ONLINE`. It then takes `vip/netmask` from the header, a MAC of `02:4e:56:4c:..` and the name `VLAN-<host#>`. The last WSACleanup sets `want=0`, and the page lets the switch go after a while (`NET_PAGE_IDLE`). |
| Ring layout | `runtime/web/net_proto.h` (193 lines) | 512-byte header (magic `NTN1`, version 4, mode, `tx_head/tx_bell/rx_tail/want/udp_ports` C->page, `page_state/vip/netmask/generation/tx_tail/rx_head/rx_bell/stats/peer_bits[64]` page->C) + TX 128 x 1536 + RX 256 x 1536 slots (`len,kind,src_ip,dst_ip,sport,dport,t_ms,data[1504]`), so about 590 KB. Frame kinds 3/4/5 = LAN UDP/TCP/ICMP, and an 8-byte LAN header `[op][ttl][0][0][cid LE]` before the data. Kinds 1/2 and 16-21 are free roam and page-only extras that we do not need. |

### 1.2 Page / worker side (TS)

| File | Role |
|---|---|
| `web/src/net/worker.ts` (127) | A dedicated worker that runs the `Link`, so no datagram waits for the main thread. It receives `{t:'init', memory}` and `{t:'reg', kind, ptr, size}` and posts state back. |
| `web/src/net/remote.ts` (173) | The page's proxy for that worker. It falls back to running the Link on the main thread if the worker does not start within 10 s. |
| `web/src/net/index.ts` (1147) | `Link`: one WebSocket to `/lan` (`binaryType='arraybuffer'`), with a `{"t":"hello",name,lan,udp,bundle,...,vip?,token?}` hello. `welcome{vip,mask,token}` writes vip/netmask/generation into every mailbox and sets `page_state`. `peers`/`delta` go into the `peer_bits` bitmap; the online host is listed as `server:true`, so it is in the bitmap and hidden from players. `txLoop`: `Atomics.waitAsync(tx_bell)` -> `drainTx`, which builds BE frames `[kind][0 0 0][src][dst][sport][dport][data]` (copied out, because a socket cannot send shared memory), bundles them, and holds LAN frames for 1 ms (`TX_HOLD_LAN_MS`). `rx`: by kind into the RX ring, then `Atomics.notify(rx_bell)`. While the ring is full, TCP frames wait in order (`Endpoint.waiting`). A peer that leaves causes a synthetic `NET_LAN_TCP_GONE` frame. Reconnect uses back-off 250 ms .. 5 s and `?resume`, and the token gets the same vip back. A ping every 10 s is written to `switch_rtt_ms`. Most of the file is free-roam features (compact/delta bundles, fpred, friends, crews, rooms, kicks/bans, the queue). |
| `web/src/net/proto.ts` (345) | The same constants as `net_proto.h` (a test checks that they agree), `ipStr/ipNum`, unpacking of compact bundles. |

### 1.3 Server side

| Piece | Now (Rust) | Old Node (6826c08d) | What it does |
|---|---|---|---|
| Switch | `crates/lan` (lib.rs 1819 + 13 modules) | `web/server/lan.mjs` 731 lines, `ws` package | Hands out addresses round robin over 10.77.0-55.x (Rust /18) or 10.77.0-7.x (Node /21, mask 255.255.248.0; host bytes 2..251, so .1 and .253 stay reserved). A 128-bit token per address, reserved for `reserveMs` after a drop. It rewrites the source vip (`data.writeUInt32BE(vip,4)`, so nobody can spoof) and routes by destination. A broadcast of kinds 3-5 goes only to pages with LAN open (and with the UDP port bit). Bundles (kind 32). Per-peer/group/site token buckets. A send buffer cap disconnects slow peers. `route()` hands frames for `online.vip` to `online.frame(vip, data, ctx)` and for `roam.vip` to the hub. |
| Online host 10.77.0.1 | `crates/online/vhost.rs` 534 + services | `online/vhost.mjs` 200 + `online/index.mjs` 65 | A **virtual host** that speaks the browsers' LAN frames: TCP endpoint (SYN on a listened port -> SYNACK, else RST; DATA in order; FIN; RST; `conn_write` chunks of 1464), ICMP echo answered, **UDP always "port unreachable"** (NFSU2 online is TCP only). `listen(port, service)`: 20920 redirector, 20922 lobby, 13505 EA Messenger, 80 http. `peerGone(vip)` drops that browser's streams. |
| Auth | none for play (`deploy/README.md`: "no accounts or passwords", firewall/Cloudflare decides) | same | Lobby login accepts any name except a slur. The password, CD key and checksum are never decoded (`aries.rs SECRET_KEYS`). Optional "claimed names": a keyphrase token goes in the hello (`claim`) and is checked by `crates/claims`. Operator kicks and bans go over a control socket. |
| Deploy | `web/deploy`: one static binary + systemd, TLS by Caddy, port 443, or a Cloudflare Tunnel (`cloudflared.yml`). `/lan` is a plain WebSocket upgrade on the page's origin. | Docker image (removed) | COOP/COEP headers, as we already do. |

**Race traffic in NFSU2** goes browser to browser through the switch (UDP 3658, a star around the creator), and the
online host never sees it. **In PU's Internet mode it is the other way round:** everything, race state included, is
UDP between the client and the entry host. So our virtual host must be a **UDP endpoint**, which theirs never is.

## 2. PU's side (what the game calls)

`PORSCHE.EXE` imports **WSOCK32.dll by ordinal** (27 functions, list in `_mod/docs/findings/N1-internet-entry-server.md`
"WinSock trace") **and ICMP.DLL** (`IcmpCreateFile`, `IcmpSendEcho`, `IcmpCloseHandle`, confirmed in the EXE's import
names). Our runtime has no ICMP.DLL handler today, native or web.

| Call | PU use | Notes |
|---|---|---|
| `socket` | `(2,2,17)` UDP session socket at 00560555; `(2,1,0)` TCP at 00581293/0058189F (the TCPDATA fallback); `(2,1,proto)` probe + close at 00583A2D; `(6,2,1000)` = **IPX** at 00560845/00560DA2 (LAN mode, impossible, must fail cleanly) | no raw sockets: the ping is ICMP.DLL |
| `setsockopt` | `SOL_SOCKET SO_BROADCAST`, `SO_KEEPALIVE`, `IPPROTO_TCP TCP_NODELAY`, IPX `0x400F` | no IP-level options, so Winsock 1 vs 2 option numbers do not matter here |
| `bind/sendto/recvfrom/select/ioctlsocket(FIONBIO)` | UDP session on an ephemeral port (trace: bind port 0) | non-blocking polling (WSAEWOULDBLOCK counted) |
| `connect/send/recv/closesocket` | TCP fallback to entry:9442 every ~8 s, `[u16 len][ff][00]c<acct>...`, closed after 2.5-5 s | `server.js` only logs it |
| `gethostbyname` | 005805B0 resolves the registry `Primary Address` (default `porsche-entry.earacing.com`, 004B7240 index 6), port `[005B4990]` = **9442**; `gethostname` + own lookup for the session name `%c%s.%d.%s` | |
| `IcmpSendEcho` | server-list rows (`B8` text `"a.b.c.d"`): 0048C6E0 -> 0055DA10 queues request type 8 (address via `inet_addr`) to a worker queue at 006A3B60. **A row appears only after the echo answers.** | blocking call on a game thread, so it must release the guest lock |

Other preconditions (native N1 needs them too): the Castanet updater must be off (`fe.txt castanet=0`, or skip
`004A56E0`, whose `[00657E30]==0` path means "no update"). Otherwise the Internet screen hangs on *Checking for
updates*. The clean folder's `fe.txt` has `castanet=1` twice.

**Our runtime today:** `runtime/win/winsock_win.c` (181 lines, native only) passes WSOCK32 straight to host Winsock 2,
releasing the guest lock around blocking calls, and maps the Winsock 1 IP options. On the web build,
`runtime/pu_stubs.c` has weak `winsock_call()` returning 0. Every WSOCK32 call then goes down win32.c's
"unimplemented" path and returns 0, so `WSAStartup` "succeeds" and `socket()` returns handle 0. The game believes it
has a network. `host_abi.h` already reserves `HOST_KIND_NET=4` and `HOST_KIND_LAN=7` (copied from them), and
`abi.ts` has `HostKind.Lan`. `game-worker.ts` forwards `register` to the page and `process.ts registered(kind)` exists,
but there is no net worker yet. The guest lock is the same as theirs: `guest_wait_begin/end` in `win32_ext.c` l.415 =
`gil_release/gil_acquire`. Game threads are wasi threads (workers), so `memory.atomic.wait32` is allowed. Host
pthreads work (`audio_web.c`). `web/server.mjs` is a static server with Basic auth today. It has no WebSocket and
no `ws` dependency yet.

## 3. What we can take nearly verbatim

| Take | From | Changes for PU |
|---|---|---|
| `net_proto.h` | `runtime/web/net_proto.h` | Keep the layout and version, and drop the roam-only fields from the docs (or keep them unused). |
| `lan_vfd.h` | as is | none |
| `net_lan.c` | as is, minus race_note/`vlan_rt_*`/`vlan_race_*`/`racenet.h` (NFSU2 race-link stats, quick chat) | about 150 lines removed |
| `winsock.c` | wasm paths of `runtime/winsock.c` | Remove: `online_patch` (0x7FBDBC), `online_lobby_read`/`olfilter_*`, NetTick `sub_0074b730` override, Mac `deny_*`, `module_note_socket_call`, Netbios (PU does not import netapi32). Change `ONLINE_HOST` to `porsche-entry.earacing.com` (and allow `10.77.0.1`). Make `write_wsadata` report **1.1** (`0x0101`, iMaxSockets 256, iMaxUdpDg 65467, as winsock_win.c does). Check `GBUF_BASE 0x00BE0000` against PU's address space, or use `gheap_alloc` as winsock_win.c does. Only on wasm: native keeps `winsock_win.c`. |
| switch | old `web/server/lan.mjs` (Node, `ws`) | Trim to addresses + tokens + hello/welcome/peers/delta/ping + `route()` + LAN-only broadcast + buckets. That is about 300 of 731 lines: no roam, no compact/delta bundles, no bans/queue. |
| virtual host | old `online/vhost.mjs` | Keep TCP + ICMP. **Add a UDP port table** in place of "always unreachable". |
| net worker | `net/worker.ts`, trimmed `net/index.ts` (Endpoint + connect/control/txLoop/drainTx/rx/lanIn/peerGone/refuse), LAN subset of `proto.ts` | Drop the main-thread fallback (`remote.ts`). We require cross-origin isolation anyway. |

## 4. Port design for nfs5-web

### 4.1 C (runtime)

- New `runtime/web/winsock_web.c` (= their winsock.c wasm path, trimmed) + `runtime/web/net_lan.c` + `net_proto.h` +
  `lan_vfd.h`. Add `winsock_web` to `web/build.sh`. That overrides the weak stub, and only wasm builds see it.
  Mode is always `vlan`. `NFS5_NET=off` gives the old "no network" behaviour (WSAStartup -> WSASYSNOTREADY).
- **Socket table:** their `WsSock[1024]`, handles `0x1000+4n`. Each guest socket has a `VSock` (vid) and a vfd. TCP
  gets its fd at connect/accept time. `AF_INET` only: `socket(6=AF_IPX,...)` -> `WSAEAFNOSUPPORT`, which the game
  already handles natively when IPX is missing.
- **Ring:** one `HOST_KIND_LAN` mailbox, net_proto.h v4 as is. TX 128 slots is plenty: PU sends about 10 packets/s
  of about 45 B in a race (WEB2n).
- **DNS:** `porsche-entry.earacing.com` -> 10.77.0.1. Dotted quads and own name are handled locally, everything else
  is not found. No EXE patch is needed for the host or the port: the registry default is the EA name, and the port
  stays **9442**. The local clash that forced 9443 natively cannot happen on separate vips. Optional: let a page
  option seed `Primary Address` = `a.b.c.d.port` through the registry HLE (`win32_ext.c` `reg_find`).
- **ICMP.DLL HLE** (new, about 120 lines, in winsock_web.c). `IcmpCreateFile` returns a handle. `IcmpSendEcho(h, dst,
  data, n, opts, reply, replySize, timeout)` opens an internal raw VSock (`vlan_open` kind 3), `vlan_sendto` sends
  type 8 with id/seq, then `vfd_poll` waits up to `timeout` with the guest lock released, `vlan_recvfrom` strips the
  20-byte IP header and matches id/seq, and the call fills `ICMP_ECHO_REPLY` (Address, Status 0, RoundTripTime,
  DataSize, Data ptr, Options) and returns 1. On timeout it returns 0 with `IP_REQ_TIMED_OUT` (11010). Natively,
  `winsock_win.c` should pass ICMP.DLL to the host's `IcmpSendEcho` (about 20 lines). Our native N1 test needs the
  rows too.
- **Castanet:** a hook or a patch so that 004A56E0 always takes the "no update" path (better than editing the
  player's `fe.txt`). Natively it is the same.
- **Blocking under the guest lock:** as theirs. Every wait in `swait`/`select`/`connect`/`vlan_hello`/`IcmpSendEcho`
  is `guest_wait_begin()`, then `vfd_poll()`/`atomic.wait32` in slices of 50 ms or less, then `guest_wait_end()`. The
  LAN thread is a host pthread that never takes the guest lock and holds only `g_mu`. `g_mu` is never held while
  taking the guest lock.

### 4.2 TS (page)

- `web/src/net/{proto.ts, link.ts, worker.ts}`. The **WebSocket lives in the net worker** (as ARCHITECTURE.md
  already shows). `main.ts`: `p.registered(HostKind.Lan).then(r => netWorker.postMessage({t:'reg', ...r}))` after
  `{t:'init', memory}`. The worker connects only when the mailbox is registered and `want=1` (first WSAStartup), and
  goes idle after the last WSACleanup.
- `link.ts`: from `index.ts` keep `Endpoint` (LAN only), `connect/control` (welcome, peers, delta, pong), `txLoop`/`drainTx`
  (kinds 3-5), `rx` -> `put()` with the TCP waiting list, `peerGone` (`NET_LAN_TCP_GONE`), `refuse`, reconnect with
  resume + token. About 350 lines. A small status hook (vip, rtt, online) feeds the overlay.

### 4.3 Server (Node, `web/server.mjs`)

- Add the `ws` dependency. Add an `upgrade` handler for `/lan` with the **same password** as the HTTP side. Do not
  count on browsers sending Basic credentials on a WebSocket upgrade. Set an HttpOnly session cookie after the Basic
  check and verify it on upgrade, plus an Origin check.
- `web/server/lan.mjs`: the trimmed switch (4.1 frame format, /21 addresses, tokens, peers with
  `{vip:10.77.0.1, name:'NFS5 Online', server:true}` so `peer_bits` has it; without that, a TCP connect to it
  fails with WSAEHOSTUNREACH).
- `web/server/vhost.mjs`: the old `VirtualHost` (TCP + ICMP echo) plus `udpListen(port, onDatagram)` and
  `udpSend(vip, sport, dport, data)` (kind 3, op 0). It answers UDP to an unbound port with UNREACH. TCP 9442: accept
  and drain/log, which is what N1 does now. RST also works: the game only falls back.
- **N1 entry server as host 10.77.0.1: embed it in-process (recommended).** Refactor `server.js` (use the live copy
  in `nfs5-reforged/netserver/server.js`, 936 lines; `_mod/netserver/server.js` is 878) into a module
  `createEntryServer({ transports, log })` with `onDatagram(buf, peer)`. The only socket touchpoints are
  `udp.send` (3 places: l.80, 105, 855), `udp.on('message')`, `bind`, and `NFS_SERVER_ADDR` in `serverEntry()` (the
  `B8` row text the client pings). `peer` = `{key:'v:<vip>:<port>', send(buf), pingAddr:'10.77.0.1'}` for browsers,
  `{key:'<ip>:<port>', send, pingAddr:PUBLIC}` for a real `dgram` socket. The B8 text then becomes `peer.pingAddr`,
  so each client pings an address it can reach. `node netserver/server.js` keeps working standalone (real UDP only).
  - Why embed: no UDP port to open on the host or tunnel (a Cloudflare Tunnel cannot carry UDP at all). Client
    identity is the true vip:port, not a NAT-like mapping. There is one process to deploy. The same lobby can later
    hold **PC players over real UDP and browsers** at once (cross-play falls out, because all race state is relayed
    by the server).
  - **Bridge** (vhost forwards each browser vip:port to its own `dgram` socket -> `127.0.0.1:9443`) needs no change to
    server.js. It is good for a first smoke test, but the B8 ping text must then be `10.77.0.1`, which breaks PC
    clients on that server, and it adds a socket per browser. Use it for the first test only.

### 4.4 Effort (estimate)

| Piece | Days |
|---|---|
| winsock_web.c + net_lan.c port, build, WSADATA 1.1, IPX refusal, DNS | 1.5 |
| ICMP.DLL HLE (web) + native passthrough | 0.5 |
| Castanet/updater skip | 0.25 |
| TS net worker + link (trimmed) + main.ts wiring | 1 |
| Node switch (trimmed lan.mjs) + `/lan` upgrade + auth cookie | 1 |
| vhost with UDP + ICMP, server.js refactor to an embeddable module | 1 |
| Tests: two headless pages + server (login -> lobby -> server row ping -> game -> race start), scripted like `tools/net_test.sh` | 1-1.5 |
| **Total** | **about 6-7** |

Free on top: browser-to-browser IP mode (peer-to-peer join by `10.77.x.x`) through the same switch.

## 5. Risks

- **Wrapper-side N1 features are not in the web runtime.** The MODE combo race modes (and the HS8 GetString repoint
  that turned them off), police cars, PvE bots (`net_*` hooks), `net_smooth` (hook 004A0D3E) and `net_send_rate`
  (0048EE20). `server.js` PvE/pursuit paths assume them. Start with PvP only and port the hooks later. Pursuit is
  dropped anyway.
- **The server list needs the ICMP echo.** If `IcmpSendEcho` is missing or slow, there is no row, so no login. The
  echo goes over WebSocket + switch, so its RTT is the real one plus queueing. The vhost answers echoes immediately.
- **The reliable link (llpacket) timers** (1 s ack/keepalive, open-handshake within 5 s of `'a'`, CONMAN hello
  `[FE][01]` within 5 s) have to hold through `vlan_hello`'s 5 s wait and the WebSocket connect. Connect at the first
  WSAStartup, before the login screen, which their `want` logic already does.
- **The UDP port-bit filter** (`udp_ports`, port % 31) only affects broadcasts. PU's Internet mode is unicast. Fine.
- **Winsock 1.1 details.** Their HLE is Winsock 2. Check the WSADATA fields, `inet_ntoa` buffer, `hostent` slot
  memory (`GBUF_BASE`) and `select` with `nfds` ignored. Compare against the native WEB2n trace (`NETLOG=2`).
- **Basic auth on the WebSocket** (see 4.3). Without it the switch is open to anyone who can reach the port.
- **Address in payloads.** The join request copies the 20-byte sockaddr the client sent to (10.77.0.1:9442 in the
  browser) and the client's own (vip:ephemeral). server.js copies them back unchanged, so this works. The session
  name `s<vip>.<port>.<name>` is informational.
- **The unimplemented-import fallback today** makes WSAStartup look successful on web. Until the port lands, the weak
  stub should fail `WSAStartup` with WSASYSNOTREADY so the game shows its own error instead of using handle 0.
