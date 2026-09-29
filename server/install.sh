#!/usr/bin/env bash
# Idempotent provisioning of the water-controller part of hc-data (Debian 13), next to heating-controller.
# Run as root from /opt/wc-server (deploy.sh does this). Never restarts shared services: Mosquitto and
# PostgreSQL are only reloaded, so heating-controller devices and ingest keep running.
set -euo pipefail
cd "$(dirname "$0")"

SECRETS=/etc/wc-server/secrets.env
[ -f "$SECRETS" ] || { echo "$SECRETS missing" >&2; exit 1; }
set -a; . "$SECRETS"; set +a
: "${MQTT_DEVICE_PASS:?}" "${MQTT_INGEST_PASS:?}" "${PG_READ_PASS:?}" "${MQTT_MODEL_PASS:?}"

echo "== packages"
for pkg in mosquitto mosquitto-clients postgresql python3-venv; do
    dpkg -s "$pkg" >/dev/null 2>&1 || { apt-get update -q && DEBIAN_FRONTEND=noninteractive apt-get install -y -q "$pkg"; }
done

echo "== mosquitto (passwd/acl assembled from per-project fragments)"
# Safety copy of the files as they were before the first fragment-based run
[ -d /root/mosquitto-pre-fragments ] || { install -d -m 700 /root/mosquitto-pre-fragments &&
    cp -a /etc/mosquitto/passwd /etc/mosquitto/acl /root/mosquitto-pre-fragments/ 2>/dev/null || true; }
install -d -m 750 -o root -g mosquitto /etc/mosquitto/passwd.d /etc/mosquitto/acl.d
# First run on a heating-only broker: keep heating's users and ACL as its own fragments
if [ ! -f /etc/mosquitto/acl.d/heating.acl ] && [ -f /etc/mosquitto/acl ]; then
    install -m 640 -o root -g mosquitto /etc/mosquitto/acl /etc/mosquitto/acl.d/heating.acl
    grep -E '^(heating-controller|hc-ingest):' /etc/mosquitto/passwd > /etc/mosquitto/passwd.d/heating || true
    chown root:mosquitto /etc/mosquitto/passwd.d/heating && chmod 640 /etc/mosquitto/passwd.d/heating
fi
install -m 640 -o root -g mosquitto mosquitto/water.acl /etc/mosquitto/acl.d/water.acl
PASSWD_TMP=$(mktemp)
mosquitto_passwd -c -b "$PASSWD_TMP" water-controller "$MQTT_DEVICE_PASS"
mosquitto_passwd -b "$PASSWD_TMP" wc-ingest "$MQTT_INGEST_PASS"
mosquitto_passwd -b "$PASSWD_TMP" wc-model "$MQTT_MODEL_PASS"
install -m 640 -o root -g mosquitto "$PASSWD_TMP" /etc/mosquitto/passwd.d/water
rm -f "$PASSWD_TMP"
cat /etc/mosquitto/passwd.d/* > /etc/mosquitto/passwd.new
cat /etc/mosquitto/acl.d/*.acl > /etc/mosquitto/acl.new
chown root:mosquitto /etc/mosquitto/passwd.new /etc/mosquitto/acl.new
chmod 640 /etc/mosquitto/passwd.new /etc/mosquitto/acl.new
mv /etc/mosquitto/passwd.new /etc/mosquitto/passwd
mv /etc/mosquitto/acl.new /etc/mosquitto/acl
systemctl reload mosquitto          # SIGHUP: re-reads passwd and acl, keeps clients connected

echo "== postgresql"
PG_VER=$(ls /etc/postgresql | sort -V | tail -1)
PG_CONF=/etc/postgresql/$PG_VER/main
HBA_LINE="host    water           wc_read         192.168.11.0/24         scram-sha-256"
grep -qxF "$HBA_LINE" "$PG_CONF/pg_hba.conf" || { echo "$HBA_LINE" >> "$PG_CONF/pg_hba.conf"; systemctl reload postgresql; }

psql_admin() { runuser -u postgres -- psql -v ON_ERROR_STOP=1 -q "$@"; }
psql_admin -v read_pass="$PG_READ_PASS" <<'SQL'
SELECT 'CREATE ROLE wc_ingest LOGIN' WHERE NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'wc_ingest') \gexec
SELECT 'CREATE ROLE wc_read LOGIN'   WHERE NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'wc_read') \gexec
SELECT 'CREATE ROLE wc_model LOGIN'  WHERE NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'wc_model') \gexec
ALTER ROLE wc_read PASSWORD :'read_pass';
SELECT 'CREATE DATABASE water' WHERE NOT EXISTS (SELECT FROM pg_database WHERE datname = 'water') \gexec
SQL
psql_admin -d water -f db/schema.sql

echo "== ingest"
id wc_ingest >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin wc_ingest
[ -x venv/bin/python ] || python3 -m venv venv
venv/bin/pip install -q --disable-pip-version-check -r ingest/requirements.txt
install -m 644 ingest/wc-ingest.service /etc/systemd/system/wc-ingest.service

echo "== usage model: hourly check (training runs on a workstation, publish.py copies thresholds.json here)"
id wc_model >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin wc_model
install -d -m 755 -o wc_model -g wc_model /var/lib/wc-model /var/lib/wc-model/out
# The first version trained here (1 CPU, 1 GB: too small); drop its units and heavy venv
systemctl disable -q --now wc-model-train.timer 2>/dev/null || true
rm -f /etc/systemd/system/wc-model-train.service /etc/systemd/system/wc-model-train.timer
if model-venv/bin/python -c "import sklearn" 2>/dev/null; then rm -rf model-venv /var/lib/wc-model/data; fi
[ -x model-venv/bin/python ] || python3 -m venv model-venv
model-venv/bin/pip install -q --disable-pip-version-check -r model/requirements-score.txt
install -m 644 model/wc-model-score.service model/wc-model-score.timer /etc/systemd/system/

echo "== backups"
install -d -m 750 -o postgres -g postgres /var/backups/water
install -m 644 backup/wc-pg-backup.service backup/wc-pg-backup.timer /etc/systemd/system/

systemctl daemon-reload
systemctl enable -q --now wc-pg-backup.timer
# The hourly check starts once publish.py has delivered thresholds.json
if [ -f /var/lib/wc-model/out/thresholds.json ]; then systemctl enable -q --now wc-model-score.timer; fi
systemctl enable -q wc-ingest
systemctl restart wc-ingest

echo "== status"
systemctl is-active mosquitto postgresql wc-ingest wc-pg-backup.timer
systemctl is-active wc-model-score.timer || echo "(hourly model check waits for server/model/publish.py)"
systemctl is-active hc-ingest 2>/dev/null && echo "(heating ingest still running)" || true
