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
- Recreated after flush or a new stream; adding a receiver mid-stream keeps the
  existing group anchor.

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
- Local Windows endpoint compensation.
- Room/group presets.
- Multiple protocol families in one synchronized group only where their timing
  primitives are strong enough.
