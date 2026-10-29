// Shared appearance policy for the selectable Arcade gun and its emergency baked fallback.
#pragma once
#include <glm/glm.hpp>
#include <cstring>

namespace vr::xr
{
// Logical native player identity, independent of controller hand and mirror transform.
inline glm::vec3 arcadeGunBaseColor(int player)
{
	return player == 1 ? glm::vec3(0.04f, 0.25f, 1.00f) : glm::vec3(0.90f, 0.04f, 0.04f);
}

// Named plastic surfaces of the bundled Namco model. Grey grip textures still recolor;
// screws, the lens and unrecognized replacement-model surfaces keep their own material.
inline bool arcadeGunPlasticPart(const char *name)
{
 return name != nullptr && (!std::strcmp(name, "hardsurfaces") || !std::strcmp(name, "softsurfaces"));
}

// The override replaces RGB before lighting; alpha retains the original texture/material cutout.
inline constexpr const char *ArcadeModelBaseSource = R"(
mediump vec4 arcadeModelBase(mediump vec4 texel, mediump vec4 vertexColor, mediump vec4 overrideColor)
{
	mediump vec4 base = texel * vertexColor;
	if (overrideColor.a > 0.5)
		base = vec4(vec3(overrideColor), base.a);
	return base;
}
)";
}
