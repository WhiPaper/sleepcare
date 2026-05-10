#include "ws_server.h"
#include "protocol.h"
#include "session.h"
#include "hr_buffer.h"
#include "eye_socket.h"
#include "risk_socket.h"
#include "alert_local.h"
#include "config.h"
#include "sleepcare_proto.h"

#include <libwebsockets.h>
#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Message queue ──────────────────────────────────────────────────── */
#define MSG_QUEUE_MAX  16
#define MSG_MAX_LEN   2048

typedef struct {
    char   data[MSG_QUEUE_MAX][MSG_MAX_LEN];
    int    head, tail, count;
} MsgQueue;

static void mq_push(MsgQueue* q, const char* msg) {
    if (q->count >= MSG_QUEUE_MAX) {
        fprintf(stderr, "[core] message queue full, dropping\n");
        return;
    }
    snprintf(q->data[q->tail], MSG_MAX_LEN, "%s", msg);
    q->tail  = (q->tail + 1) % MSG_QUEUE_MAX;
    q->count++;
}

static bool mq_pop(MsgQueue* q, char* out, size_t out_len) {
    if (q->count == 0) return false;
    snprintf(out, out_len, "%s", q->data[q->head]);
    q->head  = (q->head + 1) % MSG_QUEUE_MAX;
    q->count--;
    return true;
}

/* ── Per-connection context ─────────────────────────────────────────── */
typedef struct {
    SessionCtx session;
    MsgQueue   send_queue;
    int64_t    session_open_ms;
    bool       summary_pending;
} ConnCtx;

/* ── Server context ─────────────────────────────────────────────────── */
struct ScWsServer {
    struct lws_context* lws_ctx;
    int                 eye_fd;
    int                 risk_fd;
    ScAlert*            alert;
    uint32_t            risk_seq;
    uint8_t             qr_ready;
    /* pointer to active connection (NULL if none) */
    struct lws*         active_wsi;
    ConnCtx*            active_conn;
};

static ScWsServer* g_srv = NULL;  /* single global for LWS callback */

/* Canonical session-summary reasons */
static const char* kReasonUserStop           = "user_stop";

static void send_ui_state_frame(uint8_t state, uint8_t alert_level, uint8_t qr_ready,
                                float eye_score, float fused_score, uint16_t hr_bpm) {
    if (!g_srv || g_srv->risk_fd < 0) return;

    RiskFrame rf = {0};
    rf.magic[0]='S'; rf.magic[1]='R'; rf.magic[2]='S'; rf.magic[3]='K';
    rf.version     = 1;
    rf.state       = state;
    rf.alert_level = alert_level;
    rf.qr_ready    = qr_ready;
    rf.eye_score   = eye_score;
    rf.fused_score = fused_score;
    rf.hr_bpm      = hr_bpm;
    rf.seq         = g_srv->risk_seq++;
    rf.ts_ms       = (uint64_t)sc_now_ms();
    sc_risk_send(g_srv->risk_fd, &rf);
}

static void log_pre_session_eye(bool got_eye, const EyeFrame* eye, float eye_score, uint8_t ui_state) {
    static int64_t s_last_log_ms = 0;
    int64_t now_ms = (int64_t)time(NULL) * 1000;
    if ((now_ms - s_last_log_ms) < 1000) return;

    if (got_eye && eye) {
        printf("[core][pre-session] eye_rx status=%u eye_score=%.3f seq=%u ts_ms=%llu ui_state=%u\n",
               (unsigned)eye->status,
               eye_score,
               (unsigned)eye->seq,
               (unsigned long long)eye->ts_ms,
               (unsigned)ui_state);
    } else {
        printf("[core][pre-session] eye_rx pending(last unavailable), ui_state=%u\n",
               (unsigned)ui_state);
    }
    s_last_log_ms = now_ms;
}

/* ── Helpers ────────────────────────────────────────────────────────── */
static void send_msg(struct lws* wsi, ConnCtx* conn, const char* msg) {
    mq_push(&conn->send_queue, msg);
    lws_callback_on_writable(wsi);
}

