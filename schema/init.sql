-- Shared SDR signal database schema
-- Used by: AcquisitionApp (raw detections), AnalysisApp (classifications), signal_logger
-- Run once:  psql -U sdr -d sdr_scanner -f schema/init.sql
-- Idempotent — safe to re-run.

CREATE TABLE IF NOT EXISTS signals (
    id              BIGSERIAL           PRIMARY KEY,

    -- Timestamps
    first_seen      TIMESTAMPTZ         NOT NULL DEFAULT now(),
    last_seen       TIMESTAMPTZ         NOT NULL DEFAULT now(),

    -- RF parameters (set by AcquisitionApp on first detection)
    freq_hz         DOUBLE PRECISION    NOT NULL,
    freq_mhz        DOUBLE PRECISION    NOT NULL,
    bandwidth_hz    DOUBLE PRECISION,
    power_db        REAL,
    snr_db          REAL,
    scanner_id      TEXT                NOT NULL DEFAULT '',
    channel         SMALLINT            NOT NULL DEFAULT 0,

    -- Modulation (filled in by AnalysisApp; empty until classified)
    -- Signal identity: (freq ±10 kHz, modulation, bandwidth ±50%)
    -- Same freq + different modulation → separate row (different signal type)
    modulation      TEXT                NOT NULL DEFAULT '',
    mod_class       TEXT                NOT NULL DEFAULT '',   -- 'analog','digital','unclassified'
    is_ofdm         BOOLEAN             NOT NULL DEFAULT false,
    is_burst        BOOLEAN             NOT NULL DEFAULT false,
    is_fhss         BOOLEAN             NOT NULL DEFAULT false,
    symbol_rate_sps DOUBLE PRECISION,
    bit_rate_bps    DOUBLE PRECISION,

    -- Top protocol hypothesis
    hypothesis      TEXT                NOT NULL DEFAULT '',   -- e.g. 'FM Broadcast', 'GSM'
    hyp_category    TEXT                NOT NULL DEFAULT '',   -- e.g. 'Broadcast', 'Cellular'
    hyp_confidence  REAL                DEFAULT 0,

    -- Classification metadata
    classified      BOOLEAN             NOT NULL DEFAULT false,
    rule_confidence REAL                DEFAULT 0,
    onnx_used       BOOLEAN             NOT NULL DEFAULT false,
    onnx_confidence REAL                DEFAULT 0,
    fast_path       BOOLEAN             NOT NULL DEFAULT false,
    reject_reason   TEXT                NOT NULL DEFAULT '',

    hits            INTEGER             NOT NULL DEFAULT 1
);

CREATE INDEX IF NOT EXISTS idx_signals_freq     ON signals (freq_hz);
CREATE INDEX IF NOT EXISTS idx_signals_time     ON signals (last_seen DESC);
CREATE INDEX IF NOT EXISTS idx_signals_mod      ON signals (modulation);
CREATE INDEX IF NOT EXISTS idx_signals_hyp      ON signals (hypothesis);
CREATE INDEX IF NOT EXISTS idx_signals_unclass  ON signals (classified, last_seen DESC)
    WHERE classified = false;

-- ── P25 trunked system channel catalog ───────────────────────────────────────
-- Populated by AcquisitionApp P25GrantConsumer as grants are observed.
-- The control channel row is inserted by DemodApp P25Monitor on startup.

