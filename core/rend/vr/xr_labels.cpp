/*
	The how-to-play labels by the controllers' buttons (hotd2-vr). See xr_labels.h.

	Copyright 2026 mikermak. This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#include "xr_labels.h"
#include "xr_controllers.h"
#include "xr_host.h"
#include "xr_own.h"
#include "xr_panel.h"
#include "log/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace vr::xr::labels
{
namespace
{

using controllers::Button;

struct Label
{
	Button button;
	const char *text;
};

// Single-player labels keep A=P1 Start, X=P2 Join, either reload button for P1,
// and either trigger for shooting/moving the gun. Dual labels follow the actual
// assigned players. Button anchors remain the runtime profile's own controls.
// Roboto Bold has no triple bar (U+2261), so Touch's menu button says "Menu".
struct Words
{
	const char *profile;
	std::vector<Label> hand[2];	// left, right
};
const Words KindsSingle[] {
	{ "/interaction_profiles/oculus/touch_controller", {
		{ { controllers::Y, "Y - P1 Reload" }, { controllers::X, "X - P2 Join" }, { controllers::Menu, "Menu" },
			{ controllers::Trigger, "Trigger - Shoot / move gun" }, { controllers::Stick, "Stick - Recenter" } },
		{ { controllers::B, "B - P1 Reload" }, { controllers::A, "A - P1 Start" }, { controllers::Trigger, "Trigger - Shoot / move gun" },
			{ controllers::Stick, "Stick - Laser" } } } },
	{ "/interaction_profiles/valve/index_controller", {
		{ { controllers::Y, "B - P1 Reload" }, { controllers::X, "A - P2 Join" }, { controllers::Menu, "Pad squeeze - Menu" },
			{ controllers::Trigger, "Trigger - Shoot / move gun" }, { controllers::Stick, "Stick - Recenter" } },
		{ { controllers::B, "B - P1 Reload" }, { controllers::A, "A - P1 Start" }, { controllers::Trigger, "Trigger - Shoot / move gun" },
			{ controllers::Stick, "Stick - Laser" } } } },
	{ "/interaction_profiles/htc/vive_controller", {
		{ { controllers::Y, "Pad click - P1 Reload" }, { controllers::X, "Menu - P2 Join (hold: Menu)" },
			{ controllers::Trigger, "Trigger - Shoot / move gun" } },
		{ { controllers::B, "Pad click - P1 Reload" }, { controllers::A, "Menu - P1 Start" }, { controllers::Trigger, "Trigger - Shoot / move gun" } } } },
	{ "/interaction_profiles/microsoft/motion_controller", {
		{ { controllers::Y, "Pad click - P1 Reload" }, { controllers::X, "Menu - P2 Join (hold: Menu)" },
			{ controllers::Trigger, "Trigger - Shoot / move gun" }, { controllers::Stick, "Stick - Recenter" } },
		{ { controllers::B, "Pad click - P1 Reload" }, { controllers::A, "Menu - P1 Start" }, { controllers::Trigger, "Trigger - Shoot / move gun" },
			{ controllers::Stick, "Stick - Laser" } } } },
};
const Words Kinds[] {
	{ "/interaction_profiles/oculus/touch_controller", {
		{ { controllers::Y, "Y - P2 Reload" }, { controllers::X, "X - P2 Start" }, { controllers::Menu, "Menu" },
			{ controllers::Trigger, "Trigger - P2 Shoot" }, { controllers::Stick, "Stick - Recenter" } },
		{ { controllers::B, "B - P1 Reload" }, { controllers::A, "A - P1 Start" }, { controllers::Trigger, "Trigger - P1 Shoot" },
			{ controllers::Stick, "Stick - Laser" } } } },
	{ "/interaction_profiles/valve/index_controller", {
		{ { controllers::Y, "B - P2 Reload" }, { controllers::X, "A - P2 Start" }, { controllers::Menu, "Pad squeeze - Menu" },
			{ controllers::Trigger, "Trigger - P2 Shoot" }, { controllers::Stick, "Stick - Recenter" } },
		{ { controllers::B, "B - P1 Reload" }, { controllers::A, "A - P1 Start" }, { controllers::Trigger, "Trigger - P1 Shoot" },
			{ controllers::Stick, "Stick - Laser" } } } },
	{ "/interaction_profiles/htc/vive_controller", {
		{ { controllers::Y, "Pad click - P2 Reload" }, { controllers::X, "Menu - P2 Start (hold: Menu)" },
			{ controllers::Trigger, "Trigger - P2 Shoot" } },
		{ { controllers::B, "Pad click - P1 Reload" }, { controllers::A, "Menu - P1 Start" }, { controllers::Trigger, "Trigger - P1 Shoot" } } } },
	{ "/interaction_profiles/microsoft/motion_controller", {
		{ { controllers::Y, "Pad click - P2 Reload" }, { controllers::X, "Menu - P2 Start (hold: Menu)" },
			{ controllers::Trigger, "Trigger - P2 Shoot" }, { controllers::Stick, "Stick - Recenter" } },
		{ { controllers::B, "Pad click - P1 Reload" }, { controllers::A, "Menu - P1 Start" }, { controllers::Trigger, "Trigger - P1 Shoot" },
			{ controllers::Stick, "Stick - Laser" } } } },
};
// Dual P1-left layout: each hand's buttons name its player (the same
// buttons and order as Kinds)
const Words KindsP1Left[] {
	{ "/interaction_profiles/oculus/touch_controller", {
		{ { controllers::Y, "Y - P1 Reload" }, { controllers::X, "X - P1 Start" }, { controllers::Menu, "Menu" },
			{ controllers::Trigger, "Trigger - P1 Shoot" }, { controllers::Stick, "Stick - Recenter" } },
		{ { controllers::B, "B - P2 Reload" }, { controllers::A, "A - P2 Start" }, { controllers::Trigger, "Trigger - P2 Shoot" },
			{ controllers::Stick, "Stick - Laser" } } } },
	{ "/interaction_profiles/valve/index_controller", {
		{ { controllers::Y, "B - P1 Reload" }, { controllers::X, "A - P1 Start" }, { controllers::Menu, "Pad squeeze - Menu" },
			{ controllers::Trigger, "Trigger - P1 Shoot" }, { controllers::Stick, "Stick - Recenter" } },
		{ { controllers::B, "B - P2 Reload" }, { controllers::A, "A - P2 Start" }, { controllers::Trigger, "Trigger - P2 Shoot" },
			{ controllers::Stick, "Stick - Laser" } } } },
	{ "/interaction_profiles/htc/vive_controller", {
		{ { controllers::Y, "Pad click - P1 Reload" }, { controllers::X, "Menu - P1 Start (hold: Menu)" },
			{ controllers::Trigger, "Trigger - P1 Shoot" } },
		{ { controllers::B, "Pad click - P2 Reload" }, { controllers::A, "Menu - P2 Start" }, { controllers::Trigger, "Trigger - P2 Shoot" } } } },
	{ "/interaction_profiles/microsoft/motion_controller", {
		{ { controllers::Y, "Pad click - P1 Reload" }, { controllers::X, "Menu - P1 Start (hold: Menu)" },
			{ controllers::Trigger, "Trigger - P1 Shoot" }, { controllers::Stick, "Stick - Recenter" } },
		{ { controllers::B, "Pad click - P2 Reload" }, { controllers::A, "Menu - P2 Start" }, { controllers::Trigger, "Trigger - P2 Shoot" },
			{ controllers::Stick, "Stick - Laser" } } } },
};
static_assert(std::size(KindsP1Left) == std::size(Kinds), "a P1-left set for each controller kind");
static_assert(std::size(KindsSingle) == std::size(Kinds), "a single-player set for each controller kind");
// The log's order, the README's: Start, the reload, the shot, the stick, the menu (by Button: A, B, X, Y, Menu, Stick,
// Trigger, Grip)
constexpr int ReadingOrder[controllers::Buttons] { 0, 1, 0, 1, 4, 3, 2, 5 };

// A label's letters (its picture's pixels) and its height in the room: letters ~7.5 mm high, about
// as sharp as a Quest 3 shows them at 40 cm. The column: from the hand's outermost button, and from
// one label to the next at least. The stick's label: over the column's top and the stick; the trigger's
// one Pitch over that.
constexpr float Px = 40.f;
constexpr float Height = 0.016f;
constexpr float Gap = 0.025f;
constexpr float Pitch = 0.021f;
constexpr float StickRise = 0.026f;
constexpr float LineWidth = 0.0012f, Dot = 0.0024f;
const glm::vec3 TextColour(0.96f);
const glm::vec4 LineColour(0.92f, 0.92f, 0.92f, 0.85f);

// The frame's two eyes, 6-7 cm apart, share one layout: laid out again once the eye it was made for
// is further than this (or the grip moved).
constexpr float EyeMoves = 0.1f;

// A label as laid out (room space): its text (its picture, textTexture's, looked up when drawn: a
// session's pictures go with termPanel), its middle and half sizes, and its line from the button
// (from) to the label's near edge (to).
struct Placed
{
	std::string text;
	glm::vec3 centre;
	float hx, hy;
	glm::vec3 from, to;
};
// One hand's labels as laid out for a grip pose and its buttons' places (room space, in the list's order),
// seen from an eye
struct Layout
{
	glm::mat4 grip { 0.f };
	std::vector<glm::vec3> at;
	glm::vec3 eye { 0.f };
	const Words *words = nullptr;
	std::vector<Placed> placed;
};
Layout layouts[2];

// Where each hand's label lines end on its controller, as last logged
struct Logged
{
	controllers::Button button;
	bool ownModel, fromFile;
	glm::vec3 local;
	bool operator==(const Logged& o) const { return button == o.button && ownModel == o.ownModel && fromFile == o.fromFile && local == o.local; }
};
std::vector<Logged> logged[2];
// Each hand's set as last logged (once a session, unless the controller kind changes)
const Words *setLogged[2] {};
const char *const HandName[2] { "left", "right" };

// The current mode and dual layout select each profile's instructions.
const Words& wordsFor(const char *profile)
{
	const Words *kinds = !dualWielding() ? KindsSingle : p1Hand() == 0 ? KindsP1Left : Kinds;
	for (size_t i = 0; i < std::size(Kinds); i++)
		if (profile != nullptr && !strcmp(profile, kinds[i].profile))
			return kinds[i];
	return kinds[0];
}

// The eye of a projection (room space): the point it sends to x = y = w = 0.
glm::vec3 eyeOf(const glm::mat4& viewProj)
{
	const int rows[3] { 0, 1, 3 };
	glm::mat3 m;
	glm::vec3 b;
	for (int i = 0; i < 3; i++)
	{
		for (int c = 0; c < 3; c++)
			m[c][i] = viewProj[c][rows[i]];
		b[i] = -viewProj[3][rows[i]];
	}
	return glm::inverse(m) * b;
}

// Across (to the right as seen) and up, in a plane facing `eye` from `at`, the room's up kept up
// (looking straight down: across along `fallback`).
void facing(const glm::vec3& at, const glm::vec3& eye, const glm::vec3& fallback, glm::vec3& across, glm::vec3& up, glm::vec3& toEye)
{
	toEye = eye - at;
	toEye = glm::length(toEye) > 1e-4f ? glm::normalize(toEye) : glm::vec3(0.f, 0.f, 1.f);
	across = glm::cross(glm::vec3(0.f, 1.f, 0.f), toEye);
	if (glm::length(across) < 1e-3f)
		across = fallback - toEye * glm::dot(fallback, toEye);
	across = glm::normalize(across);
	up = glm::cross(toEye, across);
}

// Where one hand's labels go, seen from `eye`: a column beside the hand's outermost button (the
// right hand's to its right as seen, the left's to its left), in the list's order, Pitch apart at
// least and centred on their buttons' height; the stick's above the column's top, over the stick, and
// the trigger's right on top of the stick's (1.2 round 2, the user's ask: not under A or X), its line to
// its outer end, past the stick's. A list without a stick (the Vive wand's) keeps the trigger's in the column.
void layOut(Layout& layout, int hand, const glm::mat4& grip, const Words& words, const glm::vec3& eye)
{
	layout.placed.clear();
	const std::vector<Label>& list = words.hand[hand];
	const std::vector<glm::vec3>& at = layout.at;
	if (list.empty() || at.size() != list.size())
		return;
	glm::vec3 middle(0.f);
	for (const glm::vec3& p : at)
		middle += p;
	middle /= (float)at.size();
	glm::vec3 across, up, toEye;
	facing(middle, eye, glm::vec3(grip[0]), across, up, toEye);
	auto planeX = [&](const glm::vec3& p) { return glm::dot(p - middle, across); };
	auto planeY = [&](const glm::vec3& p) { return glm::dot(p - middle, up); };
	auto room = [&](float x, float y) { return middle + across * x + up * y; };
	const float side = hand == 1 ? 1.f : -1.f;
	float outer = 0.f;
	for (const glm::vec3& p : at)
		outer = std::max(outer, side * planeX(p));
	const float columnX = side * (outer + Gap);

	const bool stacked = std::any_of(list.begin(), list.end(), [](const Label& l) { return l.button == controllers::Stick; });
	std::vector<size_t> column;
	std::vector<float> ys;
	float buttonsY = 0.f;
	for (size_t i = 0; i < list.size(); i++)
		if (list[i].button != controllers::Stick && !(stacked && list[i].button == controllers::Trigger))
		{
			float y = planeY(at[i]);
			buttonsY += y;
			if (!ys.empty())
				y = std::min(y, ys.back() - Pitch);
			column.push_back(i);
			ys.push_back(y);
		}
	if (!ys.empty())
	{
		float labelsY = 0.f;
		for (float y : ys)
			labelsY += y;
		const float shift = (buttonsY - labelsY) / ys.size();
		for (float& y : ys)
			y += shift;
	}
	// toOuter: the line to the label's outer end (side), not to `to`
	auto place = [&](size_t i, float x, float y, float alignX, const glm::vec3& to, bool toOuter = false) {
		int w, h;
		const unsigned texture = textTexture(list[i].text, Px, TextColour, w, h);
		if (texture == 0 || h == 0)
			return;
		const float hy = Height * 0.5f, hx = hy * w / h;
		layout.placed.push_back({ list[i].text, room(x + alignX * hx, y), hx, hy, at[i],
				toOuter ? room(x + alignX * hx + side * hx, y) : to });
	};
	for (size_t k = 0; k < column.size(); k++)
		place(column[k], columnX, ys[k], side, room(columnX, ys[k]));
	float stickX = 0.f, stickY = 0.f;
	for (size_t i = 0; i < list.size(); i++)
		if (list[i].button == controllers::Stick)
		{
			const float x = planeX(at[i]);
			const float y = std::max(planeY(at[i]), ys.empty() ? -1.f : ys.front()) + StickRise;
			place(i, x, y, 0.f, room(x, y - Height * 0.5f));
			stickX = x;
			stickY = y;
		}
	if (stacked)
		for (size_t i = 0; i < list.size(); i++)
			if (list[i].button == controllers::Trigger)
				place(i, stickX, stickY + Pitch, 0.f, glm::vec3(0.f), true);
}

// A quad facing `eye` at `centre`, its top towards the room's up.
glm::mat4 billboard(const glm::vec3& centre, const glm::vec3& eye)
{
	glm::vec3 across, up, toEye;
	facing(centre, eye, glm::vec3(1.f, 0.f, 0.f), across, up, toEye);
	return glm::mat4(glm::vec4(across, 0.f), glm::vec4(up, 0.f), glm::vec4(toEye, 0.f), glm::vec4(centre, 1.f));
}

}	// namespace

void draw(const glm::mat4& viewProj, int hand, const glm::mat4& grip, const glm::mat4& aim, const char *profile)
{
	if (hand != 0 && hand != 1)
		return;
	const glm::vec3 eye = eyeOf(viewProj);
	const Words& words = wordsFor(profile);
	Layout& layout = layouts[hand];
	// where the lines end on the controller drawn (its own model's points move with the aim pose)
	const std::vector<Label>& list = words.hand[hand];
	controllers::LabelPoint points[8];
	const size_t count = std::min<size_t>(list.size(), 8);
	bool moved = layout.at.size() != count;
	for (size_t i = 0; i < count; i++)
	{
		points[i] = controllers::labelPoint(hand, list[i].button, grip, aim);
		moved = moved || layout.at[i] != points[i].room;
	}
	if (moved || layout.grip != grip || layout.words != &words || layout.placed.empty() || glm::length(eye - layout.eye) > EyeMoves)
	{
		layout.grip = grip;
		layout.eye = eye;
		layout.words = &words;
		layout.at.resize(count);
		for (size_t i = 0; i < count; i++)
			layout.at[i] = points[i].room;
		layOut(layout, hand, grip, words, eye);
	}
	// the log: the set this hand shows, in the reading order, when it changes
	if (setLogged[hand] != &words)
	{
		setLogged[hand] = &words;
		std::vector<Label> sorted(list);
		std::stable_sort(sorted.begin(), sorted.end(), [](const Label& a, const Label& b) {
			return ReadingOrder[a.button] < ReadingOrder[b.button];
		});
		std::string set;
		for (const Label& l : sorted)
			set += (set.empty() ? "" : ", ") + std::string(l.text);
		NOTICE_LOG(RENDERER, "XR: labels set: %s: %s", HandName[hand], set.c_str());
	}
	// the log: where each line ends on the controller, when that changes (not with the pose)
	bool same = logged[hand].size() == count;
	for (size_t i = 0; i < count && same; i++)
		same = logged[hand][i] == Logged { list[i].button, points[i].ownModel, points[i].fromFile, points[i].local };
	if (!same)
	{
		logged[hand].clear();
		for (size_t i = 0; i < count; i++)
		{
			const controllers::LabelPoint& p = points[i];
			logged[hand].push_back({ list[i].button, p.ownModel, p.fromFile, p.local });
			const char *name = own::ButtonNames[list[i].button];
			const std::string why = p.fromFile ? "from the file"
					: p.ownModel ? std::string("built in: no \"") + name + "\" from " + controllers::ownSidecar(hand) : "built in";
			NOTICE_LOG(RENDERER, "XR: labels: %s %s at (%.4f, %.4f, %.4f) in the %s pose (%s)", HandName[hand], name,
					p.local.x, p.local.y, p.local.z, p.fromFile ? "aim" : "grip", why.c_str());
		}
	}
	// the lines and their dots first, the labels over them
	for (const Placed& p : layout.placed)
	{
		const glm::vec3 along = p.to - p.from;
		const float length = glm::length(along);
		if (length > 1e-4f)
		{
			const glm::vec3 middle = (p.from + p.to) * 0.5f;
			const glm::vec3 x = along / length;
			glm::vec3 y = glm::cross(x, eye - middle);
			if (glm::length(y) > 1e-6f)
			{
				y = glm::normalize(y);
				const glm::mat4 model(glm::vec4(x, 0.f), glm::vec4(y, 0.f), glm::vec4(glm::cross(x, y), 0.f), glm::vec4(middle, 1.f));
				drawTexture(viewProj, model, 0, length * 0.5f, LineWidth * 0.5f, LineColour);
			}
		}
		drawTexture(viewProj, billboard(p.from, eye), 0, Dot * 0.5f, Dot * 0.5f, LineColour);
	}
	for (const Placed& p : layout.placed)
	{
		int w, h;
		const unsigned texture = textTexture(p.text, Px, TextColour, w, h);
		if (texture != 0)
			drawTexture(viewProj, billboard(p.centre, eye), texture, p.hx, p.hy);
	}
}

void term()
{
	for (Layout& layout : layouts)
		layout = Layout();
	for (std::vector<Logged>& l : logged)
		l.clear();
	setLogged[0] = setLogged[1] = nullptr;
}

}