static void send_envelope(struct lws* wsi, ConnCtx* conn,
                           const char* t, const char* body, bool ack) {
    char* s = sc_proto_build(t, conn->session.sid,
                              &conn->session.seq_out, body, ack);
    if (s) {
        /* compute used seq (sc_proto_build returns old seq and increments) */
        uint32_t used_seq = conn && conn->session.seq_out ? (conn->session.seq_out - 1) : 0;
        printf("[sleepcare-pi] tx json: %s\n", s);
        printf("[sleepcare-pi] tx meta: t=%s sid=%s seq=%u ack_required=%d body=%s\n",
               t,
               (conn && conn->session.sid[0]) ? conn->session.sid : "null",
               used_seq,
               ack ? 1 : 0,
               body ? body : "{}");
        send_msg(wsi, conn, s);
        free(s);
    }
}

static void handle_hello(struct lws* wsi, ConnCtx* conn, const char* body_raw) {
    /* Parse watch_available */
    cJSON* root = cJSON_Parse(body_raw ? body_raw : "{}");
    if (root) {
        cJSON* wa = cJSON_GetObjectItemCaseSensitive(root, "watch_available");
        conn->session.watch_available = cJSON_IsTrue(wa);
        cJSON* eo = cJSON_GetObjectItemCaseSensitive(root, "supports_eye_only");
        conn->session.eye_only_mode = cJSON_IsTrue(eo);
        cJSON_Delete(root);
    }
    conn->session.hello_done = true;

    const char* mode = conn->session.watch_available ? "eye+hr" : "eye-only";
    char body[256];
    snprintf(body, sizeof(body),
             "{\"device_id\":\"%s\",\"mode\":\"%s\",\"proto\":\"v1\"}",
             SC_DEVICE_ID, mode);
    send_envelope(wsi, conn, T_HELLO_ACK, body, false);

    /* HELLO only syncs capabilities; QR is hidden on session.open. */
    send_ui_state_frame((uint8_t)SC_STATE_IDLE, 0, g_srv->qr_ready, 0.0f, 0.0f, 0);
    lws_set_timer_usecs(wsi, SC_RISK_INTERVAL_MS * 1000);
}

static void handle_session_open(struct lws* wsi, ConnCtx* conn, ScEnvelope* env,
                                  const char* body_raw) {
    session_reset(&conn->session);
    snprintf(conn->session.sid, sizeof(conn->session.sid), "%s", env->sid);
    conn->session.session_open = true;
    conn->session_open_ms = (int64_t)time(NULL) * 1000;
    conn->summary_pending = false;

    cJSON* body = cJSON_Parse(body_raw ? body_raw : "{}");
    if (body) {
        cJSON* wa = cJSON_GetObjectItemCaseSensitive(body, "watch_available");
        if (cJSON_IsBool(wa))
            conn->session.watch_available = cJSON_IsTrue(wa);

        cJSON* eo = cJSON_GetObjectItemCaseSensitive(body, "eye_only");
        if (cJSON_IsBool(eo))
            conn->session.eye_only_mode = cJSON_IsTrue(eo);

        cJSON* sm = cJSON_GetObjectItemCaseSensitive(body, "study_mode");
        if (cJSON_IsString(sm) && sm->valuestring)
            snprintf(conn->session.study_mode, sizeof(conn->session.study_mode),
                     "%s", sm->valuestring);

        cJSON_Delete(body);
    }

    const char* mode_str = conn->session.watch_available ? "eye+hr" : "eye-only";
    printf("[core] Session %s mode=%s (watch=%d eye_only=%d)\n",
           env->sid, mode_str,
           conn->session.watch_available, conn->session.eye_only_mode);

    conn->session.state = SS_BASELINE;  /* EARSYS runs camera independently */
    g_srv->active_wsi  = wsi;
    g_srv->active_conn = conn;

    send_ui_state_frame((uint8_t)SC_STATE_BASELINE, 0, g_srv->qr_ready,
                        0.0f, 0.0f, 0);

    /* Check camera readiness: drain any pending eye frames */
    int64_t now_ms = sc_now_ms();
    EyeFrame tmp_eye;
    bool got_eye_now = false;
    while (sc_eye_recv(g_srv->eye_fd, &tmp_eye)) {
        got_eye_now = true;
        conn->session.last_eye = tmp_eye;
        conn->session.last_eye_ms = now_ms;
    }
    bool camera_ready = got_eye_now ||
        (conn->session.last_eye_ms > 0 &&
         (now_ms - conn->session.last_eye_ms) < 5000);

    char ack_body[128];
    snprintf(ack_body, sizeof(ack_body),
             "{\"accepted\":true,\"camera_ready\":%s}",
             camera_ready ? "true" : "false");
    send_envelope(wsi, conn, T_SESSION_ACK, ack_body, false);
    /* Start 1-second risk timer */
    lws_set_timer_usecs(wsi, SC_RISK_INTERVAL_MS * 1000);
    printf("[core] Session %s opened\n", conn->session.sid);
}

