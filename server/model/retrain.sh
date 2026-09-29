#!/usr/bin/env bash
# Monthly on hc-data (wc-model-train.service): refresh the flow cache, retrain, and publish the learned limits.
# publish.py holds back a run that fails its guard, so the device keeps the previous limits.
set -euo pipefail
cd "$(dirname "$0")"
PY=${WC_MODEL_PYTHON:-python3}
"$PY" data.py --refresh
"$PY" train.py
"$PY" anomaly.py
"$PY" publish.py
