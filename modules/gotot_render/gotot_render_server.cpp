#include "gotot_render_server.h"

#include "core/os/memory.h"
#include "core/string/print_string.h"
#include "servers/rendering/rendering_server.h"

GototRenderServer *GototRenderServer::server_singleton = nullptr;

namespace {
// GOTOT-004: uniform/UBO data filled on every gpu_scene_set_camera and uploaded to view_ubo.
// Mirrors the GLSL ViewBlock (std140): mat4 vp + mat4 view + vec4 planes[6] + vec4 viewport
// + uint occ_count + float far_plane + 2 pads = 256 bytes, column-major matrices.
struct GototViewData {
	float vp[16];
	float view[16];
	float planes[6][4];
	float viewport[4]; // x = viewport_w, y = viewport_h, z = hzb_texel_count, w = tan_half_fov_v
	uint32_t occ_count;
	float far_plane;
	uint32_t hzb_valid;
	float pad1;
};
// Relocatable deterministinc instance generation for the GPU Scene prototype.
// index -> transform (position + scale) + AABB bounds + instance id.
const char *gpu_scene_compute_glsl = R"(
#version 450
#extension GL_EXT_samplerless_texture_functions : enable

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform GototSceneParams {
	uint instance_count;
	uint seed;
	float spread;
	uint pad;
}
params;

layout(std430, set = 0, binding = 0) buffer TransformBuffer {
	vec4 position_scale[];
}
transforms;

layout(std430, set = 0, binding = 1) buffer BoundsBuffer {
	vec4 bounds[];
}
bounds_data;

layout(std430, set = 0, binding = 2) buffer InstanceIdBuffer {
	uint ids[];
}
instance_ids;

uint hash_uint(uint x) {
	x = (x >> 16u) ^ (x * 0x45d9f3bu);
	x = (x >> 16u) ^ (x * 0x45d9f3bu);
	x = (x >> 16u) ^ x;
	return x;
}

float rand01(inout uint s) {
	s = s * 1664525u + 1013904223u;
	return float(s >> 8u) / 16777216.0;
}

void main() {
	uint i = gl_GlobalInvocationID.x;
	if (i >= params.instance_count) {
		return;
	}

	uint s = i * 2654435761u + hash_uint(params.seed);

	float x = (rand01(s) - 0.5) * params.spread;
	float y = (rand01(s) - 0.5) * params.spread;
	float z = (rand01(s) - 0.5) * params.spread;
	float scale = 0.5 + rand01(s) * 1.5;

	vec3 pos = vec3(x, y, z);
	transforms.position_scale[i] = vec4(pos, scale);

	vec3 half_ext = vec3(scale);
	bounds_data.bounds[i * 2u + 0u] = vec4(pos - half_ext, 1.0);
	bounds_data.bounds[i * 2u + 1u] = vec4(pos + half_ext, 1.0);

	instance_ids.ids[i] = i;
}
)";

// GOTOT-002 + GOTOT-004: frustum + HZB occlusion in one pass. Planes and view
// data come from the ViewData UBO (set 0 binding 5); only instance_count is pushed.
const char *gpu_cull_compute_glsl = R"(
#version 450
#extension GL_EXT_samplerless_texture_functions : enable

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform CullParams {
	uint instance_count;
	uint pad0;
	uint pad1;
	uint pad2;
}
params;

layout(std430, set = 0, binding = 0) buffer TransformBuffer {
	vec4 position_scale[];
}
transforms;

layout(std430, set = 0, binding = 1) buffer VisibilityBuffer {
	uint visible[];
}
visibility;

layout(std430, set = 0, binding = 2) buffer CountBuffer {
	uint count;
}
counter;

layout(std430, set = 0, binding = 3) buffer CompactBuffer {
	uint row[];
}
compact;

layout(set = 0, binding = 4) uniform utexture2DArray hzb_sampler;

layout(std140, set = 0, binding = 5) uniform ViewBlock {
	mat4 vp;
	mat4 view;
	vec4 planes[6];
	vec4 viewport; // x = viewport_w, y = viewport_h, z = hzb_texel_count, w = tan_half_fov_v
	uint occ_count;
	float far_plane;
	uint hzb_valid;
	float pad1;
}
viewdata;

void main() {
	uint i = gl_GlobalInvocationID.x;
	if (i >= params.instance_count) {
		return;
	}

	vec4 ts = transforms.position_scale[i];
	vec3 center = ts.xyz;
	float radius = ts.w;

	uint vis = 1u;
	for (uint p = 0u; p < 6u; p++) {
		vec4 pl = viewdata.planes[p];
		float d = dot(pl.xyz, center) + pl.w;
		if (d < -radius) {
			vis = 0u;
			break;
		}
	}

	// HZB occlusion test (only if the frustum test passed and the HZB is fresh).
	if (vis == 1u && viewdata.hzb_valid != 0u) {
		vec3 vc = (viewdata.view * vec4(center, 1.0)).xyz;
		float z_view = -vc.z;
		float r_px = (radius * 0.5 * viewdata.viewport.y) / (viewdata.viewport.w * max(z_view, 0.0001));
		if (r_px >= 1.0) {
			vec4 clip = viewdata.vp * vec4(center, 1.0);
			vec2 ndc = clip.xy / clip.w;
			vec2 px = (ndc * 0.5 + 0.5) * viewdata.viewport.xy;
			float logt = ceil(log2(r_px));
			int level = clamp(int(logt), 0, int(log2(viewdata.viewport.z)));
			float scale = exp2(float(level));
			int texels = int(viewdata.viewport.z) >> level;
			vec2 uv = clamp(px / scale, vec2(0.0), vec2(float(texels - 1)));
			ivec2 t = ivec2(uv);
			uint sphere_inv = floatBitsToUint(max(viewdata.far_plane - (z_view - radius), 0.0));
			uint max_inv = 0u;
			for (int dy = 0; dy <= 1; dy++) {
				for (int dx = 0; dx <= 1; dx++) {
					ivec2 tc = clamp(t + ivec2(dx, dy), ivec2(0), ivec2(texels - 1));
					max_inv = max(max_inv, texelFetch(hzb_sampler, ivec3(tc, level), 0).r);
				}
			}
			if (max_inv > sphere_inv) {
				vis = 0u;
			}
		}
	}

	visibility.visible[i] = vis;
	if (vis == 1u) {
		uint slot = atomicAdd(counter.count, 1u);
		compact.row[slot] = i;
	}
}
)";

// GOTOT-004: clear HZB layer 0.
const char *gpu_hzb_clear_compute_glsl = R"(
#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(push_constant, std430) uniform ClearParams {
	uint texel_count;
	uint pad0;
	uint pad1;
	uint pad2;
}
params;

layout(set = 0, binding = 0) uniform writeonly uimage2DArray hzb_img;

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (px.x < int(params.texel_count) && px.y < int(params.texel_count)) {
		imageStore(hzb_img, ivec3(px, 0), uvec4(0u));
	}
}
)";

// GOTOT-004: rasterize occluder boxes (projected to the square HZB grid) into layer 0.
// Depth is inverted (far - view_z); imageAtomicMax keeps the nearest depth.
const char *gpu_hzb_occ_compute_glsl = R"(
#version 450
#extension GL_EXT_samplerless_texture_functions : enable

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, r32ui) uniform uimage2DArray hzb_img;

layout(std430, set = 0, binding = 1) buffer OccMinBuffer {
	vec4 data[];
}
occmin;

layout(std430, set = 0, binding = 2) buffer OccMaxBuffer {
	vec4 data[];
}
occmax;

layout(std140, set = 0, binding = 5) uniform ViewBlock {
	mat4 vp;
	mat4 view;
	vec4 planes[6];
	vec4 viewport; // x = viewport_w, y = viewport_h, z = hzb_texel_count, w = tan_half_fov_v
	uint occ_count;
	float far_plane;
	float pad0;
	float pad1;
}
viewdata;

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	int texel_count = int(viewdata.viewport.z);
	if (px.x >= texel_count || px.y >= texel_count) {
		return;
	}

	for (uint b = 0u; b < viewdata.occ_count; b++) {
		vec3 mn = occmin.data[b].xyz;
		vec3 mx = occmax.data[b].xyz;
		vec3 corners[8] = vec3[](
				mn, vec3(mx.x, mn.y, mn.z), vec3(mn.x, mx.y, mn.z), vec3(mx.x, mx.y, mn.z),
				vec3(mn.x, mn.y, mx.z), vec3(mx.x, mn.y, mx.z), vec3(mn.x, mx.y, mx.z), mx);

		vec2 lo = vec2(1e9);
		vec2 hi = vec2(-1e9);
		float min_z = 1e9;
		bool usable = true;
		for (int k = 0; k < 8; k++) {
			vec4 cl = viewdata.vp * vec4(corners[k], 1.0);
			if (cl.w <= 0.0) {
				usable = false;
				break;
			}
			vec3 vw = (viewdata.view * vec4(corners[k], 1.0)).xyz;
			min_z = min(min_z, -vw.z);
			vec2 ndc = cl.xy / cl.w;
			vec2 p = (ndc * 0.5 + 0.5) * viewdata.viewport.xy;
			lo = min(lo, p);
			hi = max(hi, p);
		}
		if (!usable) {
			continue;
		}

		vec2 fp = vec2(px) + 0.5;
		// Map projected pixel-space rect into the square HZB grid using the SAME
		// mapping the cull pass applies (viewport px -> texel).
		vec2 tscale = vec2(viewdata.viewport.z) / viewdata.viewport.xy;
		if (all(greaterThanEqual(fp, lo * tscale)) && all(lessThanEqual(fp, hi * tscale))) {
			uint dv = floatBitsToUint(viewdata.far_plane - min_z);
			imageAtomicMax(hzb_img, ivec3(px, 0), dv);
		}
	}
}
)";

// GOTOT-004: downsample one HZB level (2x2 max) into the next.
const char *gpu_hzb_down_compute_glsl = R"(
#version 450
#extension GL_EXT_samplerless_texture_functions : enable

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(push_constant, std430) uniform DownParams {
	uint level_size;
	uint src_layer;
	uint dst_layer;
	uint pad;
}
params;

