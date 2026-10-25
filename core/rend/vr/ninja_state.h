// Ninja Assault native gun-state snapshots. GPL-2.0-or-later.
#pragma once
#include <cstdint>
namespace vr {
struct NinjaGunSample {
 bool codeValid = false;
 uint32_t slot = 0, lifecycle = 0, active = 0, ammo = 0, magazine = 0;
 int32_t ring = -1;
 uint32_t combat = 0;
};
struct NinjaGunTracker {
 bool known = false;
 uint32_t ammo = 0, magazine = 0;
 int32_t ring = -1;
 uint16_t accepted = 0;
};
struct NinjaGunSnapshot {
 bool valid = false, inGame = false, canShoot = false;
 uint8_t ammo = 0, magazine = 0;
 uint16_t shots = 0, fired = 0;
};
// Samples come from a guarded NJA2 read at VBlank, one tracker per player. The
// native projectile ring has four entries and starts at -1; its movement must
// agree with an ammo decrease before a round is accepted. Neither a trigger
// request nor a reload is evidence that the game fired.
inline NinjaGunSnapshot updateNinjaGun(NinjaGunTracker& tracker, const NinjaGunSample& sample) {
 NinjaGunSnapshot out;
 out.shots = out.fired = tracker.accepted;
 const bool bounded = sample.codeValid && sample.slot <= 1 && sample.lifecycle <= 3
  && sample.active <= 1 && sample.magazine > 0 && sample.magazine <= 255
  && sample.ammo <= sample.magazine && sample.ring >= -1 && sample.ring <= 3 && sample.combat <= 1;
 if (!bounded) { tracker.known = false; return out; }
 out.ammo = static_cast<uint8_t>(sample.ammo);
 out.magazine = static_cast<uint8_t>(sample.magazine);
 out.inGame = sample.slot == 1 && sample.lifecycle == 1 && sample.active == 1;
 out.valid = out.canShoot = out.inGame && sample.combat == 1;
 if (!out.canShoot) { tracker.known = false; return out; }
 if (tracker.known && tracker.magazine == sample.magazine && sample.ring >= 0) {
  const int drop = static_cast<int>(tracker.ammo) - static_cast<int>(sample.ammo);
  const int ringStep = tracker.ring < 0 ? sample.ring + 1 : (sample.ring - tracker.ring + 4) % 4;
  // Four/eight rounds can wrap the ring completely between samples. Ammo is the
  // independent evidence for that count; a ring change alone never makes a shot.
  if (drop > 0 && drop <= 8 && (drop % 4) == (ringStep % 4))
   tracker.accepted = static_cast<uint16_t>(tracker.accepted + drop);
 }
 tracker.known = true;
 tracker.ammo = sample.ammo;
 tracker.magazine = sample.magazine;
 tracker.ring = sample.ring;
 out.shots = out.fired = tracker.accepted;
 return out;
}
}
