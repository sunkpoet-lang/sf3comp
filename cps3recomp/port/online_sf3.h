// SPDX-License-Identifier: GPL-3.0-only
// (ForkAde) What ForkAde's online session (online.cpp, copied from the CPS fork) takes from the CPS port, in sf3comp's
// terms: the CPS names the copy uses (cps2recomp::), the game's online switch, the round results (none yet: sf3comp's
// fight module does not say them, so the match flow counts no rounds) and the hello's identity (the sets' manifest
// and the commit the program is built from, as sf3comp's own netplay checks them).
#pragma once

#include "../runtime/game.h"
#include "game_info.h" // (GAME_ID, GAME_BIOS and the SIMMs: the generated game.cfg values)

#include <zlib.h>

#include <cstring>
#include <string>

namespace cps3recomp {

namespace battle {
    // (no round results: -1, as a game that is not a fighting game says it)
    inline int RoundResultNow() { return -1; }
    inline int RoundsToWin() { return 2; }
} // namespace battle

// The features online play turns off on both sides, as the CPS port's OnlineOff: run-ahead, the custom colours (each
// player's own library), rapid fire and the cheats and the sound (the custom soundtrack: each player's own)
inline bool OnlineOff(const std::string &name) {
    if (name == "run_ahead" || name == "rapid_fire" || name == "custom_colours") {
        return true;
    }
    for (const Feature &feature : TheGame().features) {
        if (name == feature.name) {
            return !strcmp(feature.category, "Cheats") || !strcmp(feature.category, "Sound");
        }
    }
    return false;
}

// The CPS port's TheGame().online: those features off for this run (settings.ini left as it is), and back as the
// player has them when the session ends
inline void SetOnline(bool on) {
    for (const Feature &feature : TheGame().features) {
        if (OnlineOff(feature.name)) {
            SetFeatureForNow(feature.name, on ? 0 : FeatureSetting(feature.name));
        }
    }
}

// (the CPS port's TheGame().onlineMatchResult: a game's own match result; none here)
using MatchResult = int (*)();
inline MatchResult OnlineMatchResult() { return nullptr; }

// The hello's checksum: the game as this build is made (the sets its code was translated from)
inline uint32_t OnlineBuildCrc() {
    uLong h = crc32(0, nullptr, 0);
    for (const char *text : {GAME_BIOS, GAME_SIMM1, GAME_SIMM2}) {
        h = crc32(h, reinterpret_cast<const Bytef *>(text), static_cast<uInt>(strlen(text)));
    }
    return static_cast<uint32_t>(h);
}

// The hello's version: VERSION and the commit the program is built from (FORKADE_BUILD), so that another build is
// refused as another version, each side's said
inline std::string OnlineVersion() {
#ifdef FORKADE_BUILD
    return std::string(RECOMP_VERSION) + "+" + FORKADE_BUILD;
#else
    return RECOMP_VERSION;
#endif
}

} // namespace cps3recomp

namespace cps2recomp = cps3recomp;
