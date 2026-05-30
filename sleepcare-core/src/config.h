/**
 * @file config.h
 * @brief Compile-time configuration for sleepcare-ws.
 *        Runtime overrides via environment variables where noted.
 */
#pragma once

/* ── Runtime config getters ── */
#ifdef __cplusplus
extern "C" {
#endif

const char* sc_cfg_device_id(void);
const char* sc_cfg_display_name(void);
int sc_cfg_port(void);
const char* sc_cfg_ws_path(void);
const char* sc_cfg_cert_path(void);
const char* sc_cfg_key_path(void);
const char* sc_cfg_service_type(void);
const char* sc_cfg_qr_bin_path(void);
const char* sc_cfg_qr_dir(void);
int sc_cfg_risk_interval_ms(void);
float sc_cfg_t1_suspect(void);
float sc_cfg_t2_alerting(void);
float sc_cfg_h1_hr_supp(void);
double sc_cfg_cooldown_sec(void);
double sc_cfg_suspect_hold_sec(void);
double sc_cfg_alert_fast_hold_sec(void);
double sc_cfg_alert_slow_hold_sec(void);

#ifdef __cplusplus
}
#endif

/* ── Identity ── */
#define SC_DEVICE_ID        (sc_cfg_device_id())
#define SC_DISPLAY_NAME     (sc_cfg_display_name())

/* ── Network ── */
#define SC_PORT             (sc_cfg_port())
#define SC_WS_PATH          (sc_cfg_ws_path())

/* ── TLS certificates ── */
#define SC_CERT_PATH        (sc_cfg_cert_path())
#define SC_KEY_PATH         (sc_cfg_key_path())

/* ── mDNS ── */
#define SC_SERVICE_TYPE     (sc_cfg_service_type())

/* ── IPC / files ── */
#define SC_QR_BIN_PATH      (sc_cfg_qr_bin_path())
#define SC_QR_DIR           (sc_cfg_qr_dir())

/* ── Session timing ── */
#define SC_RISK_INTERVAL_MS (sc_cfg_risk_interval_ms())   /* risk.update send period */

/* ── Risk thresholds ── */
#define SC_T1_SUSPECT       (sc_cfg_t1_suspect())   /* BASELINE → SUSPECT */
#define SC_T2_ALERTING      (sc_cfg_t2_alerting())  /* SUSPECT  → ALERTING (fast) */
#define SC_H1_HR_SUPP       (sc_cfg_h1_hr_supp())   /* HR support weight threshold */
#define SC_COOLDOWN_SEC     (sc_cfg_cooldown_sec())  /* ALERTING → COOLDOWN → BASELINE */

/* ── State machine timings (seconds) ── */
#define SC_SUSPECT_HOLD_SEC    (sc_cfg_suspect_hold_sec())  /* eye>=T1 持續 → SUSPECT */
#define SC_ALERT_FAST_HOLD_SEC (sc_cfg_alert_fast_hold_sec())  /* eye>=T2 持続 → ALERTING */
#define SC_ALERT_SLOW_HOLD_SEC (sc_cfg_alert_slow_hold_sec())  /* eye>=T1 + hr_support → ALERTING */
