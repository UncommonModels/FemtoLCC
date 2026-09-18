// Console messages nobody asked for.
//
// The hardware drivers (IoBoards, ServoBoards) report boards appearing and
// disappearing through notice(). These go to the console, which on this board
// is UART0 on the J1 header; the USB port carries GridConnect and nothing else,
// so a status message can never land in the middle of the frame stream.
//
// setNoticesEnabled() is here for a caller that needs the console quiet.
//
//   Uncommon Models — https://uncommonmodels.com

#pragma once

#include <stdbool.h>

void setNoticesEnabled(bool on);
bool noticesEnabled();

/// printf to the console, unless notices are held back.
void notice(const char *format, ...) __attribute__((format(printf, 1, 2)));
