# Universal Multiroom Audio Bridge UI Audit

## Scope

This audit covers the everyday speaker-selection surfaces, their connection to
the AirPlay control path, and visual parity with the output picker used by
Apple Music. Preferences and protocol diagnostics are intentionally secondary.

## Implemented

| Area | State | Notes |
| --- | --- | --- |
| Playback command | Complete | `Playback > Select Wireless Speakers...` opens the full picker. In a Buttons toolbar, use `Customize Buttons`, then choose `Playback > Select Wireless Speakers...`. |
| Native toolbar dropdown | Complete | Foobar2000 2.x `toolbarDropDown` service named `Wireless Speakers`, with an active-session label, quick toggles, and an entry into the full picker. Add a `Toolbar Dropdown` control to the toolbar row and select `Wireless Speakers` as its data source. |
| Dockable compact control | Complete | Default UI playback-information element named `Wireless Speakers`, with a scalable audio glyph, active destination (`Idle`, `Connecting...`, or ready receivers), keyboard activation, focus state, and light/dark colors. |
| Speaker popup | Complete for normal selection | Apple Music-style hierarchy with a clean centered product title, `Speakers & TVs` section, flat device rows, speaker glyphs, trailing check indicators, custom accent sliders, percentages, conditional PIN pairing, rounded popover clipping, refresh footer, scrolling, theme adaptation, and automatic dismissal. Parent and child surfaces are fully invalidated after scroll/control rebuilds so stale or blank fragments are not retained. |
| Dropdown visibility | Complete | Every discovered speaker has a persistent checkbox in Preferences. Clearing it removes only that speaker from the toolbar dropdown; selection and playback state are unchanged, and legacy saved state defaults to visible. |
| Persisted selection and volume | Complete | Canonical speaker IDs and aliases retain selection/volume across discovery identity promotion. |
| Live per-speaker volume | Fixed in this pass | Thumb tracking updates the UI locally; the final value is persisted and sent through a coalesced, control-plane-only path. Synchronous RTSP volume requests no longer own either the PCM transport mutex or the session mutex while waiting for the receiver. |
| Master volume and signal quality | Complete | PCM is sent at full scale. Foobar's main volume remains a receiver-side group multiplier: effective receiver volume is `master × speaker`, clamped to 0–100%. |
| Group timing | Fixed in this pass | Every receiver now uses one group-wide NTP/RTP clock anchor. Per-speaker offsets alter presentation time without creating a separate wall-clock origin for each session. |
| Receiver playback controls | Complete | Encrypted AirPlay events for play, pause, toggle, stop, next, and previous are applied to foobar on its main thread; duplicate group events are suppressed. |
| Non-blocking UI/startup | Fixed in this pass | Popup/preferences discovery starts on the refresh worker. AirPlay discovery, pair verification, session SETUP, metadata setup, receiver connection, and seek/flush network work run on workers instead of blocking `output::open()` or a foobar UI callback. |
| Current playback destination | Fixed in this pass | Toolbar labels are derived from ready/open AirPlay sessions rather than the persisted speaker checkboxes, and are refreshed on connect, reconfiguration, failure, and stop. |
| Named virtual groups | Complete | Preferences provide a dedicated group editor with persisted names and member checkboxes. Applying a group replaces the selected set atomically, resolves discovery aliases, skips temporarily unavailable members, and is exposed in both speaker selection surfaces. |

## Root cause of the volume stall

The old trackbar handler called `set_output_volume()` for every `WM_HSCROLL`
thumb position. Each call scheduled the same heavyweight worker used for group
selection. That worker held `transport_mutex_`, updated every output, checked
connections, and shared the lock used by PCM writes. RTSP round trips therefore
queued behind the drag gesture while audio delivery waited.

The corrected split is:

- speaker selection or discovery changes use the full control update;
- volume changes use only the affected output IDs;
- repeated values are coalesced before the worker sends receiver control;
- volume-only RTSP waits do not acquire the PCM transport lock and do not hold
  the session-state mutex used by packet delivery;
- a mouse drag sends its final value after `TB_ENDTRACK` instead of every
  intermediate pixel;
- a late UI value is preserved if it arrives while a control update is in
  flight.

## Repaint and startup stalls corrected

The popup previously relied on `WM_ERASEBKGND`, while most state changes called
`Invalidate()` without requesting an erase. Destroying and recreating child
trackbars during scroll therefore exposed regions which the next `WM_PAINT`
did not fill. The popup now paints its entire background in `WM_PAINT` and
invalidates parent plus child windows as one redraw operation.

The speaker refresh entry point also called `start_discovery()` before creating
its worker, and the foobar output called the full AirPlay connection path from
`output::open()`. Both paths could include Bonjour waits and RTSP/crypto network
round trips. Discovery now begins inside the refresh worker, while the output
queues PCM and lets its render worker establish the native AirPlay session.
Stopping or reopening output cancels the pending setup: discovery is performed
in short cancellable slices and pending TCP/event sockets are closed to wake
connect/send/receive immediately before the render worker is joined.

## Apple Music parity achieved

- a compact AirPlay audio glyph instead of a text-only generic toolbar button;
- a clean centered AirPlay title and a separate `Speakers & TVs` section;
- flat, spacious device rows with a speaker glyph on the leading edge, the
  selection control on the trailing edge, and restrained inset dividers;
- per-device volume directly beneath the device name, without turning each
  output into a separate Windows-style card;
- blue system-style accent for selected outputs and active slider ranges;
- a single rounded popover surface with a distinct full-width footer action;
- PIN controls shown only when discovery reports an explicit password/PIN
  requirement, so transient AirPlay 2 encryption does not clutter the normal
  picker and it stays focused on routing;
- actual ready playback destination shown directly in the toolbar state;
- icon-only fallback at narrow toolbar widths;
- consistent light and dark appearances derived from foobar theme colors, with
  a dark Apple-style default for the command surface that has no UI callback.

## Remaining work

These are product gaps, not fallbacks for broken AirPlay behavior:

1. Expose per-speaker latency offset in the everyday picker. The transport and
   registry support it, but the current popup does not provide the planned
   control.
2. Add a compact recent-groups history in addition to the completed named virtual groups.
3. Surface per-speaker session phases and actionable inline errors instead of
   relying mainly on the shared status text and console.
4. Map device-side volume feedback so receiver changes update foobar and every
   visible UI surface. Playback actions are already mapped.
5. Add accessibility metadata for the custom-drawn rows and sliders beyond the
   existing keyboard/focus support.
6. Validate final spacing, hit targets, scaling, and color contrast on 100%,
   125%, 150%, and 200% Windows display scaling with the packaged PR artifact.
7. Add first-class receiver-type metadata to discovery if product-specific
   icons (HomePod, Apple TV, Mac, television) are desired. The current picker
   intentionally uses one honest generic speaker glyph instead of guessing a
   device class from its display name.
8. Add optional compact now-playing context to the local speaker popup only if
   it does not displace routing controls. Protocol delivery of current metadata
   and artwork to receivers is implemented independently of this UI choice.
9. Validate both documented toolbar flows (`Customize Buttons > Playback >
   Select Wireless Speakers...` and `Toolbar Dropdown > Wireless Speakers`)
   against the exact Default UI version used for packaged-artifact testing.
