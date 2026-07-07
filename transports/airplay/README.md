# AirPlay Transport

First native transport target for `foo_out_multiroom_bridge`.

The AirPlay implementation should be built as part of this project, with a clean
internal boundary so the foobar component does not know protocol details.

## Responsibilities

- discover AirPlay targets with mDNS/Bonjour,
- model speakers and groups,
- perform pairing/auth where required,
- negotiate stream format,
- send audio packets with timestamps,
- maintain a shared playback clock,
- compensate per-speaker latency and configured offsets,
- expose device status to the foobar UI.

## MVP Scope

Start with stereo PCM/ALAC to AirPlay-capable speakers and synchronized playback
across selected outputs. AirPlay 2 group behavior can be layered once discovery,
clocking, and basic remote playback are stable.

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

