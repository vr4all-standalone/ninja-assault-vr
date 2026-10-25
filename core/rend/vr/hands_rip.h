/*
	The game's own model in the player's hand, made on the spot from the player's own game
	(hotd2-vr): the agent's hands and pistol in The House of the Dead 2, the hero's staff in
	The Maze of the Kings. Each game keeps its own file, and never shows another game's.

	With no hands model yet, the first start of The House of the Dead 2 runs the game by
	itself, fast and silent, to the game over scene: Start through the title and Arcade mode,
	then no input, so the agent loses and kneels with his pistol in his hand. That frame is
	taken apart into the model (hands_build.h) and saved; the game goes on as usual. On the
	headset a panel says what's going on, and the gun's B skips it (the arcade gun then, and
	another go at the next start). Whenever the scene comes up in play, it's taken too.

	The Maze of the Kings goes the same way to its attract demo, with no input at all: the
	first frame with the hero and his staff, whole, in it (about half a minute of game).

	Confidential Mission draws its guns by themselves, with no hand on them (Source::CmGuns,
	gun-cm-*.bin, gun only): Start once at 30 s of game takes its SEGA logo to the reload
	tutorial, and from 2 s later Start every 2 s for 14 s takes it on to the stage 1 intro,
	where the agents hold their pistol, never whole for long: every pass with enough of it is
	captured, and 10 s of game after the last (or at 150 s) they're merged into the agents'
	pistol, flagged complete when all of it came together. (Up to 1.1 the tutorial's cabinet
	gun was made first and the presses waited for it; since 1.2 there is no cabinet gun.) It
	runs while the pistol's file is missing; B skips it here too, and nothing more is made that
	session (the assault rifle's part: hands_rip.cpp).

	Copyright 2026 mikermak. This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#pragma once
#include "hands_build.h"
#include <string>

struct rend_context;
struct RenderPass;

namespace vr::hands
{

// What a game's own model is made from (its profile, vr_reproject.cpp: gameModelSource()),
// and the file it's kept in, in the data directory.
struct Source
{
	enum Kind { None, AgentsPistol, HerosStaff, CmGuns, LupinGun, NinjaGun } kind = None;
	const char *file = "";
	Parts pistol {};		// AgentsPistol: the textures in the game over scene
	StaffParts staff {};	// HerosStaff: the textures in the attract demo and the story's intro
	// CmGuns: the guns, each its own file (unused: no file). guns[0] is none (1.1's cabinet gun's;
	// the slot stays so guns[kind - 1] lines up with xr_hands.h GunKind), guns[1] the agents' pistol.
	GunTarget guns[3] {};
	// LupinGun/NinjaGun: watch the neutral attract sequence until a complete gun appears.
	float prepSeconds = 180.f;
	bool attractGun() const { return kind == LupinGun || kind == NinjaGun; }
	bool gunModels() const { return kind == CmGuns || attractGun(); }
	// The file modelPath() names (CmGuns: the agents' pistol's, the one the run is for, for the
	// "made before" line; the headset reads each gun's own file: xr_hands.h gunModel())
	const char *modelFile() const { return gunModels() ? guns[1].file : file; }
	const char *name() const {
		return kind == NinjaGun ? "the Mach Gun" : kind == LupinGun ? "the in-game pistol" : kind == CmGuns ? "the guns" : kind == HerosStaff ? "the hero's staff" : "the agent's hands and pistol";
	}
};

// Where the running game's model is kept (the user data directory; none for a game without
// one), and where older builds put the agent's hands.
std::string modelPath();
std::string legacyModelPath();

// Every parsed render pass (vr::dropShotMarker): looks for the scene while there's no
// model, and makes it.
void observePass(const rend_context& ctx, const RenderPass& pass, const RenderPass& prev);

// The model to draw changed since the last call: one was just made, or another game started
// (the renderer loads it again then).
bool takeNewModel();

// The game is being run to the scene: no player input, a panel instead of the game.
bool preparing();
// How far along that is, 0..1 (a guess from the time the run usually takes).
float prepProgress();
// The player doesn't want to wait: the arcade gun this time.
void skipPrep();

}
