# Windows validation handoff

**What has actually been verified for this branch:**

| Level | Covered | Not covered |
|---|---|---|
| Compiled | Nothing. The C++ has not been built on this branch. | Everything below starts here. |
| Simulated (Node, stubbed browser APIs) | Audio worklet: drift, jitter, loss, 44.1 kHz context, stalls. Audio reorder buffer: 7 cases. `media.js` codec/generation state machine: 11 cases. All JS syntax-checked. | Real `VideoDecoder`/`AudioDecoder`, real network, the host. |
| Static review | All C++ changes, locking and lifetimes (`STABILIZATION_AUDIT.md`). | — |
| Real-world | Nothing. | — |

## Build

The repo's own build: CMake + vcpkg manifest (`vcpkg.json`) + MSVC (VS 2022, x64).
Run from a **"x64 Native Tools Command Prompt for VS 2022"** in the repo root:

```bat
git fetch origin claude/elegant-mendel-wplopx
git checkout claude/elegant-mendel-wplopx

rem Same steps as build_installer_release.bat, minus the installer and the forced clean:
C:\vcpkg\vcpkg.exe install --triplet x64-windows
cmake -S . -B build -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release --target SlipStream 2> build-errors.txt
```

Output: `build\bin\Release\SlipStream.exe`, with `index.html`, `styles.css` and `js\*.js` copied next to it.
For the installer: `build_installer_release.bat` (needs Inno Setup 6). It deletes `build\` first.

If compilation fails, send `build-errors.txt`. The most likely trouble spots, since they are new code compiled for the first time:

- `host_app.cpp`: `std::optional` rebuild lambda; `SecureDesktopMonitor` (`OpenInputDesktop`, `_wcsicmp`)
- `audio.cpp`: `WAVEFORMATEXTENSIBLE` / `WAVE_FORMAT_EXTENSIBLE` (needs `<mmreg.h>`)
- `capture.cpp`: `std::atomic<int>` `w`/`h` in brace initialisers and `LOG` varargs
- `webrtc.hpp`: nested `CallbackGuard` touching private members

## Logs

- Host: `%APPDATA%\SlipStream\slipstream.log`. Run with `--debug` for DBG lines. `run-slipstream.ps1` also tees the console output to `.\slipstream-debug.log`.
- Client: browser DevTools console (`[NET]`, `[MEDIA]`, `[AUDIO]`, `[VIDEO]` prefixes). Enable *Verbose* to see debug lines. Save as `.log` via right-click.
- Chrome extras: `chrome://webrtc-internals` (dump), and `chrome://media-internals` (decoder choice and errors).

Send both logs for any failure, plus the GPU model, driver version and browser version.

## First tests, in order

| # | Test | Expected result | Grep for |
|---|---|---|---|
| 1 | Start the host, open `https://<host>:8443`, log in. | Video within a few seconds. | `Hardware codec support: AV1=… H265=… H264=…` must match the GPU (RTX 20/30: AV1=0; RTX 40+, Arc, RX 7000: AV1=1). `Codec support: … (advertised; …)`. |
| 2 | Clear site data (or a private window), connect. | The client picks hardware decode first; the dropdown shows the codec actually in use. | Client: `Codec set`, `Codec ack`. Host: `Encoder state: usingHardware=1`. A software encoder name (`libsvtav1`, `libx265`) on a GPU host is a failure. |
| 3 | Switch codecs several times through the dropdown. | Under ~0.5 s of frozen picture, no decoder errors. | Client: `Stream codec changed - switching decoder`. No `Decoder error`. Host: `Encoder rebuild (codec-change)`. |
| 4 | Pick a codec the GPU can't encode (if the dropdown still offers one, test 1 failed). | Dropdown reverts; video continues. | Host: `Encoder creation failed`. ACK carries the old codec. |
| 5 | Resize the browser window repeatedly; toggle fullscreen. | Picture keeps updating during the drag; one rebuild after it settles; no corruption. | Host: one `Encoder rebuild (client-stream-target)` per settled resize. Client: possibly `Encoder generation changed - waiting for its keyframe`, never repeated `Decoder error`. |
| 6 | Change the host display resolution and scaling (Settings → Display) while streaming. | Brief stretch, then a correct picture; the mouse lands in the right place. | `Capture: Source resized`, `Resolution change`, `Encoder rebuild (source-resolution-change)`. |
| 7 | Audio on; play music; switch the host default output device (e.g. speakers → headset); unplug the device. | Audio follows within ~1 s; after unplug, recovers when a device returns. | `AudioCapture: Reopening output device (default output device changed)`, `Output device reopened`. |
| 8 | Host with 5.1/7.1 output configured. | Clean stereo, not noise. | `Audio: 48000Hz -> 48000Hz, 6ch -> 2ch` (or 8ch). |
| 9 | WAN, or simulated loss: `clumsy` on the host (drop 2%, lag 40 ms ± 30 ms). | Audio stays continuous, with occasional soft dropouts and no clicks or buzz. | Client: `Concealed lost packets`, `Worklet underruns` (rare). DevTools stats overlay → audio buffer ms. |
| 10 | Trigger UAC on the host (e.g. run `cmd` as administrator). | Notice appears; picture frozen; notice clears after the prompt closes, and video resumes. | Host: `Host input desktop is now secure` / `normal`. Client: `Host desktop state`. |
| 11 | Keep an old tab open, restart the host, let the tab reconnect. | The new host refuses the offer with 426 instead of a black stream. | Host: `rejecting client protocol 1`. |
| 12 | Close the host from the tray during a codec switch. | Clean exit, no crash dialog. | `Shutting down...` → `Shutdown complete`. No `callback(s) still running`. |

Test 11 needs a page from the previous build. Without one, approximate it by sending an offer without `protocolVersion` (DevTools: edit and resend the `/api/offer` request).
