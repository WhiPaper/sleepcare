#include "qr_writer.h"
#include "config.h"
#include <qrencode.h>
#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <cstdint>

/* qr.bin format:
 *   [4B] magic "SCQR"
 *   [2B] width  uint16_t LE (== height for QR codes)
 *   [2B] height uint16_t LE
 *   [W*H bytes] module data: 1=black, 0=white
 */

bool sc_qr_write(const char* spki_sha256, int port, const char* out_path) {
    (void)port;
    /* Build JSON payload */
    int64_t now_ms = (int64_t)time(NULL) * 1000;
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "proto",        "sleepcare-pair-v1");
    cJSON_AddStringToObject(root, "device_id",    SC_DEVICE_ID);
    cJSON_AddStringToObject(root, "display_name", SC_DISPLAY_NAME);
    cJSON_AddStringToObject(root, "service",      SC_SERVICE_TYPE);
    cJSON_AddStringToObject(root, "ws",           SC_WS_PATH);
    cJSON_AddNumberToObject(root, "tls",          1);
    cJSON_AddStringToObject(root, "spki_sha256",  spki_sha256 ? spki_sha256 : "");

    /* pin_hint: short display-friendly fingerprint — first 10 + "..." + last 6 */
    char pin_hint[32] = "";
    if (spki_sha256) {
        size_t len = strlen(spki_sha256);
        if (len > 16) {
            snprintf(pin_hint, sizeof(pin_hint), "%.*s...%.*s",
                     10, spki_sha256, 6, spki_sha256 + len - 6);
        } else {
            snprintf(pin_hint, sizeof(pin_hint), "%s", spki_sha256);
        }
    }
    cJSON_AddStringToObject(root, "pin_hint", pin_hint);

    cJSON_AddNumberToObject(root, "issued_at_ms", (double)now_ms);

    char* json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return false;

    /* Generate QR - version 3 (29x29) to reduce complexity */
    QRcode* qr = QRcode_encodeString(json_str, 3, QR_ECLEVEL_L, QR_MODE_8, 1);
    free(json_str);
    if (!qr) { fprintf(stderr, "[qr] QRcode_encodeString failed\n"); return false; }

    /* Ensure output directory exists */
    mkdir(SC_QR_DIR, 0755);

    FILE* f = fopen(out_path, "wb");
    if (!f) { perror(out_path); QRcode_free(qr); return false; }

    /* Write header */
    const char magic[4] = {'S','C','Q','R'};
    uint16_t w = (uint16_t)qr->width;
    uint16_t h = (uint16_t)qr->width;  /* QR is always square */
    fwrite(magic, 1, 4, f);
    fwrite(&w, 2, 1, f);
    fwrite(&h, 2, 1, f);

    /* Write module data (libqrencode: bit 0 of each byte = black) */
    size_t total = (size_t)qr->width * qr->width;
    for (size_t i = 0; i < total; i++) {
        uint8_t m = (qr->data[i] & 1) ? 1 : 0;
        fwrite(&m, 1, 1, f);
    }

    fclose(f);
    QRcode_free(qr);
    printf("[qr] Written %dx%d QR to %s\n", w, h, out_path);
    return true;
}
