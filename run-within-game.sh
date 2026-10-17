#!/bin/bash
#
# Palworld VR Injector Launcher
# Launches the game with the injector library preloaded via LD_PRELOAD.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INJECTOR_LIB="${SCRIPT_DIR}/libpalworldvr.so"
GAME_EXE="/home/joshc/.local/share/Steam/steamapps/common/Palworld/Pal/Binaries/Win64/Palworld-Win64-Shipping.exe"

# Fallback: try wine64, wine, proton, or protontricks if none is installed
WINE_CMD=""
for cmd in wine64 wine proton protontricks; do
    if command -v "$cmd" &>/dev/null; then
        WINE_CMD="$cmd"
        break
    fi
done

if [ -z "$WINE_CMD" ]; then
    echo "[ERROR] No Wine-compatible runner found. Install Proton (Steam Play) or install wine." >&2
    exit 1
fi

# Verify injector library exists (built by 'make all' in this directory)
if [ ! -f "$INJECTOR_LIB" ]; then
    echo "[ERROR] Injector library not found: $INJECTOR_LIB" >&2
    echo "        Build it first: cd \"$SCRIPT_DIR\" && make all" >&2
    exit 1
fi

echo "=== Palworld VR Injector ==="
echo "Lib:    $INJECTOR_LIB"
echo "Game:   $GAME_EXE"
echo "Wine:   $WINE_CMD"
echo ""

# Preload the injector library — append to any existing LD_PRELOAD value.
if [ -n "${LD_PRELOAD:-}" ]; then
    export LD_PRELOAD="${LD_PRELOAD}:${INJECTOR_LIB}"
else
    export LD_PRELOAD="$INJECTOR_LIB"
fi

exec "$WINE_CMD" "$GAME_EXE" "$@"
