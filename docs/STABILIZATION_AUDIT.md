# Stabilization audit (branch `claude/elegant-mendel-wplopx`)

Status labels used throughout:

- **Static review** — read the code; nothing executed.
- **Simulated** — ran the real JS module (or the exact functions extracted from it) in Node with stubbed browser APIs.
- **Compiled** — none. No Windows toolchain was available; the C++ has never been compiled on this branch.
- **Real-world validated** — none. See `WINDOWS_VALIDATION.md`.

---

## 1. Codec override (`TEMP: force H.264`)

### What it was

Two overrides, not one:

- `network.js`: `applyCodec(2)` on `CODEC_CAPS`. This ignored the user's saved choice and the client's detected best codec.
- `ui.js` (`updateCodecOpts`): after computing a fallback, it unconditionally set `codecSel.value = 2` and `S.currentCodec = 2`.

### Why it was most likely added (static review)

The repo has a single squashed commit, so there's no history to confirm this. The code gives a strong candidate cause, though:

1. **The host's capability probe could not tell what the GPU supports.** `ProbeSupport` and the old `ProbeHardwareSupport` only called `avcodec_find_encoder_by_name`. That checks whether FFmpeg was *compiled* with an encoder, not whether the GPU can run it. vcpkg's FFmpeg (`nvcodec`, `qsv` features) ships `av1_nvenc`, `hevc_nvenc` and the rest on every machine. So:
   - an RTX 20/30-series GPU (no AV1 encoder) advertised AV1;
   - the client's detection picks AV1 first whenever the browser can decode it, so it chose AV1;
   - on the host, `av1_nvenc` failed to open, then QSV and AMF failed too, and it fell back to `libsvtav1` on the CPU. That is unusable at desktop resolution and frame rate. With no software AV1 encoder in the build, the result was "No encoder available" and a black stream.
2. **Codec switches broke the stream** (fixed in the previous commit). Any switch away from the host's default (AV1) caused decoder errors and keyframe stalls, and H.264 is the codec that "just works".
3. **AV1 keyframe requests on NVENC.** `forced-idr` was set for H.264/HEVC but not for AV1. A recovery request could therefore produce an intra frame without the key flag, and the client would keep asking.

Cause 1 fully explains a "force H.264" hack. It is the one fixed with the most care below.

### What changed

| Area | Change |
|---|---|
| Host probe | `ProbeHardwareSupport` now opens a real hardware session per codec at 1280×720@60 with software fallback disabled (new `allowSoftware` constructor flag). |
| Advertised caps | `SelectAdvertisedCodecs`: offer only hardware codecs when the GPU has any. With none, offer software H.264 only (x264 ultrafast copes). Only if even that is missing, whatever the build has. |
| Host keyframes | `forced-idr=1` for AV1 on NVENC too. On FFmpeg builds without the option, `av_opt_set` fails and only logs. |
| Constructor leak | A throwing `VideoEncoder` constructor no longer leaks device/context references (relevant now that probing constructs and discards encoders). |
| Client choice | Overrides removed. Choice = saved preference if both sides support it, else the first entry of a priority list that puts hardware decode first (AV1 hw, HEVC hw, H.264 hw, then H.264 sw, AV1 sw), else H.264. `detectCodecs().best` uses the same ordering. |
| Fallback | If the browser can't configure a decoder it claimed to support, `requestCodecFallback` asks the host for the next codec, H.264 first, skipping codecs that already failed. If the host rejects a `CODEC_SET`, its ACK carries the old codec and the dropdown follows it. |

### Known codec risks that remain (need Windows validation)

- **Codec strings declare low levels.** `hev1.1.6.L93.B0` is HEVC Main level 3.1 and `avc1.42001f` is H.264 Constrained Baseline 3.1. NVENC emits High-profile H.264 and streams above 720p. Chromium's Windows decoders generally ignore the declared level for Annex-B streams. Strict decoders (Safari, some Android) may not. Not changed, because a different string changes `isConfigSupported` results and I can't test that here.
- **The probe runs at 1280×720.** A GPU that encodes 720p but not the actual source size (e.g. H.264 above 4096 px wide) still advertises the codec. The encoder then fails at stream time; on a rebuild the previous encoder stays installed, so this degrades rather than dying.
- **Probe cost.** Up to three hardware session opens at startup, roughly 0.1–1 s in total. Check the time between `Probing encoder support` and `Hardware codec support` in the log.
- **Firefox.** No HEVC decode in WebCodecs. Handled by detection plus fallback; simulated, not run.