layout(set = 0, binding = 0, r32ui) uniform uimage2DArray hzb_img;

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (px.x >= int(params.level_size) || px.y >= int(params.level_size)) {
		return;
	}
	ivec2 p2 = px * 2;
	uvec4 r = uvec4(0u);
	for (int dy = 0; dy < 2; dy++) {
		for (int dx = 0; dx < 2; dx++) {
			r = max(r, imageLoad(hzb_img, ivec3(p2 + ivec2(dx, dy), int(params.src_layer))));
		}
	}
	imageStore(hzb_img, ivec3(px, int(params.dst_layer)), r);
}
)";

// GOTOT-003: single-thread pass that fills a VkDrawIndexedIndirectCommand.
const char *gpu_drawargs_compute_glsl = R"(
#version 450
#extension GL_EXT_samplerless_texture_functions : enable

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(std430, set = 0, binding = 0) buffer IndirectArgsBlock {
	uint args[5];
}
indirect;

layout(std430, set = 0, binding = 1) buffer CountBuffer {
	uint count;
}
counter;

void main() {
	if (gl_GlobalInvocationID.x != 0u) {
		return;
	}
	uint n = counter.count;
	indirect.args[0] = 6u;      // index_count (01 quad)
	indirect.args[1] = n;       // instance_count (visible)
	indirect.args[2] = 0u;      // first_index
	indirect.args[3] = 0u;      // vertex_offset
	indirect.args[4] = 0u;      // first_instance
}
)";

// GOTOT-005: vertex shader that expands one billboard quad per visible instance.
// The original scene index is fetched from the compacted list (binding 0); the
// instance transform comes from the GPU Scene transform buffer (binding 1).
const char *gpu_raster_vert_glsl = R"(
#version 450

layout(std430, set = 0, binding = 0) buffer CompactBuffer {
	uint row[];
}
compact;

layout(std430, set = 0, binding = 1) buffer TransformBuffer {
	vec4 position_scale[];
}
transforms;

layout(std140, set = 0, binding = 2) uniform ViewBlock {
	mat4 vp;
	mat4 view;
	vec4 planes[6];
	vec4 viewport;
	uint occ_count;
	float far_plane;
	uint hzb_valid;
	float pad1;
}
viewdata;

void main() {
	uint orig = compact.row[gl_InstanceIndex];
	vec4 ts = transforms.position_scale[orig];
	vec3 center = ts.xyz;
	float s = ts.w;

	vec2 c[4] = vec2[4](
			vec2(-1.0, -1.0),
			vec2(1.0, -1.0),
			vec2(1.0, 1.0),
			vec2(-1.0, 1.0));
	vec2 cc = c[gl_VertexIndex];

	vec3 wp = center + vec3(cc.x * s, cc.y * s, 0.0);
	gl_Position = viewdata.vp * vec4(wp, 1.0);
}
)";

// GOTOT-005: magenta billboards, no blending, no depth.
const char *gpu_raster_frag_glsl = R"(
#version 450

layout(location = 0) out vec4 out_color;

void main() {
	out_color = vec4(0.95, 0.18, 0.9, 1.0);
}
)";

// GOTOT-008A: real-mesh draw args. Reuses the exact GOTOT-003 indirect argument
// system/buffer (set 0 binding 0 = args, binding 1 = visible count), but the
// index_count is supplied at dispatch time instead of being hardcoded, so the
// mesh path can generalize later without introducing a mesh table now.
const char *gpu_mesh_drawargs_compute_glsl = R"(
#version 450

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform MeshArgsParams {
	uint index_count;
}
params;

layout(std430, set = 0, binding = 0) buffer IndirectArgsBlock {
	uint args[5];
}
indirect;

layout(std430, set = 0, binding = 1) buffer CountBuffer {
	uint count;
}
counter;

void main() {
	if (gl_GlobalInvocationID.x != 0u) {
		return;
	}
	uint n = counter.count;
	indirect.args[0] = params.index_count; // index_count (real mesh)
	indirect.args[1] = n;                  // instance_count (existing visible count)
	indirect.args[2] = 0u;                 // first_index
	indirect.args[3] = 0u;                 // vertex_offset
	indirect.args[4] = 0u;                 // first_instance
}
)";

// GOTOT-008A: real geometry vertex transform. The original scene index comes
// from the existing compacted list; the position is a REAL vertex attribute
// fetched from a REAL vertex buffer (not a procedural gl_VertexIndex expansion).
const char *gpu_mesh_vert_glsl = R"(
#version 450

layout(location = 0) in vec3 vertex_position;

layout(std430, set = 0, binding = 0) buffer CompactBuffer {
	uint row[];
}
compact;

layout(std430, set = 0, binding = 1) buffer TransformBuffer {
	vec4 position_scale[];
}
transforms;

layout(std140, set = 0, binding = 2) uniform ViewBlock {
	mat4 vp;
	mat4 view;
	vec4 planes[6];
	vec4 viewport;
	uint occ_count;
	float far_plane;
	uint hzb_valid;
	float pad1;
}
viewdata;

void main() {
	uint orig = compact.row[gl_InstanceIndex];
	vec4 ts = transforms.position_scale[orig];
	vec3 world = ts.xyz + vertex_position * ts.w;
	gl_Position = viewdata.vp * vec4(world, 1.0);
}
)";

// GOTOT-008A: flat green real geometry (visually distinct from the magenta
// billboard path). No lighting, no materials, no textures.
const char *gpu_mesh_frag_glsl = R"(
#version 450

layout(location = 0) out vec4 out_color;

void main() {
	out_color = vec4(0.15, 0.85, 0.35, 1.0);
}
)";
} // namespace

void GototRenderServer::_bind_methods() {
	ClassDB::bind_static_method("GototRenderServer", D_METHOD("get_server_singleton"), &GototRenderServer::get_server_singleton);
	ClassDB::bind_method(D_METHOD("initialize"), &GototRenderServer::initialize);
	ClassDB::bind_method(D_METHOD("shutdown"), &GototRenderServer::shutdown);
	ClassDB::bind_method(D_METHOD("is_initialized"), &GototRenderServer::is_initialized);
	ClassDB::bind_method(D_METHOD("ensure_gpu_device"), &GototRenderServer::ensure_gpu_device);
	ClassDB::bind_method(D_METHOD("is_gpu_ready"), &GototRenderServer::is_gpu_ready);

	ClassDB::bind_method(D_METHOD("gpu_scene_create", "instance_count", "spread"), &GototRenderServer::gpu_scene_create);
	ClassDB::bind_method(D_METHOD("gpu_scene_dispatch", "seed"), &GototRenderServer::gpu_scene_dispatch);
	ClassDB::bind_method(D_METHOD("gpu_scene_readback_positions", "index", "count"), &GototRenderServer::gpu_scene_readback_positions);
	ClassDB::bind_method(D_METHOD("gpu_scene_readback_scales", "index", "count"), &GototRenderServer::gpu_scene_readback_scales);
	ClassDB::bind_method(D_METHOD("gpu_scene_stats"), &GototRenderServer::gpu_scene_stats);
	ClassDB::bind_method(D_METHOD("gpu_scene_get_instance_count"), &GototRenderServer::gpu_scene_get_instance_count);
	ClassDB::bind_method(D_METHOD("gpu_scene_destroy"), &GototRenderServer::gpu_scene_destroy);

	ClassDB::bind_method(D_METHOD("gpu_scene_set_camera", "camera_transform", "projection"), &GototRenderServer::gpu_scene_set_camera);
	ClassDB::bind_method(D_METHOD("gpu_cull_dispatch"), &GototRenderServer::gpu_cull_dispatch);
	ClassDB::bind_method(D_METHOD("gpu_cull_get_visible_count"), &GototRenderServer::gpu_cull_get_visible_count);
	ClassDB::bind_method(D_METHOD("gpu_cull_get_visibility"), &GototRenderServer::gpu_cull_get_visibility);
	ClassDB::bind_method(D_METHOD("gpu_scene_get_frustum_planes"), &GototRenderServer::gpu_scene_get_frustum_planes);

	ClassDB::bind_method(D_METHOD("gpu_drawargs_finalize"), &GototRenderServer::gpu_drawargs_finalize);
	ClassDB::bind_method(D_METHOD("gpu_drawargs_read"), &GototRenderServer::gpu_drawargs_read);
	ClassDB::bind_method(D_METHOD("gpu_compact_read"), &GototRenderServer::gpu_compact_read);

	ClassDB::bind_method(D_METHOD("gpu_scene_set_viewport", "viewport_w", "viewport_h"), &GototRenderServer::gpu_scene_set_viewport);
	ClassDB::bind_method(D_METHOD("gpu_scene_set_occluders", "occluders"), &GototRenderServer::gpu_scene_set_occluders);
	ClassDB::bind_method(D_METHOD("gpu_visibility_dispatch"), &GototRenderServer::gpu_visibility_dispatch);

	ClassDB::bind_method(D_METHOD("gpu_raster_indirect_draw"), &GototRenderServer::gpu_raster_indirect_draw);
	ClassDB::bind_method(D_METHOD("gpu_raster_read_pixels"), &GototRenderServer::gpu_raster_read_pixels);
	ClassDB::bind_method(D_METHOD("gpu_scene_get_vp"), &GototRenderServer::gpu_scene_get_vp);

	ClassDB::bind_method(D_METHOD("gpu_scene_set_instance_transform", "index", "position", "scale"), &GototRenderServer::gpu_scene_set_instance_transform);
	ClassDB::bind_method(D_METHOD("gpu_raster_read_depth"), &GototRenderServer::gpu_raster_read_depth);
	ClassDB::bind_method(D_METHOD("gpu_raster_get_depth_format"), &GototRenderServer::gpu_raster_get_depth_format);
	ClassDB::bind_method(D_METHOD("gpu_mesh_get_depth_enabled"), &GototRenderServer::gpu_mesh_get_depth_enabled);
	ClassDB::bind_method(D_METHOD("gpu_raster_get_depth_enabled"), &GototRenderServer::gpu_raster_get_depth_enabled);

	ClassDB::bind_method(D_METHOD("gpu_mesh_create"), &GototRenderServer::gpu_mesh_create);
	ClassDB::bind_method(D_METHOD("gpu_mesh_drawargs_finalize"), &GototRenderServer::gpu_mesh_drawargs_finalize);
	ClassDB::bind_method(D_METHOD("gpu_mesh_indirect_draw"), &GototRenderServer::gpu_mesh_indirect_draw);
	ClassDB::bind_method(D_METHOD("gpu_mesh_get_index_count"), &GototRenderServer::gpu_mesh_get_index_count);
	ClassDB::bind_method(D_METHOD("gpu_mesh_get_vertex_count"), &GototRenderServer::gpu_mesh_get_vertex_count);
}

