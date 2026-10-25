// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string_view>

namespace vr {
// Evaluate loaded media metadata, never a path or emulated RAM. Ordinary Flycast and
// regression builds keep their normal game selection when the option is absent/OFF.
inline const char *standaloneGameError(std::string_view gameId)
{
#if defined(VR_STANDALONE_NINJA) && VR_STANDALONE_NINJA
    if (gameId != "NINJA ASSAULT")
        return "Ninja Assault VR only supports Ninja Assault (World NJA2 Ver.A).";
#elif defined(VR_STANDALONE_LUPIN) && VR_STANDALONE_LUPIN
    if (gameId != "LUPIN THE THIRD  -THE SHOOTING-")
        return "Lupin the Third VR only supports Lupin the Third: The Shooting (original GDS-0018).";
#endif
    return nullptr;
}
}
