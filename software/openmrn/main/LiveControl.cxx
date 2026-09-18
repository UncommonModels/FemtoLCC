// Live control, memory space 0xE0. See LiveControl.hxx.
//
//   Uncommon Models — https://uncommonmodels.com

#include "LiveControl.hxx"

#include <stdio.h>
#include <string.h>

#include "Arduino.h"
#include "Config.h"
#include "FemtoController.hxx"

LiveControl::LiveControl(Channels &channels, Occupancy &occupancy,
    FemtoController &controller, DCCSource &dcc)
    : channels_(channels)
    , occupancy_(occupancy)
    , controller_(controller)
    , dcc_(dcc)
    , leaseS_(0)
    , lastWrite_(0)
    , locoLive_(false)
{
    memset(set_, 0, sizeof(set_));
    memset(loco_, 0, sizeof(loco_));
}

bool LiveControl::covers(
    uint32_t address, size_t len, uint32_t from, uint32_t count)
{
    return address <= from && address + len >= from + count;
}

size_t LiveControl::read(address_t source, uint8_t *dst, size_t len,
    errorcode_t *error, Notifiable *again)
{
    // Served on the stack's executor; the drivers belong to the hardware thread.
    OSMutexLock l(controller_.hw_lock());

    if (source >= SIZE)
    {
        *error = openlcb::Defs::ERROR_PERMANENT;
        return 0;
    }
    if (source + len > SIZE)
    {
        len = SIZE - source;
    }

    // The space is small, so it is simplest to render the whole of it and then
    // hand back the window that was asked for.
    uint8_t image[SIZE];
    memset(image, 0, sizeof(image));

    image[0] = 1;                   // version
    image[1] = leaseS_;

    for (uint8_t ch = 0; ch < NUM_CHANNELS; ++ch)
    {
        uint8_t *b = image + CHANNELS_AT + 4 * ch;
        const ChannelMode mode = channels_.mode(ch);
        b[0] = mode == ChannelMode::DC ? 1 : mode == ChannelMode::DCC ? 2 : 0;
        b[1] = channels_.reversed(ch) ? 1 : 0;
        b[2] = channels_.duty(ch);
        b[3] = (channels_.faulted(ch) ? 0x01 : 0) |
            (occupancy_.state(ch) == BlockState::Occupied ? 0x02 : 0) |
            (controller_.role(ch) == ChannelRole::Block ? 0x04 : 0) |
            (set_[ch].live ? 0x08 : 0);
    }

    image[SOURCE_AT] = dcc_.isRunning() ? 1 : 0;
    memcpy(image + LOCO_AT, loco_, sizeof(loco_));

    // The locomotives the source is refreshing, as the layout documents them:
    // address high, address low, speed (0xFF emergency stop), flags.
    uint16_t addr;
    int16_t speed;
    bool forward;
    for (uint8_t i = 0; i < 8 && dcc_.locoAt(i, addr, speed, forward); ++i)
    {
        uint8_t *b = image + TABLE_AT + 4 * i;
        b[0] = (uint8_t)(addr >> 8);
        b[1] = (uint8_t)addr;
        b[2] = speed < 0 ? 0xFF : (uint8_t)speed;
        b[3] = forward ? 1 : 0;
    }

    memcpy(dst, image + source, len);
    *error = 0;
    return len;
}

size_t LiveControl::write(address_t destination, const uint8_t *data,
    size_t len, errorcode_t *error, Notifiable *again)
{
    // Served on the stack's executor; the drivers belong to the hardware thread.
    OSMutexLock l(controller_.hw_lock());

    if (destination >= SIZE)
    {
        *error = openlcb::Defs::ERROR_PERMANENT;
        return 0;
    }
    if (destination + len > SIZE)
    {
        len = SIZE - destination;
    }

    // Any write is a sign of life from the dispatcher, so it renews the lease.
    lastWrite_ = millis();

    // Byte i of the write lands at destination + i.
    auto at = [&](uint32_t offset) { return data + (offset - destination); };

    if (covers(destination, len, 1, 1))
    {
        leaseS_ = *at(1);
    }

    // Byte 20 starts and stops the DCC source on channel A.
    if (covers(destination, len, SOURCE_AT, 1))
    {
        const bool on = *at(SOURCE_AT) != 0;
        if (on && !dcc_.isRunning())
        {
            // Channel A stops being this space's to switch off: the source
            // owns it now.
            set_[0].live = false;
            if (dcc_.begin())
            {
                printf("live control: channel A is now a DCC source\n");
            }
            else
            {
                printf("live control: no hardware timer for the DCC source\n");
            }
        }
        else if (!on && dcc_.isRunning())
        {
            dcc_.end();
            locoLive_ = false;
            printf("live control: DCC source stopped\n");
        }
    }

    bool changed = false;
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ++ch)
    {
        const uint32_t base = CHANNELS_AT + 4 * ch;
        if (covers(destination, len, base, 3))
        {
            apply_channel(ch, at(base));
            changed = true;
        }
    }
    if (changed)
    {
        channels_.flush();
    }

    if (covers(destination, len, LOCO_AT, 8))
    {
        apply_loco(at(LOCO_AT));
    }

    *error = 0;
    return len;
}