GototRenderServer::GototRenderServer() {
}

GototRenderServer::~GototRenderServer() {
	shutdown();
}

void GototRenderServer::set_server_singleton(GototRenderServer *p_server) {
	server_singleton = p_server;
}

GototRenderServer *GototRenderServer::get_server_singleton() {
	return server_singleton;
}

void GototRenderServer::initialize() {
	// The server object is ready from startup. The local RenderingDevice is
	// created lazily (ensure_gpu_device) once the engine's renderer is up,
	// matching how LightmapperRD acquires its device.
	rendering_device = nullptr;
}

bool GototRenderServer::ensure_gpu_device() {
	if (rendering_device != nullptr) {
		return true;
	}

	rendering_device = RenderingServer::get_singleton()->create_local_rendering_device();

	if (rendering_device == nullptr) {
		print_error("[GOTOT-NEXT] Failed to create local RenderingDevice. Requires an RD-based renderer (Forward+ / Mobile).");
		return false;
	}

	print_line("[GOTOT-NEXT] Local RenderingDevice created.");

	return true;
}

void GototRenderServer::_destroy_mesh() {
	if (rendering_device != nullptr) {
		if (mesh_drawargs_uniform_set.is_valid()) {
			rendering_device->free_rid(mesh_drawargs_uniform_set);
			mesh_drawargs_uniform_set = RID();
		}
		if (mesh_uniform_set.is_valid()) {
			rendering_device->free_rid(mesh_uniform_set);
			mesh_uniform_set = RID();
		}
		if (mesh_drawargs_pipeline.is_valid()) {
			rendering_device->free_rid(mesh_drawargs_pipeline);
			mesh_drawargs_pipeline = RID();
		}
		if (mesh_drawargs_shader.is_valid()) {
			rendering_device->free_rid(mesh_drawargs_shader);
			mesh_drawargs_shader = RID();
		}
		if (mesh_pipeline.is_valid()) {
			rendering_device->free_rid(mesh_pipeline);
			mesh_pipeline = RID();
		}
		if (mesh_shader.is_valid()) {
			rendering_device->free_rid(mesh_shader);
			mesh_shader = RID();
		}
		if (mesh_index_array.is_valid()) {
			rendering_device->free_rid(mesh_index_array);
			mesh_index_array = RID();
		}
		if (mesh_vertex_array.is_valid()) {
			rendering_device->free_rid(mesh_vertex_array);
			mesh_vertex_array = RID();
		}
		if (mesh_index_buffer.is_valid()) {
			rendering_device->free_rid(mesh_index_buffer);
			mesh_index_buffer = RID();
		}
		if (mesh_vertex_buffer.is_valid()) {
			rendering_device->free_rid(mesh_vertex_buffer);
			mesh_vertex_buffer = RID();
		}
	}
	mesh_vertex_format = -1;
	mesh_vertex_count = 0;
	mesh_index_count = 0;
	gpu_mesh_valid = false;
}

void GototRenderServer::_destroy_gpu_scene() {
	_destroy_mesh();

	if (rendering_device == nullptr) {
		gpu_scene_valid = false;
		gpu_instance_count = 0;
		return;
	}

	if (cull_uniform_set.is_valid()) {
		rendering_device->free_rid(cull_uniform_set);
		cull_uniform_set = RID();
	}
	if (drawargs_uniform_set.is_valid()) {
		rendering_device->free_rid(drawargs_uniform_set);
		drawargs_uniform_set = RID();
	}
	if (hzb_occ_uniform_set.is_valid()) {
		rendering_device->free_rid(hzb_occ_uniform_set);
		hzb_occ_uniform_set = RID();
	}
	if (hzb_occ_pipeline.is_valid()) {
		rendering_device->free_rid(hzb_occ_pipeline);
		hzb_occ_pipeline = RID();
	}
	if (hzb_occ_shader.is_valid()) {
		rendering_device->free_rid(hzb_occ_shader);
		hzb_occ_shader = RID();
	}
	if (hzb_down_uniform_set.is_valid()) {
		rendering_device->free_rid(hzb_down_uniform_set);
		hzb_down_uniform_set = RID();
	}
	if (raster_uniform_set.is_valid()) {
		rendering_device->free_rid(raster_uniform_set);
		raster_uniform_set = RID();
	}
	if (quad_index_array.is_valid()) {
		rendering_device->free_rid(quad_index_array);
		quad_index_array = RID();
	}
	if (hzb_down_pipeline.is_valid()) {
		rendering_device->free_rid(hzb_down_pipeline);
		hzb_down_pipeline = RID();
	}
	if (hzb_down_shader.is_valid()) {
		rendering_device->free_rid(hzb_down_shader);
		hzb_down_shader = RID();
	}
	if (hzb_clear_uniform_set.is_valid()) {
		rendering_device->free_rid(hzb_clear_uniform_set);
		hzb_clear_uniform_set = RID();
	}
	if (hzb_clear_pipeline.is_valid()) {
		rendering_device->free_rid(hzb_clear_pipeline);
		hzb_clear_pipeline = RID();
	}
	if (hzb_clear_shader.is_valid()) {
		rendering_device->free_rid(hzb_clear_shader);
		hzb_clear_shader = RID();
	}
	if (raster_pipeline.is_valid()) {
		rendering_device->free_rid(raster_pipeline);
		raster_pipeline = RID();
	}
	if (raster_shader.is_valid()) {
		rendering_device->free_rid(raster_shader);
		raster_shader = RID();
	}
	if (quad_index_buffer.is_valid()) {
		rendering_device->free_rid(quad_index_buffer);
		quad_index_buffer = RID();
	}
	if (raster_framebuffer.is_valid()) {
		rendering_device->free_rid(raster_framebuffer);
		raster_framebuffer = RID();
	}
	if (raster_color_texture.is_valid()) {
		rendering_device->free_rid(raster_color_texture);
		raster_color_texture = RID();
	}
	if (raster_depth_texture.is_valid()) {
		rendering_device->free_rid(raster_depth_texture);
		raster_depth_texture = RID();
	}
	raster_framebuffer_format = -1;
	raster_depth_attached = false;
	raster_depth_format_value = -1;
	mesh_depth_enabled = false;
	raster_depth_enabled = false;
	if (view_ubo.is_valid()) {
		rendering_device->free_rid(view_ubo);
		view_ubo = RID();
	}
	if (occluder_max_buffer.is_valid()) {
		rendering_device->free_rid(occluder_max_buffer);
		occluder_max_buffer = RID();
	}
	if (occluder_min_buffer.is_valid()) {
		rendering_device->free_rid(occluder_min_buffer);
		occluder_min_buffer = RID();
	}
	if (hzb_array.is_valid()) {
		rendering_device->free_rid(hzb_array);
		hzb_array = RID();
	}
	if (drawargs_pipeline.is_valid()) {
		rendering_device->free_rid(drawargs_pipeline);
		drawargs_pipeline = RID();
	}
	if (drawargs_shader.is_valid()) {
		rendering_device->free_rid(drawargs_shader);
		drawargs_shader = RID();
	}
	if (indirect_args_buffer.is_valid()) {
		rendering_device->free_rid(indirect_args_buffer);
		indirect_args_buffer = RID();
	}
	if (cull_pipeline.is_valid()) {
		rendering_device->free_rid(cull_pipeline);
		cull_pipeline = RID();
	}
	if (cull_shader.is_valid()) {
		rendering_device->free_rid(cull_shader);
		cull_shader = RID();
	}
	if (compact_buffer.is_valid()) {
		rendering_device->free_rid(compact_buffer);
		compact_buffer = RID();
	}
	if (visible_count_buffer.is_valid()) {
		rendering_device->free_rid(visible_count_buffer);
		visible_count_buffer = RID();
	}
	if (visibility_buffer.is_valid()) {
		rendering_device->free_rid(visibility_buffer);
		visibility_buffer = RID();
	}
	if (uniform_set.is_valid()) {
		rendering_device->free_rid(uniform_set);
		uniform_set = RID();
	}
	if (compute_pipeline.is_valid()) {
		rendering_device->free_rid(compute_pipeline);
		compute_pipeline = RID();
	}
	if (compute_shader.is_valid()) {
		rendering_device->free_rid(compute_shader);
		compute_shader = RID();
	}
	if (instance_id_buffer.is_valid()) {
		rendering_device->free_rid(instance_id_buffer);
		instance_id_buffer = RID();
	}
	if (bounds_buffer.is_valid()) {
		rendering_device->free_rid(bounds_buffer);
		bounds_buffer = RID();
	}
	if (transform_buffer.is_valid()) {
		rendering_device->free_rid(transform_buffer);
		transform_buffer = RID();
	}

	gpu_scene_valid = false;
	gpu_cull_valid = false;
	gpu_drawargs_valid = false;
	gpu_hzb_valid = false;
	gpu_raster_valid = false;
	camera_view_valid = false;
	frustum_valid = false;
	gpu_instance_count = 0;
	occluder_count = 0;
}

void GototRenderServer::shutdown() {
	if (rendering_device == nullptr) {
		return;
	}

	_destroy_gpu_scene();

	memdelete(rendering_device);
	rendering_device = nullptr;

	print_line("[GOTOT-NEXT] Local RenderingDevice destroyed.");
}

bool GototRenderServer::is_initialized() const {
	return true;
}

bool GototRenderServer::is_gpu_ready() const {
	return rendering_device != nullptr;
}

