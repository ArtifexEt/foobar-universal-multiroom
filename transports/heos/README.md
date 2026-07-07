# HEOS Transport Placeholder

Future transport target.

The HEOS implementation should adapt to the same internal contract used by the
native AirPlay transport. It should not require new foobar UI surfaces.

Expected mapping:

- HEOS players/zones -> `OutputDevice`
- HEOS groups -> grouped output capability
- player volume -> `set_output_volume`
- group membership -> `set_enabled_outputs` or a transport-specific grouping
  extension

