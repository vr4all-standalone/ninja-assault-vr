/*
	The player's light gun in the headset (hotd2-vr): a Namco arcade gun in red plastic, or
	the agent's own pistol and hands from the game (xr_hands.h), held in the hand that
	shoots, with recoil, a muzzle flash and an optional aim line and dot. Drawn by
	xr_host.cpp into each eye after the game image.

	Room space here is the headset's local space relative to the game camera's origin
	(metres, y up), the same space the eye views are built in.

	Copyright 2026 mikermak. This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#pragma once
#include "xr_hands.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace vr::xr
{

// Gun space is the controller's aim pose: -z forward, y up, metres. Model space is
// gun_model.h as baked, in the same axes; gunPlacement() puts the model in the hand.

// The arcade gun's muzzle (the model's front lens), in model space. Shots and the aim line
// start at gunPlacement(...) * gunMuzzle(), along -z. Must match gun_model.h (checked when
// compiling xr_gun.cpp).
constexpr glm::vec3 GunMuzzle { 0.00000f, 0.04710f, -0.14010f };

// Where the hand holds the model: the middle of its grip, between the middle and ring
// fingers, in model space. (The bake's anchor sits lower on the grip than its comment
// said, which put the gun about 6 cm above the hand.)
constexpr glm::vec3 ModelPalm { 0.f, -0.018f, 0.062f };
// Where the hand is, in gun space, when the runtime can't say (xr_host.cpp asks for the
// controller's grip pose): Meta's grip -> aim offset for Touch controllers, (0, -0.0196,
// -0.1010) and -60 degrees about x, inverted. For the "old" aim pose (OpenXR before
// 1.1.49; this app asks for 1.0.34).
constexpr glm::vec3 HandPalm { 0.f, -0.078f, 0.068f };

// Whether a hand holding `kind` (handGun()) draws a model from the game (xr_hands.h
// handModel(): the agent's own pistol, in The Maze of the Kings the hero's staff, in
// Confidential Mission the gun of that kind) rather than the arcade gun. Model space is then
// gun space of xr_hands.h, with its origin in the fist (for a gun alone, gun only in HND1 v2,
// Confidential Mission's: the top of its grip, where the fist goes).
bool gameGun(GunKind kind);
// The muzzle in model space, of whichever pistol a hand holding `kind` draws.
glm::vec3 gunMuzzle(GunKind kind);

// Model space -> gun space: the model's grip on the hand (palm, gun space), at `scale`
// times the model's own size (arcade gun, vr.GunScale: 1 is 24.7 cm long, 17.1 cm tall;
// game pistol, vr.HandScale: 1 is 20 cm long, the staff 1 m). Mirrored in x for the left
// hand (the game's pistol is in a right hand).
inline glm::mat4 gunPlacement(bool game, float scale, const glm::vec3& palm = HandPalm, bool mirror = false)
{
	glm::mat4 m = glm::translate(glm::mat4(1.f), palm);
	m = glm::scale(m, glm::vec3(mirror ? -scale : scale, scale, scale));
	return game ? m : glm::translate(m, -ModelPalm);
}

struct GunView
{
	glm::mat4 pose { 1.f };		// model space -> room space: placement and recoil included
	float scale = 1.f;			// for the flash and smoke: 1 goes with the arcade gun at full size
	glm::vec3 restMuzzle { 0.f };	// room space, without recoil: where smoke leaves the barrel
	glm::vec3 restForward { 0.f, 0.f, -1.f };
	float trigger = 0.f;		// 0..1, how far the trigger is pulled
	double now = 0.0;			// seconds, any steady clock (smoke drifts with it)
	float sinceShot = 1e9f;		// seconds since the last shot
	unsigned shot = 0;			// counts shots: each looks a bit different
	bool menuPointer = false; // purple UI pointer; gameplay keeps its player color
	bool aimLine = false;		// draw the line from the muzzle to the aim point
	bool aimDot = false;		// draw the dot at the aim point
	bool aimOutside = false;	// ...greyed: the game can't hit there (outside its own view)
	glm::vec3 aimPoint { 0.f };	// room space
	int player = 0;				// whose gun: player 1's aim is red, player 2's blue
	GunKind kind = GunKind::Arcade;	// the gun in the hand (handGun()): the model, its muzzle and flash
	// The game's pistol only (gameGun(kind)):
	float slide = 0.f;			// metres its slide is back, in model space
	bool otherHand = false;		// the open hand on the other controller, or racking the slide...
	glm::mat4 handPose { 1.f };	// ...hand space (xr_hands.h) -> room space
};

// How hard the gun kicks back, sinceShot seconds after a shot: a sharp kick, a small
// bounce forward, settled after about a quarter second. 0..1, can go slightly negative.
float gunKick(float sinceShot);

// Draws the gun into the bound eye framebuffer, on top of the game image, in two passes:
// first every gun's model (the first clears the depth: the guns are in front of the game),
// then every gun's glow (aim line and dot, flash; the smoke of all guns with the first), so
// one gun's model never covers the other's dot or flash.
// viewProj: room space -> this eye's clip space; eyePos: the eye, in room space.
// Arcade colors follow gun.player (P1 red, P2 blue), including the baked fallback.
// tint: retained for caller compatibility; native and unrelated folder models keep their own colors.
void drawGunModel(const glm::mat4& viewProj, const glm::vec3& eyePos, const GunView& gun, bool clearDepth, const glm::vec3& tint);
void drawGunGlow(const glm::mat4& viewProj, const glm::vec3& eyePos, const GunView& gun, bool withSmoke);
// The GL context is going away.
void termGun();

// The gun model's lighting, GLSL for a fragment shader (after PixelCompatShader): a fixed key
// light and a dim fill from below, a plastic highlight and a rim, so a model in the hand reads
// as a solid object in the game's dark scenes. Also the controllers' (xr_controllers.h).
//   mediump vec3 modelLighting(highp vec3 n, highp vec3 v, mediump vec3 base, mediump float spec,
//           mediump float shine, mediump vec3 glow)
// n: the unit normal, turned to face the eye; v: the unit vector to the eye, both in room
// space; base: the colour; spec, shine: the highlight's strength and tightness; glow: added.
extern const char *const ModelLightingSource;

}
