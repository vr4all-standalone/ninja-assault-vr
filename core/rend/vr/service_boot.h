// Standard NAOMI BIOS main/test handoff: R1 addresses the selected boot field.
#pragma once
#include <cstdint>
namespace vr::xr {
inline bool nativeMainBoot(uint32_t pc, uint32_t selector, uint32_t gamePC, uint32_t testPC) {
    constexpr uint32_t mask=0x1fffffff;
    const auto game=gamePC & mask, test=testPC & mask;
    const auto ramEntry=[](uint32_t value) { return value>=0x0c020000 && value<0x0d000000 && !(value&1); };
    return (selector & mask)==0x0c01f820 && ramEntry(game) && ramEntry(test) && (pc & mask)==game;
}
}
