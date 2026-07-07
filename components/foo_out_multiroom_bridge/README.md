# foo_out_multiroom_bridge

Standalone foobar2000 component.

Responsibilities:

- receive foobar PCM,
- stream PCM to the selected transport,
- expose speaker selection in normal foobar UI,
- persist transport settings and speaker presets,
- translate transport errors into actionable foobar UI messages.

Non-responsibilities:

- Windows Spatial Audio rendering,
- library management,
- controlling unrelated media server libraries.
