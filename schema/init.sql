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
