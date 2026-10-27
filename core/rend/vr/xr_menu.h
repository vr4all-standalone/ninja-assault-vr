/*
	The settings menu in the headset (hotd2-vr), on the left controller's menu button: the game
	pauses (xr_host.h, requestPause), its last picture stays around the player with head tracking,
	and a panel of rows (xr_panel.h) stands in front of them. Either hand points at a row and its
	trigger chooses (on a row with - and +, the left half is -, the right half +); the stick moves
	the selection (up and down, wrapping) and is - and + (left and right); A chooses (+ on a value
	row); the menu button or B closes it. Each change is saved in emu.cfg at once. How to play shows
	the controllers with their labels in both hands while the menu stays open (chosen again, or the
	menu closing, ends it; xr_host.h setHowToPlay). Quit asks to be chosen again within 3 s; once it
	quits, the menu takes nothing more.

	Copyright 2026 mikermak. This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#pragma once
#include <glm/glm.hpp>

namespace vr::xr::menu
{

enum Row { Resume, Recenter, Gun, GunAngle, Reload, Hands, Laser, Haptics, WorldSize, PanelDistance, WiderView, HowToPlay, ServiceMenu, Quit, Rows };

// What the controllers did in a headset frame. The buttons are edges (just pressed); the rays,
// one per hand (0 left, 1 right), are in the game camera's room space, with their trigger's click.
struct Input
{
	// repeat: left or right again from a stick held on the same push (0.4 s, then every 0.1 s); only
	// Gun angle, World size and Panel distance step on it (the Gun row loads a model each step)
	bool toggle, back, select, up, down, left, right, repeat;
	struct Ray
	{
		bool valid, click;
		glm::vec3 origin, dir;
	} ray[2];
};

// Shown: while the emulator is held paused for it (menuPaused).
bool isOpen();
// Opens it 1.2 m ahead of the head (`head`: head space -> room space; its yaw only), its middle
// 0.1 m below the eyes, at Resume, and asks for the pause (requestPause(true)).
void open(const glm::mat4& head);
// The panel in front of the head again (the game camera moved: a recenter).
void place(const glm::mat4& head);
// Asks for the game to go on (requestPause(false)).
void close();
// The emulator was started again elsewhere (a new game) and the menu let go of: Quit's
// confirmation and its latch are cleared.
void reset();
// A headset frame's input, render thread.
void update(const Input& input);
// Where a hand's ray meets the panel, as of the last update. False: it doesn't (or no menu).
bool pointer(int hand, glm::vec3& at);
// The panel, into the bound eye framebuffer; viewProj: room space -> the eye's clip space.
void draw(const glm::mat4& viewProj);

}
