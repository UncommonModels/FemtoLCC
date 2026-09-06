#include "LccTraction.h"
#include <Arduino.h>
#include <math.h>

// Traction Control Command, an addressed OpenLCB message.
static const uint16_t MTI_TRACTION_COMMAND = 0x5EB;

// First payload byte selects the operation.
static const uint8_t TRACTION_SET_SPEED    = 0x00;
static const uint8_t TRACTION_SET_FUNCTION = 0x01;
static const uint8_t TRACTION_ESTOP        = 0x02;

LccTraction::LccTraction(DCCSource& dcc)
    : dcc_(dcc), enabled_(false), fullSpeedMps_(40.0f) {
    for (uint8_t i = 0; i < MAX_ALIASES; ++i) {
        aliases_[i].used = false;
        aliases_[i].alias = 0;
        aliases_[i].nodeId = 0;
    }
}

void LccTraction::setSpeedScale(float metresPerSecondAtFullSpeed) {
    if (metresPerSecondAtFullSpeed > 0.1f) {
        fullSpeedMps_ = metresPerSecondAtFullSpeed;
    }
}

// --- alias bookkeeping -----------------------------------------------------

void LccTraction::noteAlias(uint16_t alias, uint64_t nodeId) {
    for (uint8_t i = 0; i < MAX_ALIASES; ++i) {
        if (aliases_[i].used && aliases_[i].alias == alias) {
            aliases_[i].nodeId = nodeId;
            return;
        }
    }
    for (uint8_t i = 0; i < MAX_ALIASES; ++i) {
        if (!aliases_[i].used) {
            aliases_[i].used = true;
            aliases_[i].alias = alias;
            aliases_[i].nodeId = nodeId;
            return;
        }
    }
}

void LccTraction::forgetAlias(uint16_t alias) {
    for (uint8_t i = 0; i < MAX_ALIASES; ++i) {
        if (aliases_[i].used && aliases_[i].alias == alias) {
            aliases_[i].used = false;
            return;
        }
    }
}

bool LccTraction::lookupAlias(uint16_t alias, uint64_t& nodeId) const {
    for (uint8_t i = 0; i < MAX_ALIASES; ++i) {
        if (aliases_[i].used && aliases_[i].alias == alias) {
            nodeId = aliases_[i].nodeId;
            return true;
        }
    }
    return false;
}

uint8_t LccTraction::knownTrains() const {
    uint8_t n = 0;
    uint16_t addr;
    for (uint8_t i = 0; i < MAX_ALIASES; ++i) {
        if (aliases_[i].used && isTrainNode(aliases_[i].nodeId, addr)) {
            ++n;
        }
    }
    return n;
}

bool LccTraction::isTrainNode(uint64_t nodeId, uint16_t& dccAddress) {
    if ((nodeId & 0xFFFFFFFF0000ULL) != LCC_DCC_TRAIN_PREFIX) {
        return false;
    }
    const uint16_t raw = (uint16_t)(nodeId & 0xFFFF);
    // Bit 14 marks a long address; the DCC address is the low 14 bits.
    dccAddress = (uint16_t)(raw & 0x3FFF);
    return dccAddress != 0;
}

// IEEE 754 half precision, which is how the traction standard carries speed.
float LccTraction::halfToFloat(uint16_t half) {
    const uint16_t sign = (half >> 15) & 0x1;
    const int16_t exponent = (int16_t)((half >> 10) & 0x1F);
    const uint16_t mantissa = half & 0x3FF;

    float value;
    if (exponent == 0) {
        value = ldexpf((float)mantissa, -24);            // subnormal
    } else if (exponent == 31) {
        value = mantissa ? NAN : INFINITY;
    } else {
        value = ldexpf((float)(mantissa + 1024), exponent - 25);
    }
    return sign ? -value : value;
}

// --- frame handling --------------------------------------------------------

bool LccTraction::handleFrame(const AOLCB::Message& msg) {
    const uint32_t id = msg.getId();

    // Track alias-to-node-ID mappings so an addressed command can be resolved
    // back to the train it is meant for.
    if (AOLCB::isControlFrame(id)) {
        const uint16_t var = AOLCB::variableField(id);
        if (var == AOLCB::VAR_AMD && msg.getLength() >= 6) {
            noteAlias(AOLCB::sourceAlias(id), AOLCB::bytesToNodeId(msg.getData()));
        } else if (var == AOLCB::VAR_AMR) {
            forgetAlias(AOLCB::sourceAlias(id));
        }
        return false;
    }

    if (!enabled_ || AOLCB::messageMTI(id) != MTI_TRACTION_COMMAND) {
        return false;
    }
    if (msg.getLength() < 3) {
        return false;
    }

    // An addressed message carries the destination alias in the first two
    // payload bytes; the top nibble holds framing flags.
    const uint8_t* p = msg.getData();
    const uint16_t destAlias = (uint16_t)(((p[0] & 0x0F) << 8) | p[1]);

    uint64_t destNode = 0;
    if (!lookupAlias(destAlias, destNode)) {
        return false;                 // not a node we have seen announced
    }
    uint16_t address = 0;
    if (!isTrainNode(destNode, address)) {
        return false;                 // addressed to something that is not a train
    }

    const uint8_t* body = p + 2;
    const size_t bodyLen = msg.getLength() - 2;

    switch (body[0]) {
    case TRACTION_SET_SPEED: {
        if (bodyLen < 3) {
            return false;
        }
        const uint16_t half = (uint16_t)((body[1] << 8) | body[2]);
        const float mps = halfToFloat(half);
        if (isnan(mps)) {
            return false;
        }
        const bool forward = !signbit(mps);
        float magnitude = fabsf(mps);
        if (magnitude > fullSpeedMps_) {
            magnitude = fullSpeedMps_;
        }
        const int16_t step = (int16_t)((magnitude / fullSpeedMps_) * 126.0f + 0.5f);
        dcc_.setSpeed(address, step, forward);
        return true;
    }

    case TRACTION_SET_FUNCTION: {
        // Function address is 24-bit, value is 16-bit; non-zero means on.
        if (bodyLen < 6) {
            return false;
        }
        const uint32_t fn = ((uint32_t)body[1] << 16) | ((uint32_t)body[2] << 8) | body[3];
        const uint16_t value = (uint16_t)((body[4] << 8) | body[5]);
        if (fn <= 28) {
            dcc_.setFunction(address, (uint8_t)fn, value != 0);
        }
        return true;
    }

    case TRACTION_ESTOP:
        dcc_.setSpeed(address, -1, true);
        return true;

    default:
        return false;
    }
}
