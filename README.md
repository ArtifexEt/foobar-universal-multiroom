# Universal Multiroom Audio Bridge

Universal Multiroom Audio Bridge is a standalone foobar2000 component for playing
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
- Opening the picker or native toolbar dropdown is read-only for the active
  transport and never refreshes discovery, takes the PCM transport lock, or
  reconciles sessions. A requested discovery refresh during playback is queued
  until the stream stops.
- `Playback > Select Wireless Speakers...` is available as a normal foobar command and can be
  placed in the Buttons toolbar through `Customize Buttons`.
- Foobar2000 2.x also receives a native `Wireless Speakers` toolbar dropdown. It shows the
  current group, exposes quick speaker toggles, and opens the full picker.
- Preferences expose status, refresh, repository, and support actions with
  foobar dark-mode/scaling hooks and PIN pairing for discovered AirPlay 2
  speakers. The Speaker Groups tab creates, edits, deletes, and applies named
  virtual groups; saved groups are also available in the toolbar dropdown and
  the full speaker picker.
- The transport-neutral core includes output state, packet scheduling, stream
  clocking, and the AirPlay transport boundary.
- AirPlay discovery now classifies AirPlay 2, legacy unencrypted L16, auth, and
  unsupported endpoints separately.
- AirPlay 2 now uses the Apache-2.0 `airplay2-sender-cpp` crypto/wire-format
  core as a pinned build dependency, without vendoring it into this repository.
- The first real AirPlay 2 sender path performs transient HAP pair-setup,
  switches the RTSP control channel to ChaCha20-Poly1305 framing, negotiates
  binary-plist session/stream `SETUP`, and sends encrypted lossless realtime
  ALAC or PCM RTP according to receiver compatibility.
- Timing setup follows the receiver feature flags: PTP-capable endpoints use
  the AirPlay timing-peer/`SETPEERS` handshake, while NTP endpoints use the
  native timing port and sync packets.
- PIN pairing persists AirPlay credentials through foobar configuration and
  reconnects with stored pair-verify credentials.
- The legacy RTSP/TCP probe client has `OPTIONS`,
  `ANNOUNCE`, `SETUP`, `RECORD`, `FLUSH`, and `TEARDOWN` session setup against
  discovered endpoints.
- The RTP/L16 sender path remains only as a diagnostic legacy probe path, not
  the MVP or product streaming path.
- Foobar registers a high-latency `Universal Multiroom Audio Bridge` output device
  that feeds selected AirPlay 2 sessions from the normal foobar output
  pipeline.
- Per-speaker UI volume changes are sent to active AirPlay sessions with native
  RTSP `SET_PARAMETER` volume updates. Drag events are committed once at the
  end of the gesture and coalesced on a volume-only control path so they do not
  repeatedly block PCM delivery or reconnect the selected group. Speaker
  percentages use AirPlay's native linear `-30..0 dB` scale (`0%` is the
  `-144 dB` mute sentinel), so the effective percentage sent after applying
  foobar's master multiplier matches the receiver's percentage scale.
- The main foobar2000 volume is treated as a remote AirPlay master volume
  multiplier instead of digitally attenuating the PCM stream.
- Foobar now-playing callbacks publish the current title, artist, album, album
  artist, composer, genre, year, track/disc numbers, duration, position, and
  front-cover artwork to active AirPlay sessions. New sessions receive the
  current snapshot. Duration also falls back to foobar's dynamic length API and
  is resent if it becomes available after playback starts, so the receiver gets
  a complete progress end timestamp. Track starts and playback stop explicitly
  clear stale receiver metadata before the next artwork is available.
- AirPlay 2 receiver-side play, pause, play/pause toggle, stop, next, and
  previous commands are decoded from the encrypted event channel and dispatched
  to foobar on its main thread. Duplicate command IDs from a speaker group are
  ignored, and a mid-stream transport failure requests one controlled foobar
  stop instead of leaving the output in a retry loop. A receiver that rejects
  remote-command advertisement keeps its already-negotiated audio session.
- macOS PR builds package `MultiroomMacPlaybackTester`, a command-line tester
  for the portable core/AirPlay path that uses system Bonjour and sends a real
  generated tone through `MultiroomEngine`.
- Still missing before daily use: device-side volume feedback and
  multi-speaker drift/timing hardening.
- Preferences list every discovered speaker with a visibility checkbox. Clearing
  it hides that speaker from the toolbar dropdown without changing its selected
  or connected state; existing settings migrate as visible.

## TODO

- Define the non-Windows support path for reusable transport/core pieces, such
  as a future service, helper, or library build outside the Windows foobar2000
  component.

## Adding the speaker selector to foobar2000

- In a Buttons toolbar, open its configuration and add
  `Playback > Select Wireless Speakers...` for a button that opens the picker.
- For the live destination shown in the toolbar row, right-click the toolbar
  header, add foobar2000's `Toolbar Dropdown`, then select
  `Wireless Speakers` as
  its data source. It shows `Idle`, `Connecting...`, or the receiver(s) whose
  AirPlay sessions are actually ready.
- In Default UI Layout Editing Mode, the standalone playback-information
  element is named `Wireless Speakers` and can be placed next to playback
  controls, the seekbar, or volume.

The command, native dropdown, and standalone element use the same picker and
persisted speaker group. The two live-label surfaces report active sessions,
not merely saved checkboxes.

## Repository Layout

- `components/foo_out_multiroom_bridge/` - foobar2000 component and public
  component boundary.
- `transports/airplay/` - native AirPlay discovery and transport implementation.
- `transports/heos/` - future HEOS transport area.
- `docs/ARCHITECTURE.md` - proposed system design.
- `docs/AIRPLAY_ENGINE.md` - native AirPlay engine plan.
- `docs/SYNC_ENGINE.md` - synchronized playback clock plan.
- `docs/UI_PLAN.md` - foobar UI plan.
- `docs/UI_AUDIT.md` - implemented UI surfaces, fixes, and remaining parity work.
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

On macOS, `MultiroomMacPlaybackTester` uses system Bonjour discovery and then
exercises the same portable `MultiroomEngine` plus AirPlay transport path:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build build --config Release
ctest --test-dir build --output-on-failure

./build/MultiroomMacPlaybackTester --list-only --timeout-ms 2500 --require-speaker
./build/MultiroomMacPlaybackTester --target korytarz --duration-ms 5000 --volume 45 --require-speaker
./build/MultiroomMacPlaybackTester --target wiim --duration-ms 5000 --volume 45 --require-speaker
```

`MultiroomAirPlayMacProbe` remains as a compatibility alias for the same tester
source in current PR artifacts.

GitHub Actions also downloads the foobar2000 SDK, builds the component with
MSBuild, and packages `foo_out_multiroom_bridge.fb2k-component`. Pull request
builds also publish `Foobar-Universal-Multiroom-macos-tester`.
