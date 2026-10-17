#!/bin/bash
#
# Palworld VR Injector Launcher
# Launches the game with our stub library preloaded via LD_PRELOAD.
#

set -euo pipefail

STUB_LIB="/home/joshc/.openclaw/workspace/coordinator/palworld-vr-injector/libpalworldvrstub.so"
GAME_EXE="/home/joshc/.local/share/Steam/steamapps/common/Palworld/Pal/Binaries/Win64/Palworld-Win64-Shipping.exe"

# Fallback: try wine, proton, or protontricks if wine64 isn't installed
WINE_CMD=""
for cmd in wine proton protontricks; do
    if command -v "$cmd" &>/dev/null; then
        WINE_CMD="$cmd"
        break
    fi
done

if [ -z "$WINE_CMD" ]; then
    echo "[ERROR] No Wine-compatible runner found. Install Proton (Steam Play) or install wine." >&2
    exit 1
fi

# Verify stub library exists
if [ ! -f "$STUB_LIB" ]; then
    echo "[ERROR] Stub library not found: $STUB_LIB" >&2
    exit 1
fi

echo "=== Palworld VR Injector ==="
echo "Stub:   $STUB_LIB"
echo "Game:   $GAME_EXE"
echo "Wine:   $WINE_CMD"
echo ""

# Launch with stub preloaded — initialize LD_PRELOAD to avoid unbound variable error
export LD_PRELOAD="${LD_PRELOAD:-}"
exec "$WINE_CMD" "$GAME_EXE" "$@"
