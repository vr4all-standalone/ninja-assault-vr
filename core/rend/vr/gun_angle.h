/* Shared displayed gun-angle policy. GPL v2 or later; see xr_host.cpp. */
#pragma once
#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>

namespace vr::gun_angle
{
inline int displayed(int value)
{
	return std::clamp(value, -30, 30);
}

// The menu keeps its -30..30 adjustment; its new zero is the previous -10 pose.
inline glm::mat4 tilt(const glm::vec3& palm, int value)
{
	const float pitch = static_cast<float>(displayed(value) - 10);
	return glm::translate(glm::mat4(1.f), palm)
			* glm::rotate(glm::mat4(1.f), glm::radians(pitch), glm::vec3(1, 0, 0))
			* glm::translate(glm::mat4(1.f), -palm);
}
}
