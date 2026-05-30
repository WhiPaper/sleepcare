#include "config.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <type_traits>

template <typename T>
static bool parse_value(const char* raw, T* out);

template <>
bool parse_value<const char*>(const char* raw, const char** out) {
    if (!raw || raw[0] == '\0') return false;
    *out = raw;
    return true;
}

template <>
bool parse_value<int>(const char* raw, int* out) {
    if (!raw || raw[0] == '\0') return false;
    char* end = nullptr;
    errno = 0;
    long parsed = std::strtol(raw, &end, 10);
    if (errno != 0 || end == raw || *end != '\0') return false;
    if (parsed < std::numeric_limits<int>::min() ||
        parsed > std::numeric_limits<int>::max()) {
        return false;
    }
    *out = static_cast<int>(parsed);
    return true;
}

template <>
bool parse_value<float>(const char* raw, float* out) {
    if (!raw || raw[0] == '\0') return false;
    char* end = nullptr;
    errno = 0;
    float parsed = std::strtof(raw, &end);
    if (errno != 0 || end == raw || *end != '\0') return false;
    *out = parsed;
    return true;
}

template <>
bool parse_value<double>(const char* raw, double* out) {
    if (!raw || raw[0] == '\0') return false;
    char* end = nullptr;
    errno = 0;
    double parsed = std::strtod(raw, &end);
    if (errno != 0 || end == raw || *end != '\0') return false;
    *out = parsed;
    return true;
}

template <typename T>
static T cfg_get(const char* env_name, T fallback) {
    const char* raw = std::getenv(env_name);
    T parsed{};
    if (!parse_value<T>(raw, &parsed)) return fallback;
    return parsed;
}

const char* sc_cfg_device_id(void) {
    return cfg_get<const char*>("SC_DEVICE_ID", "deskpi-a1");
}

const char* sc_cfg_display_name(void) {
    return cfg_get<const char*>("SC_DISPLAY_NAME", "SleepCare Pi Desk");
}

int sc_cfg_port(void) {
    return cfg_get<int>("SC_PORT", 8443);
}

const char* sc_cfg_ws_path(void) {
    return cfg_get<const char*>("SC_WS_PATH", "/ws");
}

const char* sc_cfg_cert_path(void) {
    return cfg_get<const char*>("SC_CERT_PATH", "/etc/sleepcare/pi_cert.pem");
}

const char* sc_cfg_key_path(void) {
    return cfg_get<const char*>("SC_KEY_PATH", "/etc/sleepcare/pi_key.pem");
}

const char* sc_cfg_service_type(void) {
    return cfg_get<const char*>("SC_SERVICE_TYPE", "_sleepcare._tcp");
}

const char* sc_cfg_qr_bin_path(void) {
    return cfg_get<const char*>("SC_QR_BIN_PATH", "/run/sleepcare/qr.bin");
}

const char* sc_cfg_qr_dir(void) {
    return cfg_get<const char*>("SC_QR_DIR", "/run/sleepcare");
}

int sc_cfg_risk_interval_ms(void) {
    return cfg_get<int>("SC_RISK_INTERVAL_MS", 1000);
}

float sc_cfg_t1_suspect(void) {
    return cfg_get<float>("SC_T1_SUSPECT", 0.60f);
}

float sc_cfg_t2_alerting(void) {
    return cfg_get<float>("SC_T2_ALERTING", 0.80f);
}

float sc_cfg_h1_hr_supp(void) {
    return cfg_get<float>("SC_H1_HR_SUPP", 0.50f);
}

double sc_cfg_cooldown_sec(void) {
    return cfg_get<double>("SC_COOLDOWN_SEC", 20.0);
}

double sc_cfg_suspect_hold_sec(void) {
    return cfg_get<double>("SC_SUSPECT_HOLD_SEC", 3.0);
}

double sc_cfg_alert_fast_hold_sec(void) {
    return cfg_get<double>("SC_ALERT_FAST_HOLD_SEC", 2.0);
}

double sc_cfg_alert_slow_hold_sec(void) {
    return cfg_get<double>("SC_ALERT_SLOW_HOLD_SEC", 5.0);
}