bool GototRenderServer::gpu_scene_create(int p_instance_count, float p_spread) {
	if (!ensure_gpu_device()) {
		return false;
	}

	if (p_instance_count <= 0 || p_instance_count > 100000000) {
		print_error("[GOTOT-NEXT] gpu_scene_create: instance count must be in (0, 100000000].");
		return false;
	}

	_destroy_gpu_scene();

	int count = p_instance_count;
	int64_t transform_bytes = (int64_t)count * 16;
	int64_t bounds_bytes = (int64_t)count * 32;
	int64_t id_bytes = (int64_t)count * 4;

	String compile_error;
	Vector<uint8_t> spirv = rendering_device->shader_compile_spirv_from_source(
			RD::SHADER_STAGE_COMPUTE, String(gpu_scene_compute_glsl), RD::SHADER_LANGUAGE_GLSL, &compile_error);
	if (spirv.is_empty()) {
		print_error("[GOTOT-NEXT] gpu_scene_create: compute shader compile failed:");
		print_error(compile_error);
		return false;
	}

	RD::ShaderStageSPIRVData stage;
	stage.shader_stage = RD::SHADER_STAGE_COMPUTE;
	stage.spirv = spirv;
	Vector<RD::ShaderStageSPIRVData> stages;
	stages.push_back(stage);

	compute_shader = rendering_device->shader_create_from_spirv(stages, "gotot_gpu_scene");
	if (compute_shader.is_null()) {
		print_error("[GOTOT-NEXT] gpu_scene_create: shader_create_from_spirv failed.");
		return false;
	}

	transform_buffer = rendering_device->storage_buffer_create((uint32_t)transform_bytes);
	bounds_buffer = rendering_device->storage_buffer_create((uint32_t)bounds_bytes);
	instance_id_buffer = rendering_device->storage_buffer_create((uint32_t)id_bytes);

	compute_pipeline = rendering_device->compute_pipeline_create(compute_shader);

	Vector<RD::Uniform> uniforms;
	const RID fill_buffers[3] = { transform_buffer, bounds_buffer, instance_id_buffer };
	for (uint32_t b = 0; b < 3; b++) {
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = b;
		u.append_id(fill_buffers[b]);
		uniforms.push_back(u);
	}

	uniform_set = rendering_device->uniform_set_create(uniforms, compute_shader, 0);
	if (uniform_set.is_null()) {
		print_error("[GOTOT-NEXT] gpu_scene_create: uniform_set_create failed.");
		_destroy_gpu_scene();
		return false;
	}

	// GOTOT-002: culling resources.
	String cull_error;
	Vector<uint8_t> cull_spirv = rendering_device->shader_compile_spirv_from_source(
			RD::SHADER_STAGE_COMPUTE, String(gpu_cull_compute_glsl), RD::SHADER_LANGUAGE_GLSL, &cull_error);
	if (cull_spirv.is_empty()) {
		print_error("[GOTOT-NEXT] gpu_scene_create: cull shader compile failed:");
		print_error(cull_error);
		_destroy_gpu_scene();
		return false;
	}

	RD::ShaderStageSPIRVData cull_stage;
	cull_stage.shader_stage = RD::SHADER_STAGE_COMPUTE;
	cull_stage.spirv = cull_spirv;
	Vector<RD::ShaderStageSPIRVData> cull_stages;
	cull_stages.push_back(cull_stage);

	cull_shader = rendering_device->shader_create_from_spirv(cull_stages, "gotot_gpu_cull");
	if (cull_shader.is_null()) {
		print_error("[GOTOT-NEXT] gpu_scene_create: cull shader_create_from_spirv failed.");
		_destroy_gpu_scene();
		return false;
	}

	visibility_buffer = rendering_device->storage_buffer_create((uint32_t)count * 4);
	visible_count_buffer = rendering_device->storage_buffer_create(4);
	compact_buffer = rendering_device->storage_buffer_create((uint32_t)count * 4);

	cull_pipeline = rendering_device->compute_pipeline_create(cull_shader);

	// GOTOT-004: HZB resources (hierarchical depth pyramid of occluders).
	RD::TextureFormat hzb_format;
	hzb_format.format = RD::DATA_FORMAT_R32_UINT;
	hzb_format.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
	hzb_format.width = HZB_TEXEL_COUNT;
	hzb_format.height = HZB_TEXEL_COUNT;
	hzb_format.depth = 1;
	hzb_format.array_layers = HZB_LEVELS;
	hzb_format.mipmaps = 1;
	hzb_format.samples = RD::TEXTURE_SAMPLES_1;
	hzb_format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
	hzb_array = rendering_device->texture_create(hzb_format, RD::TextureView());

	occluder_min_buffer = rendering_device->storage_buffer_create(64 * 16);
	occluder_max_buffer = rendering_device->storage_buffer_create(64 * 16);

	view_ubo = rendering_device->uniform_buffer_create(sizeof(GototViewData));

	if (!_create_hzb_passes()) {
		_destroy_gpu_scene();
		return false;
	}

	Vector<RD::Uniform> cull_uniforms;

	for (uint32_t b = 0; b < 4; b++) {
		RD::Uniform cu;
		cu.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		cu.binding = b;
		if (b == 0) {
			cu.append_id(transform_buffer);
		} else if (b == 1) {
			cu.append_id(visibility_buffer);
		} else if (b == 2) {
			cu.append_id(visible_count_buffer);
		} else {
			cu.append_id(compact_buffer);
		}
		cull_uniforms.push_back(cu);
	}

	RD::Uniform cu_hzb;
	cu_hzb.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
	cu_hzb.binding = 4;
	cu_hzb.append_id(hzb_array);
	cull_uniforms.push_back(cu_hzb);

	RD::Uniform cu_view;
	cu_view.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
	cu_view.binding = 5;
	cu_view.append_id(view_ubo);
	cull_uniforms.push_back(cu_view);

	cull_uniform_set = rendering_device->uniform_set_create(cull_uniforms, cull_shader, 0);
	if (cull_uniform_set.is_null()) {
		print_error("[GOTOT-NEXT] gpu_scene_create: cull uniform_set_create failed.");
		_destroy_gpu_scene();
		return false;
	}

	gpu_cull_valid = true;
	gpu_hzb_valid = true;

	// GOTOT-003: indirect draw args resources.
	String drawargs_error;
	Vector<uint8_t> drawargs_spirv = rendering_device->shader_compile_spirv_from_source(
			RD::SHADER_STAGE_COMPUTE, String(gpu_drawargs_compute_glsl), RD::SHADER_LANGUAGE_GLSL, &drawargs_error);
	if (drawargs_spirv.is_empty()) {
		print_error("[GOTOT-NEXT] gpu_scene_create: drawargs shader compile failed:");
		print_error(drawargs_error);
		_destroy_gpu_scene();
		return false;
	}

	RD::ShaderStageSPIRVData drawargs_stage;
	drawargs_stage.shader_stage = RD::SHADER_STAGE_COMPUTE;
	drawargs_stage.spirv = drawargs_spirv;
	Vector<RD::ShaderStageSPIRVData> drawargs_stages;
	drawargs_stages.push_back(drawargs_stage);

	drawargs_shader = rendering_device->shader_create_from_spirv(drawargs_stages, "gotot_gpu_drawargs");
	if (drawargs_shader.is_null()) {
		print_error("[GOTOT-NEXT] gpu_scene_create: drawargs shader_create_from_spirv failed.");
		_destroy_gpu_scene();
		return false;
	}

	// GOTOT-005: same buffer also feeds VkDrawIndexedIndirect (needs INDIRECT usage).
	indirect_args_buffer = rendering_device->storage_buffer_create(
			20, Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	drawargs_pipeline = rendering_device->compute_pipeline_create(drawargs_shader);

	Vector<RD::Uniform> drawargs_uniforms;

	RD::Uniform da_u0;
	da_u0.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	da_u0.binding = 0;
	da_u0.append_id(indirect_args_buffer);
	drawargs_uniforms.push_back(da_u0);

	RD::Uniform da_u1;
	da_u1.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	da_u1.binding = 1;
	da_u1.append_id(visible_count_buffer);
	drawargs_uniforms.push_back(da_u1);

	drawargs_uniform_set = rendering_device->uniform_set_create(drawargs_uniforms, drawargs_shader, 0);
	if (drawargs_uniform_set.is_null()) {
		print_error("[GOTOT-NEXT] gpu_scene_create: drawargs uniform_set_create failed.");
		_destroy_gpu_scene();
		return false;
	}

	// GOTOT-005: indirect draw pipeline, quad index buffer, offscreen target, framebuffer.
	Vector<uint32_t> quad_indices;
	quad_indices.push_back(0);
	quad_indices.push_back(1);
	quad_indices.push_back(2);
	quad_indices.push_back(2);
	quad_indices.push_back(3);
	quad_indices.push_back(0);
	Vector<uint8_t> quad_index_bytes;
	quad_index_bytes.resize(quad_indices.size() * 4);
	memcpy(quad_index_bytes.ptrw(), quad_indices.ptr(), quad_indices.size() * 4);
	quad_index_buffer = rendering_device->index_buffer_create(6, RD::INDEX_BUFFER_FORMAT_UINT32, quad_index_bytes);
	quad_index_array = rendering_device->index_array_create(quad_index_buffer, 0, 6);

	if (!_create_raster_pipeline()) {
		_destroy_gpu_scene();
		return false;
	}

	gpu_raster_valid = true;
	gpu_drawargs_valid = true;
	gpu_instance_count = count;
	gpu_scene_spread = p_spread;
	gpu_scene_valid = true;

	print_line("[GOTOT-NEXT] GPU Scene created. instances=" + itos(count) +
			" spread=" + String::num(p_spread) +
			" buffers=" + String::num((transform_bytes + bounds_bytes + id_bytes) / 1024.0 / 1024.0, 2) + " MB");

	return true;
}

bool GototRenderServer::gpu_scene_dispatch(int p_seed) {
	if (!gpu_scene_valid) {
		print_error("[GOTOT-NEXT] gpu_scene_dispatch: no scene. Call gpu_scene_create first.");
		return false;
	}

	struct PushParams {
		uint32_t instance_count;
		uint32_t seed;
		float spread;
		uint32_t pad;
	};

	PushParams params;
	params.instance_count = (uint32_t)gpu_instance_count;
	params.seed = (uint32_t)p_seed;
	params.spread = gpu_scene_spread;
	params.pad = 0;

	uint32_t groups = (uint32_t)(((gpu_instance_count - 1) / 64) + 1);

	RD::ComputeListID list = rendering_device->compute_list_begin();
	rendering_device->compute_list_bind_compute_pipeline(list, compute_pipeline);
	rendering_device->compute_list_bind_uniform_set(list, uniform_set, 0);
	rendering_device->compute_list_set_push_constant(list, &params, sizeof(params));
	rendering_device->compute_list_dispatch(list, groups, 1, 1);
	rendering_device->compute_list_end();

	rendering_device->submit();
	rendering_device->sync();

	return true;
}

PackedVector3Array GototRenderServer::gpu_scene_readback_positions(int p_index, int p_count) {
	PackedVector3Array ret;
	if (!gpu_scene_valid || p_index < 0 || p_count <= 0 || p_index + p_count > gpu_instance_count) {
		return ret;
	}

	Vector<uint8_t> bytes = rendering_device->buffer_get_data(
			transform_buffer, (uint32_t)(p_index * 16), (uint32_t)(p_count * 16));

	int instances = bytes.size() / 16;
	ret.resize(instances);
	const float *fptr = (const float *)bytes.ptr();
	for (int i = 0; i < instances; i++) {
		ret.set(i, Vector3(fptr[i * 4 + 0], fptr[i * 4 + 1], fptr[i * 4 + 2]));
	}
	return ret;
}

PackedFloat32Array GototRenderServer::gpu_scene_readback_scales(int p_index, int p_count) {
	PackedFloat32Array ret;
	if (!gpu_scene_valid || p_index < 0 || p_count <= 0 || p_index + p_count > gpu_instance_count) {
		return ret;
	}

	Vector<uint8_t> bytes = rendering_device->buffer_get_data(
			transform_buffer, (uint32_t)(p_index * 16), (uint32_t)(p_count * 16));

	int instances = bytes.size() / 16;
	ret.resize(instances);
	const float *fptr = (const float *)bytes.ptr();
	for (int i = 0; i < instances; i++) {
		ret.set(i, fptr[i * 4 + 3]);
	}
	return ret;
}

Dictionary GototRenderServer::gpu_scene_stats() {
	Dictionary d;
	d["valid"] = gpu_scene_valid;
	d["instance_count"] = gpu_instance_count;
	d["transform_bytes"] = (int64_t)gpu_instance_count * 16;
	d["bounds_bytes"] = (int64_t)gpu_instance_count * 32;
	d["ids_bytes"] = (int64_t)gpu_instance_count * 4;
	d["total_bytes"] = (int64_t)gpu_instance_count * (16 + 32 + 4);
	d["spread"] = gpu_scene_spread;
	return d;
}

int GototRenderServer::gpu_scene_get_instance_count() const {
	return gpu_scene_valid ? gpu_instance_count : 0;
}

void GototRenderServer::gpu_scene_destroy() {
	_destroy_gpu_scene();
	print_line("[GOTOT-NEXT] GPU Scene destroyed.");
}

void GototRenderServer::gpu_scene_set_camera(const Transform3D &p_camera_transform, const Projection &p_projection) {
	Vector<Plane> planes = p_projection.get_projection_planes(p_camera_transform);
	for (int i = 0; i < 6; i++) {
		frustum_planes[i] = planes[i];
	}
	frustum_valid = true;

	if (rendering_device == nullptr || !gpu_hzb_valid) {
		return;
	}

	// GOTOT-004: fill and upload the ViewData UBO (VP + view + planes + viewport + far).
	Projection cam_view(p_camera_transform.inverse());
	Projection vp = p_projection * cam_view;

	GototViewData vd;
	memset(&vd, 0, sizeof(vd));
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			vd.vp[c * 4 + r] = vp.columns[c][r];
			vd.view[c * 4 + r] = cam_view.columns[c][r];
			last_vp[c * 4 + r] = vd.vp[c * 4 + r];
		}
	}
	for (int i = 0; i < 6; i++) {
		vd.planes[i][0] = frustum_planes[i].normal.x;
		vd.planes[i][1] = frustum_planes[i].normal.y;
		vd.planes[i][2] = frustum_planes[i].normal.z;
		vd.planes[i][3] = frustum_planes[i].d;
	}
	vd.viewport[0] = hzb_viewport_w;
	vd.viewport[1] = hzb_viewport_h;
	vd.viewport[2] = (float)HZB_TEXEL_COUNT;
	vd.viewport[3] = _projection_tan_half_fov_v(p_projection);
	vd.occ_count = (uint32_t)occluder_count;
	vd.far_plane = p_projection.get_z_far();
	vd.hzb_valid = 0;

	rendering_device->buffer_update(view_ubo, 0, sizeof(GototViewData), &vd);
	camera_view_valid = true;
}

