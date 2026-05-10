#include "alert_local.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/ioctl.h>
#include <linux/types.h>

/* PWM chardev ioctl interface — kernel uapi/linux/pwm.h */
struct pwm_export_info { __u32 channel; };
struct pwm_period_info  { __u32 channel; __u64 period; };
struct pwm_duty_info    { __u32 channel; __u64 duty; };

#define PWM_EXPORT     _IOW(0, 2, struct pwm_export_info)
#define PWM_UNEXPORT   _IOW(0, 3, struct pwm_export_info)
#define PWM_PERIOD     _IOW(0, 4, struct pwm_period_info)
#define PWM_DUTY_CYCLE _IOW(0, 5, struct pwm_duty_info)
#define PWM_ENABLE     _IOW(0, 7, unsigned int)

static const char* kPwmChipDev = "/dev/pwmchip2";
static const unsigned int kPwmChannel = 1;

struct ScAlert {
    int fd;
};

static bool buzzer_enable(int fd, unsigned int freq_hz) {
    if (fd < 0) return false;

    unsigned int val = 0;
    ioctl(fd, PWM_ENABLE, &val);

    if (freq_hz == 0) return true;

    struct pwm_period_info per;
    struct pwm_duty_info duty;

    memset(&per, 0, sizeof(per));
    per.channel = kPwmChannel;
    per.period = 1000000000ULL / freq_hz;

    memset(&duty, 0, sizeof(duty));
    duty.channel = kPwmChannel;
    duty.duty = per.period / 2;

    if (ioctl(fd, PWM_PERIOD, &per) < 0) return false;
    if (ioctl(fd, PWM_DUTY_CYCLE, &duty) < 0) return false;
    val = 1;
    return ioctl(fd, PWM_ENABLE, &val) == 0;
}

static void buzzer_stop_internal(int fd) {
    if (fd < 0) return;
    unsigned int val = 0;
    ioctl(fd, PWM_ENABLE, &val);
}

static void buzzer_play(ScAlert* alert, unsigned int freq_hz, unsigned int duration_ms) {
    if (alert && alert->fd >= 0 && buzzer_enable(alert->fd, freq_hz)) {
        usleep(duration_ms * 1000);
        buzzer_stop_internal(alert->fd);
        return;
    }
    usleep(duration_ms * 1000);
}

ScAlert* sc_alert_open(void) {
    ScAlert* a = (ScAlert*)calloc(1, sizeof(ScAlert));
    a->fd = -1;

    int fd = open(kPwmChipDev, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "[alert] failed to open %s: %s\n", kPwmChipDev, strerror(errno));
        return a;
    }

    struct pwm_export_info exp;
    memset(&exp, 0, sizeof(exp));
    exp.channel = kPwmChannel;

    if (ioctl(fd, PWM_EXPORT, &exp) < 0 && errno != EBUSY) {
        fprintf(stderr, "[alert] PWM_EXPORT channel %u failed: %s\n", kPwmChannel, strerror(errno));
        close(fd);
        return a;
    }

    a->fd = fd;
    printf("[alert] opened PWM chardev %s channel %u\n", kPwmChipDev, kPwmChannel);
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
    buzzer_stop_internal(a->fd);
}

void sc_alert_close(ScAlert* a) {
    if (!a) return;
    sc_alert_stop(a);
    if (a->fd >= 0) close(a->fd);
    free(a);
}
