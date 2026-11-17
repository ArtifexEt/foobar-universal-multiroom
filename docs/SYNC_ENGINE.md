# Sync Engine

## Purpose

The sync engine makes one foobar playback timeline audible on multiple selected
network speakers at the same time.

## Core Concepts

`PlaybackClock`

- Monotonic sender timeline.
- Converts foobar frames to stream timestamps.
- Survives pause/resume and flush events.

`DeviceClock`

- Per-speaker timing estimate.
- Tracks connection latency, receiver buffer depth, and drift.

`PacketScheduler`

- Sends frames to each device with enough lead time.
- Keeps speaker buffers filled without letting one slow device stall the whole
  group forever.

`GroupClockAnchor`

- Captured once when the first PCM packet of a stream is scheduled.
- Shared by every selected receiver so equal presentation timestamps resolve
  to the same NTP wall-clock instant.
- The wall clock supplies the initial epoch. A monotonic clock drives the
  timing replies and stream anchors for the life of the component.
- Recreated after flush or a new stream; adding a receiver mid-stream keeps the
  existing group anchor.
- Sender RTP timestamps advance by source frames. Speaker offsets change the
  time announced in sync packets, and a flush keeps the RTP timestamp carried
  in the FLUSH request as the starting point for the new source timeline.

`OffsetModel`

- Combines measured latency with user offsets.
- Stores user offsets in milliseconds.
- Keeps advanced correction visible in the normal speaker UI.

## MVP Behavior

- One source timeline.
- Multiple AirPlay outputs.
- Per-output volume.
- Per-output offset.
- Conservative buffering for stability over low latency.

## Later Behavior

- Automatic latency measurement.
- Drift correction.
- Native PTP clock exchange for receivers that require it.
- Local Windows endpoint compensation.
- Room/group presets.
- Multiple protocol families in one synchronized group only where their timing
  primitives are strong enough.
