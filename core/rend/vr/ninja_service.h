// SPDX-License-Identifier: GPL-2.0-or-later
// Native Test return for the measured Ninja Assault World NJA2 Ver. A program.
#pragma once
#include <cstdint>

namespace vr::ninja {
struct ServiceGuard {unsigned offset, word;};
inline constexpr unsigned ServiceMode=0x31afe0, DispatchCallback=0x386304;
inline constexpr unsigned MenuMode=0xffffffff, MenuCallback=0x0c0f7520, GameCallback=0x0c0f7a40;
inline constexpr ServiceGuard ServiceGuards[]={
    // Exact World program revision, independent of the XR projection patch.
    {0x15c8c0,0xd128},{0x15c8c8,0x6312},{0x15c8ca,0x2338},
    {0x15c8d4,0xd323},{0x15c8d6,0x6232},{0x15c8d8,0x2228},
    // Native Test entry stores -1; the Test toggle dispatch reads that mode.
    {0x0f74c0,0xd25d},{0x0f74c2,0xe3ff},{0x0f74c8,0x2232},
    {0x0f7534,0xdd40},{0x0f753a,0x60d2},{0x0f7540,0x88ff},{0x0f7544,0x8964},
    {0x0f7638,0xafe0},{0x0f763a,0x0c31},{0x0f7644,0x7520},{0x0f7646,0x0c0f},
    // Native callback setter and its RAM destination.
    {0x0f72c2,0xd047},{0x0f72cc,0x0246},{0x0f73e0,0x6304},{0x0f73e2,0x0c38},
    // Native main initializer restores mode 1 and the measured main callback.
    {0x0f7a00,0xd233},{0x0f7a02,0xe301},{0x0f7a0c,0x2232},
    {0x0f7a1c,0xd330},{0x0f7a1e,0xd42f},{0x0f7adc,0x7a40},{0x0f7ade,0x0c0f}
};
class ServiceExitObserver {
    bool menuSeen=false;
public:
    void reset() {menuSeen=false;}
    template<class Read16,class Read32> bool poll(Read16 read16,Read32 read32) {
        for(const auto &guard:ServiceGuards)
            if(read16(guard.offset)!=guard.word)return false;
        const unsigned mode=read32(ServiceMode), callback=read32(DispatchCallback);
        // All native submenus retain this Test mode and root callback.
        if(mode==MenuMode && callback==MenuCallback) {
            menuSeen=true;
            return false;
        }
        // Unknown/incomplete snapshots preserve the latch. Report only the
        // measured native main initializer, once per actual menu return.
        if(menuSeen && mode==1 && callback==GameCallback) {
            menuSeen=false;
            return true;
        }
        return false;
    }
};
}