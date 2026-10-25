/*
	The agent's hands and pistol, the hero's staff, or Confidential Mission's guns, from the
	player's own game (hotd2-vr). See hands_rip.h.

	Copyright 2026 mikermak. This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#include "hands_rip.h"
#include "hands_build.h"
#include "xr_host.h"
#include "xr_sim.h"
#include "rend/vr_reproject.h"
#include "rend/transform_matrix.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/sh4/sh4_sched.h"
#include "input/udp_lightgun.h"
#include "cfg/option.h"
#include "emulator.h"
#include "stdclass.h"
#include "log/Log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <mutex>

namespace vr::hands
{
namespace
{

// the pistol in the game over scene: this many of its polygons and it's all there
constexpr u32 MinGunPolys = 20;

// The run to HOTD2's game over scene. Start gets it through the notices at boot ("PRESS START
// BUTTON", with saved data or without, which then makes it), the intro, the title and its
// menu (Arcade mode comes first). It is only pressed while one of those is on: a screen
// drawn every frame with little 3D in it (the title draws ~450 polygons a frame, the menu
// 86, the notices none), for the story and the game draw 900 and more, and Start would
// pause them. From the story on, nothing: the agent loses, and kneels with his pistol at
// about 115 s of game (measured on the PC, both with saved data and without).
constexpr u32 Story3D = 600;		// 3D polygons a frame
constexpr double PressEvery = 2.0, PressHold = 0.2;
constexpr int Window = 30;			// vblanks looked back over (half a second)
constexpr double TypicalRun = 115.0;
constexpr double GiveUpAfter = 330.0;

// The run for the hero's staff (The Maze of the Kings): its attract demo, from boot, with no
// input at all. Its logos end at about 31 s of game; from 32 s both heroes walk towards the
// camera, staff in hand (the first frame of that is enough), at 53 s they raise them, and the
// demo comes round again every two and a half to three minutes (measured on the PC).
constexpr double StaffRun = 33.0;
constexpr double StaffGiveUpAfter = 150.0;

// The run for Confidential Mission's guns: Start once at 30 s of game (findings startOk), its
// SEGA logo's; the reload tutorial comes a second later (measured on the PC).
// Task 12: before that, while a gun from the attract demo (GunTarget::attractBy, the assault rifle)
// has no file, no presses from boot: the demo drops the rifle at about 316 s of game (measured on
// the PC). Once each such gun is made, or its attractBy has come, Start leaves the demo (CmStartAfter
// later) for the same reload tutorial, and everything below counts from that Start as from 30 s.
// Start can't come first: the demo doesn't come back in play.
constexpr double CmStartAt = 30.0;
constexpr double CmRun = 76.0;		// (done at 74 s of game, CM VR 1.2 Task 2: the progress bar)
constexpr double CmRifleRun = 316.0;	// the attract demo's part, typically (the progress bar)
constexpr double CmStartAfter = 0.5;
// Then the merged guns (GunTarget::merge, the agents' pistol): from CmPistolAfter after that Start
// (the next 2 s beat after where 1.1 made the tutorial's cabinet gun, 0.1 s after Start, measured
// on the PC; 1.2 has no cabinet gun), Start every 2 s for CmPressFor, 32 to 46 s of game, through
// the gun adjustment screen and the briefing to the stage 1 intro (about 50-66 s of game), where
// the agents hold it. Those are 1.1's presses after its cabinet gun (its first, at 30.1 s, fell
// inside Start's own hold); one more, at 48 s, skips the intro (measured: the pistol never came up).
// Every pass with minPolys of it or more is captured, and 10 s of game after the last one (or at
// 150 s) they're merged (buildGun). Never while watching in play. (1.1 waited 20 s; the last pass
// comes at 64 s either way, and 10 s gave the same 403 captures and the same file: CM VR 1.2
// Task 2, confmiss-findings.md Guns.)
constexpr double CmPistolAfter = 2.0;
constexpr double CmPressFor = 14.0;
constexpr double CmQuietFor = 10.0;
constexpr double CmLastAt = 150.0;
constexpr size_t CmMaxCaptures = 1200;	// 20 s of passes (about 15 kB each)

enum State { Off, Watching, Preparing };
std::atomic<int> state { Off };
std::atomic<bool> newModel, skipRequested;
std::atomic<float> progress;
std::atomic<bool> staffRun;	// the run is for the hero's staff
std::atomic<bool> cmRun;	// ...for Confidential Mission's guns
std::atomic<bool> attractRun;	// ...for a neutral attract-sequence gun
// Confidential Mission's guns (Source::guns): made (or not wanted), and the pass a try at one
// that isn't merged failed (gunMutex)
std::atomic<bool> gunDone[3];
std::mutex gunMutex;
u32 gunFailedAt[3];
// the merged guns' captures, and the textures they use, decoded when captured (VRAM moves on)
std::vector<Rip> gunCaptures[3];
struct CachedTexture { u32 tcw, size; bool ok; Image image; };
std::vector<CachedTexture> textureCache;
std::atomic<bool> gunSeen[3];	// a pass with minPolys of it since the last vblank
std::atomic<bool> gunOver[3];	// given up (or merged) in this run
double gunLastSeen[3];			// game seconds (run time)
std::atomic<bool> cmCapturing;	// the merged guns' part of the run is on
double cmMergedAt;				// ...since then (run time as if Start came at CmStartAt; < 0 not yet)
double cmStartAt;				// the run's Start (run time; < 0: the attract demo's part is on)
bool cmSaved;
bool cmStartLogged;
double startedAt;			// game seconds at the start
bool fastForwardWas;
// why the last try at the staff didn't make it (for when the run gives up)
std::mutex failureMutex;
std::string lastFailure;
// what the passes showed since the last vblank
std::atomic<u32> seen3D, seenPasses;
// ...and over the last vblanks
u32 recent3D[Window], recentPasses[Window];
int recentAt;
bool storyBegun;
double lastPress;
u32 passes, failedAt;

constexpr int OffScreen = -10000;

double gameSeconds() {
	return sh4_sched_now64() / (double)SH4_MAIN_CLOCK;
}

std::string gunPath(const GunTarget& gun) {
	return get_writable_data_path(gun.file);
}

// Attract-gun games retry incomplete/corrupt final caches; other policies stay unchanged.
bool gunCached(const Source& source, const GunTarget& gun)
{
	const std::string path = gunPath(gun);
	return source.attractGun() ? loadGunCache(path, gun) : file_exists(path);
}

bool haveModel()
{
	const Source *source = gameModelSource();
	if (source != nullptr && source->gunModels())
	{
		// every gun's file (the agents' pistol's) but those from the attract demo (the assault
		// rifle: its file missing alone starts no run, PF20; it's watched for in play). An old
		// gun-cm-cabinet.bin (1.1) is no gun's: left alone, never read.
		for (const GunTarget& gun : source->guns)
			if (gun.used() && gun.attractBy <= 0.f && !gunCached(*source, gun))
				return false;
		return true;
	}
	return file_exists(modelPath()) || (!legacyModelPath().empty() && file_exists(legacyModelPath()));
}

// Confidential Mission: the guns from the attract demo still to make (gunMutex, or before the run).
bool attractGunsLeft(const Source& source)
{
	for (int i = 0; i < 3; i++)
		if (source.guns[i].attractBy > 0.f && !gunDone[i])
			return true;
	return false;
}

// ...and that they're watched for in play.
void logWatched(const Source& source)
{
	for (int i = 0; i < 3; i++)
		if (source.guns[i].attractBy > 0.f && !gunDone[i])
			NOTICE_LOG(RENDERER, "VR hands: watching for the %s in play", source.guns[i].name);
}

bool saveGun(const GunTarget& gun, int i, const std::vector<u8>& model, u32 polys, bool complete,
		const std::string *verdict = nullptr);
void finishMerged(const GunTarget& gun, int i, std::vector<Rip> captures, std::vector<CachedTexture> cache);

// The run over: back to the game as it was, still watching for the scene in play (next), or not.
void stopPrep(const char *why, State next = Watching)
{
	if (state != Preparing)
		return;
	state = next;
	settings.input.fastForwardMode = fastForwardWas;
	lightgunSet(0, OffScreen, OffScreen, 0);
	if (cmRun)
	{
		// the merged guns' captures and their textures (up to ~20 MB) go: nothing is captured
		// outside the run (gunMutex: not held by the callers)
		std::lock_guard<std::mutex> lock(gunMutex);
		cmCapturing = false;
		for (std::vector<Rip>& captures : gunCaptures)
			std::vector<Rip>().swap(captures);
		std::vector<CachedTexture>().swap(textureCache);
	}
	NOTICE_LOG(RENDERER, "VR hands: %s after %.0f s of game", why, gameSeconds() - startedAt);
}

void onStart()
{
	state = Off;
	skipRequested = false;
	progress = 0.f;
	// a new game: whatever model the renderer has is another game's
	newModel = true;
	const Source *source = gameModelSource();
	if (source == nullptr || !config::VrGameHands)
		return;
	cmRun = source->kind == Source::CmGuns;
	attractRun = source->attractGun();
	const bool watchOnly = haveModel();
	if (watchOnly)
	{
		NOTICE_LOG(RENDERER, "VR hands: %s, made before: %s", source->name(),
				(file_exists(modelPath()) ? modelPath() : legacyModelPath()).c_str());
		// but a gun from the attract demo without its file: watched for in play, no run (PF20)
		bool left = false;
		for (const GunTarget& gun : source->guns)
			left = left || (cmRun && gun.used() && gun.attractBy > 0.f && !file_exists(gunPath(gun)));
		if (!left)
			return;
	}
	state = Watching;
	passes = failedAt = 0;
	staffRun = source->kind == Source::HerosStaff;
	{
		std::lock_guard<std::mutex> lock(gunMutex);
		for (int i = 0; i < 3; i++)
		{
			const GunTarget& gun = source->guns[i];
			gunDone[i] = !(cmRun || attractRun) || !gun.used() || gunCached(*source, gun);
			gunFailedAt[i] = 0;
			std::vector<Rip>().swap(gunCaptures[i]);
			gunSeen[i] = false;
			gunOver[i] = false;
			gunLastSeen[i] = -1.0;
		}
		textureCache.clear();
		cmCapturing = false;
		cmMergedAt = -1.0;
		cmStartAt = cmRun && attractGunsLeft(*source) ? -1.0 : CmStartAt;
		cmSaved = false;
	}
	cmStartLogged = false;
	{
		std::lock_guard<std::mutex> lock(failureMutex);
		lastFailure.clear();
	}
	if (watchOnly)
	{
		logWatched(*source);
		return;
	}
	// The run: in the headset, or on the PC when asked (vr.HandsPrep, for testing; the OpenXR
	// simulator counts as the PC). Never online: it presses Start for this side's player in the
	// shared game (watching for the scene in play still makes the model when it comes by).
	if ((!xr::enabled() || xr::sim::active()) && !config::VrHandsPrep)
		return;
	if (config::GGPOEnable)
	{
		NOTICE_LOG(RENDERER, "VR hands: online, no run for the model (watching for it in play)");
		return;
	}
	std::fill(std::begin(recent3D), std::end(recent3D), 0u);
	std::fill(std::begin(recentPasses), std::end(recentPasses), 0u);
	recentAt = 0;
	seen3D = seenPasses = 0;
	storyBegun = false;
	lastPress = -1e9;
	startedAt = gameSeconds();
	fastForwardWas = settings.input.fastForwardMode;
	settings.input.fastForwardMode = true;
	state = Preparing;
	if (staffRun)
		NOTICE_LOG(RENDERER, "VR hands: no model yet, running the attract demo for the hero's staff (%s)", modelPath().c_str());
	else if (cmRun)
		NOTICE_LOG(RENDERER, "VR hands: no model yet, taking the guns from the game");
	else if (attractRun)
		NOTICE_LOG(RENDERER, "VR hands: no model yet, watching the attract demo for %s", source->name());
	else
		NOTICE_LOG(RENDERER, "VR hands: no model yet, running the game to its game over scene for it");
}

void onVBlank()
{
	if (state != Preparing)
		return;
	if (skipRequested)
	{
		// Confidential Mission (S23): skipped is skipped for the session, no gun made by watching
		// play either (the reload tutorial comes right after Start), so the arcade gun stays and
		// the next start asks again. But the guns from the attract demo (the assault rifle), which
		// come in play too, are still watched for with vr.GameHands on (Task 12): the others are
		// marked done, and Watching.
		const Source *source = gameModelSource();
		bool watch = false;
		if (cmRun && config::VrGameHands && source != nullptr && source->kind == Source::CmGuns)
		{
			std::lock_guard<std::mutex> lock(gunMutex);
			watch = attractGunsLeft(*source);
			for (int i = 0; i < 3; i++)
				if (source->guns[i].attractBy <= 0.f)
					gunDone[i] = true;
		}
		stopPrep("skipped", attractRun || (cmRun && !watch) ? Off : Watching);
		if (watch)
			logWatched(*source);
		return;
	}
	const double now = gameSeconds() - startedAt;
	if (attractRun)
	{
		const Source *source = gameModelSource();
		if (source == nullptr || !source->attractGun())
		{
			stopPrep("stopped", Off);
			return;
		}
		const double limit = std::isfinite(source->prepSeconds) && source->prepSeconds > 0.f
				? source->prepSeconds : 180.0;
		progress = (float)std::min(now / limit, 0.97);
		// No guessed Start timing: recognize the measured texture and complete geometry in
		// whatever attract scene supplies them. The timeout is a bound, not a scene identity.
		lightgunSet(0, OffScreen, OffScreen, 0);
		if (now >= limit)
			stopPrep("no complete in-game gun appeared (gave up)", Watching);
		return;
	}
	if (cmRun)
	{
		const Source *source = gameModelSource();
		if (source == nullptr || source->kind != Source::CmGuns)
		{
			stopPrep("stopped");
			return;
		}
		auto advance = [](double to) {
			progress = std::max(progress.load(), (float)std::min(to, 0.97));
		};
		if (cmStartAt < 0.0)
		{
			// the attract demo's part: hands off until each of its guns is made or its time has come
			bool waiting = false;
			{
				std::lock_guard<std::mutex> lock(gunMutex);
				for (int i = 0; i < 3; i++)
				{
					const GunTarget& gun = source->guns[i];
					if (gun.attractBy <= 0.f || gunDone[i] || gunOver[i])
						continue;
					if (now < gun.attractBy)
						waiting = true;
					else
					{
						gunOver[i] = true;
						NOTICE_LOG(RENDERER, "VR hands: no %s in the attract demo by %.0f s of game", gun.name, now);
					}
				}
			}
			advance(now / (CmRifleRun + CmStartAfter + CmRun - CmStartAt));
			if (waiting)
			{
				lightgunSet(0, OffScreen, OffScreen, 0);
				return;
			}
			cmStartAt = now + CmStartAfter;
		}
		// the rest of the run's time as if Start came at CmStartAt
		const double t = now - cmStartAt + CmStartAt;
		advance(now / (cmStartAt + CmRun - CmStartAt));
		bool start = t >= CmStartAt && t < CmStartAt + PressHold;
		if (start && !cmStartLogged)
		{
			cmStartLogged = true;
			NOTICE_LOG(RENDERER, "VR hands: Start at %.1f s", now);
		}
		// merged guns taken out (under gunMutex) to be built without it: the render thread takes it
		// every pass, and a merge can take a second on the headset
		struct Merge { int i; std::vector<Rip> captures; std::vector<CachedTexture> textures; };
		std::vector<Merge> merges;
		{
			std::lock_guard<std::mutex> lock(gunMutex);
			// the merged ones CmPistolAfter after Start, the others made or given up by then (the
			// assault rifle, the only other: made or given up in the attract demo's part)
			bool othersDone = true, merging = false;
			for (int i = 0; i < 3; i++)
			{
				if (gunSeen[i].exchange(false))
					gunLastSeen[i] = t;
				if (!gunDone[i] && !gunOver[i])
				{
					if (source->guns[i].merge)
						merging = true;
					else
						othersDone = false;
				}
			}
			if (cmMergedAt < 0.0 && merging && othersDone && t >= CmStartAt + CmPistolAfter)
			{
				cmMergedAt = t;
				cmCapturing = true;
				NOTICE_LOG(RENDERER, "VR hands: on to the merged guns at %.1f s", now);
			}
			if (cmMergedAt >= 0.0)
			{
				const double since = t - cmMergedAt;
				start = start || (since < CmPressFor + PressHold && std::fmod(since, PressEvery) < PressHold);
				for (int i = 0; i < 3; i++)
					if (source->guns[i].merge && !gunDone[i] && !gunOver[i]
							&& ((gunLastSeen[i] >= 0.0 && t - gunLastSeen[i] >= CmQuietFor) || t >= CmLastAt))
					{
						gunOver[i] = true;
						Merge& m = merges.emplace_back();
						m.i = i;
						m.captures.swap(gunCaptures[i]);
					}
				// the textures go with them (a copy while another merged gun is still capturing)
				bool still = false;
				for (int i = 0; i < 3; i++)
					still = still || (source->guns[i].merge && !gunDone[i] && !gunOver[i]);
				for (size_t k = 0; k < merges.size(); k++)
					if (k + 1 == merges.size() && !still)
						merges[k].textures.swap(textureCache);
					else
						merges[k].textures = textureCache;
			}
		}
		for (Merge& m : merges)
			finishMerged(source->guns[m.i], m.i, std::move(m.captures), std::move(m.textures));
		// over when each gun is made or given up
		bool over = true, all = true, saved;
		{
			std::lock_guard<std::mutex> lock(gunMutex);
			for (int i = 0; i < 3; i++)
			{
				over = over && (gunDone[i] || gunOver[i]);
				all = all && gunDone[i];
			}
			saved = cmSaved;
		}
		if (over)
		{
			if (saved)
				progress = 1.f;
			stopPrep(saved ? "done" : "no gun came up whole (gave up)", all ? Off : Watching);
			return;
		}
		lightgunSet(0, OffScreen, OffScreen, start ? 4 : 0);
		return;
	}
	const double t = now;
	if (staffRun)
	{
		// Hands off: the attract demo plays by itself.
		progress = (float)std::min(t / StaffRun, 0.97);
		if (t > StaffGiveUpAfter)
		{
			std::string why;
			{
				std::lock_guard<std::mutex> lock(failureMutex);
				why = lastFailure.empty() ? "the hero's staff never came up whole (gave up)"
						: "no whole staff (gave up; last time " + lastFailure + ")";
			}
			stopPrep(why.c_str());
			return;
		}
		lightgunSet(0, OffScreen, OffScreen, 0);
		return;
	}
	progress = (float)std::min(t / TypicalRun, 0.97);
	if (t > GiveUpAfter)
	{
		stopPrep("no game over scene (gave up)");
		return;
	}
	recent3D[recentAt] = seen3D.exchange(0);
	recentPasses[recentAt] = seenPasses.exchange(0);
	recentAt = (recentAt + 1) % Window;
	u32 polys = 0, drawn = 0;
	for (int i = 0; i < Window; i++)
	{
		polys += recent3D[i];
		drawn += recentPasses[i];
	}
	const bool drawing = drawn >= Window * 2 / 3;
	if (!storyBegun && drawing && polys >= Story3D * drawn)
	{
		storyBegun = true;
		NOTICE_LOG(RENDERER, "VR hands: the story has begun at %.0f s, hands off", t);
	}
	bool start = false;
	if (!storyBegun)
	{
		if (t - lastPress < PressHold)
			start = true;
		else if (drawing && t - lastPress >= PressEvery)
		{
			lastPress = t;
			start = true;
			INFO_LOG(RENDERER, "VR hands: Start at %.1f s (%u 3D polygons a frame)", t, polys / std::max(drawn, 1u));
		}
	}
	lightgunSet(0, OffScreen, OffScreen, start ? 4 : 0);
}

void onEvent(Event event, void *)
{
	switch (event)
	{
	case Event::Start:
		onStart();
		break;
	case Event::VBlank:
		onVBlank();
		break;
	case Event::Terminate:
		stopPrep("stopped");
		state = Off;
		break;
	default:
		break;
	}
}

struct Registration
{
	Registration() {
		EventManager::listen(Event::Start, onEvent);
		EventManager::listen(Event::VBlank, onEvent);
		EventManager::listen(Event::Terminate, onEvent);
	}
} registration;

bool save(const std::vector<u8>& data, const std::string& path)
{
	if (path.empty())
		return false;
	const std::string temp = path + ".part";
	FILE *f = nowide::fopen(temp.c_str(), "wb");
	if (f == nullptr)
		return false;
	const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
	if (fclose(f) != 0 || !ok)
	{
		nowide::remove(temp.c_str());
		return false;
	}
	nowide::remove(path.c_str());
	return nowide::rename(temp.c_str(), path.c_str()) == 0;
}

// One of Confidential Mission's guns made (complete: all of it, as buildGun flagged it; a merged
// one says so either way, verdict): into its file, and drawn from now on.
bool saveGun(const GunTarget& gun, int i, const std::vector<u8>& model, u32 polys, bool complete,
		const std::string *verdict)
{
	const std::string path = gunPath(gun);
	const Source *source = gameModelSource();
	const bool lupin = source != nullptr && source->attractGun();
	if (!(lupin ? saveGunCache(model, path, gun) : save(model, path)))
	{
		WARN_LOG(RENDERER, "VR hands: can't write %s", path.c_str());
		return false;
	}
	cmSaved = true;
	if (verdict != nullptr)
		NOTICE_LOG(RENDERER, "VR hands: the %s from this game (%s), %zu bytes to %s", gun.name, verdict->c_str(),
				model.size(), path.c_str());
	else if (complete)
		NOTICE_LOG(RENDERER, "VR hands: the %s from this game, %zu bytes to %s", gun.name, model.size(), path.c_str());
	else
		NOTICE_LOG(RENDERER, "VR hands: the %s from this game, %zu bytes to %s (%u of its %u polygons)", gun.name,
				model.size(), path.c_str(), polys, (u32)gun.wholePolys);
	gunDone[i] = true;
	newModel = true;
	return true;
}

u32 textureOf(const PolyParam& pp) {
	return pp.pcw.Texture ? pp.tcw.TexAddr << 3 : 0;
}

// buildGun's front and back (GunReport), for the log.
std::string facing(const GunReport& r)
{
	char s[200];
	snprintf(s, sizeof(s), "the lowest fifth %+.3f of its length behind the middle, the front %.3f and the back %.3f high%s",
			r.lowBehind, r.endHeights[0], r.endHeights[1], r.byEnds ? " (under 0.05: the thinner end is the muzzle)" : "");
	return s;
}

// A merged gun's captures and the textures they use, taken out of gunMutex's care (emu thread):
// put together into its model without the lock, then saved under it, complete or not, with what the
// merge did (and how long it took) in the log. They're let go of on the way out.
void finishMerged(const GunTarget& gun, int i, std::vector<Rip> captures, std::vector<CachedTexture> cache)
{
	if (captures.empty())
	{
		WARN_LOG(RENDERER, "VR hands: the %s never came up", gun.name);
		return;
	}
	const TextureSource textures = [&cache](u32 tcw, u32 size, Image& image) {
		for (const CachedTexture& c : cache)
			if (c.tcw == tcw && c.size == (size & 0x3F))
			{
				image = c.image;
				return c.ok;
			}
		return false;
	};
	std::vector<u8> model;
	std::string error;
	bool complete = false;
	GunReport r;
	const auto from = std::chrono::steady_clock::now();
	const bool built = buildGun(captures, gun, textures, model, error, &complete, &r);
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - from).count();
	NOTICE_LOG(RENDERER, "VR hands: the %s: %u captures, %u merged onto the fullest (no. %u, %u polygons; %u didn't fit, "
			"worst rms %.4f %% of its length): %u polygons %u/%u/%u/%u, the body's open edges %u, extents 1 : %.3f : %.3f; "
			"built in %.0f ms", gun.name, r.captures, r.merged, r.reference + 1, r.referencePolys, r.dropped, r.rms * 100.0,
			r.polys, r.texturePolys[0], r.texturePolys[1], r.texturePolys[2], r.texturePolys[3], r.openEdges, r.extents[1],
			r.extents[2], ms);
	if (!built)
	{
		WARN_LOG(RENDERER, "VR hands: the %s, but %s", gun.name, error.c_str());
		return;
	}
	NOTICE_LOG(RENDERER, "VR hands: the %s's front and back: %s", gun.name, facing(r).c_str());
	const std::string verdict = complete ? std::string("complete") : "incomplete: " + r.incomplete;
	std::lock_guard<std::mutex> lock(gunMutex);
	saveGun(gun, i, model, r.polys, complete, &verdict);
}

// The game's camera and framebuffer of a pass, for a capture.
Rip passRip(const rend_context& ctx)
{
	Rip rip;
	int fbWidth, fbHeight;
	getPvrFramebufferSize(ctx, fbWidth, fbHeight);
	rip.fbWidth = fbWidth;
	rip.fbHeight = fbHeight;
	const GameCamera cam = gameCamera(ctx);
	rip.focalX = cam.dcSize.x * 0.5f / cam.tanHalf.x;
	rip.focalY = cam.dcSize.y * 0.5f / cam.tanHalf.y;
	return rip;
}

void addRipPoly(const rend_context& ctx, const PolyParam& pp, Rip& rip)
{
	RipPoly& p = rip.polys.emplace_back();
	p.isp = pp.isp.full;
	p.tsp = pp.tsp.full;
	p.tcw = pp.tcw.full;
	p.pcw = pp.pcw.full;
	p.v.reserve(pp.count);
	for (u32 v = pp.first; v < pp.first + pp.count; v++)
	{
		const Vertex& vx = ctx.verts[v];
		p.v.push_back({ vx.x, vx.y, vx.z, vx.u, vx.v, { vx.col[0], vx.col[1], vx.col[2], vx.col[3] } });
	}
}

// A polygon of the gun: its texture, three corners or more.
bool gunPoly(const rend_context& ctx, const PolyParam& pp, const GunTarget& gun)
{
	return gun.has(textureOf(pp)) && pp.count >= 3 && pp.first + pp.count <= ctx.verts.size();
}

// A merged gun in a pass (gunMutex held): all of its polygons there, if minPolys or more,
// captured (the first vr.HandsCaptureLimit, if set), and their textures decoded while they're
// in VRAM.
void captureMerged(const rend_context& ctx, const RenderPass& pass, const RenderPass& prev, const GunTarget& gun, int i)
{
	std::vector<const PolyParam *> found;
	auto scan = [&](const std::vector<PolyParam>& polys, u32 from, u32 to) {
		for (u32 k = from; k < to && k < polys.size(); k++)
			if (gunPoly(ctx, polys[k], gun))
				found.push_back(&polys[k]);
	};
	scan(ctx.global_param_op, std::max(prev.op_count, 1u), pass.op_count);
	scan(ctx.global_param_pt, prev.pt_count, pass.pt_count);
	scan(ctx.global_param_tr, prev.tr_count, pass.tr_count);
	if (found.size() < gun.minPolys)
		return;
	gunSeen[i] = true;
	const int limit = config::VrHandsCaptureLimit;
	if ((limit > 0 && gunCaptures[i].size() >= (size_t)limit) || gunCaptures[i].size() >= CmMaxCaptures)
		return;
	Rip& rip = gunCaptures[i].emplace_back(passRip(ctx));
	for (const PolyParam *pp : found)
	{
		addRipPoly(ctx, *pp, rip);
		if (!pp->pcw.Texture)
			continue;
		const u32 tcw = pp->tcw.full, size = pp->tsp.full & 0x3F;
		if (std::none_of(textureCache.begin(), textureCache.end(), [&](const CachedTexture& c) { return c.tcw == tcw && c.size == size; }))
		{
			CachedTexture& c = textureCache.emplace_back();
			c.tcw = tcw;
			c.size = size;
			c.ok = decodeTexture(&vram[0], VRAM_SIZE, PALETTE_RAM, PAL_RAM_CTRL, tcw, size, c.image);
		}
	}
}

// Confidential Mission's guns, each pass: a merged one is captured (captureMerged); for each
// other still to get (the assault rifle, one draw), its longest run of polygons one after the
// other in a list. The first pass with all of it (wholePolys) that buildGun flags complete makes
// the gun.
void observeGuns(const rend_context& ctx, const RenderPass& pass, const RenderPass& prev, const Source& source)
{
	passes++;
	const TextureSource textures = [](u32 tcw, u32 tsp, Image& image) {
		return decodeTexture(&vram[0], VRAM_SIZE, PALETTE_RAM, PAL_RAM_CTRL, tcw, tsp, image);
	};
	std::unique_lock<std::mutex> lock(gunMutex);
	for (int i = 0; i < 3; i++)
	{
		const GunTarget& gun = source.guns[i];
		if (gunDone[i])
			continue;
		if (gun.merge && !source.attractGun())
		{
			// only in the run, from the stage 1 intro's part of it on
			if (state == Preparing && cmCapturing && !gunOver[i])
				captureMerged(ctx, pass, prev, gun, i);
			continue;
		}
		if (gunFailedAt[i] != 0 && passes - gunFailedAt[i] < 30)
			continue;
		const std::vector<PolyParam> *runList = nullptr;
		u32 runFrom = 0, runCount = 0;
		auto scan = [&](const std::vector<PolyParam>& polys, u32 from, u32 to) {
			u32 start = from, count = 0;
			for (u32 k = from; k < to && k < polys.size(); k++)
			{
				if (!gunPoly(ctx, polys[k], gun))
				{
					count = 0;
					continue;
				}
				if (count++ == 0)
					start = k;
				if (count > runCount)
				{
					runList = &polys;
					runFrom = start;
					runCount = count;
				}
			}
		};
		scan(ctx.global_param_op, std::max(prev.op_count, 1u), pass.op_count);
		scan(ctx.global_param_pt, prev.pt_count, pass.pt_count);
		scan(ctx.global_param_tr, prev.tr_count, pass.tr_count);
		if (runCount < gun.wholePolys)
			continue;

		std::vector<Rip> captures { passRip(ctx) };
		for (u32 k = runFrom; k < runFrom + runCount; k++)
			addRipPoly(ctx, (*runList)[k], captures[0]);
		std::vector<u8> model;
		std::string error;
		bool complete = false;
		GunReport r;
		if (!buildGun(captures, gun, textures, model, error, &complete, &r))
		{
			gunFailedAt[i] = passes;
			WARN_LOG(RENDERER, "VR hands: the %s, but %s", gun.name, error.c_str());
			continue;
		}
		if (!complete)
		{
			// whole by this count but not to buildGun's: not tried again for a while
			INFO_LOG(RENDERER, "VR hands: the %s's %u polygons, but not all of it to buildGun", gun.name, runCount);
			gunFailedAt[i] = passes;
			continue;
		}
		NOTICE_LOG(RENDERER, "VR hands: the %s's front and back: %s", gun.name, facing(r).c_str());
		if (!saveGun(gun, i, model, runCount, true))
			gunFailedAt[i] = passes;
	}
	lock.unlock();	// stopPrep takes it
	for (int i = 0; i < 3; i++)
		if (!gunDone[i])
			return;
	// all of them
	progress = 1.f;
	stopPrep("done");
	state = Off;
}

}	// namespace

std::string modelPath()
{
	// each game its own (HOTD2's, either version: hands.bin)
	const Source *source = gameModelSource();
	return source == nullptr ? std::string() : get_writable_data_path(source->modelFile());
}

std::string legacyModelPath()
{
	// the app's internal files directory, from the game path (files/games/...); only ever
	// the agent's hands
	const Source *source = gameModelSource();
	if (source == nullptr || source->kind != Source::AgentsPistol)
		return std::string();
	const std::string& game = settings.content.path;
	const size_t at = game.find("/files/");
	return at == std::string::npos ? std::string() : game.substr(0, at + 6) + "/hands.bin";
}

void observePass(const rend_context& ctx, const RenderPass& pass, const RenderPass& prev)
{
	const Source *source = gameModelSource();
	if (state == Off || source == nullptr)
		return;
	if (source->gunModels())
	{
		observeGuns(ctx, pass, prev, *source);
		return;
	}
	const bool staff = source->kind == Source::HerosStaff;
	passes++;
	auto each = [&](auto&& fn) {
		auto list = [&](const std::vector<PolyParam>& polys, u32 from, u32 to) {
			for (u32 k = from; k < to && k < polys.size(); k++)
				fn(polys[k]);
		};
		list(ctx.global_param_op, std::max(prev.op_count, 1u), pass.op_count);
		list(ctx.global_param_pt, prev.pt_count, pass.pt_count);
		list(ctx.global_param_tr, prev.tr_count, pass.tr_count);
	};
	auto texture = [](const PolyParam& pp) -> u32 {
		return pp.pcw.Texture ? pp.tcw.TexAddr << 3 : 0;
	};
	// how much 3D is in view (for the run), and how much of the pistol (or of the rods)
	const float overlayW = config::VrHudW;
	u32 polys3D = 0, gunPolys = 0;
	each([&](const PolyParam& pp) {
		gunPolys += staff ? source->staff.isRod(texture(pp)) : texture(pp) == source->pistol.gun;
		for (u32 i = pp.first; i < pp.first + pp.count && i < ctx.verts.size(); i++)
		{
			const float z = ctx.verts[i].z;
			if (z > 0.f && std::isfinite(z) && 1.f / z >= 0.5f && std::abs(1.f / z - overlayW) > 0.05f)
			{
				polys3D++;
				break;
			}
		}
	});
	seen3D += polys3D;
	seenPasses++;
	// a failed try (not all of it in view yet) isn't repeated every frame
	if (gunPolys < (staff ? StaffRodPolys : MinGunPolys) || (failedAt != 0 && passes - failedAt < 30))
		return;

	Rip rip;
	int fbWidth, fbHeight;
	getPvrFramebufferSize(ctx, fbWidth, fbHeight);
	rip.fbWidth = fbWidth;
	rip.fbHeight = fbHeight;
	const GameCamera cam = gameCamera(ctx);
	rip.focalX = cam.dcSize.x * 0.5f / cam.tanHalf.x;
	rip.focalY = cam.dcSize.y * 0.5f / cam.tanHalf.y;
	each([&](const PolyParam& pp) {
		if (!(staff ? source->staff.has(texture(pp)) : source->pistol.has(texture(pp))) || pp.first + pp.count > ctx.verts.size())
			return;
		RipPoly& p = rip.polys.emplace_back();
		p.isp = pp.isp.full;
		p.tsp = pp.tsp.full;
		p.tcw = pp.tcw.full;
		p.pcw = pp.pcw.full;
		p.v.reserve(pp.count);
		for (u32 i = pp.first; i < pp.first + pp.count; i++)
		{
			const Vertex& v = ctx.verts[i];
			p.v.push_back({ v.x, v.y, v.z, v.u, v.v, { v.col[0], v.col[1], v.col[2], v.col[3] } });
		}
	});
	std::vector<u8> model;
	std::string error;
	const TextureSource textures = [](u32 tcw, u32 tsp, Image& image) {
		return decodeTexture(&vram[0], VRAM_SIZE, PALETTE_RAM, PAL_RAM_CTRL, tcw, tsp, image);
	};
	const bool built = staff ? buildStaff(rip, source->staff, textures, model, error)
			: build(rip, source->pistol, textures, model, error);
	if (!built)
	{
		failedAt = passes;
		if (staff)
		{
			WARN_LOG(RENDERER, "VR hands: the hero's staff, but %s", error.c_str());
			std::lock_guard<std::mutex> lock(failureMutex);
			lastFailure = error;
		}
		else
			WARN_LOG(RENDERER, "VR hands: the game over scene, but %s", error.c_str());
		return;
	}
	if (!save(model, modelPath()))
	{
		failedAt = passes;
		WARN_LOG(RENDERER, "VR hands: can't write %s", modelPath().c_str());
		return;
	}
	NOTICE_LOG(RENDERER, "VR hands: %s from this game, %zu bytes to %s", source->name(), model.size(), modelPath().c_str());
	progress = 1.f;
	stopPrep("done");
	state = Off;
	newModel = true;
}

bool takeNewModel() {
	return newModel.exchange(false);
}

bool preparing() {
	return state == Preparing;
}

float prepProgress() {
	return progress;
}

void skipPrep() {
	skipRequested = true;
}

}
