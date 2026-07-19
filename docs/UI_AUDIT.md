# AirPlay Speaker UI Audit

## Scope

This audit covers the everyday speaker-selection surfaces, their connection to
the AirPlay control path, and visual parity with the output picker used by
Apple Music. Preferences and protocol diagnostics are intentionally secondary.

## Implemented

| Area | State | Notes |
| --- | --- | --- |
| Playback command | Complete | `Playback > AirPlay Speakers...` opens the full picker and can be assigned to foobar's Buttons toolbar. |
| Native toolbar dropdown | Complete | Foobar2000 2.x `toolbarDropDown` service named `AirPlay`, with a live group label, quick toggles, and an entry into the full picker. |
| Dockable compact control | Complete | Default UI utility element with a scalable AirPlay audio glyph, current selection, keyboard activation, focus state, light/dark colors, and a compact icon-only layout. |
| Speaker popup | Complete for normal selection | Popover positioning, scrollable speaker list, selected-state emphasis, custom check indicators, custom accent sliders, percentages, refresh, PIN pairing, dark/light adaptation, and automatic dismissal. |
| Persisted selection and volume | Complete | Canonical speaker IDs and aliases retain selection/volume across discovery identity promotion. |
| Live per-speaker volume | Fixed in this pass | Thumb tracking updates the UI locally; the final value is persisted and sent through a coalesced volume-only path. It no longer runs output selection, session connection, and all-device volume work for every pixel of a drag. |
| Master volume | Complete | Foobar's main volume remains a group multiplier and now shares the coalesced volume-only path. |

## Root cause of the volume stall

The old trackbar handler called `set_output_volume()` for every `WM_HSCROLL`
thumb position. Each call scheduled the same heavyweight worker used for group
selection. That worker held `transport_mutex_`, updated every output, checked
connections, and shared the lock used by PCM writes. RTSP round trips therefore
queued behind the drag gesture while audio delivery waited.

The corrected split is:

- speaker selection or discovery changes use the full control update;
- volume changes use only the affected output IDs;
- repeated values are coalesced before the worker acquires the transport lock;
- a mouse drag sends its final value after `TB_ENDTRACK` instead of every
  intermediate pixel;
- a late UI value is preserved if it arrives while a control update is in
  flight.

## Apple Music parity achieved

- a compact AirPlay audio glyph instead of a text-only generic button;
- clear title, selection summary, spacious rows, and restrained dividers;
- blue system-style accent for selected outputs and active slider ranges;
- rounded selection surfaces rather than Windows group-box/table styling;
- current group shown directly in the wide toolbar state;
- icon-only fallback at narrow toolbar widths;
- consistent light and dark appearances derived from foobar theme colors.

## Remaining work

These are product gaps, not fallbacks for broken AirPlay behavior:

1. Expose per-speaker latency offset in the everyday picker. The transport and
   registry support it, but the current popup does not provide the planned
   control.
2. Add named group presets and recent groups.
3. Surface per-speaker session phases and actionable inline errors instead of
   relying mainly on the shared status text and console.
4. Add artwork/now-playing context only if it remains compact; speaker routing
   must stay the primary hierarchy.
5. Map encrypted AirPlay event-channel feedback so device-side volume and
   remote playback actions update foobar and every visible UI surface.
6. Add accessibility metadata for the custom-drawn rows and sliders beyond the
   existing keyboard/focus support.
7. Validate final spacing, hit targets, scaling, and color contrast on 100%,
   125%, 150%, and 200% Windows display scaling with the packaged PR artifact.
