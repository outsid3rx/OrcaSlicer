#!/bin/sh
# uv owns the isolated Python runtime; never install into system Python.
set -eu
if ! command -v uv >/dev/null 2>&1; then
    echo 'uv is required. Install it from https://docs.astral.sh/uv/getting-started/installation/ and retry.' >&2
    exit 1
fi
SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
# A dry run must not download Python or create a uv environment/cache.
for arg in "$@"; do
    if [ "$arg" = '--dry-run' ]; then
        exec uv run --offline --no-cache --no-project --no-python-downloads --python 3.12 "$SCRIPT_DIR/setup_mcp.py" "$@"
    fi
done
exec uv run --no-project --python 3.12 "$SCRIPT_DIR/setup_mcp.py" "$@"
