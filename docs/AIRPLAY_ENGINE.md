# AirPlay Engine

## Goal

Implement native AirPlay playback inside this project instead of depending on an
external multiroom server. The foobar component should be able to discover,
select, and stream to speakers by itself.

## Modules

`discovery`

- mDNS/Bonjour browser for AirPlay service records.
- Parses device names, ids, IP/port, feature flags, model, password/auth hints,
  and grouping hints where available.

`session`

- Owns connection setup, authentication, stream negotiation, teardown, and
  reconnect behavior.
- Keeps protocol details out of foobar UI code.

`encoder`

- Converts foobar PCM to the negotiated network format.
- Starts with stereo 16-bit/44.1 kHz or 48 kHz.
- Adds ALAC/AAC only when the basic uncompressed path and packet clock are
  reliable.

`clock`

- Maintains the shared sender timeline.
- Produces packet timestamps.
- Tracks per-device drift and latency.
- Applies user offsets after measured compensation.

`scheduler`

- Buffers outgoing frames.
- Sends packets early enough for receiver buffering.
- Handles retransmit/recovery hooks where the protocol supports them.

`device_state`

- Stores speaker status, volume, auth state, supported formats, measured
  latency, and selected state.

## Synchronization

The sender clock is the center of the system. Each selected speaker receives
packets stamped against the same stream timeline. The scheduler keeps a target
buffer horizon per device, then the clock layer applies:

- measured device latency,
- protocol-reported latency,
- user-configured offset,
- optional group offset.

The UI should display two different ideas clearly:

- selected AirPlay speakers can be synchronized by this plugin,
- sync against a separate Windows Spatial Audio endpoint requires local latency
  measurement and is a later phase.

## First Working Path

1. Discover devices.
2. Show devices in foobar UI.
3. Connect to one speaker.
4. Stream stereo PCM.
5. Add multiple selected speakers using the same sender timeline.
6. Add per-speaker volume.
7. Add offset compensation.
8. Add pairing/auth.
9. Add metadata/artwork.

