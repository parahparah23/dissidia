#!/bin/sh
# Frame-rate parity test: every move of every playable character must play at every frame rate the game offers as it does at
# 30 fps (the PSP's rate). Boots the game headless once per character and rate (tools/fps_parity.py, docs/testing.md
# "Frame-rate parity tests"); exit code 1 when any trial differs. Long: hours for the whole roster at all 8 rates.
#
#   run_fps_parity_test.sh <work dir> [out dir] [fps_parity.py run options...]
#     e.g. run_fps_parity_test.sh ~/dissidia/work /tmp/parity --chars squall,exdeath --rates 30,60,120
#
# Needs a runtime built from this tree in <work dir>/build, Xvfb, python3 + Pillow and a save with Quick Battle unlocked.
set -e
here=$(cd "$(dirname "$0")" && pwd)
work=${1:?usage: run_fps_parity_test.sh <work dir> [out dir] [options]}
shift
out=${1:-${TMPDIR:-/tmp}/fps_parity}
[ $# -gt 0 ] && shift
exec python3 "$here/../tools/fps_parity.py" run --work "$work" --out "$out" "$@"
