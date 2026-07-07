# foo_out_multiroom_bridge

Planned foobar2000 output component.

Responsibilities:

- receive foobar PCM,
- stream PCM to the selected transport,
- expose speaker selection in normal foobar UI,
- persist transport settings and speaker presets,
- translate transport errors into actionable foobar UI messages.

Non-responsibilities:

- direct AirPlay packet scheduling in v1,
- Windows Spatial Audio rendering,
- library management,
- controlling unrelated media server libraries.

## Initial Files To Add

- `main.cpp` - foobar component registration.
- `multiroom_output.h/.cpp` - output implementation.
- `preferences.h/.cpp/.rc` - transport configuration.
- `speaker_panel.h/.cpp/.rc` - modeless UI/dialog for speaker selection.
- `transport.h` - transport-neutral interface.
- `airplay_transport.h/.cpp` - first native transport implementation.
- `discovery.h/.cpp` - mDNS/Bonjour discovery wrapper.
- `sync_clock.h/.cpp` - shared sender timeline and offsets.

The `.vcxproj` should mirror the existing foobar output component structure from
the home theater project, with the target renamed to
`foo_out_multiroom_bridge`.
