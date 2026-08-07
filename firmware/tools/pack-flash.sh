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
by_offset = sorted(args["flash_files"].items(), key=lambda kv: int(kv[0], 16))
files = [v for _, v in by_offset]
# python3, not python: macOS ships no bare "python" (and PEP 394 leaves it
# optional everywhere else), so the generated command has to name the
# interpreter that actually exists on the flashing machine.
cmd = "python3 -m esptool --chip esp32s3 -b 460800 write-flash " + " ".join(
    f"{off} {os.path.basename(path)}" for off, path in by_offset)
with zipfile.ZipFile(os.path.join(b, "flash-pack.zip"), "w") as z:
    for f in files:
        z.write(os.path.join(b, f), os.path.basename(f))
    z.writestr("FLASH-COMMAND.txt", cmd + "\n")
print("wrote", os.path.join(b, "flash-pack.zip"))
EOF2
