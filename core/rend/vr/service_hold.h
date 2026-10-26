// Deliberate cabinet service access and loading-time Start release gates.
#pragma once
#include <algorithm>
#include <cstdint>
namespace vr::xr {
struct StartReleaseGate {
    bool waiting[2]{true, true};
    void reset() { waiting[0] = waiting[1] = true; }
    bool sample(int hand, bool down, bool ready) {
        if (!ready) waiting[hand] = true;
        else if (!down) waiting[hand] = false;
        return ready && down && !waiting[hand];
    }
};
struct ServiceHold {
    static constexpr int64_t Duration = 3'000'000'000LL;
    int64_t began=0, pulseUntil=0, tapUntil=0, now=0;
    bool held=false, fired=false, needsRelease=false, tapEligible=false;
    int owner=0, tapOwner=-1;
    void reset() { began=pulseUntil=tapUntil=now=0; held=fired=tapEligible=false; needsRelease=true; tapOwner=-1; }
    bool update(int64_t time, bool down, bool allowed, bool ready=true, int player=0) {
        now=time;
        if (!allowed) { reset(); return false; }
        if (needsRelease) { if (!down) needsRelease=false; return false; }
        if (down && !held) { began=time; fired=false; tapEligible=ready; owner=player; }
        if (!ready) { tapEligible=false; tapUntil=0; }
        if ((down || held) && !fired && time-began>=Duration) { fired=true; pulseUntil=time+200'000'000LL; tapUntil=0; }
        if (!down && held) {
            if (!fired && tapEligible && ready) { tapOwner=owner; tapUntil=time+150'000'000LL; }
        }
        held=down;
        return time<pulseUntil;
    }
    int startPlayer() const { return now<tapUntil ? tapOwner : -1; }
    bool visible() const { return held && !fired && now-began>=200'000'000LL; }
    float progress() const { return std::clamp(float(now-began)/float(Duration),0.f,1.f); }
};
inline uint32_t serviceButtons(uint32_t input, bool xr, bool request,
        uint32_t test) {
    return input | (xr && request ? test : 0);
}
}
