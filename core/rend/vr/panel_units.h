// Reference panel units and positive overlay depth. GPL-2.0-or-later.
#pragma once
#include <algorithm>
namespace vr {
inline float panelDepthInGameUnits(float distance,float unitScale) { return distance / (unitScale > 0.f ? unitScale : 1.f); }
inline float snappedOverlayDepth(float w,float hud,float low,float high) {
 const float t=w<hud?(w-hud)/(hud-low):(w-hud)/(high-hud);
 return hud+std::min(.0015f,hud*.5f)*std::clamp(t,-1.f,1.f);
}
}
