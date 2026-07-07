# Risks

## Licensing

Do not import third-party AirPlay implementation code unless the license and
distribution model are explicitly chosen first. The safest path is a clean-room
implementation guided by protocol behavior, public documentation, packet traces
from owned devices, and small isolated experiments.

## AirPlay Complexity

Native AirPlay support in a foobar component is a large project: mDNS,
pairing/auth, encryption, timing, retransmits, clock sync, metadata, and
device-specific behavior. Treat discovery, session setup, and packet timing as
separate modules so each can be tested independently.

## Windows And Foobar Output Fanout

Playing local Spatial Audio and remote AirPlay at the same time may require a
dedicated fanout/tap because foobar normally chooses one output. This is separate
from simply sending playback to remote speakers.

## Latency Claims

Remote speakers selected through this plugin can be synchronized with each other
only after the sender clock and per-device scheduler are stable. Synchronization
with a local AVR/Windows Spatial endpoint is a separate latency compensation
problem and should be labelled as experimental until measured.

## Network Availability

Network speakers can disappear, change IP addresses, require auth, or reject a
session while foobar is playing. The plugin needs robust rediscovery, reconnect,
and user-visible error states.
