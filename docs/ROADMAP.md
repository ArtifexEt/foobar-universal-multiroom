# Roadmap

## Phase 0 - Repository And Contracts

- Create separate project on `D:`.
- Record architecture and transport contract.
- Add lightweight CMake sanity target.

## Phase 1 - Native Discovery And UI

- Add `foo_out_multiroom_bridge` foobar component skeleton.
- Add native AirPlay discovery wrapper.
- Implement speaker model and selected output state.
- Implement per-output volume and offset.
- Add a modeless speaker picker UI.
- Package as `.fb2k-component`.

Acceptance:

- foobar UI lists AirPlay outputs discovered on the network.
- selecting speakers updates the plugin's active output set.
- volume/offset changes are persisted and reflected in UI state.

## Phase 2 - Single Speaker Playback

- Convert foobar audio chunks to transport format.
- Open one AirPlay session.
- Start/stop the stream with foobar playback.
- Handle pause/seek/flush cleanly.

Acceptance:

- foobar playback is audible through one selected AirPlay output.
- pause, seek, and stop do not leave stale sessions behind.

## Phase 3 - Multi Speaker Sync

- Add shared playback clock.
- Add packet scheduler.
- Add per-speaker target buffer and offset handling.
- Allow speakers to join/leave during playback.

Acceptance:

- foobar playback is audible through multiple selected AirPlay outputs.
- selected remote speakers stay synchronized under the plugin's clocking.
- switching selected speakers during playback does not require restarting foobar.

## Phase 4 - Usability

- Add presets.
- Add status/error messages.
- Add auth warning and optional PIN flow.
- Add metadata forwarding.
- Add output format preference where speakers expose multiple formats.

## Phase 5 - Local Spatial Coexistence

- Add optional local output fanout design.
- Measure or configure delay between local endpoint and network outputs.
- Decide whether this belongs in the bridge plugin or a shared PCM tap.

## Phase 6 - More Transports

- Add HEOS transport contract implementation.
- Add Music Assistant transport if its API is a better universal abstraction.
- Add Snapcast transport if low-latency synchronized local zones are desired.
