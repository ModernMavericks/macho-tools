# Release notes

One file per release, named for the tag, e.g. `1.0.0.md`. Its contents become the
GitHub Release body. (In repos that ship an updater they also become the Sparkle
appcast `<description>`; this repo has no appcast yet.)

**The file is optional.** A release with no matching file still gets a generated
body — title, "What changed", footer — and a committed file adds its prose
verbatim. What the publish job does refuse is an artifact whose
`RELEASE_NOTES.md` is missing or empty, and `release.yml` generates that on
every run, so the build fails first if it ever could not.

## Why there is no `build/version.sh`

This repo is its own upstream (no external thing to repackage), so — like `mavericks-porthole`,
the family's precedent for this shape — it computes its version in `release.yml` directly from
`UPSTREAM_VERSION`, rather than through a `build/version.sh` wrapper. Shipyard's shared
`scripts/version.sh` only ever produces the `<upstream>-mavericks.N` shape ports use; there is no
mode that emits this repo's own bare `X.Y.Z`, so a wrapper around it here would print the exact
suffix this repo's versioning deliberately does not use.
