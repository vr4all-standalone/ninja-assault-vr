/*
	Lightgun input over UDP (hotd2-vr).

	Lets an external tracker (webcam finger guns, later the Quest controllers) aim and
	fire the emulated light gun without taking over the host mouse. Listens on
	127.0.0.1 only, on the port in vr.UdpPort (0 = off). One text datagram per update:

		LG <player 0-3> <x> <y> <buttons>

	x, y: position on the game screen in 1/10000ths (0..10000; outside is off-screen).
	Integers on purpose: Flycast switches the C locale to the user's, and "%f" then
	expects a decimal comma on e.g. Dutch Windows.
	buttons: bit 0 trigger, bit 1 reload (off-screen shot), bit 2 start, bits 3-6 the gun's
	D-pad (up, down, left, right), bit 7 its B button, bit 8: the shot hit the headset's 2D
	plane (menus, text), where the game's own shot marker is in the right place.

	PC builds: lines starting "XR" go to the OpenXR simulator (vr.XrSim, rend/vr/xr_sim.h), and
	"POKE <addr hex> <size 1|2|4> <value hex>" writes RAM at the next VBlank (vr::queuePoke).

	Copyright 2026 mikermak. This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#include "udp_lightgun.h"
#include "types.h"
#include "cfg/option.h"
#include "emulator.h"
#include "input/gamepad.h"
#include "input/gamepad_device.h"
#include "input/mouse.h"
#include "log/Log.h"
#include "rend/vr_reproject.h"
#include "rend/vr/xr_sim.h"
#include "rend/vr/gun_input.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static void closeSocket(socket_t s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t INVALID_SOCKET = -1;
static void closeSocket(socket_t s) { close(s); }
#endif

namespace
{

std::atomic<bool> running;
u32 lastButtons[4];
vr::GunInput gunInputs[4];
std::atomic<int64_t> lastPacketMs[4];

int64_t nowMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void setButton(int player, u32 key, bool pressed)
{
	// kcode is active low
	if (pressed)
		kcode[player] &= ~key;
	else
		kcode[player] |= key;
}

void handle(const char *msg)
{
#ifndef __ANDROID__
	if (strncmp(msg, "XR", 2) == 0)
	{
		vr::xr::sim::udpLine(msg);
		return;
	}
	if (strncmp(msg, "POKE ", 5) == 0)
	{
		unsigned addr, size, value;
		char tail;
		if (sscanf(msg + 5, "%x %u %x %c", &addr, &size, &value, &tail) == 3)
			vr::queuePoke(addr, size, value);
		else
			WARN_LOG(INPUT, "UDP lightgun: bad line '%s'", msg);
		return;
	}
#endif
	int player, x, y;
	unsigned buttons;
	if (sscanf(msg, "LG %d %d %d %u", &player, &x, &y, &buttons) == 4)
		lightgunSet(player, x, y, buttons);
}

}

void lightgunSet(int player, int x, int y, u32 buttons)
{
	if (player < 0 || player > 3)
		return;
	vr::adaptGunInput(gunInputs[player], vr::gameAnalogOffscreenReload(), x, y, buttons);
	vr::adaptNinjaInput(gunInputs[player], settings.content.gameId == "NINJA ASSAULT", x, y, buttons);
	lastPacketMs[player] = nowMs();
	const int screenX = x * 640 / 10000, screenY = y * 480 / 10000;
	int cabinetX = screenX, cabinetY = screenY;
	vr::calibrateGunInput(player, cabinetX, cabinetY);
	mo_x_abs[player] = cabinetX;
	mo_y_abs[player] = cabinetY;
	// Only touch the buttons that changed, so the keyboard and pads keep working.
	const u32 changed = buttons ^ lastButtons[player];
	if (changed & 1)
		setButton(player, DC_BTN_A, buttons & 1);
	if ((changed & 1) && (buttons & 1) && player <= 1)
		// the game draws its own shot marker here (see vr::dropShotMarker)
		vr::noteGunShot(screenX, screenY, (buttons & 256) == 0);
	if (changed & 2)
		setButton(player, DC_BTN_RELOAD, buttons & 2);
	if (changed & 4)
		setButton(player, DC_BTN_START, buttons & 4);
	if (changed & 8)
		setButton(player, DC_DPAD_UP, buttons & 8);
	if (changed & 16)
		setButton(player, DC_DPAD_DOWN, buttons & 16);
	if (changed & 32)
		setButton(player, DC_DPAD_LEFT, buttons & 32);
	if (changed & 64)
		setButton(player, DC_DPAD_RIGHT, buttons & 64);
	if (changed & 128)
		setButton(player, DC_BTN_B, buttons & 128);
	if (changed & 0xff)		// (bit 8 flips whenever a held trigger crosses into the 2D plane)
		NOTICE_LOG(INPUT, "Light gun P%d at %d,%d: trigger %d reload %d start %d d-pad %x B %d 2D %d", player + 1,
				screenX, screenY, buttons & 1, (buttons >> 1) & 1, (buttons >> 2) & 1,
				(buttons >> 3) & 15, (buttons >> 7) & 1, (buttons >> 8) & 1);
	if ((changed & 1) && (buttons & 1) && (cabinetX != screenX || cabinetY != screenY))
		NOTICE_LOG(INPUT, "VR: P%d cabinet %d,%d for ray %d,%d", player + 1, cabinetX, cabinetY, screenX, screenY);
	lastButtons[player] = buttons;
}

namespace
{

void listen(int port)
{
#ifdef _WIN32
	WSADATA wsaData;
	WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
	socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock == INVALID_SOCKET)
	{
		ERROR_LOG(INPUT, "UDP lightgun: cannot create socket");
		running = false;
		return;
	}
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons((u16)port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(sock, (sockaddr *)&addr, sizeof(addr)) != 0)
	{
		ERROR_LOG(INPUT, "UDP lightgun: cannot bind 127.0.0.1:%d", port);
		closeSocket(sock);
		running = false;
		return;
	}
	NOTICE_LOG(INPUT, "UDP lightgun listening on 127.0.0.1:%d", port);
	char buf[128];
	while (running)
	{
		int n = (int)recv(sock, buf, sizeof(buf) - 1, 0);
		if (n <= 0)
			continue;
		buf[n] = '\0';
		handle(buf);
	}
	closeSocket(sock);
}

void onStart(Event, void *)
{
	for (auto& input : gunInputs) input = vr::GunInput {};
	const int port = config::VrUdpPort;
	if (port <= 0 || running)
		return;
	running = true;
	// Lives for the rest of the process; recv() blocks, so it is detached rather than joined.
	std::thread(listen, port).detach();
}

}

bool udpLightgunActive(int port)
{
	return port >= 0 && port < 4 && nowMs() - lastPacketMs[port] < 500;
}

namespace
{

struct Registration
{
	Registration() {
		EventManager::listen(Event::Start, onStart);
	}
} registration;

}
