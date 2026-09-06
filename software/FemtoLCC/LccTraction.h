// Translation between LCC train control and DCC.
//
// OpenLCB gives every train a node ID. For DCC the standard reserves a
// well-known range, 06.01.00.00.xx.xx, whose low 14 bits are the DCC address
// with bit 14 set for long addresses. A throttle drives a train by sending
// addressed Traction Control Commands to that node.
//
// This class watches the bus for those commands and turns them into DCC packets,
// so an LCC throttle ends up driving a DCC locomotive through channel A.
//
// Scope: it decodes set speed/direction, set function and emergency stop, which
// is what a throttle sends moment to moment. It does not yet host the train
// nodes itself - see "Limits" in software/README.md.

#pragma once

#include <stdint.h>
#include <AOLCB.h>
#include "DCCSource.h"

// Well-known OpenLCB node ID prefix for a DCC locomotive.
static const uint64_t LCC_DCC_TRAIN_PREFIX = 0x060100000000ULL;

class LccTraction {
public:
    explicit LccTraction(DCCSource& dcc);

    void setEnabled(bool on) { enabled_ = on; }
    bool isEnabled() const { return enabled_; }

    // Offer every frame the node did not consume. Returns true if this was a
    // traction message and was acted on.
    bool handleFrame(const AOLCB::Message& msg);

    // Highest DCC speed step, used to scale the throttle's metres-per-second
    // into DCC's 0-126.
    void setSpeedScale(float metresPerSecondAtFullSpeed);

    uint8_t knownTrains() const;

private:
    struct AliasEntry {
        uint16_t alias;
        uint64_t nodeId;
        bool used;
    };
    static const uint8_t MAX_ALIASES = 24;

    void noteAlias(uint16_t alias, uint64_t nodeId);
    void forgetAlias(uint16_t alias);
    bool lookupAlias(uint16_t alias, uint64_t& nodeId) const;

    static bool isTrainNode(uint64_t nodeId, uint16_t& dccAddress);
    static float halfToFloat(uint16_t half);

    DCCSource& dcc_;
    bool enabled_;
    float fullSpeedMps_;
    AliasEntry aliases_[MAX_ALIASES];
};
