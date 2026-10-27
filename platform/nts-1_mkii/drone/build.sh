#!/bin/bash
# Build script for drone NTS-1 mkII ARM unit

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "============================================"
echo "Building drone NTS-1 mkII ARM unit"
echo "============================================"

cd "$SCRIPT_DIR"
make install

echo ""
echo "============================================"
echo "Build complete!"
echo "============================================"
echo "ARM unit: $SCRIPT_DIR/drone.nts1mkiiunit"
