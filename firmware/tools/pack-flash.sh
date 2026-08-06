#!/usr/bin/env bash
# Zip the flashable artifacts for a machine with USB access to the device.
set -euo pipefail
B="$(dirname "$0")/../build"
OUT="$B/flash-pack.zip"
rm -f "$OUT"
python3 - "$B" <<'EOF2'
import json, os, sys, zipfile
b = sys.argv[1]
args = json.load(open(os.path.join(b, "flasher_args.json")))
files = [v for _, v in sorted(args["flash_files"].items())]
cmd = "python -m esptool --chip esp32s3 -b 460800 write_flash " + " ".join(
    f"{off} {os.path.basename(path)}" for off, path in sorted(args["flash_files"].items()))
with zipfile.ZipFile(os.path.join(b, "flash-pack.zip"), "w") as z:
    for f in files:
        z.write(os.path.join(b, f), os.path.basename(f))
    z.writestr("FLASH-COMMAND.txt", cmd + "\n")
print("wrote", os.path.join(b, "flash-pack.zip"))
EOF2
