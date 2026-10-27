// Explicit VR-menu cabinet controls and loading-time Start release gates.
#pragma once
#include <cstdint>
#include <string_view>
namespace vr::xr {
enum class ServiceStyle { Naomi, Ninja, Survivor };
inline ServiceStyle serviceStyle(std::string_view id) {
    return (id=="ninjaslt" || id=="NINJA ASSAULT") ? ServiceStyle::Ninja
        : (id=="gunsur2" || id==" BIOHAZARD  GUN SURVIVOR2") ? ServiceStyle::Survivor : ServiceStyle::Naomi;
}
struct StartReleaseGate {
    bool waiting[2]{true, true};
    void reset() { waiting[0] = waiting[1] = true; }
    bool sample(int hand, bool down, bool ready) {
        if (!ready) waiting[hand] = true;
        else if (!down) waiting[hand] = false;
        return ready && down && !waiting[hand];
    }
};
struct ServiceControls {
    bool active=false, pending=false, exiting=false;
    bool waiting[2]{true,true}, held[2]{};
    int64_t until[2]{}, entryUntil=0;
    uint64_t bootGeneration=0;
    void cancel() {
        waiting[0]=waiting[1]=true; held[0]=held[1]=false;
        until[0]=until[1]=entryUntil=0;
    }
    void reset() { active=pending=exiting=false; cancel(); }
    void request(uint64_t generation) {
        if (active) return;
        active=pending=true; bootGeneration=generation; cancel();
    }
    void requestClose() { if (active) { cancel(); pending=exiting=true; } }
    bool finishOnGameBoot(uint64_t generation) {
        if (!active || generation==bootGeneration) return false;
        reset(); return true;
    }
    void update(int64_t time, bool left, bool right, bool allowed) {
        if (!active || !allowed) { cancel(); return; }
        if (pending) { pending=false; entryUntil=time+200'000'000LL; }
        if (exiting) return;
        const bool down[2]{left,right};
        for (int hand=0;hand<2;++hand) {
            if (waiting[hand]) { if (!down[hand]) waiting[hand]=false; continue; }
            if (down[hand] && !held[hand]) until[hand]=time+75'000'000LL;
            held[hand]=down[hand];
        }
    }
    bool entry(int64_t time) const { return active && time<entryUntil; }
    bool select(int64_t time) const { return active && time<until[1]; }
    bool test(int64_t time) const { return active && (time<entryUntil || time<until[1]); }
    bool next(int64_t time) const { return active && time<until[0]; }
};
inline uint32_t serviceButtons(uint32_t input, bool xr, bool request,
        uint32_t test, bool next=false, uint32_t service=0, bool select=false, uint32_t enter=0) {
    return input | (xr ? (request ? test : 0) | (next ? service : 0) | (select ? enter : 0) : 0);
}
}