static void handle_session_close(struct lws* wsi, ConnCtx* conn, const char* body_raw) {
    /* Capture the current state for final_state before overwriting */
    SessionState prev_state = conn->session.state;
    const char* state_name = session_state_name(prev_state);
    conn->session.state = SS_CLOSING;

    /* Parse reason */
    const char* reason = kReasonUserStop;
    cJSON* root = cJSON_Parse(body_raw ? body_raw : "{}");
    if (root) {
        cJSON* r = cJSON_GetObjectItemCaseSensitive(root, "reason");
        if (cJSON_IsString(r) && r->valuestring)
            reason = r->valuestring;
    }
    snprintf(conn->session.final_reason, sizeof(conn->session.final_reason),
             "%s", reason);

    char body[512];
    snprintf(body, sizeof(body),
             "{\"final_state\":\"%s\","
             "\"total_alerts\":%d,"
             "\"peak_fused_score\":%.3f,"
             "\"mode\":\"%s\","
             "\"summary_reason\":\"%s\"}",
             state_name, conn->session.total_alerts,
             conn->session.peak_fused,
             conn->session.watch_available ? "eye+hr" : "eye-only",
             reason);

    if (root) cJSON_Delete(root);
    send_envelope(wsi, conn, T_SESSION_SUMM, body, false);
    conn->summary_pending = true;

    /* Mark session as closed so timer ticks return to pre-session path. */
    conn->session.session_open = false;
    conn->session.state = SS_IDLE;

    g_srv->active_wsi  = NULL;
    g_srv->active_conn = NULL;
    printf("[core] Session %s closed (%s)\n", conn->session.sid, reason);
}

