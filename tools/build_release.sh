#!/bin/zsh
# Release build: firmware + migrator (with the firmware's bootloader and partition table embedded).
# Output in releases/ (gitignored) with sha256 files. Uses ../home-idf when present (see firmware/build.sh).
# tools/build_release.sh --spare   rehearsal images that download water-controller-spare.bin (separate build dirs)
set -eo pipefail
ROOT=${0:A:h:h}
local_home_idf="${ROOT:h}/home-idf"
if [ -z "$HOME_IDF_FROM_GIT" ] && [ -d "$local_home_idf" ]; then
    export HOME_IDF_LOCAL="$local_home_idf"
    TOOL="$local_home_idf/tools/build_migrator.sh"
else
    TOOL="$ROOT/migrator/managed_components/home-idf/tools/build_migrator.sh"
fi
if [ "$1" = "--spare" ]; then
    shift
    export BUILD_DIR=build-spare
    set -- -B build-spare -DWATER_OTA_FILE=water-controller-spare.bin "$@"
    OUT="$ROOT/releases/spare"
else
    OUT="$ROOT/releases"
fi
"$TOOL" "$ROOT/firmware" "$ROOT/migrator" "$OUT" "$@"
