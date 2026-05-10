#include "alert_local.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

static const char* kPwmSysfsPath = "/sys/class/pwm/pwmchip2/pwm1";

struct ScAlert {
    bool pwm_ready;
};

static bool write_text_file(const char* path, const char* value) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "[alert] open %s: %s\n", path, strerror(errno));
        return false;
    }
    size_t len = strlen(value);
    ssize_t written = write(fd, value, len);
    if (written != (ssize_t)len) {
        fprintf(stderr, "[alert] write %s (%zu bytes): %s\n",
                path, len, written < 0 ? strerror(errno) : "short write");
    }
    close(fd);
    return written == (ssize_t)len;
}

static bool write_pwm_attr(const char* attr, unsigned int value) {
    char path[256];
    char value_buf[32];
    snprintf(path, sizeof(path), "%s/%s", kPwmSysfsPath, attr);
    snprintf(value_buf, sizeof(value_buf), "%u\n", value);
    return write_text_file(path, value_buf);
}

static bool buzzer_enable(unsigned int freq_hz) {
    if (freq_hz == 0) return write_pwm_attr("enable", 0);
    unsigned int period_ns = (unsigned int)(1000000000ULL / freq_hz);
    unsigned int duty_ns = period_ns / 2;
    if (!write_pwm_attr("period", period_ns)) return false;
    if (!write_pwm_attr("duty_cycle", duty_ns)) return false;
    return write_pwm_attr("enable", 1);
}

static void buzzer_stop_internal(void) {
    (void)write_pwm_attr("enable", 0);
}

static void buzzer_play(ScAlert* alert, unsigned int freq_hz, unsigned int duration_ms) {
    if (alert && alert->pwm_ready && buzzer_enable(freq_hz)) {
        usleep(duration_ms * 1000);
        buzzer_stop_internal();
        return;
    }
    usleep(duration_ms * 1000);
}

ScAlert* sc_alert_open(void) {
    ScAlert* a = (ScAlert*)calloc(1, sizeof(ScAlert));
    a->pwm_ready = false;

    /* Export channel 1 so /sys/class/pwm/pwmchip2/pwm1 appears */
    write_text_file("/sys/class/pwm/pwmchip2/export", "1\n");

    a->pwm_ready = access(kPwmSysfsPath, F_OK) == 0;
    if (!a->pwm_ready) {
        fprintf(stderr, "[alert] failed to find PWM buzzer at %s\n", kPwmSysfsPath);
    } else {
        printf("[alert] opened PWM buzzer at %s\n", kPwmSysfsPath);
    }

    return a;
}

void sc_alert_fire(ScAlert* a, int level) {
    if (!a) { printf("[alert] LOCAL ALERT level=%d\n", level); return; }

    switch (level) {
    case 1:
        buzzer_play(a, 2000, 150);
        break;
    case 2:
        for (int i = 0; i < 2; i++) {
            buzzer_play(a, 2200, 200);
            buzzer_play(a, 1700, 200);
            usleep(100000);
        }
        break;
    default:
        for (int i = 0; i < 4; i++) {
            buzzer_play(a, 2500, 300);
            buzzer_play(a, 1500, 300);
        }
    }
}

void sc_alert_stop(ScAlert* a) {
    if (!a) return;
    buzzer_stop_internal();
}

void sc_alert_close(ScAlert* a) {
    if (!a) return;
    sc_alert_stop(a);
    free(a);
}