bool GototRenderServer::gpu_cull_dispatch() {
	if (!gpu_scene_valid || !gpu_cull_valid) {
		print_error("[GOTOT-NEXT] gpu_cull_dispatch: no gpu scene. Call gpu_scene_create first.");
		return false;
	}
	if (!frustum_valid) {
		print_error("[GOTOT-NEXT] gpu_cull_dispatch: no camera. Call gpu_scene_set_camera first.");
		return false;
	}

	struct CullPushParams {
		uint32_t instance_count;
		uint32_t pad0;
		uint32_t pad1;
		uint32_t pad2;
	};

	CullPushParams params;
	params.instance_count = (uint32_t)gpu_instance_count;
	params.pad0 = 0;
	params.pad1 = 0;
	params.pad2 = 0;

	rendering_device->buffer_clear(visible_count_buffer, 0, 4);

	// Frustum-only mode: disable the HZB occlusion test for this dispatch.
	if (gpu_hzb_valid) {
		uint32_t zero = 0;
		rendering_device->buffer_update(view_ubo, offsetof(GototViewData, hzb_valid), 4, &zero);
	}

	uint32_t groups = (uint32_t)(((gpu_instance_count - 1) / 64) + 1);

	_run_compute_pass(cull_pipeline, cull_uniform_set, &params, sizeof(params), groups, 1, 1);

	return true;
}

int GototRenderServer::gpu_cull_get_visible_count() {
	if (!gpu_scene_valid || !gpu_cull_valid) {
		return -1;
	}
	Vector<uint8_t> bytes = rendering_device->buffer_get_data(visible_count_buffer, 0, 4);
	if (bytes.size() != 4) {
		return -1;
	}
	uint32_t count = 0;
	memcpy(&count, bytes.ptr(), 4);
	return (int)count;
}

PackedInt32Array GototRenderServer::gpu_cull_get_visibility() {
	PackedInt32Array ret;
	if (!gpu_scene_valid || !gpu_cull_valid) {
		return ret;
	}
	Vector<uint8_t> bytes = rendering_device->buffer_get_data(visibility_buffer, 0, (uint32_t)gpu_instance_count * 4);
	int flags = bytes.size() / 4;
	ret.resize(flags);
	const uint32_t *fptr = (const uint32_t *)bytes.ptr();
	for (int i = 0; i < flags; i++) {
		ret.set(i, (int32_t)fptr[i]);
	}
	return ret;
}

PackedVector4Array GototRenderServer::gpu_scene_get_frustum_planes() {
	PackedVector4Array ret;
	if (!frustum_valid) {
		return ret;
	}
	ret.resize(6);
	for (int i = 0; i < 6; i++) {
		ret.set(i, Vector4(frustum_planes[i].normal.x, frustum_planes[i].normal.y, frustum_planes[i].normal.z, frustum_planes[i].d));
	}
	return ret;
}

bool GototRenderServer::gpu_drawargs_finalize() {
	if (!gpu_scene_valid || !gpu_drawargs_valid) {
		print_error("[GOTOT-NEXT] gpu_drawargs_finalize: no gpu scene. Call gpu_scene_create first.");
		return false;
	}

	RD::ComputeListID list = rendering_device->compute_list_begin();
	rendering_device->compute_list_bind_compute_pipeline(list, drawargs_pipeline);
	rendering_device->compute_list_bind_uniform_set(list, drawargs_uniform_set, 0);
	rendering_device->compute_list_dispatch(list, 1, 1, 1);
	rendering_device->compute_list_end();

	rendering_device->submit();
	rendering_device->sync();

	return true;
}

PackedInt32Array GototRenderServer::gpu_drawargs_read() {
	PackedInt32Array ret;
	if (!gpu_scene_valid || !gpu_drawargs_valid) {
		return ret;
	}
	Vector<uint8_t> bytes = rendering_device->buffer_get_data(indirect_args_buffer, 0, 20);
	if (bytes.size() != 20) {
		return ret;
	}
	ret.resize(5);
	const uint32_t *fptr = (const uint32_t *)bytes.ptr();
	for (int i = 0; i < 5; i++) {
		ret.set(i, (int32_t)fptr[i]);
	}
	return ret;
}

PackedInt32Array GototRenderServer::gpu_compact_read() {
	PackedInt32Array ret;
	if (!gpu_scene_valid || !gpu_cull_valid) {
		return ret;
	}
	int visible = gpu_cull_get_visible_count();
	if (visible <= 0) {
		return ret;
	}
	Vector<uint8_t> bytes = rendering_device->buffer_get_data(compact_buffer, 0, (uint32_t)visible * 4);
	int ids = bytes.size() / 4;
	ret.resize(ids);
	const uint32_t *fptr = (const uint32_t *)bytes.ptr();
	for (int i = 0; i < ids; i++) {
		ret.set(i, (int32_t)fptr[i]);
	}
	return ret;
}

void GototRenderServer::gpu_scene_set_viewport(float p_viewport_w, float p_viewport_h) {
	hzb_viewport_w = p_viewport_w;
	hzb_viewport_h = p_viewport_h;
}

void GototRenderServer::gpu_scene_set_occluders(const Vector<Vector4> &p_occluders) {
	if (!gpu_hzb_valid || p_occluders.is_empty()) {
		occluder_count = 0;
		return;
	}
	int pairs = p_occluders.size() / 2;
	if (pairs > 64) {
		pairs = 64;
	}
	Vector<Vector4> min_pts;
	Vector<Vector4> max_pts;
	min_pts.resize(pairs);
	max_pts.resize(pairs);
	for (int i = 0; i < pairs; i++) {
		min_pts.set(i, p_occluders[i * 2]);
		max_pts.set(i, p_occluders[i * 2 + 1]);
	}
	rendering_device->buffer_update(occluder_min_buffer, 0, (uint32_t)pairs * 16, min_pts.ptr());
	rendering_device->buffer_update(occluder_max_buffer, 0, (uint32_t)pairs * 16, max_pts.ptr());
	occluder_count = pairs;
}

