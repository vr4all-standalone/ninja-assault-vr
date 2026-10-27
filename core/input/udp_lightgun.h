/*
	Lightgun input over UDP (hotd2-vr). See udp_lightgun.cpp.

	Copyright 2026 mikermak. This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#pragma once

#include "types.h"

// Aim and fire the emulated light gun directly (the UDP datagrams and the headset's
// controller both end up here). x, y in 1/10000ths of the game screen (outside is
// off-screen); buttons: bit 0 trigger, bit 1 reload (off-screen shot), bit 2 start,
// bits 3-6 the D-pad (up, down, left, right), bit 7 the gun's B (skip, back), bit 8: this
// shot hit the headset's 2D plane (menus, text), so the game's own shot marker stays.
void lightgunSet(int player, int x, int y, u32 buttons);

// True while the UDP light gun drives this port (a datagram arrived recently).
// Mouse and analog-stick aiming stand aside meanwhile, so they can't fight over the crosshair.
bool udpLightgunActive(int port);

// Explicit XR cabinet entry/exit Test, Next and Select; separate from ordinary Start.
void lightgunService(bool pressed, bool next=false, bool select=false);
bool lightgunServiceActive();
bool lightgunServiceNextActive();
bool lightgunServiceSelectActive();
