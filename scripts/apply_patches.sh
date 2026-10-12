#!/usr/bin/env bash
# Apply this project's changes to the two framework submodules.
#
# psxrecomp and recomp-ui are pinned to their upstream commits; the changes
# this port needs (MSVC build fixes, the button-prompt bar, Smooth motion and
# widescreen fixes) live in patches/ and are applied on a local branch.
# Safe to re-run: a submodule already on that branch is left alone.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BRANCH="vagrant-story-local"

git -C "$ROOT" submodule update --init --recursive

for sub in psxrecomp recomp-ui; do
    dir="$ROOT/$sub"
    if [[ "$(git -C "$dir" rev-parse --abbrev-ref HEAD)" == "$BRANCH" ]]; then
        echo "$sub: already on $BRANCH, skipping"
        continue
    fi
    git -C "$dir" checkout -q -B "$BRANCH"
    git -C "$dir" -c user.name="patches" -c user.email="patches@localhost" \
        am -q --committer-date-is-author-date "$ROOT/patches/$sub"/*.patch
    echo "$sub: applied $(ls "$ROOT/patches/$sub"/*.patch | wc -l) patch(es) on $BRANCH"
done
