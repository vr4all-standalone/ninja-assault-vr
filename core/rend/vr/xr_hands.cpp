/*
	The agent's hands and pistol from the game itself (hotd2-vr), or the hero's staff. See
	xr_hands.h.

	hands.bin in the data directory (little endian, made from the player's own game by
	hands_rip.cpp, or on the PC by hotd2-vr/assets/rip_hands.py), and in the same format the
	hero's staff of The Maze of the Kings, staff-mok.bin, and Confidential Mission's guns,
	gun-cm-*.bin (only the running game's files are read, again when one is made or another game
	starts; Confidential Mission's guns each on first use):
		"HND1", u32 version (1, or 2 with the two u32s below)
		f32 muzzle[3], grab[3], travel, onSlide[16] (column major)
		version 2: u32 flags (1 gun only: no hand meshes, travel 0; 2 complete: all of the
			gun was in the game's frame), u32 sourcePolys (the game's polygons it was made from)
		u32 textures, each: u32 width, height, flags (1 clamp u, 2 clamp v, 4 mirror u,
			8 mirror v, 16 alpha test), then width * height RGBA bytes
		u32 meshes (frame, slide, gun hand, open hand), each: u32 parts, each: u32 texture,
			u32 vertices, then the vertices: f32 position[3], normal[3], uv[2], u8 rgba[4]
	A model without a slide (the staff, a gun only) has travel 0 and no parts in the slide
	mesh; a gun only none in the hands' either (the other hand keeps its controller).

	The player's own guns (xr_own.h), GLB files, are turned into the same model when chosen (only the
	chosen one is read): a gun only, its frame mesh a part per base colour factor and image (the
	primitives that share both merged), the factor as the vertex colour and the image (or a white
	texel) as the texture; mipmapped, its CPU copies dropped once on the GPU (read again from its file
	after a GL loss).

	Lit like the arcade gun (xr_gun.cpp): a fixed key light and a dim fill, so the hands
	read as solid in the game's dark scenes, rather than with the light baked in that one
	frame of the game.

	Copyright 2026 mikermak. This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#include "xr_hands.h"
#include "arcade_gun_color.h"
#include "hands_rip.h"
#include "glb_mesh.h"
#include "xr_own.h"
#include "rend/vr_reproject.h"
#include "rend/gles/gles.h"
#include "rend/gles/glcache.h"
#include "cfg/option.h"
#include "log/Log.h"
#include "stdclass.h"
#include "types.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <glm/gtc/type_ptr.hpp>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <memory>
#include <cstring>
#include <string>
#include <vector>

namespace vr::xr
{
namespace
{

void logGlErrors()
{
	// (hotd2-vr: the first few seconds only, at most 10 lines: checked after every draw it
	// costs a GPU round trip and filled the log at headset rate)
	static int checks, logged;
	if (checks >= 1000 || logged >= 10)
		return;
	checks++;
	for (GLenum error; (error = glGetError()) != GL_NO_ERROR && logged < 10; logged++)
		ERROR_LOG(RENDERER, "VR hands OpenGL error 0x%04x", error);
}

enum { MeshFrame, MeshSlide, MeshGunHand, MeshOpenHand, MeshCount };

struct Texture
{
	u32 width, height, flags;
	std::vector<u8> rgba;
	GLuint id = 0;
};
struct Part
{
	u32 texture;
	u32 first, count;	// vertices
	bool arcadePlastic = false;	// semantic GLB surface, never inferred from RGB/texture
};
constexpr size_t VertexSize = 36;

struct Model
{
	HandsModel info;
	std::vector<Texture> textures;
	std::vector<Part> meshes[MeshCount];
	std::vector<u8> vertices;
	bool staff = false;	// the hero's staff (its fist off unless vr.StaffHand)
	u32 flags = 0;		// HND1 v2's (2: complete)
	bool own = false;	// a gun of the player's own: mipmapped, vertices and textures dropped once uploaded
	GLuint vbo = 0, vao = 0;	// (and the textures' ids) once drawn
};
// The running game's models: its own (handsModel()), and Confidential Mission's guns, each in
// the slot of its GunKind (the built-in gun's, 0, is the game's own model's; 1 is no kind's now).
struct Slot
{
	Model model;
	bool tried = false, loaded = false;
	bool missing = false;	// no file (else unreadable)
};
constexpr int GameSlot = 0, Slots = 4;
Slot slots[Slots];
std::string gunLogged;	// chosenGun()'s line, as last logged
bool rifleHeld[2];		// each player's hand holds the assault rifle (handGun()), as last logged
bool rifleModel[2];		// ... and with it the rifle's model (the in-game pistol's pickup), as last logged
// The player's own guns (xr_own.h), GunKind(OwnGuns + i): every guns/*.glb in own::guns()' order (the
// bundled ones first, then the player's, each in file-name order; a player's file with a bundled one's
// name takes that one's place), a skipped one too. Only the chosen one is read (chosenGun()): the
// others are just their names.
struct OwnGun
{
	Model model;
	own::GunFile file;	// as scanned
	std::string name;	// the menu's label
	bool valid = false;		// offered (the scan's checks, and not failed)
	bool loaded = false;	// read: model holds it
	bool failed = false;	// reading it failed (logged): not offered again till the file changes
};
std::vector<std::unique_ptr<OwnGun>> ownGuns;
unsigned ownGeneration;		// own::gunsGeneration() they were scanned at

GLuint program;
GLint uMvp = -1, uModel = -1, uEye = -1, uSpecular = -1, uShininess = -1, uAlphaTest = -1, uLight = -1, uTex = -1, uArcadeBaseColor = -1;

// A model's file into `model`; false without one it can read (`missing`: no file at all).
bool load(Model& model, std::string path, const std::string& legacy, bool& missing)
{
	const std::string first = path;
	// where older builds had it: the app's internal files
	FILE *f = nowide::fopen(path.c_str(), "rb");
	if (f == nullptr && !legacy.empty())
	{
		path = legacy;
		f = nowide::fopen(path.c_str(), "rb");
	}
	missing = f == nullptr;
	if (f == nullptr)
	{
		INFO_LOG(RENDERER, "XR: no %s, the arcade gun it is", first.c_str());
		return false;
	}
	bool ok = true;
	auto read = [&](void *to, size_t size) {
		ok = ok && fread(to, 1, size, f) == size;
		return ok;
	};
	auto u32At = [&]() {
		u32 v = 0;
		read(&v, 4);
		return v;
	};
	char magic[4] {};
	read(magic, 4);
	const u32 version = u32At();
	if (!ok || memcmp(magic, "HND1", 4) != 0 || (version != 1 && version != 2))
	{
		fclose(f);
		WARN_LOG(RENDERER, "XR: %s is not a hands model this build reads", path.c_str());
		return false;
	}
	float head[3 + 3 + 1 + 16];
	read(head, sizeof(head));
	model.info.muzzle = glm::make_vec3(&head[0]);
	model.info.grab = glm::make_vec3(&head[3]);
	model.info.travel = head[6];
	model.info.onSlide = glm::make_mat4(&head[7]);
	u32 flags = 0, sourcePolys = 0;
	if (version >= 2)
	{
		flags = u32At();
		sourcePolys = u32At();
	}
	model.info.gunOnly = (flags & 1) != 0;
	model.flags = flags;
	const u32 textures = u32At();
	for (u32 i = 0; ok && i < textures && i < 64; i++)
	{
		Texture t;
		t.width = u32At();
		t.height = u32At();
		t.flags = u32At();
		if (!ok || t.width == 0 || t.height == 0 || t.width > 1024 || t.height > 1024)
		{
			ok = false;
			break;
		}
		t.rgba.resize((size_t)t.width * t.height * 4);
		read(t.rgba.data(), t.rgba.size());
		model.textures.push_back(std::move(t));
	}
	const u32 meshes = u32At();
	for (u32 m = 0; ok && m < meshes && m < MeshCount; m++)
	{
		const u32 parts = u32At();
		for (u32 p = 0; ok && p < parts && p < 64; p++)
		{
			Part part;
			part.texture = u32At();
			part.count = u32At();
			part.first = (u32)(model.vertices.size() / VertexSize);
			if (!ok || part.texture >= model.textures.size() || part.count > 65536)
			{
				ok = false;
				break;
			}
			const size_t at = model.vertices.size();
			model.vertices.resize(at + part.count * VertexSize);
			read(&model.vertices[at], part.count * VertexSize);
			model.meshes[m].push_back(part);
		}
	}
	fclose(f);
	if (!ok || meshes < MeshCount)
	{
		WARN_LOG(RENDERER, "XR: %s is cut short or damaged", path.c_str());
		model = Model();
		return false;
	}
	const hands::Source *source = gameModelSource();
	model.staff = source != nullptr && source->kind == hands::Source::HerosStaff;
	NOTICE_LOG(RENDERER, "XR: the game's own model from %s (%zu textures, %zu vertices%s%s)", path.c_str(),
			model.textures.size(), model.vertices.size() / VertexSize, model.info.travel > 0.f ? "" : ", no slide",
			!model.info.gunOnly ? "" : (flags & 2) ? ", gun only" : ", gun only, not all of it");
	if (version >= 2)
		INFO_LOG(RENDERER, "XR: HND1 version %u, flags %u, from %u of the game's polygons", version, flags, sourcePolys);
	return true;
}

bool initGl()
{
	if (program != 0)
		return true;
	OpenGlSource vertex;
	vertex.addSource(VertexCompatShader).addSource(R"(
in highp vec3 in_pos;
in highp vec3 in_normal;
in highp vec2 in_uv;
in lowp vec4 in_base;
uniform highp mat4 mvp;
uniform highp mat4 model;
out highp vec3 vtx_pos;
out highp vec3 vtx_normal;
out highp vec2 vtx_uv;
out lowp vec4 vtx_color;
void main()
{
	vtx_pos = (model * vec4(in_pos, 1.0)).xyz;
	vtx_normal = mat3(model) * in_normal;
	vtx_uv = in_uv;
	vtx_color = in_base;
	gl_Position = mvp * vec4(in_pos, 1.0);
}
)");
	OpenGlSource fragment;
	fragment.addSource(PixelCompatShader).addSource(ArcadeModelBaseSource).addSource(R"(
uniform sampler2D tex;
uniform highp vec3 eyePos;
uniform mediump float specular;
uniform mediump float shininess;
uniform lowp float alphaTest;
uniform mediump vec4 arcadeBaseColor; // RGB absolute player base, A enable flag
uniform highp vec4 muzzleLight;	// room position, strength
in highp vec3 vtx_pos;
in highp vec3 vtx_normal;
in highp vec2 vtx_uv;
in lowp vec4 vtx_color;
void main()
{
	lowp vec4 base = arcadeModelBase(texture(tex, vtx_uv), vtx_color, arcadeBaseColor);
	if (base.a < alphaTest)
		discard;
	highp vec3 n = normalize(vtx_normal);
	highp vec3 v = normalize(eyePos - vtx_pos);
	if (dot(n, v) < 0.0)
		n = -n;		// a mirrored model, or inside faces seen through gaps
	const highp vec3 key = vec3(0.32, 0.86, 0.40);
	const highp vec3 fill = vec3(-0.45, -0.75, -0.48);
	mediump float diffuse = max(dot(n, key), 0.0);
	mediump float back = max(dot(n, fill), 0.0);
	highp vec3 h = normalize(key + v);
	mediump float highlight = pow(max(dot(n, h), 0.0), shininess) * specular;
	highp float edge = 1.0 - clamp(dot(n, v), 0.0, 1.0);
	mediump vec3 color = base.rgb * (0.32 + 0.72 * diffuse + 0.22 * back + 0.12 * edge * edge)
			+ vec3(highlight);
	if (muzzleLight.w > 0.0)
	{
		highp vec3 toFlash = muzzleLight.xyz - vtx_pos;
		highp float fd = length(toFlash);
		mediump float lit = muzzleLight.w * max(dot(n, toFlash / fd), 0.15) / (1.0 + fd * fd * 600.0);
		color += (base.rgb * 2.2 + 0.2) * vec3(1.0, 0.62, 0.28) * lit;
	}
	gl_FragColor = vec4(color, 1.0);
}
)");
	program = gl_CompileAndLink(vertex.generate().c_str(), fragment.generate().c_str());
	if (program == 0)
		return false;
	uMvp = glGetUniformLocation(program, "mvp");
	uModel = glGetUniformLocation(program, "model");
	uEye = glGetUniformLocation(program, "eyePos");
	uSpecular = glGetUniformLocation(program, "specular");
	uShininess = glGetUniformLocation(program, "shininess");
	uAlphaTest = glGetUniformLocation(program, "alphaTest");
	uLight = glGetUniformLocation(program, "muzzleLight");
	uTex = glGetUniformLocation(program, "tex");
	uArcadeBaseColor = glGetUniformLocation(program, "arcadeBaseColor");
	return true;
}

// A model's vertices and textures to the GPU, on its first draw; true if they went up just now. A
// gun of the player's own mipmapped (a big texture on a small gun shimmers without), its vertices and
// textures dropped after (read again from its file after a GL loss: termHands()).
bool upload(Model& model)
{
	if (model.vao != 0)
		return false;
	model.vao = createVertexArray();
	bindVertexArray(model.vao);
	glGenBuffers(1, &model.vbo);
	glBindBuffer(GL_ARRAY_BUFFER, model.vbo);
	glBufferData(GL_ARRAY_BUFFER, model.vertices.size(), model.vertices.data(), GL_STATIC_DRAW);
	glVertexAttribPointer(VERTEX_POS_ARRAY, 3, GL_FLOAT, GL_FALSE, VertexSize, (const void *)0);
	glVertexAttribPointer(VERTEX_NORM_ARRAY, 3, GL_FLOAT, GL_FALSE, VertexSize, (const void *)12);
	glVertexAttribPointer(VERTEX_UV_ARRAY, 2, GL_FLOAT, GL_FALSE, VertexSize, (const void *)24);
	glVertexAttribPointer(VERTEX_COL_BASE_ARRAY, 4, GL_UNSIGNED_BYTE, GL_TRUE, VertexSize, (const void *)32);
	glEnableVertexAttribArray(VERTEX_POS_ARRAY);
	glEnableVertexAttribArray(VERTEX_NORM_ARRAY);
	glEnableVertexAttribArray(VERTEX_UV_ARRAY);
	glEnableVertexAttribArray(VERTEX_COL_BASE_ARRAY);
	bindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glActiveTexture(GL_TEXTURE0);
	for (Texture& t : model.textures)
	{
		glGenTextures(1, &t.id);
		glcache.BindTexture(GL_TEXTURE_2D, t.id);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t.width, t.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, t.rgba.data());
		if (model.own)
			glGenerateMipmap(GL_TEXTURE_2D);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, model.own ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, t.flags & 1 ? GL_CLAMP_TO_EDGE : t.flags & 4 ? GL_MIRRORED_REPEAT : GL_REPEAT);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, t.flags & 2 ? GL_CLAMP_TO_EDGE : t.flags & 8 ? GL_MIRRORED_REPEAT : GL_REPEAT);
	}
	glcache.BindTexture(GL_TEXTURE_2D, 0);
	if (model.own)
	{
		model.vertices = std::vector<u8>();
		for (Texture& t : model.textures)
			t.rgba = std::vector<u8>();
	}
	return true;
}

// A model's GPU copies gone (it keeps its vertices and textures, but for a gun of the player's own:
// drawn again, it goes back up).
void releaseGl(Model& model)
{
	if (model.vbo != 0)
		glDeleteBuffers(1, &model.vbo);
	if (model.vao != 0)
		deleteVertexArray(model.vao);
	model.vbo = model.vao = 0;
	for (Texture& t : model.textures)
		if (t.id != 0)
		{
			glcache.DeleteTextures(1, &t.id);
			t.id = 0;
		}
}

void drawMesh(const Model& model, int mesh, const glm::mat4& viewProj, const glm::mat4& pose, float specular, float shininess, const glm::vec4& arcadeBase = glm::vec4(0.f))
{
	const glm::mat4 mvp = viewProj * pose;
	glUniformMatrix4fv(uMvp, 1, GL_FALSE, &mvp[0][0]);
	glUniformMatrix4fv(uModel, 1, GL_FALSE, &pose[0][0]);
	glUniform1f(uSpecular, specular);
	glUniform1f(uShininess, shininess);
	for (const Part& part : model.meshes[mesh])
	{
		const Texture& t = model.textures[part.texture];
		// Every part writes the enable flag, including metal and every unrelated model.
		const glm::vec4 color = part.arcadePlastic ? arcadeBase : glm::vec4(0.f);
		glUniform4f(uArcadeBaseColor, color.r, color.g, color.b, color.a);
		glcache.BindTexture(GL_TEXTURE_2D, t.id);
		glUniform1f(uAlphaTest, t.flags & 16 ? 0.5f : -1.f);
		glDrawArrays(GL_TRIANGLES, (GLint)part.first, (GLsizei)part.count);
	}
}

// A model just made, or another game: every model again, and the gun line.
void refresh()
{
	if (!hands::takeNewModel())
		return;
	termHands();
	for (Slot& slot : slots)
		slot = Slot();
	gunLogged.clear();
	rifleHeld[0] = rifleHeld[1] = false;
	rifleModel[0] = rifleModel[1] = false;
	own::scan();	// (another game: maybe another folder)
}

// A gun of the player's own into `model`, as Confidential Mission's guns are (gun space: the
// sidecar's grip the origin, its barrel -z, its up +y, scaled so the model spans its length along
// the barrel); false with why not.
bool loadOwnGun(Model& model, const own::GunFile& file, std::string& why)
{
	std::vector<u8> data;
	if (!own::readFile(file.path, data, own::MaxFileBytes))
	{
		why = "unreadable, or over 256 MB";
		return false;
	}
	GlbMesh mesh;
	std::string error;
	if (!loadGlb(data.data(), data.size(), mesh, error, own::MaxTextures, own::MaxTextureSide))
	{
		why = error;
		return false;
	}
	data = std::vector<u8>();
	const size_t triangles = mesh.indices.size() / 3;
	if (triangles > own::MaxTriangles)
	{
		why = "over " + std::to_string(own::MaxTriangles) + " triangles (" + std::to_string(triangles) + ")";
		return false;
	}
	const glm::vec3 up = glm::normalize(file.up);
	const glm::vec3 barrel = file.muzzle - file.grip;
	const glm::vec3 forward = glm::normalize(barrel - glm::dot(barrel, up) * up);
	const glm::vec3 z = -forward, x = glm::cross(up, z);
	const glm::mat3 turn = glm::transpose(glm::mat3(x, up, z));		// GLB -> gun space (rows x, up, z)
	float lo = FLT_MAX, hi = -FLT_MAX;
	for (size_t i = 0; i + 2 < mesh.positions.size(); i += 3)
	{
		const float along = glm::dot(glm::make_vec3(&mesh.positions[i]), forward);
		lo = std::min(lo, along);
		hi = std::max(hi, along);
	}
	if (!(hi - lo > 1e-9f))
	{
		why = "no length along the barrel";
		return false;
	}
	const float scale = file.length / (hi - lo);

	model = Model();
	// texture 0 a white texel (the parts without an image: their colour alone), then the images
	Texture white { 1, 1, 0 };
	white.rgba = { 255, 255, 255, 255 };
	model.textures.push_back(std::move(white));
	for (GlbImage& image : mesh.images)
	{
		Texture t { (u32)image.width, (u32)image.height, 0 };
		t.rgba = std::move(image.rgba);
		model.textures.push_back(std::move(t));
	}
	model.vertices.reserve(mesh.indices.size() * VertexSize);
	// Batch only matching material identities, colours, textures and semantic surface roles.
	struct Group { u8 rgba[4]; u32 texture; std::string materialName; bool arcadePlastic; std::vector<const GlbPart *> parts; };
	std::vector<Group> groups;
	for (const GlbPart& gp : mesh.parts)
	{
		Group g {};
		g.materialName = gp.materialName;
		g.arcadePlastic = arcadeGunPlasticPart(gp.meshName.c_str());
		for (int k = 0; k < 4; k++)
			g.rgba[k] = (u8)std::lround(std::clamp(gp.color[k], 0.f, 1.f) * 255.f);
		g.texture = gp.image >= 0 && gp.image + 1 < (int)model.textures.size() ? (u32)gp.image + 1 : 0;
		auto same = std::find_if(groups.begin(), groups.end(),
				[&](const Group& o) { return o.texture == g.texture && o.materialName == g.materialName && o.arcadePlastic == g.arcadePlastic && memcmp(o.rgba, g.rgba, 4) == 0; });
		if (same == groups.end())
			same = groups.insert(groups.end(), g);
		same->parts.push_back(&gp);
	}
	std::string colours;
	for (const Group& g : groups)
	{
		if (model.meshes[MeshFrame].size() < 8)
		{
			char hex[16];
			snprintf(hex, sizeof(hex), " #%02x%02x%02x", g.rgba[0], g.rgba[1], g.rgba[2]);
			colours += hex;
		}
		Part part;
		part.texture = g.texture;
		part.arcadePlastic = g.arcadePlastic;
		part.first = (u32)(model.vertices.size() / VertexSize);
		part.count = 0;
		for (const GlbPart *gp : g.parts)
		{
			for (u32 k = 0; k < gp->indexCount; k++)
			{
				const u32 v = mesh.indices[gp->firstIndex + k];
				const glm::vec3 p = scale * (turn * (glm::make_vec3(&mesh.positions[v * 3]) - file.grip));
				const glm::vec3 n = turn * glm::make_vec3(&mesh.normals[v * 3]);
				const float vertex[8] { p.x, p.y, p.z, n.x, n.y, n.z, mesh.uvs[v * 2], mesh.uvs[v * 2 + 1] };
				// the part's colour, times the vertex's own (COLOR_0) when the GLB has them
				u8 rgba[4] { g.rgba[0], g.rgba[1], g.rgba[2], g.rgba[3] };
				if (!mesh.colors.empty())
					for (int c = 0; c < 4; c++)
						rgba[c] = (u8)std::lround(g.rgba[c] * std::clamp(mesh.colors[v * 4 + c], 0.f, 1.f));
				const size_t at = model.vertices.size();
				model.vertices.resize(at + VertexSize);
				memcpy(&model.vertices[at], vertex, sizeof(vertex));
				memcpy(&model.vertices[at + sizeof(vertex)], rgba, 4);
			}
			part.count += gp->indexCount;
		}
		model.meshes[MeshFrame].push_back(part);
	}
	if (groups.size() > 8)
		colours += " ...";
	model.info.muzzle = scale * (turn * barrel);
	model.info.grab = glm::vec3(0.f);
	model.info.travel = 0.f;
	model.info.onSlide = glm::mat4(1.f);
	model.info.gunOnly = true;
	model.flags = 3;	// (gun only, all of it)
	model.own = true;
	const glm::vec3& m = model.info.muzzle;
	NOTICE_LOG(RENDERER, "XR: own gun \"%s\" from %s (%zu triangles, %zu primitive%s in %zu part%s%s, %zu texture%s; muzzle %.4f %.4f %.4f)",
			file.name.c_str(), file.shown.c_str(), triangles, mesh.parts.size(), mesh.parts.size() == 1 ? "" : "s",
			groups.size(), groups.size() == 1 ? "" : "s", colours.c_str(), mesh.images.size(), mesh.images.size() == 1 ? "" : "s",
			m.x, m.y, m.z);
	return true;
}

// A gun of the player's own let go: off the GPU, its model gone (read again from its file if chosen).
void unloadOwn(OwnGun& gun)
{
	releaseGl(gun.model);
	gun.model = Model();
	gun.loaded = false;
}

// The player's own guns as the folder has them now (xr_own.h): a gun whose files didn't change kept
// as it was (its model too, if read); the scan logs which are offered and which skipped, and why.
void refreshOwn()
{
	if (own::gunsGeneration() == 0)
		own::scan();
	if (own::gunsGeneration() == ownGeneration)
		return;
	ownGeneration = own::gunsGeneration();
	std::vector<std::unique_ptr<OwnGun>> next;
	for (const own::GunFile& file : own::guns())
	{
		std::unique_ptr<OwnGun> gun;
		for (auto& old : ownGuns)
			if (old != nullptr && old->file.path == file.path && old->file.stamp == file.stamp)
			{
				gun = std::move(old);
				break;
			}
		if (gun == nullptr)
			gun = std::make_unique<OwnGun>();
		gun->file = file;
		gun->name = file.name;
		gun->valid = file.usable && !gun->failed;
		if (!gun->valid && gun->loaded)
			unloadOwn(*gun);	// (its files now refused)
		next.push_back(std::move(gun));
	}
	// the files gone or changed
	for (auto& old : ownGuns)
		if (old != nullptr)
			releaseGl(old->model);
	ownGuns = std::move(next);
}

// The player's own gun i read, if it isn't (the chosen one: chosenGun()); false if it can't be, with
// "XR: own gun guns/<file> skipped: <why>" once (not offered again till its files change).
bool readOwn(int i)
{
	OwnGun& gun = *ownGuns[i];
	if (!gun.valid)
		return false;
	if (gun.loaded)
		return true;
	std::string why;
	gun.loaded = loadOwnGun(gun.model, gun.file, why);
	if (!gun.loaded)
	{
		gun.failed = true;
		gun.valid = false;
		gun.model = Model();
		NOTICE_LOG(RENDERER, "XR: own gun %s skipped: %s", gun.file.shown.c_str(), why.c_str());
	}
	return gun.loaded;
}

// The player's own gun `kind` is: its index in ownGuns; -1 if none.
int ownIndex(GunKind kind)
{
	const int i = (int)kind - OwnGuns;
	return i >= 0 && i < (int)ownGuns.size() ? i : -1;
}

bool selectableGuns() {
	return gameGunMenu();
}

}	// namespace

const HandsModel *handsModel()
{
	refresh();
	if (selectableGuns())
		return gunModel(chosenGun());
	Slot& slot = slots[GameSlot];
	if (!slot.tried)
	{
		slot.tried = true;
		const std::string path = hands::modelPath();
		if (path.empty())
			INFO_LOG(RENDERER, "XR: no model of its own for this game, the arcade gun it is");
		else
			slot.loaded = load(slot.model, path, hands::legacyModelPath(), slot.missing);
	}
	return slot.loaded ? &slot.model.info : nullptr;
}

const HandsModel *gunModel(GunKind kind)
{
	refresh();
	if (kind == GunKind::Arcade || !selectableGuns())
		return nullptr;
	if ((int)kind >= OwnGuns)
	{
		// (read by chosenGun(), the chosen one only)
		refreshOwn();
		const int i = ownIndex(kind);
		return i >= 0 && ownGuns[i]->loaded ? &ownGuns[i]->model.info : nullptr;
	}
	Slot& slot = slots[(int)kind];
	if (!slot.tried)
	{
		slot.tried = true;
		// (a kind's file is the profile's guns[kind - 1]: guns[0], 1.1's cabinet gun's, is none now)
		slot.missing = true;
		const hands::Source *source = gameModelSource();
		if (source != nullptr)
		{
			const hands::GunTarget& gun = source->guns[(int)kind - 1];
			if (gun.used())
			{
				const std::string path = get_writable_data_path(gun.file);
				// Lupin's prep and renderer must agree: skipped/rejected caches keep the fallback.
				// CM still accepts its partial guns under the existing generic-loader policy.
				if (source->attractGun() && !hands::loadGunCache(path, gun))
					slot.missing = !file_exists(path);
				else
					slot.loaded = load(slot.model, path, std::string(), slot.missing);
			}
		}
	}
	return slot.loaded ? &slot.model.info : nullptr;
}

bool gunComplete(GunKind kind)
{
	if ((int)kind >= OwnGuns)
	{
		// (offered, read or not)
		refreshOwn();
		const int i = ownIndex(kind);
		return i >= 0 && ownGuns[i]->valid;
	}
	return kind == GunKind::Arcade || (gunModel(kind) != nullptr && (slots[(int)kind].model.flags & 2) != 0);
}

const char *gunName(GunKind kind)
{
	if ((int)kind >= OwnGuns)
	{
		const int i = ownIndex(kind);
		return i >= 0 ? ownGuns[i]->name.c_str() : "folder gun";
	}
	switch (kind)
	{
	case GunKind::Pistol:
		return gameModelSource() != nullptr && gameModelSource()->kind == hands::Source::NinjaGun ? "in-game Mach Gun" : "in-game pistol";
	case GunKind::Rifle:
		return "assault rifle";
	default:
		return "built-in gun";
	}
}

GunKind chosenGun()
{
	refresh();
	if (!selectableGuns())
		return GunKind::Arcade;	// (another game's own model takes no choosing: handModel())
	// the assault rifle's model too, read at the start with the other guns (drawHands() puts it on
	// the GPU with them): at the pickup, mid-play, it could cost the headset a frame (Ruling S33)
	if (config::VrGameHands)
		gunModel(GunKind::Rifle);
	// vr.Gun 4 and over: the folder gun vr.GunFile names, its file name without .glb in any case, as the
	// scan folds it; any other setting the in-game pistol (1.1's 0 Auto, 2 the cabinet gun, 3 the arcade
	// gun and below 0 too: read so, not rewritten)
	refreshOwn();
	auto folderGun = [](const std::string& stem) {
		for (size_t i = 0; i < ownGuns.size(); i++)
			if (own::sameStem(ownGuns[i]->file.stem, stem))
				return (int)i;
		return -1;
	};
	GunKind kind = GunKind::Arcade;
	int own = -1;		// the folder gun held (read; the others let go)
	std::string why;	// why not the one chosen
	bool builtIn = false;
#ifndef __ANDROID__
	if (config::VrBuiltInGun)
	{
		builtIn = true;
		why = "vr.BuiltInGun";
	}
#endif
	if (!builtIn && config::VrGun >= OwnGuns)
	{
		const int i = folderGun(config::VrGunFile.get());
		if (i >= 0 && readOwn(i))
			own = i;
		else
			why = (i >= 0 ? ownGuns[i]->name : !config::VrGunFile.get().empty() ? config::VrGunFile.get() : std::string("the folder gun"))
					+ " is not there";
	}
	// the in-game pistol (one made without all of it too), else the Arcade gun's file, else the built-in gun
	if (!builtIn && own < 0 && config::VrGameHands && gunModel(GunKind::Pistol) != nullptr)
		kind = GunKind::Pistol;
	else if (!builtIn && own < 0)
	{
		const int i = folderGun(ArcadeGunStem);
		if (i >= 0 && readOwn(i))
		{
			own = i;
			if (why.empty())
				why = !config::VrGameHands ? "the game's models are off"
						: gameModelSource() == nullptr ? "native gun model unavailable" : slots[(int)GunKind::Pistol].missing ? "the in-game pistol is not made yet" : "the in-game pistol can't be read";
		}
		else
			why += std::string(why.empty() ? "" : ", ") + "no gun file loads";
	}
	if (own >= 0)
		kind = GunKind(OwnGuns + own);
	// only the one held stays read (its textures can be big)
	for (size_t i = 0; i < ownGuns.size(); i++)
		if (ownGuns[i]->loaded && (int)i != own)
			unloadOwn(*ownGuns[i]);
	const std::string line = std::string(gunName(kind)) + " (chosen" + (why.empty() ? "" : ": " + why) + ")";
	if (line != gunLogged)
	{
		gunLogged = line;
		NOTICE_LOG(RENDERER, "XR: gun: %s", line.c_str());
	}
	return kind;
}

GunKind handGun(int player)
{
	if (player < 0 || player > 1 || !selectableGuns())
		return chosenGun();
	// The in-game pistol's pickup is the in-game rifle (its model); any other gun stays in the hand and
	// fires the rifle's rounds (CM VR 1.2 spec section 2: gunKnocks' Shot and Auto knocks, each with its
	// kick and flash at that gun's muzzle), the pistol too without the rifle's model.
	const GunKind held = chosenGun();
	const bool model = held == GunKind::Pistol && handModel(GunKind::Rifle) != nullptr;
	// (not read: the emulator stopped, the VR menu open: the hand keeps what it holds)
	vr::GunState gun;
	if (vr::gunState(player, gun) && gun.valid && gun.rifle != rifleHeld[player])
	{
		rifleHeld[player] = gun.rifle;
		if (gun.rifle)
		{
			rifleModel[player] = model;
			if (model)
				NOTICE_LOG(RENDERER, "XR: P%d hand: assault rifle (%s)", player + 1, gameModelSource()->guns[(int)GunKind::Rifle - 1].file);
			else
				NOTICE_LOG(RENDERER, "XR: P%d hand: rifle rounds with the %s", player + 1, gunName(held));
		}
		else if (rifleModel[player])
			NOTICE_LOG(RENDERER, "XR: P%d hand: back to the %s", player + 1, gunName(held));
		else
			NOTICE_LOG(RENDERER, "XR: P%d hand: rifle rounds over (the %s)", player + 1, gunName(held));
	}
	return rifleHeld[player] && model ? GunKind::Rifle : held;
}

const HandsModel *handModel(GunKind kind)
{
	if ((int)kind >= OwnGuns)
		return gunModel(kind);
	if (!config::VrGameHands)
		return nullptr;
	return selectableGuns() ? gunModel(kind) : handsModel();
}

std::vector<GunChoice> gunChoices()
{
	// another game's own model takes no choosing: one choice, the row locked (vr.Gun kept)
	if (!selectableGuns())
		return { { (int)config::VrGun, config::VrGameHands && handsModel() != nullptr ? "The game's own" : "Arcade gun", true, true } };
	// (with the game's own models off the pistol isn't made: the row passes it)
	std::vector<GunChoice> choices;
	if (gameModelSource() != nullptr)
		choices.push_back({ 1, gameModelSource()->kind == hands::Source::NinjaGun ? "In-game Mach Gun" : "In-game pistol", config::VrGameHands && gunModel(GunKind::Pistol) != nullptr, gunComplete(GunKind::Pistol) });
	// the folder guns, whatever vr.GameHands says: the bundled ones first (a skipped one isn't made)
	refreshOwn();
	for (size_t i = 0; i < ownGuns.size(); i++)
		choices.push_back({ OwnGuns, ownGuns[i]->name.c_str(), ownGuns[i]->valid, true, ownGuns[i]->file.stem.c_str() });
	if (choices.empty()) choices.push_back({ 1, "Arcade gun", true, true });
	return choices;
}

void drawHands(const glm::mat4& viewProj, const glm::vec3& eyePos, const HandsView& view)
{
	Model *found = nullptr;
	bool arcade = false;
	for (Slot& slot : slots)
		if (slot.loaded && view.model == &slot.model.info)
			found = &slot.model;
	for (auto& gun : ownGuns)
		if (gun->loaded && view.model == &gun->model.info)
		{
			found = &gun->model;
			arcade = own::sameStem(gun->file.stem, ArcadeGunStem);
		}
	if (found == nullptr || !initGl())
		return;
	// every model loaded goes to the GPU at the first draw, not just this one: the assault
	// rifle's at its pickup mid-play could cost a frame (Ruling S33); of the player's own guns only
	// the chosen one is loaded
	for (Slot& slot : slots)
		if (slot.loaded)
			upload(slot.model);
	for (auto& gun : ownGuns)
		if (gun->loaded)
		{
			size_t bytes = 0;
			for (const Texture& t : gun->model.textures)
				bytes += t.rgba.size();
			const size_t images = gun->model.textures.size() - 1;	// (after the white texel)
			if (upload(gun->model))
				NOTICE_LOG(RENDERER, "XR: own gun \"%s\" on the GPU (%zu image%s, %.1f MB, mipmapped)", gun->name.c_str(),
						images, images == 1 ? "" : "s", bytes / 1048576.0);
		}
	Model& model = *found;
	glcache.UseProgram(program);
	// Logical-player RGB is enabled only by named plastic parts in drawMesh.
	const glm::vec4 arcadeBase = arcade ? glm::vec4(arcadeGunBaseColor(view.player), 1.f) : glm::vec4(0.f);
	glUniform3f(uEye, eyePos.x, eyePos.y, eyePos.z);
	glUniform4f(uLight, view.muzzleLight.x, view.muzzleLight.y, view.muzzleLight.z, view.muzzleLight.w);
	glUniform1i(uTex, 0);
	glActiveTexture(GL_TEXTURE0);
	bindVertexArray(model.vao);

	// metal with a sheen, skin and cotton matt
	drawMesh(model, MeshFrame, viewProj, view.gunPose, 0.35f, 40.f, arcadeBase);
	drawMesh(model, MeshSlide, viewProj, glm::translate(view.gunPose, glm::vec3(0.f, 0.f, view.slide)), 0.45f, 50.f, arcadeBase);
	// The hero's gloves came out mirrored in the headset (the other hand a right glove on
	// the left controller: the one around his staff seems to be his left), so both are
	// drawn mirrored in x about their own origin; the staff itself stays as it is.
	const glm::mat4 gloves = model.staff ? glm::scale(glm::mat4(1.f), glm::vec3(-1.f, 1.f, 1.f)) : glm::mat4(1.f);
	// (the hero's fist sits where the player's own hand is, but made for a staff held
	// upright: it looked better left out)
	if (!model.staff || config::VrStaffHand)
		drawMesh(model, MeshGunHand, viewProj, view.gunPose * gloves, 0.06f, 10.f);
	if (view.otherHand)
		drawMesh(model, MeshOpenHand, viewProj, view.handPose * gloves, 0.06f, 10.f);

	bindVertexArray(0);
	glcache.BindTexture(GL_TEXTURE_2D, 0);
	logGlErrors();
}

void termHands()
{
	if (program != 0)
		glcache.DeleteProgram(program);
	program = 0;
	for (Slot& slot : slots)
		releaseGl(slot.model);
	// (their vertices and textures dropped once uploaded: read again from the file when next chosen)
	for (auto& gun : ownGuns)
		unloadOwn(*gun);
	uMvp = uModel = uEye = uSpecular = uShininess = uAlphaTest = uLight = uTex = uArcadeBaseColor = -1;
}

}