bool GototRenderServer::_create_hzb_passes() {
	String error;

	auto compile_pass = [&](const char *p_glsl, const char *p_name, RID &r_shader) -> bool {
		Vector<uint8_t> spirv = rendering_device->shader_compile_spirv_from_source(
				RD::SHADER_STAGE_COMPUTE, String(p_glsl), RD::SHADER_LANGUAGE_GLSL, &error);
		if (spirv.is_empty()) {
			print_error(String("[GOTOT-NEXT] ") + p_name + " shader compile failed:");
			print_error(error);
			return false;
		}
		RD::ShaderStageSPIRVData stage;
		stage.shader_stage = RD::SHADER_STAGE_COMPUTE;
		stage.spirv = spirv;
		Vector<RD::ShaderStageSPIRVData> stages;
		stages.push_back(stage);
		r_shader = rendering_device->shader_create_from_spirv(stages, p_name);
		return r_shader.is_valid();
	};

	// Clear pass: uimage2DArray binding 0.
	if (!compile_pass(gpu_hzb_clear_compute_glsl, "gotot_hzb_clear", hzb_clear_shader)) {
		return false;
	}
	hzb_clear_pipeline = rendering_device->compute_pipeline_create(hzb_clear_shader);
	Vector<RD::Uniform> clear_uniforms;
	RD::Uniform cu_img;
	cu_img.uniform_type = RD::UNIFORM_TYPE_IMAGE;
	cu_img.binding = 0;
	cu_img.append_id(hzb_array);
	clear_uniforms.push_back(cu_img);
	hzb_clear_uniform_set = rendering_device->uniform_set_create(clear_uniforms, hzb_clear_shader, 0);
	if (hzb_clear_uniform_set.is_null()) {
		print_error("[GOTOT-NEXT] hzb_clear uniform_set_create failed.");
		return false;
	}

	// Occluder pass: image binding 0, occ min/max storage buffers 1/2, view UBO 5.
	if (!compile_pass(gpu_hzb_occ_compute_glsl, "gotot_hzb_occ", hzb_occ_shader)) {
		return false;
	}
	hzb_occ_pipeline = rendering_device->compute_pipeline_create(hzb_occ_shader);
	Vector<RD::Uniform> occ_uniforms;
	RD::Uniform o_u0;
	o_u0.uniform_type = RD::UNIFORM_TYPE_IMAGE;
	o_u0.binding = 0;
	o_u0.append_id(hzb_array);
	occ_uniforms.push_back(o_u0);
	RD::Uniform o_u1;
	o_u1.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	o_u1.binding = 1;
	o_u1.append_id(occluder_min_buffer);
	occ_uniforms.push_back(o_u1);
	RD::Uniform o_u2;
	o_u2.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	o_u2.binding = 2;
	o_u2.append_id(occluder_max_buffer);
	occ_uniforms.push_back(o_u2);
	RD::Uniform o_u5;
	o_u5.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
	o_u5.binding = 5;
	o_u5.append_id(view_ubo);
	occ_uniforms.push_back(o_u5);
	hzb_occ_uniform_set = rendering_device->uniform_set_create(occ_uniforms, hzb_occ_shader, 0);
	if (hzb_occ_uniform_set.is_null()) {
		print_error("[GOTOT-NEXT] hzb_occ uniform_set_create failed.");
		return false;
	}

	// Downsample pass: image binding 0.
	if (!compile_pass(gpu_hzb_down_compute_glsl, "gotot_hzb_down", hzb_down_shader)) {
		return false;
	}
	hzb_down_pipeline = rendering_device->compute_pipeline_create(hzb_down_shader);
	Vector<RD::Uniform> down_uniforms;
	RD::Uniform d_u0;
	d_u0.uniform_type = RD::UNIFORM_TYPE_IMAGE;
	d_u0.binding = 0;
	d_u0.append_id(hzb_array);
	down_uniforms.push_back(d_u0);
	hzb_down_uniform_set = rendering_device->uniform_set_create(down_uniforms, hzb_down_shader, 0);
	if (hzb_down_uniform_set.is_null()) {
		print_error("[GOTOT-NEXT] hzb_down uniform_set_create failed.");
		return false;
	}

	return true;
}

void GototRenderServer::_run_compute_pass(RID p_pipeline, RID p_uniform_set, const void *p_push_data, uint32_t p_push_size, uint32_t p_groups_x, uint32_t p_groups_y, uint32_t p_groups_z) {
	RD::ComputeListID list = rendering_device->compute_list_begin();
	rendering_device->compute_list_bind_compute_pipeline(list, p_pipeline);
	rendering_device->compute_list_bind_uniform_set(list, p_uniform_set, 0);
	if (p_push_data != nullptr && p_push_size > 0) {
		rendering_device->compute_list_set_push_constant(list, p_push_data, p_push_size);
	}
	rendering_device->compute_list_dispatch(list, p_groups_x, p_groups_y, p_groups_z);
	rendering_device->compute_list_end();
	rendering_device->submit();
	rendering_device->sync();
}

float GototRenderServer::_projection_tan_half_fov_v(const Projection &p_projection) {
	// For a perspective matrix, column[1].y == cotangent(v_fov / 2).
	float f = p_projection.columns[1][1];
	if (fabsf(f) < 1e-6f) {
		return 0.0f;
	}
	return 1.0f / f;
}

bool GototRenderServer::gpu_visibility_dispatch() {
	if (!gpu_scene_valid || !gpu_cull_valid || !gpu_hzb_valid) {
		print_error("[GOTOT-NEXT] gpu_visibility_dispatch: no gpu scene. Call gpu_scene_create first.");
		return false;
	}
	if (!frustum_valid || !camera_view_valid) {
		print_error("[GOTOT-NEXT] gpu_visibility_dispatch: no camera. Call gpu_scene_set_camera first.");
		return false;
	}

	struct ClearParams {
		uint32_t texel_count;
		uint32_t pad0;
		uint32_t pad1;
		uint32_t pad2;
	};
	ClearParams clear_params;
	clear_params.texel_count = HZB_TEXEL_COUNT;
	clear_params.pad0 = 0;
	clear_params.pad1 = 0;
	clear_params.pad2 = 0;
	uint32_t clear_groups = HZB_TEXEL_COUNT / 8;

	struct DownParams {
		uint32_t level_size;
		uint32_t src_layer;
		uint32_t dst_layer;
		uint32_t pad;
	};
	DownParams down_params;

	// Refresh UBO fields that are set outside gpu_scene_set_camera: HZB freshness
	// flag and the current occluder count.
	uint32_t hzb_one = 1;
	rendering_device->buffer_update(view_ubo, offsetof(GototViewData, hzb_valid), 4, &hzb_one);
	int32_t occ_count = occluder_count;
	rendering_device->buffer_update(view_ubo, offsetof(GototViewData, occ_count), 4, &occ_count);
	rendering_device->buffer_clear(visible_count_buffer, 0, 4);

	// 1) Clear HZB layer 0.
	_run_compute_pass(hzb_clear_pipeline, hzb_clear_uniform_set, &clear_params, sizeof(clear_params), clear_groups, clear_groups, 1);
	// 2) Rasterize occluders into layer 0 (skipped if no occluders registered).
	if (occluder_count > 0) {
		_run_compute_pass(hzb_occ_pipeline, hzb_occ_uniform_set, nullptr, 0, clear_groups, clear_groups, 1);
	}
	// 3) Downsample into levels 1..N-1 (each level in its own pass for correct barriers).
	for (int level = 1; level < HZB_LEVELS; level++) {
		int size = HZB_TEXEL_COUNT >> level;
		down_params.level_size = (uint32_t)size;
		down_params.src_layer = (uint32_t)(level - 1);
		down_params.dst_layer = (uint32_t)level;
		down_params.pad = 0;
		uint32_t groups = MAX((size + 7) / 8, 1);
		_run_compute_pass(hzb_down_pipeline, hzb_down_uniform_set, &down_params, sizeof(down_params), groups, groups, 1);
	}

	// 4) Cull (frustum + HZB occlusion + compaction).
	struct CullPushParams {
		uint32_t instance_count;
		uint32_t pad0;
		uint32_t pad1;
		uint32_t pad2;
	};
	CullPushParams cull_params;
	cull_params.instance_count = (uint32_t)gpu_instance_count;
	cull_params.pad0 = 0;
	cull_params.pad1 = 0;
	cull_params.pad2 = 0;
	uint32_t groups = (uint32_t)(((gpu_instance_count - 1) / 64) + 1);
	_run_compute_pass(cull_pipeline, cull_uniform_set, &cull_params, sizeof(cull_params), groups, 1, 1);

	return true;
}

