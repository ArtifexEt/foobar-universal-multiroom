# macOS Playback Tester

`MultiroomMacPlaybackTester` is the macOS command-line tester for the portable
multiroom core and AirPlay transport. It is not a foobar2000 component. It uses
system Bonjour discovery, selects AirPlay 2 outputs, opens the same
`MultiroomEngine`/`AirPlayTransport` path used by the Windows component, and
sends a generated 44.1 kHz stereo test tone.

## Commands

List visible AirPlay receivers:

```bash
./MultiroomMacPlaybackTester --list-only --timeout-ms 5000 --require-speaker
```

Play a short test tone:

```bash
./MultiroomMacPlaybackTester --target korytarz --duration-ms 5000 --volume 30 --require-speaker
./MultiroomMacPlaybackTester --target wiim --duration-ms 5000 --volume 35 --require-speaker
```

If an AirPlay receiver requires PIN pairing:

```bash
./MultiroomMacPlaybackTester --target "Living Room" --pin 1234 --duration-ms 5000 --volume 35 --require-speaker
```

## Expected Output

A successful playback test prints:

```text
SESSIONS stage=connected count=1
SESSION name="Korytarz" ... phase=ready open=yes ...
AUDIO chunks=... queued_packets_before_flush=... duration_ms=...
```

If discovery succeeds but a receiver rejects AirPlay 2 negotiation, the tester
prints the failing protocol stage, for example session `SETUP`, stream `SETUP`,
or audio write.
