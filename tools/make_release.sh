#!/usr/bin/env bash
# Bump PROJECT_VER, build, and emit build/latest.json ready to upload.
#
# Usage:
#   tools/make_release.sh 1.0.1
#
# After it finishes, drag build/panda.bin + build/latest.json into a new
# GitHub release tagged v<version> (Settings → Releases → Draft a new release).
#
# Optional: if `gh` is installed and authenticated, pass --publish to
# create the release and upload assets in one step.

set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "usage: $0 <version> [--publish]"
    echo "  version must be N.N.N (e.g. 1.0.1)"
    exit 2
fi

VERSION="$1"
PUBLISH=0
[[ "${2-}" == "--publish" ]] && PUBLISH=1

# Sanity-check version format — device's version_cmp() parses %d.%d.%d.
if ! [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "error: version must be N.N.N (got: $VERSION)" >&2
    exit 2
fi

cd "$(dirname "$0")/.."
REPO_ROOT="$PWD"
CMAKE="$REPO_ROOT/CMakeLists.txt"
ASSET_URL="https://github.com/ruiofang/ESP32S3-TFT-BS/releases/latest/download/panda.bin"

# Bump PROJECT_VER in the top-level CMakeLists.txt.
if ! grep -qE '^set\(PROJECT_VER "[0-9]+\.[0-9]+\.[0-9]+"\)' "$CMAKE"; then
    echo "error: PROJECT_VER line not found in $CMAKE" >&2
    exit 1
fi
sed -i -E "s/^set\(PROJECT_VER \"[0-9]+\.[0-9]+\.[0-9]+\"\)/set(PROJECT_VER \"$VERSION\")/" "$CMAKE"
echo "[make_release] bumped PROJECT_VER -> $VERSION"

# Build. Expect ESP-IDF env already sourced; if not, give a helpful error.
if ! command -v idf.py >/dev/null 2>&1; then
    echo "error: idf.py not on PATH. Source export.sh first:" >&2
    echo "    source ~/esp/v5.5.2/esp-idf/export.sh" >&2
    exit 1
fi
idf.py build

# Emit latest.json next to panda.bin.
cat > build/latest.json <<EOF
{"version":"$VERSION","url":"$ASSET_URL"}
EOF
echo "[make_release] wrote build/latest.json:"
cat build/latest.json
echo

# Optional: publish via gh.
if [[ "$PUBLISH" -eq 1 ]]; then
    if ! command -v gh >/dev/null 2>&1; then
        echo "error: --publish needs the gh CLI (https://cli.github.com/)" >&2
        exit 1
    fi
    TAG="v$VERSION"
    git add CMakeLists.txt
    git commit -m "Release $TAG" || echo "[make_release] nothing to commit"
    git push
    gh release create "$TAG" \
        build/panda.bin build/latest.json \
        --title "$TAG" \
        --generate-notes \
        --latest
    echo "[make_release] gh release $TAG created."
else
    cat <<EOF
[make_release] done. Next steps:
  1. git add CMakeLists.txt && git commit -m "Release v$VERSION" && git push
  2. https://github.com/ruiofang/ESP32S3-TFT-BS/releases/new
     - tag: v$VERSION (Create new tag on publish)
     - attach: build/panda.bin AND build/latest.json
     - Set as the latest release
     - Publish
  3. Verify:
     curl -sL -o /dev/null -w "%{http_code} -> %{url_effective}\n" \\
       https://github.com/ruiofang/ESP32S3-TFT-BS/releases/latest/download/latest.json
EOF
fi
