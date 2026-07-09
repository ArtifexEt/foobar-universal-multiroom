# Project Instructions

- Do not ship fallback features as substitutes for correct protocol behavior.
- Prefer native AirPlay 2 discovery, pairing, session, timing, and error-state fixes over manual endpoint entry or user-managed workarounds.
- Diagnostic probe paths may exist only when they are clearly separated from the product path and do not mask real AirPlay failures.
- Ship changes through pull requests. Run the PR build and use its uploaded artifacts for testing; PR builds must not publish a normal release.
- Wait for the GPT bot review before merging a PR. Inspect whether the review comments make sense, address actionable feedback, rerun checks, and merge only after that review loop is complete.
