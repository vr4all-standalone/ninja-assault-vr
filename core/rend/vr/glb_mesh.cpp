/*
	A glTF 2.0 binary (GLB) read into one triangle mesh (hotd2-vr). See glb_mesh.h.

	The data comes from the OpenXR runtime or the player's own files (xr_own.h), so it is checked as untrusted: cgltf_validate (every
	accessor within its buffer), positions and indices only from buffer views (not sparse),
	indices within their primitive's vertices, at most 1,000,000 vertices and 3,000,000 indices,
	and images decoded only from the GLB's own buffer, at most 4096 pixels a side (asked before
	decoding).

	Copyright 2026 mikermak. This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#include "glb_mesh.h"

// the one translation unit with cgltf's implementation
#define CGLTF_IMPLEMENTATION
#include "cgltf/cgltf.h"
// (stb_image's implementation is in rend/CustomTexture.cpp, PNG and JPEG only)
#include <stb_image.h>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cstring>
#include <map>

namespace vr::xr
{

namespace
{

const char *resultName(cgltf_result r)
{
	switch (r)
	{
	case cgltf_result_success: return "success";
	case cgltf_result_data_too_short: return "data too short";
	case cgltf_result_unknown_format: return "unknown format";
	case cgltf_result_invalid_json: return "invalid JSON";
	case cgltf_result_invalid_gltf: return "invalid glTF";
	case cgltf_result_invalid_options: return "invalid options";
	case cgltf_result_file_not_found: return "file not found";
	case cgltf_result_io_error: return "I/O error";
	case cgltf_result_out_of_memory: return "out of memory";
	case cgltf_result_legacy_gltf: return "glTF 1.0";
	default: return "error";
	}
}

constexpr size_t MaxVertices = 1000000, MaxIndices = 3000000;
constexpr int MaxImageSide = 4096;

// What a walk through the nodes has so far besides the mesh.
struct Reader
{
	GlbMesh& out;
	int maxImages = 1, fitSide = 0;
	bool pixels = true;		// false: images sized, not decoded
	std::map<const cgltf_image *, int> imageAt;	// each image tried: its index in out.images (-1: not usable)
	bool tooBig = false;		// over MaxVertices or MaxIndices: nothing is drawn
};

// The base colour image, decoded into out: from a buffer view of the GLB (or a data: URI cgltf
// loaded into a buffer), PNG or JPEG. Else why not: its MIME type, or what was wrong.
std::string loadImage(const cgltf_image *image, GlbImage& out, bool pixels)
{
	const std::string mime = image->mime_type != nullptr ? image->mime_type : "no MIME type";
	if (image->mime_type != nullptr && strcmp(image->mime_type, "image/png") && strcmp(image->mime_type, "image/jpeg"))
		return mime;
	if (image->buffer_view == nullptr || image->buffer_view->buffer->data == nullptr)
		return mime + ", not in the GLB";
	const cgltf_buffer_view *view = image->buffer_view;
	if (view->offset + view->size > view->buffer->size || view->size > 0x7fffffff)
		return mime + ", past its buffer";
	const stbi_uc *data = (const stbi_uc *)cgltf_buffer_view_data(view);
	int w = 0, h = 0, channels = 0;
	// its size first: nothing big is decoded
	if (data == nullptr || !stbi_info_from_memory(data, (int)view->size, &w, &h, &channels))
		return mime + ", not decodable";
	if (w <= 0 || h <= 0 || w > MaxImageSide || h > MaxImageSide)
		return mime + " " + std::to_string(w) + "x" + std::to_string(h) + ", over 4096";
	if (!pixels)
	{
		out.width = w;
		out.height = h;
		return std::string();
	}
	// top row first (stb_image's flip is per thread, and other Flycast callers turn it on)
	stbi_set_flip_vertically_on_load_thread(0);
	u8 *decoded = stbi_load_from_memory(data, (int)view->size, &w, &h, &channels, 4);
	if (decoded == nullptr)
		return mime + ", not decodable";
	std::string why;
	if (w > 0 && h > 0 && w <= MaxImageSide && h <= MaxImageSide)
	{
		out.width = w;
		out.height = h;
		out.rgba.assign(decoded, decoded + (size_t)w * h * 4);
	}
	else
		why = mime + ", not decodable";
	stbi_image_free(decoded);
	return why;
}

// Halved, each 2 x 2 pixels averaged (an odd edge's last pixel twice), till no side is over maxSide
// (without pixels: its size alone).
void fitImage(GlbImage& image, int maxSide)
{
	while (image.width > maxSide || image.height > maxSide)
	{
		const int w = std::max(1, image.width / 2), h = std::max(1, image.height / 2);
		if (image.rgba.empty())
		{
			image.width = w;
			image.height = h;
			continue;
		}
		std::vector<u8> half((size_t)w * h * 4);
		auto at = [&](int x, int y, int c) {
			return (unsigned)image.rgba[((size_t)std::min(y, image.height - 1) * image.width + std::min(x, image.width - 1)) * 4 + c];
		};
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
				for (int c = 0; c < 4; c++)
					half[((size_t)y * w + x) * 4 + c] = (u8)((at(x * 2, y * 2, c) + at(x * 2 + 1, y * 2, c)
							+ at(x * 2, y * 2 + 1, c) + at(x * 2 + 1, y * 2 + 1, c) + 2) / 4);
		image.width = w;
		image.height = h;
		image.rgba = std::move(half);
	}
}

const cgltf_accessor *attribute(const cgltf_primitive& prim, cgltf_attribute_type type, int index = 0)
{
	for (cgltf_size i = 0; i < prim.attributes_count; i++)
		if (prim.attributes[i].type == type && prim.attributes[i].index == index)
			return prim.attributes[i].data;
	return nullptr;
}

void addPrimitive(const cgltf_primitive& prim, const glm::mat4& world, Reader& reader, const char *meshName)
{
	GlbMesh& out = reader.out;
	if (prim.type != cgltf_primitive_type_triangles && prim.type != cgltf_primitive_type_triangle_strip
			&& prim.type != cgltf_primitive_type_triangle_fan)
		return;
	// positions and indices read from a buffer view, not sparse (else left out)
	const cgltf_accessor *position = attribute(prim, cgltf_attribute_type_position);
	// (an offset past its view is refused here: cgltf_validate's own sum can wrap)
	if (position == nullptr || position->type != cgltf_type_vec3 || position->count == 0
			|| position->buffer_view == nullptr || position->is_sparse || position->offset > position->buffer_view->size)
		return;
	if (prim.indices != nullptr && (prim.indices->buffer_view == nullptr || prim.indices->is_sparse
			|| prim.indices->offset > prim.indices->buffer_view->size))
		return;
	const size_t vertexCount = position->count;
	const size_t count = prim.indices != nullptr ? prim.indices->count : vertexCount;
	const size_t triangles = prim.type == cgltf_primitive_type_triangles ? count / 3 : count >= 3 ? count - 2 : 0;
	// compared without adding or multiplying, so a huge count (e.g. "count": -1) can't wrap past the
	// caps; the totals so far never exceed them
	if (vertexCount > MaxVertices - out.positions.size() / 3 || count > MaxIndices
			|| triangles > (MaxIndices - out.indices.size()) / 3)
	{
		reader.tooBig = true;
		return;
	}
	const cgltf_accessor *normal = attribute(prim, cgltf_attribute_type_normal);
	if (normal != nullptr && (normal->type != cgltf_type_vec3 || normal->count != position->count))
		normal = nullptr;
	const cgltf_accessor *uv = attribute(prim, cgltf_attribute_type_texcoord);
	if (uv != nullptr && (uv->type != cgltf_type_vec2 || uv->count != position->count))
		uv = nullptr;
	// the vertices' own colours (kept once a primitive has them: the vertices before them white)
	const cgltf_accessor *color = attribute(prim, cgltf_attribute_type_color);
	if (color != nullptr && ((color->type != cgltf_type_vec3 && color->type != cgltf_type_vec4) || color->count != position->count))
		color = nullptr;
	if (color != nullptr && out.colors.empty())
		out.colors.assign(out.positions.size() / 3 * 4, 1.f);
	// this primitive's base colour image (KHR_texture_basisu's when it has no other): one of the
	// mesh's images if it decodes and fewer than maxImages came before it
	const cgltf_image *image = nullptr;
	if (prim.material != nullptr && prim.material->has_pbr_metallic_roughness)
	{
		const cgltf_texture_view& view = prim.material->pbr_metallic_roughness.base_color_texture;
		if (view.texture != nullptr && view.texcoord == 0)
			image = view.texture->image != nullptr ? view.texture->image : view.texture->basisu_image;
		else if (view.texture != nullptr && out.untextured.empty())
			out.untextured = "a texture on TEXCOORD_" + std::to_string(view.texcoord);
	}
	int imageIndex = -1;
	if (image != nullptr)
	{
		const auto tried = reader.imageAt.find(image);
		if (tried != reader.imageAt.end())
			imageIndex = tried->second;
		else if ((int)out.images.size() < reader.maxImages)
		{
			GlbImage decoded;
			const std::string why = loadImage(image, decoded, reader.pixels);
			if (why.empty())
			{
				if (reader.fitSide > 0)
					fitImage(decoded, reader.fitSide);
				imageIndex = (int)out.images.size();
				out.images.push_back(std::move(decoded));
			}
			else if (out.untextured.empty())
				out.untextured = why;
			reader.imageAt[image] = imageIndex;
		}
	}
	const bool textured = imageIndex >= 0;

	const u32 base = (u32)(out.positions.size() / 3);
	const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(world)));
	for (size_t i = 0; i < vertexCount; i++)
	{
		float p[3] = {}, n[3] = {}, t[2] = {};
		cgltf_accessor_read_float(position, i, p, 3);
		const glm::vec3 wp = glm::vec3(world * glm::vec4(p[0], p[1], p[2], 1.f));
		out.positions.insert(out.positions.end(), { wp.x, wp.y, wp.z });
		glm::vec3 wn(0.f);
		if (normal != nullptr && cgltf_accessor_read_float(normal, i, n, 3))
		{
			wn = normalMatrix * glm::vec3(n[0], n[1], n[2]);
			const float len = glm::length(wn);
			wn = len > 1e-12f ? wn / len : glm::vec3(0.f);
		}
		out.normals.insert(out.normals.end(), { wn.x, wn.y, wn.z });
		if (textured && uv != nullptr)
			cgltf_accessor_read_float(uv, i, t, 2);
		out.uvs.insert(out.uvs.end(), { t[0], t[1] });
		if (!out.colors.empty())
		{
			float c[4] { 1.f, 1.f, 1.f, 1.f };
			if (color != nullptr)
				cgltf_accessor_read_float(color, i, c, color->type == cgltf_type_vec4 ? 4 : 3);
			out.colors.insert(out.colors.end(), { c[0], c[1], c[2], c[3] });
		}
	}

	// the primitive's vertex order (indexed or not), as triangles
	auto vertex = [&](size_t k) -> size_t {
		return prim.indices != nullptr ? cgltf_accessor_read_index(prim.indices, k) : k;
	};
	const size_t firstIndex = out.indices.size();
	auto triangle = [&](size_t a, size_t b, size_t c) {
		if (a >= vertexCount || b >= vertexCount || c >= vertexCount)
			return;		// (an index past the primitive's vertices: left out)
		out.indices.insert(out.indices.end(), { base + (u32)a, base + (u32)b, base + (u32)c });
	};
	if (prim.type == cgltf_primitive_type_triangles)
		for (size_t k = 0; k + 2 < count; k += 3)
			triangle(vertex(k), vertex(k + 1), vertex(k + 2));
	else if (prim.type == cgltf_primitive_type_triangle_strip)
		for (size_t k = 0; k + 2 < count; k++)
			k % 2 == 0 ? triangle(vertex(k), vertex(k + 1), vertex(k + 2)) : triangle(vertex(k + 1), vertex(k), vertex(k + 2));
	else
		for (size_t k = 1; k + 1 < count; k++)
			triangle(vertex(0), vertex(k), vertex(k + 1));
	if (out.indices.size() > firstIndex)
	{
		GlbPart part;
		part.firstIndex = (u32)firstIndex;
		part.indexCount = (u32)(out.indices.size() - firstIndex);
		part.image = imageIndex;
		part.meshName = meshName != nullptr ? meshName : "";
		part.materialName = prim.material != nullptr && prim.material->name != nullptr ? prim.material->name : "";
		if (prim.material != nullptr && prim.material->has_pbr_metallic_roughness)
			for (int k = 0; k < 4; k++)
				part.color[k] = prim.material->pbr_metallic_roughness.base_color_factor[k];
		out.parts.push_back(part);
	}

	if (normal == nullptr)
	{
		// face normals, summed at the vertices
		for (size_t k = firstIndex; k < out.indices.size(); k += 3)
		{
			const u32 *tri = &out.indices[k];
			const glm::vec3 a = glm::make_vec3(&out.positions[tri[0] * 3]);
			const glm::vec3 b = glm::make_vec3(&out.positions[tri[1] * 3]);
			const glm::vec3 c = glm::make_vec3(&out.positions[tri[2] * 3]);
			const glm::vec3 fn = glm::cross(b - a, c - a);
			for (int v = 0; v < 3; v++)
				for (int j = 0; j < 3; j++)
					out.normals[tri[v] * 3 + j] += fn[j];
		}
		for (size_t v = base; v < base + vertexCount; v++)
		{
			glm::vec3 n = glm::make_vec3(&out.normals[v * 3]);
			const float len = glm::length(n);
			n = len > 1e-20f ? n / len : glm::vec3(0.f, 1.f, 0.f);
			for (int j = 0; j < 3; j++)
				out.normals[v * 3 + j] = n[j];
		}
	}
}

void addNode(const cgltf_node *node, Reader& reader, int depth)
{
	if (depth > 64 || reader.tooBig)
		return;		// (no cycles in a valid file; cgltf_validate checks)
	if (node->mesh != nullptr)
	{
		float world[16];
		cgltf_node_transform_world(node, world);
		const glm::mat4 m = glm::make_mat4(world);
		for (cgltf_size i = 0; i < node->mesh->primitives_count && !reader.tooBig; i++)
			addPrimitive(node->mesh->primitives[i], m, reader, node->mesh->name != nullptr ? node->mesh->name : node->name);
	}
	for (cgltf_size i = 0; i < node->children_count; i++)
		addNode(node->children[i], reader, depth + 1);
}

}	// namespace

bool loadGlb(const u8 *data, size_t size, GlbMesh& out, std::string& error, int maxImages, int fitSide, bool pixels)
{
	out = GlbMesh();
	if (data == nullptr || size < 12 || memcmp(data, "glTF", 4) != 0)
	{
		error = "not a GLB file";
		return false;
	}
	cgltf_options options {};
	options.type = cgltf_file_type_glb;
	cgltf_data *gltf = nullptr;
	cgltf_result r = cgltf_parse(&options, data, size, &gltf);
	if (r == cgltf_result_success)
		// the GLB's own binary chunk and data: URIs only (no file path: no files are read)
		r = cgltf_load_buffers(&options, gltf, nullptr);
	if (r == cgltf_result_success)
		r = cgltf_validate(gltf);
	if (r != cgltf_result_success)
	{
		error = std::string("glTF: ") + resultName(r);
		cgltf_free(gltf);
		return false;
	}
	Reader reader { out, std::max(maxImages, 0), fitSide, pixels };
	const cgltf_scene *scene = gltf->scene != nullptr ? gltf->scene : gltf->scenes_count > 0 ? &gltf->scenes[0] : nullptr;
	if (scene != nullptr)
	{
		for (cgltf_size i = 0; i < scene->nodes_count; i++)
			addNode(scene->nodes[i], reader, 0);
	}
	else
	{
		// no scene: every root node
		for (cgltf_size i = 0; i < gltf->nodes_count; i++)
			if (gltf->nodes[i].parent == nullptr)
				addNode(&gltf->nodes[i], reader, 0);
	}
	cgltf_free(gltf);
	if (reader.tooBig || out.indices.empty())
	{
		error = reader.tooBig ? "over 1000000 vertices or 3000000 indices" : "no triangles";
		out = GlbMesh();
		return false;
	}
	if (!out.images.empty())
		out.untextured.clear();
	else if (out.untextured.empty())
		out.untextured = "no base colour texture";
	return true;
}

}
