#!/usr/bin/env bash
# One-time ESP-IDF install for the build host. Pin: v5.5 (record any change here).
set -euo pipefail
IDF_DIR="${IDF_DIR:-$HOME/esp/esp-idf}"
if [ ! -d "$IDF_DIR" ]; then
  mkdir -p "$(dirname "$IDF_DIR")"
  git clone --depth 1 --branch v5.5 --recursive https://github.com/espressif/esp-idf.git "$IDF_DIR"
fi
"$IDF_DIR/install.sh" esp32s3
echo "Done. Activate with: source $IDF_DIR/export.sh"
