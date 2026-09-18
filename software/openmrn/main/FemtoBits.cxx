// The board's hardware as OpenLCB bits. See FemtoBits.hxx.
//
//   Uncommon Models — https://uncommonmodels.com

#include "FemtoBits.hxx"

#include "FemtoController.hxx"

FemtoBit::FemtoBit(FemtoController *controller, BitKind kind, uint8_t index,
    uint64_t on, uint64_t off)
    : openlcb::BitEventInterface(on, off)
    , controller_(controller)
    , kind_(kind)
    , index_(index)
{
}

openlcb::EventState FemtoBit::get_current_state()
{
    return controller_->bit_state(kind_, index_);
}

void FemtoBit::set_state(bool new_value)
{
    controller_->bit_set(kind_, index_, new_value);
}

openlcb::Node *FemtoBit::node()
{
    return controller_->node();
}
