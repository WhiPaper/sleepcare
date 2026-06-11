#include <sys/epoll.h>
#include <unistd.h>

#include <cstdio>
#include <ctime>
#include <memory>


#include "ClockApp.hpp"
#include "DrowsinessBridge.hpp"
#include "lvgl/lvgl.h"

static uint32_t lv_tick_cb(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint32_t>(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int main(void) {
    lv_init();
    lv_tick_set_cb(lv_tick_cb);

    // adafruit-st7735r uses the DRM/KMS "tiny" driver → /dev/dri/card0.
    lv_display_t* disp = lv_linux_drm_create();
    if (!disp) {
        std::fprintf(stderr, "[clock] ERROR: failed to create DRM display\n");
        return 1;
    }

    if (lv_linux_drm_set_file(disp, "/dev/dri/card2", -1) != LV_RESULT_OK) {
        std::fprintf(stderr, "[clock] ERROR: failed to open DRM device\n");
        return 1;
    }

    ClockApp app;
    app.init();

    // Connect to sleepcare-ws via @sleepcare/risk UDS socket.
    RiskBridge risk_bridge;
    if (!risk_bridge.open()) {
        std::fprintf(stderr, "[clock] WARNING: RiskBridge open failed — running without risk data\n");
    }

    int efd = risk_bridge.epoll_fd();

    int last_second = -1;
    uint32_t delay_ms = 5;

    while (true) {
        // Wait for risk data OR LVGL timer expiry.
        if (efd >= 0) {
            struct epoll_event ev{};
            int n = epoll_wait(efd, &ev, 1, static_cast<int>(delay_ms));
            if (n > 0 && (ev.events & EPOLLIN)) {
                // Drain all pending frames (should be at most 1, but be safe).
                while (risk_bridge.recv_nonblock()) {
                    app.apply_risk(risk_bridge);
                }
            }
        } else {
            usleep(delay_ms * 1000u);
        }

        // Clock update (once per second).
        time_t     now    = time(nullptr);
        struct tm* tm_now = localtime(&now);
        if (tm_now && tm_now->tm_sec != last_second) {
            last_second = tm_now->tm_sec;
            app.update(*tm_now);
        }

        delay_ms = lv_timer_handler();
        if (delay_ms == 0 || delay_ms > 5)
            delay_ms = 5;
    }

    return 0;
}