#include "vr4all_splash.h"
#include <cassert>
#include <chrono>
#include <iostream>
using namespace std::chrono;
int main()
{
    vr::xr::LaunchSplash splash;
    assert(splash.active());
    // Only successfully presented, focused headset frames call presented().
    for (int i = 0; i < 299; ++i) splash.presented(milliseconds(10));
    assert(splash.active());
    splash.presented(milliseconds(10));
    assert(!splash.active());
    splash.reset();
    assert(splash.active());
    splash.presented(nanoseconds(0));
    splash.presented(nanoseconds(-1));
    assert(splash.active());
    // A bad runtime period or a long background gap cannot consume the splash.
    splash.presented(seconds(60));
    for (int i = 0; i < 289; ++i) splash.presented(milliseconds(10));
    assert(splash.active());
    splash.presented(milliseconds(10));
    assert(!splash.active());
    splash.presented(milliseconds(10));
    assert(!splash.active());
    splash.reset();
    for (int i = 0; i < 360; ++i) splash.presented(nanoseconds(8333333));
    assert(splash.active());
    splash.presented(nanoseconds(8333333));
    assert(!splash.active());
    std::cout << "PASS VR4ALL splash: 3 seconds, reset, invalid periods, background gap, 120 Hz\n";
}
