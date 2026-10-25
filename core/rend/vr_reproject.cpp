/*
	VR reprojection prototype (hotd2-vr). See vr_reproject.h.

	Copyright 2026 mikermak. This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#include "vr_reproject.h"
#include "vr/xr_host.h"
#include "vr/hands_rip.h"
#include "transform_matrix.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/sh4/sh4_mem.h"
#include "stdclass.h"
#include "hw/maple/maple_cfg.h"
#include "cfg/option.h"
#include "emulator.h"
#include "log/Log.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iterator>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <array>
#include "rend/vr/gun_calibration.h"
#include "rend/vr/ninja_calibration.h"
#include "rend/vr/ninja_state.h"
#include "rend/vr/panel_units.h"

namespace vr
{

static ReprojectParams params;
static u64 frameCount;

// Near plane as a fraction of the closest vertex depth: vertices are never clipped by it
// in the identity reprojection, but anything behind the free camera is.
constexpr float NearFraction = 0.01f;

// Offsets of the x and y scale terms in a 4x4 float matrix (diagonal, either order).
constexpr u32 ScaleX = 0;
constexpr u32 ScaleY = 5 * 4;

static float readFloat(u32 ramOffset)
{
	u32 raw = ReadMem32_nommu(0x8C000000u + (ramOffset & 0x01FFFFFFu));
	float f;
	memcpy(&f, &raw, sizeof(f));
	return f;
}

static void writeFloat(u32 ramOffset, float f)
{
	u32 raw;
	memcpy(&raw, &f, sizeof(raw));
	WriteMem32_nommu(0x8C000000u + (ramOffset & 0x01FFFFFFu), raw);
}

static bool plausible(float v, float lo, float hi) {
	return std::isfinite(v) && std::abs(v) > lo && std::abs(v) < hi;
}

// A profile may expose its focal directly instead of a projection/viewport pair.
static bool readProfileFocal(glm::vec2& focal);

// The game's focal length in DC pixels, per axis: viewport scale times projection scale.
static glm::vec2 readGameFocal()
{
	if (config::VrProjAddr > 0 && config::VrViewportAddr > 0)
	{
		const glm::vec2 focal(
				std::abs(readFloat(config::VrViewportAddr + ScaleX) * readFloat(config::VrProjAddr + ScaleX)),
				std::abs(readFloat(config::VrViewportAddr + ScaleY) * readFloat(config::VrProjAddr + ScaleY)));
		if (plausible(focal.x, 10.f, 20000.f) && plausible(focal.y, 10.f, 20000.f))
			return focal;
	}
	glm::vec2 direct;
	if (readProfileFocal(direct))
		return direct;
	return glm::vec2(config::VrFocal);
}

//
// Widening the game's view, two ways.
//
// Best: the field of view the game builds its projection from. HOTD2 passes it as a
// 16-bit angle (65536 = a full turn) from a few literals in its code; raised there, the
// game itself draws and culls with the wider view (its light gun hit test keeps the stock
// view, measured on the headset). Checked every vblank, since the code is loaded after the
// game starts. See GameProfile::fovLiterals.
//
// Otherwise: scale down the viewport matrix's x/y terms by vr.FovScale. The game then
// maps (and culls against the screen) a wider cone into the same 640x480, but anything
// it computes for the screen on its own, like the light gun hit test, keeps the stock
// view. The projection itself can't be scaled: the game rebuilds it every frame. The
// viewport is set once per scene, so it is checked every vblank: a value differing from
// what we last wrote is fresh from the game and gets scaled; our own value is left alone.
//
static glm::vec2 lastWritten;
static u32 fovLiterals[4];		// RAM offsets of the game's field of view angle (0: none)
static u16 fovStock;			// the angle the game ships with

// The angle (65536 = full turn) that widens stock by `scale` (as tan of the half angle).
static u16 widenedAngle(u16 stock, float scale)
{
	const float half = stock * (3.14159265f / 65536.f);
	const float wide = 2.f * std::atan(std::tan(half) * scale);
	return (u16)std::lround(wide * (65536.f / 6.2831853f));
}

static bool widensByCode();

bool widensGameFov() {
	return fovLiterals[0] != 0 || widensByCode();
}

static void widenGameView(Event event, void *)
{
	if (event == Event::Start)
	{
		lastWritten = glm::vec2(0.f);
		return;
	}
	const float scale = config::VrFovScale;
	if (!config::VrReproject || scale <= 1.f)
		return;
	if (widensGameFov())
	{
		const u16 wide = widenedAngle(fovStock, scale);
		for (u32 offset : fovLiterals)
		{
			if (offset == 0)
				break;
			const u32 addr = 0x8C000000u + offset;
			// only the stock value: anything else isn't the code we know (not loaded yet)
			if (ReadMem16_nommu(addr) == fovStock)
			{
				WriteMem16_nommu(addr, wide);
				NOTICE_LOG(RENDERER, "VR: field of view %.1f -> %.1f degrees at %08x", fovStock * 360.f / 65536.f, wide * 360.f / 65536.f, addr);
			}
		}
		return;
	}
	// (not online: the "what we last wrote" below lives outside netplay's rollback state, so
	// after a rollback one side would scale the restored matrix a second time)
	if (config::VrViewportAddr <= 0 || widensByCode() || config::GGPOEnable)
		return;
	const u32 addr = config::VrViewportAddr;
	const glm::vec2 cur(readFloat(addr + ScaleX), readFloat(addr + ScaleY));
	if (cur == lastWritten || !plausible(cur.x, 10.f, 5000.f) || !plausible(cur.y, 10.f, 5000.f))
		return;
	lastWritten = cur / scale;
	writeFloat(addr + ScaleX, lastWritten.x);
	writeFloat(addr + ScaleY, lastWritten.y);
}

static struct WidenGameViewRegistration
{
	WidenGameViewRegistration() {
		EventManager::listen(Event::Start, widenGameView);
		EventManager::listen(Event::VBlank, widenGameView);
	}
} widenGameViewRegistration;

static void logSceneStats(const rend_context& ctx, glm::vec2 focal)
{
	std::vector<float> invW;
	invW.reserve(ctx.verts.size());
	size_t overlay = 0;
	for (const Vertex& v : ctx.verts)
		if (std::isfinite(v.z) && v.z > 0.f && std::isfinite(v.x))	// (drawn: not hidden)
		{
			invW.push_back(v.z);
			if (std::abs(1.f / v.z - config::VrHudW) < 0.002f)
				overlay++;
		}
	if (invW.empty())
		return;
	std::sort(invW.begin(), invW.end());
	auto w = [&](float q) { return 1.f / invW[(size_t)(q * (invW.size() - 1))]; };
	NOTICE_LOG(RENDERER, "VR: focal %.1f x %.1f  verts %zu overlay %zu  W near %.3f p10 %.3f median %.3f p90 %.3f far %.3f",
			focal.x, focal.y, invW.size(), overlay,
			w(1.f), w(0.9f), w(0.5f), w(0.1f), w(0.f));
}

//
// Per-game profiles, applied when the game starts so the Quest build needs no manual
// config. Values already set in the config file win.
//
struct CodePatch
{
	u32 offset;			// RAM offset of a 16-bit word of code
	u16 stock, patched;
};
struct CodeGuard
{
	u32 offset = 0;
	u16 expected = 0;
};
struct Poke
{
	u32 offset;			// RAM offset of a 16-bit word, written every frame
	u16 value;
};
struct ScreenBox
{
	float x0, y0, x1, y1;	// DC pixels
};
struct GameProfile
{
	const char *gameId = "";
	// The projection and viewport matrices in RAM: the game's live focal length.
	int projAddr = 0, viewportAddr = 0;
	// Some render libraries keep the focal itself in pixels (Lupin), with no normalized
	// projection matrix in RAM. Revision guards keep an unknown build from reading it.
	u32 liveFocalAddr = 0;
	CodeGuard focalGuards[4] {};
	float fovScale = 1.f;
	// Widening the game's own view: either the 16-bit angle literals its code builds its
	// projection from (scaled by fovScale, see widenGameView)...
	u32 fovLiterals[4] {};
	u16 fovStock = 0;
	// ...or code changed (every word stock, else none of them).
	CodePatch fovPatch[8] {};
	// The focal length the game's light-gun hit test uses (its stock view): a float in RAM
	// (it may change from scene to scene; or its reciprocal, 1/focal, if stockFocalInverse),
	// else vr.Focal. And the stock framing of the 2D plane (0: vr.Focal).
	u32 stockFocalAddr = 0;
	bool stockFocalInverse = false;
	// Original lens angle retained independently of a patched drawing projection.
	struct StockLens {
		u32 descriptors = 0, index = 0, width = 0;
		u16 stride = 0, angleOffset = 0, count = 0;
		bool pointerIndex = false;
		CodeGuard guards[12] {};
	} stockLens;
	float focal = 0.f;
	// Written every frame once the game's code is there (its fovPatch words stock or patched).
	Poke pokes[12] {};
	// The textures of the game's own hit marker (VRAM addresses) and the W it is drawn at:
	// see dropShotMarker.
	u32 markerTextures[4] {};
	float markerMinW = 0.85f, markerMaxW = 1.15f;
	ScreenBox markerUvBoxes[4] {};
	// Its marker and hit effects are drawn where its widened view puts the shot, not at the
	// gun: its hit test keeps the stock view (stockFocalAddr) while it draws the wider one.
	// See shotDrawnAt.
	bool markerWidened = false, markerBeforeSnap = false;
	// It draws no shot flash: vr.DropShotFlash never applies, so a bright full-screen 2D quad
	// just after a shot (its own fade, say) stays. See dropShotMarker.
	bool noShotFlash = false;
	// Player 2's prompt pictures and where they are: see hidePlayer2Prompt.
	u32 p2PromptTextures[4] {};
	ScreenBox p2PromptBox { 352.f, 384.f, 640.f, 480.f };
	glm::vec2 p2PromptMinSize { 0.f };
	u32 p2PromptIdleAddr = 0;
	// While the prompt shows, flat polygons on its line just after it go too (HOTD2's credit
	// count after it): off for a game where nothing follows it there. See hidePlayer2Prompt.
	bool p2HideTrailing = true;
	// 2D layers near the 2D plane (see snapNear2D): from and up to these W, at W in multiples
	// of this step; the W of the letterbox bars (0: the 2D plane's).
	float snapMinW = 0.75f, snapMaxW = 1.05f, snapStep = 0.01f;
	// Some libraries order sprites by a regular inverse-W grid rather than W.
	bool snapInverseDepth = false;
	float letterboxW = 0.f;
	// Textures that make a polygon at the letterbox's W one of its bars whatever its colour
	// (none: opaque black ones only). See hideLetterbox.
	u32 letterboxTextures[4] {};
	// 2D layers nearer than this W are snapped even partly or wholly off the screen (0: none);
	// those wholly off it are hidden, and axis-aligned quads cut to it (see cropPlaneToScreen).
	// Only below the game's near plane: none of its 3D comes that near.
	float offScreen2DMaxW = 0.f;
	// clipFarOut cuts a 3D polygon only when a corner beyond its reach is nearer than this W (a
	// pinned corner); 0: whatever the corner's W. A game that clips at its own near plane sets
	// this just under it: its far-out corners are real 3D beside the view. See clipFarOut.
	float farOutBelowW = 0.f;
	// The game's sights over its enemies, and the W they are drawn at: see placeSights.
	u32 sightTextures[8] {};
	float sightMinW = 0.f, sightMaxW = 0.f;
	// The W of its 2D plane (0: vr.HudW, HOTD2's 1.0). Never a W its 3D can have: the
	// headset's vertex shader pins every vertex near it onto the 2D plane.
	float hudW = 0.f;
	// Screens without any 3D (menus drawn in layers of depth): all on the 2D plane.
	bool flatScreens = false;
	// The size of its game units next to HOTD2's: vr.WorldScale (metres per HOTD2 unit, the
	// player's own setting) times this.
	float unitScale = 1.f;
	// The player's own model from the game, in a file of its own (hands_rip.h): the agent's
	// hands and pistol, the hero's staff, or none (the arcade gun).
	hands::Source model {};
	// Reusable VR gun choices can be available before this game has a captured pistol.
	bool gunMenu = false;
	bool ninja = false;
	CodeGuard revisionGuards[12] {};
	bool analogOffscreenReload = false, movieCutscenes = false, p2PromptSinglePlayerOnly = false;
	u32 calibrationAddr = 0, calibrationMode = 0;
	std::array<CodeGuard, 32> calibrationGuards {};
	// Each player's gun in RAM, for the knocks in the hand (gunState): the first player's
	// block, the step to the next player's, and the offsets in it of the rounds left, the
	// magazine's size (a byte each) and the shots fired (16 bits). base 0: none. The assault
	// rifle (Task 14): the offset of the weapon field (its low byte; -1: no rifle) and the
	// rifle's value there, held only while the player is in play: the player's bit (1 << p) in
	// the byte at `playing` (0: no such check).
	struct GunRam {
		u32 base = 0, stride = 0; u8 ammo = 0, magazine = 0, shots = 0;
		s16 weapon = -1; u8 rifleValue = 0; u32 playing = 0;
		// Float-valued VM variables instead of the byte/u16 block (original Lupin).
		bool floats = false; u32 session = 0, active = 0;
		int shotWaitMs = 100;
		u32 magazineAddr=0, combatAddr=0, ringAddr=0;
		CodeGuard guards[32] {};
	} gunRam;
};

static std::vector<GameProfile> makeProfiles()
{
	std::vector<GameProfile> list;
	GameProfile p;
	// The House of the Dead 2 (PAL). Its four perspective setups (0x8C029B48, 0x8C02B2C6,
	// 0x8C02B30A, 0x8C02B34E) load 41.1 degrees from two literals and call 0x8C0383C0.
	// Widened 3 times (tan of the half angle): 41.1 -> 96.7 degrees, 113 wide, about what a
	// Quest 3 eye shows (the game draws and culls with it, still at the headset's 72 Hz).
	// Its hit marker: a glow (0x59bb80, ~62 px) on the shot and rays (0x587380) flying out
	// ~100 px, drawn at W 0.9..1.01 (measured on the PC, chapter 1 and the attract demo).
	// Player 2's prompt in the bottom right corner: 0x57b700 and 0x56bb00 (chapter 1).
	p.gameId = "MK-5100250";
	p.projAddr = 0x4C65E0;
	p.viewportAddr = 0x4C6708;
	p.fovScale = 3.f;
	p.fovLiterals[0] = 0x029C12;
	p.fovLiterals[1] = 0x02B370;
	p.fovStock = 7484;
	p.markerTextures[0] = 0x59bb80;
	p.markerTextures[1] = 0x587380;
	p.p2PromptTextures[0] = 0x57b700;
	p.p2PromptTextures[1] = 0x56bb00;
	// The agent's parts in the game over scene: his hands, the cuff, the pistol.
	p.model.kind = hands::Source::AgentsPistol;
	p.model.file = "hands.bin";
	p.model.pistol = { { 0x5fa380, 0x5fc380 }, 0x601380, 0x5fe380 };
	list.push_back(p);
	// The House of the Dead 2 (USA): the same code and data as PAL, elsewhere (found by
	// comparing RAM and VRAM dumps of both at the same moments: code 0x280 lower, the matrices
	// 0x680 lower, the setups at 0x8C0298C4, 0x8C02B042, 0x8C02B086 and 0x8C02B0CA calling
	// perspective() at 0x8C038140; every texture an exact byte match of PAL's).
	p.gameId = "MK-51002";
	p.projAddr = 0x4C5F60;
	p.viewportAddr = 0x4C6088;
	p.fovLiterals[0] = 0x02998E;
	p.fovLiterals[1] = 0x02B0EC;
	p.markerTextures[0] = 0x59e000;
	p.markerTextures[1] = 0x589000;
	p.p2PromptTextures[0] = 0x53c400;
	p.p2PromptTextures[1] = 0x572000;
	p.model.pistol = { { 0x5fc800, 0x5fe800 }, 0x603800, 0x600800 };	// (the same hands.bin as PAL's)
	list.push_back(p);
	// The Maze of the Kings (NAOMI, Hitmaker 2002; worked out from rips, RAM dumps and its
	// code, see hotd2-vr/README). Its field of view changes from scene to scene (60 degrees
	// in the maze, 40 in the story, 50 in menus, 80 in the attract) and comes from camera
	// data, not code: one angle A, set through setPerspective 0x8C074630 / setFovAngle
	// 0x8C0746B0, which keep the stock focal 240/tan(A/2) at 0x0C0E7248 (what the light-gun
	// hit test unprojects with) and call the projection builder 0x8C08D8D0. Its prologue
	// is changed to take 0.75A as the half angle instead of A/2 (mov r4,r13; shlr2 r4;
	// sub r4,r13 in place of a dead stack store, its reload and shar r13): the projection,
	// culling planes and screen matrices all 1.5 times as wide (the maze 90 degrees). The
	// matrices: projection 0x0C0E7A60 (f/320, f/240), viewport 0x0C0F24D0 (320, -240).
	// It culls models by their bounding spheres against its own frame (0x8C084BB0 and the
	// node loops at 0x8C0912A0, 0x8C0923C2, 0x8C09916E), with a margin both builders make
	// from a 0.5 at 0x8C08DC4C (0x0C0F2550/54, read only there): made 4.0, a sphere counts
	// eight times its size, so the people and things beside the camera in the story scenes,
	// in view when you turn your head, are drawn too (the game's own frame stays the same;
	// about a third more polygons in the story).
	p = GameProfile();
	p.gameId = "THE MAZE OF THE KINGS";
	p.projAddr = 0x0E7A60;
	p.viewportAddr = 0x0F24D0;
	p.fovScale = 2.f;	// (only says "widen": the patch's own factor is fixed)
	p.fovPatch[0] = { 0x08D8EE, 0x1F41, 0x6D43 };
	p.fovPatch[1] = { 0x08D8F2, 0x5DF1, 0x4409 };
	p.fovPatch[2] = { 0x08D8F6, 0x4D21, 0x3D48 };
	p.fovPatch[3] = { 0x08DC4E, 0x3F00, 0x4080 };
	p.stockFocalAddr = 0x0E7248;
	p.focal = 415.72f;	// the maze's
	// The gun calibration (its gun settings screen; factory 64, 128, 500, 0, 128, 500 for
	// each player at 0x0C0E2058 and 0x0C0E2064) stretches the gun's position outward from
	// the centre, up to 30 px off. Kept at 0, 0, 512, 0, 0, 512, the game's aim is where the
	// gun points (within a pixel).
	const u16 calibration[6] { 0, 0, 512, 0, 0, 512 };
	for (int player = 0; player < 2; player++)
		for (int k = 0; k < 6; k++)
			p.pokes[player * 6 + k] = { (u32)(0x0E2058 + player * 12 + k * 2), calibration[k] };
	// Its shot flash: a starburst (0x50b800) and red stars (0x4f6800) at W 1.5.
	p.markerTextures[0] = 0x50b800;
	p.markerTextures[1] = 0x4f6800;
	p.markerMinW = 1.45f;
	p.markerMaxW = 1.55f;
	// Player 2's start prompt at the bottom centre (the same sheets as FREE PLAY under it and
	// player 1's prompt, which are wider and lower).
	p.p2PromptTextures[0] = 0x68d800;
	p.p2PromptTextures[1] = 0x48c000;
	p.p2PromptBox = { 250.f, 440.f, 390.f, 462.f };
	// The HUD at W 1.082, subtitles 1.040, area names 1.052-1.054, the continue prompt
	// 1.08-1.085, the damage flash 1.4 and its shot effects 1.5, in steps of 0.001; letterbox
	// bars at 1.090.
	p.snapMaxW = 1.6f;
	p.snapStep = 0.001f;
	p.letterboxW = 1.090f;
	// Its near plane is at W 1.0 exactly (the floor under the camera, the walls beside it,
	// the desert's ground and sky domes are clipped there, 20-340 corners a frame): with the
	// 2D plane at 1.0 those corners were pinned onto it, streaks over the floor. Its HUD's W
	// instead; FREE PLAY at 1.000 is snapped like the other layers.
	p.hudW = 1.082f;
	p.flatScreens = true;
	// (Its units are HOTD2's size: the camera rides about 14.5 units over the floor in the
	// maze, HOTD2's 15 over the street.)
	// The hero's staff, in his right fist, as the attract demo shows it from 32 s on (and
	// the story's intro): the rod's texture and the hero's body's (his gloves are on it), in
	// VRAM where each of the two scenes puts them. (The heroine's body is another texture.)
	p.model.kind = hands::Source::HerosStaff;
	p.model.file = "staff-mok.bin";
	p.model.staff = { { { 0xC1C000, 0xCB0800 }, { 0xD87000, 0xDA9800 } } };
	list.push_back(p);
	// Confidential Mission (NAOMI GD-ROM, MAME confmiss). Its boot ID starts with a space.
	// Its field of view changes from shot to shot (the camera zooms: a focal of about 540 in
	// stage 1, 450-1190 in its intro, 650-1330 in the attract demo). The matrices, found in
	// RAM dumps of all three: projection 0x0C24F660 (f/320, f/240; the same again at
	// 0x0C24F8C0), viewport 0x0C24F798 (320, -240).
	p = GameProfile();
	p.gameId = " CONFIDENTIAL MISSION ---------";
	p.gunMenu = true;
	p.projAddr = 0x24F660;
	p.viewportAddr = 0x24F798;
	// Its camera keeps the field of view as a float, horizontal degrees, at 0x0C154EC8. Each
	// frame play's camera (0x8C02734E) turns it into a 16-bit angle, degrees times 65536.0
	// over 360 (the 65536.0 a literal, 0x8C0274B4), for the projection builder 0x8C09C2F0, and
	// keeps 1/focal = tan(A/2)/320 at 0x0C154ED0 (the stock view: the game's own screen checks
	// project with it, as at 0x8C02AE0A). Two more copies of that code (0x8C05ACC0, literal
	// 0x8C05AEC0; 0x8C05D068, literal 0x8C05D260, half the angle, for the builder 0x8C098A40)
	// were not seen running. Those literals are made 118784.0 (high half 4780 -> 47E8): the
	// angle 1.8125 times as wide, stage 1's 61 degrees across (48 high) 111 (95 high). The
	// cutscene cameras (0x8C041D4E, 0x8C049B24's) build theirs from a lens angle instead, as
	// 2 atan(32 tan(L/2) / 18): both 32.0 literals (0x8C041EB8, 0x8C049BDC) are made 64.0 (high
	// half 4200 -> 4280), the tangent twice as wide, the intro's 70 degrees across (55 high)
	// 109 (92 high); there they also widen 0x0C154EC8 and 0x0C154ED0. The builder makes the
	// projection, frustum planes and screen matrices from the angle, so the game draws and
	// culls the wider view, while 0x0C154ED0 stays stock in play. The code reading three of
	// the literals (unchanged) makes sure the patch lands only on it.
	p.fovScale = 2.f;	// (only says "widen": the patch's own factors are fixed)
	p.fovPatch[0] = { 0x0274B6, 0x4780, 0x47E8 };	// high half of the float 65536.0 -> 118784.0
	p.fovPatch[1] = { 0x05AEC2, 0x4780, 0x47E8 };	// the same, the second copy's
	p.fovPatch[2] = { 0x05D262, 0x4780, 0x47E8 };	// the same, the third copy's
	p.fovPatch[3] = { 0x041EBA, 0x4200, 0x4280 };	// high half of the float 32.0 -> 64.0
	p.fovPatch[4] = { 0x049BDE, 0x4200, 0x4280 };	// the same, the other cutscene camera's
	p.fovPatch[5] = { 0x02738E, 0xC749, 0xC749 };	// mova 0x8C0274B4,r0 (unchanged)
	p.fovPatch[6] = { 0x05D06E, 0xC77C, 0xC77C };	// mova 0x8C05D260,r0 (unchanged)
	p.fovPatch[7] = { 0x041D68, 0xC753, 0xC753 };	// mova 0x8C041EB8,r0 (unchanged)
	p.stockFocalAddr = 0x154ED0;
	p.stockFocalInverse = true;
	p.focal = 540.8f;	// stage 1's (537.4-542.6: its camera sways)
	// Its 2D comes in layers from W 0.0001 (a sprite at 192,272, tex 4df800, which
	// place2DLayers puts on the plane) to 10, its 3D from its near plane at W 0.300 on: the
	// screen crack when the player is hit at 1/128, the continue prompt 0.050-0.061, credits,
	// subtitles and the attract demo's caption 0.1-0.11, the cutscenes' bars at 0.2, the
	// briefing 0.25, HUD pictures 0.5-0.6, the gun adjustment screen 0.8-0.9, the magazines
	// and the shot marker at 1.000, the how-to-play panels up to 10. So the plane stays at
	// 1.0, and every layer from 0.005 to 10 at a multiple of 1/16000 (0.001's and 1/128's) is
	// snapped onto it. (At W 10 that test is thin: w/step is 160000 there, a float's step
	// 0.0156, against its 0.02.)
	p.hudW = 1.f;
	p.snapMinW = 0.005f;
	p.snapMaxW = 10.f;
	p.snapStep = 1.f / 16000.f;
	// The caption ticker's letters (tex 78b800, W 0.1) run from x -492 to 1256 across the
	// attract demo: below the near plane, snapped wherever they are and cut to the screen.
	p.offScreen2DMaxW = 0.3f;
	// It clips its 3D at its own near plane, W 0.300, so its corners far outside the screen are
	// real 3D (its floor down to 88.6 degrees below the camera's axis): clipFarOut leaves them.
	p.farOutBelowW = 0.299f;
	// The cutscenes' bars at W 0.2: white, with a texture (400680) that draws them black.
	p.letterboxW = 0.2f;
	p.letterboxTextures[0] = 0x400680;
	// Its sights, a ring and red X marks over each enemy about to fire, tex 400000: the X
	// marks at W 0.980-0.998 (OP), the rings at 0.99995-1.00003 (TR). The texture's other
	// polygons are 3D at W 9 and more (shadows, lights) or the how-to-play gun model at
	// 0.49-0.51 (every rip of the attract demo, intro and stage 1).
	p.sightTextures[0] = 0x400000;
	p.sightMinW = 0.95f;
	p.sightMaxW = 1.005f;
	// The gun calibration: 6 bytes per player at 0x0C15A9C0 and 0x0C15A9C6 (the centre's x and
	// y, the right and left edges', then the y edges'; factory 128, 128, 248, 8, 8, 248),
	// which the game copies into each player's gun state (0x0C155844, 0x0C155958) at a game's
	// start and turns into the scales 312/(right-centre), 312/(centre-left), 232/(centre-y2),
	// 232/(y1-centre). Its adjustment screen moves the centre to an on-target shot. Kept at
	// 128, 128, 251, 1, 2, 250, the game's aim is where the gun points.
	const u16 gunCalibration[3] { 0x8080, 0x01fb, 0xfa02 };
	for (int player = 0; player < 2; player++)
		for (int k = 0; k < 3; k++)
			p.pokes[player * 3 + k] = { (u32)(0x15A9C0 + player * 6 + k * 2), gunCalibration[k] };
	// Its marker and impacts are drawn where its widened view puts the shot (its hit test
	// keeps the stock focal, 0x0C154ED0).
	p.markerWidened = true;
	// Its shot marker: a burst of 12 triangles, tex 5e8000 (and 5e7800, the census's other
	// marker texture at W 1.000), flat at W 1.000: inside the generic 0.85-1.15 band, so no
	// markerMinW/MaxW.
	p.markerTextures[0] = 0x5e8000;
	p.markerTextures[1] = 0x5e7800;
	// It draws no shot flash. In every run (t5-w1, t5-d, t5f-hall, t5f-hall2, t5f-room1,
	// t5-g2-off) the only bright full-screen all-2D quad after a shot is its own cross-fade,
	// tex 404800: the previous view drawn over the new one with white vertex colour (which is
	// what the rule's bright test goes by), 640x480, alpha fa -> 04 over ~1 s, at the same
	// point on the route (t5-w1 118.8 s, with vr.DropShotFlash off: 61 lines, none hidden),
	// starting 175-192 ms after the shot before it. With the rule on it was hidden in
	// alternating ~200 ms blocks (t5f-hall2: 28 of its 60 frames), the full-screen cross-fade
	// strobing at ~2.5 Hz while the player fires. So the rule is off for it.
	p.noShotFlash = true;
	// Player 2's start prompt, blinking at the bottom right all through a one-player game and
	// again once player 2 is out: tex 4ce800, drawn at W 0.1 (the W before snapNear2D puts it
	// on the 2D plane, where hidePlayer2Prompt finds it), 406,431..552,448 with its shadow at
	// 0.11; the sheet of FREE PLAY beside it (248,440..392,456) and of player 1's own prompt
	// in its continue (86,431..232,448), which the box leaves out, as it does player 2's HUD
	// (no prompt seen while player 2 played). From t6-rip, rips of a one-player game.
	p.p2PromptTextures[0] = 0x4ce800;
	p.p2PromptBox = { 400.f, 424.f, 560.f, 452.f };
	// Nothing follows the prompt on its line in free play, and the trailing pass took flat 2D
	// beside it: the lower half of the widening-off shot marker at 576,432 (accept-cm-off).
	p.p2HideTrailing = false;
	// Its logo, advisory and game-over screens: layers up to W 100 without 3D.
	p.flatScreens = true;
	// (Its units are HOTD2's size: a guard stands about 17.5 units tall, and the camera rides
	// 15.1 units over the floor in the attract demo's corridor, HOTD2's 15 over the street;
	// 12.0 in stage 1's first room, looking 10 degrees down.)
	// Each player's gun state (the game research's B2: 61 RAM rips set against the HUD's
	// bullets; the findings' Rifle, from the game's code): player 1's block at 0x0C1557B8,
	// player 2's 276 bytes on (0x0C1558CC). +0x4C the rounds left (the pistol's 6 down to 0, a
	// reload fills it again), +0x50 the magazine's size (6 for the pistol, 45 for the rifle),
	// both u32 whose low byte is read; +0x60 the shots fired, a u16 that goes up once per shot
	// the game fires, through reloads, but not for a pull on an empty gun. +0x48 the weapon:
	// 2 the assault rifle (findings' Rifle, profile:; only an item's pickup sets it, and the
	// game gives the pistol back at 0 rounds). A poked or stale 2 outlives the continue countdown
	// and game over (4b), so it counts only while the player's bit is set in 0x0C15A494 (bit 0
	// player 1, bit 1 player 2: set when the player starts or joins, cleared when the player goes
	// out; Task 14's t14-play: 1 from P1's start, 0 at P1's out, 2 at P2's join).
	p.gunRam = { 0x0C1557B8, 276, 0x4C, 0x50, 0x60, 0x48, 2, 0x0C15A494 };
	// Its guns, from the player's own game (hands_rip.h). guns[0] none: up to 1.1 the red cabinet
	// gun of the reload tutorial (tex 400000, 279 polygons, gun-cm-cabinet.bin), which no menu
	// offers since CM VR 1.2, so it isn't made and an old file isn't read; the slot stays so
	// guns[kind - 1] lines up with GunKind (xr_hands.h).
	p.model.kind = hands::Source::CmGuns;
	p.model.guns[0] = {};
	// The agents' pistol (Task 11; A1 candidate 1, A2): a suppressed black automatic in the stage
	// 1 intro, never drawn whole for long (small, run off-screen, the game culls parts), so the
	// passes it's in are merged. OP list; body (slide, frame and grip) tex 7e2800, its suppressor
	// 6cc400, two more parts 82f800 and 76a800: 91 polygons, 78/10/2/1; the body has 30 open edges
	// of 282 (seams, open ends) and extents 1.92 x 1.46 x 0.26. Howard's hand (tex 6ca000) on its
	// grip is not part of it. 0.30 m along the barrel and the suppressor.
	p.model.guns[1] = { "agents' pistol", "gun-cm-pistol.bin", { 0x7e2800, 0x6cc400, 0x82f800, 0x76a800 }, 91, 10, 0.30f, true,
			{ 78, 10, 2, 1 }, 30, { 1.92f, 1.46f, 0.26f }, 0.10f };
	// The assault rifle (Task 12; findings Rifle, model: and profile:): the item an enemy drops (kind
	// 4, model 0c32bb20), one mesh, tex 662000, 55 polygons, drawn whole whenever it's drawn. The
	// attract demo drops one in its train-car scene at about 316 s of game with no presses from boot,
	// so the run waits for it there first (to 340 s); in play it's watched for (nothing shows it while
	// it's held). 0.88 m, an AK's length (the model's own scale wasn't measured).
	p.model.guns[2] = { "assault rifle", "gun-cm-rifle.bin", { 0x662000 }, 55, 55, 0.88f, false, {}, 0, {}, 0, 340.f };
	list.push_back(p);
	// Lupin the Third: The Shooting, original GDS-0018 (MAME lupinshoo). Unlike CM,
	// its rendering library stores the pixel focal directly: the setter at 0x8C075E40
	// copies the screen descriptor's focal and centre into 0x0CBD6120 (f, width, height,
	// cx, cy). RAM captures flat-attract-original, flat-play-original and flat-route-original
	// show 320 during setup, 2408.04 in one attract camera and 879.4259 in the train scene:
	// the latter is 320/tan(7280*pi/65536), a horizontal field of view of 39.9902 degrees.
	// The guards are the setter's four instructions, verified in the original disc's RAM;
	// they deliberately do not imply support for Rev A. See notes/lupin-findings.md.
	p = GameProfile();
	p.gameId = "LUPIN THE THIRD  -THE SHOOTING-";
	p.gunMenu = true;
	p.liveFocalAddr = 0xBD6120;
	p.focalGuards[0] = { 0x075E40, 0xE00C };
	p.focalGuards[1] = { 0x075E42, 0xD504 };
	p.focalGuards[2] = { 0x075E44, 0xF348 };
	p.focalGuards[3] = { 0x075E46, 0xF53A };
	// The library's perspective setter takes a full-turn angle in r4 and halves it in
	// the tangent call's delay slot. Keep the full angle instead (40 -> 80 degrees),
	// affecting the library's projection and frustum together. Guard the call and target
	// too, so the write cannot land on a different revision. The live focal remains the
	// drawing frame. Causal head/body hits prove the independent stock descriptor
	// below remains the light-gun hit-test frame. Original GDS-0018 only.
	p.fovScale = 2.f;
	p.fovPatch[0] = { 0x075E1E, 0x4421, 0x0009 };
	p.fovPatch[1] = { 0x075E18, 0x4F22, 0x4F22 };
	p.fovPatch[2] = { 0x075E1A, 0xD306, 0xD306 };
	p.fovPatch[3] = { 0x075E1C, 0x430B, 0x430B };
	p.fovPatch[4] = { 0x075E34, 0xEDAC, 0xEDAC };
	p.fovPatch[5] = { 0x075E36, 0x8C08, 0x8C08 };
	p.stockLens.descriptors = 0x9A58FC; p.stockLens.index = 0x9A6994;
	p.stockLens.width = 0xBD6124; p.stockLens.stride = 248; p.stockLens.angleOffset = 92; p.stockLens.count = 4;
	p.stockLens.guards[0] = { 0x024268, 0xE304 };
	p.stockLens.guards[1] = { 0x02426A, 0x644D };
	p.stockLens.guards[2] = { 0x02426C, 0x3433 };
	p.stockLens.guards[3] = { 0x02426E, 0x8F19 };
	p.stockLens.guards[4] = { 0x024380, 0x00F8 };
	p.stockLens.guards[5] = { 0x024388, 0x58FC };
	p.stockLens.guards[6] = { 0x02438A, 0x8C9A };
	p.stockLens.guards[7] = { 0x0243B4, 0x6994 };
	p.stockLens.guards[8] = { 0x0243B6, 0x8C9A };
	p.stockLens.guards[9] = { 0x024400, 0xE05C };
	p.stockLens.guards[10] = { 0x024402, 0x03EE };
	p.focal = 879.4259f;
	// UI depths measured across title, train, ranking and failure screens: inverse W
	// is an integer multiple of 1/300 (W 0.506757..0.681818). The train's real 3D is
	// beyond W 16. Keep that whole UI band on a distinct plane; off-screen ticker
	// glyphs use the same depths and are cropped back to the original game screen.
	p.hudW = 0.6f;
	p.snapMinW = 0.49f;
	p.snapMaxW = 0.75f;
	p.snapStep = 1.f / 300.f;
	p.snapInverseDepth = true;
	p.offScreen2DMaxW = 0.75f;
	p.flatScreens = true;
	// Standalone handgun held by the train guards. Each rigid instance has 24 strips;
	// its three pieces are <0.15 units apart, other instances >10 units away. Select
	// one whole instance, including detached details, rather than fusing every gun in
	// the scene. Cache names and preparation are private to this game's Source.
	p.calibrationAddr = 0xB67DA4; p.calibrationMode = 0x991CC8;
	p.calibrationGuards = {{
		{ 0x036F44, 0xD23A }, { 0x036F54, 0x84E4 }, { 0x036F58, 0x84E1 }, { 0x036F5C, 0xB025 },
		{ 0x036F84, 0xD22A }, { 0x036F94, 0x84E5 }, { 0x036F98, 0x84E3 }, { 0x036F9C, 0x84E2 }, { 0x036F9E, 0xB004 },
		{ 0x036FAA, 0x2FE6 }, { 0x037024, 640 }, { 0x037026, 320 }, { 0x037028, 480 }, { 0x03702A, 240 },
		{ 0x037030, 0x7DA4 }, { 0x037032, 0x8CB6 },
		{ 0x037258, 0x6344 }, { 0x03725E, 0x6244 }, { 0x037260, 0x4318 }, { 0x037262, 0x332C }, { 0x037264, 0x2531 },
		{ 0x0372C8, 0x82AC }, { 0x0372CA, 0x8CB6 }
	}};
	p.p2PromptTextures[0] = 0x92d800;
	p.p2PromptBox = { 437.5f, 440.5f, 617.5f, 464.5f };
	p.p2PromptMinSize = { 8.f, 8.f };
	p.p2HideTrailing = false;
	p.p2PromptSinglePlayerOnly = true;
	p.p2PromptIdleAddr = 0xA4C74C; // 0 out, 1 active, 2 continue: keep continue glyphs
	p.analogOffscreenReload = true;
	p.movieCutscenes = true;
	p.noShotFlash = true; // no CRT/UI flash in sampled analog-gun bursts; retain movie/fade layers
	// Shared atlas: only the original crack depth, before HUD snapping, is a marker.
	// Native red/blue cracks remain at the unscaled cabinet coordinates even though
	// the world hit test uses the stock lens. World hit stars stay untouched.
	p.markerTextures[0] = 0x92d800;
	p.markerMinW = 0.52630f; p.markerMaxW = 0.52633f;
	p.markerBeforeSnap = true;
	p.markerUvBoxes[0] = { .25f, 0.f, .5f, .375f };
	p.markerUvBoxes[1] = { .5f, .5f, .5625f, .5625f };
	p.markerUvBoxes[2] = { .875f, .375f, 1.f, .5f };
	// The library clips real 3D at W=1: the 19 far-out near-plane polygons in
	// route0001 remain planar to <6e-7 of their extent. Preserve these floors/walls.
	p.farOutBelowW = 0.999f;
	// Float VM variables, measured with individual shots, empty pulls and each reload.
	// P1 has 7 rounds, P2 6; counters rise only when a round fires. Session and active
	// gates exclude attract shooting, boot noise and continue countdowns.
	p.gunRam.base = 0xA4C808; p.gunRam.stride = 4;
	p.gunRam.ammo = 0; p.gunRam.magazine = 16; p.gunRam.shots = 32;
	p.gunRam.shotWaitMs = 200; // measured accepted input -> VM counter latency up to 168 ms
	p.gunRam.floats = true; p.gunRam.session = 0xA4C728; p.gunRam.active = 0xA4C738;
	p.gunRam.guards[0] = { 0x0281EE, 0x930A };
	p.gunRam.guards[1] = { 0x0281F0, 0xD012 };
	p.gunRam.guards[2] = { 0x0281F2, 0x2439 };
	p.gunRam.guards[3] = { 0x028206, 0x07FF };
	p.gunRam.guards[4] = { 0x02823C, 0xC300 };
	p.gunRam.guards[5] = { 0x02823E, 0x8CA4 };
	p.model.kind = hands::Source::LupinGun;
	p.model.prepSeconds = 180.f;
	p.model.guns[1] = { "in-game pistol", "gun-lp-pistol.bin", { 0xb80800 }, 24, 24, 0.22f, true,
			{ 24 }, 12, { 1.f, 0.52442f, 0.08799f }, 0.05f, 0.f, 0.2f };
	list.push_back(p);

 // Ninja Assault, World NJA2 Ver.A. Private baseline and freeplay captures.
 p = GameProfile();
 p.gameId = "NINJA ASSAULT"; p.ninja = true; p.gunMenu = true;
 // The heroes' complete wood-and-metal Mach Gun in the neutral story sequence.
 // Texture0x4af000,64 strips/203 nondegenerate triangles per instance. Two heroes
 // share it;10 game units joins each gun's pieces without joining the two guns.
 // The stock and wide private captures produce the same complete gun. No game
 // data is supplied: preparation watches the user's own attract sequence.
 p.model.kind = hands::Source::NinjaGun;
 p.model.prepSeconds = 150.f;
 p.model.guns[1] = { "Mach Gun", "gun-na-mach.bin", { 0x4af000 }, 64, 64, 0.40f, false,
                    {}, 0, {}, 0.f, 0.f, 10.f };
 p.projAddr = 0x0E0600; p.viewportAddr = 0x0E0728;
 p.focal = 718.744f; p.fovScale = 2.f;
 // Main perspective setter: full angle in r4, halved at 08F8C8 before sin/tan.
 // Keep the full angle for drawing/frustum; the camera record remains original.
 p.fovPatch[0] = { 0x08F8C8, 0x4D21, 0x0009 };
 p.fovPatch[1] = { 0x08F8A0, 0x2FE6, 0x2FE6 };
 p.fovPatch[2] = { 0x08F8C4, 0x5DF1, 0x5DF1 };
 p.fovPatch[3] = { 0x08F8CA, 0x4B0B, 0x4B0B };
 p.fovPatch[4] = { 0x08F9D4, 0xC560, 0xC560 };
 p.fovPatch[5] = { 0x08F9D6, 0x0C08, 0x0C08 };
 p.stockLens.descriptors = 0x385818; p.stockLens.index = 0x3860F4;
 p.stockLens.pointerIndex = true; p.stockLens.width = 0x0AD0A8;
 p.stockLens.stride = 252; p.stockLens.angleOffset = 236; p.stockLens.count = 9;
 p.stockLens.guards[0] = { 0x10685A, 0x910B };
 p.stockLens.guards[1] = { 0x106862, 0x041A };
 p.stockLens.guards[2] = { 0x106868, 0x2242 };
 p.stockLens.guards[3] = { 0x106894, 0x5818 };
 p.stockLens.guards[4] = { 0x106896, 0x0C38 };
 p.stockLens.guards[5] = { 0x1068A0, 0x60F4 };
 p.stockLens.guards[6] = { 0x1068A2, 0x0C38 };
 p.hudW = 1.f / 1024.f; p.snapMinW = .0009f; p.snapMaxW = .01f;
 p.snapStep = 1.f / 65536.f; p.offScreen2DMaxW = .01f;
 p.farOutBelowW = .997f; p.flatScreens = true; p.unitScale = .01f;
 p.noShotFlash = true;
 p.calibrationAddr = 0x22C548;
 p.calibrationGuards[0] = { 0x1300B2, 0xD32C };
 p.calibrationGuards[1] = { 0x1300BC, 0x6242 };
 p.calibrationGuards[2] = { 0x1300BE, 0x0027 };
 p.calibrationGuards[3] = { 0x1300C0, 0x5242 };
 p.calibrationGuards[4] = { 0x1300C8, 0x420B };
 p.calibrationGuards[5] = { 0x1300D0, 0x5241 };
 p.calibrationGuards[6] = { 0x1300D2, 0x0027 };
 p.calibrationGuards[7] = { 0x130164, 0xC548 };
 p.calibrationGuards[8] = { 0x130166, 0x0C22 };
 p.revisionGuards[0] = { 0x15C8C0, 0xD128 };
 p.revisionGuards[1] = { 0x15C8C8, 0x6312 };
 p.revisionGuards[2] = { 0x15C8CA, 0x2338 };
 p.revisionGuards[3] = { 0x15C8D4, 0xD323 };
 p.revisionGuards[4] = { 0x15C8D6, 0x6232 };
 p.revisionGuards[5] = { 0x15C8D8, 0x2228 };
 p.revisionGuards[6] = { 0x15C964, 0x62AC };
 p.revisionGuards[7] = { 0x15C966, 0x0C3B };
 p.gunRam.base = 0x3869BC; p.gunRam.stride = 0x30;
 p.gunRam.magazineAddr = 0x386A30; p.gunRam.combatAddr = 0x3E6978; p.gunRam.ringAddr = 0x481338;
 p.gunRam.shotWaitMs = 200;
 const CodeGuard ninjaGuards[] {
  {0x0FAA72,0xD568},{0x0FAA7E,0xE074},{0x0FAA82,0x005D},{0x0FAA8A,0x8144},
  {0x0FAB94,0x8524},{0x0FAB96,0x70FF},{0x0FAB98,0x8124},
  {0x0FB5AA,0x8441},{0x0FB5AC,0x8801},
  {0x0F9554,0xD33F},{0x0F9556,0x6231},{0x0F9558,0x2228},
  {0x15D0A2,0x6102},{0x15D0A6,0x331C},{0x15D0A8,0x2359},{0x15D0AC,0x2032},
  {0x0FAC14,0x69BC},{0x0FAC16,0x0C38},{0x0F9654,0x6978},{0x0F9656,0x0C3E},
  {0x15D0FC,0x1338},{0x15D0FE,0x0C48},{0x15D100,0x133C},{0x15D102,0x0C48}
 };
 std::copy(std::begin(ninjaGuards),std::end(ninjaGuards),std::begin(p.gunRam.guards));
 // Native operator free play flag. Original EEPROM and ROM files are untouched.
 p.pokes[0] = { 0x3B62AC, 1 };
 list.push_back(p);
 return list;
}
static const std::vector<GameProfile> profiles = makeProfiles();
static const GameProfile *activeProfile;	// the game being played (or none)

static bool focalCodeThere(const GameProfile& profile)
{
	for (const CodeGuard& guard : profile.focalGuards)
		if (guard.offset != 0 && ReadMem16_nommu(0x8C000000u + guard.offset) != guard.expected)
			return false;
	return true;
}

static bool readProfileFocal(glm::vec2& focal)
{
	if (activeProfile == nullptr || activeProfile->liveFocalAddr == 0 || !focalCodeThere(*activeProfile))
		return false;
	const float value = readFloat(activeProfile->liveFocalAddr);
	if (!plausible(value, 10.f, 20000.f))
		return false;
	focal = glm::vec2(std::abs(value));
	return true;
}

const hands::Source *gameModelSource()
{
	for (const GameProfile& prof : profiles)
		if (settings.content.gameId == prof.gameId && prof.model.kind != hands::Source::None)
			return &prof.model;
	return nullptr;
}

bool gameGunMenu() {
	return activeProfile != nullptr && activeProfile->gunMenu;
}

int gunShotWaitMs() {
	return activeProfile != nullptr ? activeProfile->gunRam.shotWaitMs : 100;
}

bool gameManualOffscreenReload() { return activeProfile != nullptr && (activeProfile->analogOffscreenReload || activeProfile->ninja); }

bool gameAnalogOffscreenReload() {
	return activeProfile != nullptr && activeProfile->analogOffscreenReload;
}

// Invert the live cabinet calibration, preserving EEPROM and the zero-ADC reload
// sentinel. Cache a bounded integer map per axis/player; rebuild only after that
// player's six calibration bytes change. The native signed-low-byte parser is part
// of the forward model (gun_calibration.h), not a patch to shared JVS emulation.
void calibrateGunInput(int player, int& x, int& y)
{
	const GameProfile *prof = activeProfile;
	if (prof == nullptr || prof->calibrationAddr == 0 || player < 0 || player > 1 || x < 0 || x >= 640 || y < 0 || y >= 480)
		return;
	for (const CodeGuard& g : prof->calibrationGuards)
		if (g.offset != 0 && ReadMem16_nommu(0x8C000000u + g.offset) != g.expected) return;
	if (prof->calibrationMode != 0 && ReadMem32_nommu(0x8C000000u + prof->calibrationMode) != 1) return;
 if (prof->ninja) {
  s32 values[4];
  for (int k=0;k<4;k++) values[k]=(s32)ReadMem32_nommu(0x8C000000u + prof->calibrationAddr + player*16+k*4);
  struct NinjaCache { bool ready=false; s32 values[4] {}; int x[640],y[480]; };
  static NinjaCache cached[2]; static std::mutex ninjaMutex;
  std::lock_guard<std::mutex> lock(ninjaMutex); auto& map=cached[player];
  if (!map.ready || memcmp(map.values,values,sizeof(values)) != 0) {
   map.ready = ninjaCalibrationInverse(640,{values[0],values[2]},map.x)
       && ninjaCalibrationInverse(480,{values[1],values[3]},map.y);
   if (!map.ready) return;
   memcpy(map.values,values,sizeof(values));
   NOTICE_LOG(RENDERER,"VR: P%d Namco calibration inverse %d,%d,%d,%d (EEPROM unchanged)",player+1,values[0],values[1],values[2],values[3]);
  }
  x=map.x[x]; y=map.y[y]; return;
 }
	u8 values[6];
	for (int k = 0; k < 6; k++) values[k] = ReadMem8_nommu(0x8C000000u + prof->calibrationAddr + player * 6 + k);
	const GunAxisCalibration ax { values[0], values[1], values[4] }, ay { values[2], values[3], values[5] };
	if (!validGunCalibration(ax) || !validGunCalibration(ay)) return;
	struct Cache { bool ready = false; u8 values[6] {}; int x[640], y[480]; };
	static Cache cached[2];
	static std::mutex mutex;
	std::lock_guard<std::mutex> lock(mutex);
	Cache& map = cached[player];
	if (!map.ready || memcmp(map.values, values, sizeof(values)) != 0)
	{
		map.ready = gunCalibrationInverse(640, ax, map.x) && gunCalibrationInverse(480, ay, map.y);
		if (!map.ready) return;
		memcpy(map.values, values, sizeof(values));
		NOTICE_LOG(RENDERER, "VR: P%d calibration inverse %u,%u,%u,%u,%u,%u (EEPROM unchanged)", player + 1,
				values[0], values[1], values[2], values[3], values[4], values[5]);
	}
	x = map.x[x]; y = map.y[y];
}

static u32 markerTextures[4];	// the active profile's GameProfile::markerTextures
static u32 p2PromptTextures[4];	// ...and GameProfile::p2PromptTextures
// Each player's gun as read at the last VBlank (snapGuns, the emulator's thread), for gunState()
// (the headset's); valid false: not read yet.
static std::mutex gunMutex;
static GunState gunSnaps[2];
static std::atomic<u8> vmPlayers { 0 };
static std::atomic<bool> vmPlayersKnown { false };
static std::atomic<bool> ninjaStory { false };

static void applyGameProfile(Event, void *)
{
	std::fill(std::begin(fovLiterals), std::end(fovLiterals), 0u);
	std::fill(std::begin(markerTextures), std::end(markerTextures), 0u);
	std::fill(std::begin(p2PromptTextures), std::end(p2PromptTextures), 0u);
	{
		std::lock_guard<std::mutex> lock(gunMutex);
		gunSnaps[0] = gunSnaps[1] = GunState {};
	}
	vmPlayers = 0;
	vmPlayersKnown = false;
	ninjaStory = false;
	activeProfile = nullptr;
	// Online (GGPO), what the VR mod writes into a game it has a profile for must be the
	// same on both sides, whatever each player's settings or machine (a Quest, PCVR, a PC
	// without a headset): the profile's view widening as on the headset, and light guns in
	// ports A and B (B is the other player's). netplayDigest() lets the two sides check it.
	const bool netplay = config::GGPOEnable;
	if (xr::enabled())
	{
		// Everything the headset view depends on.
		config::VrReproject.override(true);
		config::NativeDepthInterpolation.override(true);
	}
	for (const GameProfile& prof : profiles)
	{
		if (settings.content.gameId != prof.gameId)
			continue;
		activeProfile = &prof;
		if (config::VrProjAddr == 0)
			config::VrProjAddr.override(prof.projAddr);
		if (config::VrViewportAddr == 0)
			config::VrViewportAddr.override(prof.viewportAddr);
		if (prof.focal > 0.f)
			config::VrFocal.override(prof.focal);
		if (prof.hudW > 0.f && config::VrHudW == 1.f)	// (unless the player set one)
			config::VrHudW.override(prof.hudW);
		if (netplay)
		{
			config::VrReproject.override(true);
			config::NativeDepthInterpolation.override(true);
			config::VrFovScale.override(prof.fovScale);
			config::VrWidenFov.override(true);
		}
		else if (xr::enabled() && config::VrWiden && config::VrFovScale <= 1.f)
			config::VrFovScale.override(prof.fovScale);
		if (config::VrWidenFov)
		{
			std::copy(std::begin(prof.fovLiterals), std::end(prof.fovLiterals), std::begin(fovLiterals));
			fovStock = prof.fovStock;
		}
		std::copy(std::begin(prof.markerTextures), std::end(prof.markerTextures), std::begin(markerTextures));
		std::copy(std::begin(prof.p2PromptTextures), std::end(prof.p2PromptTextures), std::begin(p2PromptTextures));
		const bool gunB = config::VrDualWield || netplay;
		if (((xr::enabled() && config::VrXrGun) || netplay) && settings.platform.isConsole()
				&& (config::MapleMainDevices[0] != MDT_LightGun || (gunB && config::MapleMainDevices[1] != MDT_LightGun)))
		{
			// The right controller is a light gun: plug one into port A (a pad ignores
			// where it points), and player 2's into port B for dual wielding (vr.DualWield)
			// or the other player online. The devices were made just before this event,
			// before the game runs, so they can still be swapped. (An arcade game's guns are
			// on its I/O board already.)
			config::MapleMainDevices[0].override(MDT_LightGun);
			if (gunB)
				config::MapleMainDevices[1].override(MDT_LightGun);
			mcfg_DestroyDevices();
			mcfg_CreateDevices();
			NOTICE_LOG(RENDERER, "VR: light gun in port A%s", gunB ? " and B" : "");
		}
		NOTICE_LOG(RENDERER, "VR: camera profile for %s", prof.gameId);
	}
}

static struct GameProfileRegistration
{
	GameProfileRegistration() {
		EventManager::listen(Event::Start, applyGameProfile);
	}
} gameProfileRegistration;

static bool widensByCode() {
	return activeProfile != nullptr && activeProfile->fovPatch[0].offset != 0 && config::VrWidenFov;
}

// The code a profile's patch and pokes are for is in RAM: every patched word stock or patched.
static bool profileCodeThere(const GameProfile& prof)
{
 for (const CodeGuard& g : prof.revisionGuards)
  if (g.offset && ReadMem16_nommu(0x8C000000u + g.offset) != g.expected) return false;
	if (prof.fovPatch[0].offset == 0)
		return true;
	for (const CodePatch& c : prof.fovPatch)
	{
		if (c.offset == 0)
			break;
		const u16 now = ReadMem16_nommu(0x8C000000u + c.offset);
		if (now != c.stock && now != c.patched)
			return false;
	}
	return true;
}

// Each frame: the profile's code patch (all of it, once the stock code is there) and pokes.
static void applyProfileWrites(Event, void *)
{
	const GameProfile *prof = activeProfile;
	if (prof == nullptr || !config::VrReproject || !profileCodeThere(*prof))
		return;
	if (prof->fovPatch[0].offset != 0 && config::VrWidenFov && config::VrFovScale > 1.f)
	{
		bool stock = true;
		for (const CodePatch& c : prof->fovPatch)
			stock = stock && (c.offset == 0 || ReadMem16_nommu(0x8C000000u + c.offset) == c.stock);
		if (stock)
		{
			// (the dynarec drops what it compiled from these pages when they're written)
			for (const CodePatch& c : prof->fovPatch)
				if (c.offset != 0)
					WriteMem16_nommu(0x8C000000u + c.offset, c.patched);
			NOTICE_LOG(RENDERER, "VR: the game's field of view widened (code at %08x)", 0x8C000000u + prof->fovPatch[0].offset);
		}
	}
	for (const Poke& poke : prof->pokes)
		if (poke.offset != 0 && ReadMem16_nommu(0x8C000000u + poke.offset) != poke.value)
			WriteMem16_nommu(0x8C000000u + poke.offset, poke.value);
}

void netplayDigest(MD5Sum& md5)
{
	// everything widenGameView and applyProfileWrites may write, as they'd decide it
	const GameProfile *prof = activeProfile;
	const bool on = config::VrReproject && config::VrFovScale > 1.f;
	const char *id = prof != nullptr ? prof->gameId : "";
	md5.add(id, (unsigned long)strlen(id));		// (the text, not a pointer: add(const T&) hashes the object)
	md5.add((u8)config::VrReproject);
	if (on && fovLiterals[0] != 0)
	{
		md5.add(fovLiterals).add(fovStock);
		md5.add(widenedAngle(fovStock, config::VrFovScale));
	}
	if (prof != nullptr && config::VrReproject)
	{
		if (on && widensByCode())
			for (const CodePatch& c : prof->fovPatch)
				md5.add(c.offset).add(c.stock).add(c.patched);
		for (const Poke& poke : prof->pokes)
			md5.add(poke.offset).add(poke.value);
	}
}

static struct ProfileWritesRegistration
{
	ProfileWritesRegistration() {
		EventManager::listen(Event::VBlank, applyProfileWrites);
	}
} profileWritesRegistration;

// A player's gun block in RAM (GameProfile::gunRam), or 0 when the profile has none.
static u32 gunBlock(const GameProfile *prof, int player)
{
	if (prof == nullptr || prof->gunRam.base == 0 || player < 0 || player > 1)
		return 0;
	return 0x8C000000u | ((prof->gunRam.base + player * prof->gunRam.stride) & 0x01FFFFFFu);
}

bool gunState(int player, GunState& out)
{
	out = GunState {};
	if (gunBlock(activeProfile, player) == 0)
		return false;
	if (!emu.running())
		return true;
	std::lock_guard<std::mutex> lock(gunMutex);
	out = gunSnaps[player];
	return true;
}

// A float VM counter must be finite, nonnegative and integral before it can be
// narrowed into the public snapshot. Corrupt/unloaded state never produces feedback.
static bool vmCount(u32 addr, u32 maximum, u32& value)
{
	const float f = readFloat(addr);
	u32 bits; memcpy(&bits, &f, sizeof(bits));
	if ((bits & 0x7f800000u) == 0x7f800000u || f < 0.f || f > maximum || f != std::floor(f))
		return false;
	value = (u32)f;
	return true;
}

// Each VBlank, between two of the game's frames: each player's gun state (gunState), all of it
// at once so its fields agree. A shot fired a round when the ammo went down, and every shot does
// while the rifle is held, at this read or the one before (its last, 1 -> 0, is followed by the
// pistol's 6 the next frame). Logged: the magazine's size changing ("VR: P1 magazine 6 -> 45": a
// pickup, a scripted sequence's own gun) and the rifle coming and going ("VR: P1 weapon pistol ->
// rifle", "rifle -> pistol"); the first read after the game starts only notes them.
static void snapGuns(Event, void *)
{
	const GameProfile *prof = activeProfile;
 if (prof != nullptr && prof->ninja) {
  bool guarded=profileCodeThere(*prof);
  for (const CodeGuard& g:prof->gunRam.guards)
   guarded=guarded && (!g.offset || ReadMem16_nommu(0x8C000000u+g.offset)==g.expected);
  static NinjaGunTracker tracker[2];
  u8 players=0; bool story=false;
  const auto& ram=prof->gunRam;
  const u32 combat=guarded?ReadMem16_nommu(0x8C000000u+ram.combatAddr):0;
  const u32 magazine=guarded?ReadMem16_nommu(0x8C000000u+ram.magazineAddr):0;
  for (int player=0;player<2;player++) {
   const u32 block=gunBlock(prof,player);
   NinjaGunSample sample {};
   sample.codeValid=guarded;
   if (guarded) {
    sample.slot=ReadMem8_nommu(block); sample.lifecycle=ReadMem8_nommu(block+1);
    sample.active=ReadMem8_nommu(block+2); sample.ammo=ReadMem16_nommu(block+8);
    sample.magazine=magazine; sample.ring=(s32)ReadMem32_nommu(0x8C000000u+ram.ringAddr+4*player);
    sample.combat=combat;
   }
   const auto native=updateNinjaGun(tracker[player],sample);
   // Lifecycle 3 is a real pending join during a story: retain dual mode while
   // native gameplay waits, but never treat it as a round or an active gun.
   if (native.inGame || (guarded && sample.slot==1 && sample.lifecycle==3)) players|=1<<player;
   story=story || (native.inGame && combat==0);
   GunState now {}; now.valid=native.valid; now.ammo=(u8)native.ammo;
   now.magazine=(u8)native.magazine; now.shots=native.shots; now.fired=native.fired;
   std::lock_guard<std::mutex> lock(gunMutex);
   const auto was=gunSnaps[player];
   if (now.valid && !was.valid) NOTICE_LOG(RENDERER,"VR: P%d magazine 0 -> %d",player+1,now.magazine);
   if (now.fired!=was.fired) NOTICE_LOG(RENDERER,"VR: P%d accepted round %u ammo %u",player+1,now.fired,now.ammo);
   gunSnaps[player]=now;
  }
  const auto was=vmPlayers.exchange(players); vmPlayersKnown=guarded;
  for(int player=0;player<2;player++) if (((was^players)>>player)&1)
   NOTICE_LOG(RENDERER,"VR: P%d active %s",player+1,((players>>player)&1)?"on":"off");
  if(ninjaStory.exchange(story)!=story) NOTICE_LOG(RENDERER,"VR: cutscene %s",story?"on":"off");
  return;
 }
	u8 enabled = 0;
	bool vmValid = prof != nullptr && prof->gunRam.floats;
	if (vmValid)
	{
		for (const CodeGuard& g : prof->gunRam.guards)
			vmValid = vmValid && (g.offset == 0 || ReadMem16_nommu(0x8C000000u + g.offset) == g.expected);
		u32 session = 0, active[2] {};
		vmValid = vmValid && vmCount(prof->gunRam.session, 1, session)
				&& vmCount(prof->gunRam.active, 1, active[0]) && vmCount(prof->gunRam.active + 4, 1, active[1]);
		if (vmValid && session == 1)
			enabled = (u8)(active[0] | (active[1] << 1));
		const u8 was = vmPlayers.exchange(enabled);
		vmPlayersKnown = vmValid;
		for (int player = 0; player < 2; player++)
			if (((was ^ enabled) >> player) & 1)
				NOTICE_LOG(RENDERER, "VR: P%d active %s", player + 1, ((enabled >> player) & 1) ? "on" : "off");
	}
	for (int player = 0; player < 2; player++)
	{
		const u32 block = gunBlock(prof, player);
		if (block == 0)
			continue;
		const GameProfile::GunRam& ram = prof->gunRam;
		GunState now {};
		if (ram.floats)
		{
			u32 ammo = 0, magazine = 0, shots = 0;
			const bool counts = vmValid && vmCount(block + ram.ammo, 255, ammo)
					&& vmCount(block + ram.magazine, 255, magazine) && vmCount(block + ram.shots, 0xFFFFFF, shots);
			now.valid = counts && ((enabled >> player) & 1) && magazine > 0 && ammo <= magazine;
			if (counts) { now.ammo = (u8)ammo; now.magazine = (u8)magazine; now.shots = (u16)shots; }
		}
		else
		{
			now.valid = true;
			now.ammo = ReadMem8_nommu(block + ram.ammo);
			now.magazine = ReadMem8_nommu(block + ram.magazine);
			now.shots = ReadMem16_nommu(block + ram.shots);
		}
		// (the players' byte holds their two bits only: before the game clears its RAM at boot,
		// ~11 s, it's noise that can read as a rifle in play: fb, with P2's weapon 02, in t14-play)
		const u8 playing = ram.playing != 0 ? ReadMem8_nommu(0x8C000000u | (ram.playing & 0x01FFFFFFu)) : 0;
		now.rifle = ram.weapon >= 0 && ReadMem8_nommu(block + ram.weapon) == ram.rifleValue
				&& (ram.playing == 0 || ((playing & ~3) == 0 && ((playing >> player) & 1) != 0));
		std::lock_guard<std::mutex> lock(gunMutex);
		const GunState was = gunSnaps[player];
		now.fired = was.fired;
		if (was.valid && now.valid)
		{
			// (a drop, or more than 8 at once: the game cleared its RAM, a new game; the ammo's low
			// byte compared signed: a shot is -1, a refill at most +45, and the scope view's 10000
			// rounds, 0x2710, wrap the byte at its 17th shot)
			const int rises = (s16)(u16)(now.shots - was.shots);
			if (rises > 0 && rises <= 8 && (now.rifle || was.rifle || (s8)(now.ammo - was.ammo) < 0))
				now.fired += rises;
			if (now.magazine != was.magazine)
				NOTICE_LOG(RENDERER, "VR: P%d magazine %d -> %d", player + 1, was.magazine, now.magazine);
			if (now.rifle != was.rifle)
				NOTICE_LOG(RENDERER, "VR: P%d weapon %s -> %s", player + 1, was.rifle ? "rifle" : "pistol", now.rifle ? "rifle" : "pistol");
		}
		if (ram.floats && now.valid && !was.valid)
			NOTICE_LOG(RENDERER, "VR: P%d magazine 0 -> %d", player + 1, now.magazine);
		gunSnaps[player] = now;
	}
}

static struct GunSnapRegistration
{
	GunSnapRegistration() {
		EventManager::listen(Event::VBlank, snapGuns);
	}
} gunSnapRegistration;

#ifndef __ANDROID__
//
// PC test aids for the harness (vr-test.ps1), nothing unless set: vr.WatchRam logs a change of
// any watched RAM value, read each VBlank ("VR: ram 0c155808 06 -> 2d"; 2 and 4 bytes as 4 and
// 8 hex digits); the UDP line POKE writes a value at the next VBlank ("VR: poke 0c155808 =
// 2d"); vr.WatchTex logs a texture coming into the passes and going out of them
// (watchTextures, from dropShotMarker). Addresses are SH4 ones (0c155808), values hex.
//
struct RamWatch
{
	u32 addr;		// SH4 address, aligned to its size
	u32 size;		// bytes: 1, 2 or 4
	u32 value;		// as last read
	bool known;		// read at least once
};
struct PendingPoke
{
	u32 addr, size, value;
};
static std::mutex pokeMutex;
static std::vector<PendingPoke> pendingPokes;

static u32 ramAddress(u32 addr) {
	return 0x8C000000u | (addr & 0x01FFFFFFu);
}

// area 3 (main RAM) in any of its mirrors: 0c000000, 8c000000, ac000000 ... (a typo such as
// 05000000 would otherwise land in RAM through ramAddress's mask)
static bool inMainRam(u32 addr) {
	return (addr & 0x1C000000u) == 0x0C000000u;
}

// "a,b c;d" -> a, b, c, d (vr-test.ps1 quotes a list with commas in -config)
static std::vector<std::string> splitWatchList(const std::string& spec)
{
	std::vector<std::string> items;
	std::string item;
	for (char c : spec + ",")
	{
		if (c == ',' || c == ';' || c == ' ')
		{
			if (!item.empty())
				items.push_back(item);
			item.clear();
		}
		else
			item += c;
	}
	return items;
}

static bool parseHex(const std::string& s, u32& v)
{
	if (s.empty() || s.size() > 8 || !isxdigit((unsigned char)s[0]))
		return false;
	char *end;
	v = (u32)strtoul(s.c_str(), &end, 16);
	return *end == '\0';
}

// "addr[:size],...", at most 16
static std::vector<RamWatch> parseRamWatches(const std::string& spec)
{
	std::vector<RamWatch> watches;
	for (const std::string& item : splitWatchList(spec))
	{
		const size_t colon = item.find(':');
		const std::string size = colon == std::string::npos ? "1" : item.substr(colon + 1);
		const u32 bytes = size == "1" ? 1 : size == "2" ? 2 : size == "4" ? 4 : 0;
		u32 addr;
		if (!parseHex(item.substr(0, colon), addr) || !inMainRam(addr) || bytes == 0 || addr % bytes != 0 || watches.size() == 16)
			WARN_LOG(RENDERER, "VR: vr.WatchRam: '%s' left out (want addr[:1|2|4], hex, in main RAM 0c000000-0fffffff, aligned, at most 16)", item.c_str());
		else
			watches.push_back({ addr, bytes, 0, false });
	}
	return watches;
}

static void watchRamAndPoke(Event, void *)
{
	std::vector<PendingPoke> pokes;
	{
		std::lock_guard<std::mutex> lock(pokeMutex);
		pokes.swap(pendingPokes);
	}
	for (const PendingPoke& p : pokes)
	{
		switch (p.size)
		{
		case 1: WriteMem8_nommu(ramAddress(p.addr), (u8)p.value); break;
		case 2: WriteMem16_nommu(ramAddress(p.addr), (u16)p.value); break;
		default: WriteMem32_nommu(ramAddress(p.addr), p.value); break;
		}
		NOTICE_LOG(RENDERER, "VR: poke %08x = %0*x", p.addr, (int)p.size * 2, p.value);
	}
	static std::string spec;
	static std::vector<RamWatch> watches;
	if (config::VrWatchRam.get() != spec)
	{
		spec = config::VrWatchRam.get();
		watches = parseRamWatches(spec);
	}
	for (RamWatch& w : watches)
	{
		const u32 at = ramAddress(w.addr);
		const u32 v = w.size == 1 ? ReadMem8_nommu(at) : w.size == 2 ? ReadMem16_nommu(at) : ReadMem32_nommu(at);
		if (w.known && v != w.value)
			NOTICE_LOG(RENDERER, "VR: ram %08x %0*x -> %0*x", w.addr, (int)w.size * 2, w.value, (int)w.size * 2, v);
		w.value = v;
		w.known = true;
	}
}

void queuePoke(unsigned addr, unsigned size, unsigned value)
{
	if ((size != 1 && size != 2 && size != 4) || addr % size != 0 || !inMainRam(addr))
	{
		WARN_LOG(RENDERER, "VR: POKE %08x size %u left out (want size 1, 2 or 4, aligned, in main RAM 0c000000-0fffffff)", addr, size);
		return;
	}
	if (size < 4)
		value &= (1u << (size * 8)) - 1;
	std::lock_guard<std::mutex> lock(pokeMutex);
	pendingPokes.push_back({ addr, size, value });
}

static struct WatchRamRegistration
{
	WatchRamRegistration() {
		EventManager::listen(Event::VBlank, watchRamAndPoke);
	}
} watchRamRegistration;

struct TexWatch
{
	u32 tex;		// VRAM address, as riplib's .tex
	u32 missing;	// passes in a row without it
	bool shown;
};

// vr.WatchTex: "VR: tex 7e2800 shown (78 polys)" on the first pass that has it (polygons in
// the three lists), "VR: tex 7e2800 gone" after 10 passes in a row without it.
static void watchTextures(const rend_context& ctx, const RenderPass& pass, const RenderPass& prev)
{
	static std::string spec;
	static std::vector<TexWatch> watches;
	if (config::VrWatchTex.get() != spec)
	{
		spec = config::VrWatchTex.get();
		watches.clear();
		for (const std::string& item : splitWatchList(spec))
		{
			u32 tex;
			if (!parseHex(item, tex) || watches.size() == 16)
				WARN_LOG(RENDERER, "VR: vr.WatchTex: '%s' left out (want a hex VRAM address, at most 16)", item.c_str());
			else
				watches.push_back({ tex, 0, false });
		}
	}
	if (watches.empty())
		return;
	u32 counts[16] {};
	auto count = [&](const std::vector<PolyParam>& polys, u32 from, u32 to) {
		for (u32 k = from; k < to && k < polys.size(); k++)
			if (polys[k].pcw.Texture)
			{
				const u32 tex = polys[k].tcw.TexAddr << 3;
				for (size_t i = 0; i < watches.size(); i++)
					counts[i] += watches[i].tex == tex;
			}
	};
	count(ctx.global_param_op, std::max(prev.op_count, 1u), pass.op_count);
	count(ctx.global_param_pt, prev.pt_count, pass.pt_count);
	count(ctx.global_param_tr, prev.tr_count, pass.tr_count);
	for (size_t i = 0; i < watches.size(); i++)
	{
		TexWatch& w = watches[i];
		if (counts[i] > 0)
		{
			if (!w.shown)
				NOTICE_LOG(RENDERER, "VR: tex %06x shown (%u polys)", w.tex, counts[i]);
			w.shown = true;
			w.missing = 0;
		}
		else if (w.shown && ++w.missing >= 10)
		{
			NOTICE_LOG(RENDERER, "VR: tex %06x gone", w.tex);
			w.shown = false;
		}
	}
}
#endif

float worldScale()
{
	const GameProfile *prof = activeProfile;
	return config::VrWorldScale * (prof != nullptr ? prof->unitScale : 1.f);
}

// The panel setting is in reference-game units, independently of the game's
// own world-unit size. Renderers and ray tests must use this same conversion.
float panelDepth() {
 return panelDepthInGameUnits(config::VrHudDepth,activeProfile ? activeProfile->unitScale : 1.f);
}

// The stock focal length the game's light-gun hit test uses, in DC pixels.
static float stockFocal()
{
	const GameProfile *prof = activeProfile;
	if (prof != nullptr && prof->stockLens.descriptors != 0 && focalCodeThere(*prof))
	{
		const auto& lens = prof->stockLens;
		bool guarded = true;
		for (const CodeGuard& g : lens.guards)
			guarded = guarded && (g.offset == 0 || ReadMem16_nommu(0x8C000000u + g.offset) == g.expected);
		u32 index = ReadMem32_nommu(0x8C000000u + lens.index);
  if (lens.pointerIndex) {
   const u32 offset = index & 0x01FFFFFFu;
   const bool inRam = (index & 0x1E000000u) == 0x0C000000u;
   guarded = guarded && inRam && lens.stride && offset >= lens.descriptors
       && (offset - lens.descriptors) % lens.stride == 0;
   index = guarded ? (offset - lens.descriptors) / lens.stride : lens.count;
  }
		if (guarded && index < lens.count)
		{
			const u32 angle = ReadMem32_nommu(0x8C000000u + lens.descriptors + index * lens.stride + lens.angleOffset);
			const float width = readFloat(lens.width);
			if (angle > 1 && angle <= 65535 && plausible(width, 10.f, 2000.f))
			{
				// The original SH4 setter uses an integer right shift before its
				// tangent, including odd angles; do not infer this from live focal.
				const float f = (float)((width * .5) / std::tan((angle >> 1) * (6.28318530717958647692 / 65536.0)));
				if (plausible(f, 10.f, 20000.f)) return f;
			}
		}
	}
	if (activeProfile != nullptr && activeProfile->stockFocalAddr != 0 && focalCodeThere(*prof))
	{
		float f = readFloat(prof->stockFocalAddr);
		if (prof->stockFocalInverse)
			f = f != 0.f ? 1.f / f : 0.f;
		if (plausible(f, 10.f, 20000.f))
			return f;
	}
	return config::VrFocal;
}

// Where the game draws a shot's hit (its marker, sparks on a wall), in framebuffer pixels: at
// the gun; or, with the profile's markerWidened and its view widened, where that view puts
// the ray the hit test cast with the stock focal: the centre plus the gun's offset times the
// live over the stock focal.
static glm::vec2 shotDrawnAt(glm::vec2 gun, int fbWidth, int fbHeight)
{
	const GameProfile *prof = activeProfile;
	if (prof == nullptr || !prof->markerWidened || !widensByCode() || config::VrFovScale <= 1.f)
		return gun;
	const glm::vec2 centre(fbWidth * 0.5f, fbHeight * 0.5f);
	return centre + (gun - centre) * readGameFocal() / stockFocal();
}

GameCamera gameCamera(const rend_context& ctx)
{
	int dcWidth, dcHeight;
	getPvrFramebufferSize(ctx, dcWidth, dcHeight);
	GameCamera cam;
	cam.dcSize = glm::vec2(dcWidth, dcHeight);
	cam.tanHalf = cam.dcSize * 0.5f / readGameFocal();
	cam.stockTan = cam.dcSize * 0.5f / stockFocal();
	cam.overlayW = config::VrHudW;
	return cam;
}

static glm::mat4 projection(glm::vec2 tanHalf, float zNear)
{
	glm::mat4 proj(0.f);
	proj[0][0] = 1.f / tanHalf.x;
	proj[1][1] = 1.f / tanHalf.y;
	proj[2][2] = -1.f;
	proj[2][3] = -1.f;
	proj[3][2] = -2.f * zNear;
	return proj;
}

glm::vec3 comfortParams()
{
	const float start = config::VrComfortStart;
	if (start <= 0.f)
		return glm::vec3(0.f);
	// at most half the start distance: the curve stays monotonic
	const float closest = std::clamp((float)config::VrComfortMin, 0.f, start * 0.5f);
	return glm::vec3(worldScale(), start, closest);
}

//
// The game's own 2D shot marker: the little flash it draws on its 640x480 screen where the
// light gun was when it fired. In the headset that screen is a plane at the stock framing,
// so after a shot into the (wider) 3D scene the marker shows up somewhere else, and at
// another depth, than the hit, next to our own muzzle flash and aim dot: it goes. After a
// shot at the 2D plane itself (menus, text) it stays, since there it is where the shot
// landed. It is taken out while the TA's polygons are still whole (before indexing merges
// and sorts them): NaN x/y keeps its vertices out of every index, sorted or not, like the
// PVR's own invalid vertices (is_vertex_inf). z stays finite for the sorters.
//
constexpr float MarkerMaxSize = 96.f;	// DC pixels, width and height (HOTD2's glow is 62)
constexpr float MarkerMaxOffset = 120.f;	// DC pixels, from its centre to the gun (its rays fly out ~100)
constexpr int64_t MarkerMs = 500;		// after the trigger pull
// A bright full-screen flash right after a shot (light guns on a CRT need one to find the
// beam), should the game draw it: vr.DropShotFlash.
constexpr int64_t FlashMs = 200;

// The last few shots: in a fight they come ~0.2 s apart, so with only the newest one the
// marker of the one before would come back for the rest of its life.
struct GunShot
{
	std::atomic<int64_t> ms { std::numeric_limits<int64_t>::min() / 2 };
	std::atomic<u32> pos { 0 };			// (y << 16) | x, DC pixels
	std::atomic<bool> into3D { false };
};
static GunShot gunShots[4];
static std::atomic<u32> nextShot;

static int64_t steadyMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
}

void noteGunShot(int x, int y, bool into3D)
{
	if (x < 0 || y < 0 || x >= 640 || y >= 480)
		return;
	GunShot& s = gunShots[nextShot.fetch_add(1) % std::size(gunShots)];
	s.ms = std::numeric_limits<int64_t>::min() / 2;	// invalid while it changes
	s.pos = ((u32)y << 16) | (u32)x;
	s.into3D = into3D;
	s.ms = steadyMs();	// last: whoever sees the new time sees the rest
}

static bool onOverlay(const Vertex& v, float hudW) {
	return v.z > 0.f && std::abs(1.f / v.z - hudW) < 0.002f;
}

static bool opaqueBlack(const Vertex& v) {
	return v.col[3] > 250 && v.col[0] < 3 && v.col[1] < 3 && v.col[2] < 3;
}

static void hideVertices(rend_context& ctx, const PolyParam& pp)
{
	for (u32 j = pp.first; j < pp.first + pp.count; j++)
		ctx.verts[j].x = ctx.verts[j].y = std::numeric_limits<float>::quiet_NaN();
}

// 2D layers the game draws just in front of or behind its 2D plane (HOTD2: text at W 0.98
// and 0.99 over boxes at W 1, other layers at 0.80 and 1.02): screen-aligned polygons, every
// vertex at the same W. Off the plane they went through the 3D path, ~2 cm in front of the
// eyes, and the comfort zone pushed them back bent ("B to cancel" sucked into a vortex).
// They are pulled onto the plane, their order kept. Only what looks like such a layer:
// one W, a round one (a multiple of 0.01: sprites and debris flying past the camera pass
// through every W, see the headset logs), near the plane, and on the screen. The game's
// profile can move the band (snapMinW, snapMaxW) and the step (snapStep).
constexpr float SnapMinW = 0.75f;

static void snapNear2D(rend_context& ctx, const std::vector<PolyParam>& list, u32 from, u32 to, float hudW,
		int fbWidth, int fbHeight)
{
	const GameProfile *prof = activeProfile;
	const float minW = prof != nullptr ? prof->snapMinW : SnapMinW;
	const float SnapMaxW = prof != nullptr ? prof->snapMaxW : 1.05f;
	const float step = prof != nullptr ? prof->snapStep : 0.01f;
	const float offScreenMaxW = prof != nullptr ? prof->offScreen2DMaxW : 0.f;
	for (u32 n = from; n < to && n < list.size(); n++)
	{
		const PolyParam& pp = list[n];
		if (pp.count < 3 || pp.first + pp.count > ctx.verts.size())
			continue;
		const float z = ctx.verts[pp.first].z;
		if (!(z > 0.f) || !std::isfinite(z))
			continue;
		const float w = 1.f / z;
		const float layer = prof != nullptr && prof->snapInverseDepth ? z : w;
		if (w < minW || w > SnapMaxW || w == hudW || std::abs(layer / step - std::round(layer / step)) > 0.02f)
			continue;
		const bool anywhere = w < offScreenMaxW;
		bool off = false;
		u32 i = pp.first;
		for (; i < pp.first + pp.count; i++)
		{
			const Vertex& v = ctx.verts[i];
			const bool onScreen = v.x >= -1.f && v.x <= fbWidth + 1.f && v.y >= -1.f && v.y <= fbHeight + 1.f;
			if (std::abs(v.z - z) > z * 1e-5f || !(onScreen || (anywhere && std::isfinite(v.x) && std::isfinite(v.y))))
				break;
			off = off || !onScreen;
		}
		if (i != pp.first + pp.count)
			continue;	// not flat, or not on the screen: 3D
		// one linear scale per side over the whole band, inside the plane's +-0.002
		const float snapped = snappedOverlayDepth(w,hudW,minW,SnapMaxW);
		for (u32 j = pp.first; j < pp.first + pp.count; j++)
			ctx.verts[j].z = 1.f / snapped;
		// what gets snapped, once per W (in 0.01 steps), to check nothing 3D is caught
		static u32 seen[202], seenOff[202];
		const int bucket = (int)std::lround(w * 100.f);
		if (bucket >= 0 && bucket < (int)std::size(seen) && (off ? seenOff : seen)[bucket]++ == 0)
			NOTICE_LOG(RENDERER, "VR: 2D layer at W %.3f (%u verts at %.0f,%.0f) put on the 2D plane%s",
					w, pp.count, ctx.verts[pp.first].x, ctx.verts[pp.first].y, off ? ", off the screen (quads cut to it)" : "");
	}
}

// What the game shows of its 2D is what lies on its 640x480 screen: a TV cuts the rest off,
// and in the headset it would hang in the room around the 2D plane. HOTD2's chapter title
// cards zoom in from text three times the screen's width (W 1, on the plane), and the pause
// freezes such a frame: the huge copy filled the view. A 2D polygon wholly off the screen
// goes; an axis-aligned quad (the game's sprites and text) is cut to the screen, its texture
// and colours with it (one W over the quad: they are linear in x and y). Anything else
// partly off the screen stays as it is.
static void cropToScreen(rend_context& ctx, const PolyParam& pp, float fbWidth, float fbHeight)
{
	float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
	for (u32 i = pp.first; i < pp.first + pp.count; i++)
	{
		const Vertex& v = ctx.verts[i];
		if (!std::isfinite(v.x) || !std::isfinite(v.y))
			return;		// hidden already
		x0 = std::min(x0, v.x);
		x1 = std::max(x1, v.x);
		y0 = std::min(y0, v.y);
		y1 = std::max(y1, v.y);
	}
	if (x1 <= 0.f || y1 <= 0.f || x0 >= fbWidth || y0 >= fbHeight)
	{
		hideVertices(ctx, pp);
		return;
	}
	if ((x0 >= 0.f && y0 >= 0.f && x1 <= fbWidth && y1 <= fbHeight) || pp.count != 4)
		return;
	// the vertex at each corner: top left, top right, bottom left, bottom right
	const float ex = std::max(x1 - x0, 1.f) * 1e-4f, ey = std::max(y1 - y0, 1.f) * 1e-4f;
	if (x1 - x0 < 0.01f || y1 - y0 < 0.01f)
		return;
	int at[4] { -1, -1, -1, -1 };
	for (u32 k = 0; k < 4; k++)
	{
		const Vertex& v = ctx.verts[pp.first + k];
		const int cx = std::abs(v.x - x0) <= ex ? 0 : std::abs(v.x - x1) <= ex ? 1 : -1;
		const int cy = std::abs(v.y - y0) <= ey ? 0 : std::abs(v.y - y1) <= ey ? 1 : -1;
		if (cx < 0 || cy < 0 || at[cy * 2 + cx] >= 0)
			return;		// not a rectangle on the screen's axes
		at[cy * 2 + cx] = (int)k;
	}
	Vertex c[4];
	for (int k = 0; k < 4; k++)
		c[k] = ctx.verts[pp.first + at[k]];
	auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
	auto bilinear = [&](float tl, float tr, float bl, float br, float s, float t) {
		return lerp(lerp(tl, tr, s), lerp(bl, br, s), t);
	};
	auto colour = [&](const u8 (&tl)[4], const u8 (&tr)[4], const u8 (&bl)[4], const u8 (&br)[4], u8 (&out)[4], float s, float t) {
		for (int j = 0; j < 4; j++)
			out[j] = (u8)std::clamp((int)std::lround(bilinear(tl[j], tr[j], bl[j], br[j], s, t)), 0, 255);
	};
	const float nx0 = std::max(x0, 0.f), nx1 = std::min(x1, fbWidth);
	const float ny0 = std::max(y0, 0.f), ny1 = std::min(y1, fbHeight);
	for (int k = 0; k < 4; k++)
	{
		const float x = (k & 1) ? nx1 : nx0, y = (k & 2) ? ny1 : ny0;
		const float s = (x - x0) / (x1 - x0), t = (y - y0) / (y1 - y0);
		Vertex& v = ctx.verts[pp.first + at[k]];
		v.x = x;
		v.y = y;
		v.z = bilinear(c[0].z, c[1].z, c[2].z, c[3].z, s, t);
		v.u = bilinear(c[0].u, c[1].u, c[2].u, c[3].u, s, t);
		v.v = bilinear(c[0].v, c[1].v, c[2].v, c[3].v, s, t);
		v.u1 = bilinear(c[0].u1, c[1].u1, c[2].u1, c[3].u1, s, t);
		v.v1 = bilinear(c[0].v1, c[1].v1, c[2].v1, c[3].v1, s, t);
		colour(c[0].col, c[1].col, c[2].col, c[3].col, v.col, s, t);
		colour(c[0].spc, c[1].spc, c[2].spc, c[3].spc, v.spc, s, t);
		colour(c[0].col1, c[1].col1, c[2].col1, c[3].col1, v.col1, s, t);
		colour(c[0].spc1, c[1].spc1, c[2].spc1, c[3].spc1, v.spc1, s, t);
	}
}

// Everything on the 2D plane, cut to the screen.
static void cropPlaneToScreen(rend_context& ctx, const std::vector<PolyParam>& list, u32 from, u32 to, float hudW,
		int fbWidth, int fbHeight)
{
	for (u32 n = from; n < to && n < list.size(); n++)
	{
		const PolyParam& pp = list[n];
		if (pp.count < 3 || pp.first + pp.count > ctx.verts.size())
			continue;
		u32 i = pp.first;
		while (i < pp.first + pp.count && onOverlay(ctx.verts[i], hudW))
			i++;
		if (i == pp.first + pp.count)
			cropToScreen(ctx, pp, (float)fbWidth, (float)fbHeight);
	}
}

// The game's sights (Confidential Mission: a ring and X marks over each enemy about to fire):
// 2D drawn at the enemy's point on the game's screen, at or just in front of its 2D plane.
// Widened, that point is the enemy's in the view the game draws now (focal f), but the 2D
// plane shows the stock framing (vr.Focal): there the sight sat nearer the centre than its
// enemy, by f / vr.Focal. A sight polygon (one of the profile's textures, every corner from
// sightMinW to sightMaxW) goes onto the plane with its offset from the screen's centre scaled
// back out (place2DLayers' unwiden, inverted), then is cut to the screen: an axis-aligned
// quad cut, any other shape losing the triangles that reach outside. The first one placed
// after a pass without any is logged (logFirst), with where the game drew it.
static void placeSights(rend_context& ctx, const std::vector<PolyParam>& list, u32 from, u32 to, float hudW,
		int fbWidth, int fbHeight, glm::vec2 unwiden, u32& placed, bool logFirst)
{
	const GameProfile *prof = activeProfile;
	if (prof == nullptr || prof->sightTextures[0] == 0)
		return;
	const glm::vec2 centre(fbWidth * 0.5f, fbHeight * 0.5f);
	for (u32 n = from; n < to && n < list.size(); n++)
	{
		const PolyParam& pp = list[n];
		if (pp.count < 3 || pp.first + pp.count > ctx.verts.size())
			continue;
		const u32 tex = pp.pcw.Texture ? pp.tcw.TexAddr << 3 : 0;
		if (tex == 0 || std::find(std::begin(prof->sightTextures), std::end(prof->sightTextures), tex) == std::end(prof->sightTextures))
			continue;
		glm::vec2 drawn(0.f);
		u32 i = pp.first;
		for (; i < pp.first + pp.count; i++)
		{
			const Vertex& v = ctx.verts[i];
			if (!(v.z > 0.f) || !std::isfinite(v.x) || !std::isfinite(v.y)
					|| 1.f / v.z < prof->sightMinW || 1.f / v.z > prof->sightMaxW)
				break;
			drawn += glm::vec2(v.x, v.y);
		}
		if (i != pp.first + pp.count)
			continue;
		drawn /= (float)pp.count;
		const float w = 1.f / ctx.verts[pp.first].z;
		for (u32 j = pp.first; j < pp.first + pp.count; j++)
		{
			Vertex& v = ctx.verts[j];
			const float wj = 1.f / v.z;
			const float t = wj < hudW ? (wj - hudW) / (hudW - prof->snapMinW) : (wj - hudW) / (prof->snapMaxW - hudW);
			v.z = 1.f / (hudW + 0.0015f * std::clamp(t, -1.f, 1.f));
			v.x = centre.x + (v.x - centre.x) / unwiden.x;
			v.y = centre.y + (v.y - centre.y) / unwiden.y;
		}
		if (logFirst && placed == 0)
			NOTICE_LOG(RENDERER, "VR: sight (tex %06x, W %.4f, %u verts) drawn at %.1f,%.1f put on the 2D plane at %.1f,%.1f (focal %.1f, stock %.1f)",
					tex, w, pp.count, drawn.x, drawn.y, centre.x + (drawn.x - centre.x) / unwiden.x,
					centre.y + (drawn.y - centre.y) / unwiden.y, unwiden.y * (float)config::VrFocal, (float)config::VrFocal);
		placed++;
		cropToScreen(ctx, pp, (float)fbWidth, (float)fbHeight);
		for (u32 j = pp.first; j < pp.first + pp.count; j++)
		{
			Vertex& v = ctx.verts[j];
			if (v.x < 0.f || v.x > fbWidth || v.y < 0.f || v.y > fbHeight)
				v.x = v.y = std::numeric_limits<float>::quiet_NaN();
		}
	}
}

// A textured polygon whose texture lies on the screen texel for pixel, upright or turned a
// quarter (the map from texels to pixels over its first triangle: one 1 per row, no scale).
static bool texelForPixel(const rend_context& ctx, const PolyParam& pp)
{
	if (!pp.pcw.Texture || pp.tcw.StrideSel)
		return false;
	const float tw = (float)(8 << pp.tsp.TexU);
	const float th = pp.tcw.MipMapped ? tw : (float)(8 << pp.tsp.TexV);
	const Vertex& a = ctx.verts[pp.first];
	const Vertex& b = ctx.verts[pp.first + 1];
	const Vertex& c = ctx.verts[pp.first + 2];
	// texels (s, t) and pixels (x, y) from the first corner to the other two
	const float s1 = (b.u - a.u) * tw, s2 = (c.u - a.u) * tw;
	const float t1 = (b.v - a.v) * th, t2 = (c.v - a.v) * th;
	const float det = s1 * t2 - s2 * t1;
	if (!(std::abs(det) > 1e-3f))
		return false;
	const float x1 = b.x - a.x, x2 = c.x - a.x, y1 = b.y - a.y, y2 = c.y - a.y;
	// pixels per texel: [x1 x2; y1 y2] times the inverse of [s1 s2; t1 t2]
	const float jxs = (x1 * t2 - x2 * t1) / det, jxt = (x2 * s1 - x1 * s2) / det;
	const float jys = (y1 * t2 - y2 * t1) / det, jyt = (y2 * s1 - y1 * s2) / det;
	auto one = [](float v) { return std::abs(std::abs(v) - 1.f) < 0.01f; };
	auto zero = [](float v) { return std::abs(v) < 0.01f; };
	return (one(jxs) && one(jyt) && zero(jxt) && zero(jys)) || (one(jxt) && one(jys) && zero(jxs) && zero(jyt));
}

// 2D the game draws far from its 2D plane's W, at any W. HOTD2 pauses with "1P PAUSE" at W
// 0.0006 (depth test always: on top of everything), its sight at W 0.4, the attract's date
// card over a backdrop at W 2, and its menus and the route map over full-screen pictures at
// W 200 and 100 with real 3D in front of them (the mode drum at W 78-88, the map's markers
// at W 90-101). Through the 3D path such a layer came out as big as the widening (three
// times) and, close to the eyes, bent by the comfort zone. What gives it away: one W over
// the polygon, every corner on a whole pixel and its texture drawn texel for pixel, upright
// (the game's own sprite and text routines). Whole pixels alone are not enough: The Maze of
// the Kings' corridor walls facing the camera land on them in its widened view (focal 240),
// but never texel for pixel; HOTD2's projected 3D does neither (both checked over rips of
// the whole game, stock and widened, on the PC). Or the depth test "always" well in front
// of the plane. Such a layer
//  - in front of the depth the 2D plane is shown at (vr.HudDepth): onto the plane, in its
//    order (in front of the plane's layers, or behind them), so it sits where all the
//    game's other 2D is;
//  - behind it (a full-screen picture with 3D in front of it): kept at its W, so what the
//    game draws in front of it stays in front, but scaled to the stock framing (by the
//    game's focal over the stock one) and cut to the screen, so it is as big as on the TV.
// (The background plane, the first opaque polygon, is never one of them.)
static void place2DLayers(rend_context& ctx, const std::vector<PolyParam>& list, u32 from, u32 to, float hudW,
		int fbWidth, int fbHeight, glm::vec2 unwiden)
{
	const GameProfile *prof = activeProfile;
	const float minW = prof != nullptr ? prof->snapMinW : SnapMinW;
	const float SnapMaxW = prof != nullptr ? prof->snapMaxW : 1.05f;
	const float hudDepth = panelDepth();
	const glm::vec2 centre(fbWidth * 0.5f, fbHeight * 0.5f);
	for (u32 n = from; n < to && n < list.size(); n++)
	{
		const PolyParam& pp = list[n];
		if (pp.count < 3 || pp.first + pp.count > ctx.verts.size())
			continue;
		const float z = ctx.verts[pp.first].z;
		if (!(z > 0.f) || !std::isfinite(z))
			continue;
		const float w = 1.f / z;
		if (std::abs(w - hudW) < 0.002f)
			continue;	// on the plane already
		bool aligned = true, onScreen = true;
		u32 i = pp.first;
		for (; i < pp.first + pp.count; i++)
		{
			const Vertex& v = ctx.verts[i];
			if (std::abs(v.z - z) > z * 1e-5f || !std::isfinite(v.x) || !std::isfinite(v.y))
				break;	// not flat (3D), or hidden
			// (far out, every float is a whole number: the game's clamped near-plane corners)
			aligned = aligned && std::abs(v.x) < 4096.f && std::abs(v.y) < 4096.f
					&& std::abs(v.x - std::round(v.x)) < 0.01f && std::abs(v.y - std::round(v.y)) < 0.01f;
			onScreen = onScreen && v.x >= -1.f && v.x <= fbWidth + 1.f && v.y >= -1.f && v.y <= fbHeight + 1.f;
		}
		if (i != pp.first + pp.count)
			continue;
		// (SnapMinW, not the profile's snapMinW, on purpose: "well in front of the plane" for
		// every game, whatever band its layers are snapped from)
		const bool onTop = pp.isp.DepthMode == 7 && w < SnapMinW && onScreen;
		const bool sprite = aligned && texelForPixel(ctx, pp);
		if (!sprite && !onTop)
			continue;
		const char *how;
		if (w < hudDepth)
		{
				const float snapped = snappedOverlayDepth(w,hudW,minW,SnapMaxW);
			for (u32 j = pp.first; j < pp.first + pp.count; j++)
				ctx.verts[j].z = 1.f / snapped;
			how = "put on the 2D plane";
		}
		else
		{
			cropToScreen(ctx, pp, (float)fbWidth, (float)fbHeight);
			if (!std::isfinite(ctx.verts[pp.first].x))
				continue;	// off the screen: gone
			for (u32 j = pp.first; j < pp.first + pp.count; j++)
			{
				Vertex& v = ctx.verts[j];
				v.x = centre.x + (v.x - centre.x) * unwiden.x;
				v.y = centre.y + (v.y - centre.y) * unwiden.y;
			}
			how = "kept at its W, at the stock framing";
		}
		// what gets placed, once per W (in 0.01 steps, a few hundred), to check nothing 3D is caught
		static std::unordered_set<int> seen;
		const int bucket = (int)std::lround(w * 100.f);
		if (seen.size() < 256 && seen.insert(bucket).second)
			NOTICE_LOG(RENDERER, "VR: 2D layer at W %.4f (%u verts at %.0f,%.0f, tex %06x%s) %s",
					w, pp.count, ctx.verts[pp.first].x, ctx.verts[pp.first].y, pp.pcw.Texture ? pp.tcw.TexAddr << 3 : 0,
					sprite ? "" : ", depth test always", how);
	}
}

// A screen without any 3D (a menu the game draws in layers of depth: W 2..10 in The Maze of
// the Kings) when the profile says so (flatScreens): all of it on the 2D plane, its layers
// kept in their order. True when it was one.
static bool snapFlatScreen(rend_context& ctx, const RenderPass& pass, const RenderPass& prev, float hudW)
{
	if (activeProfile == nullptr || !activeProfile->flatScreens)
		return false;
	bool flat = true;
	float wMax = 0.f, zMin = 1e30f, zMax = 0.f;
	u32 polys = 0;
	auto each = [&](auto&& fn) {
		auto list = [&](const std::vector<PolyParam>& polys, u32 from, u32 to) {
			for (u32 n = from; n < to && n < polys.size(); n++)
			{
				const PolyParam& pp = polys[n];
				if (pp.count >= 3 && pp.first + pp.count <= ctx.verts.size())
					fn(pp);
			}
		};
		list(ctx.global_param_op, std::max(prev.op_count, 1u), pass.op_count);
		list(ctx.global_param_pt, prev.pt_count, pass.pt_count);
		list(ctx.global_param_tr, prev.tr_count, pass.tr_count);
	};
	each([&](const PolyParam& pp) {
		const float z = ctx.verts[pp.first].z;
		if (!(z > 0.f) || !std::isfinite(z))
			return;
		for (u32 i = pp.first; i < pp.first + pp.count; i++)
			if (std::abs(ctx.verts[i].z - z) > z * 1e-5f)
				flat = false;
		wMax = std::max(wMax, 1.f / z);
		zMin = std::min(zMin, z);
		zMax = std::max(zMax, z);
		polys++;
	});
	if (!flat || polys == 0 || wMax <= hudW + 0.01f)
		return false;
	each([&](const PolyParam& pp) {
		const float z = ctx.verts[pp.first].z;
		if (!(z > 0.f) || !std::isfinite(z))
			return;
		// Preserve the library's inverse-depth spacing on Lupin's all-flat screens.
		// Compressing W 0.5..100 linearly collapsed neighbouring font layers to
		// the same depth-buffer value and left holes in the title and captions.
		const float t = activeProfile->snapInverseDepth && zMax > zMin
				? 2.f * (zMax - z) / (zMax - zMin) - 1.f
				: std::clamp((1.f / z - hudW) / (wMax - hudW), -1.f, 1.f);
		const float snapped = 1.f / (hudW + std::min(0.0015f,hudW*.5f) * t);
		for (u32 i = pp.first; i < pp.first + pp.count; i++)
			ctx.verts[i].z = snapped;
	});
	static u32 logged;
	if (logged++ % 600 == 0)
		NOTICE_LOG(RENDERER, "VR: a screen without 3D (%u polygons, W up to %.1f) put on the 2D plane", polys, wMax);
	return true;
}

// The cinematic letterbox: opaque black polygons on the 2D plane. Whole polygons only, so
// one with just some black corners (a text box, a fade) is never torn apart, as the old
// per-vertex hiding in the vertex shader did. A game's profile can name the bars' textures,
// for bars drawn in another colour with a texture that blackens them (counted in barsHidden).
static void hideLetterbox(rend_context& ctx, const std::vector<PolyParam>& list, u32 from, u32 to, float hudW, u32& barsHidden)
{
	const GameProfile *prof = activeProfile;
	if (prof != nullptr && prof->letterboxW > 0.f)
		hudW = prof->letterboxW;
	for (u32 n = from; n < to && n < list.size(); n++)
	{
		const PolyParam& pp = list[n];
		if (pp.count < 3 || pp.first + pp.count > ctx.verts.size())
			continue;
		const u32 tex = pp.pcw.Texture ? pp.tcw.TexAddr << 3 : 0;
		const bool bar = prof != nullptr && tex != 0
				&& std::find(std::begin(prof->letterboxTextures), std::end(prof->letterboxTextures), tex) != std::end(prof->letterboxTextures);
		u32 i = pp.first;
		while (i < pp.first + pp.count && onOverlay(ctx.verts[i], hudW) && (bar || opaqueBlack(ctx.verts[i])))
			i++;
		if (i == pp.first + pp.count)
		{
			hideVertices(ctx, pp);
			barsHidden += bar;
		}
	}
}

// Player 2's start prompt with the credit count under it, in the bottom right corner all
// through a one-player game: in the headset it floats in the room for nothing (asked for on
// Reddit, twice). Its pictures go when the game profile knows them (vr.HideP2Prompt), and so
// does what's on the line right after them (the number), unless the profile turns that off
// (p2HideTrailing). Player 1's own, bottom left, stays, and so do the story's subtitles.
static std::atomic<int64_t> p2PromptAt { -1000000 };

static void hidePlayer2Prompt(rend_context& ctx, const RenderPass& pass, const RenderPass& prev, float hudW,
		int fbWidth, int fbHeight)
{
	if (p2PromptTextures[0] == 0)
		return;
	// Lupin shares this atlas/box with P2 feedback. Hide only the measured P1-only
	// join message; waiting screens and an active P2 retain all their own glyphs.
	if (activeProfile != nullptr && activeProfile->p2PromptSinglePlayerOnly
			&& (!vmPlayersKnown.load() || vmPlayers.load() != 1))
		return;
	if (activeProfile != nullptr && activeProfile->p2PromptIdleAddr != 0)
	{
		u32 life = 0;
		if (!vmCount(activeProfile->p2PromptIdleAddr, 2, life) || life != 0)
			return;
	}
	// (not online: player 2 is a real player there, and on the guest it's the player's own)
	const bool hide = config::VrHideP2Prompt && !config::GGPOEnable;
	struct Box { float x0, y0, x1, y1; };
	auto flat = [&](const PolyParam& pp, Box& b) {
		if (pp.count < 3 || pp.first + pp.count > ctx.verts.size())
			return false;
		b = { 1e9f, 1e9f, -1e9f, -1e9f };
		for (u32 i = pp.first; i < pp.first + pp.count; i++)
		{
			const Vertex& v = ctx.verts[i];
			if (!(v.z > 0.f) || std::abs(1.f / v.z - hudW) > 0.05f || !std::isfinite(v.x) || !std::isfinite(v.y))
				return false;
			b = { std::min(b.x0, v.x), std::min(b.y0, v.y), std::max(b.x1, v.x), std::max(b.y1, v.y) };
		}
		return true;
	};
	auto each = [&](auto&& fn) {
		auto list = [&](const std::vector<PolyParam>& polys, u32 from, u32 to) {
			for (u32 n = from; n < to && n < polys.size(); n++)
				fn(polys[n]);
		};
		list(ctx.global_param_op, std::max(prev.op_count, 1u), pass.op_count);
		list(ctx.global_param_pt, prev.pt_count, pass.pt_count);
		list(ctx.global_param_tr, prev.tr_count, pass.tr_count);
	};
	Box gone[4];
	int count = 0;
	each([&](const PolyParam& pp) {
		Box b;
		const u32 tex = pp.pcw.Texture ? pp.tcw.TexAddr << 3 : 0;
		const ScreenBox& at = activeProfile != nullptr ? activeProfile->p2PromptBox : ScreenBox { 352.f, 384.f, 640.f, 480.f };
		const float sx = fbWidth / 640.f, sy = fbHeight / 480.f;
		if (tex == 0 || std::find(std::begin(p2PromptTextures), std::end(p2PromptTextures), tex) == std::end(p2PromptTextures)
				|| !flat(pp, b) || b.x0 < at.x0 * sx || b.y0 < at.y0 * sy || b.x1 > at.x1 * sx || b.y1 > at.y1 * sy
				|| (activeProfile != nullptr && (b.x1 - b.x0 < activeProfile->p2PromptMinSize.x * sx
						|| b.y1 - b.y0 < activeProfile->p2PromptMinSize.y * sy)))
			return;
		if (hide)
			hideVertices(ctx, pp);
		if (count < (int)std::size(gone))
			gone[count++] = b;
	});
	if (count == 0)
		return;
	// (seen: player 2 isn't playing, see xr_host.cpp's dual wielding)
	p2PromptAt = steadyMs();
	if (!hide)
		return;
	const GameProfile *prof = activeProfile;
	if (prof != nullptr && !prof->p2HideTrailing)
		return;
	each([&](const PolyParam& pp) {
		Box b;
		const u32 tex = pp.pcw.Texture ? pp.tcw.TexAddr << 3 : 0;
		// (never a sight the game draws over an enemy there: see placeSights)
		if (!flat(pp, b) || (prof != nullptr && tex != 0
				&& std::find(std::begin(prof->sightTextures), std::end(prof->sightTextures), tex) != std::end(prof->sightTextures)))
			return;
		for (int k = 0; k < count; k++)
			if (b.x0 >= gone[k].x0 && b.x1 <= gone[k].x1 + fbWidth * 0.12f && b.y0 >= gone[k].y0 - 4.f && b.y1 <= gone[k].y1 + 4.f)
			{
				hideVertices(ctx, pp);
				break;
			}
	});
}

bool player2Prompting() {
	if (activeProfile != nullptr && (activeProfile->gunRam.floats || activeProfile->ninja))
		return (vmPlayers.load() & 2) == 0;
	return steadyMs() - p2PromptAt < 500;
}

// The game's cutscenes (cutscene()): its letterbox bars, the profile's letterboxTextures as
// hideLetterbox counts them (CM's 400680 at W 0.2, drawn in its cutscenes only), in the frames the
// game draws while a player is in the game (gunRam.playing: the attract's scenes have the bars too,
// with no player in). Each VBlank after a drawn frame (cutscenePasses: the passes parsed since the
// last one) closes that frame: on after CutsceneOnFrames frames in a row with the bars (~0.3 s), off
// at the CutsceneOffFrames-th frame without them (6, ~0.1 s: the hall's entrance blinks for up to 2
// frames; a Start that skips a scene brings the guns back at once). Counted per drawn frame, not by
// the clock, so a slow frame doesn't end a scene. See hotd2-vr/notes/confmiss-findings.md, Cutscenes.
constexpr u32 CutsceneOnFrames = 18, CutsceneOffFrames = 6;
static std::atomic<u32> cutscenePasses { 0 }, cutsceneBars { 0 };
static std::atomic<bool> cutsceneOn { false };

static void closeCutsceneFrame(Event, void *)
{
	static const GameProfile *profWas;
	static u32 withBars, without;
	const GameProfile *prof = activeProfile;
	if (prof != profWas)
	{
		profWas = prof;
		withBars = without = 0;
		cutsceneOn = false;
	}
	if (prof == nullptr || (prof->letterboxTextures[0] == 0 && !prof->movieCutscenes) || (prof->gunRam.playing == 0 && !prof->gunRam.floats) || cutscenePasses.exchange(0) == 0)
		return;
	const bool bars = cutsceneBars.exchange(0) > 0;
	if (bars)
	{
		withBars++;
		without = 0;
	}
	else if (++without >= CutsceneOffFrames)
		withBars = 0;
	// (the players' byte holds their two bits only: anything else is the RAM before the game clears it)
	const u8 playing = prof->gunRam.playing != 0 ? ReadMem8_nommu(0x8C000000u | (prof->gunRam.playing & 0x01FFFFFFu)) : 0;
	const bool inGame = prof->gunRam.floats ? vmPlayers.load() != 0 : (playing & ~3) == 0 && (playing & 3) != 0;
	const bool on = inGame && withBars >= CutsceneOnFrames;
	if (on != cutsceneOn)
	{
		cutsceneOn = on;
		NOTICE_LOG(RENDERER, "VR: cutscene %s", on ? "on" : "off");
	}
}

static struct CutsceneRegistration
{
	CutsceneRegistration() {
		EventManager::listen(Event::VBlank, closeCutsceneFrame);
	}
} cutsceneRegistration;

bool cutscene() {
	if (activeProfile != nullptr && activeProfile->ninja) return ninjaStory.load();
	return cutsceneOn;
}

// The prompt's comings and goings logged, so PC runs show what the headset's join and leave
// go by (see xr_host.cpp's dual wielding). Held for a second after its last frame, not half
// a second: the prompts blink (HOTD2's about 1.05 s on and 0.53-0.56 s off, CM's 0.5 s on
// and 0.52-0.56 s off), and with half a second the log came and went with each blink. (The
// dual wielding acts only on player2Prompting() being true, so its gaps don't matter there.)
static void logPlayer2Prompt()
{
	static bool prompting;
	if ((steadyMs() - p2PromptAt < 1000) == prompting)
		return;
	prompting = !prompting;
	NOTICE_LOG(RENDERER, "VR: player 2 prompt %s", prompting ? "shown" : "gone");
}

bool player2PromptKnown() {
	return p2PromptTextures[0] != 0 || (activeProfile != nullptr && (activeProfile->gunRam.floats || activeProfile->ninja) && vmPlayersKnown);
}

// A 3D polygon with some of its corners at the 2D plane's W (a game whose near plane is
// there) would have just those corners pinned onto the 2D plane by the headset's vertex
// shader, stretched across the scene. They're moved a hair off it along their rays (0.25%)
// instead, and stay 3D.
static void keepMixedIn3D(rend_context& ctx, const std::vector<PolyParam>& list, u32 from, u32 to, float hudW)
{
	for (u32 n = from; n < to && n < list.size(); n++)
	{
		const PolyParam& pp = list[n];
		if (pp.count < 3 || pp.first + pp.count > ctx.verts.size())
			continue;
		u32 on = 0;
		for (u32 i = pp.first; i < pp.first + pp.count; i++)
			on += onOverlay(ctx.verts[i], hudW);
		if (on == 0 || on == pp.count)
			continue;
		for (u32 i = pp.first; i < pp.first + pp.count; i++)
		{
			Vertex& v = ctx.verts[i];
			if (onOverlay(v, hudW))
				v.z = 1.f / (1.f / v.z < hudW ? hudW - 0.0025f : hudW + 0.0025f);
		}
		static u32 logged;
		if (logged++ % 600 == 0)
			NOTICE_LOG(RENDERER, "VR: kept a 3D polygon with %u of %u corners at the 2D plane's W in 3D", on, pp.count);
	}
}

// PC: overlay-like polygons the headset's vertex shader could bend (mixed overlay and 3D
// W, or 3D right next to the overlay W) or that only partly are letterbox black.
static void logOverlayCensus(const rend_context& ctx, const RenderPass& pass, const RenderPass& prev, float hudW)
{
	int lines = 0;
	auto each = [&](const std::vector<PolyParam>& list, u32 from, u32 to) {
		for (u32 n = from; n < to && n < list.size() && lines < 20; n++)
		{
			const PolyParam& pp = list[n];
			if (pp.count < 3 || pp.first + pp.count > ctx.verts.size())
				continue;
			u32 hud = 0, nearW = 0, black = 0;
			float wMin = 1e30f, wMax = 0.f;
			for (u32 i = pp.first; i < pp.first + pp.count; i++)
			{
				const Vertex& v = ctx.verts[i];
				const float w = v.z > 0.f ? 1.f / v.z : 0.f;
				wMin = std::min(wMin, w);
				wMax = std::max(wMax, w);
				const bool overlay = onOverlay(v, hudW);
				hud += overlay;
				nearW += !overlay && w >= 0.5f && w < 4.f;
				black += opaqueBlack(v);
			}
			const char *what = hud > 0 && hud < pp.count ? "mixed 2D/3D W"
					: nearW == pp.count ? "3D right next to the 2D W"
					: hud == pp.count && black > 0 && black < pp.count ? "2D with some black corners" : nullptr;
			if (what == nullptr)
				continue;
			lines++;
			NOTICE_LOG(RENDERER, "VR census: %s, at %.0f,%.0f, %u verts, W %.3f..%.3f, black %u",
					what, ctx.verts[pp.first].x, ctx.verts[pp.first].y, pp.count, wMin, wMax, black);
		}
	};
	each(ctx.global_param_op, std::max(prev.op_count, 1u), pass.op_count);
	each(ctx.global_param_pt, prev.pt_count, pass.pt_count);
	each(ctx.global_param_tr, prev.tr_count, pass.tr_count);
}

// PC diagnostics: textures that show up within half a second of a shot and weren't drawn
// in the second before it - the game's hit effects (sparks, blood, its hit marker) - once
// per texture per shot, with where and how they're drawn.
static void logShotTextures(const rend_context& ctx, const RenderPass& pass, const RenderPass& prev, int fbWidth, int fbHeight)
{
	static std::unordered_map<u32, u32> lastSeen;	// texture -> parse number
	static std::unordered_set<u32> before, reported;
	static u32 parse;
	static int64_t snapFor = std::numeric_limits<int64_t>::min();
	parse++;
	// the newest shot
	int64_t at = std::numeric_limits<int64_t>::min();
	glm::vec2 gun(0.f);
	for (GunShot& s : gunShots)
	{
		const int64_t ms = s.ms;
		const u32 packed = s.pos;
		if (s.ms == ms && ms > at)
		{
			at = ms;
			gun = glm::vec2((packed & 0xffff) * fbWidth / 640.f, (packed >> 16) * fbHeight / 480.f);
		}
	}
	const int64_t since = steadyMs() - at;
	const bool window = since >= 0 && since <= 500;
	const glm::vec2 drawn = shotDrawnAt(gun, fbWidth, fbHeight);
	if (window && snapFor != at)
	{
		snapFor = at;
		before.clear();
		reported.clear();
		for (const auto& [tex, seen] : lastSeen)
			if (parse - seen <= 60)
				before.insert(tex);
	}
	auto each = [&](const std::vector<PolyParam>& list, u32 from, u32 to, const char *name) {
		for (u32 n = from; n < to && n < list.size(); n++)
		{
			const PolyParam& pp = list[n];
			const u32 tex = pp.pcw.Texture ? pp.tcw.TexAddr << 3 : 0xffffffffu;
			if (window && !before.count(tex) && !reported.count(tex) && pp.count > 0 && pp.first + pp.count <= ctx.verts.size())
			{
				reported.insert(tex);
				glm::vec2 lo(1e30f), hi(-1e30f);
				float wMin = 1e30f, wMax = 0.f;
				for (u32 i = pp.first; i < pp.first + pp.count; i++)
				{
					const Vertex& v = ctx.verts[i];
					if (!std::isfinite(v.x) || !std::isfinite(v.y))
						continue;
					lo = glm::min(lo, glm::vec2(v.x, v.y));
					hi = glm::max(hi, glm::vec2(v.x, v.y));
					const float w = v.z > 0.f ? 1.f / v.z : 0.f;
					wMin = std::min(wMin, w);
					wMax = std::max(wMax, w);
				}
				const u8 *c = ctx.verts[pp.first].col;
				NOTICE_LOG(RENDERER, "VR new after shot +%d ms gun %.0f,%.0f: %s %u tex %06x tsp %08x isp %08x tcw %08x, %u verts at %.0f,%.0f..%.0f,%.0f (centre %.0f px from the gun) W %.2f..%.2f col %02x%02x%02x%02x, shot drawn at %.0f,%.0f",
						(int)since, gun.x, gun.y, name, n, tex, pp.tsp.full, pp.isp.full, pp.tcw.full, pp.count, lo.x, lo.y, hi.x, hi.y,
						glm::distance((lo + hi) * 0.5f, gun), wMin, wMax, c[0], c[1], c[2], c[3], drawn.x, drawn.y);
			}
			lastSeen[tex] = parse;
		}
	};
	each(ctx.global_param_op, std::max(prev.op_count, 1u), pass.op_count, "OP");
	each(ctx.global_param_pt, prev.pt_count, pass.pt_count, "PT");
	each(ctx.global_param_tr, prev.tr_count, pass.tr_count, "TR");
}

//
// PC: "rip.request" in the working directory dumps the next render pass as the TA got it,
// for hotd2-vr/assets/rip_hands.py to lift into 3D models (models from your own copy of the
// game, never shipped): rip/passNNNN.bin with every polygon and vertex, plus the VRAM and
// palette when they changed. The request file may hold "count interval ram texture polys":
// that many passes, that many passes apart, with ram 1 also the main RAM; with a texture
// (VRAM address, hex) only from the first pass with at least that many polygons using it
// (hotd2-vr/rip-hands.ps1 waits for the agent's pistol that way).
//
static void ripRequested(const rend_context& ctx, const RenderPass& pass, const RenderPass& prev)
{
	static u32 left, every, wait, serial, withRam, waitTexture, waitPolys;
	static u64 vramHash, ramHash;
	if (left == 0)
	{
		static u32 polls;
		if (polls++ % 15 != 0)
			return;
		FILE *req = fopen("rip.request", "r");
		if (req == nullptr)
			return;
		left = 1;
		every = 1;
		withRam = 0;
		waitTexture = 0;
		waitPolys = 1;
		if (fscanf(req, "%u %u %u %x %u", &left, &every, &withRam, &waitTexture, &waitPolys) < 1)
			left = 1;
		fclose(req);
		remove("rip.request");
		left = std::clamp(left, 1u, 1000u);
		every = std::max(every, 1u);
		wait = 0;
		make_directory("rip");
		if (waitTexture != 0)
			NOTICE_LOG(RENDERER, "VR: rip waiting for texture %06x", waitTexture);
	}
	if (waitTexture != 0)
	{
		u32 found = 0;
		auto count = [&](const std::vector<PolyParam>& polys, u32 from, u32 to) {
			for (u32 k = from; k < to && k < polys.size(); k++)
				found += polys[k].pcw.Texture && (polys[k].tcw.TexAddr << 3) == waitTexture;
		};
		count(ctx.global_param_op, std::max(prev.op_count, 1u), pass.op_count);
		count(ctx.global_param_pt, prev.pt_count, pass.pt_count);
		count(ctx.global_param_tr, prev.tr_count, pass.tr_count);
		if (found < waitPolys)
			return;
		waitTexture = 0;
	}
	if (wait > 0)
	{
		wait--;
		return;
	}
	left--;
	wait = every - 1;
	const u32 n = serial++;
	auto hash = [](RamRegion& mem, size_t size) {
		u64 h = 1469598103934665603ull;
		for (size_t i = 0; i < size; i += 8)
		{
			u64 w;
			memcpy(&w, &mem[i], 8);
			h = (h ^ w) * 1099511628211ull;
		}
		return h;
	};
	auto dump = [](RamRegion& mem, size_t size, const char *path) {
		if (FILE *f = fopen(path, "wb"))
		{
			fwrite(&mem[0], 1, size, f);
			fclose(f);
		}
	};
	char path[64];
	const u64 vh = hash(vram, VRAM_SIZE);
	if (vh != vramHash)
	{
		vramHash = vh;
		snprintf(path, sizeof(path), "rip/vram%04u.bin", n);
		dump(vram, VRAM_SIZE, path);
		snprintf(path, sizeof(path), "rip/pal%04u.bin", n);
		if (FILE *f = fopen(path, "wb"))
		{
			const u32 ctrl = PAL_RAM_CTRL;
			fwrite(&ctrl, 4, 1, f);
			fwrite(PALETTE_RAM, 4, 1024, f);
			fclose(f);
		}
	}
	const u64 rh = withRam ? hash(mem_b, RAM_SIZE) : ramHash;
	if (rh != ramHash)
	{
		ramHash = rh;
		snprintf(path, sizeof(path), "rip/ram%04u.bin", n);
		dump(mem_b, RAM_SIZE, path);
	}
	snprintf(path, sizeof(path), "rip/pass%04u.bin", n);
	FILE *f = fopen(path, "wb");
	if (f == nullptr)
		return;
	int fbWidth, fbHeight;
	getPvrFramebufferSize(ctx, fbWidth, fbHeight);
	const glm::vec2 focal = readGameFocal();
	struct { char magic[4]; u32 fbWidth, fbHeight; float focalX, focalY, hudW, stockFocal; u32 lists; } head = {
		{ 'R', 'I', 'P', '1' }, (u32)fbWidth, (u32)fbHeight, focal.x, focal.y, (float)config::VrHudW, (float)config::VrFocal, 3 };
	fwrite(&head, sizeof(head), 1, f);
	auto list = [&](const std::vector<PolyParam>& polys, u32 from, u32 to) {
		to = std::min<u32>(to, (u32)polys.size());
		const u32 count = to > from ? to - from : 0;
		fwrite(&count, 4, 1, f);
		for (u32 k = from; k < to; k++)
		{
			const PolyParam& pp = polys[k];
			const u32 nverts = pp.first + pp.count <= ctx.verts.size() ? pp.count : 0;
			const u32 words[5] = { pp.isp.full, pp.tsp.full, pp.tcw.full, pp.pcw.full, nverts };
			fwrite(words, 4, 5, f);
			for (u32 i = pp.first; i < pp.first + nverts; i++)
			{
				const Vertex& v = ctx.verts[i];
				const float xyzuv[5] = { v.x, v.y, v.z, v.u, v.v };
				fwrite(xyzuv, 4, 5, f);
				fwrite(v.col, 1, 4, f);
				fwrite(v.spc, 1, 4, f);
			}
		}
	};
	list(ctx.global_param_op, std::max(prev.op_count, 1u), pass.op_count);
	list(ctx.global_param_pt, prev.pt_count, pass.pt_count);
	list(ctx.global_param_tr, prev.tr_count, pass.tr_count);
	fclose(f);
	NOTICE_LOG(RENDERER, "VR: ripped pass %u (%u to go)", n, left);
}

// The game's clamped near-plane corners. A vertex behind or right at its camera isn't
// clipped by HOTD2: its W is pinned (to 0.001; The Maze of the Kings: 1.0) and its x and y
// shoot off to millions of pixels. On a TV such a polygon is at most a sliver at the edge of
// the screen: the hardware fills its screen-space triangle, 1/W linear across it. Lifted into
// 3D the pinned corner lands thousands of units to the side at the eyes' depth, and from a
// head that moved, or a view three times wider, the sliver is a wedge across the room, or a
// zombie's arm torn in two for a frame (5% of HOTD2's polygons, in two thirds of its frames:
// the floor under the camera, walls beside it, enemies in your face). So the polygon is
// clipped in screen space as the hardware does, to a box twice the screen (the 2D plane's
// layers: to the screen), 1/W and u/W, v/W, colour/W linear: the new corners are what the
// TV would show there. The pieces go into a new strip at the end of the vertex list, NaN
// vertices between them (makePrimRestartIndex splits a strip there; a piece starts at an
// even position, which keeps the culling parity).
static void clipFarOut(rend_context& ctx, std::vector<PolyParam>& list, u32 from, u32 to, float hudW,
		int fbWidth, int fbHeight, u32& clippedPolys)
{
	struct P {
		double x, y, z;			// screen pixels, 1/W
		double uz[4];			// u, v, u1, v1 times z
		double cz[16];			// col, spc, col1, spc1 times z
	};
	auto load = [](const Vertex& v, P& p) {
		p.x = v.x;
		p.y = v.y;
		p.z = v.z;
		const float uv[4] = { v.u, v.v, v.u1, v.v1 };
		for (int k = 0; k < 4; k++)
			p.uz[k] = uv[k] * p.z;
		const u8 *c[4] = { v.col, v.spc, v.col1, v.spc1 };
		for (int k = 0; k < 16; k++)
			p.cz[k] = c[k / 4][k % 4] * p.z;
	};
	auto store = [hudW](const P& p, Vertex& v) {
		v.x = (float)p.x;
		v.y = (float)p.y;
		v.z = (float)p.z;
		// (a new corner landing at the 2D plane's W would be pinned onto the plane by the
		// headset's vertex shader: a hair off it, as keepMixedIn3D does)
		if (v.z > 0.f && std::abs(1.f / v.z - hudW) < 0.002f)
			v.z = 1.f / (1.f / v.z < hudW ? hudW - 0.0025f : hudW + 0.0025f);
		const double z = p.z > 1e-30 ? p.z : 1e-30;
		v.u = (float)(p.uz[0] / z);
		v.v = (float)(p.uz[1] / z);
		v.u1 = (float)(p.uz[2] / z);
		v.v1 = (float)(p.uz[3] / z);
		u8 *c[4] = { v.col, v.spc, v.col1, v.spc1 };
		for (int k = 0; k < 16; k++)
			c[k / 4][k % 4] = (u8)std::clamp((int)std::lround(p.cz[k] / z), 0, 255);
	};
	auto lerp = [](const P& a, const P& b, double t, P& o) {
		o.x = a.x + (b.x - a.x) * t;
		o.y = a.y + (b.y - a.y) * t;
		o.z = a.z + (b.z - a.z) * t;
		for (int k = 0; k < 4; k++)
			o.uz[k] = a.uz[k] + (b.uz[k] - a.uz[k]) * t;
		for (int k = 0; k < 16; k++)
			o.cz[k] = a.cz[k] + (b.cz[k] - a.cz[k]) * t;
	};
	// Sutherland-Hodgman against one edge of the box: inside is side * coord <= side * bound
	auto clipEdge = [&](const std::vector<P>& in, std::vector<P>& out, int axis, double bound, double side) {
		out.clear();
		const size_t n = in.size();
		for (size_t i = 0; i < n; i++)
		{
			const P& a = in[i];
			const P& b = in[(i + 1) % n];
			const double da = side * ((axis == 0 ? a.x : a.y) - bound);	// > 0: outside
			const double db = side * ((axis == 0 ? b.x : b.y) - bound);
			if (da <= 0)
				out.push_back(a);
			if ((da <= 0) != (db <= 0))
			{
				P m;
				lerp(a, b, da / (da - db), m);
				if (axis == 0)
					m.x = bound;
				else
					m.y = bound;
				out.push_back(m);
			}
		}
	};
	std::vector<P> poly, tmp;
	const Vertex nan = [] { Vertex v {}; v.x = v.y = std::numeric_limits<float>::quiet_NaN(); return v; }();
	for (u32 n = from; n < to && n < list.size(); n++)
	{
		PolyParam& pp = list[n];
		if (pp.count < 3 || pp.first + pp.count > ctx.verts.size())
			continue;
		bool onPlane = true, farOut = false, finite = true;
		for (u32 i = pp.first; i < pp.first + pp.count; i++)
		{
			const Vertex& v = ctx.verts[i];
			if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
			{
				finite = false;
				break;
			}
			onPlane = onPlane && onOverlay(v, hudW);
		}
		if (!finite)
			continue;
		const double x0 = onPlane ? 0.0 : -fbWidth * 0.5, x1 = onPlane ? fbWidth : fbWidth * 1.5;
		const double y0 = onPlane ? 0.0 : -fbHeight * 0.5, y1 = onPlane ? fbHeight : fbHeight * 1.5;
		// (3D reaching up to two screens out is real geometry beside the view, lifted fine as
		// it is: only what reaches further is cut, the pinned corners are at thousands; for a
		// game that clips at its own near plane, only a corner nearer than it: farOutBelowW)
		const double reach = onPlane ? 0.0 : fbWidth * 1.5;
		const float keepW = !onPlane && activeProfile != nullptr ? activeProfile->farOutBelowW : 0.f;
		for (u32 i = pp.first; i < pp.first + pp.count && !farOut; i++)
		{
			const Vertex& v = ctx.verts[i];
			farOut = (v.x < x0 - reach || v.x > x1 + reach || v.y < y0 - reach || v.y > y1 + reach)
					&& (keepW <= 0.f || 1.f / v.z < keepW);
		}
		if (!farOut)
			continue;
		clippedPolys++;
		const u32 start = (u32)ctx.verts.size();
		for (u32 i = 0; i + 2 < pp.count; i++)
		{
			poly.resize(3);
			// a strip's odd triangles face the other way: in the order their facing means
			for (int k = 0; k < 3; k++)
				load(ctx.verts[pp.first + i + ((i & 1) ? 2 - k : k)], poly[k]);
			clipEdge(poly, tmp, 0, x0, -1.0);
			clipEdge(tmp, poly, 0, x1, 1.0);
			clipEdge(poly, tmp, 1, y0, -1.0);
			clipEdge(tmp, poly, 1, y1, 1.0);
			if (poly.size() < 3)
				continue;
			// a new piece at an even position, after a NaN
			if (ctx.verts.size() > start)
			{
				ctx.verts.push_back(nan);
				if ((ctx.verts.size() - start) & 1)
					ctx.verts.push_back(nan);
			}
			// the convex piece as a strip: 0, 1, n-1, 2, n-2, ... keeps its winding
			const size_t m = poly.size();
			size_t lo = 1, hi = m - 1;
			ctx.verts.push_back(ctx.verts[pp.first]);
			store(poly[0], ctx.verts.back());
			for (size_t k = 1; k < m; k++)
			{
				const size_t which = (k & 1) ? lo++ : hi--;
				ctx.verts.push_back(ctx.verts[pp.first]);
				store(poly[which], ctx.verts.back());
			}
		}
		if (ctx.verts.size() == start)
			hideVertices(ctx, pp);		// nothing of it in the box
		else
		{
			pp.first = start;
			pp.count = (u32)(ctx.verts.size() - start);
		}
	}
}

void dropShotMarker(rend_context& ctx, const RenderPass& pass, const RenderPass& previousPass)
{
	// (Naomi 2 polygons are still in model space here, and aren't reprojected anyway)
	if (!ctx.isRTT)
		cutscenePasses++;
	if (!config::VrReproject || ctx.isRTT || settings.platform.isNaomi2())
		return;
	const bool onPC = !xr::enabled();
	if (onPC)
		ripRequested(ctx, pass, previousPass);
#ifndef __ANDROID__
	watchTextures(ctx, pass, previousPass);
#endif
	hands::observePass(ctx, pass, previousPass);
	const float hudW = config::VrHudW;
	static u32 parses;
	if (onPC && parses++ % 240 == 0)
		logOverlayCensus(ctx, pass, previousPass, hudW);
	int fbWidth, fbHeight;
	getPvrFramebufferSize(ctx, fbWidth, fbHeight);
	// Lupin's story uses inset (448x288) and full-screen YUV movie quads.
	// Test original (pre-snap) geometry: flat at the measured UI band, covering
	// at least half the frame width and 5/12 of its height. Session/player gates and the same 18/6-frame debounce exclude
	// attract movies, fades and momentary frame transitions.
	if (activeProfile != nullptr && activeProfile->movieCutscenes)
	{
		bool movie = false;
		auto scan = [&](const std::vector<PolyParam>& list, u32 from, u32 to) {
			for (u32 n = from; n < to && n < list.size() && !movie; n++) {
				const PolyParam& pp = list[n];
				if (!pp.pcw.Texture || pp.tcw.PixelFmt != 3 || pp.count != 4 || pp.first + pp.count > ctx.verts.size()) continue;
				const float z = ctx.verts[pp.first].z;
				if (!(z > 0.f) || 1.f / z < activeProfile->snapMinW || 1.f / z > activeProfile->snapMaxW) continue;
				glm::vec2 lo(1e30f), hi(-1e30f); bool flat = true;
				for (u32 j = pp.first; j < pp.first + pp.count; j++) { const Vertex& v = ctx.verts[j]; flat = flat && std::isfinite(v.x) && std::isfinite(v.y) && std::abs(v.z - z) < z * 1e-5f; lo = glm::min(lo, glm::vec2(v.x,v.y)); hi = glm::max(hi, glm::vec2(v.x,v.y)); }
				movie = flat && lo.x >= -1.f && lo.y >= -1.f && hi.x <= fbWidth + 1.f && hi.y <= fbHeight + 1.f
						&& hi.x - lo.x >= fbWidth * .5f && hi.y - lo.y >= fbHeight * (5.f / 12.f);
			}
		};
		scan(ctx.global_param_op, std::max(previousPass.op_count, 1u), pass.op_count);
		scan(ctx.global_param_pt, previousPass.pt_count, pass.pt_count);
		scan(ctx.global_param_tr, previousPass.tr_count, pass.tr_count);
		if (movie) cutsceneBars++;
	}
	// The letterbox first: only what the game drew at the plane's W, as the shader used to
	// (a black text shadow on a snapped layer must stay). The first opaque polygon is the
	// background plane.
	if (config::VrHideLetterbox)
	{
		static bool barsShown;
		u32 bars = 0;
		hideLetterbox(ctx, ctx.global_param_op, std::max(previousPass.op_count, 1u), pass.op_count, hudW, bars);
		hideLetterbox(ctx, ctx.global_param_pt, previousPass.pt_count, pass.pt_count, hudW, bars);
		hideLetterbox(ctx, ctx.global_param_tr, previousPass.tr_count, pass.tr_count, hudW, bars);
		if (bars > 0 && !barsShown)
			NOTICE_LOG(RENDERER, "VR: letterbox bars hidden (%u drawn with the profile's textures at W %.3f)", bars, activeProfile->letterboxW);
		barsShown = bars > 0;
		if (bars > 0)
			cutsceneBars++;
	}
	// A shared HUD atlas needs its original depth to distinguish the shot crack.
	// Classify before snapNear2D replaces that depth. Each shot is still gated by
	// 3D aim and the measured small neighborhood around the native gun coordinate.
	if (activeProfile != nullptr && activeProfile->markerBeforeSnap && config::VrDropShotMarker)
	{
		const int64_t now = steadyMs();
		u32 dropped = 0;
		auto scan = [&](const std::vector<PolyParam>& list, u32 from, u32 to) {
			for (u32 n = from; n < to && n < list.size(); n++) {
				const PolyParam& pp = list[n];
				const u32 tex = pp.pcw.Texture ? pp.tcw.TexAddr << 3 : 0;
				if (tex == 0 || pp.count != 4 || pp.first + pp.count > ctx.verts.size()
						|| std::find(std::begin(markerTextures), std::end(markerTextures), tex) == std::end(markerTextures)) continue;
				glm::vec2 lo(1e30f), hi(-1e30f), uvLo(1e30f), uvHi(-1e30f); bool match = true;
				for (u32 j = pp.first; j < pp.first + pp.count; j++) {
					const Vertex& v = ctx.verts[j]; const float w = v.z > 0.f ? 1.f / v.z : 0.f;
					match = match && w >= activeProfile->markerMinW && w <= activeProfile->markerMaxW
							&& std::isfinite(v.x) && std::isfinite(v.y);
					lo = glm::min(lo, glm::vec2(v.x,v.y)); hi = glm::max(hi, glm::vec2(v.x,v.y));
					uvLo = glm::min(uvLo, glm::vec2(v.u,v.v)); uvHi = glm::max(uvHi, glm::vec2(v.u,v.v));
				}
				bool markerUv = false;
				for (const ScreenBox& box : activeProfile->markerUvBoxes)
					markerUv = markerUv || (box.x1 > box.x0 && box.y1 > box.y0 && uvLo.x >= box.x0 - 1e-5f
							&& uvLo.y >= box.y0 - 1e-5f && uvHi.x <= box.x1 + 1e-5f && uvHi.y <= box.y1 + 1e-5f);
				if (!markerUv || !match || hi.x - lo.x > MarkerMaxSize || hi.y - lo.y > MarkerMaxSize) continue;
				const glm::vec2 centre = (lo + hi) * .5f;
				for (GunShot& shot : gunShots) {
					const int64_t at = shot.ms; const u32 pos = shot.pos; const bool into3D = shot.into3D;
					if (shot.ms != at || !into3D || now - at < 0 || now - at > MarkerMs) continue;
					const glm::vec2 hit((pos & 0xffff) * fbWidth / 640.f, (pos >> 16) * fbHeight / 480.f);
					if (glm::distance(centre, hit) > MarkerMaxOffset) continue;
					hideVertices(ctx, pp); dropped++; break;
				}
			}
		};
		scan(ctx.global_param_tr, previousPass.tr_count, pass.tr_count);
		if (dropped > 0) NOTICE_LOG(RENDERER, "VR: hid original-depth shot marker (%u polygons)", dropped);
	}
	if (!snapFlatScreen(ctx, pass, previousPass, hudW))
	{
		// (the stock framing: the plane's focal over the one the game draws with now)
		const glm::vec2 unwiden = readGameFocal() / (float)config::VrFocal;
		static bool sightsShown;
		static u32 sightLogs;
		u32 sights = 0;
		const bool logSight = !sightsShown && sightLogs < 64;
		placeSights(ctx, ctx.global_param_op, std::max(previousPass.op_count, 1u), pass.op_count, hudW, fbWidth, fbHeight, unwiden, sights, logSight);
		placeSights(ctx, ctx.global_param_pt, previousPass.pt_count, pass.pt_count, hudW, fbWidth, fbHeight, unwiden, sights, logSight);
		placeSights(ctx, ctx.global_param_tr, previousPass.tr_count, pass.tr_count, hudW, fbWidth, fbHeight, unwiden, sights, logSight);
		sightLogs += logSight && sights > 0;
		sightsShown = sights > 0;
		snapNear2D(ctx, ctx.global_param_op, std::max(previousPass.op_count, 1u), pass.op_count, hudW, fbWidth, fbHeight);
		snapNear2D(ctx, ctx.global_param_pt, previousPass.pt_count, pass.pt_count, hudW, fbWidth, fbHeight);
		snapNear2D(ctx, ctx.global_param_tr, previousPass.tr_count, pass.tr_count, hudW, fbWidth, fbHeight);
		place2DLayers(ctx, ctx.global_param_op, std::max(previousPass.op_count, 1u), pass.op_count, hudW, fbWidth, fbHeight, unwiden);
		place2DLayers(ctx, ctx.global_param_pt, previousPass.pt_count, pass.pt_count, hudW, fbWidth, fbHeight, unwiden);
		place2DLayers(ctx, ctx.global_param_tr, previousPass.tr_count, pass.tr_count, hudW, fbWidth, fbHeight, unwiden);
	}
	hidePlayer2Prompt(ctx, pass, previousPass, hudW, fbWidth, fbHeight);
	logPlayer2Prompt();
	keepMixedIn3D(ctx, ctx.global_param_op, std::max(previousPass.op_count, 1u), pass.op_count, hudW);
	keepMixedIn3D(ctx, ctx.global_param_pt, previousPass.pt_count, pass.pt_count, hudW);
	keepMixedIn3D(ctx, ctx.global_param_tr, previousPass.tr_count, pass.tr_count, hudW);
	// (after the player 2 prompt went: it looks for its pictures where the game put them)
	cropPlaneToScreen(ctx, ctx.global_param_op, std::max(previousPass.op_count, 1u), pass.op_count, hudW, fbWidth, fbHeight);
	cropPlaneToScreen(ctx, ctx.global_param_pt, previousPass.pt_count, pass.pt_count, hudW, fbWidth, fbHeight);
	cropPlaneToScreen(ctx, ctx.global_param_tr, previousPass.tr_count, pass.tr_count, hudW, fbWidth, fbHeight);
	// last: what's left reaching far outside the screen is cut to a box around it
	{
		static u32 clippedTotal, clipParses;
		u32 clipped = 0;
		clipFarOut(ctx, ctx.global_param_op, std::max(previousPass.op_count, 1u), pass.op_count, hudW, fbWidth, fbHeight, clipped);
		clipFarOut(ctx, ctx.global_param_pt, previousPass.pt_count, pass.pt_count, hudW, fbWidth, fbHeight, clipped);
		clipFarOut(ctx, ctx.global_param_tr, previousPass.tr_count, pass.tr_count, hudW, fbWidth, fbHeight, clipped);
		clippedTotal += clipped;
		if (++clipParses % 600 == 0 && clippedTotal > 0)
		{
			NOTICE_LOG(RENDERER, "VR: clipped %u polygons reaching far outside the screen in the last %u passes", clippedTotal, clipParses);
			clippedTotal = 0;
			clipParses = 0;
		}
	}

	// the recent shots, in DC framebuffer pixels
	// (hit: where the game draws its marker, shotDrawnAt)
	struct Recent { glm::vec2 gun, hit; int64_t since, at; bool into3D; };
	Recent recent[std::size(gunShots)];
	u32 count = 0;
	const int64_t now = steadyMs();
	for (GunShot& s : gunShots)
	{
		const int64_t at = s.ms;
		const u32 packed = s.pos;
		const bool into3D = s.into3D;
		if (s.ms != at || now - at < 0 || now - at > MarkerMs)
			continue;
		const glm::vec2 gun((packed & 0xffff) * fbWidth / 640.f, (packed >> 16) * fbHeight / 480.f);
		recent[count++] = { gun, shotDrawnAt(gun, fbWidth, fbHeight), now - at, at, into3D };
	}
	if (onPC)
		logShotTextures(ctx, pass, previousPass, fbWidth, fbHeight);
	if (count == 0)
		return;
	const bool dropMarkers = config::VrDropShotMarker;
	const bool dropFlash = config::VrDropShotFlash && !(activeProfile != nullptr && activeProfile->noShotFlash);
	// Diagnostics on the headset too, for the first few shots: what the game draws right
	// after a shot (to pin down its marker / flash). The PC logs every shot.
	static u32 diagShots;
	static int64_t diagFor;
	const Recent *latest = &recent[0];
	for (u32 k = 1; k < count; k++)
		if (recent[k].at > latest->at)
			latest = &recent[k];
	if (latest->at != diagFor && latest->since <= 200)
	{
		diagFor = latest->at;
		diagShots++;
	}
	const bool diag = onPC || (diagShots <= 8 && latest->at == diagFor && latest->since <= 200);
	if (diag && pass.op_count > 0 && !ctx.global_param_op.empty() && ctx.global_param_op[0].first < ctx.verts.size())
	{
		const u8 *bg = ctx.verts[ctx.global_param_op[0].first].col;
		NOTICE_LOG(RENDERER, "VR diag: shot +%d ms gun %.0f,%.0f%s, fb %dx%d, polygons op %u pt %u tr %u, background %02x%02x%02x%02x, focal %.1f stock %.1f",
				(int)latest->since, latest->gun.x, latest->gun.y, latest->into3D ? "" : " (2D)", fbWidth, fbHeight,
				pass.op_count, pass.pt_count, pass.tr_count, bg[0], bg[1], bg[2], bg[3], readGameFocal().x, stockFocal());
	}
	if (!dropMarkers && !dropFlash && !diag)
		return;
	u32 dropped = 0;
	const Recent *newest = nullptr;
	auto scan = [&](const std::vector<PolyParam>& list, u32 from, u32 to, const char *name) {
		for (u32 n = from; n < to && n < list.size(); n++)
		{
			const PolyParam& pp = list[n];
			if (pp.count < 3 || pp.first + pp.count > ctx.verts.size())
				continue;
			glm::vec2 lo(1e30f), hi(-1e30f);
			float wMin = 1e30f, wMax = 0.f;
			bool all2D = true;
			for (u32 i = pp.first; i < pp.first + pp.count; i++)
			{
				const Vertex& v = ctx.verts[i];
				if (!std::isfinite(v.x) || !std::isfinite(v.y))
				{
					all2D = false;	// hidden already, or the game's own invalid vertex
					continue;
				}
				lo = glm::min(lo, glm::vec2(v.x, v.y));
				hi = glm::max(hi, glm::vec2(v.x, v.y));
				const float w = v.z > 0.f ? 1.f / v.z : 0.f;
				wMin = std::min(wMin, w);
				wMax = std::max(wMax, w);
				all2D = all2D && onOverlay(v, hudW);
			}
			if (lo.x > hi.x)
				continue;
			const glm::vec2 size = hi - lo;
			// The hit marker: one of the profile's marker textures, or (any game) a small
			// polygon by the 2D plane that isn't flat on it - the game's flash quads come at
			// W 0.9..1.01, while its HUD and text sit at one W.
			const u32 tex = pp.pcw.Texture ? pp.tcw.TexAddr << 3 : 0u;
			const bool markerTex = !(activeProfile != nullptr && activeProfile->markerBeforeSnap) && tex != 0 && std::find(std::begin(markerTextures), std::end(markerTextures), tex) != std::end(markerTextures);
			const bool nearPlane = (wMin >= 0.85f && wMax <= 1.15f)
					|| (activeProfile != nullptr && wMin >= activeProfile->markerMinW && wMax <= activeProfile->markerMaxW);
			const bool markerLike = nearPlane && (markerTex || wMax - wMin > 0.004f)
					&& size.x <= MarkerMaxSize && size.y <= MarkerMaxSize;
			const glm::vec2 centre = (lo + hi) * 0.5f;
			const bool fullScreen = size.x >= fbWidth * 0.9f && size.y >= fbHeight * 0.9f;
			const u8 *c = ctx.verts[pp.first].col;
			const bool bright = c[0] >= 0xc0 && c[1] >= 0xc0 && c[2] >= 0xc0;
			// the nearest recent shot, for the log
			const Recent *near = &recent[0];
			for (u32 k = 1; k < count; k++)
				if (glm::distance(centre, recent[k].hit) < glm::distance(centre, near->hit))
					near = &recent[k];
			// measured from the point the line was picked by: where the game draws the shot
			// (the gun itself unless the profile's markerWidened moves it)
			const float offset = glm::distance(centre, near->hit);
			bool hide = false;
			for (u32 k = 0; k < count; k++)
			{
				const Recent& r = recent[k];
				hide = hide || (markerLike && dropMarkers && r.into3D && glm::distance(centre, r.hit) <= MarkerMaxOffset);
				hide = hide || (all2D && dropFlash && fullScreen && bright && r.since <= FlashMs);
			}
			const bool big = size.x >= fbWidth * 0.5f && size.y >= fbHeight * 0.5f && size.x < fbWidth * 4.f && size.y < fbHeight * 4.f;
			if (diag && (((all2D || markerLike) && glm::distance(centre, near->hit) <= MarkerMaxOffset) || (big && (all2D || bright))))
				NOTICE_LOG(RENDERER, "VR: shot +%d ms gun %.0f,%.0f%s: %s %u (%u verts) %.0fx%.0f, %.0f px off, W %.3f..%.3f%s, tex %06x tsp %08x col %02x%02x%02x%02x%s, at %.0f,%.0f..%.0f,%.0f, shot drawn at %.0f,%.0f",
						(int)near->since, near->gun.x, near->gun.y, near->into3D ? "" : " (2D)", name, n, pp.count,
						size.x, size.y, offset, wMin, wMax, all2D ? " all-2D" : "",
						(unsigned)(pp.tcw.TexAddr << 3), pp.tsp.full, c[0], c[1], c[2], c[3], hide ? " DROPPED" : "",
						lo.x, lo.y, hi.x, hi.y, near->hit.x, near->hit.y);
			if (!hide)
				continue;
			hideVertices(ctx, pp);
			dropped++;
			if (newest == nullptr || near->at > newest->at)
				newest = near;
		}
	};
	scan(ctx.global_param_op, std::max(previousPass.op_count, 1u), pass.op_count, "OP");
	scan(ctx.global_param_pt, previousPass.pt_count, pass.pt_count, "PT");
	scan(ctx.global_param_tr, previousPass.tr_count, pass.tr_count, "TR");
	static int64_t loggedFor;
	if (dropped > 0 && newest != nullptr && loggedFor != newest->at)
	{
		loggedFor = newest->at;
		NOTICE_LOG(RENDERER, "VR: hid the game's shot marker (%u polygons, %d ms after the shot)", dropped, (int)newest->since);
	}
}

const ReprojectParams& update(const rend_context& ctx)
{
	int dcWidth, dcHeight;
	getPvrFramebufferSize(ctx, dcWidth, dcHeight);
	const glm::vec2 halfSize(dcWidth * 0.5f, dcHeight * 0.5f);
	const bool active = config::VrReproject && !ctx.isRTT;
	params.comfort = glm::vec3(0.f);

	const float zMax = ctx.fZ_max > 0.f && std::isfinite(ctx.fZ_max) ? ctx.fZ_max : 1.f;
	const float zNear = NearFraction / zMax;

	if (!active)
	{
		params.tanHalf = halfSize / (float)config::VrFocal;
		params.overlay = glm::vec4(params.tanHalf, 1.f, -1.f);	// no vertex counts as overlay
		params.viewProj = projection(params.tanHalf, zNear);
		return params;
	}

	// The game may run with a widened view: rebuild with its live focal length, but
	// present with the stock one so the framing and HUD size stay as designed.
	const glm::vec2 gameFocal = readGameFocal();
	const glm::vec2 stockTan = halfSize / (float)config::VrFocal;
	params.tanHalf = halfSize / gameFocal;
	params.overlay = glm::vec4(stockTan, panelDepth(), (float)config::VrHudW);

	// depth statistics now and then, for tuning a new game on the PC (not in the headset)
	if (frameCount++ % 120 == 0 && !xr::enabled())
		logSceneStats(ctx, gameFocal);

	if (const xr::Eye *eye = xr::currentEye())
	{
		// In the headset: game camera at the player's head, viewed through this eye.
		params.viewProj = eye->viewProj;
		params.comfort = comfortParams();
		return params;
	}

	float yaw = config::VrYaw;
	float pitch = config::VrPitch;
	glm::vec3 offset(config::VrOffsetX, config::VrOffsetY, config::VrOffsetZ);
	if (config::VrAnimate)
	{
		// Sway around the game camera so stills from consecutive seconds show parallax.
		const float t = frameCount / 60.f;
		yaw *= std::sin(t * 0.9f);
		pitch *= std::sin(t * 0.6f);
		offset *= glm::vec3(std::sin(t * 0.7f), std::cos(t * 0.5f), std::sin(t * 0.4f));
	}
	glm::mat4 pose = glm::translate(glm::mat4(1.f), offset)
			* glm::rotate(glm::mat4(1.f), glm::radians(yaw), glm::vec3(0, 1, 0))
			* glm::rotate(glm::mat4(1.f), glm::radians(pitch), glm::vec3(1, 0, 0));
	// The renderer's ndc y points down the image; flip into y-up space for the pose.
	const glm::mat4 flipY = glm::scale(glm::mat4(1.f), glm::vec3(1, -1, 1));
	// (a test aid: the window as wide as a headset's eye, vr.WindowTanX/Y)
	const glm::vec2 windowTan = config::VrWindowTanX > 0.f && config::VrWindowTanY > 0.f
			? glm::vec2(config::VrWindowTanX, config::VrWindowTanY) : stockTan;
	params.viewProj = projection(windowTan, zNear) * flipY * glm::inverse(pose) * flipY;

	return params;
}

}
