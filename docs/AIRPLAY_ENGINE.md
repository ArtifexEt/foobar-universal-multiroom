# AirPlay Engine

## Goal

Implement native AirPlay 2 playback inside this project instead of depending on
an external multiroom server. The foobar component should be able to discover,
select, authenticate, synchronize, and stream to speakers by itself.

## Modules

`discovery`

- mDNS/Bonjour browser for AirPlay service records.
- Parses device names, ids, IP/port, feature flags, model, password/auth hints,
  and grouping hints where available.

`session`

- Owns connection setup, authentication, stream negotiation, teardown, and
  reconnect behavior.
- AirPlay 2 MVP starts with transient HAP pair-setup for receivers that accept
  it, encrypted control framing, binary-plist session/stream setup, and
  encrypted lossless realtime ALAC or PCM RTP.
- Timing setup follows receiver feature flags. PTP-capable endpoints receive
  timing-peer metadata plus `RECORD` and `SETPEERS` before stream `SETUP`;
  NTP endpoints use the native timing port and complete `RECORD` afterwards.
- Stored-credential pair-verify and on-screen PIN pairing are native auth
  layers, not a dependency on OwnTone or another runtime server.
- The legacy RTSP/TCP `OPTIONS`, `ANNOUNCE`, `SETUP`, `RECORD`, `FLUSH`, and
  `TEARDOWN` path is retained as a diagnostic probe only.
- Keeps protocol details out of foobar UI code.

`encoder`

- Converts foobar PCM to the negotiated network format.
- Starts with stereo 16-bit/44.1 kHz.
- AirPlay 2 realtime negotiates lossless ALAC or network-byte-order PCM and
  encrypts the payload with the first 32 bytes of the pairing shared secret.
  Unencrypted PCM/L16 is only a legacy/probe path.

`clock`

- Maintains the shared sender timeline.
- Produces packet timestamps.
- Tracks per-device drift and latency.
- Applies user offsets after measured compensation.

`scheduler`

- Buffers outgoing frames.
- Sends packets early enough for receiver buffering.
- Handles retransmit/recovery hooks where the protocol supports them.

`rtp_sender`

- Packetizes stereo 16-bit PCM as realtime payload type 96.
- Uses the UDP ports negotiated by RTSP `SETUP`.
- Converts little-endian host PCM samples to network byte order and encrypts
  each AirPlay 2 payload with the paired session key before sending.

`device_state`

- Stores speaker status, selected state, configured volume, auth state,
  supported formats, and measured latency.
- Sends active per-speaker volume changes over native RTSP `SET_PARAMETER`
  where a session is already open.

`remote_control`

- Listens for receiver/controller feedback such as pause, play, previous, next,
  and device-side volume changes.
- Maps transport events back to foobar playback control so changing state from
  a speaker/controller updates foobar and can be mirrored to the rest of the
  selected speaker group.
- Is implemented natively in this component's AirPlay transport; it must not
  depend on OwnTone, Music Assistant, or another external multiroom server.
- External projects may be used only as protocol/behavior references during
  research. Do not vendor, link, call, or copy their implementation into this
  component unless the licensing and architecture are explicitly revisited.
- Requires the encrypted event channel to be serviced continuously. The current
  sender decrypts pushed RTSP event requests and replies with encrypted `200 OK`
  responses; the next step is mapping those commands onto foobar playback state.

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

1. Discover devices and classify AirPlay 2 support.
2. Show AirPlay 2 devices in foobar UI with unsupported/auth state.
3. Complete transient HAP pair-setup for one AirPlay 2 speaker.
4. Establish encrypted AirPlay 2 session/stream setup.
5. Send encrypted lossless realtime ALAC or PCM packets for stereo PCM.
6. Add foobar output-device integration so playback enters the bridge from the
   normal foobar output pipeline.
7. Add multiple selected speakers using the same sender timeline.
8. Add per-speaker volume.
9. Add offset compensation.
10. Add PIN pairing and persisted pair-verify credentials.
11. Add remote playback feedback for pause, previous, next, and device-side
   volume changes.
12. Add metadata/artwork.
