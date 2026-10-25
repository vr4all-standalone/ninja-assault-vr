/*
	OpenXR host (hotd2-vr). See xr_host.h.

	Everything here runs on Flycast's render thread, which owns the EGL context the
	OpenXR session is bound to. Initialisation happens on the first frame.

	World layout: the game camera sits at the player's head as it was when the session
	started (or at the last recenter), looking where the player looked. Rebuilt game
	eye-space positions (game units, renderer y convention: y down) map to the headset's
	local space (metres, y up) by a uniform scale (vr.WorldScale) and a y flip.

	Copyright 2026 mikermak. This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#include "xr_host.h"

#ifdef USE_OPENXR
#include "xr_controllers.h"
#include "xr_gun.h"
#include "xr_hands.h"
#include "xr_labels.h"
#include "xr_menu.h"
#include "xr_panel.h"
#include "xr_sim.h"
#include "xr_own.h"
#include "hands_rip.h"
#include "rend/gles/gles.h"
#ifdef __ANDROID__
// the Quest: OpenGL ES through EGL
#include <jni.h>
#include <glad/egl.h>
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#else
// PCVR: desktop OpenGL through WGL
#include <windows.h>
#include <unknwn.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "rend/vr_reproject.h"
#include "rend/TexCache.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/pvr/ta_ctx.h"
#include "input/udp_lightgun.h"
#include "cfg/option.h"
#include "cfg/cfg.h"
#include "stdclass.h"
#include "emulator.h"
#include "network/ggpo.h"
#include "log/Log.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <iterator>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef __ANDROID__
extern JavaVM *g_jvm;
extern jobject g_activity;
#endif

#ifdef __ANDROID__
std::string getFilesPath();		// the app's internal files directory (Android.cpp)
#endif

namespace vr::xr
{
namespace
{

#ifdef __ANDROID__
using SwapchainImage = XrSwapchainImageOpenGLESKHR;
constexpr XrStructureType SwapchainImageType = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
// Plain RGBA8: the game's colours were made for a gamma display, like the eye buffers.
constexpr int64_t SwapchainFormats[] { GL_RGBA8 };
#else
using SwapchainImage = XrSwapchainImageOpenGLKHR;
constexpr XrStructureType SwapchainImageType = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
// PC runtimes take RGBA8 as linear and brighten it. An sRGB image written to as it is (no
// GL_FRAMEBUFFER_SRGB) shows the colours as on the Quest.
constexpr int64_t SwapchainFormats[] { GL_SRGB8_ALPHA8, GL_RGBA8 };
#endif

constexpr float NearMetres = 0.05f;
constexpr u32 RestartIndex = 0xFFFFFFFFu;

XrInstance instance = XR_NULL_HANDLE;
XrSystemId systemId = XR_NULL_SYSTEM_ID;
XrSession session = XR_NULL_HANDLE;
XrSpace localSpace = XR_NULL_HANDLE;
constexpr int Left = 0, Right = 1;
XrPath handPaths[2];
XrSpace aimSpaces[2];
XrSpace gripSpaces[2];		// the hand on the handle (grip pose): where the gun's grip goes
XrActionSet actionSet = XR_NULL_HANDLE;
XrAction aimAction, handAction, triggerAction, startAction, skipAction, menuAction, recenterAction, laserAction, hapticAction, stickAction, gripAction;
PFN_xrRequestDisplayRefreshRateFB requestRefreshRate;
PFN_xrPerfSettingsSetPerformanceLevelEXT setPerformanceLevel;

bool initTried;
bool sessionRunning;
bool focused;
std::chrono::steady_clock::time_point focusLost;
// The runtime lost the session (start a new one) or wants the app to quit (don't).
bool sessionLost;
bool viewsLogged;		// frame()'s once-a-session log of the views
bool exiting;

struct EyeChain
{
	XrSwapchain chain = XR_NULL_HANDLE;
	int width = 0, height = 0;
	std::vector<SwapchainImage> images;
	std::vector<GLuint> fbos;
	GLuint depthStencil = 0;
	int samples = 1;	// MSAA (the GPU resolves it on the way out of its tile memory)
};
EyeChain chains[2];
XrView views[2] { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };

Eye eye;
bool drawingEye;

// Where the game camera is: the head pose at start / recenter (yaw only).
bool originSet;
bool recenterRequested;
glm::vec3 originPos;
glm::quat originRot { 1, 0, 0, 0 };

// The light guns. Player 1's is in the hand whose trigger was pulled last; once player 2
// joins (vr.DualWield: X/left Start in single play), vr.DualP1Left puts player 1 on the left
// by default and player 2 on the right. This preference never changes the single gun's hand.
struct Gun
{
	int player;
	int hand = Right;
	bool triggerWas;
	// The current pull started outside the game's view (aimOutside): it does nothing until
	// the trigger is let go, even when the aim moves into view.
	bool pullOutside;
	XrTime lastShot;
	unsigned shotCount;
	// The last flash was a round of the rifle's burst after the pull (a smaller kick); the flashes
	// since the trigger was pulled (with the rifle logged once it's let go).
	bool autoKick;
	unsigned burstFlashes;
	int angleLogged = 0;	// the vr.GunAngle of the last muzzle line
	int muzzleLogged = -1;	// the gun whose muzzle was last logged (GunKind)
	// When the last reload started (racked slide, gun pointed down; 0: done), and a
	// trigger held through it.
	XrTime reloadAt;
	bool reloadButtonWas = false;
	bool triggerWait;
	bool pointedDown;
	// as drawn (no pose: hidden), with the last shot for recoil and flash
	GunView view;
	bool visible;
	int palmLogged = -1;	// hand * 2 + where the palm came from, as last logged
	// The knocks (gunKnocks): whether the game's profile has its gun state (gunRam), that state
	// as last read (gunKnown: read, the emulator running), when a pull on the screen went to the
	// game that it hasn't fired for yet (0: none), and when the trigger was last held.
	bool gunRam;
	vr::GunState gunWas;
	bool gunKnown;
	XrTime pullAt;
	XrTime heldAt;
	// The knocks still to give (Shots, the rifle's Auto knocks, clicks), one at a time: the next
	// once the last has ended (knockEnds). A trigger pulled since the last Shot (freshPull): the
	// rifle's next round is a Shot, the rest of the burst Auto knocks.
	int shotsDue, autosDue, clicksDue;
	XrTime knockEnds;
	bool freshPull;
	// vr.Reload 1 (Point off-screen): the aim was on the screen since the last reload by pointing
	// away (one reload each time it leaves; never from the menu, nor when it closes pointing away)
	bool offArmed = false;
};
Gun guns[2] { { 0 }, { 1, Left } };
bool dualWield;
XrTime dualSince;
bool p2StartWas, p2JoinHeld, recenterWas, laserWas, menuWas;
// The VR menu (the settings menu, xr_menu.h). Open, the emulator is held stopped: frame() draws
// the last game frame again for each new head pose, with the guns, the menu and its pointers, and
// nothing goes to the game. The menu asks for it to open or close (pauseRequest: 1 open, 0 close,
// -1 nothing asked); the top of the next Emulator::render does it (applyPause). The frame it opens
// on takes no button (menuFresh); B or A held when it closes is no reload or Start for the game
// until it's let go (skipSwallow, startSwallow). Quit on the PC: Flycast exits there too (quitPending).
std::atomic<bool> menuOpen;		// (read by the app's resume on Android's main thread)
int pauseRequest = -1;
bool menuFresh, redrawLogged;
bool skipWas, skipSwallow, startSwallow;
#ifndef __ANDROID__
bool quitPending;
#endif
// The menu's pointers from hands with no gun drawn (the guns' own aim lines point at it otherwise)
GunView pointers[2];
bool pointerShown[2];
bool ringDumpRequested;		// the left stick click held: the frame ring (debug aid) is to be written
// The controllers' models (xr_controllers.h): looked for once a session, at its first FOCUSED
// state; drawn this frame in the hands that show no gun or game hand, at their grip poses (room
// space).
bool controllersReady;
// A model the runtime didn't have yet (controllers::retryWanted) is asked for again at each
// interaction profile change and regained focus, and every 2 s while focused, 15 times a focus.
int modelRetries;
std::chrono::steady_clock::time_point nextModelRetry;
bool controllerShown[2];
glm::mat4 controllerGrip[2];
glm::mat4 controllerAim[2];		// (the aim pose: the player's own controller models go there, xr_own.h)
// What each hand holds (holds()) and how to play: the controllers with their labels (xr_labels.h).
// A game starts with a controller in each hand and the labels on; player 1's gun hand takes the gun
// at player 1's first Start or trigger pull (p1Armed; that pull fires as usual), and the controllers
// and the labels go: from then on a hand without a gun draws nothing. The other holds player 2's gun
// while player 2 is in. The menu's How to play (menuHowToPlay): both hands their controllers with the
// labels while the menu is open; the menu closing, however it closes, gives back what they held. As
// last logged (noteHolds): heldWas, labelsWere. p1StartWas: player 1's Start's last state, for its press.
bool p1Armed, menuHowToPlay;
Holds heldWas[2] { Holds::Controller, Holds::Controller };
bool labelsWere = true;
bool p1StartWas;

// The controllers on each hand as the runtime last named them (an interaction profile path;
// empty: none, or not known yet).
std::string profiles[2];
// Controllers with no button to spare for the settings menu (menuByHold): since when the
// left Start is held (0: it isn't), whether this hold already opened the menu, and until when
// a short press, let go, is the left Start.
XrTime leftHoldSince;
bool leftHoldMenu;
XrTime leftTapUntil;
// A game (re)started: one gun again (EventManager calls on the emulator's thread).
std::atomic<bool> newGame;
struct NewGameRegistration
{
	NewGameRegistration() {
		EventManager::listen(Event::Start, [](Event, void *) { newGame = true; });
	}
} newGameRegistration;
// How to play: the hands that hold their controllers show them, with the labels: before the game's
// first Start or pull, and while the menu's How to play is on. (The labels drawn only with the light
// gun on, vr.XrGun; without it the controllers always show.)
// The game's cutscenes once play has started (vr::cutscene(), read once a frame in updateLightgun):
// the hands that hold a gun hold nothing (holds(): no gun, no controller, no laser line), the other
// hand draws nothing as in play, no labels, until play resumes. A round the game fires
// in cutscene mode (gunKnocks: cutsceneVeto) ends it until the bars have gone: play is live (a
// letterboxed moment that takes shots, or a scene skipped with a shot), so the guns come back at
// once and stay, even if the bars blink.
bool inCutscene;
bool cutsceneVeto;

bool labelsOn()
{
	return menuHowToPlay || !p1Armed;
}

// What each hand holds and whether the labels show, logged where it changed (why: what did it).
void noteHolds(const char *why)
{
	static const char *const Names[] { "controller", "gun", "nothing" };
	for (int hand : { Left, Right })
	{
		const Holds now = holds(hand);
		if (now != heldWas[hand])
			NOTICE_LOG(INPUT, "XR: %s hand: %s -> %s (%s)", hand == Right ? "right" : "left", Names[(int)heldWas[hand]], Names[(int)now], why);
		heldWas[hand] = now;
	}
	if (labelsOn() != labelsWere)
		NOTICE_LOG(INPUT, "XR: labels %s (%s)", labelsOn() ? "on" : "off", why);
	labelsWere = labelsOn();
}

// for capture-shot.request: trigger pulls so far, and where the last one went (DC pixels)
std::atomic<u32> shotSerial;
glm::vec2 lastShotScreen(-1.f);

// The OpenXR functions called here, by name: the loader's (the runtime's), or with vr.XrSim the
// simulator's (xr_sim.h, PC builds). Called as XR(xrName)(...).
#define XR_FUNCTIONS(F) \
	F(xrGetInstanceProcAddr) F(xrEnumerateInstanceExtensionProperties) F(xrCreateInstance) F(xrDestroyInstance) \
	F(xrGetInstanceProperties) F(xrPollEvent) F(xrResultToString) F(xrStringToPath) F(xrPathToString) \
	F(xrGetSystem) F(xrEnumerateViewConfigurationViews) F(xrCreateSession) F(xrDestroySession) \
	F(xrBeginSession) F(xrEndSession) F(xrCreateReferenceSpace) F(xrCreateActionSpace) F(xrLocateSpace) \
	F(xrLocateViews) F(xrEnumerateSwapchainFormats) F(xrCreateSwapchain) F(xrEnumerateSwapchainImages) \
	F(xrAcquireSwapchainImage) F(xrWaitSwapchainImage) F(xrReleaseSwapchainImage) F(xrWaitFrame) \
	F(xrBeginFrame) F(xrEndFrame) F(xrCreateActionSet) F(xrCreateAction) F(xrSuggestInteractionProfileBindings) \
	F(xrAttachSessionActionSets) F(xrGetCurrentInteractionProfile) F(xrSyncActions) F(xrGetActionStateBoolean) \
	F(xrGetActionStateFloat) F(xrGetActionStateVector2f) F(xrApplyHapticFeedback)
// the ones there before an instance
#define XR_PRE_INSTANCE_FUNCTIONS(F) F(xrEnumerateInstanceExtensionProperties) F(xrCreateInstance)

struct Functions
{
#define XR_MEMBER(name) PFN_##name name = nullptr;
	XR_FUNCTIONS(XR_MEMBER)
#undef XR_MEMBER
} functions;
#define XR(fn) (functions.fn)

// Fills the table: with no instance yet the pre-instance functions (the simulator: all of
// them), then the rest with the instance.
bool loadFunctions(XrInstance inst)
{
	bool ok = true;
	auto load = [&](const char *name, PFN_xrVoidFunction *function) {
		if (sim::active())
			*function = sim::proc(name);
		else if (XR_FAILED(functions.xrGetInstanceProcAddr(inst, name, function)))
			*function = nullptr;
		if (*function == nullptr)
		{
			ERROR_LOG(RENDERER, sim::active() ? "XR: the simulator has no %s" : "XR: the OpenXR loader has no %s", name);
			ok = false;
		}
	};
#define XR_LOAD(name) load(#name, (PFN_xrVoidFunction *)&functions.name);
	if (inst == XR_NULL_HANDLE)
	{
		functions = Functions();
		if (sim::active())
		{
			XR_FUNCTIONS(XR_LOAD)
			return ok;
		}
		functions.xrGetInstanceProcAddr = ::xrGetInstanceProcAddr;
		XR_PRE_INSTANCE_FUNCTIONS(XR_LOAD)
	}
	else if (!sim::active())
	{
		XR_FUNCTIONS(XR_LOAD)
	}
#undef XR_LOAD
	return ok;
}

bool check(XrResult result, const char *what)
{
	if (XR_SUCCEEDED(result))
		return true;
	char text[XR_MAX_RESULT_STRING_SIZE] = "";
	if (instance != XR_NULL_HANDLE && XR(xrResultToString))
		XR(xrResultToString)(instance, result, text);
	ERROR_LOG(RENDERER, "XR: %s failed: %d %s", what, result, text);
	return false;
}
#define XRCHECK(call) check((call), #call)

XrPath path(const char *s)
{
	XrPath p = XR_NULL_PATH;
	XR(xrStringToPath)(instance, s, &p);
	return p;
}

bool createAction(XrAction& action, const char *name, const char *label, XrActionType type, bool perHand = false)
{
	XrActionCreateInfo info { XR_TYPE_ACTION_CREATE_INFO };
	info.actionType = type;
	strncpy(info.actionName, name, sizeof(info.actionName) - 1);
	strncpy(info.localizedActionName, label, sizeof(info.localizedActionName) - 1);
	if (perHand)
	{
		info.countSubactionPaths = 2;
		info.subactionPaths = handPaths;
	}
	return XRCHECK(XR(xrCreateAction)(actionSet, &info, &action));
}

bool initActions()
{
	handPaths[Left] = path("/user/hand/left");
	handPaths[Right] = path("/user/hand/right");
	XrActionSetCreateInfo setInfo { XR_TYPE_ACTION_SET_CREATE_INFO };
	strcpy(setInfo.actionSetName, "lightgun");
	strcpy(setInfo.localizedActionSetName, "Light gun");
	if (!XRCHECK(XR(xrCreateActionSet)(instance, &setInfo, &actionSet)))
		return false;
	if (!createAction(aimAction, "aim", "Aim", XR_ACTION_TYPE_POSE_INPUT, true)
			|| !createAction(handAction, "hand", "Hand (holds the gun)", XR_ACTION_TYPE_POSE_INPUT, true)
			|| !createAction(triggerAction, "trigger", "Fire", XR_ACTION_TYPE_FLOAT_INPUT, true)
			|| !createAction(startAction, "start", "Start (left: player 2 joins)", XR_ACTION_TYPE_BOOLEAN_INPUT, true)
			|| !createAction(skipAction, "skip", "Reload (the gun's B)", XR_ACTION_TYPE_BOOLEAN_INPUT, true)
			|| !createAction(menuAction, "menu", "Settings menu", XR_ACTION_TYPE_BOOLEAN_INPUT)
			|| !createAction(recenterAction, "recenter", "Recenter", XR_ACTION_TYPE_BOOLEAN_INPUT)
			|| !createAction(laserAction, "laser", "Laser on/off", XR_ACTION_TYPE_BOOLEAN_INPUT)
			|| !createAction(hapticAction, "recoil", "Recoil", XR_ACTION_TYPE_VIBRATION_OUTPUT, true)
			|| !createAction(stickAction, "stick", "Menus (D-pad), world size", XR_ACTION_TYPE_VECTOR2F_INPUT)
			|| !createAction(gripAction, "grip", "Rack the slide; hold for world size", XR_ACTION_TYPE_FLOAT_INPUT, true))
		return false;
	// Single play: either trigger fires and takes the gun, A starts P1, X joins P2,
	// and B or Y reloads P1. Once dual play starts, the chosen Dual hands layout
	// assigns each hand's trigger, Start and reload to its actual player.
	// The left menu button opens settings, the left stick click recenters (held:
	// frame ring), and the right stick click toggles the laser. The stick is the
	// game's D-pad; grip plus stick adjusts world/gun size. A free hand can rack
	// a game pistol's slide. These physical bindings stay the same in either layout.
	auto suggest = [](const char *profile, std::initializer_list<std::pair<XrAction, const char *>> list) {
		std::vector<XrActionSuggestedBinding> bindings;
		for (const auto& [action, where] : list)
			bindings.push_back({ action, path(where) });
		XrInteractionProfileSuggestedBinding suggested { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
		suggested.interactionProfile = path(profile);
		suggested.countSuggestedBindings = (u32)bindings.size();
		suggested.suggestedBindings = bindings.data();
		return check(XR(xrSuggestInteractionProfileBindings)(instance, &suggested), profile);
	};
	bool any = suggest("/interaction_profiles/oculus/touch_controller", {
		{ stickAction, "/user/hand/left/input/thumbstick" },
		{ stickAction, "/user/hand/right/input/thumbstick" },
		{ gripAction, "/user/hand/left/input/squeeze/value" },
		{ gripAction, "/user/hand/right/input/squeeze/value" },
		{ aimAction, "/user/hand/left/input/aim/pose" },
		{ aimAction, "/user/hand/right/input/aim/pose" },
		{ handAction, "/user/hand/left/input/grip/pose" },
		{ handAction, "/user/hand/right/input/grip/pose" },
		{ triggerAction, "/user/hand/left/input/trigger/value" },
		{ triggerAction, "/user/hand/right/input/trigger/value" },
		{ startAction, "/user/hand/right/input/a/click" },
		{ startAction, "/user/hand/left/input/x/click" },
		{ skipAction, "/user/hand/right/input/b/click" },
		{ skipAction, "/user/hand/left/input/y/click" },
		{ menuAction, "/user/hand/left/input/menu/click" },
		{ recenterAction, "/user/hand/left/input/thumbstick/click" },
		{ laserAction, "/user/hand/right/input/thumbstick/click" },
		{ hapticAction, "/user/hand/left/output/haptic" },
		{ hapticAction, "/user/hand/right/output/haptic" },
	});
#ifndef __ANDROID__
	// PCVR: the other controllers, as near as they come: a Start on each hand (the left one
	// lets player 2 join), the gun's B (reload) on each, and a settings menu. Index: A and B;
	// the menu a hard squeeze of the left pad (its menu button is the system's); recenter with
	// the left stick click, the laser with the right one. Vive wands and Windows Mixed Reality:
	// the menu button is Start, and the left one held 0.8 s the settings menu (menuByHold); a
	// click on the pad is the gun's B; the pad or stick is the D-pad (Windows Mixed Reality: the
	// stick clicks as on the Index; Vive wands: the runtime's own recenter, no laser button).
	any |= suggest("/interaction_profiles/valve/index_controller", {
		{ stickAction, "/user/hand/left/input/thumbstick" },
		{ stickAction, "/user/hand/right/input/thumbstick" },
		// (squeeze force, not how far the hand is closed: holding an Index already closes it,
		// and that changed the world size with every stick push)
		{ gripAction, "/user/hand/left/input/squeeze/force" },
		{ gripAction, "/user/hand/right/input/squeeze/force" },
		{ aimAction, "/user/hand/left/input/aim/pose" },
		{ aimAction, "/user/hand/right/input/aim/pose" },
		{ handAction, "/user/hand/left/input/grip/pose" },
		{ handAction, "/user/hand/right/input/grip/pose" },
		{ triggerAction, "/user/hand/left/input/trigger/value" },
		{ triggerAction, "/user/hand/right/input/trigger/value" },
		{ startAction, "/user/hand/right/input/a/click" },
		{ startAction, "/user/hand/left/input/a/click" },
		{ skipAction, "/user/hand/right/input/b/click" },
		{ skipAction, "/user/hand/left/input/b/click" },
		{ menuAction, "/user/hand/left/input/trackpad/force" },
		{ recenterAction, "/user/hand/left/input/thumbstick/click" },
		{ laserAction, "/user/hand/right/input/thumbstick/click" },
		{ hapticAction, "/user/hand/left/output/haptic" },
		{ hapticAction, "/user/hand/right/output/haptic" },
	});
	any |= suggest("/interaction_profiles/htc/vive_controller", {
		{ stickAction, "/user/hand/left/input/trackpad" },
		{ stickAction, "/user/hand/right/input/trackpad" },
		{ gripAction, "/user/hand/left/input/squeeze/click" },
		{ gripAction, "/user/hand/right/input/squeeze/click" },
		{ aimAction, "/user/hand/left/input/aim/pose" },
		{ aimAction, "/user/hand/right/input/aim/pose" },
		{ handAction, "/user/hand/left/input/grip/pose" },
		{ handAction, "/user/hand/right/input/grip/pose" },
		{ triggerAction, "/user/hand/left/input/trigger/value" },
		{ triggerAction, "/user/hand/right/input/trigger/value" },
		{ startAction, "/user/hand/right/input/menu/click" },
		{ startAction, "/user/hand/left/input/menu/click" },
		{ skipAction, "/user/hand/right/input/trackpad/click" },
		{ skipAction, "/user/hand/left/input/trackpad/click" },
		{ hapticAction, "/user/hand/left/output/haptic" },
		{ hapticAction, "/user/hand/right/output/haptic" },
	});
	any |= suggest("/interaction_profiles/microsoft/motion_controller", {
		{ stickAction, "/user/hand/left/input/thumbstick" },
		{ stickAction, "/user/hand/right/input/thumbstick" },
		{ gripAction, "/user/hand/left/input/squeeze/click" },
		{ gripAction, "/user/hand/right/input/squeeze/click" },
		{ aimAction, "/user/hand/left/input/aim/pose" },
		{ aimAction, "/user/hand/right/input/aim/pose" },
		{ handAction, "/user/hand/left/input/grip/pose" },
		{ handAction, "/user/hand/right/input/grip/pose" },
		{ triggerAction, "/user/hand/left/input/trigger/value" },
		{ triggerAction, "/user/hand/right/input/trigger/value" },
		{ startAction, "/user/hand/right/input/menu/click" },
		{ startAction, "/user/hand/left/input/menu/click" },
		{ skipAction, "/user/hand/right/input/trackpad/click" },
		{ skipAction, "/user/hand/left/input/trackpad/click" },
		{ recenterAction, "/user/hand/left/input/thumbstick/click" },
		{ laserAction, "/user/hand/right/input/thumbstick/click" },
		{ hapticAction, "/user/hand/left/output/haptic" },
		{ hapticAction, "/user/hand/right/output/haptic" },
	});
#endif
	if (!any)
		return false;
	XrSessionActionSetsAttachInfo attach { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	attach.countActionSets = 1;
	attach.actionSets = &actionSet;
	if (!XRCHECK(XR(xrAttachSessionActionSets)(session, &attach)))
		return false;
	for (int hand : { Left, Right })
	{
		XrActionSpaceCreateInfo spaceInfo { XR_TYPE_ACTION_SPACE_CREATE_INFO };
		spaceInfo.action = aimAction;
		spaceInfo.subactionPath = handPaths[hand];
		spaceInfo.poseInActionSpace.orientation.w = 1.f;
		if (!XRCHECK(XR(xrCreateActionSpace)(session, &spaceInfo, &aimSpaces[hand])))
			return false;
		spaceInfo.action = handAction;
		if (!XRCHECK(XR(xrCreateActionSpace)(session, &spaceInfo, &gripSpaces[hand])))
			return false;
	}
	return true;
}

bool initSwapchains()
{
	u32 formatCount = 0;
	if (!XRCHECK(XR(xrEnumerateSwapchainFormats)(session, 0, &formatCount, nullptr)))
		return false;
	std::vector<int64_t> formats(formatCount);
	if (!XRCHECK(XR(xrEnumerateSwapchainFormats)(session, formatCount, &formatCount, formats.data())))
		return false;
	int64_t format = 0;
	for (int64_t wanted : SwapchainFormats)
		if (format == 0 && std::find(formats.begin(), formats.end(), wanted) != formats.end())
			format = wanted;
	if (format == 0)
	{
		ERROR_LOG(RENDERER, "XR: no RGBA8 swapchain format");
		return false;
	}
	u32 viewCount = 0;
	if (!XRCHECK(XR(xrEnumerateViewConfigurationViews)(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr))
			|| viewCount != 2)
		return false;
	XrViewConfigurationView configViews[2] { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
	if (!XRCHECK(XR(xrEnumerateViewConfigurationViews)(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &viewCount, configViews)))
		return false;
	for (int i = 0; i < 2; i++)
	{
		EyeChain& c = chains[i];
		const float scale = std::clamp((float)config::VrXrResolution, 0.3f, 1.5f);
		c.width = (int)(configViews[i].recommendedImageRectWidth * scale);
		c.height = (int)(configViews[i].recommendedImageRectHeight * scale);
		XrSwapchainCreateInfo info { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
		info.format = format;
		info.sampleCount = 1;
		info.width = c.width;
		info.height = c.height;
		info.faceCount = 1;
		info.arraySize = 1;
		info.mipCount = 1;
		if (!XRCHECK(XR(xrCreateSwapchain)(session, &info, &c.chain)))
			return false;
		u32 imageCount = 0;
		if (!XRCHECK(XR(xrEnumerateSwapchainImages)(c.chain, 0, &imageCount, nullptr)))
			return false;
		c.images.assign(imageCount, { SwapchainImageType });
		if (!XRCHECK(XR(xrEnumerateSwapchainImages)(c.chain, imageCount, &imageCount, (XrSwapchainImageBaseHeader *)c.images.data())))
			return false;
		// Smooth edges (vr.Msaa, off by default): multisampled where the GPU resolves it as it
		// writes the picture out of its tile memory (GL_EXT_multisampled_render_to_texture: the
		// Quest). Elsewhere (PCVR) without, for now.
		int samples = 1;
		if (config::VrMsaa > 1 && GLAD_GL_EXT_multisampled_render_to_texture)
		{
			GLint most = 0;
			glGetIntegerv(GL_MAX_SAMPLES_EXT, &most);
			samples = std::clamp((int)config::VrMsaa, 1, std::max((int)most, 1));
		}
		c.fbos.resize(imageCount);
		glGenFramebuffers(imageCount, c.fbos.data());
		for (;;)
		{
			// Depth with stencil: the renderer uses stencil for modifier volumes (shadows).
			glGenRenderbuffers(1, &c.depthStencil);
			glBindRenderbuffer(GL_RENDERBUFFER, c.depthStencil);
			if (samples > 1)
				glRenderbufferStorageMultisampleEXT(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, c.width, c.height);
			else
				glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, c.width, c.height);
			bool complete = true;
			for (u32 j = 0; j < imageCount && complete; j++)
			{
				glBindFramebuffer(GL_FRAMEBUFFER, c.fbos[j]);
				if (samples > 1)
					glFramebufferTexture2DMultisampleEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c.images[j].image, 0, samples);
				else
					glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c.images[j].image, 0);
				glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, c.depthStencil);
				complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
			}
			if (complete)
				break;
			glDeleteRenderbuffers(1, &c.depthStencil);
			c.depthStencil = 0;
			if (samples == 1)
			{
				ERROR_LOG(RENDERER, "XR: eye framebuffer incomplete");
				return false;
			}
			WARN_LOG(RENDERER, "XR: no eye framebuffer with %dx MSAA, without then", samples);
			samples = 1;
		}
		c.samples = samples;
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}
	NOTICE_LOG(RENDERER, "XR: eye buffers %dx%d, %s", chains[0].width, chains[0].height,
			chains[0].samples > 1 ? (std::to_string(chains[0].samples) + "x MSAA").c_str() : "no MSAA");
	return true;
}

bool init()
{
	initTried = true;
#ifdef __ANDROID__
	if (g_jvm == nullptr || g_activity == nullptr)
	{
		ERROR_LOG(RENDERER, "XR: no Java VM / activity");
		return false;
	}
	PFN_xrInitializeLoaderKHR initLoader = nullptr;
	if (!XRCHECK(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&initLoader)))
		return false;
	XrLoaderInitInfoAndroidKHR loaderInfo { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
	loaderInfo.applicationVM = g_jvm;
	loaderInfo.applicationContext = g_activity;
	if (!XRCHECK(initLoader((const XrLoaderInitInfoBaseHeaderKHR *)&loaderInfo)))
		return false;
	const char *graphicsExtension = XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME;
	std::vector<const char *> extensions { XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, graphicsExtension };
#else
	const char *graphicsExtension = XR_KHR_OPENGL_ENABLE_EXTENSION_NAME;
	std::vector<const char *> extensions { graphicsExtension };
#endif
	if (!loadFunctions(XR_NULL_HANDLE))
		return false;

	u32 available = 0;
	if (!XRCHECK(XR(xrEnumerateInstanceExtensionProperties)(nullptr, 0, &available, nullptr)))
		return false;	// no OpenXR runtime (PC: no SteamVR, Quest Link or Virtual Desktop running)
	std::vector<XrExtensionProperties> props(available, { XR_TYPE_EXTENSION_PROPERTIES });
	XRCHECK(XR(xrEnumerateInstanceExtensionProperties)(nullptr, available, &available, props.data()));
	auto has = [&](const char *name) {
		for (const auto& p : props)
			if (!strcmp(p.extensionName, name))
				return true;
		return false;
	};
	static bool extensionsLogged;
	if (!extensionsLogged)
	{
		// what this runtime offers, once a run (init is tried again after a failure); in lines of
		// up to ~900 characters (the log's limit is 1024), each with the same start
		extensionsLogged = true;
		std::string names;
		for (size_t i = 0; i < props.size(); i++)
		{
			names += std::string(names.empty() ? "" : " ") + props[i].extensionName;
			if (names.size() > 900 || i + 1 == props.size())
			{
				NOTICE_LOG(RENDERER, "XR: runtime extensions: %s", names.c_str());
				names.clear();
			}
		}
	}
	if (!has(graphicsExtension))
	{
		ERROR_LOG(RENDERER, "XR: the OpenXR runtime has no %s", graphicsExtension);
		return false;
	}
	const bool refreshRates = has(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
	const bool perfSettings = has(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
	if (refreshRates)
		extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
	if (perfSettings)
		extensions.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
	// the controllers' own models (xr_controllers.h; the Quest: with com.oculus.permission.RENDER_MODEL)
	if (has(XR_FB_RENDER_MODEL_EXTENSION_NAME))
		extensions.push_back(XR_FB_RENDER_MODEL_EXTENSION_NAME);

	XrInstanceCreateInfo createInfo { XR_TYPE_INSTANCE_CREATE_INFO };
#ifdef __ANDROID__
	XrInstanceCreateInfoAndroidKHR androidInfo { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
	androidInfo.applicationVM = g_jvm;
	androidInfo.applicationActivity = g_activity;
	createInfo.next = &androidInfo;
#endif
	createInfo.enabledExtensionCount = (u32)extensions.size();
	createInfo.enabledExtensionNames = extensions.data();
	strcpy(createInfo.applicationInfo.applicationName, "Lupin the Third VR");
	createInfo.applicationInfo.applicationVersion = 1;
	strcpy(createInfo.applicationInfo.engineName, "Flycast");
	createInfo.applicationInfo.engineVersion = 1;
	createInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
	if (!XRCHECK(XR(xrCreateInstance)(&createInfo, &instance)))
		return false;
	if (!loadFunctions(instance))
	{
		// (only the loader's can fail here: the simulator's table is full from the start)
		(XR(xrDestroyInstance) != nullptr ? XR(xrDestroyInstance) : ::xrDestroyInstance)(instance);
		instance = XR_NULL_HANDLE;
		return false;
	}
	if (refreshRates)
		XR(xrGetInstanceProcAddr)(instance, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction *)&requestRefreshRate);
	if (perfSettings)
		XR(xrGetInstanceProcAddr)(instance, "xrPerfSettingsSetPerformanceLevelEXT", (PFN_xrVoidFunction *)&setPerformanceLevel);
	XrInstanceProperties instanceProps { XR_TYPE_INSTANCE_PROPERTIES };
	if (XR_SUCCEEDED(XR(xrGetInstanceProperties)(instance, &instanceProps)))
		NOTICE_LOG(RENDERER, "XR: runtime %s", instanceProps.runtimeName);

	XrSystemGetInfo systemInfo { XR_TYPE_SYSTEM_GET_INFO };
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	if (!XRCHECK(XR(xrGetSystem)(instance, &systemInfo, &systemId)))
		return false;	// (PC: the headset isn't connected)

	// Bind the session to Flycast's own GL context, current on this thread.
#ifdef __ANDROID__
	PFN_xrGetOpenGLESGraphicsRequirementsKHR getRequirements = nullptr;
	if (!XRCHECK(XR(xrGetInstanceProcAddr)(instance, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&getRequirements)))
		return false;
	XrGraphicsRequirementsOpenGLESKHR requirements { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
	if (!XRCHECK(getRequirements(instance, systemId, &requirements)))
		return false;

	EGLDisplay display = eglGetCurrentDisplay();
	EGLContext context = eglGetCurrentContext();
	EGLint configId = 0, configCount = 0;
	EGLConfig eglConfig = nullptr;
	if (context == EGL_NO_CONTEXT || !eglQueryContext(display, context, EGL_CONFIG_ID, &configId))
	{
		ERROR_LOG(RENDERER, "XR: no current EGL context");
		return false;
	}
	const EGLint configAttribs[] { EGL_CONFIG_ID, configId, EGL_NONE };
	if (!eglChooseConfig(display, configAttribs, &eglConfig, 1, &configCount) || configCount == 0)
	{
		ERROR_LOG(RENDERER, "XR: EGL config not found");
		return false;
	}
	XrGraphicsBindingOpenGLESAndroidKHR binding { XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
	binding.display = display;
	binding.config = eglConfig;
	binding.context = context;
#else
	PFN_xrGetOpenGLGraphicsRequirementsKHR getRequirements = nullptr;
	if (!XRCHECK(XR(xrGetInstanceProcAddr)(instance, "xrGetOpenGLGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&getRequirements)))
		return false;
	XrGraphicsRequirementsOpenGLKHR requirements { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
	if (!XRCHECK(getRequirements(instance, systemId, &requirements)))
		return false;

	XrGraphicsBindingOpenGLWin32KHR binding { XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
	binding.hDC = wglGetCurrentDC();
	binding.hGLRC = wglGetCurrentContext();
	if (binding.hDC == nullptr || binding.hGLRC == nullptr)
	{
		ERROR_LOG(RENDERER, "XR: no current OpenGL context (the PC version needs the OpenGL renderer)");
		return false;
	}
	// what goes into the sRGB eye images goes in as it is (see SwapchainFormats)
	glDisable(GL_FRAMEBUFFER_SRGB);
#endif
	XrSessionCreateInfo sessionInfo { XR_TYPE_SESSION_CREATE_INFO };
	sessionInfo.next = &binding;
	sessionInfo.systemId = systemId;
	if (!XRCHECK(XR(xrCreateSession)(instance, &sessionInfo, &session)))
		return false;

	XrReferenceSpaceCreateInfo spaceInfo { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	spaceInfo.poseInReferenceSpace.orientation.w = 1.f;
	if (!XRCHECK(XR(xrCreateReferenceSpace)(session, &spaceInfo, &localSpace)))
		return false;
	if (!initActions() || !initSwapchains())
		return false;
	NOTICE_LOG(RENDERER, "XR: session created");
	return true;
}

// The controllers the runtime has on each hand now (their interaction profile), logged when
// they change: the menu's hold rule (menuByHold) goes by them, and controllerProfile() gives them.
void readProfiles()
{
	std::string now[2];
	for (int hand : { Left, Right })
	{
		XrInteractionProfileState state { XR_TYPE_INTERACTION_PROFILE_STATE };
		char name[XR_MAX_PATH_LENGTH] = "";
		u32 length = 0;
		if (XR_SUCCEEDED(XR(xrGetCurrentInteractionProfile)(session, handPaths[hand], &state))
				&& state.interactionProfile != XR_NULL_PATH
				&& XR_SUCCEEDED(XR(xrPathToString)(instance, state.interactionProfile, sizeof(name), &length, name)))
			now[hand] = name;
	}
	if (now[Left] == profiles[Left] && now[Right] == profiles[Right])
		return;
	profiles[Left] = now[Left];
	profiles[Right] = now[Right];
	auto shown = [](const std::string& p) { return p.empty() ? "none" : p.c_str(); };
	if (profiles[Left] == profiles[Right])
		NOTICE_LOG(INPUT, "XR: controllers: %s", shown(profiles[Left]));
	else
		NOTICE_LOG(INPUT, "XR: controllers: %s (left), %s (right)", shown(profiles[Left]), shown(profiles[Right]));
}

// Vive wands and Windows Mixed Reality have no button to spare for the settings menu: their
// left Start (the menu button) held 0.8 s is the menu.
bool menuByHold()
{
	return profiles[Left] == "/interaction_profiles/htc/vive_controller"
			|| profiles[Left] == "/interaction_profiles/microsoft/motion_controller";
}

void pollEvents()
{
	XrEventDataBuffer event { XR_TYPE_EVENT_DATA_BUFFER };
	while (XR(xrPollEvent)(instance, &event) == XR_SUCCESS)
	{
		if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
		{
			const auto& change = *reinterpret_cast<XrEventDataSessionStateChanged *>(&event);
			NOTICE_LOG(RENDERER, "XR: session state %d", change.state);
			if (change.state == XR_SESSION_STATE_READY)
			{
				XrSessionBeginInfo begin { XR_TYPE_SESSION_BEGIN_INFO };
				begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				if (XRCHECK(XR(xrBeginSession)(session, &begin)))
				{
					sessionRunning = true;
					if (requestRefreshRate)
						XRCHECK(requestRefreshRate(session, (float)config::VrXrRefreshRate));
					if (setPerformanceLevel)
					{
						XRCHECK(setPerformanceLevel(session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT));
						XRCHECK(setPerformanceLevel(session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT));
					}
				}
			}
			else if (change.state == XR_SESSION_STATE_STOPPING)
			{
				XR(xrEndSession)(session);
				sessionRunning = false;
			}
			else if (change.state == XR_SESSION_STATE_LOSS_PENDING)
				sessionLost = true;
			else if (change.state == XR_SESSION_STATE_EXITING)
				exiting = true;
			const bool nowFocused = change.state == XR_SESSION_STATE_FOCUSED;
			const bool regained = nowFocused && !focused;
			if (regained && originSet
					&& std::chrono::steady_clock::now() - focusLost > std::chrono::seconds(3))
				// back after a while (headset put on again, long menu visit): the player
				// may face elsewhere now
				recenterRequested = true;
			if (focused && !nowFocused)
				focusLost = std::chrono::steady_clock::now();
			focused = nowFocused;
			if (nowFocused)
			{
				// the controllers' models, now that the session has its input (a Quest names its
				// controllers' models once it's focused and they are bound); the ones it didn't
				// have asked for again at each regained focus, then on the timer
				if (!controllersReady)
				{
					controllers::init(instance, session);
					controllersReady = true;
				}
				else if (regained)
					controllers::retry();
				modelRetries = 0;
				nextModelRetry = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			}
		}
		else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
		{
			sessionLost = true;
		}
		else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING)
		{
			recenterRequested = true;
		}
		else if (event.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED)
		{
			readProfiles();
			// controllers bound (or changed): their models may be there now
			if (controllersReady)
				controllers::retry();
		}
		event = { XR_TYPE_EVENT_DATA_BUFFER };
	}
}

glm::vec3 toVec(const XrVector3f& v) { return { v.x, v.y, v.z }; }
glm::quat toQuat(const XrQuaternionf& q) { return glm::quat(q.w, q.x, q.y, q.z); }

glm::quat yawOnly(const glm::quat& q)
{
	const glm::vec3 forward = q * glm::vec3(0, 0, -1);
	return glm::angleAxis(std::atan2(-forward.x, -forward.z), glm::vec3(0, 1, 0));
}

// Pose relative to the game camera's place in the room.
glm::vec3 relativePosition(const XrVector3f& p) {
	return glm::conjugate(originRot) * (toVec(p) - originPos);
}
glm::quat relativeRotation(const XrQuaternionf& q) {
	return glm::conjugate(originRot) * toQuat(q);
}

glm::mat4 eyeProjection(const XrFovf& fov)
{
	const float l = std::tan(fov.angleLeft), r = std::tan(fov.angleRight);
	const float u = std::tan(fov.angleUp), d = std::tan(fov.angleDown);
	glm::mat4 p(0.f);
	p[0][0] = 2.f / (r - l);
	p[1][1] = 2.f / (u - d);
	p[2][0] = (r + l) / (r - l);
	p[2][1] = (u + d) / (u - d);
	p[2][2] = -1.f;
	p[2][3] = -1.f;
	p[3][2] = -2.f * NearMetres;	// infinite far plane; depth comes from the game's own W
	return p;
}

// Rebuilt game eye space -> headset local space, relative to the origin.
glm::mat4 gameToWorld()
{
	const float s = worldScale();
	return glm::scale(glm::mat4(1.f), glm::vec3(s, -s, s));
}

// The head (between the eyes): head space -> room space (relative to the game camera's place).
glm::mat4 headPose()
{
	const glm::vec3 center = (relativePosition(views[0].pose.position) + relativePosition(views[1].pose.position)) * 0.5f;
	return glm::translate(glm::mat4(1.f), center) * glm::mat4_cast(relativeRotation(views[0].pose.orientation));
}

void updateOrigin(bool tracked)
{
	if (!tracked || (originSet && !recenterRequested))
		return;
	const glm::vec3 center = (toVec(views[0].pose.position) + toVec(views[1].pose.position)) * 0.5f;
	originPos = center;
	originRot = yawOnly(toQuat(views[0].pose.orientation));
	originSet = true;
	recenterRequested = false;
	NOTICE_LOG(RENDERER, "XR: game camera placed at the head");
	// (the menu's Recenter, or the headset put on again: the menu in front of the head again)
	if (menuOpen)
		menu::place(headPose());
}

// MÃ¶ller-Trumbore; returns the distance along the ray, or a negative value for a miss.
float rayTriangle(const glm::vec3& o, const glm::vec3& d, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
{
	const glm::vec3 e1 = b - a, e2 = c - a;
	const glm::vec3 p = glm::cross(d, e2);
	const float det = glm::dot(e1, p);
	if (std::abs(det) < 1e-9f)
		return -1.f;
	const float inv = 1.f / det;
	const glm::vec3 t = o - a;
	const float u = glm::dot(t, p) * inv;
	if (u < 0.f || u > 1.f)
		return -1.f;
	const glm::vec3 q = glm::cross(t, e1);
	const float v = glm::dot(d, q) * inv;
	if (v < 0.f || u + v > 1.f)
		return -1.f;
	return glm::dot(e2, q) * inv;
}

// Closer than this (game units) is the game's clamped near plane, not real geometry.
constexpr float MinSceneW = 0.5f;

// How the last aim was placed, for the shot log.
const char *aimHow = "";
// The last aim is on something the player sees but the game can't hit: outside its stock
// view (see aimAtScene). The dot greys and the trigger does nothing there.
bool aimOutside;
// ...and for 3D aims, the triangle that was hit (diagnostics: which game geometry the ray
// meets, at what original W).
struct AimHit
{
	const char *list = "";
	u32 poly = 0;
	float w[3] {};
	glm::vec2 xy[3] {};
	u32 tex = 0, tsp = 0, isp = 0;
	u8 col[4] {};
} aimHit;

bool onGameScreen(const glm::vec2& screen) {
	return screen.x >= 0.f && screen.x <= 1.f && screen.y >= 0.f && screen.y <= 1.f;
}

// Where the ray (game eye space) meets the plane the 2D overlay and flat screens are
// shown on, at their stock framing (see the VR_REPROJECT vertex shader and drawVrScreen):
// the position in screen ndc (-1..1, y down) and the distance along the ray.
bool hitOverlayPlane(const glm::vec3& o, const glm::vec3& d, const glm::vec2& dcSize, glm::vec2& ndc, float& dist)
{
	const float depth = vr::panelDepth();
	if (d.z > -1e-3f)
		return false;
	dist = (-depth - o.z) / d.z;
	if (dist <= 0.f)
		return false;
	const glm::vec3 hit = o + d * dist;
	const glm::vec2 halfSize = dcSize * 0.5f / (float)config::VrFocal * depth;
	ndc = glm::vec2(hit.x, hit.y) / halfSize;
	return true;
}

bool inTriangle(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b, const glm::vec2& c)
{
	auto side = [](const glm::vec2& p, const glm::vec2& a, const glm::vec2& b) {
		return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
	};
	const float s1 = side(p, a, b), s2 = side(p, b, c), s3 = side(p, c, a);
	return (s1 >= 0.f && s2 >= 0.f && s3 >= 0.f) || (s1 <= 0.f && s2 <= 0.f && s3 <= 0.f);
}

// Where on the game screen (0..1) the gun points, as the game's own hit test will see it,
// and how far along the ray the aim point is (game units).
//  - 2D overlay (menu items, text) is on its own plane at the stock framing: when the ray
//    passes through a piece of it, the shot goes there. So do all shots on a screen with
//    nothing but overlay (menus).
//  - Otherwise the nearest 3D surface, placed on the screen at the game's stock framing,
//    which its hit test uses even when its view is widened; in the wider band around
//    it: aimOutside.
//  - Nothing hit: the point far along the ray.
bool aimAtScene(const rend_context& ctx, const glm::vec3& o, const glm::vec3& d, glm::vec2& screen, float& dist)
{
	aimOutside = false;
	const GameCamera cam = gameCamera(ctx);
	const glm::vec3 comfort = comfortParams();
	static std::vector<glm::vec3> pos;
	static std::vector<glm::vec2> overlayNdc;
	enum Kind : u8 { Unused, Scene, Overlay };
	static std::vector<Kind> kind;
	pos.resize(ctx.verts.size());
	overlayNdc.resize(ctx.verts.size());
	kind.assign(ctx.verts.size(), Unused);
	for (size_t i = 0; i < ctx.verts.size(); i++)
	{
		const Vertex& v = ctx.verts[i];
		if (!(v.z > 0.f) || !std::isfinite(v.z))
			continue;
		const float w = 1.f / v.z;
		const glm::vec2 ndc(v.x * 2.f / cam.dcSize.x - 1.f, v.y * 2.f / cam.dcSize.y - 1.f);
		if (std::abs(w - cam.overlayW) < 0.002f)
		{
			// (hidden letterbox bars have NaN x/y and are in no triangle)
			overlayNdc[i] = ndc;
			kind[i] = Overlay;
		}
		else if (w >= MinSceneW)
		{
			// where it is seen: the comfort zone pulls close things back (along the line of
			// sight from the game camera, so the screen position stays the same)
			const glm::vec3 p(ndc.x * w * cam.tanHalf.x, ndc.y * w * cam.tanHalf.y, -w);
			pos[i] = p * comfortScale(p, comfort);
			kind[i] = Scene;
		}
		// else: a vertex the game clamped to its near plane (W 0.001, huge x/y). Its
		// triangles turn into long invisible slivers across the view that would catch the
		// aim; the visible part of such a polygon is hit through its other triangles.
	}

	glm::vec2 planeNdc;
	float planeDist = 0.f;
	const bool onPlane = hitOverlayPlane(o, d, cam.dcSize, planeNdc, planeDist);
	bool overlayHit = false;
	bool hasScene = false;
	float nearest = 1e30f;
	const char *listName = "";
	u32 polyIndex = 0;
	const PolyParam *poly = nullptr;
	auto triangle = [&](u32 a, u32 b, u32 c, bool solid, bool background) {
		if (a == RestartIndex || b == RestartIndex || c == RestartIndex
				|| a >= kind.size() || b >= kind.size() || c >= kind.size())
			return;
		if (kind[a] == Overlay && kind[b] == Overlay && kind[c] == Overlay)
		{
			// skip full-screen fades and backdrops: only buttons and text count
			const glm::vec2 e1 = overlayNdc[b] - overlayNdc[a], e2 = overlayNdc[c] - overlayNdc[a];
			if (onPlane && !overlayHit && std::abs(e1.x * e2.y - e1.y * e2.x) < 1.f
					&& inTriangle(planeNdc, overlayNdc[a], overlayNdc[b], overlayNdc[c]))
				overlayHit = true;
		}
		else if (solid && kind[a] == Scene && kind[b] == Scene && kind[c] == Scene)
		{
			if (!background)
				hasScene = true;
			const float t = rayTriangle(o, d, pos[a], pos[b], pos[c]);
			if (t > 0.f && t < nearest)
			{
				nearest = t;
				aimHit.list = listName;
				aimHit.poly = polyIndex;
				const u32 abc[3] { a, b, c };
				for (int k = 0; k < 3; k++)
				{
					const Vertex& v = ctx.verts[abc[k]];
					aimHit.w[k] = 1.f / v.z;
					aimHit.xy[k] = glm::vec2(v.x, v.y);
				}
				if (poly != nullptr)
				{
					aimHit.tex = poly->tcw.TexAddr << 3;
					aimHit.tsp = poly->tsp.full;
					aimHit.isp = poly->isp.full;
				}
				memcpy(aimHit.col, ctx.verts[a].col, 4);
			}
		}
	};
	// strips: first/count are ranges of the index buffer
	auto strips = [&](const std::vector<PolyParam>& list, size_t from, size_t to, bool solid, bool opaque) {
		listName = &list == &ctx.global_param_op ? "OP" : &list == &ctx.global_param_pt ? "PT" : "TR";
		for (size_t n = from; n < to && n < list.size(); n++)
		{
			const PolyParam& pp = list[n];
			polyIndex = (u32)n;
			poly = &pp;
			for (u32 j = pp.first; j + 2 < pp.first + pp.count && j + 2 < ctx.idx.size(); j++)
				// the first opaque polygon is the background plane
				triangle(ctx.idx[j], ctx.idx[j + 1], ctx.idx[j + 2], solid, opaque && n == 0);
		}
	};
	strips(ctx.global_param_op, 0, ctx.global_param_op.size(), true, true);
	strips(ctx.global_param_pt, 0, ctx.global_param_pt.size(), true, false);
	// Translucent polygons (most 2D text) of auto-sorted passes were turned into a list of
	// sorted triangles; their own first/count still point at vertices, not indices.
	RenderPass prev {};
	for (const RenderPass& pass : ctx.render_passes)
	{
		if (pass.sorted_tr_count != prev.sorted_tr_count)
		{
			for (u32 s = prev.sorted_tr_count; s < pass.sorted_tr_count && s < ctx.sortedTriangles.size(); s++)
			{
				const SortedTriangle& st = ctx.sortedTriangles[s];
				for (u32 j = st.first; j + 2 < st.first + st.count && j + 2 < ctx.idx.size(); j += 3)
					triangle(ctx.idx[j], ctx.idx[j + 1], ctx.idx[j + 2], false, false);
			}
		}
		else
			strips(ctx.global_param_tr, prev.tr_count, pass.tr_count, false, false);
		prev = pass;
	}

	if (onPlane && (overlayHit || !hasScene))
	{
		screen = planeNdc * 0.5f + 0.5f;
		dist = planeDist;
		aimHow = overlayHit ? "2D" : "menu";
		return onGameScreen(screen);
	}
	aimHow = nearest < 1e30f ? "3D" : "far";
	constexpr float Far = 10000.f;
	dist = nearest < Far ? nearest : Far;
	const glm::vec3 hit = o + d * dist;
	if (hit.z > -1e-3f)
		return false;
	// The game tests gun hits with its stock view, not the widened one it draws with. With
	// viewport widening that showed as shots drifting towards the centre, and it holds with
	// the game's own field of view widened too (vr.WidenFov): on the headset, a shot sent
	// at x 410 (of 640) had the game's hit effect ~8 degrees nearer the centre than the dot,
	// exactly where the stock framing puts it. So the hit point goes on the screen at the
	// stock framing: the shot lands where the dot is. The wider view around it is seen but
	// can't be hit (as in the original game, where it wasn't on screen): aimOutside.
	const glm::vec2 tan = config::VrAimWidened ? cam.tanHalf : cam.stockTan;
	screen = glm::vec2(hit.x / (-hit.z * tan.x), hit.y / (-hit.z * tan.y)) * 0.5f + 0.5f;
	if (onGameScreen(screen))
		return true;
	const glm::vec2 seen = glm::vec2(hit.x / (-hit.z * cam.tanHalf.x), hit.y / (-hit.z * cam.tanHalf.y)) * 0.5f + 0.5f;
	aimOutside = onGameScreen(seen);
	return false;
}

// The flat screen menus and notices are shown on (see drawVrScreen in gles.cpp).
bool aimAtFlatScreen(const glm::vec3& o, const glm::vec3& d, glm::vec2& screen, float& dist)
{
	aimOutside = false;
	glm::vec2 ndc;
	if (!hitOverlayPlane(o, d, glm::vec2(640.f, 480.f), ndc, dist))
		return false;
	screen = ndc * 0.5f + 0.5f;
	aimHow = "screen";
	return onGameScreen(screen);
}

// A button, of either hand or (for per-hand actions) of one
bool boolAction(XrAction action, int hand = -1)
{
	XrActionStateGetInfo info { XR_TYPE_ACTION_STATE_GET_INFO };
	info.action = action;
	if (hand >= 0)
		info.subactionPath = handPaths[hand];
	XrActionStateBoolean state { XR_TYPE_ACTION_STATE_BOOLEAN };
	return XR_SUCCEEDED(XR(xrGetActionStateBoolean)(session, &info, &state)) && state.isActive && state.currentState;
}

float floatAction(XrAction action, int hand)
{
	XrActionStateGetInfo info { XR_TYPE_ACTION_STATE_GET_INFO };
	info.action = action;
	info.subactionPath = handPaths[hand];
	XrActionStateFloat state { XR_TYPE_ACTION_STATE_FLOAT };
	return XR_SUCCEEDED(XR(xrGetActionStateFloat)(session, &info, &state)) && state.isActive ? state.currentState : 0.f;
}

// A buzz in a hand, at vr.Haptics' strength (hapticsLevel(), read on each call: the menu changes it
// live): 0 off, 1 light (as given: 1.2's normal), 2 medium (1.6 times as long: 1.2's strong, the
// default), 3 strong (2.4 times, half as long again as medium: the amplitudes are already at the full
// 1.0 where it counts, so a stronger knock is a longer one). Every kind of pulse alike.
float haptic(int hand, float seconds, float amplitude)
{
	static constexpr float durations[4] { 0.f, 1.f, 1.6f, 2.4f };
	const int strength = hapticsLevel();
	if (strength == 0)
		return 0.f;
	seconds *= durations[strength];
	XrHapticActionInfo info { XR_TYPE_HAPTIC_ACTION_INFO };
	info.action = hapticAction;
	info.subactionPath = handPaths[hand];
	XrHapticVibration vibration { XR_TYPE_HAPTIC_VIBRATION };
	vibration.duration = (XrDuration)(seconds * 1e9f);
	vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
	vibration.amplitude = amplitude;
	XR(xrApplyHapticFeedback)(session, &info, (const XrHapticBaseHeader *)&vibration);
	return seconds;
}

// A shot: a sharp knock. (Each returns how long it lasts: haptic().)
float recoil(int hand)
{
	return haptic(hand, 0.03f, 1.f);
}

// A pull on an empty gun: a light click.
float emptyClick(int hand)
{
	return haptic(hand, 0.012f, 0.35f);
}

// A round of the assault rifle fired with the trigger still held (12 a second, 83 ms apart):
// a short lighter knock, over long before the next, so a burst is a row of knocks, not a buzz.
float autoKnock(int hand)
{
	return haptic(hand, 0.02f, 0.6f);
}

glm::vec2 stickAction2()
{
	XrActionStateGetInfo info { XR_TYPE_ACTION_STATE_GET_INFO };
	info.action = stickAction;
	XrActionStateVector2f stick { XR_TYPE_ACTION_STATE_VECTOR2F };
	if (!XR_SUCCEEDED(XR(xrGetActionStateVector2f)(session, &info, &stick)) || !stick.isActive)
		return glm::vec2(0.f);
	return glm::vec2(stick.currentState.x, stick.currentState.y);
}

// The light gun's D-pad from a thumbstick, with some hysteresis: pushed past 60%,
// let go below 40%. Returns the lightgunSet bits (up 8, down 16, left 32, right 64).
u32 dpadFromStick(const glm::vec2& stick)
{
	static u32 held;
	auto axis = [&](float v, u32 negBit, u32 posBit) {
		const u32 was = held & (negBit | posBit);
		if (was == posBit ? v > 0.4f : v > 0.6f)
			return posBit;
		if (was == negBit ? v < -0.4f : v < -0.6f)
			return negBit;
		return 0u;
	};
	held = axis(stick.y, 16, 8) | axis(stick.x, 32, 64);
	return held;
}

// With a grip held, thumbstick up makes the world bigger around the player, down smaller
// (about 1.8x per second held), and right makes the gun bigger, left smaller (about 1.6x
// per second). Both are kept in emu.cfg when the stick is let go.
// Returns true while gripping (the stick then isn't the D-pad).
bool adjustWorldSize(XrTime time, const glm::vec2& stick)
{
	static XrTime lastTime;
	static bool worldChanged, gunChanged;
	const float dt = lastTime != 0 ? std::clamp((time - lastTime) * 1e-9f, 0.f, 0.1f) : 0.f;
	lastTime = time;
	XrActionStateGetInfo info { XR_TYPE_ACTION_STATE_GET_INFO };
	info.action = gripAction;
	XrActionStateFloat grip { XR_TYPE_ACTION_STATE_FLOAT };
	const bool gripped = XR_SUCCEEDED(XR(xrGetActionStateFloat)(session, &info, &grip)) && grip.isActive && grip.currentState > 0.7f;
	// Only on purpose: the grip squeezed with the stick at rest first, then the stick. A grip
	// held while the stick was already pushed (a menu, a hand that holds tight) changes nothing.
	static bool armed;
	if (!gripped)
		armed = false;
	else if (glm::length(stick) < 0.3f)
		armed = true;
	const glm::vec2 s = gripped && armed ? stick : glm::vec2(0.f);
	// the stronger direction only, so a diagonal doesn't change both
	const bool vertical = std::abs(s.y) >= std::abs(s.x);
	if (vertical && std::abs(s.y) > 0.5f)
	{
		const float scale = std::clamp(config::VrWorldScale * std::exp((s.y > 0.f ? 0.6f : -0.6f) * dt), 0.005f, 0.2f);
		config::VrWorldScale.set(scale);
		worldChanged = true;
	}
	else if (worldChanged)
	{
		worldChanged = false;
		config::saveFloat("config", "vr.WorldScale", config::VrWorldScale);
		NOTICE_LOG(RENDERER, "XR: world size %.4f m per game unit", (float)config::VrWorldScale);
	}
	// the size of whichever pistol is drawn (player 1's)
	const bool game = gameGun(handGun(guns[0].player));
	config::Option<float>& gunSize = game ? config::VrHandScale : config::VrGunScale;
	const char *gunKey = game ? "vr.HandScale" : "vr.GunScale";
	if (!vertical && std::abs(s.x) > 0.5f)
	{
		gunSize.set(std::clamp(gunSize * std::exp((s.x > 0.f ? 0.5f : -0.5f) * dt), 0.4f, 1.6f));
		gunChanged = true;
	}
	else if (gunChanged)
	{
		gunChanged = false;
		config::saveFloat("config", gunKey, gunSize);
		NOTICE_LOG(RENDERER, "XR: gun size %.2f (%s)", (float)gunSize, gunKey);
	}
	return gripped;
}

// Off the game screen, as a light gun sees it (any negative position).
constexpr int OffScreen = -10000;

// The other hand, with the game's pistol (xr_hands.h): the open hand follows the other
// controller. Its grip squeezed near the back of the slide takes the slide, and pulling it
// back all the way reloads, as the game knows it: a shot off the screen. Let go, the slide
// springs home. (The hero's staff has no slide: the other glove only follows the hand.)
struct Slide
{
	bool held;
	bool racked;		// this pull reloaded already
	bool squeezeWas;
	bool clack;			// pulled well back: it hits home with a knock
	float from;			// the hand along the barrel when it took the slide (model metres)
	float back;			// how far back the slide is (model metres)
	XrTime time;
} slide;
// How far the hand (the controller's grip) may be from the back of the slide to take it:
// the controllers can't overlap, and the pistol's back sits over the gun hand's ring.
constexpr float GrabReach = 0.11f;

void updateOtherHand(XrTime time, int hand, bool tracked, const XrSpaceLocation& location,
		const glm::mat4& restGun, float scale, bool mirror)
{
	const HandsModel *model = handsModel();
	const float dt = slide.time != 0 ? std::clamp((time - slide.time) * 1e-9f, 0.f, 0.1f) : 0.f;
	slide.time = time;
	GunView& gunView = guns[0].view;
	gunView.otherHand = false;
	if (!tracked || model == nullptr)
		slide.held = false;
	else
	{
		const glm::quat rot = relativeRotation(location.pose.orientation);
		const glm::mat4 aim = glm::translate(glm::mat4(1.f), relativePosition(location.pose.position)) * glm::mat4_cast(rot);
		glm::vec3 palm = HandPalm;
		XrSpaceLocation grip { XR_TYPE_SPACE_LOCATION };
		if (XR_SUCCEEDED(XR(xrLocateSpace)(gripSpaces[hand], aimSpaces[hand], time, &grip))
				&& (grip.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
				&& glm::length(toVec(grip.pose.position)) < 0.25f)
			palm = toVec(grip.pose.position);
		const glm::vec3 handAt = glm::vec3(aim * glm::vec4(palm, 1.f));
		// the hand in the pistol's model space (without recoil)
		const glm::vec3 inGun = glm::vec3(glm::inverse(restGun) * glm::vec4(handAt, 1.f));
		const float squeeze = floatAction(gripAction, hand);
		const bool squeezed = slide.squeezeWas ? squeeze > 0.35f : squeeze > 0.6f;
		if (config::VrSlideReload && model->travel > 0.f && squeezed && !slide.squeezeWas && !slide.held)
		{
			const glm::vec3 grabAt = glm::vec3(restGun * glm::vec4(model->grab + glm::vec3(0.f, 0.f, slide.back), 1.f));
			const float reach = glm::distance(grabAt, handAt);
			if (reach < GrabReach)
			{
				slide.held = true;
				slide.racked = false;
				slide.from = inGun.z - slide.back;
				haptic(hand, 0.012f, 0.35f);
			}
			NOTICE_LOG(INPUT, "XR: grip %d cm from the slide%s", (int)(reach * 100.f), slide.held ? ": took it" : "");
		}
		slide.squeezeWas = squeezed;
		if (slide.held && !squeezed)
		{
			slide.held = false;
			slide.clack = slide.back > model->travel * 0.5f;
		}
		if (slide.held)
		{
			slide.back = std::clamp(inGun.z - slide.from, 0.f, model->travel);
			if (!slide.racked && slide.back > model->travel * 0.85f)
			{
				slide.racked = true;
				guns[0].reloadAt = time;
				haptic(hand, 0.025f, 0.8f);
				haptic(guns[0].hand, 0.025f, 0.6f);
				NOTICE_LOG(INPUT, "XR: slide racked (reload)");
			}
		}
		gunView.otherHand = true;
		gunView.handPose = slide.held
				// on the slide, going along with it
				? gunView.pose * glm::translate(glm::mat4(1.f), glm::vec3(0.f, 0.f, slide.back)) * model->onSlide
				// the model is a left hand: mirrored for the right
				: aim * glm::translate(glm::mat4(1.f), palm) * glm::scale(glm::mat4(1.f), glm::vec3(mirror ? -scale : scale, scale, scale));
	}
	if (!slide.held && slide.back > 0.f)
	{
		// home it goes, fast
		slide.back = std::max(0.f, slide.back - dt * 1.5f);
		if (slide.back == 0.f && slide.clack)
			haptic(guns[0].hand, 0.02f, 0.7f);
	}
	if (slide.back == 0.f)
		slide.clack = false;
	gunView.slide = slide.back;
}

// The knocks from the game's own gun (vr::gunState, read at the game's last VBlank), each frame:
// each shot it fires (its shot counter goes up) that fired a round (vr::GunState::fired: the ammo
// went down, or the rifle is held) is a Shot, else (a pull on an empty gun counted) a click. With
// the assault rifle, the first round after a pull is a Shot and the rest of the burst Auto knocks
// (autoKnock), one per round. Each Shot and Auto knock also kicks the gun and flashes its muzzle
// (whatever vr.Haptics says), so a pull that fires no round kicks none. A pull on the screen it
// hasn't fired within the profile's measured input delay is a click. Only shots of this hand's trigger knock: held, or let go under 0.25 s ago (the
// counter also moves when the game clears its RAM at boot, and its attract demo's players shoot).
// The knocks go to the hand one at a time, each once the one before has ended (two haptic calls
// closer than that feel as one: the second cuts the first short). Without a GunRam in the game's
// profile (gunRam false) the knock comes on the pull instead, as in v1 (updateGun); with one but
// the emulator stopped (loading, Flycast's own GUI), there is none.
void gunKnocks(Gun& g, XrTime time, bool trigger)
{
	if (trigger)
		g.heldAt = time;
	if (trigger && !g.triggerWas)
		g.freshPull = true;
	vr::GunState now;
	g.gunRam = vr::gunState(g.player, now);
	if (!now.valid)
	{
		g.gunKnown = false;
		g.pullAt = 0;
		g.shotsDue = g.autosDue = g.clicksDue = 0;
		return;
	}
	const int rises = g.gunKnown ? (s16)(u16)(now.shots - g.gunWas.shots) : 0;
	const bool pulled = g.heldAt != 0 && time - g.heldAt < 250'000'000LL;
	if (rises != 0)
	{
		// (a drop, or more than 8 in one read: the counter reset, a new game; no knock)
		if (pulled && rises > 0 && rises <= 8)
		{
			const int fired = std::clamp((int)(s16)(u16)(now.fired - g.gunWas.fired), 0, rises);
			const bool rifle = now.rifle || g.gunWas.rifle;
			if (fired > 0 && inCutscene && !cutsceneVeto)
			{
				cutsceneVeto = true;
				NOTICE_LOG(INPUT, "XR: cutscene ended by a shot (P%d)", g.player + 1);
			}
			for (int i = 0; i < fired; i++)
			{
				if (!rifle || g.freshPull)
					g.shotsDue++;
				else
					g.autosDue++;
				g.freshPull = false;
			}
			g.clicksDue += rises - fired;
		}
		g.pullAt = 0;
	}
	else if (g.pullAt != 0 && time - g.pullAt > (XrTime)vr::gunShotWaitMs() * 1'000'000LL)
	{
		g.clicksDue++;
		g.pullAt = 0;
	}
	g.gunWas = now;
	g.gunKnown = true;
	// the next knock once the last has ended (and 10 ms more)
	if (time >= g.knockEnds && g.shotsDue + g.autosDue + g.clicksDue > 0)
	{
		float lasts;
		if (g.shotsDue > 0)
		{
			g.shotsDue--;
			lasts = recoil(g.hand);
			// a round fired: the kick and the flash (whatever vr.Haptics says: off, the knock is none
			// but the gun still kicks), at the muzzle of the gun the hand holds
			g.lastShot = time;
			g.shotCount++;
			g.autoKick = false;
			g.burstFlashes++;
			NOTICE_LOG(INPUT, "XR: P%d kick: %s", g.player + 1, gunName(g.view.kind));
		}
		else if (g.autosDue > 0)
		{
			g.autosDue--;
			lasts = autoKnock(g.hand);
			// each round of the burst flashes and kicks too, a little (Ruling S33: one flash for the
			// whole burst looked like the rifle had stopped)
			g.lastShot = time;
			g.shotCount++;
			g.autoKick = true;
			g.burstFlashes++;
		}
		else
		{
			g.clicksDue--;
			lasts = emptyClick(g.hand);
		}
		g.knockEnds = time + (XrTime)((lasts + 0.01f) * 1e9f);
	}
}

// Whether a gun's trigger is held at this pull: a clean click, pulled past 60%, let go below 35%,
// so a resting finger or a half release doesn't fire twice.
bool triggerHeld(const Gun& g, float pull)
{
	return g.triggerWas ? pull > 0.35f : pull > 0.6f;
}

// One gun for a headset frame: where it points (handed to the game's hit test at its stock
// framing), how it's drawn, and the buttons that go to its player's light gun. `extra` are
// the buttons that aren't the trigger's (Start, B, the D-pad). With `slide`, the other hand
// (`other`) is free to rack the game pistol's slide.
void updateGun(Gun& g, XrTime time, const XrSpaceLocation *locations, const bool *tracked, const float *pulls,
		u32 extra, bool slide, bool live = true)
{
	const float pull = pulls[g.hand];
	const bool trigger = triggerHeld(g, pull);

	glm::vec2 screen(-1.f);
	bool onScreen = false;
	aimOutside = false;
	const XrSpaceLocation& location = locations[g.hand];
	if (originSet && tracked[g.hand])
	{
		const glm::quat rot = relativeRotation(location.pose.orientation);
		const glm::mat4 pose = glm::translate(glm::mat4(1.f), relativePosition(location.pose.position)) * glm::mat4_cast(rot);
		// The drawn gun: the middle of its grip where the hand is (the controller's grip
		// pose, seen from the aim pose), at vr.GunScale times the model's size.
		glm::vec3 palm = HandPalm;
		XrSpaceLocation grip { XR_TYPE_SPACE_LOCATION };
		const bool fromRuntime = XR_SUCCEEDED(XR(xrLocateSpace)(gripSpaces[g.hand], aimSpaces[g.hand], time, &grip))
				&& (grip.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
				&& glm::length(toVec(grip.pose.position)) < 0.25f;
		if (fromRuntime)
			palm = toVec(grip.pose.position);
		if (g.palmLogged != g.hand * 2 + fromRuntime)
		{
			g.palmLogged = g.hand * 2 + fromRuntime;
			NOTICE_LOG(INPUT, "XR: %s hand at %.3f %.3f %.3f from the aim pose (%s)", g.hand == Right ? "right" : "left",
					palm.x, palm.y, palm.z, fromRuntime ? "runtime" : "fallback");
		}
		// the gun in this hand (Task 14: the rifle for a while), its model and muzzle
		const GunKind kind = handGun(g.player);
		// (looked up once: each lookup can load the model, and one made meanwhile might not read)
		const HandsModel *model = handModel(kind);
		const bool game = model != nullptr;
		const glm::vec3 muzzle = game ? model->muzzle : GunMuzzle;
		const float gunScale = std::clamp(game ? (float)config::VrHandScale : (float)config::VrGunScale, 0.4f, 1.6f);
		// the game's pistol is in a right hand: mirrored for the left
		const bool mirror = game && g.hand == Left;
		const glm::mat4 placement = gunPlacement(game, gunScale, palm, mirror);
		// vr.GunAngle (the menu's Gun angle): the gun tilted about the hand's side-to-side axis
		// through the grip, + the muzzle up; the shots, the aim line and the flash with it (every
		// gun, both hands, the rifle too)
		const int angle = std::clamp((int)config::VrGunAngle, -30, 30);
		const glm::mat4 tilt = glm::translate(glm::mat4(1.f), palm)
				* glm::rotate(glm::mat4(1.f), glm::radians((float)angle), glm::vec3(1, 0, 0))
				* glm::translate(glm::mat4(1.f), -palm);
		if (g.muzzleLogged != (int)kind || g.angleLogged != angle)
		{
			// where shots leave, as the aim line and the flash start (Task 14b: a gun of the player's own)
			g.muzzleLogged = (int)kind;
			g.angleLogged = angle;
			const glm::vec3 at = glm::vec3(tilt * placement * glm::vec4(muzzle, 1.f));
			NOTICE_LOG(INPUT, "XR: P%d muzzle: %s at %.4f %.4f %.4f from the aim pose", g.player + 1, gunName(kind), at.x, at.y, at.z);
		}
		// Shots leave the muzzle along the barrel, so the aim line and the hit agree. Into
		// rebuilt game eye space for the hit test.
		const glm::vec3 o = glm::vec3(pose * tilt * placement * glm::vec4(muzzle, 1.f));
		const glm::vec3 d = rot * (glm::mat3(tilt) * glm::vec3(0, 0, -1));
		const float s = worldScale();
		const glm::vec3 og = glm::vec3(o.x, -o.y, o.z) / s;
		const glm::vec3 dg = glm::normalize(glm::vec3(d.x, -d.y, d.z));
		float dist = -1.f;
		const rend_context *ctx = gles_vr_frame();
		if (gles_vr_showing_framebuffer())
			onScreen = aimAtFlatScreen(og, dg, screen, dist);
		else if (ctx != nullptr)
			onScreen = aimAtScene(*ctx, og, dg, screen, dist);

		// Recoil: back and muzzle up around the hand, with a small bounce (gunKick) and a
		// little sideways twist that differs per shot. With the game's gun state (gunRam) the
		// kick and the flash come with each round it fires (gunKnocks' Shot and Auto knocks):
		// a pull on an empty gun fires none, so it kicks none (CM VR 1.2 spec section 3). Without
		// it, as in v1, on the pull: a shot fired now shows its flash and kick in this very frame.
		if (trigger && !g.triggerWas)
			g.burstFlashes = 0;
		if (trigger && !g.triggerWas && onScreen && !g.gunRam)
		{
			g.lastShot = time;
			g.shotCount++;
			g.autoKick = false;
			g.burstFlashes = 1;
		}
		const float sinceShot = g.lastShot != 0 ? (time - g.lastShot) * 1e-9f : 1e9f;
		// (a round of the rifle's burst after the pull: half the kick)
		const float kick = gunKick(sinceShot) * (g.autoKick ? 0.5f : 1.f);
		const float twist = ((g.shotCount * 2654435761u) >> 24) / 255.f - 0.5f;	// -0.5..0.5 per shot
		const glm::vec3 hand = palm;
		// A long one (the hero's staff, its head 75 cm out) tips up less: its muzzle rises no
		// more than one 40 cm from the hand would. Every pistol here is shorter.
		const float reach = glm::distance(glm::vec3(placement * glm::vec4(muzzle, 1.f)), hand);
		const float tip = std::min(1.f, 0.4f / std::max(reach, 0.01f));
		glm::mat4 recoilMat = glm::translate(glm::mat4(1.f), glm::vec3(0.f, 0.f, 0.035f * kick) + hand);
		recoilMat = glm::rotate(recoilMat, glm::radians(18.f * tip * kick), glm::vec3(1, 0, 0));
		recoilMat = glm::rotate(recoilMat, glm::radians(6.f * tip * twist * std::max(kick, 0.f)), glm::vec3(0, 1, 0));
		recoilMat = glm::translate(recoilMat, -hand);
		GunView& view = g.view;
		view.kind = kind;
		view.pose = pose * tilt * recoilMat * placement;
		// (the game's pistol has a smaller muzzle than the arcade gun's lens)
		view.scale = game ? gunScale * 0.8f : gunScale;
		view.restMuzzle = o;
		view.restForward = d;
		view.trigger = std::clamp(pull, 0.f, 1.f);
		view.now = time * 1e-9;
		view.sinceShot = sinceShot;
		view.shot = g.shotCount;
		view.player = g.player;
		view.aimLine = config::VrLaser;
		view.aimDot = config::VrLaser && (onScreen || aimOutside);
		view.aimOutside = aimOutside && !onScreen;
		view.aimPoint = o + d * (dist > 0.f ? dist * s : 5.f);
		// (a hand that holds its controller shows that instead: before play, the menu's How to play)
		g.visible = config::VrShowGun && holds(g.hand) == Holds::Gun;
		// (the slide and the open hand only for a gun that's held: not while the hand holds its controller)
		// (a gun alone, Confidential Mission's: no hand of the game's for the other one)
		if (game && slide && !model->gunOnly && holds(g.hand) == Holds::Gun)
			updateOtherHand(time, 1 - g.hand, tracked[1 - g.hand], locations[1 - g.hand], pose * tilt * placement, gunScale, mirror);
		else
		{
			view.otherHand = false;
			view.slide = 0.f;
		}
		// A gun in each hand leaves no hand for the slide: pointing a gun at the floor
		// reloads it instead (a gun that's held: not while the hand holds its controller).
		// With vr.Reload 1 pointing away from the screen does (below), once each time.
		if (!slide && live && holds(g.hand) == Holds::Gun && config::VrReload != 1)
		{
			if (!g.pointedDown && d.y < -0.75f)
			{
				g.pointedDown = true;
				g.reloadAt = time;
				haptic(g.hand, 0.02f, 0.5f);
				NOTICE_LOG(INPUT, "XR: player %d pointed down (reload)", g.player + 1);
			}
			else if (g.pointedDown && d.y > -0.5f)
				g.pointedDown = false;
		}
	}

	if (!live)
	{
		// The game is paused (the VR menu): the gun is only drawn where the hand is. A trigger
		// held when the game goes on again waits to be let go.
		g.triggerWas = true;
		g.triggerWait = true;
		g.pullAt = 0;
		g.shotsDue = g.autosDue = g.clicksDue = 0;
		g.offArmed = false;		// (the menu, or closing it pointing away, reloads nothing)
		return;
	}
	gunKnocks(g, time, trigger);
	if (!trigger && g.triggerWas && g.burstFlashes > 1)
		NOTICE_LOG(INPUT, "XR: P%d rifle burst: %u flashes", g.player + 1, g.burstFlashes);
	// vr.Reload 1 (the menu's Reload: Point off-screen): the aim leaving the screen reloads once,
	// as a pull off it would, with a reload's buzz and no kick; it re-arms back on the screen. Not
	// into the grey margin (aimOutside), not with the magazine full (the game's gun state, when
	// it has one), nor while the hand isn't tracked or holds its controller.
	if (config::VrReload == 1 && holds(g.hand) == Holds::Gun && originSet && tracked[g.hand])
	{
		if (onScreen)
			g.offArmed = true;
		else if (g.offArmed && !aimOutside && (!g.gunRam || (g.gunKnown && g.gunWas.ammo < g.gunWas.magazine)))
		{
			g.offArmed = false;
			g.reloadAt = time;
			haptic(g.hand, 0.02f, 0.5f);
			NOTICE_LOG(INPUT, "XR: player %d pointed off screen (reload)", g.player + 1);
		}
	}
	else
		g.offArmed = false;	// (a cutscene or the controller held: the guns' return reloads nothing)

	// Seen but outside the game's view: the trigger does nothing (no shot, no reload).
	const bool outside = !onScreen && aimOutside;
	// What a pull does is decided when it starts. One started in the grey margin never
	// fires. One started on screen (a shot) or off it (a reload) keeps the game's A held
	// without a break until it is let go, also while crossing the margin: the reload bit is
	// A held too (maple_lightgun::transform_kcode), so no new press reaches the game and a
	// held trigger never fires again by sweeping.
	if (trigger && !g.triggerWas)
		g.pullOutside = outside;
	u32 buttons = 0;
	if (trigger && !g.pullOutside)
		// pointing away from the screen fires off-screen: that's how you reload
		buttons |= onScreen ? 1 : 2;
	// A shot at the 2D plane (menus, text, a flat screen): the game's own shot marker lands
	// right where it hit, so it stays (vr::dropShotMarker).
	if (trigger && onScreen && (!strcmp(aimHow, "2D") || !strcmp(aimHow, "menu") || !strcmp(aimHow, "screen")))
		buttons |= 256;
	buttons |= extra;
	// Lupin's cabinet B is the off-screen sensor, not CM's second button.
	// Use the existing bounded reload pulse/feedback and swallow it when released.
	if (vr::gameManualOffscreenReload())
	{
		const bool pressed = (extra & 128) != 0;
		if (pressed && !g.reloadButtonWas) { g.reloadAt = time; haptic(g.hand, 0.02f, 0.5f); NOTICE_LOG(INPUT, "XR: player %d reload button (sensor)", g.player + 1); }
		g.reloadButtonWas = pressed;
		buttons &= ~128u;
	}
	// A reload (the slide racked, the gun pointed down): the trigger let go for a moment,
	// then pulled off the screen. A trigger held through it waits to be let go, so it
	// doesn't fire right after.
	bool reloading = false;
	if (g.reloadAt != 0)
	{
		const float since = (time - g.reloadAt) * 1e-9f;
		if (since < 0.15f)
		{
			buttons &= ~3u;
			if (since >= 0.05f)
			{
				buttons |= 2;
				reloading = true;
			}
			g.triggerWait = g.triggerWait || trigger;
		}
		else
			g.reloadAt = 0;
	}
	if (g.triggerWait && !trigger)
		g.triggerWait = false;
	if (g.triggerWait && g.reloadAt == 0)
		buttons &= ~3u;
	// Trigger + B + Start is the game's own reset. A and B sit under one thumb: not by accident.
	if ((buttons & 128) && (buttons & 3))
		buttons &= ~4u;
	if (trigger && !g.triggerWas && outside)
		NOTICE_LOG(INPUT, "XR: player %d's trigger outside the game's view (no shot)", g.player + 1);
	else if (trigger && !g.triggerWas)
	{
		// The knock: when the game fires (gunKnocks), for a pull that reached it on the screen
		// (a reload off it is none); with no gun state in the game's profile, on the pull as in
		// v1; none while the emulator isn't running.
		if (!g.gunRam)
			recoil(g.hand);
		else if (g.gunKnown && (buttons & 1))
			g.pullAt = time;
		lastShotScreen = onScreen ? glm::vec2(screen.x * 640.f, screen.y * 480.f) : glm::vec2(-1.f);
		shotSerial++;
		// diagnostics: where shots go, to compare with what the game makes of them
		if (onScreen && !strcmp(aimHow, "3D"))
			NOTICE_LOG(INPUT, "XR: player %d shot at %d,%d (3D, %d cm) hit %s %u W %.1f/%.1f/%.1f at %.0f,%.0f %.0f,%.0f %.0f,%.0f tex %06x tsp %08x isp %08x col %02x%02x%02x%02x",
					g.player + 1, (int)(screen.x * 640.f), (int)(screen.y * 480.f), (int)(glm::length(g.view.aimPoint - g.view.restMuzzle) * 100.f),
					aimHit.list, aimHit.poly, aimHit.w[0], aimHit.w[1], aimHit.w[2], aimHit.xy[0].x, aimHit.xy[0].y,
					aimHit.xy[1].x, aimHit.xy[1].y, aimHit.xy[2].x, aimHit.xy[2].y, aimHit.tex, aimHit.tsp, aimHit.isp,
					aimHit.col[0], aimHit.col[1], aimHit.col[2], aimHit.col[3]);
		else if (onScreen)
			NOTICE_LOG(INPUT, "XR: player %d shot at %d,%d (%s, %d cm)", g.player + 1, (int)(screen.x * 640.f), (int)(screen.y * 480.f), aimHow,
					(int)(glm::length(g.view.aimPoint - g.view.restMuzzle) * 100.f));
		else
			NOTICE_LOG(INPUT, "XR: player %d shot off screen (reload)", g.player + 1);
	}
	g.triggerWas = trigger;
	const glm::ivec2 pos = onScreen && !reloading ? glm::ivec2(screen * 10000.f) : glm::ivec2(OffScreen);
	lightgunSet(g.player, pos.x, pos.y, buttons);
}

// Each hand's aim pose, and whether it's tracked.
void locateHands(XrTime time, XrSpaceLocation *locations, bool *tracked)
{
	const XrSpaceLocationFlags valid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	for (int hand : { Left, Right })
	{
		locations[hand] = { XR_TYPE_SPACE_LOCATION };
		tracked[hand] = XR_SUCCEEDED(XR(xrLocateSpace)(aimSpaces[hand], localSpace, time, &locations[hand]))
				&& (locations[hand].locationFlags & valid) == valid;
	}
}

// The gun in a hand, drawn this frame (nullptr: none)
Gun *gunInHand(int hand)
{
	for (int i = 0; i < (dualWield ? 2 : 1); i++)
		if (guns[i].visible && guns[i].hand == hand)
			return &guns[i];
	return nullptr;
}

// The controllers' models the runtime didn't have at the last ask (a controller asleep, hand
// tracking on, nothing bound yet): asked for again every 2 s while focused, 15 times a focus.
void retryControllerModels()
{
	if (!controllersReady || !focused || modelRetries >= 15 || !controllers::retryWanted())
		return;
	const auto now = std::chrono::steady_clock::now();
	if (now < nextModelRetry)
		return;
	modelRetries++;
	nextModelRetry = now + std::chrono::seconds(2);
	controllers::retry();
}

// The controllers drawn this frame (xr_controllers.h): only while how to play shows (labelsOn():
// before play starts, the menu's How to play; always with the light gun off, vr.XrGun), in each
// tracked hand that shows no gun (not in the game's cutscenes: the hands are empty then), at its grip pose, while focused and vr.ShowControllers is on. Once
// play starts a hand without a gun draws nothing (the menu's pointer lines still start from it).
// Logs "XR: controllers shown: left and right" (none, left, right) where that changes, tracking aside.
void updateControllers(XrTime time)
{
	const XrSpaceLocationFlags valid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	const bool howToPlay = labelsOn() || !config::VrXrGun;
	int shown = 0;		// the hands that show one: 1 left, 2 right
	for (int hand : { Left, Right })
	{
		controllerShown[hand] = false;
		if (!controllersReady || !focused || !originSet || !config::VrShowControllers || !howToPlay || gunInHand(hand) != nullptr)
			continue;
		shown |= hand == Left ? 1 : 2;
		XrSpaceLocation location { XR_TYPE_SPACE_LOCATION };
		if (XR_SUCCEEDED(XR(xrLocateSpace)(gripSpaces[hand], localSpace, time, &location))
				&& (location.locationFlags & valid) == valid)
		{
			controllerGrip[hand] = glm::translate(glm::mat4(1.f), relativePosition(location.pose.position))
					* glm::mat4_cast(relativeRotation(location.pose.orientation));
			controllerShown[hand] = true;
			controllerAim[hand] = controllerGrip[hand];		// (without an aim pose: the grip's)
			XrSpaceLocation aim { XR_TYPE_SPACE_LOCATION };
			if (XR_SUCCEEDED(XR(xrLocateSpace)(aimSpaces[hand], localSpace, time, &aim)) && (aim.locationFlags & valid) == valid)
				controllerAim[hand] = glm::translate(glm::mat4(1.f), relativePosition(aim.pose.position))
						* glm::mat4_cast(relativeRotation(aim.pose.orientation));
		}
	}
	static int shownWas = 0;
	if (shown != shownWas)
	{
		static const char *const Shown[] { "none", "left", "right", "left and right" };
		NOTICE_LOG(INPUT, "XR: controllers shown: %s", Shown[shown]);
		shownWas = shown;
	}
}

// What each hand draws this frame, logged where it changes: "XR: in the hands: left nothing, right gun".
// Its gun (with its laser line and dot), its controller, the menu's pointer line from a hand without a
// gun, or nothing (the game's cutscenes: both hands).
void noteDrawn()
{
	static std::string drawnWas;
	std::string drawn;
	for (int hand : { Left, Right })
	{
		std::string what;
		if (gunInHand(hand) != nullptr)
			what = "gun";
		if (controllerShown[hand])
			what += what.empty() ? "controller" : " and controller";
		if (pointerShown[hand])
			what += what.empty() ? "pointer" : " and pointer";
		drawn += std::string(hand == Left ? "left " : ", right ") + (what.empty() ? "nothing" : what);
	}
	if (drawn != drawnWas)
	{
		NOTICE_LOG(INPUT, "XR: in the hands: %s", drawn.c_str());
		drawnWas = drawn;
	}
}

// The settings menu, open (xr_menu.h): the game is paused (applyPause) and gets nothing; the guns
// only follow the hands. Each hand points at the menu, from its gun's muzzle (else from the
// controller), always with a line and, on the panel, a dot (whatever vr.Laser says); its trigger
// chooses. The stick moves and steps, A chooses, the menu button or B closes. The frame it opens
// on takes no button: the controls' states are only learnt then.
void updateMenu(XrTime time, bool menuEdge, bool skip, bool skipEdge)
{
	XrSpaceLocation locations[2];
	bool tracked[2];
	locateHands(time, locations, tracked);
	if (config::VrXrGun)
	{
		const float none[2] {};
		updateGun(guns[0], time, locations, tracked, none, 0, false, false);
		if (dualWield)
			updateGun(guns[1], time, locations, tracked, none, 0, false, false);
	}
	static bool aWas, pullWas[2];
	static int stickWas;	// 0 at rest, 1 up, 2 down, 3 left, 4 right: pushed past 60%, let go below 40%
	static XrTime repeatAt;	// held left or right: when it steps again (menu::Input::repeat)
	menu::Input input {};
	const bool a = boolAction(startAction, Right);
	input.select = a && !aWas;
	aWas = a;
	const glm::vec2 stick = stickAction2();
	int push = 0;
	if (stickWas != 0 && glm::length(stick) >= 0.4f)
		push = stickWas;
	else if (std::abs(stick.y) >= std::abs(stick.x))
		push = stick.y > 0.6f ? 1 : stick.y < -0.6f ? 2 : 0;
	else
		push = stick.x > 0.6f ? 4 : stick.x < -0.6f ? 3 : 0;
	if (push != stickWas)
	{
		input.up = push == 1;
		input.down = push == 2;
		input.left = push == 3;
		input.right = push == 4;
		repeatAt = time + 400000000LL;
	}
	else if ((push == 3 || push == 4) && time >= repeatAt)
	{
		// held on the same push: again after 0.4 s, then every 0.1 s
		input.left = push == 3;
		input.right = push == 4;
		input.repeat = true;
		repeatAt = time + 100000000LL;
	}
	stickWas = push;
	input.toggle = menuEdge;
	input.back = skipEdge;
	for (int hand : { Left, Right })
	{
		const float pull = tracked[hand] ? floatAction(triggerAction, hand) : 0.f;
		const bool pulled = pullWas[hand] ? pull > 0.35f : pull > 0.6f;
		menu::Input::Ray& ray = input.ray[hand];
		ray.click = pulled && !pullWas[hand];
		pullWas[hand] = pulled;
		if (!originSet || !tracked[hand])
			continue;
		ray.valid = true;
		if (const Gun *gun = gunInHand(hand))
		{
			ray.origin = gun->view.restMuzzle;
			ray.dir = gun->view.restForward;
		}
		else
		{
			ray.origin = relativePosition(locations[hand].pose.position);
			ray.dir = relativeRotation(locations[hand].pose.orientation) * glm::vec3(0.f, 0.f, -1.f);
		}
	}
	if (menuFresh)
	{
		menuFresh = false;
		input.toggle = input.back = input.select = input.up = input.down = input.left = input.right = input.repeat = false;
		input.ray[Left].click = input.ray[Right].click = false;
	}
	// B or A pressed in the menu goes no further than the menu: not to the game once it goes on
	if (input.back)
		skipSwallow = skip;
	if (input.select)
		startSwallow = true;
	menu::update(input);

	for (int hand : { Left, Right })
	{
		const menu::Input::Ray& ray = input.ray[hand];
		if (!ray.valid)
			continue;
		glm::vec3 at;
		const bool onPanel = menu::pointer(hand, at);
		Gun *gun = gunInHand(hand);
		GunView& view = gun != nullptr ? gun->view : pointers[hand];
		if (gun == nullptr)
		{
			// a line from the controller (the glow pass's muzzle is its pose times the muzzle)
			view = GunView();
			view.pose = glm::translate(glm::mat4(1.f), ray.origin - gunMuzzle(view.kind));
			view.restMuzzle = ray.origin;
			view.restForward = ray.dir;
			view.now = time * 1e-9;
			view.shot = guns[0].view.shot;	// (no new shot: no smoke)
			view.player = guns[0].player;
			pointerShown[hand] = true;
		}
		view.aimLine = true;
		view.aimDot = onPanel;
		view.aimOutside = false;
		view.aimPoint = onPanel ? at : ray.origin + ray.dir * 3.f;
	}
}

void updateLightgun(XrTime time)
{
	for (Gun& g : guns)
		g.visible = false;
	pointerShown[Left] = pointerShown[Right] = false;
	if (config::VrXrGun && newGame.exchange(false))
	{
		if (dualWield)
			lightgunSet(1, OffScreen, OffScreen, 0);
		dualWield = false;
		guns[1] = Gun { 1, Left };
		guns[0].triggerWas = true;
		slide.held = false;
		p2JoinHeld = false;
		p2StartWas = true;	// a Start of player 2's still held doesn't join at once
		// controllers in both hands again, with the labels (a How to play left on ends, with its line)
		p1Armed = false;
		setHowToPlay(false, "new game");
		noteHolds("new game");
	}
	// The menu's How to play ends with the menu, however it closed (the menu button, B, Resume, a
	// game started elsewhere): the hands get back what they held.
	if (menuHowToPlay && !menuOpen)
		setHowToPlay(false, "menu closed");
	// (a round fired in cutscene mode vetoes it until the bars go: cutsceneVeto)
	const bool scene = p1Armed && vr::cutscene();
	if (!scene)
		cutsceneVeto = false;
	if ((scene && !cutsceneVeto) != inCutscene)
	{
		inCutscene = !inCutscene;
		noteHolds(inCutscene ? "cutscene" : cutsceneVeto ? "a shot" : "play");
	}
	// The game runs itself to its game over scene for the agent's hands, or its attract demo
	// for the hero's staff (hands_rip.h): its buttons are its own then.
	const bool preparing = hands::preparing();
	if (!focused)
	{
		// headset menu or app in the background: let go of everything
		if (config::VrXrGun && !preparing && !menuOpen)
			for (const Gun& g : guns)
				lightgunSet(g.player, OffScreen, OffScreen, 0);
		for (Gun& g : guns)
		{
			g.triggerWas = false;
			g.pullAt = 0;	// (no click for a pull just before, on the first focused frame)
		}
		leftHoldSince = leftTapUntil = 0;
		leftHoldMenu = false;
		return;
	}
	const XrActiveActionSet active { actionSet, XR_NULL_PATH };
	XrActionsSyncInfo sync { XR_TYPE_ACTIONS_SYNC_INFO };
	sync.countActiveActionSets = 1;
	sync.activeActionSets = &active;
	if (!XR_SUCCEEDED(XR(xrSyncActions)(session, &sync)))
		return;
	if (!boolAction(startAction, Right))
		startSwallow = false;
	// The left Start, and the settings menu just asked for (menuEdge). Where the menu is the left
	// Start held 0.8 s (menuByHold), that hold is the menu, and a shorter press acts as the left
	// Start once it's let go, held for 150 ms (the game reads its buttons once a frame).
	bool leftStart = boolAction(startAction, Left);
	const bool menu = boolAction(menuAction);
	bool menuEdge = menu && !menuWas;
	menuWas = menu;
	if (menuByHold())
	{
		if (!leftStart)
		{
			if (leftHoldSince != 0 && !leftHoldMenu)
				leftTapUntil = time + 150'000'000LL;
			leftHoldSince = 0;
			leftHoldMenu = false;
		}
		else if (leftHoldSince == 0)
			leftHoldSince = time;
		else if (!leftHoldMenu && time - leftHoldSince >= 800'000'000LL)
			leftHoldMenu = menuEdge = true;
		leftStart = time < leftTapUntil;
	}
	else
	{
		leftHoldSince = leftTapUntil = 0;
		leftHoldMenu = false;
	}
	// B (or Y; Vive wands, Windows Mixed Reality: a pad click): the game's reload, or not now for
	// the gun run, or the VR menu closed
	const bool skip = boolAction(skipAction);
	const bool skipEdge = skip && !skipWas;
	skipWas = skip;
	if (!skip)
		skipSwallow = false;
	if (preparing)
	{
		// B (or Y): not now, the arcade gun this time (the menu button does nothing now)
		if (skip)
			hands::skipPrep();
		if (menuEdge)
		{
			NOTICE_LOG(INPUT, "XR: menu button");
			NOTICE_LOG(INPUT, "XR: menu: not while the guns are being made");
		}
		for (Gun& g : guns)
		{
			// a trigger held through it doesn't fire when it's over: it waits to be let go
			g.triggerWas = true;
			g.triggerWait = true;
			g.pullAt = 0;
		}
		return;
	}

	// The VR menu (xr_menu.h): the left menu button (Index: the left pad squeezed hard; Vive wands,
	// Windows Mixed Reality: the left Start held) opens and closes it, B closes it too. Open, the
	// game is paused (applyPause) and gets nothing: the guns only follow the hands. (Also with the
	// controllers not the gun, vr.XrGun off.)
	if (menuEdge)
		NOTICE_LOG(INPUT, "XR: menu button");
	if (menuOpen)
	{
		updateMenu(time, menuEdge, skip, skipEdge);
		return;
	}
	if (menuEdge)
		menu::open(headPose());
	if (!config::VrXrGun)
		return;
	// the left thumbstick click: recenter
	const bool recenterButton = boolAction(recenterAction);
	if (recenterButton && !recenterWas)
	{
		recenter();
		NOTICE_LOG(INPUT, "XR: recenter (button)");
	}
	recenterWas = recenterButton;
	// ... held for 1.5 s: write the frame ring (the debug aid, when it's on), with a tick
	{
		static XrTime heldSince;
		static bool dumped;
		if (!recenterButton)
			heldSince = 0, dumped = false;
		else if (heldSince == 0)
			heldSince = time;
		else if (!dumped && time - heldSince > 1'500'000'000LL)
		{
			dumped = true;
			ringDumpRequested = true;
			haptic(Left, 0.08f, 0.8f);
		}
	}
	// the right thumbstick click: the laser line and dot off or on, kept in emu.cfg (the line
	// going or coming back says it; a tick in the hand too)
	const bool laser = boolAction(laserAction);
	if (laser && !laserWas)
	{
		config::VrLaser.set(!config::VrLaser);
		config::saveBool("config", "vr.Laser", config::VrLaser);
		haptic(Right, 0.03f, 0.4f);
		NOTICE_LOG(INPUT, "XR: laser %s", config::VrLaser ? "on" : "off");
	}
	laserWas = laser;
	const glm::vec2 stick = stickAction2();
	const u32 dpad = adjustWorldSize(time, stick) ? dpadFromStick(glm::vec2(0.f)) : dpadFromStick(stick);

	XrSpaceLocation locations[2];
	bool tracked[2];
	locateHands(time, locations, tracked);
	float pulls[2];
	for (int hand : { Left, Right })
		pulls[hand] = tracked[hand] ? floatAction(triggerAction, hand) : 0.f;
	// Single play always keeps A/right Start for P1 and X/left Start for P2 Join.
	// Once joined, Start and reload follow each player's actual dual hand. The left
	// Start's menu hold remains on the physical left controller.
	int hand1 = p1Hand(), hand2 = 1 - hand1;
	const bool rightStart = boolAction(startAction, Right) && !startSwallow;
	bool p1Start = hand1 == Right ? rightStart : leftStart;
	bool p2Start = hand1 == Right ? leftStart : rightStart;
	if (!leftStart)
		p2JoinHeld = false;
	if (p2JoinHeld && dualWield)
	{
		// The initiating X stays with P2 until release even after P1 moves left.
		if (hand1 == Left)
			p1Start = false;
		p2Start = true;
	}
	auto waitForRelease = [](Gun& g) {
		g.triggerWas = g.triggerWait = true;
		g.pullAt = g.reloadAt = g.heldAt = 0;
		g.shotsDue = g.autosDue = g.clicksDue = 0;
		g.offArmed = false;
	};
	// Joining takes both controllers and a game that identifies when P2 is out.
	// In netplay the remote side owns P2, so local dual play stays off.
	const bool canJoin = config::VrDualWield && !config::GGPOEnable && player2PromptKnown() && tracked[Left] && tracked[Right];
	if (canJoin && p2Start && !p2StartWas && !dualWield)
	{
		// Keep the current controller's reload latches when its player changes.
		const bool down[2] { guns[0].hand == Left && guns[0].pointedDown, guns[0].hand == Right && guns[0].pointedDown };
		const bool reloadHeld[2] { guns[0].hand == Left && guns[0].reloadButtonWas, guns[0].hand == Right && guns[0].reloadButtonWas };
		dualWield = true;
		dualSince = time;
		// Resolve the dual layout after changing mode, including this joining frame.
		hand1 = p1Hand();
		hand2 = 1 - hand1;
		guns[0].hand = hand1;
		guns[1] = Gun { 1, hand2 };
		for (Gun& g : guns)
		{
			g.pointedDown = down[g.hand];
			g.reloadButtonWas = reloadHeld[g.hand];
			waitForRelease(g);
		}
		// The single-mode joining press belongs to P2 even when its new hand is right.
		p2Start = true;
		p2JoinHeld = leftStart;
		slide.held = false;
		haptic(hand2, 0.05f, 0.6f);
		NOTICE_LOG(INPUT, "XR: player 2 joins: a gun in each hand");
		noteHolds("player 2 joins");
	}
	if (dualWield && (!config::VrDualWield || config::GGPOEnable || !player2PromptKnown()
			|| ((time - dualSince) > 4'000'000'000LL && player2Prompting())))
	{
		dualWield = false;
		lightgunSet(1, OffScreen, OffScreen, 0);
		NOTICE_LOG(INPUT, "XR: player 2 is out: one gun again");
		noteHolds("player 2 out");
	}
	// A live dual-layout change moves both players together. Downward and button
	// reload latches stay with their physical controllers; held triggers must release.
	if (dualWield && guns[0].hand != hand1)
	{
		std::swap(guns[0].pointedDown, guns[1].pointedDown);
		std::swap(guns[0].reloadButtonWas, guns[1].reloadButtonWas);
		for (Gun& g : guns)
		{
			g.hand = 1 - g.hand;
			waitForRelease(g);
		}
		NOTICE_LOG(INPUT, "XR: the guns swap hands: player 1's in the %s", hand1 == Right ? "right" : "left");
	}
	Gun& gun = guns[0];
	if (!dualWield)
	{
		// Restore the fixed single-player buttons immediately if P2 just left.
		hand1 = Right;
		hand2 = Left;
		p1Start = rightStart;
		p2Start = leftStart;
		// One gun: in the hand whose trigger is pulled while the gun hand's isn't,
		// or the other one when the gun hand's controller is gone (asleep, battery).
		const int otherHand = 1 - gun.hand;
		if (!gun.triggerWas && ((tracked[otherHand] && !tracked[gun.hand]) || (pulls[otherHand] > 0.6f && pulls[gun.hand] < 0.35f)))
		{
			gun.hand = otherHand;
			NOTICE_LOG(INPUT, "XR: the gun is in the %s hand", gun.hand == Right ? "right" : "left");
			noteHolds("gun moves");
		}
	}
	p2StartWas = p2Start;

	// Single: A is P1 Start, X joins P2, and B or Y reloads P1. Dual: each
	// hand's Start and reload belong to its assigned player. D-pad stays with P1.
	u32 p1 = dpad, p2 = 0;
	if (dualWield)
	{
		p1 |= (p1Start ? 4 : 0) | (!skipSwallow && boolAction(skipAction, hand1) ? 128 : 0);
		p2 |= (p2Start ? 4 : 0) | (!skipSwallow && boolAction(skipAction, hand2) ? 128 : 0);
	}
	else
		p1 |= (p1Start ? 4 : 0) | (skip && !skipSwallow ? 128 : 0);

	// The game's first Start of player 1's or trigger pull: player 1 takes the gun (in the pull's hand,
	// moved there above) and the controllers and labels go. (The menu's How to play is over by now: it
	// ends with the menu, whose pulls are its own.) Before the guns' update: the gun is drawn, and the
	// pull fires, in this very frame.
	const bool pulled = (!guns[0].triggerWas && triggerHeld(guns[0], pulls[guns[0].hand]))
			|| (dualWield && !guns[1].triggerWas && triggerHeld(guns[1], pulls[guns[1].hand]));
	if (pulled && !p1Armed)
	{
		p1Armed = true;
		noteHolds("shot");
	}
	else if (p1Start && !p1StartWas && !p1Armed)
	{
		// player 1's Start puts the gun in player 1's hand, though the single gun went to the other one
		// above (player 1's controller untracked then, the other tracked)
		if (!dualWield && tracked[hand1] && guns[0].hand != hand1)
		{
			guns[0].hand = hand1;
			NOTICE_LOG(INPUT, "XR: the gun is in the %s hand", hand1 == Right ? "right" : "left");
		}
		p1Armed = true;
		noteHolds("start");
	}
	p1StartWas = p1Start;

	updateGun(guns[0], time, locations, tracked, pulls, p1, !dualWield);
	if (dualWield)
		updateGun(guns[1], time, locations, tracked, pulls, p2, false);
}

// Left-eye image, every `step`th pixel, to a PPM file. Buffers are kept and the file goes
// out in one write: a capture runs on the render thread, frame after frame.
static bool writeEye(GLuint fbo, int width, int height, const std::string& path, int step)
{
	static std::vector<u8> pixels, rgb;
	pixels.resize((size_t)width * height * 4);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
	const int w = width / step, h = height / step;
	rgb.resize((size_t)w * h * 3);
	u8 *out = rgb.data();
	for (int row = 0; row < h; row++)
	{
		const int y = height - 1 - row * step;	// GL rows go up
		for (int col = 0; col < w; col++, out += 3)
			memcpy(out, &pixels[((size_t)y * width + (size_t)col * step) * 4], 3);
	}
	FILE *f = fopen(path.c_str(), "wb");
	if (f == nullptr)
		return false;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	fwrite(rgb.data(), 1, rgb.size(), f);
	fclose(f);
	return true;
}

// The shown frame's polygons on the 2D plane, for finding what the game draws right after
// a shot. The frame has been parsed: opaque and punch-through polygons, and translucent ones
// of passes that weren't auto-sorted, are ranges of the index buffer by now (strips merged
// into one, the others left empty); only auto-sorted translucent ones still point at vertices.
// Hidden vertices (NaN) were left out of the indices, so they can't be counted here.
static void writeOverlayList(const std::string& path, const glm::vec2& gun)
{
	const rend_context *ctx = gles_vr_frame();
	FILE *s = ctx != nullptr ? fopen(path.c_str(), "w") : nullptr;
	if (s == nullptr)
		return;
	const GameCamera cam = gameCamera(*ctx);
	fprintf(s, "gun %.0f %.0f  dc %.0fx%.0f  overlayW %.4f\n", gun.x, gun.y, cam.dcSize.x, cam.dcSize.y, cam.overlayW);
	// which translucent polygons still point at vertices
	std::vector<bool> trByVertex(ctx->global_param_tr.size(), false);
	RenderPass prev {};
	for (const RenderPass& pass : ctx->render_passes)
	{
		if (pass.sorted_tr_count != prev.sorted_tr_count)
			for (u32 n = prev.tr_count; n < pass.tr_count && n < trByVertex.size(); n++)
				trByVertex[n] = true;
		prev = pass;
	}
	std::vector<u32> vis;
	int lines = 0, mixedDumped = 0;
	for (const auto& [list, name] : { std::pair(&ctx->global_param_op, "OP"), std::pair(&ctx->global_param_pt, "PT"),
			std::pair(&ctx->global_param_tr, "TR") })
		for (size_t n = 0; n < list->size() && lines < 400; n++)
		{
			const PolyParam& pp = (*list)[n];
			vis.clear();
			if (list == &ctx->global_param_tr && trByVertex[n])
			{
				for (u32 i = pp.first; i < pp.first + pp.count && i < ctx->verts.size(); i++)
					vis.push_back(i);
			}
			else
			{
				for (u32 j = pp.first; j < pp.first + pp.count && j < ctx->idx.size(); j++)
				{
					const u32 vi = ctx->idx[j];
					if (vi != RestartIndex && vi < ctx->verts.size() && (vis.empty() || vis.back() != vi))
						vis.push_back(vi);
				}
			}
			if (vis.empty())
				continue;
			glm::vec2 lo(1e30f), hi(-1e30f);
			u32 overlay = 0, finite = 0;
			float wMin = 1e30f, wMax = 0.f;
			for (u32 vi : vis)
			{
				const Vertex& v = ctx->verts[vi];
				if (!std::isfinite(v.x) || !std::isfinite(v.y))
					continue;
				finite++;
				const float w = v.z > 0.f ? 1.f / v.z : 0.f;
				overlay += std::abs(w - cam.overlayW) < 0.002f;
				wMin = std::min(wMin, w);
				wMax = std::max(wMax, w);
				lo = glm::min(lo, glm::vec2(v.x, v.y));
				hi = glm::max(hi, glm::vec2(v.x, v.y));
			}
			const u8 *c = ctx->verts[vis[0]].col;
			const bool bigBright = lo.x <= hi.x && hi.x - lo.x >= cam.dcSize.x * 0.5f && hi.y - lo.y >= cam.dcSize.y * 0.5f
					&& c[0] >= 0xc0 && c[1] >= 0xc0;
			if (overlay == 0 && !bigBright)
				continue;
			fprintf(s, "%s %zu: %zu verts (%u on 2D) at %.0f,%.0f..%.0f,%.0f W %.3f..%.3f tex %06x tsp %08x isp %08x col %02x%02x%02x%02x\n",
					name, n, vis.size(), overlay, lo.x, lo.y, hi.x, hi.y, wMin, wMax,
					(unsigned)(pp.tcw.TexAddr << 3), pp.tsp.full, pp.isp.full, c[0], c[1], c[2], c[3]);
			lines++;
			// strips with both 2D-plane and 3D vertices: every vertex
			if (overlay > 0 && overlay < finite && mixedDumped < 12)
			{
				mixedDumped++;
				for (size_t k = 0; k < vis.size() && k < 64; k++)
				{
					const Vertex& v = ctx->verts[vis[k]];
					fprintf(s, "    v %zu: %.1f,%.1f W %.3f col %02x%02x%02x%02x uv %.3f,%.3f\n", k, v.x, v.y,
							v.z > 0.f ? 1.f / v.z : 0.f, v.col[0], v.col[1], v.col[2], v.col[3], v.u, v.v);
				}
			}
		}
	fclose(s);
}

// Debug aids (run-as):
//  "touch files/capture.request": the next left-eye image to files/eye0.ppm, plus fb0.ppm
//    (the console's video output in VRAM) and scene.txt (3D triangles);
//  "touch files/capture-shot.request": at the next trigger pull, 8 frames in a row to
//    files/shot1..8.ppm (half size) with the 2D polygons of each in shot1..8.txt.
// Where the debug aids look for their request files and write their output: on the Quest
// the app's internal files directory (adb's run-as can write there, the data directory on
// the shared storage it can't), on the PC the data directory.
static const std::string& debugDir()
{
	static std::string dir;
	if (dir.empty())
	{
#ifdef __ANDROID__
		dir = getFilesPath();
#else
		dir = get_writable_data_path("");
#endif
		if (!dir.empty() && (dir.back() == '/' || dir.back() == '\\'))
			dir.pop_back();
	}
	return dir;
}

// Debug aid: the last 1.5 s of left-eye frames at a quarter size, kept in memory, written
// out as ring/NNN.ppm (oldest first) with ring/index.txt (number, ms, 1 when a game frame
// was shown) when "ring.request" appears in the debug directory: a glitch of one frame can
// be found afterwards. On only while "ring.enable" is there at the first frame: it costs a
// blit and a readback every frame.
static void ringCapture(GLuint fbo, int width, int height, bool gameFrame)
{
	constexpr size_t Frames = 216;		// 3 s at 72 Hz
	static int enabled = -1;
	static GLuint ringFbo, ringTex;
	static int w, h;
	static std::vector<u8> frames;
	static std::vector<int64_t> stamps;
	static std::vector<u8> flags;
	static size_t count, next;
	const std::string& dir = debugDir();
	if (enabled < 0)
	{
		enabled = file_exists(dir + "/ring.enable") ? 1 : 0;
		if (!enabled)
			return;
		w = width / 4;
		h = height / 4;
		glGenTextures(1, &ringTex);
		glcache.BindTexture(GL_TEXTURE_2D, ringTex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glcache.BindTexture(GL_TEXTURE_2D, 0);
		glGenFramebuffers(1, &ringFbo);
		glBindFramebuffer(GL_FRAMEBUFFER, ringFbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ringTex, 0);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		{
			WARN_LOG(RENDERER, "XR: no framebuffer for the frame ring");
			enabled = 0;
			return;
		}
		frames.resize(Frames * (size_t)w * h * 4);
		stamps.resize(Frames);
		flags.resize(Frames);
		NOTICE_LOG(RENDERER, "XR: frame ring on: the last %zu frames at %dx%d, ring.request writes them", Frames, w, h);
	}
	if (!enabled)
		return;
	glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ringFbo);
	glBlitFramebuffer(0, 0, width, height, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, ringFbo);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, &frames[next * (size_t)w * h * 4]);
	stamps[next] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	flags[next] = gameFrame;
	next = (next + 1) % Frames;
	count = std::min(count + 1, Frames);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);

	// (polled every 16th frame: a file check a frame adds up; the left stick click held asks at once)
	static u32 polls;
	const std::string request = dir + "/ring.request";
	const bool asked = ringDumpRequested || (polls++ % 16 == 0 && file_exists(request));
	if (!asked)
		return;
	ringDumpRequested = false;
	nowide::remove(request.c_str());
	make_directory(dir + "/ring");
	FILE *idx = fopen((dir + "/ring/index.txt").c_str(), "w");
	std::vector<u8> rgb((size_t)w * h * 3);
	for (size_t k = 0; k < count; k++)
	{
		const size_t slot = (next + Frames - count + k) % Frames;
		const u8 *px = &frames[slot * (size_t)w * h * 4];
		u8 *out = rgb.data();
		for (int row = 0; row < h; row++)
			for (int col = 0; col < w; col++, out += 3)
				memcpy(out, &px[((size_t)(h - 1 - row) * w + col) * 4], 3);
		char name[64];
		snprintf(name, sizeof(name), "/ring/%03zu.ppm", k);
		if (FILE *f = fopen((dir + name).c_str(), "wb"))
		{
			fprintf(f, "P6\n%d %d\n255\n", w, h);
			fwrite(rgb.data(), 1, rgb.size(), f);
			fclose(f);
		}
		if (idx != nullptr)
			fprintf(idx, "%03zu %lld %d\n", k, (long long)stamps[slot], (int)flags[slot]);
	}
	if (idx != nullptr)
		fclose(idx);
	NOTICE_LOG(RENDERER, "XR: wrote the last %zu frames to %s/ring", count, dir.c_str());
}

void captureIfRequested(GLuint fbo, int width, int height)
{
	const std::string& dir = debugDir();
	static bool shotArmed;
	static u32 armedSerial;
	static int shotFrame = -1;
	const std::string shotRequest = dir + "/capture-shot.request";
	if (!shotArmed && shotFrame < 0 && file_exists(shotRequest))
	{
		nowide::remove(shotRequest.c_str());
		shotArmed = true;
		armedSerial = shotSerial;
		NOTICE_LOG(RENDERER, "XR: capturing at the next shot");
	}
	if (shotArmed && shotSerial != armedSerial)
	{
		shotArmed = false;
		shotFrame = 0;
	}
	if (shotFrame >= 0)
	{
		shotFrame++;
		const std::string name = dir + "/shot" + std::to_string(shotFrame);
		writeEye(fbo, width, height, name + ".ppm", 2);
		writeOverlayList(name + ".txt", lastShotScreen);
		if (shotFrame >= 8)
		{
			shotFrame = -1;
			NOTICE_LOG(RENDERER, "XR: captured 8 frames after the shot to %s/shot1..8", dir.c_str());
		}
	}
	const std::string request = dir + "/capture.request";
	if (!file_exists(request))
		return;
	nowide::remove(request.c_str());
	if (!writeEye(fbo, width, height, dir + "/eye0.ppm", 1))
		return;
	NOTICE_LOG(RENDERER, "XR: captured the left eye to %s/eye0.ppm", dir.c_str());

	// ...and the picture the console's video output reads from VRAM right now, which is
	// what a TV would show when the game draws with the CPU.
	FramebufferInfo info;
	info.update();
	PixelBuffer<u32> pb;
	int fbWidth = 0, fbHeight = 0;
	ReadFramebuffer<RGBAPacker>(info, pb, fbWidth, fbHeight);
	FILE *f = fbWidth > 0 && fbHeight > 0 ? fopen((dir + "/fb0.ppm").c_str(), "wb") : nullptr;
	if (f == nullptr)
		return;
	fprintf(f, "P6\n%d %d\n255\n", fbWidth, fbHeight);
	const u32 *px = pb.data();
	for (int i = 0; i < fbWidth * fbHeight; i++)
		fwrite(&px[i], 1, 3, f);
	fclose(f);
	NOTICE_LOG(RENDERER, "XR: video output %dx%d from %06x to %s/fb0.ppm", fbWidth, fbHeight, info.fb_r_sof1, dir.c_str());

	// ...and the shown frame's solid 3D triangles, rebuilt in game eye space (y down, game
	// units, integers in 1/1000), for measuring the scene (floor height, sizes) offline.
	const rend_context *ctx = gles_vr_frame();
	FILE *s = ctx != nullptr ? fopen((dir + "/scene.txt").c_str(), "w") : nullptr;
	if (s == nullptr)
		return;
	const GameCamera cam = gameCamera(*ctx);
	fprintf(s, "tan %d %d scale %d\n", (int)(cam.tanHalf.x * 1e6f), (int)(cam.tanHalf.y * 1e6f), (int)(worldScale() * 1e6f));
	auto vertex = [&](u32 i, glm::vec3& p) {
		if (i == RestartIndex || i >= ctx->verts.size())
			return false;
		const Vertex& v = ctx->verts[i];
		if (!(v.z > 0.f) || !std::isfinite(v.z))
			return false;
		const float w = 1.f / v.z;
		if (w < MinSceneW || std::abs(w - cam.overlayW) < 0.002f)
			return false;
		const glm::vec2 ndc(v.x * 2.f / cam.dcSize.x - 1.f, v.y * 2.f / cam.dcSize.y - 1.f);
		p = glm::vec3(ndc.x * w * cam.tanHalf.x, ndc.y * w * cam.tanHalf.y, -w);
		return true;
	};
	int count = 0;
	for (const std::vector<PolyParam> *list : { &ctx->global_param_op, &ctx->global_param_pt })
		for (size_t n = list == &ctx->global_param_op ? 1 : 0; n < list->size(); n++)
		{
			const PolyParam& pp = (*list)[n];
			for (u32 j = pp.first; j + 2 < pp.first + pp.count && j + 2 < ctx->idx.size(); j++)
			{
				glm::vec3 a, b, c;
				if (!vertex(ctx->idx[j], a) || !vertex(ctx->idx[j + 1], b) || !vertex(ctx->idx[j + 2], c))
					continue;
				fprintf(s, "%d %d %d %d %d %d %d %d %d\n",
						(int)(a.x * 1000), (int)(a.y * 1000), (int)(a.z * 1000),
						(int)(b.x * 1000), (int)(b.y * 1000), (int)(b.z * 1000),
						(int)(c.x * 1000), (int)(c.y * 1000), (int)(c.z * 1000));
				count++;
			}
		}
	fclose(s);
	NOTICE_LOG(RENDERER, "XR: %d scene triangles to %s/scene.txt", count, dir.c_str());
}

}	// namespace

bool enabled()
{
	static const bool on = config::VrXr;
	return on && !(initTried && session == XR_NULL_HANDLE);
}

const Eye *currentEye()
{
	return drawingEye ? &eye : nullptr;
}

const char *controllerProfile()
{
	return (profiles[Right].empty() ? profiles[Left] : profiles[Right]).c_str();
}

bool menuPaused()
{
	return menuOpen;
}

void requestPause(bool on)
{
	pauseRequest = on ? 1 : 0;
}

void applyPause()
{
#ifndef __ANDROID__
	// The menu's Quit on the PC: Flycast exits (on Android quitToStart goes to the activity instead)
	if (quitPending)
	{
		quitPending = false;
		pauseRequest = -1;
		menuOpen = false;
		NOTICE_LOG(RENDERER, "XR: quitting");
		dc_exit();
		return;
	}
#endif
	if (menuOpen && emu.running())
	{
		// started again from elsewhere (a new game): the menu holds nothing any more, and a
		// Quit chosen in it (Android: on the way to the start screen) is over
		menuOpen = false;
		menu::reset();
		NOTICE_LOG(RENDERER, "XR: menu closed (the emulator was started elsewhere)");
	}
	const int request = pauseRequest;
	pauseRequest = -1;
	if (request == 1 && !menuOpen && emu.running())
	{
		if (ggpo::active())
		{
			// Online (GGPO), a stop ends the session: Flycast's own menu doesn't pause there either
			// (gui_open_settings).
			NOTICE_LOG(RENDERER, "XR: no pause in an online game");
			return;
		}
		// As Flycast's own menu does it from this thread (gui_open_settings): the emulator's
		// thread is let go of any wait for the renderer (rend_cancel_emu_wait), then waited for.
		// Its sound stops with it.
		emu.stop();
		menuOpen = menuFresh = true;
		redrawLogged = false;
		NOTICE_LOG(RENDERER, "XR: menu open (paused)");
	}
	else if (request == 0 && menuOpen)
	{
		NOTICE_LOG(RENDERER, "XR: menu closed (resumed)");
		emu.start();
		// (after the start: if it throws, the menu still holds the stopped emulator)
		menuOpen = false;
	}
}

void recenter()
{
	recenterRequested = true;
}

void quitToStart()
{
#ifdef __ANDROID__
	// BaseGLActivity.quitToStart(activity): the start screen opens, the game's activity finishes
	if (g_jvm == nullptr || g_activity == nullptr)
	{
		WARN_LOG(RENDERER, "XR: no activity to quit");
		return;
	}
	JNIEnv *env = nullptr;
	bool attached = false;
	if (g_jvm->GetEnv((void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED)
	{
		if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK)
		{
			WARN_LOG(RENDERER, "XR: quit: no Java on this thread");
			return;
		}
		attached = true;
	}
	jclass activity = env->GetObjectClass(g_activity);
	jmethodID quit = env->GetStaticMethodID(activity, "quitToStart", "(Landroid/app/Activity;)V");
	if (quit != nullptr)
		env->CallStaticVoidMethod(activity, quit, g_activity);
	if (env->ExceptionCheck())
	{
		env->ExceptionDescribe();
		env->ExceptionClear();
		ERROR_LOG(RENDERER, "XR: quit to the start screen failed");
	}
	env->DeleteLocalRef(activity);
	if (attached)
		g_jvm->DetachCurrentThread();
#else
	// Flycast exits (dc_exit), at the top of the next Emulator::render: not in a headset frame
	quitPending = true;
#endif
}

void setHowToPlay(bool on, const char *why)
{
	if (on == menuHowToPlay)
		return;
	menuHowToPlay = on;
	if (on)
		NOTICE_LOG(INPUT, "XR: how to play on");
	else
		NOTICE_LOG(INPUT, "XR: how to play off (%s)", why);
	noteHolds(on ? "how to play" : why);
}

bool howToPlayOn()
{
	return menuHowToPlay;
}

Holds holds(int hand)
{
	if (menuHowToPlay || (hand != Left && hand != Right))
		return Holds::Controller;
	const bool gun = (dualWield && hand == guns[1].hand) || (p1Armed && hand == guns[0].hand);
	if (!gun)
		return Holds::Controller;
	// the game's cutscenes: the gun put away and nothing in its place, until play resumes
	return p1Armed && inCutscene ? Holds::Nothing : Holds::Gun;
}

int p1Hand()
{
	return dualWield && config::VrDualP1Left ? Left : Right;
}

bool dualWielding()
{
	return dualWield;
}

int hapticsLevel()
{
	std::string level = config::VrHaptics.get();
	std::transform(level.begin(), level.end(), level.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	if (level == "off" || level == "0")
		return 0;
	if (level == "light" || level == "low" || level == "1")
		return 1;
	if (level == "strong" || level == "3")
		return 3;
	return 2;	// medium, normal, 2, missing or anything else
}

void sampleKnock(int hand)
{
	haptic(hand, 0.03f, 1.f);
}

void term()
{
	if (instance != XR_NULL_HANDLE)
	{
		NOTICE_LOG(RENDERER, "XR: render context going away, closing the session");
		// still on the render thread with the context current: our own GL objects first
		termGun();
		termPanel();
		labels::term();
		controllers::term();
		controllersReady = controllerShown[Left] = controllerShown[Right] = false;
		modelRetries = 0;
		for (EyeChain& c : chains)
		{
			if (!c.fbos.empty())
				glDeleteFramebuffers((GLsizei)c.fbos.size(), c.fbos.data());
			if (c.depthStencil != 0)
				glDeleteRenderbuffers(1, &c.depthStencil);
		}
		if (session != XR_NULL_HANDLE && XR(xrDestroySession) != nullptr)
			XR(xrDestroySession)(session);	// with its spaces and swapchains
		if (XR(xrDestroyInstance) != nullptr)
			XR(xrDestroyInstance)(instance);	// with the actions
		// let go of the guns
		if (config::VrXrGun)
			for (const Gun& g : guns)
				lightgunSet(g.player, OffScreen, OffScreen, 0);
	}
	for (EyeChain& c : chains)
		c = EyeChain();
	instance = XR_NULL_HANDLE;
	systemId = XR_NULL_SYSTEM_ID;
	session = XR_NULL_HANDLE;
	localSpace = aimSpaces[Left] = aimSpaces[Right] = gripSpaces[Left] = gripSpaces[Right] = XR_NULL_HANDLE;
	actionSet = XR_NULL_HANDLE;
	aimAction = handAction = triggerAction = startAction = skipAction = menuAction = recenterAction = laserAction = hapticAction = stickAction = gripAction = XR_NULL_HANDLE;
	requestRefreshRate = nullptr;
	setPerformanceLevel = nullptr;
	sessionRunning = focused = drawingEye = viewsLogged = false;
	sessionLost = exiting = false;
	originSet = recenterRequested = false;
	guns[0] = Gun { 0 };
	guns[1] = Gun { 1, Left };
	dualWield = p2StartWas = p2JoinHeld = recenterWas = laserWas = menuWas = p1StartWas = false;
	skipWas = skipSwallow = false;
	// (what the hands hold stays, p1Armed and menuHowToPlay: player 2's gun went without a word)
	for (int hand : { Left, Right })
		heldWas[hand] = holds(hand);
	// (menuOpen stays: the emulator is still held stopped, and the next session shows the menu)
	profiles[Left].clear();
	profiles[Right].clear();
	leftHoldSince = leftTapUntil = 0;
	leftHoldMenu = false;
	initTried = false;
}

bool frame()
{
	if (!initTried && !init())
	{
		ERROR_LOG(RENDERER, "XR: initialisation failed, showing the flat window instead");
		// no headset: no panel to show the run for the agent's hands on either
		hands::skipPrep();
		return rend_single_frame(true);
	}
	if (session != XR_NULL_HANDLE)
		pollEvents();
	if (sessionLost || exiting)
	{
		// Lost: a new session next frame. Exiting (quit from the headset menu): none.
		const bool quit = exiting;
		NOTICE_LOG(RENDERER, "XR: session %s", quit ? "exiting" : "lost");
		term();
		initTried = quit;
		exiting = quit;
	}
	// Not shown (yet): keep the emulator's frames moving without spinning. Paused by the VR menu
	// there are none: what it queued before it stopped waits for it to go on again.
	auto idle = [] {
		if (menuOpen)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		else
			rend_vr_drain(10);
	};
	if (!sessionRunning)
	{
		idle();
		return false;
	}

	XrFrameWaitInfo waitInfo { XR_TYPE_FRAME_WAIT_INFO };
	XrFrameState frameState { XR_TYPE_FRAME_STATE };
	XrResult result = XR(xrWaitFrame)(session, &waitInfo, &frameState);
	if (XR_SUCCEEDED(result))
	{
		XrFrameBeginInfo beginInfo { XR_TYPE_FRAME_BEGIN_INFO };
		result = XR(xrBeginFrame)(session, &beginInfo);
	}
	if (!check(result, "xrWaitFrame/xrBeginFrame"))
	{
		sessionLost = result == XR_ERROR_SESSION_LOST || result == XR_ERROR_INSTANCE_LOST;
		idle();
		return false;
	}

	// Let through whatever the emulator produced since the last headset frame. While it runs
	// the game to its game over scene for the agent's hands (hands_rip.h), or its attract demo
	// for the hero's staff, it goes as fast as it can: it waits for each of its frames to be
	// taken, so keep taking them for most of the headset's frame.
	const bool preparing = hands::preparing();
	if (menuOpen)
	{
		// Paused by the VR menu: the last frame shown is drawn again, from where the head is now.
		if (!redrawLogged)
		{
			redrawLogged = true;
			NOTICE_LOG(RENDERER, "XR: paused, redrawing the last frame");
		}
	}
	else if (preparing)
	{
		const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(9);
		while (std::chrono::steady_clock::now() < until && hands::preparing())
			rend_vr_drain(1);
	}
	else
		rend_vr_drain(0);
	// What the panel says: while that runs, and for a moment when it's done.
	static bool wasPreparing;
	static std::chrono::steady_clock::time_point doneUntil;
	if (wasPreparing && !preparing && hands::prepProgress() >= 1.f)
		doneUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
	wasPreparing = preparing;
	Panel panel;
	const glm::vec3 red(0.86f, 0.13f, 0.1f), white(0.92f), grey(0.66f);
	const hands::Source *source = gameModelSource();
	const bool staff = source != nullptr && source->kind == hands::Source::HerosStaff;
	const bool cmGuns = source != nullptr && source->kind == hands::Source::CmGuns;
	const bool lupinGun = source != nullptr && source->kind == hands::Source::LupinGun;
	const bool gunModels = cmGuns || lupinGun;
	const char *title = lupinGun ? "LUPIN THE THIRD VR" : cmGuns ? "CONFIDENTIAL MISSION VR" : staff ? "THE MAZE OF THE KINGS VR" : "THE HOUSE OF THE DEAD 2 VR";
	// CM VR 1.2: Confidential Mission's panels in its start screen's colours (VR4All's: the panel, its
	// line, violet, cream, muted grey, lime)
	const glm::vec3 violet(169 / 255.f, 155 / 255.f, 1.f), cream(248 / 255.f, 245 / 255.f, 239 / 255.f), muted(155 / 255.f, 165 / 255.f, 180 / 255.f);
	if (gunModels)
	{
		panel.back = glm::vec3(21 / 255.f, 25 / 255.f, 35 / 255.f);
		panel.frame = panel.barTrack = glm::vec3(52 / 255.f, 57 / 255.f, 71 / 255.f);
		panel.barFill = glm::vec3(217 / 255.f, 243 / 255.f, 108 / 255.f);
		panel.footerColour = muted;
	}
	if (preparing && gunModels)
	{
		panel.lines = {
			{ title, 0.085f, violet },
			{ "", 0.03f, cream },
			{ lupinGun ? "Making your pistol from your game files (once)" : "Making your guns from your game files (once)", 0.064f, cream },
			{ lupinGun ? "Watching the attract demo, only this once" : "About a minute or two, only this once", 0.047f, muted },
		};
		panel.progress = hands::prepProgress();
		panel.footer = "B: skip (the Arcade gun this time)";
	}
	else if (std::chrono::steady_clock::now() < doneUntil && gunModels)
		panel.lines = {
			{ title, 0.085f, violet },
			{ "", 0.03f, cream },
			{ "Got it. The gun from your game is in your hand.", 0.064f, cream },
			// (as the menu's Reload has it: vr.Reload 1, pointing away is enough)
			{ config::VrReload == 1 ? "Reload: B, or point it away from the screen." : "Reload: B, or point it away from the screen and pull.",
					0.047f, muted },
		};
	else if (preparing)
	{
		panel.lines = {
			{ title, 0.085f, red },
			{ "", 0.03f, white },
			{ staff ? "Making the hero's staff from your game" : "Taking the agent's hands and pistol from your game", 0.064f, white },
			{ staff ? "Only this once: the game plays its attract demo by itself, fast."
					: "Only this once: the game plays itself to its game over, fast.", 0.047f, grey },
		};
		panel.progress = hands::prepProgress();
		panel.footer = "B: skip it for now (the arcade gun this time)";
	}
	else if (std::chrono::steady_clock::now() < doneUntil)
		panel.lines = {
			{ title, 0.085f, red },
			{ "", 0.03f, white },
			{ staff ? "Got it. The hero's staff is yours." : "Got them. The agent's hands are yours.", 0.064f, white },
			{ staff ? "Reload: point it away from the screen and pull the trigger."
					: "Reload: grab the back of the pistol with your other hand and pull.", 0.047f, grey },
		};
	const bool showPanel = !panel.lines.empty();
	// What the panel says, in the log each time it changes: the lines under the title, and the footer
	static std::string panelSaid;
	std::string said;
	for (size_t i = 1; i < panel.lines.size(); i++)
		if (!panel.lines[i].text.empty())
			said += (said.empty() ? "" : " / ") + panel.lines[i].text;
	if (!panel.footer.empty())
		said += " | " + panel.footer;
	if (showPanel && said != panelSaid)
		NOTICE_LOG(RENDERER, "XR: first start: %s", said.c_str());
	panelSaid = showPanel ? said : std::string();
	// The panel alone in a dark room, or the VR menu over the paused game. Paused with no game
	// frame kept, the menu is in the dark room: the GL context went away (Android: the app's
	// surface, Quest home) and the renderer's Term let go of the frame; it comes back with the game.
	const bool darkRoom = menuOpen ? !gles_vr_have_frame() : showPanel;

	XrViewState viewState { XR_TYPE_VIEW_STATE };
	XrViewLocateInfo locateInfo { XR_TYPE_VIEW_LOCATE_INFO };
	locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locateInfo.displayTime = frameState.predictedDisplayTime;
	locateInfo.space = localSpace;
	u32 viewCount = 0;
	// Drawn whenever the head's orientation is known. Without its position (a headset with no
	// space set up, or tracking lost for a while) the eyes stay where they last were, or at
	// eye distance around the middle: before, nothing was drawn at all, and players heard the
	// game in a black headset.
	const bool located = XR_SUCCEEDED(XR(xrLocateViews)(session, &locateInfo, &viewState, 2, &viewCount, views))
			&& viewCount == 2 && (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
	if (located)
	{
		static glm::vec3 lastEye[2];
		static bool haveLastEye;
		static bool warned;
		if ((viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0)
		{
			lastEye[0] = toVec(views[0].pose.position);
			lastEye[1] = toVec(views[1].pose.position);
			haveLastEye = true;
		}
		else
		{
			if (!warned)
			{
				warned = true;
				WARN_LOG(RENDERER, "XR: the head's position isn't tracked: drawing from %s (orientation only)",
						haveLastEye ? "where it last was" : "the middle");
			}
			const glm::vec3 right = toQuat(views[0].pose.orientation) * glm::vec3(1.f, 0.f, 0.f);
			const glm::vec3 center = haveLastEye ? (lastEye[0] + lastEye[1]) * 0.5f : glm::vec3(0.f);
			const float halfIpd = haveLastEye ? glm::distance(lastEye[0], lastEye[1]) * 0.5f : 0.032f;
			const glm::vec3 eyes[2] { center - right * halfIpd, center + right * halfIpd };
			for (int i = 0; i < 2; i++)
				views[i].pose.position = { eyes[i].x, eyes[i].y, eyes[i].z };
		}
	}
	updateOrigin(located);
	// once a session, what the view is made of (to compare headsets and runtimes when the
	// world looks the wrong size)
	if (located && !viewsLogged)
	{
		viewsLogged = true;
		const float deg = 180.f / 3.14159265f;
		NOTICE_LOG(RENDERER, "XR: eyes %.1f mm apart; fov left %.1f %.1f %.1f %.1f, right %.1f %.1f %.1f %.1f degrees; %dx%d per eye; "
				"world %.4f m per game unit, depth scale %.2f",
				glm::distance(toVec(views[0].pose.position), toVec(views[1].pose.position)) * 1000.f,
				views[0].fov.angleLeft * deg, views[0].fov.angleRight * deg, views[0].fov.angleUp * deg, views[0].fov.angleDown * deg,
				views[1].fov.angleLeft * deg, views[1].fov.angleRight * deg, views[1].fov.angleUp * deg, views[1].fov.angleDown * deg,
				chains[0].width, chains[0].height, worldScale(), (float)config::ExtraDepthScale);
	}
	updateLightgun(frameState.predictedDisplayTime);
	retryControllerModels();
	updateControllers(frameState.predictedDisplayTime);
	noteDrawn();

	XrCompositionLayerProjectionView projViews[2] { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
	const bool render = frameState.shouldRender && located;
	bool ok = true;
	for (int i = 0; render && i < 2; i++)
	{
		EyeChain& c = chains[i];
		u32 index = 0;
		XrSwapchainImageAcquireInfo acquire { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		XrSwapchainImageWaitInfo wait { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		wait.timeout = XR_INFINITE_DURATION;
		if (!XRCHECK(XR(xrAcquireSwapchainImage)(c.chain, &acquire, &index)) || !XRCHECK(XR(xrWaitSwapchainImage)(c.chain, &wait)))
		{
			ok = false;
			break;
		}
		const glm::mat4 view = glm::inverse(glm::translate(glm::mat4(1.f), relativePosition(views[i].pose.position))
				* glm::mat4_cast(relativeRotation(views[i].pose.orientation)));
		eye = { c.fbos[index], c.width, c.height, eyeProjection(views[i].fov) * view * gameToWorld() };
		if (darkRoom)
		{
			// a dark room with the panel in it
			glBindFramebuffer(GL_FRAMEBUFFER, c.fbos[index]);
			glViewport(0, 0, c.width, c.height);
			glcache.Disable(GL_SCISSOR_TEST);
			glcache.ClearColor(0.02f, 0.02f, 0.025f, 1.f);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
			if (menuOpen)
				menu::draw(eyeProjection(views[i].fov) * view);
			else
				drawPanel(eyeProjection(views[i].fov) * view, panel);
		}
		else if (gles_vr_have_frame())
		{
			drawingEye = true;
			gles_vr_draw_eye(c.width, c.height);
			drawingEye = false;
		}
		else
		{
			// nothing to show yet: black, not whatever the image held before
			glBindFramebuffer(GL_FRAMEBUFFER, c.fbos[index]);
			glViewport(0, 0, c.width, c.height);
			glcache.Disable(GL_SCISSOR_TEST);
			glcache.ClearColor(0.f, 0.f, 0.f, 1.f);
			glClear(GL_COLOR_BUFFER_BIT);
			// (a black frame between game frames would be a flicker: counted, every 10 s)
			static u32 blackFrames;
			static std::chrono::steady_clock::time_point since;
			if (i == 0)
			{
				blackFrames++;
				const auto now = std::chrono::steady_clock::now();
				if (now - since > std::chrono::seconds(10))
				{
					if (since != std::chrono::steady_clock::time_point())
						NOTICE_LOG(RENDERER, "XR: %u black frames (no game frame to show) in the last 10 s", blackFrames);
					since = now;
					blackFrames = 0;
				}
			}
		}
		if (menuOpen && !darkRoom)
		{
			// the VR menu over the paused game, the guns over it
			glBindFramebuffer(GL_FRAMEBUFFER, c.fbos[index]);
			glViewport(0, 0, c.width, c.height);
			glcache.Disable(GL_SCISSOR_TEST);
			menu::draw(eyeProjection(views[i].fov) * view);
		}
		if ((!darkRoom || menuOpen) && (guns[0].visible || guns[1].visible || pointerShown[Left] || pointerShown[Right]
				|| controllerShown[Left] || controllerShown[Right]))
		{
			// every gun's model and the controllers in the other hands, then every gun's glow (dots
			// and flashes over all models), and the menu's pointers from hands with no gun
			glBindFramebuffer(GL_FRAMEBUFFER, c.fbos[index]);
			glViewport(0, 0, c.width, c.height);
			const glm::mat4 viewProj = eyeProjection(views[i].fov) * view;
			const glm::vec3 eyePos = relativePosition(views[i].pose.position);
			bool first = true;
			for (const Gun& g : guns)
				if (g.visible)
				{
					drawGunModel(viewProj, eyePos, g.view, first, glm::vec3(1.f));
					first = false;
				}
			if (first)
			{
				// no gun: the controllers and the pointers' lines against nothing but each other,
				// not the game's depth
				glcache.Disable(GL_SCISSOR_TEST);
				glcache.DepthMask(GL_TRUE);
				glClearDepthf(1.f);
				glClear(GL_DEPTH_BUFFER_BIT);
			}
			for (int hand : { Left, Right })
				if (controllerShown[hand])
					controllers::draw(viewProj, eyePos, hand, controllerGrip[hand], controllerAim[hand]);
			first = true;
			for (const Gun& g : guns)
				if (g.visible)
				{
					drawGunGlow(viewProj, eyePos, g.view, first);
					first = false;
				}
			for (int hand : { Left, Right })
				if (pointerShown[hand])
				{
					drawGunGlow(viewProj, eyePos, pointers[hand], first);
					first = false;
				}
			// the how-to-play labels by the buttons of the controllers held, over everything
			if (config::VrXrGun && labelsOn())
				for (int hand : { Left, Right })
					if (controllerShown[hand] && holds(hand) == Holds::Controller)
						labels::draw(viewProj, hand, controllerGrip[hand], controllerAim[hand], profiles[hand].c_str());
		}
		if (i == 0)
		{
			captureIfRequested(c.fbos[index], c.width, c.height);
			ringCapture(c.fbos[index], c.width, c.height, !showPanel && !menuOpen && gles_vr_have_frame());
		}
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		XrSwapchainImageReleaseInfo release { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		if (!XRCHECK(XR(xrReleaseSwapchainImage)(c.chain, &release)))
		{
			ok = false;
			break;
		}
		projViews[i].pose = views[i].pose;
		projViews[i].fov = views[i].fov;
		projViews[i].subImage.swapchain = c.chain;
		projViews[i].subImage.imageRect.extent = { c.width, c.height };
	}

	XrCompositionLayerProjection layer { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	layer.space = localSpace;
	layer.viewCount = 2;
	layer.views = projViews;
	const XrCompositionLayerBaseHeader *layers[] { (const XrCompositionLayerBaseHeader *)&layer };
	XrFrameEndInfo endInfo { XR_TYPE_FRAME_END_INFO };
	endInfo.displayTime = frameState.predictedDisplayTime;
	endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	endInfo.layerCount = render && ok ? 1 : 0;
	endInfo.layers = layers;
	XRCHECK(XR(xrEndFrame)(session, &endInfo));
	return render && ok;
}

}	// namespace vr::xr
#endif
