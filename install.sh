#!/usr/bin/env bash
# Build as the current user; request sudo only when installing the tested package.
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "$script_dir/scripts/package.py" --install "$@"
