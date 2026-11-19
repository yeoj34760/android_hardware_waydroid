#!/bin/sh
# Run from any directory; an optional argument selects a different source tree.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TREE=${1:-$(CDPATH= cd -- "$HERE/../.." && pwd)}
for test in "$HERE"/test_*.py; do
    python3 "$test" "$TREE"
done
