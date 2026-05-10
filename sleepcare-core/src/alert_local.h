#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ScAlert ScAlert;

ScAlert* sc_alert_open(void);
void sc_alert_fire(ScAlert* a, int level);
void sc_alert_stop(ScAlert* a);
void sc_alert_close(ScAlert* a);

#ifdef __cplusplus
}
#endif
