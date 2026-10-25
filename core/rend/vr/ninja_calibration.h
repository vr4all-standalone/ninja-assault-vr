// Ninja Assault JYU calibration inverse. GPL-2.0-or-later.
#pragma once
#include <cstdint>
#include <limits>
namespace vr {
struct NinjaGunAxisCalibration { int32_t scale, offset; };
// The original NJA2 reader applies (raw * scale + offset) / 100 using signed
// integers. JYU command 0x25 carries direct u16 coordinates; zero means no sample.
// These are not the normalized analog/three-point calibration values of Lupin.
// Build only after a guarded read of this player's live coefficients. A false
// result leaves the map unusable; the caller must retain its uncalibrated input.
inline bool ninjaCalibrationInverse(int extent, const NinjaGunAxisCalibration& axis, int* out) {
 if (!out || extent < 2 || extent > 640 || axis.scale == 0) return false;
 const int64_t first = static_cast<int64_t>(axis.scale) + axis.offset;
 const int64_t last = static_cast<int64_t>(axis.scale) * 65535 + axis.offset;
 const int64_t minimum = std::numeric_limits<int32_t>::min();
 const int64_t maximum = std::numeric_limits<int32_t>::max();
 if (first < minimum || first > maximum || last < minimum || last > maximum) return false;
 const auto rawAt = [&](int index) { return axis.scale > 0 ? index : 65536 - index; };
 const auto projected = [&](int index) {
  return (static_cast<int64_t>(rawAt(index)) * axis.scale + axis.offset) / 100;
 };
 // Search the monotonic native output, including its truncation toward zero.
 // At most 16 comparisons per display pixel, and no table of all 65535 inputs.
 for (int target = 0; target < extent; ++target) {
  int low = 1, high = 65536;
  while (low < high) {
   const int middle = low + (high - low) / 2;
   if (projected(middle) < target) low = middle + 1;
   else high = middle;
  }
  int best = low <= 65535 ? low : 65535;
  int64_t error = projected(best) - target;
  if (error < 0) error = -error;
  if (low > 1) {
   const int other = low - 1;
   int64_t otherError = projected(other) - target;
   if (otherError < 0) otherError = -otherError;
   if (otherError < error || (otherError == error && rawAt(other) < rawAt(best))) {
    best = other; error = otherError;
   }
  }
  // Reject broken/unreachable calibrations instead of turning a visible aim into
  // a distant hit. Measured NJA2 coefficients stay within one pixel throughout.
  if (error > 2) return false;
  out[target] = rawAt(best);
 }
 return true;
}
}