bool GototRenderServer::_create_raster_pipeline() {
	String error;

	Vector<uint8_t> vert_spirv = rendering_device->shader_compile_spirv_from_source(
			RD::SHADER_STAGE_VERTEX, String(gpu_raster_vert_glsl), RD::SHADER_LANGUAGE_GLSL, &error);
	if (vert_spirv.is_empty()) {
		print_error("[GOTOT-NEXT] raster vertex shader compile failed:");
		print_error(error);
		return false;
	}

	Vector<uint8_t> frag_spirv = rendering_device->shader_compile_spirv_from_source(
			RD::SHADER_STAGE_FRAGMENT, String(gpu_raster_frag_glsl), RD::SHADER_LANGUAGE_GLSL, &error);
	if (frag_spirv.is_empty()) {
		print_error("[GOTOT-NEXT] raster fragment shader compile failed:");
		print_error(error);
		return false;
	}

	Vector<RD::ShaderStageSPIRVData> stages;
	RD::ShaderStageSPIRVData vs;
	vs.shader_stage = RD::SHADER_STAGE_VERTEX;
	vs.spirv = vert_spirv;
	stages.push_back(vs);
	RD::ShaderStageSPIRVData fs;
	fs.shader_stage = RD::SHADER_STAGE_FRAGMENT;
	fs.spirv = frag_spirv;
	stages.push_back(fs);

	raster_shader = rendering_device->shader_create_from_spirv(stages, "gotot_raster");
	if (raster_shader.is_null()) {
		print_error("[GOTOT-NEXT] raster shader_create_from_spirv failed.");
		return false;
	}

	RD::TextureFormat cf;
	cf.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	cf.width = RASTER_TARGET_W;
	cf.height = RASTER_TARGET_H;
	cf.depth = 1;
	cf.texture_type = RD::TEXTURE_TYPE_2D;
	cf.usage_bits = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	raster_color_texture = rendering_device->texture_create(cf, RD::TextureView());
	if (raster_color_texture.is_null()) {
		print_error("[GOTOT-NEXT] raster color texture_create failed.");
		return false;
	}

	// GOTOT-009: real depth attachment, D32_SFLOAT, cleared to 1.0 (far) at the
	// start of every frame by the draw list flags (DRAW_CLEAR_DEPTH).
	RD::TextureFormat df;
	df.format = RD::DATA_FORMAT_D32_SFLOAT;
	df.width = RASTER_TARGET_W;
	df.height = RASTER_TARGET_H;
	df.depth = 1;
	df.texture_type = RD::TEXTURE_TYPE_2D;
	df.usage_bits = RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	raster_depth_texture = rendering_device->texture_create(df, RD::TextureView());
	if (raster_depth_texture.is_null()) {
		print_error("[GOTOT-NEXT] raster depth texture_create failed.");
		return false;
	}

	Vector<RD::AttachmentFormat> afs;
	RD::AttachmentFormat af;
	af.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	af.samples = RD::TEXTURE_SAMPLES_1;
	af.usage_flags = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	afs.push_back(af);
	RD::AttachmentFormat df_af;
	df_af.format = RD::DATA_FORMAT_D32_SFLOAT;
	df_af.samples = RD::TEXTURE_SAMPLES_1;
	df_af.usage_flags = RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	afs.push_back(df_af);
	raster_framebuffer_format = rendering_device->framebuffer_format_create(afs);
	if (raster_framebuffer_format < 0) {
		print_error("[GOTOT-NEXT] raster framebuffer_format_create failed.");
		return false;
	}

	Vector<RID> attachments;
	attachments.push_back(raster_color_texture);
	attachments.push_back(raster_depth_texture);
	raster_framebuffer = rendering_device->framebuffer_create(attachments, raster_framebuffer_format);
	if (raster_framebuffer.is_null()) {
		print_error("[GOTOT-NEXT] raster framebuffer_create failed.");
		return false;
	}
	raster_depth_attached = true;
	raster_depth_format_value = (int)RD::DATA_FORMAT_D32_SFLOAT;
	raster_depth_enabled = false;

	// No vertex input attributes (procedural gl_VertexIndex expansion) -> INVALID_ID like the engine's blit shaders.
	RD::PipelineRasterizationState rs;
	RD::PipelineMultisampleState ms;
	RD::PipelineDepthStencilState ds;
	RD::PipelineColorBlendState bs = RD::PipelineColorBlendState::create_disabled(1);
	raster_pipeline = rendering_device->render_pipeline_create(
			raster_shader, raster_framebuffer_format, RD::INVALID_ID, RD::RENDER_PRIMITIVE_TRIANGLES, rs, ms, ds, bs, 0, 0);
	if (raster_pipeline.is_null()) {
		print_error("[GOTOT-NEXT] raster render_pipeline_create failed.");
		return false;
	}

	Vector<RD::Uniform> uniforms;
	RD::Uniform u0;
	u0.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	u0.binding = 0;
	u0.append_id(compact_buffer);
	uniforms.push_back(u0);
	RD::Uniform u1;
	u1.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	u1.binding = 1;
	u1.append_id(transform_buffer);
	uniforms.push_back(u1);
	RD::Uniform u2;
	u2.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
	u2.binding = 2;
	u2.append_id(view_ubo);
	uniforms.push_back(u2);

	raster_uniform_set = rendering_device->uniform_set_create(uniforms, raster_shader, 0);
	if (raster_uniform_set.is_null()) {
		print_error("[GOTOT-NEXT] raster uniform_set_create failed.");
		return false;
	}

	print_line("[GOTOT-NEXT] Raster framebuffer: color R8G8B8A8 + depth D32_SFLOAT attached=" +
			itos((int)raster_depth_attached));

	return true;
}

// GOTOT-008A: builds the real-mesh pipeline + uniform set. Uses a REAL vertex
// format so the vertex shader reads a REAL vertex attribute from a vertex buffer.
bool GototRenderServer::_create_mesh_pipeline() {
	String error;

	Vector<uint8_t> vert_spirv = rendering_device->shader_compile_spirv_from_source(
			RD::SHADER_STAGE_VERTEX, String(gpu_mesh_vert_glsl), RD::SHADER_LANGUAGE_GLSL, &error);
	if (vert_spirv.is_empty()) {
		print_error("[GOTOT-NEXT] mesh vertex shader compile failed:");
		print_error(error);
		return false;
	}

	Vector<uint8_t> frag_spirv = rendering_device->shader_compile_spirv_from_source(
			RD::SHADER_STAGE_FRAGMENT, String(gpu_mesh_frag_glsl), RD::SHADER_LANGUAGE_GLSL, &error);
	if (frag_spirv.is_empty()) {
		print_error("[GOTOT-NEXT] mesh fragment shader compile failed:");
		print_error(error);
		return false;
	}

	Vector<RD::ShaderStageSPIRVData> stages;
	RD::ShaderStageSPIRVData vs;
	vs.shader_stage = RD::SHADER_STAGE_VERTEX;
	vs.spirv = vert_spirv;
	stages.push_back(vs);
	RD::ShaderStageSPIRVData fs;
	fs.shader_stage = RD::SHADER_STAGE_FRAGMENT;
	fs.spirv = frag_spirv;
	stages.push_back(fs);

	mesh_shader = rendering_device->shader_create_from_spirv(stages, "gotot_mesh");
	if (mesh_shader.is_null()) {
		print_error("[GOTOT-NEXT] mesh shader_create_from_spirv failed.");
		return false;
	}

	RD::PipelineRasterizationState rs;
	RD::PipelineMultisampleState ms;
	RD::PipelineDepthStencilState ds;
	// GOTOT-009: the REAL MESH path tests and writes depth (LESS_OR_EQUAL,
	// write enabled), depth buffer cleared to 1.0 (far) each frame.
	ds.enable_depth_test = true;
	ds.enable_depth_write = true;
	ds.depth_compare_operator = RD::COMPARE_OP_LESS_OR_EQUAL;
	RD::PipelineColorBlendState bs = RD::PipelineColorBlendState::create_disabled(1);
	mesh_pipeline = rendering_device->render_pipeline_create(
			mesh_shader, raster_framebuffer_format, mesh_vertex_format, RD::RENDER_PRIMITIVE_TRIANGLES, rs, ms, ds, bs, 0, 0);
	if (mesh_pipeline.is_null()) {
		print_error("[GOTOT-NEXT] mesh render_pipeline_create failed.");
		return false;
	}
	mesh_depth_enabled = true;

	Vector<RD::Uniform> uniforms;
	RD::Uniform u0;
	u0.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	u0.binding = 0;
	u0.append_id(compact_buffer);
	uniforms.push_back(u0);
	RD::Uniform u1;
	u1.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	u1.binding = 1;
	u1.append_id(transform_buffer);
	uniforms.push_back(u1);
	RD::Uniform u2;
	u2.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
	u2.binding = 2;
	u2.append_id(view_ubo);
	uniforms.push_back(u2);

	mesh_uniform_set = rendering_device->uniform_set_create(uniforms, mesh_shader, 0);
	if (mesh_uniform_set.is_null()) {
		print_error("[GOTOT-NEXT] mesh uniform_set_create failed.");
		return false;
	}

	return true;
}

