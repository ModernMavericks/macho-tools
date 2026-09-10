# Build ingredients

Everything baked into the shipped artifacts, and how a change to it reaches a
release. An *ingredient* is an input to the product; the *own upstream* is the
thing this repo exists to port.

This repo is unusual in the family: **it is its own upstream.** The six tools are
not a port of somebody else's project — they were written for this problem, and
`Mavericks-Porting-Resources` is where they lived before, not what they track.

| Ingredient | Pinned in | Renovate | On a bump |
|---|---|---|---|
| the six tools' C sources (own upstream) | `UPSTREAM_VERSION`, bumped by hand | **untrackable** — nothing external releases them; they are this repo | a hand bump plus a `*-mavericks.*` tag cuts the release |
| MacOSX10.9 SDK, CMake helpers, compat guard, test runner | `ModernMavericks/shipyard@v1` | ✅ github-actions manager tracks the tag | `@v1` is a *moving* tag: content changes without the pin changing, so nothing auto-repackages |
| `tests/fixture.macho` + `tests/EXPECTED` | committed | **untrackable** — a characterization reference, deliberately frozen | never bumped by a bot; changing it is a deliberate commit that says why |

Not ingredients: `CMakeLists.txt`, the test scripts and the workflows are this
repo's own recipe. A change there is a repackage you cut deliberately.

## Why there is no Renovate customManager for the own upstream

Every other repo in the family points a customManager at `UPSTREAM_VERSION` so a
new upstream release opens a bump PR that, once green, cuts a release. There is
nothing to point one at here: no external project publishes these tools, so
there is no datasource that could observe a new version. The pin moves when a
human decides it has, which is the honest arrangement rather than a missing one.

That is also why the release model is **tag-only-publish** rather than
auto-cut-on-main: with no upstream bump to trigger it, a release is always a
deliberate act.

## Why `tests/EXPECTED` is an ingredient

It is the digest of what the whole pipeline produces from `tests/fixture.macho`,
**recorded from a native build on real 10.9 hardware**. CI has no 10.9 runner, so
every build there is a cross-build; comparing its output against that digest is
what makes "cross ≡ native" checkable. Treating it as an ingredient is the point:
it must change only when someone means it to.

## Upstream release notes

No upstream release notes: macho-tools is its own upstream (original ModernMavericks code), so
there are no someone-else's notes for a release to link.
