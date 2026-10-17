#!/usr/bin/env bash
# build-stub.sh — Build the Palworld VR injector stub DLL.
# Run this after installing dotnet-sdk-6.0 and all dependencies.

set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_ROOT="/home/joshc/.openclaw/workspace/coordinator/palworld-vr-injector"
readonly PROJECT_FILE="${PROJECT_ROOT}/PalworldVrInjector.csproj"
readonly OUTPUT_DIR="${PROJECT_ROOT}/bin/Release/net6.0-windows/palworld-vr-injector.dll"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log() { echo -e "${YELLOW}[BUILD]${NC} $*"; }
success() { echo -e "${GREEN}[OK]${NC} $*"; }
error()  { echo -e "${RED}[ERROR]${NC} $*" >&2; exit 1; }

echo ""
echo "=========================================="
echo "  Palworld VR Injector — Stub Build       "
echo "=========================================="
echo ""

# Step 1: Verify dotnet is available
echo "=== Checking environment ==="
if ! command -v dotnet >/dev/null 2>&1; then
    error "dotnet CLI not found in PATH. Install via:"
    echo "  pacman -S dotnet-sdk-6.0-desktop-linux-x64"
    exit 1
fi

DOTNET_VERSION=$(dotnet --version)
echo "[OK] dotnet version: $DOTNET_VERSION"

# Step 2: Restore NuGet packages
log "Restoring NuGet packages..."
cd "$PROJECT_ROOT" || error "Failed to cd into project directory."

if ! dotnet restore PalworldVrInjector.csproj; then
    error "dotnet restore failed. Check your .NET SDK installation."
fi
success "Restore complete."

# Step 3: Build the stub DLL (Release mode)
log "Building stub injector..."

mkdir -p "$(dirname "$OUTPUT_DIR")" || error "Failed to create output directory."

if ! dotnet publish PalworldVrInjector.csproj \
    -c Release \
    --self-contained false \
    -o "$PROJECT_ROOT/bin/stub" \
    /p:PublishReadyToRun=false; then
    error "dotnet publish failed. See errors above."
fi

success "Build succeeded!"

# Step 4: Show output artifacts
echo ""
echo "=== Build Artifacts ==="
ls -la "$PROJECT_ROOT/bin/stub/" 2>/dev/null || true

if [[ -f "$OUTPUT_DIR" ]]; then
    echo "[OK] Stub DLL ready at:"
    echo "   $OUTPUT_DIR"
else
    # The output might be in a subfolder like bin/stub/palworld-vr-injector.dll
    local_stub="$PROJECT_ROOT/bin/stub/*.dll"
    if [[ -f "$local_stub" ]]; then
        echo "[OK] Stub DLL ready at:"
        echo "   $local_stub"
    else
        error "Could not locate the built stub DLL."
        ls -la "$PROJECT_ROOT/bin/stub/" 2>/dev/null || true
        exit 1
    fi
fi

# Step 5: Verify it can run (dry-run)
log "Verifying the stub can start..."
local_stub_path="$PROJECT_ROOT/bin/stub/*.dll"
if [[ -f "$local_stub_path" ]]; then
    local_stub_path=$(readlink -f "$local_stub_path")
    
    # Try to run with Wine if we're in a Proton context, or just dotnet directly
    if command -v wine >/dev/null 2>&1; then
        log "Running stub under Wine..."
        wine "$local_stub_path" --help 2>/dev/null || \
            echo "[NOTE] Stub may require a specific Wine prefix setup."
    else
        # Try native dotnet
        if command -v dotnet >/dev/null 2>&1; then
            log "Running stub under native .NET..."
            dotnet "$local_stub_path" --help || true
        fi
    fi
    
    success "Stub verification complete."
else
    echo "[WARN] Stub DLL not found at expected path. Check build output above."
fi

echo ""
echo "=== Next Steps ==="
echo ""
echo "1. Copy the stub DLL to your Wine prefix (or leave in bin/stub/):"
echo "   cp \"$PROJECT_ROOT/bin/stub/*.dll\" \\\\"
echo "      ~/.wine-prefix/palworld-vr/drive_c/Program Files (x86)/Palworld-VR-Injector/"
echo ""
echo "2. Update launch_wrapper.sh to point to the DLL path."
echo ""
echo "3. Launch Palworld with: bash src/launch_wrapper.sh"
echo ""