### Is negotiation end to end?

Static review plus simulation: client detection → host caps → saved/priority choice → `CODEC_SET` → host rebuild (or rejection) → `CODEC_ACK` → decoder follows the frames' codec tags. A configure failure in the browser triggers a fallback request. The Node harness (`media.js` with stubbed `VideoDecoder`) exercised: stream-driven switch with the keyframe arriving before the ACK; late old-codec deltas (dropped, and they no longer tear down the new decoder — the harness caught this bug and it's fixed); same-codec encoder-generation change; an untagged legacy stream; and fallback on configure failure. **11/11 pass. Not validated against a real browser or host.**

---

## 2. TURN credential

The value is deliberately not reproduced here.

### Where it is

- `src/host/net/webrtc.cpp`: a long-term TURN user and password for a raw IP on port 3478, enabled by default (`SLIPSTREAM_ENABLE_TURN` defaults to true).
- `client/js/network.js`: the same username and password for a `turns:` hostname on port 443.
- `client/js/network.js.before-turn443`: the same credential.
- The public GitHub repository and its initial commit.

### Classification

- **Long-lived.** It's a static TURN long-term credential (RFC 5389/8489 username + password). It never expires and works until someone changes it on the TURN server.
- **What it grants.** Anyone holding it can allocate relays on that TURN server: relay arbitrary UDP (and TCP, if allowed) to any peer address the server permits, using the server's bandwidth. Unless coturn sets `denied-peer-ip` for private ranges, an allocation can reach the server's own loopback and LAN (TURN-as-SSRF). Whether quotas and peer restrictions are set can't be seen from this repo; check `turnserver.conf`.
- **Actual exposure.**
  1. **The repository is public**, so the credential is effectively published.
  2. The static routes (`/`, `/js/*.js`) are served **without authentication**. Anyone who can reach the host's HTTPS port can fetch `network.js` and read it, without logging in.
  3. Every client that loads the page gets it.

**It needs rotation — treat it as compromised now**, independent of the redesign. Rewriting git history afterwards is optional; rotation is what actually closes the hole.

### Also found while auditing: `VPS.conf`

