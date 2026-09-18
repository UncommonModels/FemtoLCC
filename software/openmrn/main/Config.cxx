// The board helper functions the hardware drivers call, and notice().
//
// These are the small pieces of glue the drivers expect to find alongside
// Config.h: the pin lookups, and the sink for status messages. They are
// implemented over ESP-IDF, which is why they live here rather than in the
// drivers themselves.
//
//   Uncommon Models — https://uncommonmodels.com

#include "Config.h"
#include "Notice.h"

#include <stdarg.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Which addresses a board may sit at
// ---------------------------------------------------------------------------

bool ioBoardAddressValid(IoBoardType type, uint8_t address)
{
    const bool low = address >= 0x21 && address <= 0x27;     // 0x20 is U3
    const bool high = address >= 0x38 && address <= 0x3F;    // the A variants
    switch (type)
    {
        case IoBoardType::MCP23017: return low;
        case IoBoardType::PCF8574:
        case IoBoardType::TCA9534:  return low || high;
        default:                    return false;
    }
}

bool servoBoardAddressValid(uint8_t address)
{
    return address >= 0x40 && address <= 0x7F;
}

uint8_t ioBoardLines(IoBoardType type)
{
    switch (type)
    {
        case IoBoardType::MCP23017: return 16;
        case IoBoardType::PCF8574:
        case IoBoardType::TCA9534:  return 8;
        default:                    return 0;
    }
}

const char *ioBoardName(IoBoardType type)
{
    switch (type)
    {
        case IoBoardType::MCP23017: return "MCP23017";
        case IoBoardType::PCF8574:  return "PCF8574";
        case IoBoardType::TCA9534:  return "TCA9534";
        default:                    return "none";
    }
}

// ---------------------------------------------------------------------------
// Console messages
// ---------------------------------------------------------------------------

static bool noticesOn = true;

void setNoticesEnabled(bool on)
{
    noticesOn = on;
}

bool noticesEnabled()
{
    return noticesOn;
}

void notice(const char *format, ...)
{
    if (!noticesOn)
    {
        return;
    }
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}