bool GototRenderServer::gpu_mesh_create() {
	if (!gpu_scene_valid || !gpu_raster_valid) {
		print_error("[GOTOT-NEXT] gpu_mesh_create: no gpu scene/raster. Call gpu_scene_create first.");
		return false;
	}

	_destroy_mesh();

	// GOTOT-008A: a single real CUBE mesh (positions only). No normals, no UVs,
	// no materials, no textures, no mesh table, no mesh IDs.
	const float cube_positions[8][3] = {
		{ -0.5f, -0.5f, -0.5f },
		{ 0.5f, -0.5f, -0.5f },
		{ 0.5f, 0.5f, -0.5f },
		{ -0.5f, 0.5f, -0.5f },
		{ -0.5f, -0.5f, 0.5f },
		{ 0.5f, -0.5f, 0.5f },
		{ 0.5f, 0.5f, 0.5f },
		{ -0.5f, 0.5f, 0.5f },
	};
	const uint32_t cube_indices[36] = {
		4, 5, 6, 4, 6, 7, // +Z
		1, 0, 3, 1, 3, 2, // -Z
		0, 4, 7, 0, 7, 3, // -X
		5, 1, 2, 5, 2, 6, // +X
		3, 7, 6, 3, 6, 2, // +Y
		0, 1, 5, 0, 5, 4, // -Y
	};

	Vector<uint8_t> vertex_bytes;
	vertex_bytes.resize(sizeof(cube_positions));
	memcpy(vertex_bytes.ptrw(), cube_positions, sizeof(cube_positions));

	Vector<uint8_t> index_bytes;
	index_bytes.resize(sizeof(cube_indices));
	memcpy(index_bytes.ptrw(), cube_indices, sizeof(cube_indices));

	RD::VertexAttribute attr;
	attr.binding = 0;
	attr.location = 0;
	attr.offset = 0;
	attr.format = RD::DATA_FORMAT_R32G32B32_SFLOAT;
	attr.stride = sizeof(float) * 3;
	attr.frequency = RD::VERTEX_FREQUENCY_VERTEX;
	Vector<RD::VertexAttribute> attrs;
	attrs.push_back(attr);
	mesh_vertex_format = rendering_device->vertex_format_create(attrs);
	if (mesh_vertex_format < 0) {
		print_error("[GOTOT-NEXT] gpu_mesh_create: vertex_format_create failed.");
		_destroy_mesh();
		return false;
	}

	mesh_vertex_buffer = rendering_device->vertex_buffer_create((uint32_t)vertex_bytes.size(), vertex_bytes);
	mesh_index_buffer = rendering_device->index_buffer_create(36, RD::INDEX_BUFFER_FORMAT_UINT32, index_bytes);
	if (mesh_vertex_buffer.is_null() || mesh_index_buffer.is_null()) {
		print_error("[GOTOT-NEXT] gpu_mesh_create: vertex/index buffer_create failed.");
		_destroy_mesh();
		return false;
	}

	Vector<RID> src_buffers;
	src_buffers.push_back(mesh_vertex_buffer);
	mesh_vertex_array = rendering_device->vertex_array_create(8, mesh_vertex_format, src_buffers);
	mesh_index_array = rendering_device->index_array_create(mesh_index_buffer, 0, 36);
	if (mesh_vertex_array.is_null() || mesh_index_array.is_null()) {
		print_error("[GOTOT-NEXT] gpu_mesh_create: vertex/index array_create failed.");
		_destroy_mesh();
		return false;
	}

	mesh_vertex_count = 8;
	mesh_index_count = 36;

	if (!_create_mesh_pipeline()) {
		_destroy_mesh();
		return false;
	}

	// GOTOT-008A: mesh indirect draw args. Reuses the exact GOTOT-003 indirect
	// argument system/buffer; a distinct pipeline is used because the mesh path
	// pushes its real index_count instead of hardcoding it.
	String drawargs_error;
	Vector<uint8_t> drawargs_spirv = rendering_device->shader_compile_spirv_from_source(
			RD::SHADER_STAGE_COMPUTE, String(gpu_mesh_drawargs_compute_glsl), RD::SHADER_LANGUAGE_GLSL, &drawargs_error);
	if (drawargs_spirv.is_empty()) {
		print_error("[GOTOT-NEXT] gpu_mesh_create: mesh drawargs shader compile failed:");
		print_error(drawargs_error);
		_destroy_mesh();
		return false;
	}

	RD::ShaderStageSPIRVData da_stage;
	da_stage.shader_stage = RD::SHADER_STAGE_COMPUTE;
	da_stage.spirv = drawargs_spirv;
	Vector<RD::ShaderStageSPIRVData> da_stages;
	da_stages.push_back(da_stage);

	mesh_drawargs_shader = rendering_device->shader_create_from_spirv(da_stages, "gotot_mesh_drawargs");
	if (mesh_drawargs_shader.is_null()) {
		print_error("[GOTOT-NEXT] gpu_mesh_create: mesh drawargs shader_create_from_spirv failed.");
		_destroy_mesh();
		return false;
	}

	mesh_drawargs_pipeline = rendering_device->compute_pipeline_create(mesh_drawargs_shader);

	Vector<RD::Uniform> da_uniforms;
	RD::Uniform da_u0;
	da_u0.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	da_u0.binding = 0;
	da_u0.append_id(indirect_args_buffer);
	da_uniforms.push_back(da_u0);
	RD::Uniform da_u1;
	da_u1.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	da_u1.binding = 1;
	da_u1.append_id(visible_count_buffer);
	da_uniforms.push_back(da_u1);

	mesh_drawargs_uniform_set = rendering_device->uniform_set_create(da_uniforms, mesh_drawargs_shader, 0);
	if (mesh_drawargs_uniform_set.is_null()) {
		print_error("[GOTOT-NEXT] gpu_mesh_create: mesh drawargs uniform_set_create failed.");
		_destroy_mesh();
		return false;
	}

	gpu_mesh_valid = true;
	print_line("[GOTOT-NEXT] GPU Mesh created. vertices=" + itos(mesh_vertex_count) +
			" indices=" + itos(mesh_index_count) +
			" vertex_format=" + itos((int)mesh_vertex_format));
	return true;
}

bool GototRenderServer::gpu_mesh_drawargs_finalize() {
	if (!gpu_scene_valid || !gpu_mesh_valid) {
		print_error("[GOTOT-NEXT] gpu_mesh_drawargs_finalize: no gpu mesh. Call gpu_mesh_create first.");
		return false;
	}

	uint32_t index_count = (uint32_t)mesh_index_count;

	RD::ComputeListID list = rendering_device->compute_list_begin();
	rendering_device->compute_list_bind_compute_pipeline(list, mesh_drawargs_pipeline);
	rendering_device->compute_list_bind_uniform_set(list, mesh_drawargs_uniform_set, 0);
	rendering_device->compute_list_set_push_constant(list, &index_count, sizeof(index_count));
	rendering_device->compute_list_dispatch(list, 1, 1, 1);
	rendering_device->compute_list_end();

	rendering_device->submit();
	rendering_device->sync();

	return true;
}

bool GototRenderServer::gpu_mesh_indirect_draw() {
	if (!gpu_scene_valid || !gpu_mesh_valid) {
		print_error("[GOTOT-NEXT] gpu_mesh_indirect_draw: no gpu mesh. Call gpu_mesh_create first.");
		return false;
	}
	if (!frustum_valid) {
		print_error("[GOTOT-NEXT] gpu_mesh_indirect_draw: no camera. Call gpu_scene_set_camera first.");
		return false;
	}

	Vector<Color> clear_colors;
	clear_colors.push_back(Color(0, 0, 0, 0));
	RD::DrawListID dl = rendering_device->draw_list_begin(raster_framebuffer, RD::DRAW_CLEAR_COLOR_0 | RD::DRAW_CLEAR_DEPTH, clear_colors, 1.0f, 0, Rect2(), 0);
	if (dl == RD::INVALID_ID) {
		print_error("[GOTOT-NEXT] gpu_mesh_indirect_draw: draw_list_begin failed.");
		return false;
	}
	rendering_device->draw_list_bind_render_pipeline(dl, mesh_pipeline);
	rendering_device->draw_list_bind_uniform_set(dl, mesh_uniform_set, 0);
	rendering_device->draw_list_bind_vertex_array(dl, mesh_vertex_array);
	rendering_device->draw_list_bind_index_array(dl, mesh_index_array);
	rendering_device->draw_list_draw_indirect(dl, true, indirect_args_buffer, 0, 1, 0);
	rendering_device->draw_list_end();

	rendering_device->submit();
	rendering_device->sync();

	return true;
}

int GototRenderServer::gpu_mesh_get_index_count() const {
	return mesh_index_count;
}

int GototRenderServer::gpu_mesh_get_vertex_count() const {
	return mesh_vertex_count;
}

bool GototRenderServer::gpu_raster_indirect_draw() {
	if (!gpu_scene_valid || !gpu_raster_valid || !gpu_drawargs_valid) {
		print_error("[GOTOT-NEXT] gpu_raster_indirect_draw: no gpu scene. Call gpu_scene_create first.");
		return false;
	}
	if (!frustum_valid) {
		print_error("[GOTOT-NEXT] gpu_raster_indirect_draw: no camera. Call gpu_scene_set_camera first.");
		return false;
	}

	Vector<Color> clear_colors;
	clear_colors.push_back(Color(0, 0, 0, 0));
	RD::DrawListID dl = rendering_device->draw_list_begin(raster_framebuffer, RD::DRAW_CLEAR_COLOR_0 | RD::DRAW_CLEAR_DEPTH, clear_colors, 1.0f, 0, Rect2(), 0);
	if (dl == RD::INVALID_ID) {
		print_error("[GOTOT-NEXT] gpu_raster_indirect_draw: draw_list_begin failed.");
		return false;
	}
	rendering_device->draw_list_bind_render_pipeline(dl, raster_pipeline);
	rendering_device->draw_list_bind_uniform_set(dl, raster_uniform_set, 0);
	rendering_device->draw_list_bind_index_array(dl, quad_index_array);
	rendering_device->draw_list_draw_indirect(dl, true, indirect_args_buffer, 0, 1, 0);
	rendering_device->draw_list_end();

	rendering_device->submit();
	rendering_device->sync();

	return true;
}

PackedByteArray GototRenderServer::gpu_raster_read_pixels() {
	PackedByteArray ret;
	if (!gpu_scene_valid || !gpu_raster_valid) {
		return ret;
	}
	Vector<uint8_t> data = rendering_device->texture_get_data(raster_color_texture, 0);
	ret.resize(data.size());
	memcpy(ret.ptrw(), data.ptr(), data.size());
	return ret;
}

// GOTOT-009: D32_SFLOAT depth readback (values in [0, 1], clear = 1.0 = far).
PackedFloat32Array GototRenderServer::gpu_raster_read_depth() {
	PackedFloat32Array ret;
	if (!gpu_scene_valid || !gpu_raster_valid) {
		return ret;
	}
	Vector<uint8_t> data = rendering_device->texture_get_data(raster_depth_texture, 0);
	int count = data.size() / 4;
	ret.resize(count);
	memcpy(ret.ptrw(), data.ptr(), data.size());
	return ret;
}

// GOTOT-009: overwrite a single instance's transform (position_scale) in the
// SoA transform buffer. Pure fill helper for the 009 overlay demo; the fill/
// cull/HZB/compaction/drawargs layout is untouched.
void GototRenderServer::gpu_scene_set_instance_transform(int p_index, const Vector3 &p_position, float p_scale) {
	if (!gpu_scene_valid || p_index < 0 || p_index >= gpu_instance_count) {
		return;
	}
	float data[4] = { p_position.x, p_position.y, p_position.z, p_scale };
	Error err = rendering_device->buffer_update(transform_buffer, p_index * sizeof(data), sizeof(data), data);
	if (err != OK) {
		print_error("[GOTOT-NEXT] gpu_scene_set_instance_transform: buffer_update failed.");
	}
}

int GototRenderServer::gpu_raster_get_depth_format() const {
	return raster_depth_format_value;
}

bool GototRenderServer::gpu_mesh_get_depth_enabled() const {
	return mesh_depth_enabled;
}

bool GototRenderServer::gpu_raster_get_depth_enabled() const {
	return raster_depth_enabled;
}

PackedFloat32Array GototRenderServer::gpu_scene_get_vp() {
	PackedFloat32Array ret;
	if (!camera_view_valid) {
		return ret;
	}
	ret.resize(16);
	for (int i = 0; i < 16; i++) {
		ret.set(i, last_vp[i]);
	}
	return ret;
}