`VPS.conf`, committed to the same public repo, is a WireGuard client config with an **interface private key**, plus the peer public key and endpoint (the same server IP as the TURN host). Anyone holding that private key can impersonate this peer on the WireGuard tunnel (`10.50.0.0/24`). **Rotate it** (generate a new key pair, replace the peer's public key on the server), remove the file from the repo, and add it to `.gitignore`.

### Proposed design (not implemented, pending your go-ahead)

Use coturn's TURN REST shared-secret mode (`use-auth-secret` + `static-auth-secret`, draft-uberti-behave-turn-rest), so no reusable secret ever reaches a browser.

1. **Secret storage.** The host reads the TURN shared secret from configuration, never from source: `%APPDATA%\SlipStream\turn.json` (or the service's ProgramData directory later), protected with DPAPI (`CryptProtectData`, machine or user scope) or a restrictive ACL. Also store `urls` (`turns:host:443?transport=tcp`, `turn:host:3478?transport=udp`). With no config, TURN is off: no default server.
2. **Short-lived credentials.** A new authenticated endpoint (`GET /api/ice`, behind `AuthRequired` like `/api/offer`) returns:
   - `username = "<unix-expiry>:<slipstream-user>"`
   - `credential = base64(HMAC-SHA1(secret, username))`
   - the `urls`
   - the TTL is the session length cap (proposal: 1 h).
3. **The client** fetches `/api/ice` before creating the `RTCPeerConnection`. `network.js` hard-codes STUN only.
4. **The host's own TURN usage** (libdatachannel): generate the same kind of credential locally per peer connection.
5. **Expiry.** coturn checks the timestamp when allocations are created and refreshed. Sessions longer than the TTL either need a longer TTL or an ICE restart with fresh credentials before expiry. Measure which coturn does for `ALLOCATE`/`REFRESH` on your version before choosing the TTL.
6. **Server hardening** (`turnserver.conf`):
   - `denied-peer-ip` for 0.0.0.0/8, 10/8, 127/8, 169.254/16, 172.16/12, 192.168/16, the WireGuard subnet, and the IPv6 equivalents;
   - `no-multicast-peers`, `user-quota`, `total-quota`, `max-bps`;
   - `no-tcp-relay` unless needed;
   - a fixed `realm`.
7. **Clean-up.** Delete `network.js.before-turn443`, `webrtc.cpp.bak-wan-ice` and `VPS.conf` from the repo (`*.bak` is already ignored, but these names don't match it).

---

## 3. Protocol compatibility

### Wire changes on this branch

| Change | New host → stale client | New client → old host |
|---|---|---|
| Video `frameType` bits 1–3 = encoder generation, 4–6 = codec + 1 | **Broken.** Old clients test `frameType === 1`. Tagged keyframes (e.g. 0x33) never match, so the client waits for a keyframe forever (black screen). | Works: tag 0 means untagged, and the client falls back to its previous behaviour. |
| Audio FEC group 10 → 4 | Works (the group size is carried in each header). | Works. |
| Audio decoder timestamps derived from `packetId` | Client-only. | Client-only. |
| `MSG_DESKTOP_STATE` (new control message) | Logged as unhandled; harmless. | Never sent. |
| `protocolVersion` in `/api/offer` and its answer | — | Old hosts ignore the extra JSON field. |

### Version negotiation (implemented)

- `kProtocolVersion = 2` (host, `protocol.hpp`) and `PROTOCOL_VERSION = 2` (client, `constants.js`). The comment in `protocol.hpp` records what each version means.
- The client sends `protocolVersion` in the offer body. The host rejects anything else, missing included (a v1 page), with **HTTP 426** and `{code: "protocol_mismatch", error: "...Reload the page...", protocolVersion}`. This happens **before** any peer connection or data channel exists, so a stale client can never receive a packet it could misread.
- A current client getting 426 stops retrying and shows the error on the login screen.
- A *v1* page getting 426 runs its old generic error path. On first connect it shows `Connection failed: <message>`. On a background reconnect it only logs, and the reconnect spinner stays up. That's not perfectly clean, but nothing gets misparsed. A reload fixes it.

### Static asset caching

`RegisterStaticAsset` sets `Cache-Control: no-cache` and is the only static route. It covers `/`, `/styles.css`, `/SlipStream.ico` and all 12 JS modules served (`constants, input, media, network, renderer, state, ui, mic, auth, protocol, audio-worklet, mic-worklet`), which are the 12 the CMake post-build step copies. The worklets load through `audioWorklet.addModule('/js/…')`, which uses the same routes. There's no service worker. The responses carry no `Last-Modified` or `ETag`, so browsers had little to heuristically cache even before. The real stale-client case is **a tab left open across a host upgrade**. Caching can't solve that; the 426 check does.

Gap: `install(DIRECTORY client/js …)` in CMake would install `network.js.before-turn443` too. It's not served (not in the route list), but it ships the TURN credential in installs made with `cmake --install`.

---

## 4. Concurrency and lifetime audit

All static review. Items marked **fixed** were changed in this pass.

### Encoder ownership

- `encoder` (`unique_ptr`) is touched only under `encoderMutex`: by the encoder thread, by install, by `clearStreamingState` and by `onFpsChange`. Construction happens outside `encoderMutex` but under `encoderBuildMutex`.
- Lock order is always build → encoder. `onFpsChange` now takes build before encoder (**fixed**; before, it built under `encoderMutex` alone and could race a worker rebuild).
- **Fixed: stale-parameter rebuild race.** The worker read codec, fps and size when a rebuild was *requested*, then blocked on the build lock behind a codec change, then built the *old* codec and silently reverted it after the client had been ACKed. `rebuildEncoder` now resolves codec, fps and size after taking the build lock. Requests carry only "switch to codec X" or "keep current".
- The stream epoch check under `encoderMutex` discards a build that straddles a disconnect.

### Same-codec stale frames (encoder generations) — fixed

- Each installed encoder gets a generation (mod 8), carried in `frameType` bits 1–3.
- The client tracks the generation of the last keyframe it decoded. A delta from a different generation sets `needKey` and is dropped until that generation's keyframe arrives. This covers a resize rebuild whose first keyframe is lost (before, the new encoder's deltas were decoded against the old encoder's reference frames).
- **Fixed (also pre-existing):** frames completing out of order on the unordered channel were decoded out of order. `processFrame` now drops any frame whose id is at or below the last decoded id. Frame ids are assigned in encode order across encoder instances, so this also stops old-encoder frames that arrive after the new keyframe.

### Frame-slot generations — fixed (pre-existing bug)

- Capture set the slot generation from `captureGen + 1`, and `clearStreamingState` set it from `slotGen + 1`. These are independent counters, so they could produce the same value and let stale frames survive an invalidation. Now there is one monotonic `FrameSlot::BumpGeneration()`.
- `MarkReleased(idx, generation)` ignores releases from a previous generation. After a `Reset` (resize, monitor switch), the old frame's pool index may already belong to a queued new frame, and clearing its in-flight bit let capture overwrite a texture still waiting to be encoded.

### Capture resize

- The resize runs inside `OnFrame` under the capture mutex. The callback runs after unlocking and only posts to the rebuild worker, so there is no lock inversion with `onFpsChange`, which holds the encoder locks and takes the capture mutex in `StartCapture`.
- In-flight frames hold their own texture references, so old pool textures stay alive until released. Scaler SRVs cached by texture pointer keep old textures alive until that encoder is replaced (a bounded leak).
- `w` and `h` are now atomic because the worker reads them.

### Audio device change and resampler lifetime

- `AudioCapture` and `MicPlayback` now touch the resampler only on their own threads. `SetStreaming` clears the queue under its mutex and sets a reset flag (this fixed a race in the original code).
- Device reopen, the Opus encoder swap and resampler recreation all happen on the capture thread. `Stop()` joins the thread before touching `audioClient`.
- COM objects created on the main thread are used from the capture thread; both are in the MTA (`winrt::init_apartment` keeps it alive).
- Residual: `DefaultDeviceChanged` follows the `eConsole` role only. If a user routes apps to a non-default device, the loopback still captures the default device (unchanged behaviour).

### Reconnects

- Host: `clearStreamingState` bumps the epoch, releases the encoder, and bumps the slot generation when a session is replaced. `Reset()` restarts audio packet ids.
- Client:
  - `resetState` clears the decoder state, invalidating any in-flight `initDecoder` by sequence number;
  - the audio reorder buffer is cleared, and after 10 consecutive late packets it resyncs on its own (**fixed**: before, a counter restart without a client reset dropped audio for up to 2 s);
  - the decode-order watermark is cleared.

### Shutdown during reconfiguration — fixed

- **Pre-existing hazard:** libdatachannel callback threads run host lambdas that capture `RunHostApp` locals by reference. A codec-change rebuild still running when `RunHostApp` unwound would use destroyed objects.
- `WebRTCServer::Shutdown()` now refuses new callbacks (message, open/close, state change, `SetRemote`) and waits up to 5 s for in-flight ones before `Reset()`.
- The rebuild worker is stopped (joined) before `ShutdownHost`.
- **Still open:** if a callback outlives the 5 s wait, teardown proceeds with a warning in the log. Also, the exception path in `RunHostApp` destroys joinable `std::thread`s (`WorkerThreads`), which calls `std::terminate` (pre-existing).

### Other things noticed (not changed)

- README documents `--trace` and `--force-software`, but `ConfigureRuntime` only parses `--debug`/`-d`.
- README says port 443; the code listens on 8443 (`kHttpsPort`).
- `onMonitorChange` runs `SwitchMonitor` synchronously on the control-channel thread (pre-existing).
