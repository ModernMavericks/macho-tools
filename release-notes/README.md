# Release notes

One file per release, named `<full-version>.md`, e.g. `1.0.0.md`.
Its contents become the Sparkle appcast `<description>` and the GitHub Release body.
The publish job fails if the notes for the version being published are missing or empty.

## Why there is no `build/version.sh`

This repo is its own upstream (no external thing to repackage), so — like `mavericks-porthole`,
the family's precedent for this shape — it computes its version in `release.yml` directly from
`UPSTREAM_VERSION`, rather than through a `build/version.sh` wrapper. Shipyard's shared
`scripts/version.sh` only ever produces the `<upstream>-mavericks.N` shape ports use; there is no
mode that emits this repo's own bare `X.Y.Z`, so a wrapper around it here would print the exact
suffix this repo's versioning deliberately does not use.
