# Foobar Universal Multiroom

Foobar Universal Multiroom is a standalone foobar2000 component for playing
foobar audio on selected network speakers with synchronized multiroom playback.

The first transport target is native AirPlay. The plugin owns discovery, speaker
selection, stream scheduling, and synchronization state itself. The transport
boundary stays generic so HEOS, Music Assistant, Chromecast groups, Snapcast, or
other renderers can be added later without changing the foobar UI.

## Goals

- Play the same foobar2000 music at the same time on selected AirPlay speakers.
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
  speakers as a checkbox menu.
- Preferences expose status, refresh, repository, and support actions with
  foobar dark-mode/scaling hooks.
- The transport-neutral core includes output state, packet scheduling, stream
  clocking, and the AirPlay transport boundary.
- AirPlay playback/session negotiation is the next large implementation area.

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

The CMake build covers the transport-neutral core and a small contract probe.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

GitHub Actions also downloads the foobar2000 SDK, builds the component with
MSBuild, and packages `foo_out_multiroom_bridge.fb2k-component`.
