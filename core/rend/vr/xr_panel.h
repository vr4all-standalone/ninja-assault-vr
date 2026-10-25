/*
	Panels with text in the headset (hotd2-vr): a few centred lines for what the app has to say
	when there's no game to show (making the agent's hands at the first start, hands_rip.h), a
	panel of rows to choose from (the settings menu, xr_menu.h), and small pictures of one line
	each (the controllers' labels, xr_labels.h).

	Copyright 2026 mikermak. This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#pragma once
#include <glm/glm.hpp>
#include "xr_theme.h"
#include <string>
#include <vector>

namespace vr::xr
{

struct PanelLine
{
	std::string text;
	float size;			// letter height, of the panel's height
	glm::vec3 colour;
};

struct Panel
{
	bool brand = false; // embedded VR4ALL launch image
	std::vector<PanelLine> lines;
	float progress = -1.f;	// a bar under the text, 0..1 (below 0: none)
	std::string footer;		// small, at the bottom
	// Shared purple VR4ALL appearance for every preparation and completion panel.
	glm::vec3 back = theme::Background, frame = theme::Accent;
	glm::vec3 barTrack = theme::Line, barFill = theme::Accent, footerColour = theme::Muted;
};

// The panel, 1.3 m wide, 1.6 m in front of where the game camera is, at eye height. The
// room is dark around it. viewProj is the eye's (room space, relative to the game camera's
// place). Its picture is made again only when what it says changes.
void drawPanel(const glm::mat4& viewProj, const Panel& panel);

// A row: its label on the left, its value on the right. A stepper row has a - at its left end
// and a + at its right one (a click on its left half is -, on its right half +).
struct PanelRow
{
	std::string label, value;
	bool selected = false, dim = false;
	bool stepper = false;
};

struct RowsPanel
{
	std::string title;
	std::vector<PanelRow> rows;
	std::string footer;
};

// A rows panel's size and its rows' place, in metres: the panel's width and height, how far
// apart its rows are, and the top row's middle over the panel's middle.
struct RowsLayout
{
	float width, height, rowPitch, topRow;
};
RowsLayout rowsLayout(const RowsPanel& panel);

// The rows panel at `model`: its middle, +y up, facing +z (its front is seen from +z), in the
// same room space as viewProj's. Like drawPanel, drawn over what's there (no depth test).
void drawRowsPanel(const glm::mat4& viewProj, const glm::mat4& model, const RowsPanel& panel);
// Where a ray (room space) meets the rows panel at `model`, from the front: the point, and
// across and down the panel (0..1 each). False when it misses.
bool rowsPanelHit(const glm::mat4& model, const RowsPanel& panel, const glm::vec3& origin, const glm::vec3& dir,
		glm::vec3 *point = nullptr, glm::vec2 *uv = nullptr);
// The row the ray points at, or -1 (it misses, or it's on the title or the footer); *u: where
// across the panel, 0 at its left edge, 1 at its right one.
int rowAt(const glm::mat4& model, const RowsPanel& panel, const glm::vec3& origin, const glm::vec3& dir, float *u);

// One line of text as a small picture of its own (the controllers' labels, xr_labels.h): letters px
// high in `colour`, on a dark plate with round corners and a margin; width and height: its size in
// pixels. Painted and uploaded once for each text, px and colour (cached by them: the same arguments
// give the same texture again, nothing uploaded); termPanel lets go of them. 0 (0 x 0) without the font.
unsigned textTexture(const std::string& text, float px, const glm::vec3& colour, int& width, int& height);
// A texture (0: plain white, for a line or a dot) on a quad hx by hy metres each way from the
// middle of `model`, facing +z, its top at +y; like drawPanel, over what's there (no depth test),
// blended, its colour times `tint`.
void drawTexture(const glm::mat4& viewProj, const glm::mat4& model, unsigned texture, float hx, float hy,
		const glm::vec4& tint = glm::vec4(1.f));

void termPanel();

}