static bool do_risk_tick(struct lws* wsi, ConnCtx* conn) {
    if (!conn->session.session_open) {
        /*
         * Before mobile session_open, still reflect live eye score on LVGL.
         * This keeps the local UI from being pinned at fused 0.00.
         */
        EyeFrame eye = {0};
        bool got_eye = false;
        while (sc_eye_recv(g_srv->eye_fd, &eye)) got_eye = true;
        if (got_eye) {
            conn->session.last_eye = eye;
        }

        bool no_face = conn->session.last_eye.status == SC_STATUS_NOFACE;
        float eye_score = no_face ? 0.0f : conn->session.last_eye.eye_score;
        uint8_t ui_state = no_face ? (uint8_t)SC_STATE_NO_FACE : (uint8_t)SC_STATE_IDLE;

        log_pre_session_eye(got_eye, got_eye ? &eye : NULL, eye_score, ui_state);
        send_ui_state_frame(ui_state, 0, g_srv->qr_ready, eye_score, eye_score, 0);
        return true;
    }

    /* Read eye frame(s) */
    EyeFrame eye = {0};
    bool got_eye = false;
    while (sc_eye_recv(g_srv->eye_fd, &eye)) got_eye = true;
    if (got_eye) {
        conn->session.last_eye    = eye;
        conn->session.last_eye_ms = (int64_t)time(NULL) * 1000;
    }

    bool no_face = conn->session.last_eye.status == SC_STATUS_NOFACE;
    float eye_score = no_face ? 0.0f : conn->session.last_eye.eye_score;

    /* HR */
    int64_t now_ms = sc_now_ms();
    int32_t bpm    = 0;
    float   hr_w   = 0.0f;
    hr_buffer_latest(&conn->session.hr_buf, now_ms, 10000, &bpm, &hr_w);

    /* hr_score: rough mapping — lower bpm = higher score */
    float hr_score = 0.0f;
    if (bpm > 0 && hr_w > 0.0f) {
        hr_score = (bpm < 55) ? 0.9f :
                   (bpm < 65) ? 0.6f :
                   (bpm < 75) ? 0.3f : 0.1f;
    }

    float fused = (hr_w > 0.0f)
                  ? (0.7f * eye_score + 0.3f * (hr_score * hr_w))
                  : eye_score;

    SessionState prev  = conn->session.state;
    SessionState state = session_tick(&conn->session, fused, hr_w, 1.0 /* 1s */);
    if (state != prev) {
        printf("[core][state] %s -> %s fused=%.3f eye=%.3f hr_w=%.2f\n",
               session_state_name(prev),
               session_state_name(state),
               fused, eye_score, hr_w);
    }

    /* Build risk.update */
    bool alerting_transition = (state == SS_ALERTING && prev != SS_ALERTING);
    const char* state_str = (state == SS_ALERTING)
                                ? session_state_name(state)
                                : (no_face ? "NO_FACE" : session_state_name(state));
    uint8_t risk_state_code = (state == SS_ALERTING)
                                  ? (uint8_t)state
                                  : (no_face ? (uint8_t)SC_STATE_NO_FACE : (uint8_t)state);
    int flush_sec = (state == SS_SUSPECT) ? 5 :
                    (state == SS_ALERTING) ? 2 : 0;

    cJSON* risk_body = cJSON_CreateObject();
    cJSON_AddStringToObject(risk_body, "mode", conn->session.watch_available ? "eye+hr" : "eye-only");
    cJSON_AddNumberToObject(risk_body, "eye_score", (double)eye_score);
    bool has_hr = (bpm > 0 && hr_w > 0.0f);
    if (has_hr)
        cJSON_AddNumberToObject(risk_body, "hr_score", (double)hr_score);
    else
        cJSON_AddNullToObject(risk_body, "hr_score");
    cJSON_AddNumberToObject(risk_body, "fused_score", (double)fused);
    cJSON_AddStringToObject(risk_body, "state", state_str);
    if (flush_sec > 0)
        cJSON_AddNumberToObject(risk_body, "recommended_flush_sec", flush_sec);
    else
        cJSON_AddNullToObject(risk_body, "recommended_flush_sec");

    char* risk_body_str = cJSON_PrintUnformatted(risk_body);
    cJSON_Delete(risk_body);
    if (risk_body_str) {
        send_envelope(wsi, conn, T_RISK_UPDATE, risk_body_str, false);
        free(risk_body_str);
    }

    /* Fire risk frame to clock-gui */
    RiskFrame rf = {0};
    rf.magic[0]='S'; rf.magic[1]='R'; rf.magic[2]='S'; rf.magic[3]='K';
    rf.version     = 1;
    rf.state       = risk_state_code;
    rf.qr_ready    = g_srv->qr_ready;
    rf.eye_score   = eye_score;
    rf.fused_score = fused;
    rf.hr_bpm      = (uint16_t)bpm;
    rf.seq         = g_srv->risk_seq++;
    rf.ts_ms       = (uint64_t)now_ms;
    sc_risk_send(g_srv->risk_fd, &rf);

    /* ALERTING transition — fire alert */
    if (alerting_transition) {
        conn->session.total_alerts++;
        int level = (fused >= 0.90f) ? 3 : (fused >= 0.75f) ? 2 : 1;
        rf.alert_level = (uint8_t)level;
        sc_risk_send(g_srv->risk_fd, &rf);

        char abody[256];
        snprintf(abody, sizeof(abody),
                 "{\"level\":%d,\"reason\":\"eye_closed_persistent\","
                 "\"duration_ms\":5000}", level);
        send_envelope(wsi, conn, T_ALERT_FIRE, abody, true);
        sc_alert_fire(g_srv->alert, level);

        conn->session.state = SS_COOLDOWN;
        conn->session.cooldown_acc_sec = 0.0;
    }

    return true;
}

