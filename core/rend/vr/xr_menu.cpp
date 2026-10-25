/*
	The settings menu in the headset (hotd2-vr). See xr_menu.h.

	Copyright 2026 mikermak. This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#include "xr_menu.h"
#include "xr_hands.h"
#include "xr_own.h"
#include "xr_host.h"
#include "xr_panel.h"
#include "cfg/cfg.h"
#include "cfg/option.h"
#include "emulator.h"
#include "log/Log.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

namespace vr::xr::menu
{
namespace
{

constexpr float Distance = 1.2f, BelowEyes = 0.1f;
constexpr int Left = 0, Right = 1;

const char *const Labels[Rows] { "Resume", "Recenter", "Gun", "Gun angle", "Reload", "Dual hands", "Laser", "Haptics",
		"World size", "Panel distance", "Wider view", "How to play", "Quit" };
const char *const HapticsNames[4] { "Off", "Light", "Medium", "Strong" };
// vr.Haptics' words, saved by the Haptics row (hapticsLevel() reads them, and 1.2's numbers)
const char *const HapticsWords[4] { "off", "light", "medium", "strong" };
const char *const ReloadNames[2] { "Shoot off-screen", "Point off-screen" };
// The preference applies only while both players are in. Single controls stay fixed.
const char *const HandsNames[2] { "P1 left / P2 right", "P1 right / P2 left" };
constexpr int GunAngleMax = 30;	// vr.GunAngle's range, degrees either way

glm::mat4 model { 1.f };	// the panel: its middle, facing the player (+z)
int selected;
// The row each hand's ray is on (-1: none), and whether that's known yet: the first update after
// opening, or after How to play swapped a hand's gun and controller (the ray leaves the muzzle or
// the controller then), learns them without choosing (the selection follows a ray onto another row).
int hovered[2];
bool fresh;
// A Gun angle step turns the guns, and the pointers that leave their muzzles, in the next frame:
// that frame's pointers select nothing (else a stick held on Gun angle goes on stepping another row)
bool gunsTurned;
bool pointing[2];
glm::vec3 pointAt[2];
// Quit is chosen twice (S16): the first choose arms it for 3 s ("Quit? Choose again"); another
// selection, a stick move or closing takes that back. Once it quits, the menu is latched and takes
// nothing more: no second quit, no Resume starting the emulator again (reset clears it).
constexpr double QuitConfirmSeconds = 3.0;
bool quitArmed;
double quitArmedAt;
bool quitting;

double seconds()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// vr.Widen as the game started with it: it widens at the start only, so a change says "(next start)"
bool widenAtStart = true;
struct WidenRegistration
{
	WidenRegistration() {
		EventManager::listen(Event::Start, [](Event, void *) { widenAtStart = config::VrWiden; });
	}
} widenRegistration;

// vr.Gun's place in its choices (xr_hands.h gunChoices()): a folder gun by vr.GunFile (4 and over all
// meaning that, its name in any case); any other setting, 1.1's 0, 2 and 3 too, the in-game pistol's, as
// chosenGun() reads them; not there: the first's
int gunAt(const std::vector<GunChoice>& choices)
{
	const int setting = config::VrGun >= OwnGuns ? OwnGuns : 1;
	for (size_t i = 0; i < choices.size(); i++)
		if (choices[i].setting == setting && (choices[i].file == nullptr || own::sameStem(config::VrGunFile.get(), choices[i].file)))
			return (int)i;
	return 0;
}

bool stepper(int row) {
	return row == Gun || row == GunAngle || row == Reload || row == Haptics || row == WorldSize || row == PanelDistance;
}
// The rows a stick held left or right steps again and again (Input::repeat)
bool repeats(int row) {
	return row == GunAngle || row == WorldSize || row == PanelDistance;
}
int gunAngle() {
	return std::clamp((int)config::VrGunAngle, -GunAngleMax, GunAngleMax);
}
// (Hands too: either way, its other value)
bool onOff(int row) {
	return row == Hands || row == Laser || row == WiderView || row == HowToPlay;
}

std::string value(int row)
{
	char s[32];
	switch (row)
	{
	case Gun:
	{
		// a gun not made yet shows dimmed (only when vr.Gun names it: the row skips it)
		const std::vector<GunChoice> choices = gunChoices();
		const GunChoice& gun = choices[gunAt(choices)];
		return std::string(gun.label) + (!gun.made ? (gun.setting >= 4 ? " (not there)" : config::VrGameHands ? " (not made yet)" : " (game's models off)")
				: !gun.complete ? " (incomplete)" : "");
	}
	case GunAngle:
		// (with a sign, no degree sign: the panel adds it, the log stays ASCII)
		std::snprintf(s, sizeof(s), gunAngle() == 0 ? "%d" : "%+d", gunAngle());
		return s;
	case Reload:
		return ReloadNames[std::clamp((int)config::VrReload, 0, 1)];
	case Hands:
		return HandsNames[config::VrDualP1Left ? Left : Right];
	case Laser:
		return config::VrLaser ? "On" : "Off";
	case Haptics:
		return HapticsNames[hapticsLevel()];
	case WorldSize:
		std::snprintf(s, sizeof(s), "%.3f", (float)config::VrWorldScale);
		return s;
	case PanelDistance:
		std::snprintf(s, sizeof(s), "%.0f", (float)config::VrHudDepth);
		return s;
	case WiderView:
		return std::string(config::VrWiden ? "On" : "Off") + (config::VrWiden != widenAtStart ? " (next start)" : "");
	case HowToPlay:
		return howToPlayOn() ? "On" : "Off";
	default:
		return "";
	}
}

RowsPanel panel()
{
	RowsPanel p;
	p.title = "PAUSED";
	for (int row = 0; row < Rows; row++)
	{
		PanelRow r;
		r.label = row == Quit && quitArmed ? "Quit? Choose again" : Labels[row];
		r.value = value(row);
		if (row == GunAngle)
			r.value += "\xB0";	// (the degree sign: panel text is drawn byte by byte as Latin-1)
		r.selected = row == selected;
		r.stepper = stepper(row);
		if (row == Gun)
		{
			const std::vector<GunChoice> choices = gunChoices();
			r.dim = !choices[gunAt(choices)].made;
		}
		p.rows.push_back(r);
	}
	// (the font has no menu button sign)
	p.footer = "Point and pull the trigger, or stick and A.   Menu or B: back to the game";
	return p;
}

// A value row one step down (dir -1) or up (+1); an On/Off row the other way either way. Each
// change is saved at once; a change of the knock's strength is felt at once in the hand that made it.
void change(int row, int dir, int hand)
{
	switch (row)
	{
	case Gun:
	{
		// the next one each way, round and round, skipping those not made yet
		const std::vector<GunChoice> choices = gunChoices();
		const int n = (int)choices.size();
		if (n < 2)
			break;	// (locked: another game's own model)
		int at = gunAt(choices);
		for (int i = 0; i < n; i++)
		{
			at = (at + dir + n) % n;
			if (choices[at].made)
				break;
		}
		config::VrGun.set(choices[at].setting);
		config::saveInt("config", "vr.Gun", choices[at].setting);
		// a gun of the player's own by its file (one added before it in file-name order changes nothing)
		if (choices[at].file != nullptr)
		{
			config::VrGunFile.set(choices[at].file);
			config::saveStr("config", "vr.GunFile", choices[at].file);
		}
		break;
	}
	case GunAngle:
	{
		// 1 degree a step; at an end nothing (no save, no line)
		const int was = gunAngle();
		const int angle = std::clamp(was + dir, -GunAngleMax, GunAngleMax);
		if (angle == was)
			return;
		config::VrGunAngle.set(angle);
		config::saveInt("config", "vr.GunAngle", angle);
		gunsTurned = true;
		break;
	}
	case Reload:
	{
		const int was = std::clamp((int)config::VrReload, 0, 1);
		const int reload = std::clamp(was + dir, 0, 1);
		if (reload == was)
			return;
		config::VrReload.set(reload);
		config::saveInt("config", "vr.Reload", reload);
		break;
	}
	case Hands:
	{
		// with two players in, the guns swap hands once the menu closes (xr_host.cpp updateLightgun)
		config::VrDualP1Left.set(!config::VrDualP1Left);
		config::saveBool("config", "vr.DualP1Left", config::VrDualP1Left);
		break;
	}
	case Laser:
		config::VrLaser.set(!config::VrLaser);
		config::saveBool("config", "vr.Laser", config::VrLaser);
		break;
	case Haptics:
	{
		const int was = hapticsLevel();
		const int strength = std::clamp(was + dir, 0, 3);
		if (strength == was)
			return;
		config::VrHaptics.set(HapticsWords[strength]);
		config::saveStr("config", "vr.Haptics", HapticsWords[strength]);
		NOTICE_LOG(INPUT, "XR: menu %s -> %s", Labels[row], value(row).c_str());
		sampleKnock(hand);
		return;
	}
	case WorldSize:
	{
		const float was = config::VrWorldScale;
		const float scale = std::clamp(dir > 0 ? was * 1.1f : was / 1.1f, 0.005f, 0.2f);
		if (scale == was)
			return;
		config::VrWorldScale.set(scale);
		config::saveFloat("config", "vr.WorldScale", scale);
		break;
	}
	case PanelDistance:
	{
		const float was = config::VrHudDepth;
		const float depth = std::clamp(was + 5.f * dir, 15.f, 120.f);
		if (depth == was)
			return;
		config::VrHudDepth.set(depth);
		config::saveFloat("config", "vr.HudDepth", depth);
		break;
	}
	case WiderView:
		config::VrWiden.set(!config::VrWiden);
		config::saveBool("config", "vr.Widen", config::VrWiden);
		break;
	case HowToPlay:
		// on or off, the menu staying open (closing it turns it off: xr_host.h). The next frame the
		// hands swap gun and controller, and a ray moves by the muzzle's offset: that update learns
		// the rows again, so the selection stays on this row.
		NOTICE_LOG(INPUT, "XR: menu How to play");
		setHowToPlay(!howToPlayOn(), "chosen");
		fresh = true;
		return;
	default:
		return;
	}
	NOTICE_LOG(INPUT, "XR: menu %s -> %s", Labels[row], value(row).c_str());
	if (row == Gun)
		chosenGun();	// (its "XR: gun: ..." line at once)
}

// A row chosen (A, or a click away from a value row's halves): its action, or + on a value row.
void choose(int row, int hand)
{
	switch (row)
	{
	case Resume:
		NOTICE_LOG(INPUT, "XR: menu Resume");
		close();
		break;
	case Recenter:
		NOTICE_LOG(INPUT, "XR: menu Recenter");
		recenter();
		break;
	case Quit:
		if (quitArmed && seconds() - quitArmedAt <= QuitConfirmSeconds)
		{
			quitArmed = false;
			quitting = true;
			NOTICE_LOG(INPUT, "XR: menu quit");
			quitToStart();
		}
		else
		{
			quitArmed = true;
			quitArmedAt = seconds();
			NOTICE_LOG(INPUT, "XR: menu quit armed");
		}
		break;
	default:
		change(row, +1, hand);
		break;
	}
}

}	// namespace

bool isOpen()
{
	return menuPaused();
}

void place(const glm::mat4& head)
{
	const glm::vec3 eyes(head[3]);
	glm::vec3 forward = -glm::vec3(head[2]);
	// looking straight down (or up), the top of the head (or the chin) is where the face is turned
	if (std::abs(forward.x) + std::abs(forward.z) < 1e-3f)
		forward = glm::vec3(head[1]) * (forward.y < 0.f ? 1.f : -1.f);
	const glm::mat4 turn = glm::rotate(glm::mat4(1.f), std::atan2(-forward.x, -forward.z), glm::vec3(0.f, 1.f, 0.f));
	model = glm::translate(glm::mat4(1.f), eyes + glm::vec3(turn * glm::vec4(0.f, 0.f, -Distance, 0.f)) - glm::vec3(0.f, BelowEyes, 0.f)) * turn;
}

void open(const glm::mat4& head)
{
	place(head);
	selected = Resume;
	hovered[Left] = hovered[Right] = -1;
	pointing[Left] = pointing[Right] = false;
	fresh = true;
	quitArmed = quitting = false;
	// the player's own guns and controllers read again (xr_own.h): one put in the folder meanwhile is offered
	own::scan();
	const RowsLayout layout = rowsLayout(panel());
	NOTICE_LOG(INPUT, "XR: menu at %.2f m, %d rows, row pitch %.3f m, top row %+.3f m, %.2f m wide",
			Distance, (int)Rows, layout.rowPitch, layout.topRow, layout.width);
	requestPause(true);
}

void close()
{
	quitArmed = false;
	requestPause(false);
}

void reset()
{
	quitArmed = quitting = false;
}

void update(const Input& input)
{
	// (quitting: nothing more, whatever is pressed)
	if (!isOpen() || quitting)
		return;
	if (quitArmed && seconds() - quitArmedAt > QuitConfirmSeconds)
		quitArmed = false;
	if (input.toggle || input.back)
	{
		close();
		return;
	}
	const int before = selected;
	const RowsPanel rows = panel();
	int clicked[2] { -1, -1 };
	float clickedAt[2] { 0.5f, 0.5f };
	for (int hand : { Left, Right })
	{
		const Input::Ray& ray = input.ray[hand];
		float u = 0.5f;
		const int row = ray.valid ? rowAt(model, rows, ray.origin, ray.dir, &u) : -1;
		pointing[hand] = ray.valid && rowsPanelHit(model, rows, ray.origin, ray.dir, &pointAt[hand]);
		if (!fresh && !gunsTurned && row >= 0 && row != hovered[hand])
			selected = row;
		hovered[hand] = row;
		if (!fresh && ray.click && row >= 0)
		{
			clicked[hand] = row;
			clickedAt[hand] = u;
		}
	}
	fresh = false;
	gunsTurned = false;
	if (input.up)
		selected = (selected + Rows - 1) % Rows;
	if (input.down)
		selected = (selected + 1) % Rows;
	// another selection or a stick move takes Quit's confirmation back
	if (selected != before || input.up || input.down || input.left || input.right)
		quitArmed = false;
	for (int hand : { Left, Right })
	{
		const int row = clicked[hand];
		if (row < 0 || quitting)
			continue;
		if (row != selected)
			quitArmed = false;
		selected = row;
		NOTICE_LOG(INPUT, "XR: menu pointer click on %s at %.2f (%s hand)", Labels[row], clickedAt[hand], hand == Right ? "right" : "left");
		if (stepper(row))
			change(row, clickedAt[hand] < 0.5f ? -1 : +1, hand);
		else if (onOff(row))
			change(row, +1, hand);
		else
			choose(row, hand);
	}
	if (quitting)
		return;
	if ((input.left || input.right) && (stepper(selected) || onOff(selected)) && (!input.repeat || repeats(selected)))
		change(selected, input.left ? -1 : +1, Right);
	if (input.select)
		choose(selected, Right);
}

bool pointer(int hand, glm::vec3& at)
{
	if (!isOpen() || !pointing[hand])
		return false;
	at = pointAt[hand];
	return true;
}

void draw(const glm::mat4& viewProj)
{
	if (isOpen())
		drawRowsPanel(viewProj, model, panel());
}

}
