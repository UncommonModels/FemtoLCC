// Console messages nobody asked for.
//
// Occupancy changes, events acted on, driver faults and network status are
// printed as they happen. When the USB port is carrying GridConnect to a
// computer those would land in the middle of the frame stream, so they are
// held back while it is; replies to commands typed on the console still print.

#pragma once

#include <stdbool.h>

void setNoticesEnabled(bool on);
bool noticesEnabled();

// printf to the console, unless notices are held back.
void notice(const char* format, ...) __attribute__((format(printf, 1, 2)));
