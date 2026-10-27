#include "ninja_service.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

int main() {
    using namespace vr::ninja;
    std::unordered_map<unsigned,unsigned> words;
    for (const auto &g:ServiceGuards) words[g.offset]=g.word;
    unsigned mode=1, callback=GameCallback, checks=0;
    auto read16=[&](unsigned a) {return words[a];};
    auto read32=[&](unsigned a) {return a==ServiceMode ? mode : callback;};
    ServiceExitObserver observer;
    auto expect=[&](bool wanted) {++checks;if(observer.poll(read16,read32)!=wanted){std::printf("FAIL %u\n",checks);std::exit(1);}};
    expect(false); // Ordinary game frames do not report an exit.
    mode=MenuMode; callback=GameCallback; expect(false);
    mode=1; expect(false); // A mode value alone cannot arm the observer.
    mode=MenuMode; callback=MenuCallback; expect(false);
    expect(false); // Native submenu frames keep the menu armed.
    mode=0; expect(false); // Unmeasured gameplay values do not report a return.
    mode=2; expect(false);
    mode=99; expect(false); // Unknown state cannot report or erase the latch.
    mode=1; callback=0x0c0f7a80; expect(false); // Wait for validated main callback.
    callback=GameCallback;
    const unsigned guard=ServiceGuards[0].offset, stock=words[guard];
    words[guard]^=1; expect(false); // Guard failure cannot erase the latch.
    words[guard]=stock; expect(true);
    expect(false); // One notification per native menu return.
    mode=MenuMode; callback=MenuCallback; expect(false);
    mode=1; callback=GameCallback; expect(true); // Repeated entry and exit.
    mode=MenuMode; callback=MenuCallback; expect(false);
    observer.reset(); mode=1; callback=GameCallback; expect(false);
    for (const auto &g:ServiceGuards) {
        mode=MenuMode; callback=MenuCallback;
        const unsigned old=words[g.offset];words[g.offset]^=1;
        expect(false);words[g.offset]=old;
        mode=1;callback=GameCallback;expect(false);
    }
    std::printf("ninja_service_exit_test: %u checks passed\n",checks);
}