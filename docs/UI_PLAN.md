# UI Plan

## Principle

Speaker selection belongs in foobar's everyday playback surface, not only in
Preferences. Preferences should hold connection details and advanced defaults.

## Surfaces

### Playback Menu

Add:

- `Playback > Multiroom > Enable bridge`
- `Playback > Multiroom > Refresh speakers`
- `Playback > Multiroom > Presets`

### Main Panel Or Modeless Dialog

Provide a compact speaker picker:

- transport status at top,
- refresh button,
- list of outputs with checkboxes,
- volume slider per output,
- small offset field per output,
- auth/error indicator,
- group preset buttons.

### Preferences

Only configuration that should not change during normal listening:

- transport type,
- discovery on/off,
- preferred network interface,
- default sample rate,
- metadata forwarding on/off,
- local playback coexistence mode,
- retry/log verbosity.

## First Version UI Scope

MVP:

- discover AirPlay outputs,
- select/unselect outputs,
- per-output volume,
- per-output offset,
- save/load selection presets.

Do after MVP:

- metadata and artwork,
- PIN entry,
- automatic output rediscovery,
- local spatial output delay compensation,
- embedded mini transport controls.
