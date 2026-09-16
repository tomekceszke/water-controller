-- water-controller schema (PostgreSQL, database "water"). Idempotent: safe to re-apply.
-- device = publisher MAC from the MQTT topic; 'bigquery' for history imported from GCP.

CREATE TABLE IF NOT EXISTS flow_event (
    device           text        NOT NULL,
    start_ts         timestamptz NOT NULL,
    stop_ts          timestamptz NOT NULL,
    pulses           integer     NOT NULL CHECK (pulses >= 0),
    pulses_per_liter integer,                           -- calibration at the time; NULL = unknown (history)
    max_lpm          real,
    closed           boolean,                           -- the flow ended because the valve closed
    suspect          boolean     NOT NULL DEFAULT false, -- legacy firmware: 16-bit counter wrap detected
    pulses_corrected integer,                           -- suspect rows: pulses minus the removed wraps
    PRIMARY KEY (device, start_ts)                      -- also dedups QoS 1 redeliveries and re-imports
);
ALTER TABLE flow_event ADD COLUMN IF NOT EXISTS pulses_corrected integer;
CREATE INDEX IF NOT EXISTS flow_event_start_brin ON flow_event USING brin (start_ts);

CREATE TABLE IF NOT EXISTS flow_sample (
    device      text        NOT NULL,
    ts          timestamptz NOT NULL,
    pulses      integer     NOT NULL,
    period_ms   integer     NOT NULL,
    lpm         real,
    flow_pulses integer,
    PRIMARY KEY (device, ts)
);
CREATE INDEX IF NOT EXISTS flow_sample_ts_brin ON flow_sample USING brin (ts);

CREATE TABLE IF NOT EXISTS valve_event (
    device text        NOT NULL,
    ts     timestamptz NOT NULL,
    state  text        NOT NULL CHECK (state IN ('open', 'closed')),
    reason text,
    detail text,
    PRIMARY KEY (device, ts, state)
);

CREATE TABLE IF NOT EXISTS rule_event (
    device text        NOT NULL,
    ts     timestamptz NOT NULL,
    rule   text        NOT NULL,
    close  boolean     NOT NULL,
    pulses integer,
    detail text,
    PRIMARY KEY (device, ts, rule)
);

CREATE TABLE IF NOT EXISTS alert_event (
    device text        NOT NULL,
    ts     timestamptz NOT NULL,
    detail text,
    PRIMARY KEY (device, ts)
);

CREATE TABLE IF NOT EXISTS device_status (
    device     text PRIMARY KEY,
    status     text        NOT NULL,
    updated_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS setting (
    key   text PRIMARY KEY,
    value text NOT NULL
);
-- Used for history that carries no calibration; 410 measured 2026-09-16 (docs/CALIBRATION.md)
INSERT INTO setting VALUES ('default_pulses_per_liter', '410') ON CONFLICT (key) DO NOTHING;

-- Views: local time is Europe/Warsaw (timestamp without time zone).

CREATE OR REPLACE VIEW flow AS
SELECT
    f.device,
    f.start_ts AT TIME ZONE 'Europe/Warsaw' AS start_local,
    f.stop_ts  AT TIME ZONE 'Europe/Warsaw' AS stop_local,
    extract(epoch FROM f.stop_ts - f.start_ts)::integer AS duration_s,
    round((coalesce(f.pulses_corrected, f.pulses)::numeric / coalesce(f.pulses_per_liter, s.value::integer)), 2)::double precision AS liters,
    CASE WHEN f.stop_ts > f.start_ts
         THEN round((coalesce(f.pulses_corrected, f.pulses)::numeric / coalesce(f.pulses_per_liter, s.value::integer))
                    / (extract(epoch FROM f.stop_ts - f.start_ts) / 60), 1)::double precision
    END AS avg_lpm,
    f.max_lpm,
    f.closed,
    f.suspect,
    f.start_ts,
    f.stop_ts
FROM flow_event f
CROSS JOIN (SELECT value FROM setting WHERE key = 'default_pulses_per_liter') s;

CREATE OR REPLACE VIEW usage_hourly AS
SELECT date_trunc('hour', start_local) AS hour, sum(liters) AS liters, count(*) AS flows,
       count(*) FILTER (WHERE suspect) AS suspect_flows
FROM flow
GROUP BY 1;

CREATE OR REPLACE VIEW usage_daily AS
SELECT start_local::date AS day, sum(liters) AS liters, count(*) AS flows,
       max(duration_s) AS longest_flow_s, count(*) FILTER (WHERE closed) AS shutoffs
FROM flow
GROUP BY 1;

-- Typical week: average liters per hour of week (0 = Sunday), without suspect history
CREATE OR REPLACE VIEW usage_hour_of_week AS
WITH weeks AS (
    SELECT greatest(1, ceil(extract(epoch FROM max(start_ts) - min(start_ts)) / (7 * 86400)))::numeric AS n
    FROM flow_event WHERE NOT suspect
)
SELECT extract(dow FROM start_local)::integer AS dow, extract(hour FROM start_local)::integer AS hour,
       round(sum(liters)::numeric / (SELECT n FROM weeks), 1)::double precision AS avg_liters,
       round(count(*)::numeric / (SELECT n FROM weeks), 2)::double precision AS avg_flows
FROM flow
WHERE NOT suspect
GROUP BY 1, 2;

GRANT SELECT ON ALL TABLES IN SCHEMA public TO wc_read;
GRANT SELECT, INSERT, UPDATE ON flow_event, flow_sample, valve_event, rule_event, alert_event, device_status TO wc_ingest;
GRANT SELECT ON setting TO wc_ingest;
