#include "alert_local.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <dirent.h>

struct ScAlert {
    int buzzer_fd;
};

static void buzzer_set_freq(int fd, unsigned int freq_hz) {
    if (fd >= 0) {
        struct input_event ev = {0};
        ev.type = EV_SND;
        ev.code = SND_TONE;
        ev.value = freq_hz;
        write(fd, &ev, sizeof(ev));
    }
}

static void buzzer_stop_internal(int fd) {
    buzzer_set_freq(fd, 0);
}

static void buzzer_play(int fd, unsigned int freq_hz, unsigned int duration_ms) {
    if (fd >= 0) {
        buzzer_set_freq(fd, freq_hz);
        usleep(duration_ms * 1000);
        buzzer_stop_internal(fd);
    } else {
        // If no hardware, just delay to simulate timing
        usleep(duration_ms * 1000);
    }
}

static int find_buzzer_device() {
    DIR* dir = opendir("/dev/input");
    if (!dir) return -1;
    
    struct dirent* ent;
    int found_fd = -1;
    
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) == 0) {
            char path[256];
            snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
            
            int tmp_fd = open(path, O_RDWR);
            if (tmp_fd >= 0) {
                char name[256] = "Unknown";
                if (ioctl(tmp_fd, EVIOCGNAME(sizeof(name)), name) >= 0) {
                    if (strstr(name, "pwm-beeper") || strstr(name, "buzzer")) {
                        found_fd = tmp_fd;
                        break;
                    }
                }
                close(tmp_fd);
            }
        }
    }
    closedir(dir);
    return found_fd;
}

ScAlert* sc_alert_open(void) {
    ScAlert* a = (ScAlert*)calloc(1, sizeof(ScAlert));
    
    a->buzzer_fd = find_buzzer_device();
    if (a->buzzer_fd < 0) {
        fprintf(stderr, "[alert] failed to find pwm-beeper in /dev/input/\n");
    } else {
        printf("[alert] opened pwm-beeper device fd=%d\n", a->buzzer_fd);
    }

    return a;
}

void sc_alert_fire(ScAlert* a, int level) {
    if (!a) { printf("[alert] LOCAL ALERT level=%d\n", level); return; }

    switch (level) {
    case 1:
        // Level 1: 짧고 경쾌한 단음 (2000Hz)
        buzzer_play(a->buzzer_fd, 2000, 150);
        break;

    case 2:
        // Level 2: 띠-동- (고음 2200Hz -> 저음 1700Hz) 2회 반복
        for (int i = 0; i < 2; i++) {
            buzzer_play(a->buzzer_fd, 2200, 200);
            buzzer_play(a->buzzer_fd, 1700, 200);
            usleep(100000); // 100ms 휴식 간격
        }
        break;

    default: /* level 3 */
        // Level 3: 구급차 사이렌 (고음 2500Hz <-> 저음 1500Hz 교차) 4회 반복
        for (int i = 0; i < 4; i++) {
            buzzer_play(a->buzzer_fd, 2500, 300);
            buzzer_play(a->buzzer_fd, 1500, 300);
        }
    }
}

void sc_alert_stop(ScAlert* a) {
    if (!a) return;
    buzzer_stop_internal(a->buzzer_fd);
}

void sc_alert_close(ScAlert* a) {
    if (!a) return;
    sc_alert_stop(a);
    if (a->buzzer_fd >= 0) close(a->buzzer_fd);
    free(a);
}
