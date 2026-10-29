/*
	A glTF 2.0 binary (GLB) read into one triangle mesh (hotd2-vr): the controllers' models from
	the OpenXR runtime (xr_controllers.h, XR_FB_render_model), and the player's own guns and
	controllers (xr_own.h). Parsed by cgltf (core/deps/cgltf).

	Copyright 2026 mikermak. This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#pragma once
#include "types.h"

#include <string>
#include <vector>

namespace vr::xr
{

// A base colour image as RGBA rows (top row first).
struct GlbImage { int width = 0, height = 0; std::vector<u8> rgba; };
// A primitive's triangles: indices[firstIndex, firstIndex + indexCount), its material's base
// colour factor (RGBA; 1, 1, 1, 1 without one) and base colour image (an index into images; -1:
// none, or not decoded). Mesh/material names retain semantic identity through later batching.
struct GlbPart { u32 firstIndex = 0, indexCount = 0; float color[4] { 1.f, 1.f, 1.f, 1.f }; int image = -1;
	std::string meshName, materialName; };

// Every triangle of the default scene (else of every node), in the file's own space (glTF:
// metres, y up) with the node transforms applied: positions and normals x, y, z, texture
// coordinates u, v (v down the image, as glTF has it; 0 for a primitive without a decoded
// image), indices into them, a part per primitive. The base colour images of the primitives
// that have one, when a PNG or a JPEG: the first maxImages that decode, in the order the
// primitives come; when none is, why in `untextured`: the image's MIME type (image/ktx2: glTF
// subset 1's Basis Universal, not decoded here) or what was wrong with it (empty when textured).
// colors: the vertices' own colours (COLOR_0, RGBA, linear as the base colour factor is), 4 a vertex,
// white where a primitive has none; empty when none has any.
struct GlbMesh { std::vector<float> positions, normals, uvs; std::vector<u32> indices; std::vector<GlbImage> images;
	std::vector<GlbPart> parts; std::string untextured; std::vector<float> colors; };

// All primitives (triangle lists, strips and fans), node transforms applied; base colour
// PNG/JPEG via stb_image, at most 4096 pixels a side, and with fitSide > 0 halved (2 x 2 pixels
// averaged) till no side is over it; other images: untextured. Primitives with no normals get
// face normals. External files (a buffer or image by file name) are not read. False, with why in
// `error`, when the data isn't a GLB this can draw (no triangles, or over 1,000,000 vertices or
// 3,000,000 indices). maxImages 1: the runtime's controllers, one texture for the whole model.
// pixels false: the images only looked at, not decoded (each GlbImage its size as fitted, its rgba
// empty), for what a model would cost before it is read (xr_own.h).
bool loadGlb(const u8 *data, size_t size, GlbMesh& out, std::string& error, int maxImages = 1, int fitSide = 0, bool pixels = true);

}