CREATE TABLE IF NOT EXISTS p25_channels (
    id              BIGSERIAL           PRIMARY KEY,
    first_seen      TIMESTAMPTZ         NOT NULL DEFAULT now(),
    last_seen       TIMESTAMPTZ         NOT NULL DEFAULT now(),

    -- Site identity (from RFSS_STATUS_BCAST TSBK)
    wacn            INTEGER             NOT NULL DEFAULT 0,    -- Wideband Area Comm Network ID
    sys_id          INTEGER             NOT NULL DEFAULT 0,    -- System ID
    rfss_id         SMALLINT            NOT NULL DEFAULT 0,
    site_id         SMALLINT            NOT NULL DEFAULT 0,

    -- Channel info
    freq_hz         DOUBLE PRECISION    NOT NULL,
    freq_mhz        DOUBLE PRECISION    GENERATED ALWAYS AS (round((freq_hz/1e6)::numeric,4)) STORED,
    channel_iden    SMALLINT,           -- Channel identifier from IDEN_UP
    channel_num     INTEGER,            -- Channel number within band

    -- Channel role
    is_control      BOOLEAN             NOT NULL DEFAULT false, -- TRUE for the control channel
    talk_group      INTEGER,            -- NULL for control channel; TG ID for voice channels
    source_id       INTEGER,            -- Originating unit ID (voice channels)
    encrypted       BOOLEAN             NOT NULL DEFAULT false,
    emergency       BOOLEAN             NOT NULL DEFAULT false,

    -- Encryption info — populated from voice channel HDU
    alg_id          SMALLINT,           -- ALGID byte: 0x00=Clear 0x04=AES-256 0x41=DVP 0x03=3TDEA
    alg_name        TEXT,               -- Human-readable: Clear / AES-256 / DVP (DES-OFB) / etc.
    key_id          INTEGER,            -- Key ID from HDU (identifies key slot in KMF)

    -- Grant count (how many times we've seen traffic on this channel)
    grant_count     INTEGER             NOT NULL DEFAULT 1,

    UNIQUE (freq_hz, talk_group)
);

CREATE INDEX IF NOT EXISTS idx_p25_site   ON p25_channels (wacn, sys_id, rfss_id, site_id);
CREATE INDEX IF NOT EXISTS idx_p25_freq   ON p25_channels (freq_hz);
CREATE INDEX IF NOT EXISTS idx_p25_tg     ON p25_channels (talk_group);
CREATE INDEX IF NOT EXISTS idx_p25_ctrl   ON p25_channels (is_control) WHERE is_control = true;

-- Convenience view: all P25 channels for the most recently active site
CREATE OR REPLACE VIEW p25_site_channels AS
SELECT
    CASE WHEN is_control THEN 'CONTROL' ELSE 'VOICE' END  AS role,
    freq_mhz,
    talk_group,
    encrypted,
    emergency,
    alg_name,
    grant_count,
    to_char(last_seen, 'HH24:MI:SS')                      AS last_seen,
    lpad(to_hex(wacn),  5, '0')                            AS wacn,
    lpad(to_hex(sys_id),3, '0')                            AS sys_id
FROM p25_channels
WHERE last_seen > now() - interval '1 hour'
ORDER BY is_control DESC, grant_count DESC;

-- ── Views ─────────────────────────────────────────────────────────────────────
-- Drop old views from previous schema versions before recreating.
DROP VIEW IF EXISTS recent_detections;
DROP VIEW IF EXISTS recent_classifications;
DROP VIEW IF EXISTS freq_activity;
DROP VIEW IF EXISTS freq_classification_summary;
DROP VIEW IF EXISTS classification_path_stats;
DROP VIEW IF EXISTS recent_signals;

-- All signals active in the last 60 seconds
CREATE OR REPLACE VIEW recent_signals AS
SELECT
    last_seen,
    round((freq_hz  / 1e6)::numeric, 3)      AS freq_mhz,
    round((bandwidth_hz / 1e3)::numeric, 1)  AS bw_khz,
    round(power_db::numeric, 1)              AS power_db,
    round(snr_db::numeric,   1)              AS snr_db,
    CASE WHEN modulation != '' THEN modulation ELSE '(unclassified)' END AS modulation,
    hypothesis,
    round(hyp_confidence::numeric, 2)        AS hyp_conf,
    hits,
    scanner_id
FROM signals
WHERE last_seen > now() - interval '60 seconds'
ORDER BY last_seen DESC;

-- Per-frequency signal catalog
CREATE OR REPLACE VIEW freq_activity AS
SELECT
    round((freq_hz / 1e6)::numeric, 3)       AS freq_mhz,
    CASE WHEN modulation != '' THEN modulation ELSE '(unclassified)' END AS modulation,
    round((bandwidth_hz / 1e3)::numeric, 1)  AS bw_khz,
    round(power_db::numeric, 1)              AS power_db,
    round(snr_db::numeric, 1)                AS snr_db,
    hypothesis,
    hits,
    first_seen,
    last_seen
FROM signals
ORDER BY last_seen DESC;

-- Recent classified signals
CREATE OR REPLACE VIEW recent_classifications AS
SELECT
    last_seen,
    round((freq_hz / 1e6)::numeric, 3)        AS freq_mhz,
    round((bandwidth_hz / 1e3)::numeric, 1)   AS bw_khz,
    round(snr_db::numeric, 1)                 AS snr_db,
    modulation,
    mod_class,
    hypothesis,
    round(hyp_confidence::numeric, 2)         AS hyp_conf,
    round(rule_confidence::numeric, 2)        AS rule_conf,
    onnx_used,
    fast_path,
    hits,
    scanner_id
FROM signals
WHERE classified = true
  AND last_seen > now() - interval '60 seconds'
ORDER BY last_seen DESC;

-- ── RF Alerts (jamming / spoofing / anomaly detection) ──────────────────────
CREATE TABLE IF NOT EXISTS rf_alerts (
    id           BIGSERIAL PRIMARY KEY,
    detected_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    alert_type   TEXT NOT NULL,        -- GPS_JAMMING, ADSB_SPOOFING, etc.
    severity     TEXT NOT NULL,        -- LOW / MEDIUM / HIGH / CRITICAL
    freq_mhz     DOUBLE PRECISION,     -- centre frequency of threat (null if N/A)
    power_db     REAL,                 -- measured power
    baseline_db  REAL,                 -- expected baseline (for delta calc)
    scanner_id   TEXT,
    details      TEXT NOT NULL         -- human-readable description
);

CREATE INDEX IF NOT EXISTS rf_alerts_detected_at_idx ON rf_alerts (detected_at DESC);
CREATE INDEX IF NOT EXISTS rf_alerts_type_idx        ON rf_alerts (alert_type);

CREATE OR REPLACE VIEW recent_alerts AS
SELECT
    to_char(detected_at, 'YYYY-MM-DD HH24:MI:SS') AS time,
    alert_type,
    severity,
    round(freq_mhz::numeric, 4)                    AS freq_mhz,
    round(power_db::numeric, 1)                    AS power_db,
    round((power_db - baseline_db)::numeric, 1)    AS delta_db,
    details
FROM rf_alerts
WHERE detected_at > now() - INTERVAL '1 hour'
ORDER BY detected_at DESC;
