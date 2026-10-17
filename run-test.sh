#!/bin/bash
set -euo pipefail

TARGET_PID="${1:-}"
if [[ -z "$TARGET_PID" ]]; then
    echo "[ERROR] No PID provided. Use: ./run-test.sh <PID>"
    exit 1
fi

echo "=== Palworld VR Injector — Test Run ==="
echo "Target PID: $TARGET_PID"
echo ""

# Build
cd "$(dirname "$0")/PalworldVrInjector"
dotnet build -c Release --no-restore > /dev/null 2>&1 || {
    echo "[ERROR] Build failed."
    exit 1
}

DLL_PATH="PalworldVrInjector/bin/Release/net6.0-windows/PalworldVrInjector.dll"

# Run the injector stub against the target process
echo "--- Running injector stub ---"
dotnet "$DLL_PATH" --target-pid "$TARGET_PID"