/* ── LWS callback ───────────────────────────────────────────────────── */
static int sc_ws_callback(struct lws* wsi, enum lws_callback_reasons reason,
                           void* user, void* in, size_t len) {
    ConnCtx* conn = (ConnCtx*)user;

    switch (reason) {
    case LWS_CALLBACK_ESTABLISHED:
        session_init(&conn->session);
        memset(&conn->send_queue, 0, sizeof(conn->send_queue));
        conn->session_open_ms = 0;
        conn->summary_pending = false;
        g_srv->qr_ready = 0;
        send_ui_state_frame((uint8_t)SC_STATE_IDLE, 0, g_srv->qr_ready,
                            0.0f, 0.0f, 0);
        printf("[sleepcare-pi] websocket client connected path=/ws\n");
        break;

    case LWS_CALLBACK_RECEIVE: {
        char buf[4096];
        size_t cplen = (len < sizeof(buf)-1) ? len : sizeof(buf)-1;
        memcpy(buf, in, cplen);
        buf[cplen] = '\0';

        printf("[sleepcare-pi] rx json: %s\n", buf);

        ScEnvelope env = {0};
        if (sc_proto_parse(buf, &env) != 0) break;
        
        printf("[sleepcare-pi] rx t=%s sid=%s seq=%lld\n", env.t, env.sid[0] ? env.sid : "null", (long long)env.seq);

        /* Re-parse body from original buffer */
        cJSON* root = cJSON_Parse(buf);
        const char* body_raw = "{}";
        char body_buf[2048] = "{}";
        if (root) {
            cJSON* body = cJSON_GetObjectItemCaseSensitive(root, "body");
            char* bs = body ? cJSON_PrintUnformatted(body) : NULL;
            if (bs) { snprintf(body_buf, sizeof(body_buf), "%s", bs); free(bs); }
            cJSON_Delete(root);
            body_raw = body_buf;
        }

        if      (strcmp(env.t, T_HELLO)         == 0) handle_hello(wsi, conn, body_raw);
        else if (strcmp(env.t, T_SESSION_OPEN)  == 0) handle_session_open(wsi, conn, &env, body_raw);
        else if (strcmp(env.t, T_HR_INGEST)     == 0) hr_buffer_ingest(&conn->session.hr_buf, body_raw);
        else if (strcmp(env.t, T_SESSION_CLOSE) == 0) handle_session_close(wsi, conn, body_raw);
        break;
    }

    case LWS_CALLBACK_SERVER_WRITEABLE: {
        char msg[MSG_MAX_LEN];
        if (!mq_pop(&conn->send_queue, msg, sizeof(msg))) break;

        size_t msg_len = strlen(msg);
        /* parse minimal metadata from queued JSON for logging */
        cJSON* _root = cJSON_Parse(msg);
        const char* _t = NULL;
        long _seq = -1;
        if (_root) {
            cJSON* _tobj = cJSON_GetObjectItemCaseSensitive(_root, "t");
            if (cJSON_IsString(_tobj) && _tobj->valuestring) _t = _tobj->valuestring;
            cJSON* _sobj = cJSON_GetObjectItemCaseSensitive(_root, "seq");
            if (cJSON_IsNumber(_sobj)) _seq = (long)_sobj->valuedouble;
            cJSON_Delete(_root);
        }

        unsigned char* out = (unsigned char*)malloc(LWS_PRE + msg_len);
        if (!out) break;
        memcpy(out + LWS_PRE, msg, msg_len);
        int w = lws_write(wsi, out + LWS_PRE, msg_len, LWS_WRITE_TEXT);
        printf("[sleepcare-pi] lws_write: t=%s seq=%ld len=%zu ret=%d queued=%d\n",
               _t ? _t : "?",
               _seq,
               msg_len,
               w,
               conn->send_queue.count);
          if (w >= 0 && _t && strcmp(_t, T_SESSION_SUMM) == 0) {
              conn->summary_pending = false;
          }
        free(out);

        if (conn->send_queue.count > 0)
            lws_callback_on_writable(wsi);
        break;
    }

    case LWS_CALLBACK_TIMER:
        if (!do_risk_tick(wsi, conn)) {
            return -1;
        }
        lws_set_timer_usecs(wsi, SC_RISK_INTERVAL_MS * 1000);
        break;

    case LWS_CALLBACK_CLOSED:
        if (!conn->session.hello_done && !conn->session.session_open) {
            printf("[sleepcare-pi] websocket closed reason=spki_probe_done\n");
        } else {
            printf("[sleepcare-pi] websocket closed\n");
        }
        
        if (g_srv->active_wsi == wsi) {
            g_srv->active_wsi  = NULL;
            g_srv->active_conn = NULL;
        }
        g_srv->qr_ready = 1;
        send_ui_state_frame((uint8_t)SC_STATE_IDLE, 0, g_srv->qr_ready,
                            0.0f, 0.0f, 0);
        break;

    default:
        break;
    }
    return 0;
}

