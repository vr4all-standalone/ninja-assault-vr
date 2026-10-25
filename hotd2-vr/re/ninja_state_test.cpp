#include "ninja_state.h"
#include <cstdio>

int main() {
 int failed = 0, checks = 0;
 auto check = [&](bool pass, const char* name) {
  ++checks; std::printf("%s %s\n",pass?"PASS":"FAIL",name); failed+=!pass;
 };
 const auto sample = [](unsigned ammo, int ring) {
  return vr::NinjaGunSample { true,1,1,1,ammo,8,ring,1 };
 };
 vr::NinjaGunTracker p1, p2;
 auto s=vr::updateNinjaGun(p1,sample(8,-1));
 check(s.valid&&s.inGame&&s.canShoot&&s.ammo==8&&s.magazine==8&&s.fired==0,"native combat initialization exposes the gun without inventing a round");
 s=vr::updateNinjaGun(p1,sample(7,0));
 check(s.shots==1&&s.fired==1,"first measured shot accepts ring -1 to 0 and ammo 8 to 7");
 s=vr::updateNinjaGun(p1,sample(6,1));
 check(s.shots==2&&s.fired==2,"second measured shot counts one accepted round");
 s=vr::updateNinjaGun(p1,sample(8,1));
 check(s.shots==2&&s.fired==2,"measured offscreen reload refills ammo without firing feedback");
 s=vr::updateNinjaGun(p1,sample(7,2));
 check(s.shots==3&&s.fired==3,"first shot after reload resumes the accepted sequence");
 s=vr::updateNinjaGun(p1,sample(8,2));
 check(s.shots==3&&s.fired==3,"manual reload leaves the firing sequence unchanged");
 vr::updateNinjaGun(p2,sample(8,-1));
 auto s2=vr::updateNinjaGun(p2,sample(7,0));
 check(s2.fired==1&&s.fired==3,"the measured player 2 shot has an independent counter");

 vr::NinjaGunTracker empty;
 vr::updateNinjaGun(empty,sample(1,2));
 s=vr::updateNinjaGun(empty,sample(0,3));
 check(s.fired==1&&s.ammo==0,"the last magazine round still counts as a real shot");
 s=vr::updateNinjaGun(empty,sample(0,3));
 check(s.fired==1,"an empty pull never increments accepted rounds");
 vr::updateNinjaGun(empty,sample(8,3));
 s=vr::updateNinjaGun(empty,sample(7,0));
 check(s.fired==2,"native modulo four ring wrap preserves the next shot");

 vr::NinjaGunTracker batch;
 vr::updateNinjaGun(batch,sample(8,0));
 s=vr::updateNinjaGun(batch,sample(4,0));
 check(s.fired==4,"four accepted rounds remain detectable across a complete ring wrap");
 vr::NinjaGunTracker mismatch;
 vr::updateNinjaGun(mismatch,sample(8,0));
 s=vr::updateNinjaGun(mismatch,sample(7,2));
 check(s.fired==0,"an ammo fall with the wrong projectile ring cannot invent a shot");
 s=vr::updateNinjaGun(mismatch,sample(6,3));
 check(s.fired==1,"a mismatched sample rebases before later legitimate shots");
 s=vr::updateNinjaGun(mismatch,sample(6,0));
 check(s.fired==1,"ring movement alone cannot turn an empty pull or reset into a round");

 auto story=sample(8,-1); story.combat=0;
 vr::NinjaGunTracker storyTracker;
 s=vr::updateNinjaGun(storyTracker,story);
 check(s.inGame&&!s.valid&&!s.canShoot,"a real player in a story remains joined while the gun cannot shoot");
 story.ring=2; story.ammo=5; vr::updateNinjaGun(storyTracker,story);
 s=vr::updateNinjaGun(storyTracker,sample(5,2));
 check(s.fired==0,"story or scripted ring changes are rebased at combat resume");
 s=vr::updateNinjaGun(storyTracker,sample(4,3));
 check(s.fired==1,"a real shot after a story still counts");
 auto joining=sample(0,-1); joining.lifecycle=3; joining.active=0;
 s=vr::updateNinjaGun(storyTracker,joining);
 check(!s.inGame&&!s.valid,"native player 2 joining state 3 does not claim active play");
 auto continuing=sample(4,3); continuing.lifecycle=2; continuing.active=0;
 s=vr::updateNinjaGun(storyTracker,continuing);
 check(!s.inGame&&!s.valid,"continue state 2 removes the active gun");
 s=vr::updateNinjaGun(storyTracker,sample(8,-1));
 check(s.fired==1,"a new session rebases without replaying old rounds");

 bool invalid=true;
 for(int field=0;field<9;++field) {
  vr::NinjaGunTracker tracker;
  vr::updateNinjaGun(tracker,sample(8,-1));
  auto bad=sample(7,0);
  switch(field) {
   case 0:bad.codeValid=false;break;
   case 1:bad.slot=2;break;
   case 2:bad.lifecycle=255;break;
   case 3:bad.active=2;break;
   case 4:bad.ammo=9;break;
   case 5:bad.magazine=0;break;
   case 6:bad.ring=-2;break;
   case 7:bad.ring=4;break;
   case 8:bad.combat=2;break;
  }
  s=vr::updateNinjaGun(tracker,bad);
  invalid &= !s.valid&&!s.inGame&&!tracker.known&&s.fired==0;
  s=vr::updateNinjaGun(tracker,sample(6,1));
  invalid &= s.fired==0;
 }
 check(invalid,"all invalid native fields and failed code guards suppress and rebase feedback");
 vr::NinjaGunTracker wide;
 auto huge=sample(8,0);huge.magazine=256;
 check(!vr::updateNinjaGun(wide,huge).valid,"out of range magazine is rejected before narrowing");
 wide.accepted=65535;
 vr::updateNinjaGun(wide,sample(8,3));
 s=vr::updateNinjaGun(wide,sample(7,0));
 check(s.fired==0&&static_cast<uint16_t>(s.fired-65535)==1,"accepted sequence wrap still yields one unsigned round");
 std::printf("%d checks, %d failures\n",checks,failed);
 return failed?1:0;
}
