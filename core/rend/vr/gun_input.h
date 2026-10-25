// Lupin analog off-screen sensor adapter. GPL v2 or later.
#pragma once
#include <cstdint>
namespace vr {
struct GunInput { int x = 5000, y = 5000; bool held = false, reload = false; };
// Lupin treats a zero/off-screen ADC as reload even without a trigger. Keep a valid
// coordinate until the VR driver explicitly requests reload; B uses that sensor too.
// Other games retain their native input unchanged. One state per player.
inline void adaptGunInput(GunInput& last, bool sensorReload, int& x, int& y, uint32_t& buttons) {
 if (!sensorReload) return;
 const bool on = x >= 0 && x < 10000 && y >= 0 && y < 10000;
 if (on) { last.x=x; last.y=y; }
 const bool manual = (buttons & 128u) != 0;
 const bool held = (buttons & 3u) != 0 || manual;
 if (held) {
  // Bits 1 and 2 are the same held cabinet trigger. The VR aim may cross the
  // screen edge during it; keep the original shot/reload category until release.
  // An explicit B is allowed to request a new reload. The VR driver first releases
  // its trigger for the reload pulse, so that path is a fresh pull too.
  if (!last.held || manual) last.reload = manual || (buttons & 2u) || !on;
  if (last.reload) { x=y=-10000; buttons=(buttons & ~(1u|128u))|2u; }
  else { buttons=(buttons & ~2u)|1u; if (!on) { x=last.x; y=last.y; } }
 } else if (!on) { x=last.x; y=last.y; }
 last.held = held;
}
// Namco JYU reloads from its off-screen position plus trigger, not its B button.
// Keep the first pull's shot/reload category until release, independently for
// each player. Idle coordinates remain native: unlike Lupin there is no reason
// to hold its last ADC merely because the aim leaves the screen.
inline void adaptNinjaInput(GunInput& last, bool ninja, int& x, int& y, uint32_t& buttons) {
 if (!ninja) return;
 const bool on = x >= 0 && x < 10000 && y >= 0 && y < 10000;
 if (on) { last.x=x; last.y=y; }
 const bool manual = (buttons & 128u) != 0;
 const bool held = (buttons & 3u) != 0 || manual;
 if (held) {
  if (!last.held || manual) last.reload = manual || (buttons & 2u) || !on;
  if (last.reload) { x=y=-10000; buttons=(buttons & ~(1u|128u))|2u; }
  else { buttons=(buttons & ~2u)|1u; if (!on) { x=last.x; y=last.y; } }
 }
 last.held = held;
}
}
