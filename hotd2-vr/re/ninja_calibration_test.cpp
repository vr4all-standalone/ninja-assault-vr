#include "ninja_calibration.h"
#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// Independent reference for the two multiply/add/signed-divide sequences at
// original NJA2 0x0C130A3C..0x0C130A66. This deliberately uses no inverse helper.
static int nativeForward(int raw, const vr::NinjaGunAxisCalibration& a) {
 return static_cast<int>((static_cast<int64_t>(raw) * a.scale + a.offset) / 100);
}
int main() {
 int failed = 0, checks = 0;
 auto check = [&](bool pass, const char* name) {
  ++checks; std::printf("%s %s\n", pass ? "PASS" : "FAIL", name); failed += !pass;
 };
 int map[640] {};
 const vr::NinjaGunAxisCalibration measured[4] = {
  {179,-9984}, {101,-6656}, {179,-9984}, {101,-6656}
 };
 const int literalRaw[4][3] = {{100,235,391},{113,304,494},{100,235,391},{113,304,494}};
 const int literalPixels[4][3] = {{79,320,600},{47,240,432},{79,320,600},{47,240,432}};
 for (int axis=0; axis<4; ++axis) {
  const int extent = axis%2 ? 480 : 640;
  bool fixtures = true;
  for (int k=0;k<3;++k) fixtures &= nativeForward(literalRaw[axis][k], measured[axis]) == literalPixels[axis][k];
  check(fixtures,"measured native arithmetic fixtures match literal expectations");
  const bool ready = vr::ninjaCalibrationInverse(extent, measured[axis], map);
  check(ready,"both players' measured axes build inverse maps");
  bool bounded=ready, accurate=ready;
  for (int target=0;target<extent && ready;++target) {
   bounded &= map[target]>=1 && map[target]<=65535;
   accurate &= std::abs(nativeForward(map[target],measured[axis])-target)<=1;
  }
  check(bounded,"inverse never emits zero sentinel or wraps the JYU u16");
  check(accurate,"every game pixel maps through the independent native reference within one pixel");
  check(ready && nativeForward(map[extent/2],measured[axis])==extent/2,"center aim remains exact");
  if(axis%2)check(ready && map[479]>479,"bottom row can use the measured raw Y range above 479");
 }
 check(vr::ninjaCalibrationInverse(640,{100,0},map) && map[0]==1 && map[639]==639,"neutral axis preserves zero sentinel while reaching far edge");
 check(vr::ninjaCalibrationInverse(640,{-100,64000},map) && nativeForward(map[0],{-100,64000})==0 && nativeForward(map[639],{-100,64000})==639,"inverted axes remain supported");
 check(vr::ninjaCalibrationInverse(640,{1,0},map) && nativeForward(map[639],{1,0})==639,"small valid scale uses the full raw u16 range");
 check(!vr::ninjaCalibrationInverse(640,{0,0},map),"zero scale is rejected");
 check(!vr::ninjaCalibrationInverse(640,{INT_MAX,INT_MAX},map),"coefficients that overflow native signed arithmetic are rejected");
 check(!vr::ninjaCalibrationInverse(640,{100,INT_MIN},map),"unreachable screen range is rejected");
 check(!vr::ninjaCalibrationInverse(640,{100,1000000},map),"offset that cannot cover the screen is rejected");
 check(!vr::ninjaCalibrationInverse(640,{1000,0},map),"calibration with excessive unavoidable pixel error is rejected");
 check(!vr::ninjaCalibrationInverse(0,{100,0},map) && !vr::ninjaCalibrationInverse(641,{100,0},map),"invalid display extents are rejected");
 check(!vr::ninjaCalibrationInverse(640,{100,0},nullptr),"null map output is rejected");
 std::printf("%d checks, %d failures\n",checks,failed);
 return failed?1:0;
}
