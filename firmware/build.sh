#!/bin/zsh
# ESP-IDF 5.4.2 build: ./build.sh [idf.py args...]   (default: build)
# A home-idf checkout next to this repository (../../home-idf) replaces the pinned git version,
# unless HOME_IDF_FROM_GIT=1.
source "$HOME/.espressif/tools/activate_idf_v5.4.2.sh" >/dev/null 2>&1
cd "$(dirname "$0")" || exit 1
[ $# -eq 0 ] && set -- build
# 230400 is the fastest reliable rate on the spare board USB adapter (460800+ corrupts packets)
export ESPBAUD=${ESPBAUD:-230400}
local_home_idf="$(cd ../.. && pwd)/home-idf"
if [ -z "$HOME_IDF_FROM_GIT" ] && [ -d "$local_home_idf" ]; then
    export HOME_IDF_LOCAL="$local_home_idf"
else
    unset HOME_IDF_LOCAL
fi
exec "$IDF_PYTHON_ENV_PATH/bin/python" "$IDF_PATH/tools/idf.py" "$@"
