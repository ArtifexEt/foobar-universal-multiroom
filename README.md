# Foobar Universal Multiroom

Foobar Universal Multiroom is a standalone foobar2000 component for playing
foobar audio on selected network speakers with synchronized multiroom playback.

The first transport target is native AirPlay 2. The plugin owns discovery,
speaker selection, stream scheduling, authentication/session state, and
synchronization itself. The transport boundary stays generic so HEOS, Music
Assistant, Chromecast groups, Snapcast, or other renderers can be added later
without changing the foobar UI.

## Goals

- Play the same foobar2000 music at the same time on selected AirPlay 2 speakers.
- Keep remote speakers synchronized with a project-owned playback clock and
  packet scheduler.
- Put speaker selection in the foobar UI, not only in Preferences.
- Make transports interchangeable behind one internal interface.
- Preserve local playback through the existing foobar output/spatial renderer
  when desired.
- Support future transports such as HEOS without changing the foobar-facing UI.
- Keep transport/core code portable enough to support non-Windows hosts later,
  even though the current foobar2000 component build targets Windows.

## Current Implementation

- `foo_out_multiroom_bridge` builds as a foobar2000 component in GitHub Actions.
- The foobar UI includes a compact speaker selector element.
- The selector refreshes native AirPlay/mDNS discovery and shows discovered
  speakers in a dockable AirPlay-style popup with checkboxes and per-speaker
  volume sliders, plus PIN pairing for AirPlay 2 receivers that require auth.
- Preferences expose status, refresh, repository, and support actions with
  foobar dark-mode/scaling hooks and PIN pairing for discovered AirPlay 2
  speakers.
- The transport-neutral core includes output state, packet scheduling, stream
  clocking, and the AirPlay transport boundary.
- AirPlay discovery now classifies AirPlay 2, legacy unencrypted L16, auth, and
  unsupported endpoints separately.
- AirPlay 2 now uses the Apache-2.0 `airplay2-sender-cpp` crypto/wire-format
  core as a pinned build dependency, without vendoring it into this repository.
- The first real AirPlay 2 sender path performs transient HAP pair-setup,
  switches the RTSP control channel to ChaCha20-Poly1305 framing, negotiates
  binary-plist session/stream `SETUP`, and sends encrypted realtime ALAC RTP.
- PIN pairing persists AirPlay credentials through foobar configuration and
  reconnects with stored pair-verify credentials.
- The legacy RTSP/TCP probe client has `OPTIONS`,
  `ANNOUNCE`, `SETUP`, `RECORD`, `FLUSH`, and `TEARDOWN` session setup against
  discovered endpoints.
- The RTP/L16 sender path remains only as a diagnostic legacy probe path, not
  the MVP or product streaming path.
- Foobar registers a high-latency `Universal Multiroom Bridge` output device
  that feeds selected AirPlay 2 sessions from the normal foobar output
  pipeline.
- Per-speaker UI volume changes are sent to active AirPlay sessions with native
  RTSP `SET_PARAMETER` volume updates.
- The main foobar2000 volume is treated as a remote AirPlay master volume
  multiplier instead of digitally attenuating the PCM stream.
- Still missing before daily use: full encrypted event-channel handling,
  real-device validation, and timing hardening.

## TODO

- Define the non-Windows support path for reusable transport/core pieces, such
  as a future service, helper, or library build outside the Windows foobar2000
  component.

## Repository Layout

- `components/foo_out_multiroom_bridge/` - foobar2000 component and public
  component boundary.
- `transports/airplay/` - native AirPlay discovery and transport implementation.
- `transports/heos/` - future HEOS transport area.
- `docs/ARCHITECTURE.md` - proposed system design.
- `docs/AIRPLAY_ENGINE.md` - native AirPlay engine plan.
- `docs/SYNC_ENGINE.md` - synchronized playback clock plan.
- `docs/UI_PLAN.md` - foobar UI plan.
- `docs/ROADMAP.md` - implementation phases.
- `docs/RISKS.md` - technical and licensing risks.

## Build

The CMake build covers the transport-neutral core, a small contract probe, and
an AirPlay network probe for local discovery/playback diagnostics.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure

.\build\Release\MultiroomAirPlayNetworkProbe.exe --timeout-ms 4000 --list-txt
.\build\Release\MultiroomAirPlayNetworkProbe.exe --play --target first --duration-ms 5000 --require-speaker
.\build\Release\MultiroomAirPlayAudioDiagnostics.exe --target korytarz --duration-ms 5000 --volume 45 --require-speaker
.\build\Release\MultiroomAirPlayAudioDiagnostics.exe --target korytarz --pin 1234 --duration-ms 5000 --volume 45 --require-speaker
.\build\Release\MultiroomAirPlayAudioDiagnostics.exe --loopback-self-test
```

On macOS, the diagnostics-only `MultiroomAirPlayMacProbe` target uses system
Bonjour discovery and then exercises the same AirPlay transport path:

```bash
./MultiroomAirPlayMacProbe --list-only --timeout-ms 2500 --require-speaker
./MultiroomAirPlayMacProbe --target korytarz --duration-ms 5000 --volume 45 --require-speaker
./MultiroomAirPlayMacProbe --target wiim --duration-ms 5000 --volume 45 --require-speaker
```

GitHub Actions also downloads the foobar2000 SDK, builds the component with
MSBuild, and packages `foo_out_multiroom_bridge.fb2k-component`.
