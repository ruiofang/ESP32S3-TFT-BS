#!/usr/bin/env bash
# Read PROJECT_VER from CMakeLists.txt, build, and emit build/latest.json.
#
# Usage:
#   tools/make_release.sh
#
# The version is read from CMakeLists.txt (PROJECT_VER), not modified here.
# Bump the version manually in CMakeLists.txt before running this script.
#
# After it finishes, upload build/panda.bin + build/latest.json to the
# release server at http://120.27.145.121:8090/ESP32S3-TFT-BS/releases/main/.

set -euo pipefail

cd "$(dirname "$0")/.."
REPO_ROOT="$PWD"
CMAKE="$REPO_ROOT/CMakeLists.txt"

# --- Read PROJECT_VER from CMakeLists.txt (do not modify) ---
if ! grep -qE '^set\(PROJECT_VER "[0-9]+\.[0-9]+\.[0-9]+"\)' "$CMAKE"; then
    echo "error: PROJECT_VER line not found in $CMAKE" >&2
    exit 1
fi
VERSION=$(grep -oP 'set\(PROJECT_VER "\K[0-9]+\.[0-9]+\.[0-9]+' "$CMAKE")
echo "[make_release] PROJECT_VER = $VERSION"

# --- URLs ---
RELEASE_BASE_URL="http://120.27.145.121:8090/ESP32S3-TFT-BS/releases/main"
MANIFEST_URL="$RELEASE_BASE_URL/latest.json"
DOWNLOAD_URL="$RELEASE_BASE_URL/panda.bin"

# --- Build ---
if ! command -v idf.py >/dev/null 2>&1; then
    echo "error: idf.py not on PATH. Source export.sh first:" >&2
    echo "    source ~/esp/v5.5.2/esp-idf/export.sh" >&2
    exit 1
fi
idf.py build

# --- Emit latest.json ---
cat > build/latest.json <<EOF
{"version":"$VERSION","url":"$DOWNLOAD_URL"}
EOF
echo "[make_release] wrote build/latest.json:"
cat build/latest.json
echo

cat <<EOF
[make_release] done. Next steps:
  1. Upload to release server:
       scp build/panda.bin build/latest.json user@120.27.145.121:/path/to/ESP32S3-TFT-BS/releases/main/
  2. Verify:
       curl -s $MANIFEST_URL
       curl -sI $DOWNLOAD_URL
EOF
