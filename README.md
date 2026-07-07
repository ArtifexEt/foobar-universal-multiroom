# Foobar Universal Multiroom

Foobar Universal Multiroom is a planned standalone foobar2000 component that
plays foobar audio on selected network speakers with synchronized multiroom
playback.

The first transport target is native AirPlay. The plugin should own discovery,
speaker selection, stream scheduling, and synchronization itself. The transport
boundary stays generic so HEOS, Music Assistant, Chromecast groups, Snapcast, or
other renderers can be added later without changing the foobar UI.

This project is intentionally separate from `foobar-for-home-theater`. The home
theater project owns Windows Spatial Audio rendering. This project owns
multiroom transport and speaker selection. They can later share a PCM tap or
profile concepts, but neither should be forced into the other.

## Goals

- Play the same foobar2000 music at the same time on selected AirPlay speakers.
- Keep remote speakers synchronized with a project-owned playback clock and
  packet scheduler.
- Put speaker selection in the foobar UI, not only in Preferences.
- Make transports interchangeable behind one internal interface.
- Preserve local playback through the existing foobar output/spatial renderer
  when desired.
- Support future transports such as HEOS without changing the foobar-facing UI.

## Recommended Shape

The first implementation should be a foobar output component named
`foo_out_multiroom_bridge`.

It should expose a compact foobar UI panel/menu for:

- transport connection status,
- output list refresh,
- speaker enable/disable toggles,
- per-speaker volume,
- per-speaker offset in milliseconds,
- preset groups such as `Living room`, `Kitchen`, or `Whole home`.

The component should stream foobar PCM into its own transport layer. The AirPlay
transport handles discovery, session setup, packet timing, speaker volume, and
offset control. Other systems should implement the same internal transport
contract.

## Prior Art

OwnTone was reviewed as a feature baseline for AirPlay multiroom behavior. This
project is not an OwnTone wrapper and should not depend on its server, API, or
code.

## Repository Layout

- `components/foo_out_multiroom_bridge/` - foobar2000 component plan and public
  component boundary.
- `transports/airplay/` - native AirPlay transport notes.
- `transports/heos/` - future HEOS transport placeholder.
- `docs/ARCHITECTURE.md` - proposed system design.
- `docs/AIRPLAY_ENGINE.md` - native AirPlay engine plan.
- `docs/SYNC_ENGINE.md` - synchronized playback clock plan.
- `docs/UI_PLAN.md` - foobar UI plan.
- `docs/ROADMAP.md` - implementation phases.
- `docs/RISKS.md` - technical and licensing risks.

## Build

The current CMake target is only a tiny contract probe so CI has a cheap sanity
check while the real foobar SDK project is added.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The real component build should follow the pattern in `foobar-for-home-theater`:
download the foobar2000 SDK in CI, copy `components/foo_out_multiroom_bridge`
into the SDK tree, build with MSBuild, then package the DLL as
`foo_out_multiroom_bridge.fb2k-component`.
