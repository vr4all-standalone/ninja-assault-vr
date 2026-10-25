// Shared VR4ALL launch splash. The host advances this only for focused, presented frames.
#pragma once
#include <algorithm>
#include <chrono>

namespace vr::xr
{
class LaunchSplash
{
    std::chrono::nanoseconds remaining = std::chrono::seconds(3);
public:
    void reset() { remaining = std::chrono::seconds(3); }
    bool active() const { return remaining.count() > 0; }
    void presented(std::chrono::nanoseconds period)
    {
        if (period.count() <= 0 || !active()) return;
        // Guard malformed runtime periods; hiding the app never spends its visible time.
        period = std::min(period, std::chrono::nanoseconds(std::chrono::milliseconds(100)));
        remaining -= std::min(remaining, period);
    }
};
}
