#include "Notice.h"
#include <Arduino.h>
#include <stdarg.h>

static bool enabled = true;

void setNoticesEnabled(bool on) {
    enabled = on;
}

bool noticesEnabled() {
    return enabled;
}

void notice(const char* format, ...) {
    if (!enabled) {
        return;
    }
    char text[160];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    Serial.print(text);
}
