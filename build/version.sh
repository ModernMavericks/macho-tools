#!/bin/sh
# Thin wrapper: the logic lives in shipyard (scripts/version.sh) so it cannot drift between repos.
# UPSTREAM_VERSION here holds THIS repo's own version -- it is its own upstream -- hand-bumped,
# with no Renovate customManager watching it because nothing external releases it.
set -eu
SELF="$(cd "$(dirname "$0")" && pwd)"
MAVERICKS_ROOT="$(cd "$SELF/.." && pwd)"; export MAVERICKS_ROOT
. "$SELF/msc.sh"
exec sh "$SHIPYARD/version.sh" "$@"
