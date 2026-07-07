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

### Dockable Speaker Selector

Provide a compact Default UI element that can be added from the layout context
menu and docked like other foobar controls:

- compact AirPlay-style button showing the current selection,
- popup speaker picker opened from the button,
- list of outputs with checkboxes,
- volume slider per output in the same popup,
- small offset field per output,
- auth/error indicator,
- group preset buttons.

The picker should feel closer to Apple Music's output popover than to a plain
Windows menu, because per-speaker volume needs real controls rather than menu
items.

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

- discover AirPlay 2 outputs,
- select/unselect outputs,
- show auth/encryption/timing readiness before a speaker can be selected,
- per-output volume,
- per-output offset,
- save/load selection presets.

Do after MVP:

- metadata and artwork,
- PIN entry,
- automatic output rediscovery,
- local spatial output delay compensation,
- embedded mini transport controls.
- remote playback feedback from AirPlay/DACP-style events so pause, previous,
  next, and device-side volume changes can update foobar and the rest of the
  selected speaker group. This is native component behavior, not integration
  with an external multiroom server. External implementations can inform the
  expected behavior, but they must not become runtime dependencies.
