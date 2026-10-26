#include "lockstep.h"

namespace bigred {

uint8_t pack_input(const VehicleInput& in) {
    return static_cast<uint8_t>((in.accel ? 0x01 : 0) | (in.brake ? 0x02 : 0) |
                                (in.left ? 0x04 : 0) | (in.right ? 0x08 : 0) |
                                (in.nitro ? 0x10 : 0) | (in.recover ? 0x20 : 0));
}

VehicleInput unpack_input(uint8_t bits) {
    VehicleInput in;
    in.accel = (bits & 0x01) != 0;
    in.brake = (bits & 0x02) != 0;
    in.left = (bits & 0x04) != 0;
    in.right = (bits & 0x08) != 0;
    in.nitro = (bits & 0x10) != 0;
    in.recover = (bits & 0x20) != 0;
    return in;
}

void LockstepInputs::configure(int players, int delay_ticks) {
    players_ = players > 0 ? players : 1;
    const int max_delay = static_cast<int>(kWindow) - 1;
    delay_ = delay_ticks < 0 ? 0 : (delay_ticks > max_delay ? max_delay : delay_ticks);
    base_ = 1;
    slots_.assign(static_cast<size_t>(kWindow) * static_cast<size_t>(players_), 0);
    slot_tick_.assign(kWindow, 0);
    for (uint32_t t = 1; t <= static_cast<uint32_t>(delay_); ++t)
        for (int p = 0; p < players_; ++p) submit(p, t, 0);
}

bool LockstepInputs::submit(int player, uint32_t tick, uint8_t bits) {
    if (player < 0 || player >= players_) return false;
    if (tick < base_ || tick >= base_ + kWindow) return false;
    const size_t row = tick % kWindow;
    if (slot_tick_[row] != tick) {
        // A row freed by release_before, or never used: claim it for this tick.
        slot_tick_[row] = tick;
        for (int p = 0; p < players_; ++p) slots_[slot(tick, p)] = 0;
    }
    uint16_t& s = slots_[slot(tick, player)];
    if (s == 0) s = static_cast<uint16_t>(0x100 | (bits & 0x3F));
    return true;
}

bool LockstepInputs::ready(uint32_t tick) const {
    if (tick < base_ || tick >= base_ + kWindow) return false;
    if (slot_tick_[tick % kWindow] != tick) return false;
    for (int p = 0; p < players_; ++p)
        if (slots_[slot(tick, p)] == 0) return false;
    return true;
}

int LockstepInputs::missing(uint32_t tick) const {
    if (tick < base_ || tick >= base_ + kWindow || slot_tick_[tick % kWindow] != tick)
        return players_ > 0 ? 0 : -1;
    for (int p = 0; p < players_; ++p)
        if (slots_[slot(tick, p)] == 0) return p;
    return -1;
}

void LockstepInputs::inputs_for(uint32_t tick, VehicleInput* out) const {
    for (int p = 0; p < players_; ++p)
        out[p] = unpack_input(static_cast<uint8_t>(slots_[slot(tick, p)] & 0xFF));
}

LockstepGate lockstep_gate(const LockstepInputs& lk, uint32_t next, double banked_ticks) {
    if (!lk.ready(next)) return LockstepGate::Wait;
    if (!lk.ready(next + 1) && banked_ticks < 2.0) return LockstepGate::Hold;
    return LockstepGate::Run;
}

void LockstepInputs::release_before(uint32_t tick) {
    while (base_ < tick) {
        const size_t row = base_ % kWindow;
        if (slot_tick_[row] == base_) slot_tick_[row] = 0;
        ++base_;
    }
}

} // namespace bigred