void LiveControl::apply_channel(uint8_t ch, const uint8_t *b)
{
    // Only a track block may be driven from here: a turnout motor has its own
    // events and its own idea of position, and an unused output stays off.
    // Channel A is also off limits while the DCC source has it.
    if (controller_.role(ch) != ChannelRole::Block ||
        (ch == 0 && dcc_.isRunning()) || b[0] > 2)
    {
        return;
    }

    const ChannelMode mode = b[0] == 1   ? ChannelMode::DC
                           : b[0] == 2   ? ChannelMode::DCC
                                         : ChannelMode::Off;
    const bool reverse = b[1] != 0;
    const uint8_t duty = mode == ChannelMode::DC ? b[2] : 0;

    if (channels_.mode(ch) != mode || channels_.reversed(ch) != reverse ||
        channels_.duty(ch) != duty)
    {
        channels_.set(ch, mode, reverse, duty);
        controller_.channel_changed(ch);
    }
    set_[ch] = Setting{true, mode, reverse, duty};
}

void LiveControl::poll()
{
    if (leaseS_ == 0 || millis() - lastWrite_ < (uint32_t)leaseS_ * 1000)
    {
        return;
    }
    expire();
}

// The dispatcher has gone quiet. Switch off what it left running — unless
// something else has driven an output since, in which case that output is no
// longer this space's to switch off.
void LiveControl::expire()
{
    bool changed = false;
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ++ch)
    {
        Setting &s = set_[ch];
        if (!s.live)
        {
            continue;
        }
        s.live = false;
        if (channels_.mode(ch) == s.mode &&
            channels_.reversed(ch) == s.reverse &&
            channels_.duty(ch) == s.duty && s.mode != ChannelMode::Off)
        {
            channels_.set(ch, ChannelMode::Off, false, 0);
            controller_.channel_changed(ch);
            changed = true;
        }
    }
    if (changed)
    {
        channels_.flush();
    }
    // A dispatcher that has gone quiet must not leave locomotives running.
    if (locoLive_ && dcc_.isRunning())
    {
        dcc_.emergencyStopAll();
    }
    locoLive_ = false;
    if (changed)
    {
        printf("live control: lease ran out, outputs set here switched off\n");
    }
    leaseS_ = 0;
}

// A locomotive command, as one 8-byte block: address (2), speed, direction,
// functions F0-F28 (4). Only what changed is sent, so holding a throttle still
// does not flood the bus with identical packets.
void LiveControl::apply_loco(const uint8_t *b)
{
    const uint16_t addr = (uint16_t)((b[0] << 8) | b[1]) & 0x3FFF;
    if (addr == 0 || addr > 10239)
    {
        return;
    }
    const uint32_t fns = ((uint32_t)b[4] << 24) | ((uint32_t)b[5] << 16) |
        ((uint32_t)b[6] << 8) | b[7];
    const uint16_t lastAddr = (uint16_t)((loco_[0] << 8) | loco_[1]) & 0x3FFF;
    const uint32_t lastFns = ((uint32_t)loco_[4] << 24) |
        ((uint32_t)loco_[5] << 16) | ((uint32_t)loco_[6] << 8) | loco_[7];

    dcc_.setSpeed(addr, b[2] == 0xFF ? -1 : (b[2] > 126 ? 126 : b[2]),
        b[3] != 0);
    for (uint8_t f = 0; f <= 28; ++f)
    {
        const bool on = (fns >> f) & 1;
        if (addr != lastAddr || on != (bool)((lastFns >> f) & 1))
        {
            dcc_.setFunction(addr, f, on);
        }
    }
    memcpy(loco_, b, sizeof(loco_));
    locoLive_ = true;
}
