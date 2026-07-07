# AirPlay Transport

First native AirPlay 2 transport target for `foo_out_multiroom_bridge`.

The AirPlay implementation should be built as part of this project, with a clean
internal boundary so the foobar component does not know protocol details.

## Responsibilities

- discover AirPlay 2 targets with mDNS/Bonjour,
- model speakers and groups,
- perform pairing/auth where required,
- negotiate stream format,
- encrypt and send audio packets with timestamps,
- maintain a shared playback clock,
- compensate per-speaker latency and configured offsets,
- expose device status to the foobar UI.

## MVP Scope

Start with one AirPlay 2 speaker using transient HAP pair-setup, encrypted
control framing, binary-plist `SETUP`, and encrypted realtime ALAC RTP. Legacy
unencrypted PCM/L16 is a probe/fallback path only and does not satisfy the MVP.

The project uses `akustikrausch/airplay2-sender-cpp` as a pinned build-time
dependency for the AirPlay 2 crypto and wire-format core. The foobar component
does not run or depend on OwnTone, Music Assistant, or any external multiroom
server.

## Boundary

The transport should expose only transport-neutral objects to the rest of the
plugin:

- output id,
- display name,
- protocol type,
- selected state,
- auth state,
- volume,
- measured latency,
- configured offset.
