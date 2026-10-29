/*
	The agent's own hands and pistol in the headset (hotd2-vr), taken from the player's copy
	of the game: hands.bin in the data directory, made by the app (hands_rip.h) or on the PC
	by hotd2-vr/assets/rip_hands.py from a rip of the game over scene. In The Maze of the
	Kings the hero's staff in his fist, and his other glove, from staff-mok.bin. Each game
	has its own file; nothing of the game is in this source; without the file the built-in gun
	of xr_gun.h (v1's arcade gun) is drawn instead.

	Gun space is that of xr_gun.h (barrel along -z, y up, metres) with its origin in the fist
	around the grip. Hand space holds the open left hand: fingers along -z, palm towards +x,
	thumb up, origin just off the palm, where the controller's grip goes. Mirrored in x, both
	serve the other side. (The staff: its head along -z, origin on its axis in the fist; hand
	space is staff space mirrored, with the left glove a fist like the right one.)

	Copyright 2026 mikermak. This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#pragma once
#include <glm/glm.hpp>
#include <vector>

namespace vr::xr
{

struct HandsModel
{
	glm::vec3 muzzle;		// gun space
	glm::vec3 grab;			// gun space: the back of the slide, where the other hand takes it
	float travel;			// metres the slide goes back (+z); 0: no slide (the staff)
	glm::mat4 onSlide;		// hand space -> gun space: the open hand racking the slide (at rest)
	bool gunOnly = false;	// no hands (HND1 v2): the other hand keeps its controller
};

// The running game's model, loaded on first use (again once another game starts); nullptr
// without one. (Confidential Mission's guns are each their own: gunModel(); here the one
// chosenGun() gives.)
const HandsModel *handsModel();

// The guns a hand can hold in Confidential Mission: the in-game pistol and the assault rifle, each its
// own file made from the player's game (hands_rip.h; a kind's file is vr_reproject.cpp's guns[kind - 1],
// so the values stay as they were with the cabinet gun's 1, which no hand holds now); and the folder guns
// (xr_own.h: guns/*.glb, the bundled ones first, then the player's, each in file-name order) as
// GunKind(OwnGuns + i), chosen as vr.Gun 4 with vr.GunFile the file's name without .glb, each drawn as the
// game's guns are (gun only) and feeling like the pistol, only the chosen one read: the bundled namco.glb
// (ArcadeGunStem, the menu's "Arcade gun") and zapper.glb ("Zapper"), and any the player adds. Arcade:
// xr_gun.h's own (v1's arcade gun, always there), now only the hidden last resort, never on the menu.
enum class GunKind { Arcade = 0, Pistol = 2, Rifle = 3 };
constexpr int OwnGuns = 4;	// the first of the folder guns (GunKind and vr.Gun)
// The folder gun the hand falls back to without the in-game pistol: the bundled Arcade gun's file name
// (without .glb, any case)
constexpr const char *ArcadeGunStem = "namco";
// Its model, loaded on first use from its file (again once one is made or another game starts);
// nullptr if missing or unreadable, for the built-in gun, and in a game without these guns. A folder
// gun: read when chosen (chosenGun()), nullptr if not or skipped.
const HandsModel *gunModel(GunKind kind);
// All of it was in the game's frame (HND1 v2 flag 2); the built-in gun always.
bool gunComplete(GunKind kind);
// The gun vr.Gun gives: 1 the in-game pistol; 4 (and over) the folder gun vr.GunFile names (with
// vr.GameHands off too; read here, any other let go); any other setting (1.1's 0 Auto, 2 the cabinet gun,
// 3 the arcade gun, below 0) reads as 1, not rewritten. A folder gun not there ("<name> is not there")
// falls to the pistol; the pistol not there (not made yet, or vr.GameHands off; one made without all of
// it counts) to the Arcade gun's file (ArcadeGunStem); that one not loading either ("no gun file loads")
// to the built-in gun. PC: vr.BuiltInGun, the built-in gun whatever else (the harness's runs, which must
// never draw the bundled models). The built-in gun in a game without these guns. Logs "XR: gun: <gun>
// (chosen[: <why the fallback>])" at the start and at each change.
GunKind chosenGun();
// The gun in a player's (0, 1) hand: while the game says the player holds the assault rifle
// (vr::gunState's rifle) and the chosen gun is the in-game pistol with the rifle's model there, the
// rifle; else chosenGun() (any other gun stays in the hand and fires the rifle's rounds). Logs "XR: P1
// hand: assault rifle (gun-cm-rifle.bin)" then "XR: P1 hand: back to the <gun>"; or "XR: P1 hand: rifle
// rounds with the <gun>" then "XR: P1 hand: rifle rounds over (the <gun>)".
GunKind handGun(int player);
// "in-game pistol", "assault rifle", "built-in gun", a folder gun: its name ("Arcade gun", "Zapper", ...)
const char *gunName(GunKind kind);
// The model a hand holding `kind` draws, with vr.GameHands on: Confidential Mission's gun of
// that kind (gunModel(), none for the built-in gun), another game's own whatever the kind
// (handsModel()); nullptr: the built-in gun. The folder guns whatever vr.GameHands says.
const HandsModel *handModel(GunKind kind);

// vr.Gun's choices, in the menu's order, round and round: the in-game pistol, then every folder gun in
// own::guns()'s order (each vr.Gun 4 and its file for vr.GunFile), a skipped one too (not made: the row
// passes it). The built-in gun is none of them. The game's own models off: the pistol isn't made.
// Another game: one choice (its own model or the built-in gun), so the row can't change vr.Gun.
struct GunChoice
{
	int setting;		// vr.Gun
	const char *label;	// the menu's: "In-game pistol"
	bool made;			// its model is there: skipped if not
	bool complete;		// ...all of it (the menu says "(incomplete)" if not)
	const char *file = nullptr;	// a folder gun: its file name without .glb (vr.GunFile)
};
std::vector<GunChoice> gunChoices();

struct HandsView
{
	int player = 0;		// logical player identity (0 P1, 1 P2), independent of hand
	const HandsModel *model = nullptr;	// which: handsModel() or a gunModel() (handModel())
	glm::mat4 gunPose;		// gun space -> room space
	float slide = 0.f;		// metres the slide is back
	bool otherHand = false;	// draw the open hand...
	glm::mat4 handPose;		// ...hand space -> room space
	glm::vec4 muzzleLight { 0.f };	// room position, strength (the muzzle flash)
};

// Draws the view's model (the pistol in its hand, and the other hand) into the bound eye framebuffer.
void drawHands(const glm::mat4& viewProj, const glm::vec3& eyePos, const HandsView& view);
// The GL context is going away.
void termHands();

}
