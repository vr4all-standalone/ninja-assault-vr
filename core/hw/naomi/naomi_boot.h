// Called with materialized SH4 registers at instruction/block entry, never from I/O helpers.
#pragma once
#ifdef USE_OPENXR
#include "hw/sh4/sh4_mem.h"
#include "rend/vr/service_boot.h"
#include "rend/vr/xr_host.h"
inline void observeNaomiGameBoot(u32 pc, u32 selector)
{
    if ((selector & 0x1fffffff)!=0x0c01f820 || !settings.platform.isNaomi()) return;
    if (vr::xr::nativeMainBoot(pc, selector, ReadMem32_nommu(0x8c01f820), ReadMem32_nommu(0x8c01f824)))
    {
        vr::xr::notifyServiceGameBoot();
        NOTICE_LOG(INPUT, "XR: native NAOMI main-game handoff");
    }
}
#else
inline void observeNaomiGameBoot(u32, u32) {}
#endif