/* ── Public API ─────────────────────────────────────────────────────── */
static struct lws_protocols kProtocols[] = {
    { "sleepcare-v1", sc_ws_callback, sizeof(ConnCtx), 4096, 0, NULL, 0 },
    LWS_PROTOCOL_LIST_TERM
};

static const struct lws_http_mount kMount = {
    .mount_next       = NULL,
    .mountpoint       = SC_WS_PATH,
    .origin           = NULL,
    .def              = NULL,
    .protocol         = "sleepcare-v1",
    .cgienv           = NULL,
    .extra_mimetypes  = NULL,
    .interpret        = NULL,
    .cgi_timeout      = 0,
    .cache_max_age    = 0,
    .auth_mask        = 0,
    .cache_intermediaries = 0,
    .origin_protocol  = LWSMPRO_CALLBACK,
    .mountpoint_len   = sizeof(SC_WS_PATH) - 1,
    .basic_auth_login_file = NULL,
};

ScWsServer* sc_ws_create(int port, const char* cert_path, const char* key_path,
                          int eye_fd, int risk_fd) {
    ScWsServer* srv = (ScWsServer*)calloc(1, sizeof(ScWsServer));
    srv->eye_fd  = eye_fd;
    srv->risk_fd = risk_fd;
    srv->alert   = sc_alert_open();
    srv->qr_ready = 1;
    g_srv        = srv;

    struct lws_context_creation_info info = {0};
    info.port      = port;
    info.protocols = kProtocols;
    info.mounts    = &kMount;
    info.ssl_cert_filepath        = cert_path;
    info.ssl_private_key_filepath = key_path;
    info.options   = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.gid       = -1;
    info.uid       = -1;

    srv->lws_ctx = lws_create_context(&info);
    if (!srv->lws_ctx) {
        fprintf(stderr, "[core] lws_create_context failed\n");
        free(srv);
        g_srv = NULL;
        return NULL;
    }
    printf("[core] WSS server started on port %d\n", port);
    return srv;
}

void sc_ws_service(ScWsServer* srv, int timeout_ms) {
    lws_service(srv->lws_ctx, timeout_ms);
}

void sc_ws_destroy(ScWsServer* srv) {
    if (!srv) return;
    lws_context_destroy(srv->lws_ctx);
    sc_alert_close(srv->alert);
    g_srv = NULL;
    free(srv);
}
