#include "gotot_render_server.h"

#include "core/os/memory.h"
#include "core/io/file_access.h"
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
// GOTOT-012: view-space depth copy for the production pyramid (z_view).
layout(location = 1) out float out_view_z;

void main() {
	out_color = vec4(0.95, 0.18, 0.9, 1.0);
	out_view_z = -1.0 / gl_FragCoord.w;
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
// GOTOT-012: view-space depth copy for the production pyramid (z_view).
layout(location = 1) out float out_view_z;

void main() {
	out_color = vec4(0.15, 0.85, 0.35, 1.0);
	out_view_z = -1.0 / gl_FragCoord.w;
}
)";

// GOTOT-010: CPU<->GPU mesh table descriptor. Mirrors GototMeshDesc (32 bytes,
// std430-compatible: 6x uint32/int32 then 2 reserved uint32). The batch assembly
// compute shader reads it (set 0 binding 1, array of this struct).
struct GototMeshDesc {
	uint32_t index_buffer_slot;
	uint32_t vertex_buffer_slot;
	uint32_t index_count;
	uint32_t vertex_count;
	uint32_t first_index;
	int32_t vertex_offset;
	uint32_t reserved[2];
};

// GOTOT-010: palette-based flat colors per mesh. mesh 0 keeps the 008A/009 green
// so the cube evidence matches earlier milestones; meshes 1..3 use distinct hues.
Color gotot_mesh_palette(int p_mesh_id) {
	switch (p_mesh_id) {
		case 0:
			return Color(0.15f, 0.85f, 0.35f);
		case 1:
			return Color(0.25f, 0.45f, 0.95f);
		case 2:
			return Color(0.95f, 0.45f, 0.15f);
		default:
			return Color(0.9f, 0.82f, 0.2f);
	}
}

// GOTOT-010 pass 1: per-mesh counting. One thread per VISIBLE instance reads its
// mesh_id (from the per-instance mesh_id buffer) and atoms the count for that
// mesh into batch_count[mesh], while staging the original scene index into the
// per-mesh scratch chunk. Ordering within a mesh is irrelevant (7.2: prefix sum
// only), so no sort pass is needed.
const char *gpu_mesh_batch_count_glsl = R"(
#version 450
#extension GL_EXT_samplerless_texture_functions : enable

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform BatchCountParams {
	uint instance_count;
	uint scratch_stride;
	uint pad0;
	uint pad1;
}
params;

layout(std430, set = 0, binding = 0) buffer CompactBuffer {
	uint row[];
}
compact;

layout(std430, set = 0, binding = 1) buffer MeshIdBuffer {
	uint mesh_id[];
}
meshids;

layout(std430, set = 0, binding = 2) buffer BatchCountBuffer {
	uint count[];
}
batch_count;

layout(std430, set = 0, binding = 3) buffer MeshScratchBuffer {
	uint data[];
}
scratch;

void main() {
	uint i = gl_GlobalInvocationID.x;
	if (i >= params.instance_count) {
		return;
	}
	uint orig = compact.row[i];
	uint m = meshids.mesh_id[orig];
	uint local = atomicAdd(batch_count.count[m], 1u);
	scratch.data[m * params.scratch_stride + local] = orig;
}
)";

// GOTOT-011 pass 2: workgroup-parallel prefix sum + batch assembly + grouping.
// One workgroup (64 threads = one per mesh table slot) computes:
//   1. exClusive batch offsets via a Hillis-Steele inclusive scan (the 011
//      replacement for the 010 single-thread loop) - fully deterministic, same
//      output as the old prefix loop wherever grouping is a no-op,
//   2. the concatenated batch_instances[] and the dense per-mesh batch_args[]
//      (identical to 010 for regression byte-compat),
//   3. a group partition (G groups, G = active for PER_MESH, min(active, 5)
//      for GROUPED/REORDERED) plus per-group non-indexed VkDrawIndirectCommand
//      records and an ordered member mesh-id list (batch reorder evidence).
// The instance copy per mesh is done by each mesh's own lane; the group tail is
// finished by lane 0 (fixed loop over <=64 entries, deterministic). The final
// batch_total holds the number of commands to draw (G when merged, else active
// - a GPU-written, frame-varying count consumed by draw_list_draw_indirect).
const char *gpu_mesh_batch_assemble_glsl = R"(
#version 450
#extension GL_EXT_samplerless_texture_functions : enable

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform BatchAssembleParams {
	uint mesh_capacity;
	uint scratch_stride;
	uint strategy;
	uint pad1;
}
params;

struct GototMeshDescStd430 {
	uint index_buffer_slot;
	uint vertex_buffer_slot;
	uint index_count;
	uint vertex_count;
	uint first_index;
	int vertex_offset;
	uint reserved0;
	uint reserved1;
};

layout(std430, set = 0, binding = 0) buffer BatchCountBuffer {
	uint count[];
}
batch_count;

layout(std430, set = 0, binding = 1) buffer MeshTableBlock {
	GototMeshDescStd430 table[64];
}
mesh_table;

layout(std430, set = 0, binding = 2) buffer BatchOffsetBuffer {
	uint offset[];
}
batch_offset;

layout(std430, set = 0, binding = 3) buffer BatchArgsBlock {
	uint args[320];
}
batch_args;

layout(std430, set = 0, binding = 4) buffer BatchInstancesBuffer {
	uint instances[];
}
batch_instances;

layout(std430, set = 0, binding = 5) buffer MeshScratchBuffer {
	uint data[];
}
scratch;

layout(std430, set = 0, binding = 6) buffer BatchTotalBuffer {
	uint total;
}
batch_total;

layout(std430, set = 0, binding = 7) buffer GroupArgsBlock {
	uint args[256];
}
group_args;

layout(std430, set = 0, binding = 8) buffer GroupMemberCountBuffer {
	uint member_count[];
}
group_member_count;

layout(std430, set = 0, binding = 9) buffer GroupMemberListBuffer {
	uint member_list[];
}
group_member_list;

shared uint s_count[64];
shared uint s_scan[64];
shared uint s_buf[64];
shared uint s_actscan[64];
shared uint s_actbuf[64];
shared uint s_actid[64];

void main() {
	uint tid = gl_LocalInvocationID.x;
	uint M = min(params.mesh_capacity, 64u);
	uint c = (tid < M) ? batch_count.count[tid] : 0u;
	s_count[tid] = c;
	s_buf[tid] = (c > 0u) ? 1u : 0u;
	barrier();

	// Workgroup-parallel inclusive prefix sums (Hillis-Steele scan).
	s_scan[tid] = s_count[tid];
	s_actscan[tid] = s_buf[tid];
	barrier();
	for (uint d = 1u; d < 64u; d <<= 1u) {
		uint cv = s_scan[tid];
		uint aa = s_actscan[tid];
		uint pv = (tid >= d) ? s_scan[tid - d] : 0u;
		uint pa = (tid >= d) ? s_actscan[tid - d] : 0u;
		barrier();
		s_buf[tid] = cv + pv;
		s_actbuf[tid] = aa + pa;
		barrier();
		s_scan[tid] = s_buf[tid];
		s_actscan[tid] = s_actbuf[tid];
		barrier();
	}

	if (tid < M) {
		uint excl = (tid > 0u) ? s_scan[tid - 1u] : 0u;
		uint actexcl = (tid > 0u) ? s_actscan[tid - 1u] : 0u;
		batch_offset.offset[tid] = excl;
		if (c > 0u) {
			for (uint k = 0u; k < c; k++) {
				batch_instances.instances[excl + k] = scratch.data[tid * params.scratch_stride + k];
			}
			uint base = actexcl * 5u;
			GototMeshDescStd430 d = mesh_table.table[tid];
			batch_args.args[base + 0u] = d.index_count;
			batch_args.args[base + 1u] = c;
			batch_args.args[base + 2u] = d.first_index;
			batch_args.args[base + 3u] = uint(d.vertex_offset);
			batch_args.args[base + 4u] = excl;
			s_actid[actexcl] = tid;
		}
	}
	barrier();

	if (tid != 0u) {
		return;
	}

	uint actn = (M > 0u) ? s_actscan[M - 1u] : 0u;
	if (actn == 0u) {
		batch_total.total = 0u;
		return;
	}

	uint G = actn;
	if (params.strategy == 1u || params.strategy == 2u) {
		G = min(actn, 5u);
	}
	if (G == 0u) {
		G = 1u;
	}

	uint base = actn / G;
	uint rem = actn % G;

	for (uint g = 0u; g < G; g++) {
		uint size = base + ((g < rem) ? 1u : 0u);
		uint start = g * base + min(g, rem);
		uint inst = 0u;
		uint mvc = 0u;
		for (uint r = 0u; r < size; r++) {
			uint m = s_actid[start + r];
			inst += batch_count.count[m];
			mvc = max(mvc, mesh_table.table[m].index_count);
			group_member_list.member_list[g * 64u + r] = m;
		}
		group_member_count.member_count[g] = size;
		uint first_instance = batch_offset.offset[s_actid[start]];
		uint base4 = g * 4u;
		group_args.args[base4 + 0u] = mvc;
		group_args.args[base4 + 1u] = inst;
		group_args.args[base4 + 2u] = 0u;
		group_args.args[base4 + 3u] = first_instance;
	}

	bool merged = (G < actn);
	batch_total.total = merged ? G : actn;
}
)";

// GOTOT-010: multi-mesh batch vertex shader. gl_InstanceIndex includes the
// VkDrawIndexedIndirectCommand.first_instance base, so batch_instances[]
// (concatenated compact reordered list) maps the instance straight back to its
// original scene index -> transform. The mesh_id is forwarded flat per-instance
// so the fragment stage can pick the per-mesh color.
const char *gpu_mesh_batch_vert_glsl = R"(
#version 450

layout(location = 0) in vec3 vertex_position;

layout(std430, set = 0, binding = 0) buffer BatchInstancesBuffer {
	uint instances[];
}
batch_instances;

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

layout(std430, set = 0, binding = 3) buffer MeshIdBuffer {
	uint mesh_id[];
}
meshids;

layout(location = 1) flat out uint v_mesh_id;

void main() {
	uint orig = batch_instances.instances[gl_InstanceIndex];
	uint m = meshids.mesh_id[orig];
	vec4 ts = transforms.position_scale[orig];
	vec3 world = ts.xyz + vertex_position * ts.w;
	gl_Position = viewdata.vp * vec4(world, 1.0);
	v_mesh_id = m;
}
)";

// GOTOT-010: per-mesh flat color (mesh color table binding 4). GOTOT-011 adds
// early-Z: layout(early_fragment_tests) lets the fixed-function depth/stencil
// reject occluded fragments BEFORE shading (the depth test/write pipeline state
// is unchanged - surviving pixels keep the exact same output/depth, so the 010
// evidence photocopy is unaffected).
const char *gpu_mesh_batch_frag_glsl = R"(
#version 450

layout(early_fragment_tests) in;

layout(location = 1) flat in uint v_mesh_id;

layout(std430, set = 0, binding = 4) buffer MeshColorBuffer {
	vec4 colors[];
}
mesh_colors;

layout(location = 0) out vec4 out_color;
// GOTOT-012: view-space depth copy for the production pyramid (z_view).
layout(location = 1) out float out_view_z;

void main() {
	out_color = mesh_colors.colors[v_mesh_id];
	out_view_z = -1.0 / gl_FragCoord.w;
}
)";

// GOTOT-011: procedural (non-indexed) batch vertex shader used by the GROUPED /
// REORDERED multi-batch draw. One VkDrawIndirectCommand covers every instance of
// every member mesh of a group: command.vertexCount == the largest member
// index_count and each instance picks its OWN sub-range through the mesh table;
// out-of-range vertex indices are pushed outside the clip volume (fully clipped,
// no raster, no depth write). The shared vertex/index data is read as SSBO
// storage mirrors (RD vertex/index buffer owners cannot be bound as storage).
const char *gpu_mesh_group_batch_vert_glsl = R"(
#version 450

struct GototMeshDescStd430 {
	uint index_buffer_slot;
	uint vertex_buffer_slot;
	uint index_count;
	uint vertex_count;
	uint first_index;
	int vertex_offset;
	uint reserved0;
	uint reserved1;
};

layout(std430, set = 0, binding = 0) buffer BatchInstancesBuffer {
	uint instances[];
}
batch_instances;

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

layout(std430, set = 0, binding = 3) buffer MeshIdBuffer {
	uint mesh_id[];
}
meshids;

layout(std430, set = 0, binding = 5) buffer MeshTableBlock {
	GototMeshDescStd430 table[64];
}
mesh_table;

layout(std430, set = 0, binding = 6) buffer VertexDataBuffer {
	float data[];
}
vertex_data;

layout(std430, set = 0, binding = 7) buffer IndexDataBuffer {
	uint data[];
}
index_data;

layout(location = 1) flat out uint v_mesh_id;

void main() {
	uint orig = batch_instances.instances[gl_InstanceIndex];
	uint m = meshids.mesh_id[orig];
	GototMeshDescStd430 d = mesh_table.table[m];
	if (gl_VertexIndex >= d.index_count) {
		gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
		v_mesh_id = m;
		return;
	}
	uint li = index_data.data[d.first_index + gl_VertexIndex];
	int vi = d.vertex_offset + int(li);
	vec3 p = vec3(vertex_data.data[vi * 3 + 0], vertex_data.data[vi * 3 + 1], vertex_data.data[vi * 3 + 2]);
	vec4 ts = transforms.position_scale[orig];
	vec3 world = ts.xyz + p * ts.w;
	gl_Position = viewdata.vp * vec4(world, 1.0);
	v_mesh_id = m;
}
)";

// GOTOT-012: build HZB base layer by sampling the PREVIOUS frame's D32_SFLOAT
// depth. Given A = projection.columns[2][2], B = projection.columns[3][2] the
// Godot 4 perspective maps clip.z = A*z_cam + B, clip.w = -z_cam, so
// ndc_z = -A + B/z_view -> z_view = B / (A + ndc_z) (device depth d -> ndc
// via d*2-1 since the depth attachment is cleared to 1.0 = far). Stored as
// equal tower than the 004 occlusion test. One thread per base texel; viewport (window) size drives
// the square grid mapping and the raster image size drives the depth sample
// UV, so texels map to the SAME screen texels the cull pass tests. The source
// is the R32 view-space-depth color attachment (color-opaque sampling), NOT the
// D32 - sampling a D32 attachment's current version is a no-op in this RDG fork,
// while the R32 coexists with the D32 in the same draw pass and is written by
// the same fragment shaders (out_view_z = -1/gl_FragCoord.w).
const char *gpu_hzb_depth_source_glsl = R"(
#version 450
#ifdef GL_EXT_samplerless_texture_functions
#extension GL_EXT_samplerless_texture_functions : enable
#endif

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(push_constant, std430) uniform DepthSourceParams {
	uint level0_size;
	uint levels;
	float viewport_w;
	float viewport_h;
	float image_w;
	float image_h;
	float far_plane;
}
params;

layout(set = 0, binding = 0) uniform sampler2D depth_tex;
layout(set = 0, binding = 1, r32ui) uniform uimage2DArray hzb_img;
layout(std430, set = 0, binding = 2) buffer ProbeBuffer {
	uint pcount;
	uint pmax_inv;
	uint pd_at_probe;
	uint pndc_at_probe;
}
probe;

// Flat mirror of the pyramid for the occlusion passes (the array layers are for
// CPU readback evidence only). Level L starts at off(L) = sum_{k<L} (size>>k)^2.
layout(std430, set = 0, binding = 3) buffer PyramidDataBuffer {
	uint data[];
}
pyrbuf;

// Builds ALL levels in ONE dispatch by re-sampling the R32 view-z attachment at
// every scale (R32 samplers in the same submission as the draw are the ONE
// reliable GPU read in this RDG fork - imageLoad of a compute-written image is
// always stale). Level texel (x,y) covers the 2^level x 2^level level-0 block;
// the stored value is the max inverted depth over that block's R32 pixels.
void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	int size = int(params.level0_size);
	if (px.x >= size || px.y >= size) {
		return;
	}
	for (int level = 0; level < int(params.levels); level++) {
		int lsize = int(params.level0_size) >> level;
		if (px.x >= lsize || px.y >= lsize) {
			continue;
		}
		int span = 1 << level;
		int bx = px.x << level;
		int by = px.y << level;
		uint best = 0u;
		for (int ay = 0; ay < span; ay++) {
			for (int ax = 0; ax < span; ax++) {
				int lx = bx + ax;
				int ly = by + ay;
				// Texel -> viewport pixel (inverse of the cull mapping pixel*texels/viewport).
				vec2 c = vec2(float(lx), float(ly)) + 0.5;
				vec2 pixel = c * vec2(params.viewport_w, params.viewport_h) / float(params.level0_size);
				// Viewport pixel -> depth image UV (the raster image is a fixed res).
				vec2 uv = clamp(pixel / vec2(params.image_w, params.image_h), vec2(0.0), vec2(1.0));
				float z_view = textureLod(depth_tex, uv, 0.0).r;
				uint inv = floatBitsToUint(max(params.far_plane - z_view, 0.0));
				if (inv > best) {
					best = inv;
				}
			}
		}
		if (level == 0) {
			if (px.x == 864 && px.y == 1024) {
				probe.pd_at_probe = floatBitsToUint(best);
				probe.pndc_at_probe = floatBitsToUint(1.0);
			}
			if (best > 0u) {
				atomicAdd(probe.pcount, 1u);
				atomicMax(probe.pmax_inv, best);
			}
		}
		imageStore(hzb_img, ivec3(px, level), uvec4(best));
		uint poff = 0u;
		for (int k = 0; k < level; k++) {
			uint s = uint(int(params.level0_size) >> k);
			poff += s * s;
		}
		pyrbuf.data[poff + uint(px.x) * uint(lsize) + uint(px.y)] = best;
	}
}
)";

// GOTOT-012 (FINAL pyramid source): OCCLUDER-BOX rasterization. The pyramid is
// built by projecting each registered occluder's world-space AABB into the
// view (matching exactly what 004 proved reliable) and writing its near-face
// inverted depth over every texel of every level that the projected box
// covers - into the FLAT storage buffer the phases read cross-submission (the
// ONLY reliable GPU pipeline in this RDG fork; image/R32 attachment reads and
// compute-image loads are all stale). Conservative (fills the whole AABB) so
// no false dropout. Level texels index the SAME poff layout the phases use.
const char *gpu_hzb_occbuf_glsl = R"(
#version 450

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(std430, set = 0, binding = 0) buffer OccluderMinBuffer {
	vec4 mn[];
}
occmin;

layout(std430, set = 0, binding = 1) buffer OccluderMaxBuffer {
	vec4 mx[];
}
occmax;

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
vd;

layout(std430, set = 0, binding = 3) buffer PyramidDataBuffer {
	uint data[];
}
pyrbuf;

layout(set = 0, binding = 4, r32ui) uniform uimage2DArray hzb_evimg;

void main() {
	if (vd.occ_count == 0u) {
		return;
	}
	for (uint o = 0u; o < vd.occ_count; o++) {
		vec3 bmin = occmin.mn[o].xyz;
		vec3 bmax = occmax.mx[o].xyz;

		// Nearest corner in view space (largest inverted depth = closest).
		float nz = 1e30;
		for (int i = 0; i < 8; i++) {
			vec3 c = mix(bmin, bmax, vec3(float(i & 1), float((i >> 1) & 1), float((i >> 2) & 1)));
			float cz = vd.view[0][2] * c.x + vd.view[1][2] * c.y + vd.view[2][2] * c.z + vd.view[3][2];
			nz = min(nz, -cz);
		}
		uint inv = floatBitsToUint(max(vd.far_plane - nz, 0.0));
		if (inv == 0u) {
			continue;
		}

		// Projected screen AABB.
		vec2 s0 = vec2(1e30);
		vec2 s1 = vec2(-1e30);
		bool behind = false;
		for (int i = 0; i < 8; i++) {
			bool xb = (i & 1) == 1;
			bool yb = ((i >> 1) & 1) == 1;
			bool zb = ((i >> 2) & 1) == 1;
			vec3 c = vec3(xb ? bmax.x : bmin.x, yb ? bmax.y : bmin.y, zb ? bmax.z : bmin.z);
			vec4 clip = vd.vp * vec4(c, 1.0);
			if (clip.w <= 0.0) {
				behind = true;
			} else {
				vec2 ndc = clip.xy / clip.w;
				vec2 px = (ndc * 0.5 + 0.5) * vd.viewport.xy;
				s0 = min(s0, px);
				s1 = max(s1, px);
			}
		}
		if (behind || s1.x <= s0.x || s1.y <= s0.y) {
			continue;
		}

		for (int level = 0; level < 12; level++) {
			uint uv_texels = 2048u >> uint(level);
			uint t0x = uint(clamp(floor(s0.x * float(uv_texels) / vd.viewport.x), 0.0, float(uv_texels - 1)));
			uint t1x = uint(clamp(ceil(s1.x * float(uv_texels) / vd.viewport.x), 0.0, float(uv_texels)));
			uint t0y = uint(clamp(floor(s0.y * float(uv_texels) / vd.viewport.y), 0.0, float(uv_texels - 1)));
			uint t1y = uint(clamp(ceil(s1.y * float(uv_texels) / vd.viewport.y), 0.0, float(uv_texels)));
			uint poff = 0u;
			for (int k = 0; k < level; k++) {
				uint s = 2048u >> uint(k);
				poff += s * s;
			}
			for (uint ty = t0y; ty < t1y; ty++) {
				for (uint tx = t0x; tx < t1x; tx++) {
					uint idx = poff + tx * uv_texels + ty;
					if (inv > pyrbuf.data[idx]) {
						pyrbuf.data[idx] = inv;
					}
					imageStore(hzb_evimg, ivec3(int(tx), int(ty), level), uvec4(inv));
				}
			}
		}
	}
}
)";

// GOTOT-012 phase 1: FRUSTUM-ONLY cull. Every instance is tested against the 6
// frustum planes (no HZB here - the pyramid is consumed by phase 2 only); each
// survivor is appended to the phase-1 list and every instance's visibility flag
// is written. phase2 (running over this list) later decides true survivors.
const char *gpu_hzb_phase1_glsl = R"(
#version 450
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform Phase1Params {
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

layout(std430, set = 0, binding = 1) buffer Phase1CompactBuffer {
	uint row[];
}
phase1;

layout(std430, set = 0, binding = 2) buffer Phase1CountBuffer {
	uint count;
}
p1count;

layout(std430, set = 0, binding = 3) buffer VisibilityBuffer {
	uint visible[];
}
visibility;

layout(std140, set = 0, binding = 5) uniform ViewBlock {
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
	visibility.visible[i] = vis;
	if (vis == 1u) {
		uint slot = atomicAdd(p1count.count, 1u);
		phase1.row[slot] = i;
	}
}
)";

// GOTOT-012 phase 2: HZB occlusion over ALL instances (run inside the same
// submission as the pyramid build). Each instance frustum-checks itself too
// (phase 1 remains as the frustum-only EVIDENCE count). The bounding sphere of
// each instance is projected and tested against the 12-level pyramid exactly
// like the 004 test (2x2 texel max against the level matching the projected
// radius). params.inflate scales the radius conservatively. The pyramid is read
// from the flat STORAGE buffer mirror (buffers are the only reliable GPU reads
// in this RDG fork). Survivors compact into compact[]/visible_count[] - the
// SAME buffers the 011 batch assembler consumes.
const char *gpu_hzb_phase2_glsl = R"(
#version 450

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform Phase2Params {
	uint instance_count;
	float inflate;
	uint pad0;
	uint pad1;
}
params;

layout(std430, set = 0, binding = 0) buffer Phase1CompactBuffer {
	uint row[];
}
phase1;

layout(std430, set = 0, binding = 1) buffer TransformBuffer {
	vec4 position_scale[];
}
transforms;

layout(std430, set = 0, binding = 2) buffer CountBuffer {
	uint count;
}
counter;

layout(std430, set = 0, binding = 3) buffer CompactBuffer {
	uint row[];
}
compact;

layout(std430, set = 0, binding = 4) buffer PyramidDataBuffer {
	uint data[];
}
pyrbuf;

layout(std430, set = 0, binding = 6) buffer DbgProbeBuffer {
	uint pcount;
	uint pmax_inv;
	uint pd_at_probe;
	uint pndc_at_probe;
}
dprobe;

layout(std140, set = 0, binding = 5) uniform ViewBlock {
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
	uint j = gl_GlobalInvocationID.x;
	if (j >= params.instance_count) {
		return;
	}
	vec4 ts = transforms.position_scale[j];
	vec3 center = ts.xyz;
	float radius = ts.w * params.inflate;

	uint vis = 1u;
	for (uint p = 0u; p < 6u; p++) {
		vec4 pl = viewdata.planes[p];
		float d = dot(pl.xyz, center) + pl.w;
		if (d < -radius) {
			vis = 0u;
			break;
		}
	}

	if (vis == 1u && viewdata.hzb_valid != 0u) {
		vec3 vc = (viewdata.view * vec4(center, 1.0)).xyz;
		float z_view = -vc.z;
		float r_px = (radius * 0.5 * viewdata.viewport.y) / (viewdata.viewport.w * max(z_view, 0.0001));
		if (r_px >= 1.0) {
			vec4 clip = viewdata.vp * vec4(center, 1.0);
			vec2 ndc = clip.xy / clip.w;
			// Sample the square 2048-grid the writer (occbuf) projects into:
			// writer texel = frac_ndc * (2048>>L) (the aspect cancels because it
			// scales px by uv_texels/viewport.xy), so the reader must sample
			// frac_ndc * texels, NOT the raster viewport-xy pixels (=0.94 x and
			// 0.53 y factors - guaranteed misses on the y axis).
			float logt = ceil(log2(r_px));
			int level = clamp(int(logt), 0, int(log2(viewdata.viewport.z)));
			float scale = exp2(float(level));
			int texels = int(viewdata.viewport.z) >> level;
			vec2 uv = clamp((ndc * 0.5 + 0.5) * vec2(float(texels)), vec2(0.0), vec2(float(texels - 1)));
			ivec2 t = ivec2(uv);
			float z_sphere = max(z_view - radius, 0.0);
			uint sphere_inv = floatBitsToUint(max(viewdata.far_plane - z_sphere, 0.0));
			uint poff = 0u;
			int base = int(viewdata.viewport.z);
			for (int k = 0; k < level; k++) {
				uint s = uint(base >> k);
				poff += s * s;
			}
			uint max_inv = 0u;
			for (int dy = 0; dy <= 1; dy++) {
				for (int dx = 0; dx <= 1; dx++) {
					ivec2 tc = clamp(t + ivec2(dx, dy), ivec2(0), ivec2(texels - 1));
					max_inv = max(max_inv, pyrbuf.data[poff + uint(tc.x) * uint(texels) + uint(tc.y)]);
				}
			}
			if (max_inv > sphere_inv) {
				vis = 0u;
			}

			// GOTOT-012 debug: dump instance 91's occlusion math into the probe
			// buffer (level, pyramid max_inv, sphere_inv, texel x*2048+y).
			if (j == 91u) {
				dprobe.pcount = uint(level);
				dprobe.pmax_inv = max_inv;
				dprobe.pd_at_probe = sphere_inv;
				dprobe.pndc_at_probe = uint(t.x) * 2048u + uint(t.y);
			}
		}
	}

	if (vis == 1u) {
		uint slot = atomicAdd(counter.count, 1u);
		compact.row[slot] = j;
	}
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

	ClassDB::bind_method(D_METHOD("gpu_hzb_prod_create"), &GototRenderServer::gpu_hzb_prod_create);
	ClassDB::bind_method(D_METHOD("gpu_hzb_build"), &GototRenderServer::gpu_hzb_build);
	ClassDB::bind_method(D_METHOD("gpu_visibility_prod_dispatch"), &GototRenderServer::gpu_visibility_prod_dispatch);
	ClassDB::bind_method(D_METHOD("gpu_hzb_enable_temporal", "enabled"), &GototRenderServer::gpu_hzb_enable_temporal);
	ClassDB::bind_method(D_METHOD("gpu_hzb_set_occluders", "occluders"), &GototRenderServer::gpu_hzb_set_occluders);
	ClassDB::bind_method(D_METHOD("gpu_hzb_get_level_count"), &GototRenderServer::gpu_hzb_get_level_count);
	ClassDB::bind_method(D_METHOD("gpu_hzb_get_phase_counts"), &GototRenderServer::gpu_hzb_get_phase_counts);
	ClassDB::bind_method(D_METHOD("gpu_hzb_get_coherent"), &GototRenderServer::gpu_hzb_get_coherent);
	ClassDB::bind_method(D_METHOD("gpu_hzb_dbg_valid"), &GototRenderServer::gpu_hzb_dbg_valid);
	ClassDB::bind_method(D_METHOD("gpu_hzb_dbg_level0", "x", "y"), &GototRenderServer::gpu_hzb_dbg_level0);
	ClassDB::bind_method(D_METHOD("gpu_hzb_dbg_probe"), &GototRenderServer::gpu_hzb_dbg_probe);
	ClassDB::bind_method(D_METHOD("gpu_hzb_dbg_scan_level0"), &GototRenderServer::gpu_hzb_dbg_scan_level0);
	ClassDB::bind_method(D_METHOD("gpu_hzb_dbg_scan_level1", "level"), &GototRenderServer::gpu_hzb_dbg_scan_level1);
	ClassDB::bind_method(D_METHOD("gpu_hzb_dbg_scan_buffer", "level"), &GototRenderServer::gpu_hzb_dbg_scan_buffer);
	ClassDB::bind_method(D_METHOD("gpu_hzb_dbg_sim2"), &GototRenderServer::gpu_hzb_dbg_sim2);

	ClassDB::bind_method(D_METHOD("gpu_meshlet_load", "data"), &GototRenderServer::gpu_meshlet_load);
	ClassDB::bind_method(D_METHOD("gpu_meshlet_load_path", "path"), &GototRenderServer::gpu_meshlet_load_path);
	ClassDB::bind_method(D_METHOD("gpu_meshlet_set_lod_thresholds", "t0", "t1"), &GototRenderServer::gpu_meshlet_set_lod_thresholds);
	ClassDB::bind_method(D_METHOD("gpu_meshlet_cull_dispatch"), &GototRenderServer::gpu_meshlet_cull_dispatch);
	ClassDB::bind_method(D_METHOD("gpu_meshlet_raster_dispatch"), &GototRenderServer::gpu_meshlet_raster_dispatch);
	ClassDB::bind_method(D_METHOD("gpu_meshlet_get_total_meshlets"), &GototRenderServer::gpu_meshlet_get_total_meshlets);
	ClassDB::bind_method(D_METHOD("gpu_meshlet_get_lod0_tri_count"), &GototRenderServer::gpu_meshlet_get_lod0_tri_count);
	ClassDB::bind_method(D_METHOD("gpu_meshlet_get_lod0_meshlet_count"), &GototRenderServer::gpu_meshlet_get_lod0_meshlet_count);
	ClassDB::bind_method(D_METHOD("gpu_meshlet_stats"), &GototRenderServer::gpu_meshlet_stats);
	ClassDB::bind_method(D_METHOD("gpu_meshlet_get_cluster_counts"), &GototRenderServer::gpu_meshlet_get_cluster_counts);
	ClassDB::bind_method(D_METHOD("gpu_meshlet_get_instance_lods"), &GototRenderServer::gpu_meshlet_get_instance_lods);
	ClassDB::bind_method(D_METHOD("gpu_meshlet_get_cull_debug"), &GototRenderServer::gpu_meshlet_get_cull_debug);
	ClassDB::bind_method(D_METHOD("gpu_meshlet_raster_evidence"), &GototRenderServer::gpu_meshlet_raster_evidence);
	ClassDB::bind_method(D_METHOD("gpu_meshlet_destroy"), &GototRenderServer::gpu_meshlet_destroy);

	ClassDB::bind_method(D_METHOD("gpu_raster_indirect_draw"), &GototRenderServer::gpu_raster_indirect_draw);
	ClassDB::bind_method(D_METHOD("gpu_raster_read_pixels"), &GototRenderServer::gpu_raster_read_pixels);
	ClassDB::bind_method(D_METHOD("gpu_scene_get_vp"), &GototRenderServer::gpu_scene_get_vp);

	ClassDB::bind_method(D_METHOD("gpu_scene_set_instance_transform", "index", "position", "scale"), &GototRenderServer::gpu_scene_set_instance_transform);
	ClassDB::bind_method(D_METHOD("gpu_raster_read_depth"), &GototRenderServer::gpu_raster_read_depth);
	ClassDB::bind_method(D_METHOD("gpu_raster_read_viewz", "x", "y"), &GototRenderServer::gpu_raster_read_viewz);
	ClassDB::bind_method(D_METHOD("gpu_raster_get_depth_format"), &GototRenderServer::gpu_raster_get_depth_format);
	ClassDB::bind_method(D_METHOD("gpu_mesh_get_depth_enabled"), &GototRenderServer::gpu_mesh_get_depth_enabled);
	ClassDB::bind_method(D_METHOD("gpu_raster_get_depth_enabled"), &GototRenderServer::gpu_raster_get_depth_enabled);

	ClassDB::bind_method(D_METHOD("gpu_mesh_create"), &GototRenderServer::gpu_mesh_create);
	ClassDB::bind_method(D_METHOD("gpu_mesh_drawargs_finalize"), &GototRenderServer::gpu_mesh_drawargs_finalize);
	ClassDB::bind_method(D_METHOD("gpu_mesh_indirect_draw"), &GototRenderServer::gpu_mesh_indirect_draw);
	ClassDB::bind_method(D_METHOD("gpu_mesh_get_index_count"), &GototRenderServer::gpu_mesh_get_index_count);
	ClassDB::bind_method(D_METHOD("gpu_mesh_get_vertex_count"), &GototRenderServer::gpu_mesh_get_vertex_count);

	ClassDB::bind_method(D_METHOD("gpu_scene_set_instance_mesh", "index", "mesh_id"), &GototRenderServer::gpu_scene_set_instance_mesh);
	ClassDB::bind_method(D_METHOD("gpu_scene_get_instance_mesh", "index"), &GototRenderServer::gpu_scene_get_instance_mesh);
	ClassDB::bind_method(D_METHOD("gpu_mesh_create_from_arrays", "verts", "indices"), &GototRenderServer::gpu_mesh_create_from_arrays);
	ClassDB::bind_method(D_METHOD("gpu_mesh_batch_dispatch"), &GototRenderServer::gpu_mesh_batch_dispatch);
	ClassDB::bind_method(D_METHOD("gpu_mesh_batch_draw"), &GototRenderServer::gpu_mesh_batch_draw);
	ClassDB::bind_method(D_METHOD("gpu_mesh_get_mesh_id_count"), &GototRenderServer::gpu_mesh_get_mesh_id_count);
	ClassDB::bind_method(D_METHOD("gpu_mesh_get_batch_count"), &GototRenderServer::gpu_mesh_get_batch_count);
	ClassDB::bind_method(D_METHOD("gpu_mesh_get_draw_counts"), &GototRenderServer::gpu_mesh_get_draw_counts);
	ClassDB::bind_method(D_METHOD("gpu_mesh_get_batch_args", "batch_index"), &GototRenderServer::gpu_mesh_get_batch_args);
	ClassDB::bind_method(D_METHOD("gpu_mesh_get_mesh_color", "mesh_id"), &GototRenderServer::gpu_mesh_get_mesh_color);
	ClassDB::bind_method(D_METHOD("gpu_mesh_set_batch_strategy", "strategy"), &GototRenderServer::gpu_mesh_set_batch_strategy);
	ClassDB::bind_method(D_METHOD("gpu_mesh_get_batch_strategy"), &GototRenderServer::gpu_mesh_get_batch_strategy);
	ClassDB::bind_method(D_METHOD("gpu_mesh_get_batch_group_count"), &GototRenderServer::gpu_mesh_get_batch_group_count);
	ClassDB::bind_method(D_METHOD("gpu_mesh_get_batch_order"), &GototRenderServer::gpu_mesh_get_batch_order);
	ClassDB::bind_method(D_METHOD("gpu_mesh_get_indirect_count"), &GototRenderServer::gpu_mesh_get_indirect_count);
	ClassDB::bind_method(D_METHOD("gpu_mesh_get_draw_call_count"), &GototRenderServer::gpu_mesh_get_draw_call_count);
	ClassDB::bind_integer_constant("GototRenderServer", "GototBatchStrategy", "GOTOT_BATCH_STRATEGY_PER_MESH", GOTOT_BATCH_STRATEGY_PER_MESH);
	ClassDB::bind_integer_constant("GototRenderServer", "GototBatchStrategy", "GOTOT_BATCH_STRATEGY_GROUPED", GOTOT_BATCH_STRATEGY_GROUPED);
	ClassDB::bind_integer_constant("GototRenderServer", "GototBatchStrategy", "GOTOT_BATCH_STRATEGY_REORDERED", GOTOT_BATCH_STRATEGY_REORDERED);
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

void GototRenderServer::_destroy_mesh_batch() {
	if (rendering_device != nullptr) {
		if (group_batch_uniform_set.is_valid()) {
			rendering_device->free_rid(group_batch_uniform_set);
			group_batch_uniform_set = RID();
		}
		if (mesh_batch_uniform_set.is_valid()) {
			rendering_device->free_rid(mesh_batch_uniform_set);
			mesh_batch_uniform_set = RID();
		}
		if (mesh_batch_pipeline.is_valid()) {
			rendering_device->free_rid(mesh_batch_pipeline);
			mesh_batch_pipeline = RID();
		}
		if (mesh_batch_shader.is_valid()) {
			rendering_device->free_rid(mesh_batch_shader);
			mesh_batch_shader = RID();
		}
		if (batch_assemble_uniform_set.is_valid()) {
			rendering_device->free_rid(batch_assemble_uniform_set);
			batch_assemble_uniform_set = RID();
		}
		if (batch_assemble_pipeline.is_valid()) {
			rendering_device->free_rid(batch_assemble_pipeline);
			batch_assemble_pipeline = RID();
		}
		if (batch_assemble_shader.is_valid()) {
			rendering_device->free_rid(batch_assemble_shader);
			batch_assemble_shader = RID();
		}
		if (batch_count_uniform_set.is_valid()) {
			rendering_device->free_rid(batch_count_uniform_set);
			batch_count_uniform_set = RID();
		}
		if (batch_count_pipeline.is_valid()) {
			rendering_device->free_rid(batch_count_pipeline);
			batch_count_pipeline = RID();
		}
		if (batch_count_shader.is_valid()) {
			rendering_device->free_rid(batch_count_shader);
			batch_count_shader = RID();
		}
		if (mesh_010_index_array.is_valid()) {
			rendering_device->free_rid(mesh_010_index_array);
			mesh_010_index_array = RID();
		}
		if (mesh_010_vertex_array.is_valid()) {
			rendering_device->free_rid(mesh_010_vertex_array);
			mesh_010_vertex_array = RID();
		}
		if (batch_total_buffer.is_valid()) {
			rendering_device->free_rid(batch_total_buffer);
			batch_total_buffer = RID();
		}
		if (batch_args_buffer.is_valid()) {
			rendering_device->free_rid(batch_args_buffer);
			batch_args_buffer = RID();
		}
		if (batch_instances_buffer.is_valid()) {
			rendering_device->free_rid(batch_instances_buffer);
			batch_instances_buffer = RID();
		}
		if (mesh_scratch_buffer.is_valid()) {
			rendering_device->free_rid(mesh_scratch_buffer);
			mesh_scratch_buffer = RID();
		}
		if (batch_offset_buffer.is_valid()) {
			rendering_device->free_rid(batch_offset_buffer);
			batch_offset_buffer = RID();
		}
		if (batch_count_buffer.is_valid()) {
			rendering_device->free_rid(batch_count_buffer);
			batch_count_buffer = RID();
		}
		if (mesh_color_buffer.is_valid()) {
			rendering_device->free_rid(mesh_color_buffer);
			mesh_color_buffer = RID();
		}
		if (mesh_table_buffer.is_valid()) {
			rendering_device->free_rid(mesh_table_buffer);
			mesh_table_buffer = RID();
		}
		if (mesh_id_buffer.is_valid()) {
			rendering_device->free_rid(mesh_id_buffer);
			mesh_id_buffer = RID();
		}
		if (group_batch_pipeline.is_valid()) {
			rendering_device->free_rid(group_batch_pipeline);
			group_batch_pipeline = RID();
		}
		if (group_batch_shader.is_valid()) {
			rendering_device->free_rid(group_batch_shader);
			group_batch_shader = RID();
		}
		if (group_vertex_array.is_valid()) {
			rendering_device->free_rid(group_vertex_array);
			group_vertex_array = RID();
		}
		if (group_member_list_buffer.is_valid()) {
			rendering_device->free_rid(group_member_list_buffer);
			group_member_list_buffer = RID();
		}
		if (group_member_count_buffer.is_valid()) {
			rendering_device->free_rid(group_member_count_buffer);
			group_member_count_buffer = RID();
		}
		if (group_args_buffer.is_valid()) {
			rendering_device->free_rid(group_args_buffer);
			group_args_buffer = RID();
		}
		if (mesh_index_storage_buffer.is_valid()) {
			rendering_device->free_rid(mesh_index_storage_buffer);
			mesh_index_storage_buffer = RID();
		}
		if (mesh_vertex_storage_buffer.is_valid()) {
			rendering_device->free_rid(mesh_vertex_storage_buffer);
			mesh_vertex_storage_buffer = RID();
		}
	}
	mesh_table_count = 0;
	mesh_next_vertex_offset = 0;
	mesh_next_index_offset = 0;
	last_batch_count = 0;
	last_group_count = 0;
	last_used_group_draw = false;
	gpu_mesh_batch_valid = false;
	gpu_mesh_table_valid = false;
}

void GototRenderServer::_destroy_mesh() {
	// GOTOT-010: the batch path's 010 vertex/index arrays reference the shared
	// mesh vertex/index buffers below, so they must be released first.
	_destroy_mesh_batch();

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

void GototRenderServer::_destroy_hzb_prod() {
	if (rendering_device == nullptr) {
		gpu_hzb_prod_valid = false;
		hzb_pyramid_fresh = false;
		hzb_phase1_count = 0;
		hzb_phase2_count = 0;
		return;
	}

	auto free_rid = [&](RID &r) {
		if (r.is_valid()) {
			rendering_device->free_rid(r);
			r = RID();
		}
	};

	free_rid(hzb_phase2_uniform_set);
	free_rid(hzb_phase2_pipeline);
	free_rid(hzb_phase2_shader);
	free_rid(hzb_phase1_uniform_set);
	free_rid(hzb_phase1_pipeline);
	free_rid(hzb_phase1_shader);
	free_rid(hzb_occbuf_uniform_set);
	free_rid(hzb_occbuf_pipeline);
	free_rid(hzb_occbuf_shader);
	free_rid(hzb_depth_source_uniform_set);
	free_rid(hzb_depth_source_pipeline);
	free_rid(hzb_depth_source_shader);
	free_rid(hzb_depth_sampler);
	free_rid(hzb_prod_down_set);
	free_rid(hzb_prod_occ_set);
	free_rid(hzb_prod_clear_set);
	free_rid(hzb_dbg_probe_buffer);
	free_rid(phase1_count_buffer);
	free_rid(phase1_compact_buffer);
	free_rid(hzb_pyramid_data_buffer);
	free_rid(hzb_prod_array);

	gpu_hzb_prod_valid = false;
	hzb_pyramid_fresh = false;
	hzb_coherent = false;
	hzb_stable_frames = 0;
	hzb_phase1_count = 0;
	hzb_phase2_count = 0;
}

void GototRenderServer::_destroy_gpu_scene() {
	_destroy_mesh();
	_destroy_hzb_prod();

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
	if (raster_viewz_texture.is_valid()) {
		rendering_device->free_rid(raster_viewz_texture);
		raster_viewz_texture = RID();
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

	_destroy_meshlet();
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

	_destroy_meshlet();
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

	// GOTOT-013: capture the camera data UNCONDITIONALLY (independent of the
	// HZB path) so the meshlet pipeline - which has its OWN UBO, not view_ubo -
	// always receives a fresh vp/planes/cam/far even when HZB is not active.
	// last_vp keeps the same value the 004/009/012 paths compute, so nothing
	// downstream changes.
	meshlet_camera_position[0] = p_camera_transform.origin.x;
	meshlet_camera_position[1] = p_camera_transform.origin.y;
	meshlet_camera_position[2] = p_camera_transform.origin.z;
	far_plane = p_projection.get_z_far();
	{
		Projection ml_cam_view(p_camera_transform.inverse());
		Projection ml_vp_mat = p_projection * ml_cam_view;
		for (int c013 = 0; c013 < 4; c013++) {
			for (int r013 = 0; r013 < 4; r013++) {
				last_vp[c013 * 4 + r013] = ml_vp_mat.columns[c013][r013];
			}
		}
	}

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

	// GOTOT-012: store the depth-reconstruction projection coefficients for the
	// production pyramid (ndc_z = -A - B/z_view). Only the projection decides
	// them (the view matrix cancels out), so the same camera/view space used to
	// write the previous frame's D32 depth is recoverable exactly.
	far_plane = p_projection.get_z_far();
	hzb_proj_a = p_projection.columns[2][2];
	hzb_proj_b = p_projection.columns[3][2];

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

// GOTOT-012: build the production pyramid resources. Reuses the 004 clear/occ/
// down SHADERS and PIPELINES with separate uniform sets bound to the 2048x2048
// prod array (uniform-set reuse is valid as long as each set is created against
// the matching shader). The depth-source and the two-phase cull passes are new.
bool GototRenderServer::gpu_hzb_prod_create() {
	if (!ensure_gpu_device()) {
		return false;
	}
	if (!gpu_scene_valid || !gpu_hzb_valid || !raster_depth_attached) {
		print_error("[GOTOT-NEXT] gpu_hzb_prod_create: requires an existing GPU scene (gpu_scene_create) with the D32 raster depth attachment.");
		return false;
	}
	if (gpu_hzb_prod_valid) {
		return true;
	}

	String error;
	auto compile_compute = [&](const char *p_glsl, const char *p_name, RID &r_shader) -> bool {
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

	// Production pyramid texture: 2048x2048 R32UI 2D-array, 12 levels.
	RD::TextureFormat tf;
	tf.format = RD::DATA_FORMAT_R32_UINT;
	tf.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
	tf.width = HZB_PROD_TEXEL_COUNT;
	tf.height = HZB_PROD_TEXEL_COUNT;
	tf.depth = 1;
	tf.array_layers = HZB_PROD_LEVELS;
	tf.mipmaps = 1;
	tf.samples = RD::TEXTURE_SAMPLES_1;
	tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	hzb_prod_array = rendering_device->texture_create(tf, RD::TextureView());
	if (hzb_prod_array.is_null()) {
		print_error("[GOTOT-NEXT] gpu_hzb_prod_create: prod pyramid texture_create failed.");
		return false;
	}

	phase1_compact_buffer = rendering_device->storage_buffer_create((uint32_t)gpu_instance_count * 4u);
	phase1_count_buffer = rendering_device->storage_buffer_create(4);
	hzb_pyramid_data_buffer = rendering_device->storage_buffer_create(HZB_PYRAMID_DATA_UINTS * 4u);
	if (hzb_pyramid_data_buffer.is_null()) {
		print_error("[GOTOT-NEXT] gpu_hzb_prod_create: pyramid data buffer_create failed.");
		_destroy_hzb_prod();
		return false;
	}

	hzb_depth_sampler = rendering_device->sampler_create(RD::SamplerState());
	hzb_dbg_probe_buffer = rendering_device->storage_buffer_create(16);
	if (hzb_depth_sampler.is_null()) {
		print_error("[GOTOT-NEXT] gpu_hzb_prod_create: depth sampler_create failed.");
		_destroy_hzb_prod();
		return false;
	}

	// Reused uniform sets (004 shaders/pipelines) bound to the prod array.
	{
		Vector<RD::Uniform> cu;
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 0;
		u.append_id(hzb_prod_array);
		cu.push_back(u);
		hzb_prod_clear_set = rendering_device->uniform_set_create(cu, hzb_clear_shader, 0);
		if (hzb_prod_clear_set.is_null()) {
			print_error("[GOTOT-NEXT] gpu_hzb_prod_create: prod clear uniform_set_create failed.");
			_destroy_hzb_prod();
			return false;
		}
	}
	{
		Vector<RD::Uniform> ou;
		RD::Uniform u0;
		u0.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u0.binding = 0;
		u0.append_id(hzb_prod_array);
		ou.push_back(u0);
		RD::Uniform u1;
		u1.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u1.binding = 1;
		u1.append_id(occluder_min_buffer);
		ou.push_back(u1);
		RD::Uniform u2;
		u2.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u2.binding = 2;
		u2.append_id(occluder_max_buffer);
		ou.push_back(u2);
		RD::Uniform u5;
		u5.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u5.binding = 5;
		u5.append_id(view_ubo);
		ou.push_back(u5);
		hzb_prod_occ_set = rendering_device->uniform_set_create(ou, hzb_occ_shader, 0);
		if (hzb_prod_occ_set.is_null()) {
			print_error("[GOTOT-NEXT] gpu_hzb_prod_create: prod occ uniform_set_create failed.");
			_destroy_hzb_prod();
			return false;
		}
	}
	{
		Vector<RD::Uniform> du;
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 0;
		u.append_id(hzb_prod_array);
		du.push_back(u);
		hzb_prod_down_set = rendering_device->uniform_set_create(du, hzb_down_shader, 0);
		if (hzb_prod_down_set.is_null()) {
			print_error("[GOTOT-NEXT] gpu_hzb_prod_create: prod down uniform_set_create failed.");
			_destroy_hzb_prod();
			return false;
		}
	}

	// Depth-source pass: sampler2D (0) + image (1) + probe buffer (2) +
	// pyramid-data storage buffer (3) - the buffer is the RELIABLE pyramid the
	// phase-2 occlusion reads (storage buffers sync correctly in this fork).
	if (!compile_compute(gpu_hzb_depth_source_glsl, "gotot_hzb_depth_source", hzb_depth_source_shader)) {
		_destroy_hzb_prod();
		return false;
	}
	hzb_depth_source_pipeline = rendering_device->compute_pipeline_create(hzb_depth_source_shader);
	Vector<RD::Uniform> ds_uniforms;
	RD::Uniform ds0;
	ds0.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
	ds0.binding = 0;
	ds0.append_id(hzb_depth_sampler);
	ds0.append_id(raster_viewz_texture);
	ds_uniforms.push_back(ds0);
	RD::Uniform ds1;
	ds1.uniform_type = RD::UNIFORM_TYPE_IMAGE;
	ds1.binding = 1;
	ds1.append_id(hzb_prod_array);
	ds_uniforms.push_back(ds1);
	RD::Uniform ds2;
	ds2.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	ds2.binding = 2;
	ds2.append_id(hzb_dbg_probe_buffer);
	ds_uniforms.push_back(ds2);
	RD::Uniform ds3;
	ds3.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	ds3.binding = 3;
	ds3.append_id(hzb_pyramid_data_buffer);
	ds_uniforms.push_back(ds3);
	hzb_depth_source_uniform_set = rendering_device->uniform_set_create(ds_uniforms, hzb_depth_source_shader, 0);
	if (hzb_depth_source_uniform_set.is_null()) {
		print_error("[GOTOT-NEXT] gpu_hzb_prod_create: depth-source uniform_set_create failed.");
		_destroy_hzb_prod();
		return false;
	}

	// GOTOT-012 final source: occluder-box pyramid (occmin(0)/occmax(1)/UBO(2)/
	// pyramid-data(3)/evidence image(4)). Only the flat storage buffer is read
	// by the phases - the image keeps a readback copy for evidence.
	if (!compile_compute(gpu_hzb_occbuf_glsl, "gotot_hzb_occbuf", hzb_occbuf_shader)) {
		_destroy_hzb_prod();
		return false;
	}
	hzb_occbuf_pipeline = rendering_device->compute_pipeline_create(hzb_occbuf_shader);
	Vector<RD::Uniform> ob_uniforms;
	RD::Uniform ob0;
	ob0.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	ob0.binding = 0;
	ob0.append_id(occluder_min_buffer);
	ob_uniforms.push_back(ob0);
	RD::Uniform ob1;
	ob1.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	ob1.binding = 1;
	ob1.append_id(occluder_max_buffer);
	ob_uniforms.push_back(ob1);
	RD::Uniform ob2;
	ob2.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
	ob2.binding = 2;
	ob2.append_id(view_ubo);
	ob_uniforms.push_back(ob2);
	RD::Uniform ob3;
	ob3.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	ob3.binding = 3;
	ob3.append_id(hzb_pyramid_data_buffer);
	ob_uniforms.push_back(ob3);
	RD::Uniform ob4;
	ob4.uniform_type = RD::UNIFORM_TYPE_IMAGE;
	ob4.binding = 4;
	ob4.append_id(hzb_prod_array);
	ob_uniforms.push_back(ob4);
	hzb_occbuf_uniform_set = rendering_device->uniform_set_create(ob_uniforms, hzb_occbuf_shader, 0);
	if (hzb_occbuf_uniform_set.is_null()) {
		print_error("[GOTOT-NEXT] gpu_hzb_prod_create: occbuf uniform_set_create failed.");
		_destroy_hzb_prod();
		return false;
	}

	// Phase 1 (frustum only): transforms(0), phase1 list(1), phase1 count(2),
	// visibility(3), view UBO(5).
	if (!compile_compute(gpu_hzb_phase1_glsl, "gotot_hzb_phase1", hzb_phase1_shader)) {
		_destroy_hzb_prod();
		return false;
	}
	hzb_phase1_pipeline = rendering_device->compute_pipeline_create(hzb_phase1_shader);
	Vector<RD::Uniform> p1_uniforms;
	RD::Uniform p1u0;
	p1u0.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	p1u0.binding = 0;
	p1u0.append_id(transform_buffer);
	p1_uniforms.push_back(p1u0);
	RD::Uniform p1u1;
	p1u1.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	p1u1.binding = 1;
	p1u1.append_id(phase1_compact_buffer);
	p1_uniforms.push_back(p1u1);
	RD::Uniform p1u2;
	p1u2.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	p1u2.binding = 2;
	p1u2.append_id(phase1_count_buffer);
	p1_uniforms.push_back(p1u2);
	RD::Uniform p1u3;
	p1u3.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	p1u3.binding = 3;
	p1u3.append_id(visibility_buffer);
	p1_uniforms.push_back(p1u3);
	RD::Uniform p1u5;
	p1u5.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
	p1u5.binding = 5;
	p1u5.append_id(view_ubo);
	p1_uniforms.push_back(p1u5);
	hzb_phase1_uniform_set = rendering_device->uniform_set_create(p1_uniforms, hzb_phase1_shader, 0);
	if (hzb_phase1_uniform_set.is_null()) {
		print_error("[GOTOT-NEXT] gpu_hzb_prod_create: phase1 uniform_set_create failed.");
		_destroy_hzb_prod();
		return false;
	}

	// Phase 2 (HZB occlusion): phase1 list(0), transforms(1), count(2),
	// compact(3), pyramid data buffer(4), view UBO(5).
	if (!compile_compute(gpu_hzb_phase2_glsl, "gotot_hzb_phase2", hzb_phase2_shader)) {
		_destroy_hzb_prod();
		return false;
	}
	hzb_phase2_pipeline = rendering_device->compute_pipeline_create(hzb_phase2_shader);
	Vector<RD::Uniform> p2_uniforms;
	RD::Uniform p2u0;
	p2u0.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	p2u0.binding = 0;
	p2u0.append_id(phase1_compact_buffer);
	p2_uniforms.push_back(p2u0);
	RD::Uniform p2u1;
	p2u1.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	p2u1.binding = 1;
	p2u1.append_id(transform_buffer);
	p2_uniforms.push_back(p2u1);
	RD::Uniform p2u2;
	p2u2.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	p2u2.binding = 2;
	p2u2.append_id(visible_count_buffer);
	p2_uniforms.push_back(p2u2);
	RD::Uniform p2u3;
	p2u3.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	p2u3.binding = 3;
	p2u3.append_id(compact_buffer);
	p2_uniforms.push_back(p2u3);
	RD::Uniform p2u4;
	p2u4.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	p2u4.binding = 4;
	p2u4.append_id(hzb_pyramid_data_buffer);
	p2_uniforms.push_back(p2u4);
	RD::Uniform p2u5;
	p2u5.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
	p2u5.binding = 5;
	p2u5.append_id(view_ubo);
	p2_uniforms.push_back(p2u5);
	RD::Uniform p2u6;
	p2u6.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	p2u6.binding = 6;
	p2u6.append_id(hzb_dbg_probe_buffer);
	p2_uniforms.push_back(p2u6);
	hzb_phase2_uniform_set = rendering_device->uniform_set_create(p2_uniforms, hzb_phase2_shader, 0);
	if (hzb_phase2_uniform_set.is_null()) {
		print_error("[GOTOT-NEXT] gpu_hzb_prod_create: phase2 uniform_set_create failed.");
		_destroy_hzb_prod();
		return false;
	}

	gpu_hzb_prod_valid = true;
	hzb_temporal_enabled = true;
	hzb_pyramid_fresh = false;
	hzb_coherent = false;
	hzb_stable_frames = 0;
	hzb_inflate_factor = 2.0f;
	hzb_phase1_count = 0;
	hzb_phase2_count = 0;

	print_line("[GOTOT-NEXT] Production HZB created. levels=" + itos(HZB_PROD_LEVELS) +
			" base=" + itos(HZB_PROD_TEXEL_COUNT) + "x" + itos(HZB_PROD_TEXEL_COUNT));

	return true;
}

// GOTOT-012: rebuild the production pyramid from the PREVIOUS frame's depth and
// run the temporal bookkeeping. The pyramid may only be used when the camera vp
// that wrote that depth matches the CURRENT vp; otherwise hzb_valid is left 0
// (frustum-only, conservative - no false dropout) and the pyramid is rebuilt
// next frame from depth the settled camera just wrote.
bool GototRenderServer::gpu_hzb_build() {
	if (!gpu_scene_valid || !gpu_hzb_prod_valid) {
		print_error("[GOTOT-NEXT] gpu_hzb_build: production HZB not created.");
		return false;
	}
	if (!camera_view_valid || !frustum_valid) {
		print_error("[GOTOT-NEXT] gpu_hzb_build: no camera. Call gpu_scene_set_camera first.");
		return false;
	}

	bool same_vp = hzb_pyramid_fresh && (memcmp(hzb_build_vp, last_vp, sizeof(last_vp)) == 0);
	hzb_coherent = (hzb_stable_frames >= 2) && hzb_temporal_enabled;
	memcpy(hzb_build_vp, last_vp, sizeof(last_vp));
	hzb_pyramid_fresh = true;

	if (!same_vp) {
		// Camera moved since the last build (or first frame before any depth).
		// The previous depth buffer lives in a different view space -> do NOT
		// build a pyramid from it. Conservative: frustum-only this frame.
		hzb_stable_frames = 1;
		hzb_coherent = false;
		uint32_t zero = 0;
		rendering_device->buffer_update(view_ubo, offsetof(GototViewData, hzb_valid), 4, &zero);
		return true;
	}
	hzb_stable_frames++;

	// Patch the shared view UBO for the production square grid.
	int32_t texel_count = HZB_PROD_TEXEL_COUNT;
	rendering_device->buffer_update(view_ubo, offsetof(GototViewData, viewport) + 2 * sizeof(float), 4, &texel_count);
	int32_t occ_count = occluder_count;
	rendering_device->buffer_update(view_ubo, offsetof(GototViewData, occ_count), 4, &occ_count);
	uint32_t hzb_one = 1;
	rendering_device->buffer_update(view_ubo, offsetof(GototViewData, hzb_valid), 4, &hzb_one);

	// Queue the pyramid passes to run INSIDE the next gpu_mesh_batch_draw
	// submission (clear, R32 depth-source sample, box occluders, downsample
	// chain). The depth source samples the R32 color attachment written by the
	// draw of the very same command stream - cross-submission attachment
	// sampling returns the pre-draw (cleared) version in this RDG fork.
	hzb_rebuild_requested = true;
	return true;
}

// GOTOT-012: two-phase production visibility dispatch. Phase 1 produces the
// frustum survivors (phase-1 list + count + visibility flags), phase 2 applies
// the HZB occlusion to that list and compacts the survivors into the FINAL
// compact[]/visible_count[] consumed by the 011 batch assembler.
bool GototRenderServer::gpu_visibility_prod_dispatch() {
	if (!gpu_scene_valid || !gpu_cull_valid || !gpu_hzb_valid || !gpu_hzb_prod_valid) {
		print_error("[GOTOT-NEXT] gpu_visibility_prod_dispatch: no production HZB. Call gpu_hzb_prod_create first.");
		return false;
	}
	if (!frustum_valid || !camera_view_valid) {
		print_error("[GOTOT-NEXT] gpu_visibility_prod_dispatch: no camera. Call gpu_scene_set_camera first.");
		return false;
	}

	// Patch the shared view UBO for the production square grid + the freshest
	// occluder count (hzb_valid reflects what gpu_hzb_build left in the UBO).
	{
		int32_t texel_count = HZB_PROD_TEXEL_COUNT;
		rendering_device->buffer_update(view_ubo, offsetof(GototViewData, viewport) + 2 * sizeof(float), 4, &texel_count);
		int32_t occ_count = occluder_count;
		rendering_device->buffer_update(view_ubo, offsetof(GototViewData, occ_count), 4, &occ_count);
	}

	// Two-phase cull in THIS synced submission. First rebuild the pyramid from
	// the registered occluder AABBs (flat storage buffer - cross-submission
	// reads are the ONE reliable GPU route in this RDG fork), then phase 1
	// (frustum-only evidence count) and phase 2 (frustum + HZB occlusion into
	// compact[]/visible_count[]).
	rendering_device->buffer_clear(phase1_count_buffer, 0, 4);
	rendering_device->buffer_clear(visible_count_buffer, 0, 4);
	rendering_device->buffer_clear(hzb_pyramid_data_buffer, 0, HZB_PYRAMID_DATA_UINTS * 4);
	if (occluder_count > 0) {
		RD::ComputeListID ocl = rendering_device->compute_list_begin();
		rendering_device->compute_list_bind_compute_pipeline(ocl, hzb_occbuf_pipeline);
		rendering_device->compute_list_bind_uniform_set(ocl, hzb_occbuf_uniform_set, 0);
		rendering_device->compute_list_dispatch(ocl, 1, 1, 1);
		rendering_device->compute_list_end();
	}

	struct Phase1Params {
		uint32_t instance_count;
		uint32_t pad0;
		uint32_t pad1;
		uint32_t pad2;
	};
	Phase1Params ph1;
	ph1.instance_count = (uint32_t)gpu_instance_count;
	ph1.pad0 = 0;
	ph1.pad1 = 0;
	ph1.pad2 = 0;
	uint32_t pgroups = (uint32_t)(((gpu_instance_count - 1) / 64) + 1);
	_run_compute_pass(hzb_phase1_pipeline, hzb_phase1_uniform_set, &ph1, sizeof(ph1), pgroups, 1, 1);

	struct Phase2Params {
		uint32_t instance_count;
		float inflate;
		uint32_t pad0;
		uint32_t pad1;
	};
	Phase2Params ph2;
	ph2.instance_count = (uint32_t)gpu_instance_count;
	ph2.inflate = hzb_coherent ? 1.0f : hzb_inflate_factor;
	ph2.pad0 = 0;
	ph2.pad1 = 0;
	_run_compute_pass(hzb_phase2_pipeline, hzb_phase2_uniform_set, &ph2, sizeof(ph2), pgroups, 1, 1);

	hzb_phase1_count = -1;
	{
		Vector<uint8_t> p1bytes = rendering_device->buffer_get_data(phase1_count_buffer, 0, 4);
		if (p1bytes.size() == 4) {
			uint32_t c = 0;
			memcpy(&c, p1bytes.ptr(), 4);
			hzb_phase1_count = (int)c;
		}
	}
	uint32_t p1count = (uint32_t)MAX(hzb_phase1_count, 0);

	// Phase 2 result (visible_count = survivors after occlusion).
	hzb_phase2_count = (int)p1count;
	{
		Vector<uint8_t> p2bytes = rendering_device->buffer_get_data(visible_count_buffer, 0, 4);
		if (p2bytes.size() == 4) {
			uint32_t c = 0;
			memcpy(&c, p2bytes.ptr(), 4);
			hzb_phase2_count = (int)c;
		}
	}

	return true;
}

void GototRenderServer::gpu_hzb_enable_temporal(bool p_enabled) {
	hzb_temporal_enabled = p_enabled;
	if (!p_enabled) {
		hzb_coherent = false;
	}
}

void GototRenderServer::gpu_hzb_set_occluders(const Vector<Vector4> &p_occluders) {
	gpu_scene_set_occluders(p_occluders);
}

int GototRenderServer::gpu_hzb_get_level_count() {
	if (gpu_hzb_prod_valid) {
		return HZB_PROD_LEVELS;
	}
	return gpu_hzb_valid ? HZB_LEVELS : 0;
}

PackedInt32Array GototRenderServer::gpu_hzb_get_phase_counts() {
	PackedInt32Array ret;
	ret.resize(2);
	ret.set(0, hzb_phase1_count);
	ret.set(1, hzb_phase2_count);
	return ret;
}

bool GototRenderServer::gpu_hzb_get_coherent() const {
	return hzb_coherent;
}

// CPU replica of the phase-2 shader's occlusion math for a probe set of
// instances, using the REAL GPU buffers phase 2 reads (view UBO, transform
// buffer, pyramid data buffer). Returns [level, t.x, t.y, max_inv, sphere_inv,
// vis] rows. Validates the index mapping + pyramid content end-to-end.
PackedInt32Array GototRenderServer::gpu_hzb_dbg_sim2() {
	PackedInt32Array out;
	if (!gpu_hzb_prod_valid) {
		return out;
	}
	const int probe_insts[8] = { 0, 64, 72, 91, 97, 120, 128, 131 };
	Vector<uint8_t> vb = rendering_device->buffer_get_data(view_ubo, 0, 256);
	Vector<uint8_t> tb = rendering_device->buffer_get_data(transform_buffer, 0, (uint32_t)gpu_instance_count * 16);
	Vector<uint8_t> pb = rendering_device->buffer_get_data(hzb_pyramid_data_buffer, 0, HZB_PYRAMID_DATA_UINTS * 4);
	if (vb.size() < 256 || tb.size() < (size_t)gpu_instance_count * 16 || pb.size() < HZB_PYRAMID_DATA_UINTS * 4) {
		return out;
	}
	const float *vp = (const float *)vb.ptr();
	const float *view = vp + 16;
	const float *viewport = vp + 16 + 16 + 24; // after planes[6]
	float far_plane = ((const float *)vb.ptr())[61];
	float tanv = viewport[3];
	const float *ts = (const float *)tb.ptr();
	const uint32_t *pyr = (const uint32_t *)pb.ptr();
	float inflate = hzb_coherent ? 1.0f : hzb_inflate_factor;

	for (int n = 0; n < 8; n++) {
		int j = probe_insts[n];
		Vector3 center(ts[j * 4 + 0], ts[j * 4 + 1], ts[j * 4 + 2]);
		float radius = ts[j * 4 + 3] * inflate;
		float cx = vp[0] * center.x + vp[4] * center.y + vp[8] * center.z + vp[12];
		float cy = vp[1] * center.x + vp[5] * center.y + vp[9] * center.z + vp[13];
		float cw = vp[3] * center.x + vp[7] * center.y + vp[11] * center.z + vp[15];
		float vx = view[0] * center.x + view[4] * center.y + view[8] * center.z + view[12];
		float vy = view[1] * center.x + view[5] * center.y + view[9] * center.z + view[13];
		float vz = view[2] * center.x + view[6] * center.y + view[10] * center.z + view[14];
		float z_view = -vz;
		float r_px = (radius * 0.5f * viewport[1]) / (viewport[3] * MAX(z_view, 0.0001f));
		int level = 0;
		int tx = -1;
		int ty = -1;
		uint32_t max_inv = 0;
		uint32_t sphere_inv = 0;
		int vis = 1;
		if (r_px >= 1.0f) {
			float ndcx = cx / cw;
			float ndcy = cy / cw;
			float px_sx = (ndcx * 0.5f + 0.5f) * viewport[0];
			float px_sy = (ndcy * 0.5f + 0.5f) * viewport[1];
			float logt = ceil(log2(r_px));
			level = CLAMP((int)logt, 0, (int)log2((double)viewport[2]));
			float scale = exp2((float)level);
			int texels = (int)viewport[2] >> level;
			int txi = CLAMP((int)(px_sx / scale), 0, texels - 1);
			int tyi = CLAMP((int)(px_sy / scale), 0, texels - 1);
			float z_sphere = MAX(z_view - radius, 0.0f);
			sphere_inv = (uint32_t)floor(MAX(far_plane - z_sphere, 0.0f));
			uint64_t poff = 0;
			for (int k = 0; k < level; k++) {
				uint64_t s = HZB_PROD_TEXEL_COUNT >> k;
				poff += s * s;
			}
			for (int dy = 0; dy <= 1; dy++) {
				for (int dx = 0; dx <= 1; dx++) {
					int tcx = CLAMP(txi + dx, 0, texels - 1);
					int tcy = CLAMP(tyi + dy, 0, texels - 1);
					uint32_t v = pyr[poff + tcx * (uint64_t)texels + tcy];
					if (v > max_inv) {
						max_inv = v;
					}
				}
			}
			if (max_inv > sphere_inv) {
				vis = 0;
			}
			tx = txi;
			ty = tyi;
		} else {
			vis = -2; // r_px<1 (sphere under a pixel): always visible
		}
		out.append(j);
		out.append(level);
		out.append(tx);
		out.append(ty);
		out.append((int)max_inv);
		out.append((int)sphere_inv);
		out.append(vis);
		out.append((int)r_px);
	}
	return out;
}

int GototRenderServer::gpu_hzb_dbg_valid() {
	if (!gpu_hzb_prod_valid) {
		return -1;
	}
	Vector<uint8_t> bytes = rendering_device->buffer_get_data(view_ubo, offsetof(GototViewData, hzb_valid), 4);
	if (bytes.size() != 4) {
		return -2;
	}
	uint32_t v = 0;
	memcpy(&v, bytes.ptr(), 4);
	return (int)v;
}

int GototRenderServer::gpu_hzb_dbg_level0(int p_x, int p_y) {
	if (!gpu_hzb_prod_valid) {
		return -1;
	}
	if (p_x < 0 || p_x >= HZB_PROD_TEXEL_COUNT || p_y < 0 || p_y >= HZB_PROD_TEXEL_COUNT) {
		return -3;
	}
	Vector<uint8_t> bytes = rendering_device->texture_get_data(hzb_prod_array, 0);
	if (bytes.size() < (p_y * HZB_PROD_TEXEL_COUNT + p_x + 1) * 4) {
		return -2;
	}
	uint32_t v = 0;
	memcpy(&v, bytes.ptr() + (p_y * HZB_PROD_TEXEL_COUNT + p_x) * 4, 4);
	return (int)v;
}

// Whole level-0 scan: returns [max_inv, max_x, max_y, count_of_nonzero] so the
// verify-bridge can tell whether real geometry ever lands in the pyramid (the
// per-texel probes above sit on far/sky pixels chosen blindly).
PackedInt32Array GototRenderServer::gpu_hzb_dbg_scan_level0() {
	PackedInt32Array out;
	if (!gpu_hzb_prod_valid) {
		return out;
	}
	Vector<uint8_t> bytes = rendering_device->texture_get_data(hzb_prod_array, 0);
	if (bytes.size() < HZB_PROD_TEXEL_COUNT * HZB_PROD_TEXEL_COUNT * 4) {
		return out;
	}
	const uint32_t *vals = (const uint32_t *)bytes.ptr();
	uint32_t maxv = 0;
	int maxx = -1;
	int maxy = -1;
	uint32_t nonzero = 0;
	for (int y = 0; y < HZB_PROD_TEXEL_COUNT; y++) {
		for (int x = 0; x < HZB_PROD_TEXEL_COUNT; x++) {
			uint32_t v = vals[y * HZB_PROD_TEXEL_COUNT + x];
			if (v > 0u) {
				nonzero++;
				if (v > maxv) {
					maxv = v;
					maxx = x;
					maxy = y;
				}
			}
		}
	}
	out.append((int32_t)maxv);
	out.append(maxx);
	out.append(maxy);
	out.append((int32_t)nonzero);
	return out;
}

// Same scan restricted to a SINGLE pyramid layer (downsample verification).
PackedInt32Array GototRenderServer::gpu_hzb_dbg_scan_level1(int p_level) {
	PackedInt32Array out;
	if (!gpu_hzb_prod_valid) {
		return out;
	}
	if (p_level < 0 || p_level >= HZB_PROD_LEVELS) {
		return out;
	}
	int size = HZB_PROD_TEXEL_COUNT >> p_level;
	Vector<uint8_t> bytes = rendering_device->texture_get_data(hzb_prod_array, p_level);
	if (bytes.size() < size * size * 4) {
		return out;
	}
	const uint32_t *vals = (const uint32_t *)bytes.ptr();
	uint32_t maxv = 0;
	int maxx = -1;
	int maxy = -1;
	uint32_t nonzero = 0;
	for (int y = 0; y < size; y++) {
		for (int x = 0; x < size; x++) {
			uint32_t v = vals[y * size + x];
			if (v > 0u) {
				nonzero++;
				if (v > maxv) {
					maxv = v;
					maxx = x;
					maxy = y;
				}
			}
		}
	}
	out.append((int32_t)maxv);
	out.append(maxx);
	out.append(maxy);
	out.append((int32_t)nonzero);
	return out;
}

PackedInt32Array GototRenderServer::gpu_hzb_dbg_scan_buffer(int p_level) {
	PackedInt32Array out;
	if (!gpu_hzb_prod_valid) {
		return out;
	}
	if (p_level < 0 || p_level >= HZB_PROD_LEVELS) {
		return out;
	}
	int size = HZB_PROD_TEXEL_COUNT >> p_level;
	uint64_t poff = 0;
	for (int k = 0; k < p_level; k++) {
		uint64_t s = HZB_PROD_TEXEL_COUNT >> k;
		poff += s * s;
	}
	Vector<uint8_t> bytes = rendering_device->buffer_get_data(hzb_pyramid_data_buffer, (uint32_t)(poff * 4), size * size * 4);
	if (bytes.size() < size * size * 4) {
		return out;
	}
	const uint32_t *vals = (const uint32_t *)bytes.ptr();
	uint32_t maxv = 0;
	int maxx = -1;
	int maxy = -1;
	uint32_t nonzero = 0;
	for (int y = 0; y < size; y++) {
		for (int x = 0; x < size; x++) {
			uint32_t v = vals[y * size + x];
			if (v > 0u) {
				nonzero++;
				if (v > maxv) {
					maxv = v;
					maxx = x;
					maxy = y;
				}
			}
		}
	}
	out.append((int32_t)maxv);
	out.append(maxx);
	out.append(maxy);
	out.append((int32_t)nonzero);
	return out;
}

PackedInt32Array GototRenderServer::gpu_hzb_dbg_probe() {
	PackedInt32Array out;
	if (!gpu_hzb_prod_valid) {
		return out;
	}
	Vector<uint8_t> bytes = rendering_device->buffer_get_data(hzb_dbg_probe_buffer, 0, 16);
	if (bytes.size() != 16) {
		return out;
	}
	const uint32_t *vals = (const uint32_t *)bytes.ptr();
	for (int i = 0; i < 4; i++) {
		out.append((int32_t)vals[i]);
	}
	return out;
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
	// start of every frame by the draw list flags (DRAW_CLEAR_DEPTH). Exact 004
	// usage bits (the prod pyramid reads the R32 view-space-depth copy below, so
	// the D32 needs no sampling usage).
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

	// GOTOT-012: R32_SFLOAT view-space depth (positive z_view) written by the
	// batch/group/raster fragment shaders as output location 1 in the SAME draw
	// pass as the D32. The production pyramid's depth-source pass samples THIS
	// color texture instead of the D32 (the D32 sampled view is a broken/no-op
	// path in this RDG fork), keeping the occlusion build 100% GPU-side.
	RD::TextureFormat vf;
	vf.format = RD::DATA_FORMAT_R32_SFLOAT;
	vf.width = RASTER_TARGET_W;
	vf.height = RASTER_TARGET_H;
	vf.depth = 1;
	vf.texture_type = RD::TEXTURE_TYPE_2D;
	vf.usage_bits = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	raster_viewz_texture = rendering_device->texture_create(vf, RD::TextureView());
	if (raster_viewz_texture.is_null()) {
		print_error("[GOTOT-NEXT] raster view-z texture_create failed.");
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
	RD::AttachmentFormat vf_af;
	vf_af.format = RD::DATA_FORMAT_R32_SFLOAT;
	vf_af.samples = RD::TEXTURE_SAMPLES_1;
	vf_af.usage_flags = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	afs.push_back(vf_af);
	raster_framebuffer_format = rendering_device->framebuffer_format_create(afs);
	if (raster_framebuffer_format < 0) {
		print_error("[GOTOT-NEXT] raster framebuffer_format_create failed.");
		return false;
	}

	Vector<RID> attachments;
	attachments.push_back(raster_color_texture);
	attachments.push_back(raster_viewz_texture);
	attachments.push_back(raster_depth_texture);
	// Skip the format-check id so RD recomputes the format from the textures
	// themselves (identical layout; the check only guards against stale ids).
	raster_framebuffer = rendering_device->framebuffer_create(attachments, RD::INVALID_ID);
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
	RD::PipelineColorBlendState bs = RD::PipelineColorBlendState::create_disabled(2);
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
	RD::PipelineColorBlendState bs = RD::PipelineColorBlendState::create_disabled(2);
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

	// GOTOT-008A: a single real CUBE mesh (positions only) occupying mesh table
	// slot 0. No normals, no UVs, no materials, no textures. GOTOT-010 then
	// registers additional meshes (gpu_mesh_create_from_arrays) as appended
	// sub-ranges of the SHARED vertex/index buffers below.
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

	// GOTOT-010: the SHARED vertex/index buffers get capacity for the whole mesh
	// table up-front; the cube is uploaded at offset 0 so every existing 008A/009
	// draw (first_index/vertex_offset 0, index_count 36) renders identically.
	// Per-mesh data is appended at increasing offsets by create_from_arrays.
	Vector<uint8_t> vcap_bytes;
	vcap_bytes.resize((uint32_t)GOTOT_MAX_MESH_VERTS * 12);
	mesh_vertex_buffer = rendering_device->vertex_buffer_create((uint32_t)(GOTOT_MAX_MESH_VERTS * 12), vcap_bytes);
	Vector<uint8_t> icap_bytes;
	icap_bytes.resize((uint32_t)GOTOT_MAX_MESH_INDICES * 4);
	mesh_index_buffer = rendering_device->index_buffer_create(GOTOT_MAX_MESH_INDICES, RD::INDEX_BUFFER_FORMAT_UINT32, icap_bytes);
	if (mesh_vertex_buffer.is_null() || mesh_index_buffer.is_null()) {
		print_error("[GOTOT-NEXT] gpu_mesh_create: vertex/index buffer_create failed.");
		_destroy_mesh();
		return false;
	}
	rendering_device->buffer_update(mesh_vertex_buffer, 0, (uint32_t)vertex_bytes.size(), vertex_bytes.ptr());
	rendering_device->buffer_update(mesh_index_buffer, 0, (uint32_t)index_bytes.size(), index_bytes.ptr());

	// GOTOT-011: storage mirrors of the shared vertex/index buffers so the
	// procedural (non-indexed) group draw can fetch geometry as SSBOs (RD
	// vertex/index buffer owners cannot be bound in a uniform set).
	{
		Vector<uint8_t> vscap_bytes;
		vscap_bytes.resize((uint32_t)GOTOT_MAX_MESH_VERTS * 12);
		memcpy(vscap_bytes.ptrw(), vertex_bytes.ptr(), vertex_bytes.size());
		mesh_vertex_storage_buffer = rendering_device->storage_buffer_create((uint32_t)(GOTOT_MAX_MESH_VERTS * 12), vscap_bytes);
		Vector<uint8_t> iscap_bytes;
		iscap_bytes.resize((uint32_t)GOTOT_MAX_MESH_INDICES * 4);
		memcpy(iscap_bytes.ptrw(), index_bytes.ptr(), index_bytes.size());
		mesh_index_storage_buffer = rendering_device->storage_buffer_create((uint32_t)(GOTOT_MAX_MESH_INDICES * 4), iscap_bytes);
		if (mesh_vertex_storage_buffer.is_null() || mesh_index_storage_buffer.is_null()) {
			print_error("[GOTOT-NEXT] gpu_mesh_create: storage mirror buffer_create failed.");
			_destroy_mesh();
			return false;
		}
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
	// GOTOT-010: full-capacity arrays bound by the multi-batch draw path (its
	// commands may reference any sub-range of the shared buffers via vertex_offset
	// / first_index).
	mesh_010_vertex_array = rendering_device->vertex_array_create(GOTOT_MAX_MESH_VERTS, mesh_vertex_format, src_buffers);
	mesh_010_index_array = rendering_device->index_array_create(mesh_index_buffer, 0, GOTOT_MAX_MESH_INDICES);
	if (mesh_010_vertex_array.is_null() || mesh_010_index_array.is_null()) {
		print_error("[GOTOT-NEXT] gpu_mesh_create: 010 vertex/index array_create failed.");
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

	// GOTOT-010: mesh table GPU resources + batch assembly/draw pipelines.
	if (!_init_mesh_table_gpu()) {
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
	clear_colors.push_back(Color(far_plane, 0.0f, 0.0f, 0.0f));
	RD::DrawListID dl = rendering_device->draw_list_begin(raster_framebuffer, RD::DRAW_CLEAR_COLOR_0 | RD::DRAW_CLEAR_COLOR_1 | RD::DRAW_CLEAR_DEPTH, clear_colors, 1.0f, 0, Rect2(), 0);
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

// GOTOT-010: creates the mesh table GPU buffers + the batch assembly compute
// pipelines + the multi-batch draw pipeline, and registers the 008A cube as
// mesh table slot 0. Called from gpu_mesh_create (additive).
bool GototRenderServer::_init_mesh_table_gpu() {
	String error;

	auto compile_compute = [&](const char *p_glsl, const char *p_name, RID &r_shader) -> bool {
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

	int64_t inst = gpu_instance_count;

	mesh_id_buffer = rendering_device->storage_buffer_create((uint32_t)inst * 4);
	mesh_table_buffer = rendering_device->storage_buffer_create((uint32_t)GOTOT_MESH_TABLE_SIZE * sizeof(GototMeshDesc));
	mesh_color_buffer = rendering_device->storage_buffer_create((uint32_t)GOTOT_MESH_TABLE_SIZE * 16);
	batch_count_buffer = rendering_device->storage_buffer_create((uint32_t)GOTOT_MESH_TABLE_SIZE * 4);
	batch_offset_buffer = rendering_device->storage_buffer_create((uint32_t)GOTOT_MESH_TABLE_SIZE * 4);
	mesh_scratch_buffer = rendering_device->storage_buffer_create((uint32_t)(inst * GOTOT_MESH_TABLE_SIZE * 4));
	batch_instances_buffer = rendering_device->storage_buffer_create((uint32_t)inst * 4);
	batch_args_buffer = rendering_device->storage_buffer_create(
			(uint32_t)(GOTOT_MESH_TABLE_SIZE * 20), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	batch_total_buffer = rendering_device->storage_buffer_create(4);
	// GOTOT-011: grouping outputs. group_args holds non-indexed
	// VkDrawIndirectCommand[64] (16 bytes each, INDIRECT usage for the
	// procedural multi-draw); group_member_count/list hold the batch order
	// evidence (members per group + per-group mesh ids).
	group_args_buffer = rendering_device->storage_buffer_create(
			(uint32_t)(GOTOT_MESH_TABLE_SIZE * 16), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	group_member_count_buffer = rendering_device->storage_buffer_create((uint32_t)(GOTOT_MESH_TABLE_SIZE * 4));
	group_member_list_buffer = rendering_device->storage_buffer_create((uint32_t)(GOTOT_MESH_TABLE_SIZE * GOTOT_MESH_TABLE_SIZE * 4));
	if (mesh_id_buffer.is_null() || mesh_table_buffer.is_null() || mesh_color_buffer.is_null() ||
			batch_count_buffer.is_null() || batch_offset_buffer.is_null() || mesh_scratch_buffer.is_null() ||
			batch_instances_buffer.is_null() || batch_args_buffer.is_null() || batch_total_buffer.is_null() ||
			group_args_buffer.is_null() || group_member_count_buffer.is_null() || group_member_list_buffer.is_null()) {
		print_error("[GOTOT-NEXT] _init_mesh_table_gpu: buffer_create failed.");
		return false;
	}
	// All mesh_id entries default to 0 (the cube) until a test sets them.
	rendering_device->buffer_clear(mesh_id_buffer, 0, (uint32_t)inst * 4);

	for (int i = 0; i < GOTOT_MESH_TABLE_SIZE; i++) {
		mesh_colors[i] = Color(0, 0, 0, 1);
	}
	mesh_colors[0] = gotot_mesh_palette(0);
	rendering_device->buffer_update(mesh_color_buffer, 0, 16, &mesh_colors[0]);

	// Register the cube as mesh table slot 0 (shared buffer offset 0).
	GototMeshDesc cube_desc;
	memset(&cube_desc, 0, sizeof(cube_desc));
	cube_desc.index_buffer_slot = 0;
	cube_desc.vertex_buffer_slot = 0;
	cube_desc.index_count = 36;
	cube_desc.vertex_count = 8;
	cube_desc.first_index = 0;
	cube_desc.vertex_offset = 0;
	rendering_device->buffer_update(mesh_table_buffer, 0, (uint32_t)sizeof(GototMeshDesc), &cube_desc);
	mesh_table_count = 1;
	mesh_next_vertex_offset = 8;
	mesh_next_index_offset = 36;

	// Pass 1: per-mesh counting.
	if (!compile_compute(gpu_mesh_batch_count_glsl, "gotot_mesh_batch_count", batch_count_shader)) {
		return false;
	}
	batch_count_pipeline = rendering_device->compute_pipeline_create(batch_count_shader);
	Vector<RD::Uniform> bc_uniforms;
	const RID bc_buffers[4] = { compact_buffer, mesh_id_buffer, batch_count_buffer, mesh_scratch_buffer };
	for (uint32_t b = 0; b < 4; b++) {
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = b;
		u.append_id(bc_buffers[b]);
		bc_uniforms.push_back(u);
	}
	batch_count_uniform_set = rendering_device->uniform_set_create(bc_uniforms, batch_count_shader, 0);
	if (batch_count_uniform_set.is_null()) {
		print_error("[GOTOT-NEXT] _init_mesh_table_gpu: batch_count uniform_set_create failed.");
		return false;
	}

	// Pass 2: workgroup-parallel prefix sum + batch assembly + grouping.
	if (!compile_compute(gpu_mesh_batch_assemble_glsl, "gotot_mesh_batch_assemble", batch_assemble_shader)) {
		return false;
	}
	batch_assemble_pipeline = rendering_device->compute_pipeline_create(batch_assemble_shader);
	Vector<RD::Uniform> as_uniforms;
	const RID as_buffers[10] = {
		batch_count_buffer, mesh_table_buffer, batch_offset_buffer, batch_args_buffer,
		batch_instances_buffer, mesh_scratch_buffer, batch_total_buffer, group_args_buffer,
		group_member_count_buffer, group_member_list_buffer
	};
	for (uint32_t b = 0; b < 10; b++) {
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = b;
		u.append_id(as_buffers[b]);
		as_uniforms.push_back(u);
	}
	batch_assemble_uniform_set = rendering_device->uniform_set_create(as_uniforms, batch_assemble_shader, 0);
	if (batch_assemble_uniform_set.is_null()) {
		print_error("[GOTOT-NEXT] _init_mesh_table_gpu: batch_assemble uniform_set_create failed.");
		return false;
	}

	// Multi-batch draw pipeline (keeps 009 depth test/write behavior).
	Vector<uint8_t> vert_spirv = rendering_device->shader_compile_spirv_from_source(
			RD::SHADER_STAGE_VERTEX, String(gpu_mesh_batch_vert_glsl), RD::SHADER_LANGUAGE_GLSL, &error);
	if (vert_spirv.is_empty()) {
		print_error("[GOTOT-NEXT] mesh batch vertex shader compile failed:");
		print_error(error);
		return false;
	}
	Vector<uint8_t> frag_spirv = rendering_device->shader_compile_spirv_from_source(
			RD::SHADER_STAGE_FRAGMENT, String(gpu_mesh_batch_frag_glsl), RD::SHADER_LANGUAGE_GLSL, &error);
	if (frag_spirv.is_empty()) {
		print_error("[GOTOT-NEXT] mesh batch fragment shader compile failed:");
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
	mesh_batch_shader = rendering_device->shader_create_from_spirv(stages, "gotot_mesh_batch");
	if (mesh_batch_shader.is_null()) {
		print_error("[GOTOT-NEXT] mesh batch shader_create_from_spirv failed.");
		return false;
	}

	RD::PipelineRasterizationState rs;
	RD::PipelineMultisampleState ms;
	RD::PipelineDepthStencilState ds;
	ds.enable_depth_test = true;
	ds.enable_depth_write = true;
	ds.depth_compare_operator = RD::COMPARE_OP_LESS_OR_EQUAL;
	RD::PipelineColorBlendState bs = RD::PipelineColorBlendState::create_disabled(2);
	mesh_batch_pipeline = rendering_device->render_pipeline_create(
			mesh_batch_shader, raster_framebuffer_format, mesh_vertex_format, RD::RENDER_PRIMITIVE_TRIANGLES, rs, ms, ds, bs, 0, 0);
	if (mesh_batch_pipeline.is_null()) {
		print_error("[GOTOT-NEXT] mesh batch render_pipeline_create failed.");
		return false;
	}

	Vector<RD::Uniform> uniforms;
	const RID draw_buffers[5] = {
		batch_instances_buffer, transform_buffer, view_ubo, mesh_id_buffer, mesh_color_buffer
	};
	for (uint32_t b = 0; b < 5; b++) {
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = b;
		if (b == 2) {
			u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		}
		u.append_id(draw_buffers[b]);
		uniforms.push_back(u);
	}
	mesh_batch_uniform_set = rendering_device->uniform_set_create(uniforms, mesh_batch_shader, 0);
	if (mesh_batch_uniform_set.is_null()) {
		print_error("[GOTOT-NEXT] mesh batch uniform_set_create failed.");
		return false;
	}

	// GOTOT-011: grouped (procedural, NON-indexed) draw pipeline. The vertex
	// shader selects each instance's geometry sub-range through the mesh table,
	// so one VkDrawIndirectCommand per GROUP draws all of its member instances.
	// An EMPTY vertex format is used (geometry comes from the SSBO storage
	// mirrors); depth test/write matches the per-mesh batch path; the fragment
	// shader is the same early-Z one.
	{
		Vector<uint8_t> gvert_spirv = rendering_device->shader_compile_spirv_from_source(
				RD::SHADER_STAGE_VERTEX, String(gpu_mesh_group_batch_vert_glsl), RD::SHADER_LANGUAGE_GLSL, &error);
		if (gvert_spirv.is_empty()) {
			print_error("[GOTOT-NEXT] mesh group batch vertex shader compile failed:");
			print_error(error);
			return false;
		}
		Vector<uint8_t> gfrag_spirv = rendering_device->shader_compile_spirv_from_source(
				RD::SHADER_STAGE_FRAGMENT, String(gpu_mesh_batch_frag_glsl), RD::SHADER_LANGUAGE_GLSL, &error);
		if (gfrag_spirv.is_empty()) {
			print_error("[GOTOT-NEXT] mesh group batch fragment shader compile failed:");
			print_error(error);
			return false;
		}
		Vector<RD::ShaderStageSPIRVData> gstages;
		RD::ShaderStageSPIRVData gvs;
		gvs.shader_stage = RD::SHADER_STAGE_VERTEX;
		gvs.spirv = gvert_spirv;
		gstages.push_back(gvs);
		RD::ShaderStageSPIRVData gfs;
		gfs.shader_stage = RD::SHADER_STAGE_FRAGMENT;
		gfs.spirv = gfrag_spirv;
		gstages.push_back(gfs);
		group_batch_shader = rendering_device->shader_create_from_spirv(gstages, "gotot_mesh_group_batch");
		if (group_batch_shader.is_null()) {
			print_error("[GOTOT-NEXT] mesh group batch shader_create_from_spirv failed.");
			return false;
		}

		group_vertex_format = rendering_device->vertex_format_create(Vector<RD::VertexAttribute>());
		if (group_vertex_format < 0) {
			print_error("[GOTOT-NEXT] mesh group batch vertex_format_create failed.");
			return false;
		}
		group_vertex_array = rendering_device->vertex_array_create(1, group_vertex_format, Vector<RID>(), Vector<uint64_t>());
		if (group_vertex_array.is_null()) {
			print_error("[GOTOT-NEXT] mesh group batch vertex_array_create failed.");
			return false;
		}

		RD::PipelineRasterizationState grs;
		RD::PipelineMultisampleState gms;
		RD::PipelineDepthStencilState gds;
		gds.enable_depth_test = true;
		gds.enable_depth_write = true;
		gds.depth_compare_operator = RD::COMPARE_OP_LESS_OR_EQUAL;
		RD::PipelineColorBlendState gbs = RD::PipelineColorBlendState::create_disabled(2);
		group_batch_pipeline = rendering_device->render_pipeline_create(
				group_batch_shader, raster_framebuffer_format, group_vertex_format, RD::RENDER_PRIMITIVE_TRIANGLES, grs, gms, gds, gbs, 0, 0);
		if (group_batch_pipeline.is_null()) {
			print_error("[GOTOT-NEXT] mesh group batch render_pipeline_create failed.");
			return false;
		}

		Vector<RD::Uniform> guniforms;
		const RID gdraw_buffers[8] = {
			batch_instances_buffer, transform_buffer, view_ubo, mesh_id_buffer,
			mesh_color_buffer, mesh_table_buffer, mesh_vertex_storage_buffer, mesh_index_storage_buffer
		};
		for (uint32_t b = 0; b < 8; b++) {
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = b;
			if (b == 2) {
				u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
			}
			u.append_id(gdraw_buffers[b]);
			guniforms.push_back(u);
		}
		group_batch_uniform_set = rendering_device->uniform_set_create(guniforms, group_batch_shader, 0);
		if (group_batch_uniform_set.is_null()) {
			print_error("[GOTOT-NEXT] mesh group batch uniform_set_create failed.");
			return false;
		}
	}

	gpu_mesh_batch_valid = true;
	gpu_mesh_table_valid = true;
	last_batch_count = 0;
	last_group_count = 0;
	last_used_group_draw = false;
	print_line("[GOTOT-NEXT] Mesh table (GOTOT-010) initialized. slots=" + itos(mesh_table_count) +
			" max_verts=" + itos(GOTOT_MAX_MESH_VERTS) + " max_indices=" + itos(GOTOT_MAX_MESH_INDICES));
	return true;
}

void GototRenderServer::gpu_scene_set_instance_mesh(int p_index, int p_mesh_id) {
	if (!gpu_scene_valid || !gpu_mesh_batch_valid || p_index < 0 || p_index >= gpu_instance_count || mesh_id_buffer.is_null()) {
		return;
	}
	if (p_mesh_id < 0 || p_mesh_id >= mesh_table_count) {
		p_mesh_id = 0;
	}
	uint32_t v = (uint32_t)p_mesh_id;
	rendering_device->buffer_update(mesh_id_buffer, (uint32_t)(p_index * 4), 4, &v);
}

int GototRenderServer::gpu_scene_get_instance_mesh(int p_index) {
	if (!gpu_scene_valid || !gpu_mesh_batch_valid || p_index < 0 || p_index >= gpu_instance_count || mesh_id_buffer.is_null()) {
		return -1;
	}
	Vector<uint8_t> bytes = rendering_device->buffer_get_data(mesh_id_buffer, (uint32_t)(p_index * 4), 4);
	if (bytes.size() != 4) {
		return -1;
	}
	uint32_t v = 0;
	memcpy(&v, bytes.ptr(), 4);
	return (int)v;
}

int GototRenderServer::gpu_mesh_create_from_arrays(const PackedVector3Array &p_verts, const PackedInt32Array &p_indices) {
	if (!gpu_scene_valid || !gpu_mesh_valid || !gpu_mesh_batch_valid) {
		print_error("[GOTOT-NEXT] gpu_mesh_create_from_arrays: no batch mesh. Call gpu_mesh_create first.");
		return -1;
	}
	if (mesh_table_count >= GOTOT_MESH_TABLE_SIZE) {
		print_error("[GOTOT-NEXT] gpu_mesh_create_from_arrays: mesh table full.");
		return -1;
	}
	int vc = p_verts.size();
	int ic = p_indices.size();
	if (vc <= 0 || ic <= 0) {
		print_error("[GOTOT-NEXT] gpu_mesh_create_from_arrays: empty arrays.");
		return -1;
	}
	for (int i = 0; i < ic; i++) {
		if (p_indices[i] < 0 || p_indices[i] >= vc) {
			print_error("[GOTOT-NEXT] gpu_mesh_create_from_arrays: index out of range.");
			return -1;
		}
	}
	if (mesh_next_vertex_offset + vc > GOTOT_MAX_MESH_VERTS || mesh_next_index_offset + ic > GOTOT_MAX_MESH_INDICES) {
		print_error("[GOTOT-NEXT] gpu_mesh_create_from_arrays: shared buffer capacity exceeded.");
		return -1;
	}

	int vert_bytes = vc * 12;
	Vector<uint8_t> vbytes;
	vbytes.resize(vert_bytes);
	memcpy(vbytes.ptrw(), p_verts.ptr(), (size_t)vert_bytes);
	rendering_device->buffer_update(mesh_vertex_buffer, (uint32_t)(mesh_next_vertex_offset * 12), (uint32_t)vert_bytes, vbytes.ptr());
	if (mesh_vertex_storage_buffer.is_valid()) {
		rendering_device->buffer_update(mesh_vertex_storage_buffer, (uint32_t)(mesh_next_vertex_offset * 12), (uint32_t)vert_bytes, vbytes.ptr());
	}

	Vector<uint8_t> ibytes;
	ibytes.resize(ic * 4);
	const int32_t *src = p_indices.ptr();
	uint32_t *dst = (uint32_t *)ibytes.ptrw();
	for (int i = 0; i < ic; i++) {
		dst[i] = (uint32_t)src[i];
	}
	rendering_device->buffer_update(mesh_index_buffer, (uint32_t)(mesh_next_index_offset * 4), (uint32_t)(ic * 4), ibytes.ptr());
	if (mesh_index_storage_buffer.is_valid()) {
		rendering_device->buffer_update(mesh_index_storage_buffer, (uint32_t)(mesh_next_index_offset * 4), (uint32_t)(ic * 4), ibytes.ptr());
	}

	GototMeshDesc desc;
	memset(&desc, 0, sizeof(desc));
	desc.index_buffer_slot = 0;
	desc.vertex_buffer_slot = 0;
	desc.index_count = (uint32_t)ic;
	desc.vertex_count = (uint32_t)vc;
	desc.first_index = (uint32_t)mesh_next_index_offset;
	desc.vertex_offset = mesh_next_vertex_offset;
	rendering_device->buffer_update(mesh_table_buffer, (uint32_t)(mesh_table_count * sizeof(GototMeshDesc)), (uint32_t)sizeof(GototMeshDesc), &desc);

	Color c = gotot_mesh_palette(mesh_table_count);
	mesh_colors[mesh_table_count] = c;
	rendering_device->buffer_update(mesh_color_buffer, (uint32_t)(mesh_table_count * 16), 16, &c);

	int id = mesh_table_count;
	mesh_table_count++;
	mesh_next_vertex_offset += vc;
	mesh_next_index_offset += ic;
	print_line("[GOTOT-NEXT] GPU Mesh table entry added. mesh_id=" + itos(id) +
			" verts=" + itos(vc) + " indices=" + itos(ic) + " color=" + String(c));
	return id;
}

bool GototRenderServer::gpu_mesh_batch_dispatch() {
	if (!gpu_scene_valid || !gpu_mesh_valid || !gpu_mesh_batch_valid) {
		print_error("[GOTOT-NEXT] gpu_mesh_batch_dispatch: no batch mesh. Call gpu_mesh_create first.");
		return false;
	}
	if (!frustum_valid) {
		print_error("[GOTOT-NEXT] gpu_mesh_batch_dispatch: no camera. Call gpu_scene_set_camera first.");
		return false;
	}

	int visible = gpu_cull_get_visible_count();
	if (visible <= 0) {
		last_batch_count = 0;
		last_group_count = 0;
		last_used_group_draw = false;
		return true;
	}

	rendering_device->buffer_clear(batch_count_buffer, 0, (uint32_t)(GOTOT_MESH_TABLE_SIZE * 4));

	struct BatchCountParams {
		uint32_t instance_count;
		uint32_t scratch_stride;
		uint32_t pad0;
		uint32_t pad1;
	};
	BatchCountParams cp;
	cp.instance_count = (uint32_t)visible;
	cp.scratch_stride = (uint32_t)gpu_instance_count;
	cp.pad0 = 0;
	cp.pad1 = 0;
	uint32_t groups = (uint32_t)(((visible - 1) / 64) + 1);
	_run_compute_pass(batch_count_pipeline, batch_count_uniform_set, &cp, sizeof(cp), groups, 1, 1);

	// GOTOT-011 pass 2: workgroup-parallel prefix sum + grouping under the
	// current strategy. One workgroup (64 threads) covers the 64-slot table.
	struct BatchAssembleParams {
		uint32_t mesh_capacity;
		uint32_t scratch_stride;
		uint32_t strategy;
		uint32_t pad1;
	};
	BatchAssembleParams ap;
	ap.mesh_capacity = (uint32_t)GOTOT_MESH_TABLE_SIZE;
	ap.scratch_stride = (uint32_t)gpu_instance_count;
	ap.strategy = (uint32_t)mesh_batch_strategy;
	ap.pad1 = 0;
	_run_compute_pass(batch_assemble_pipeline, batch_assemble_uniform_set, &ap, sizeof(ap), 1, 1, 1);

	Vector<uint8_t> bytes = rendering_device->buffer_get_data(batch_total_buffer, 0, 4);
	if (bytes.size() == 4) {
		uint32_t t = 0;
		memcpy(&t, bytes.ptr(), 4);
		last_batch_count = (int)t;
	} else {
		last_batch_count = 0;
	}

	// GOTOT-011: derive the grouping state on the CPU with the SAME deterministic
	// formula as the workgroup assembler; batch_total (read from the GPU) stays
	// the authoritative indirect draw count (SPEC 011 section 3.2 fallback).
	int active = 0;
	{
		Vector<uint8_t> cb = rendering_device->buffer_get_data(batch_count_buffer, 0, (uint32_t)(GOTOT_MESH_TABLE_SIZE * 4));
		const uint32_t *fptr = (const uint32_t *)cb.ptr();
		int nn = cb.size() / 4;
		for (int i = 0; i < nn; i++) {
			if (fptr[i] > 0) {
				active++;
			}
		}
	}
	int G = active;
	if (mesh_batch_strategy == GOTOT_BATCH_STRATEGY_GROUPED || mesh_batch_strategy == GOTOT_BATCH_STRATEGY_REORDERED) {
		G = active > 5 ? 5 : active;
	}
	if (G <= 0) {
		G = 1;
	}
	last_group_count = G;
	last_used_group_draw = (G < active);
	int expect = last_used_group_draw ? G : active;
	if (last_batch_count != expect) {
		print_line("[GOTOT-NEXT] gpu_mesh_batch_dispatch: batch_total(" + itos(last_batch_count) +
				") != expected(" + itos(expect) + ") - using GPU value.");
	}
	return true;
}

bool GototRenderServer::gpu_mesh_batch_draw() {
	if (!gpu_scene_valid || !gpu_mesh_valid || !gpu_mesh_batch_valid) {
		print_error("[GOTOT-NEXT] gpu_mesh_batch_draw: no batch mesh. Call gpu_mesh_create first.");
		return false;
	}
	if (!frustum_valid) {
		print_error("[GOTOT-NEXT] gpu_mesh_batch_draw: no camera. Call gpu_scene_set_camera first.");
		return false;
	}

	// Phase-count buffers are cleared inside gpu_visibility_prod_dispatch
	// (where phase1/phase2 actually run).
	if (hzb_rebuild_requested && gpu_hzb_prod_valid) {
		rendering_device->buffer_clear(hzb_dbg_probe_buffer, 0, 16);
	}

	Vector<Color> clear_colors;
	clear_colors.push_back(Color(0, 0, 0, 0));
	clear_colors.push_back(Color(far_plane, 0.0f, 0.0f, 0.0f));
	RD::DrawListID dl = rendering_device->draw_list_begin(raster_framebuffer, RD::DRAW_CLEAR_COLOR_0 | RD::DRAW_CLEAR_COLOR_1 | RD::DRAW_CLEAR_DEPTH, clear_colors, 1.0f, 0, Rect2(), 0);
	if (dl == RD::INVALID_ID) {
		print_error("[GOTOT-NEXT] gpu_mesh_batch_draw: draw_list_begin failed.");
		return false;
	}
	rendering_device->draw_list_bind_render_pipeline(dl, mesh_batch_pipeline);
	rendering_device->draw_list_bind_uniform_set(dl, mesh_batch_uniform_set, 0);
	rendering_device->draw_list_bind_vertex_array(dl, mesh_010_vertex_array);
	rendering_device->draw_list_bind_index_array(dl, mesh_010_index_array);
	if (last_batch_count > 0) {
		if (last_used_group_draw) {
			// GOTOT-011 grouped/reordered: one PROCEDURAL (non-indexed)
			// VkDrawIndirectCommand per batch GROUP (<=5 for 64 visible meshes);
			// 16 = sizeof(VkDrawIndirectCommand). draw_count is the GPU-written,
			// frame-varying batch_total (SPEC 011 3.2 draw_indirect fallback).
			rendering_device->draw_list_bind_render_pipeline(dl, group_batch_pipeline);
			rendering_device->draw_list_bind_uniform_set(dl, group_batch_uniform_set, 0);
			rendering_device->draw_list_bind_vertex_array(dl, group_vertex_array);
			rendering_device->draw_list_draw_indirect(dl, false, group_args_buffer, 0, (uint32_t)last_batch_count, 16);
		} else {
			// Multi-draw (010 per-mesh path): draw_count == number of distinct
			// visible meshes; 20 = sizeof(VkDrawIndexedIndirectCommand).
			rendering_device->draw_list_draw_indirect(dl, true, batch_args_buffer, 0, (uint32_t)last_batch_count, 20);
		}
	}
	rendering_device->draw_list_end();

	// GOTOT-012: the production pyramid is built in gpu_visibility_prod_dispatch
	// (own synced submission) by projecting the registered occluder AABBs with
	// the occbuf pass into the flat storage buffer the phases read
	// cross-submission - the ONE reliable GPU route in this RDG fork (R32
	// attachment writes and compute-image loads are both stale). This draw
	// submission only patches the shared view UBO; nothing pyramid-related.
	int32_t otexel_count = HZB_PROD_TEXEL_COUNT;
	rendering_device->buffer_update(view_ubo, offsetof(GototViewData, viewport) + 2 * sizeof(float), 4, &otexel_count);
	int32_t oocc_count = occluder_count;
	rendering_device->buffer_update(view_ubo, offsetof(GototViewData, occ_count), 4, &oocc_count);

	if (gpu_hzb_prod_valid) {
		RD::ComputeListID cl = rendering_device->compute_list_begin();
		uint32_t ogroups = HZB_PROD_TEXEL_COUNT / 8;

		rendering_device->compute_list_end();
	}

	rendering_device->submit();
	rendering_device->sync();

	return true;
}

int GototRenderServer::gpu_mesh_get_mesh_id_count() const {
	return mesh_table_count;
}

int GototRenderServer::gpu_mesh_get_batch_count() {
	return last_batch_count;
}

PackedInt32Array GototRenderServer::gpu_mesh_get_draw_counts() {
	PackedInt32Array ret;
	if (!gpu_scene_valid || !gpu_mesh_batch_valid || batch_count_buffer.is_null()) {
		return ret;
	}
	Vector<uint8_t> bytes = rendering_device->buffer_get_data(batch_count_buffer, 0, (uint32_t)(GOTOT_MESH_TABLE_SIZE * 4));
	int n = bytes.size() / 4;
	ret.resize(n);
	const uint32_t *fptr = (const uint32_t *)bytes.ptr();
	for (int i = 0; i < n; i++) {
		ret.set(i, (int32_t)fptr[i]);
	}
	return ret;
}

PackedInt32Array GototRenderServer::gpu_mesh_get_batch_args(int p_batch_index) {
	PackedInt32Array ret;
	if (!gpu_scene_valid || !gpu_mesh_batch_valid || batch_args_buffer.is_null()) {
		return ret;
	}
	if (p_batch_index < 0 || p_batch_index >= GOTOT_MESH_TABLE_SIZE) {
		return ret;
	}
	Vector<uint8_t> bytes = rendering_device->buffer_get_data(batch_args_buffer, (uint32_t)(p_batch_index * 20), 20);
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

Color GototRenderServer::gpu_mesh_get_mesh_color(int p_mesh_id) const {
	if (p_mesh_id < 0 || p_mesh_id >= GOTOT_MESH_TABLE_SIZE) {
		return Color(0, 0, 0, 1);
	}
	return mesh_colors[p_mesh_id];
}

bool GototRenderServer::gpu_mesh_set_batch_strategy(int p_strategy) {
	if (p_strategy < GOTOT_BATCH_STRATEGY_PER_MESH || p_strategy > GOTOT_BATCH_STRATEGY_REORDERED) {
		print_error("[GOTOT-NEXT] gpu_mesh_set_batch_strategy: invalid strategy " + itos(p_strategy));
		return false;
	}
	mesh_batch_strategy = p_strategy;
	print_line("[GOTOT-NEXT] Batch strategy set to " + itos(p_strategy));
	return true;
}

int GototRenderServer::gpu_mesh_get_batch_strategy() const {
	return mesh_batch_strategy;
}

int GototRenderServer::gpu_mesh_get_batch_group_count() const {
	return last_group_count;
}

PackedInt32Array GototRenderServer::gpu_mesh_get_batch_order() {
	PackedInt32Array ret;
	if (!gpu_scene_valid || !gpu_mesh_batch_valid || group_member_list_buffer.is_null() || group_member_count_buffer.is_null()) {
		return ret;
	}
	int n = last_group_count;
	if (n <= 0 || n > GOTOT_MESH_TABLE_SIZE) {
		return ret;
	}
	Vector<uint8_t> mcbytes = rendering_device->buffer_get_data(group_member_count_buffer, 0, (uint32_t)(GOTOT_MESH_TABLE_SIZE * 4));
	if (mcbytes.size() != (int)(GOTOT_MESH_TABLE_SIZE * 4)) {
		return ret;
	}
	const uint32_t *mc = (const uint32_t *)mcbytes.ptr();
	for (int g = 0; g < n; g++) {
		uint32_t mg = mc[g];
		if (mg == 0 || mg > GOTOT_MESH_TABLE_SIZE) {
			continue;
		}
		Vector<uint8_t> bytes = rendering_device->buffer_get_data(group_member_list_buffer, (uint32_t)(g * GOTOT_MESH_TABLE_SIZE * 4), mg * 4);
		if (bytes.size() != (int)(mg * 4)) {
			continue;
		}
		const uint32_t *fptr = (const uint32_t *)bytes.ptr();
		for (uint32_t i = 0; i < mg; i++) {
			ret.push_back((int32_t)fptr[i]);
		}
	}
	return ret;
}

int GototRenderServer::gpu_mesh_get_indirect_count() const {
	// The draw count consumed by draw_list_draw_indirect. It originates on the
	// GPU (batch_total readback) and is frame-varying - the SPEC 011 section 3.2
	// fallback for the missing vkCmdDrawIndexedIndirectCount API.
	return last_batch_count;
}

int GototRenderServer::gpu_mesh_get_draw_call_count() const {
	// Number of indirect draw commands executed by the last gpu_mesh_batch_draw.
	return last_batch_count;
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
	clear_colors.push_back(Color(far_plane, 0.0f, 0.0f, 0.0f));
	RD::DrawListID dl = rendering_device->draw_list_begin(raster_framebuffer, RD::DRAW_CLEAR_COLOR_0 | RD::DRAW_CLEAR_COLOR_1 | RD::DRAW_CLEAR_DEPTH, clear_colors, 1.0f, 0, Rect2(), 0);
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

// GOTOT-012: read a single R32 view-z texel (bits as the depth source sees it).
float GototRenderServer::gpu_raster_read_viewz(int p_x, int p_y) {
	if (!gpu_scene_valid || !gpu_raster_valid) {
		return 0.0f;
	}
	if (p_x < 0 || p_x >= RASTER_TARGET_W || p_y < 0 || p_y >= RASTER_TARGET_H) {
		return 0.0f;
	}
	Vector<uint8_t> data = rendering_device->texture_get_data(raster_viewz_texture, 0);
	if (data.size() < ((p_y * RASTER_TARGET_W + p_x + 1) * 4)) {
		return 0.0f;
	}
	float v = 0.0f;
	memcpy(&v, data.ptr() + (p_y * RASTER_TARGET_W + p_x) * 4, 4);
	return v;
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

// GOTOT-013: Meshlets / LOD / cluster culling.
// See header block comment. The .gomlet format (tools/meshlet_import/main.cpp):
//   file header: "GOTOML11" + u32 version(1) + u32 lod_count + u32 reserved
//   per LOD: u32 vertex_count, u32 tri_count, u32 meshlet_count, u32 ref_count
//            vec4 positions[vertex_count], u32 refs[ref_count],
//            u8 micro_tris[3 * tri_count], desc[meshlet_count] (48 B each);
//   per-meshlet desc (48 B): u32 vertex_offset, u32 vertex_count,
//            u32 triangle_offset (u8 units), u32 triangle_count,
//            f32 center[3], f32 radius, f32 cone_axis[3], f32 cone_cutoff.
namespace {
// std140 layout shared by the meshlet cull/raster UBO. Mirrors GototViewData
// conventions (column-major vp, Godot plane test dot(n,p)+d used by 001B/004).
struct GOTOTMeshletViewData {
	float vp[16];          // 0
	float viewport[4];     // 64
	float planes[6][4];    // 80
	float cam[4];          // 176
	float far_plane;       // 192
	float pad_align[3];    // 196-207 padding to 16-byte boundary
	alignas(16) float pad0[3]; // 208-219 (shader vec3)
};
struct GpuMlLodDesc {
	uint32_t a[4]; // vert_base, tri_base, meshlet_ordinal, pad
	uint32_t b[4]; // vert_count, tri_count, meshlet_count, pad
	uint32_t c[4];
};
struct GpuMlMeshletDesc {
	uint32_t a[4]; // tri_base, vertex_count, triangle_count, pad
	float b[4];    // center.xyz, radius
	float c[4];    // cone_axis.xyz, cone_cutoff
};
struct MlCullPush {
	uint32_t ic;
	uint32_t ml0;
	uint32_t lod_count;
	uint32_t pad;
	float lod_t0;
	float lod_t1;
	float pad1;
	float pad2;
};
struct MlRasterPush {
	uint32_t ic;
	uint32_t ml0;
	uint32_t vis_w;
	uint32_t vis_h;
	uint32_t pass;
	uint32_t pad0;
	uint32_t pad1;
	uint32_t pad2;
};
static_assert(sizeof(GOTOTMeshletViewData) == 224, "meshlet view data layout");
static_assert(sizeof(GpuMlLodDesc) == 48, "lod desc layout");
static_assert(sizeof(GpuMlMeshletDesc) == 48, "meshlet desc layout");
static_assert(sizeof(MlCullPush) == 32, "cull push layout");
static_assert(sizeof(MlRasterPush) == 32, "raster push layout");

// Cluster cull: LOD by distance (world-space point distance), frustum (Godot
// plane convention), backface normal cone (meshoptimizer strict form). Dense
// deterministic mapping: thread (i, m) <-> slot i*ml0+m; no compaction atomics.
const char *gpu_meshlet_cull_glsl = R"(
#version 450
#extension GL_EXT_samplerless_texture_functions : enable

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform CullParams {
	uvec4 cfg; // x = instance_count, y = ml0_max, z = lod_count, w = 0
	vec4 thr;  // x = lod_t0, y = lod_t1
}
params;

layout(std140, set = 0, binding = 1) uniform MeshletView {
	mat4 vp;
	vec4 viewport;
	vec4 planes[6];
	vec4 cam;
	float far_plane;
	vec3 pad0;
}
mv;

layout(std430, set = 0, binding = 0) buffer TransformBuffer {
	vec4 position_scale[];
}
transforms;

layout(std430, set = 0, binding = 2) buffer DescBuffer {
	uvec4 desc[];
}
descs;

layout(std430, set = 0, binding = 3) buffer StateBuffer {
	uint data[];
}
state;

layout(std430, set = 0, binding = 4) buffer DebugBuffer {
	uint data[];
}
dbg;

const uint STATE_HEADER = 10u;

void main() {
	if (gl_GlobalInvocationID.x == 0u) {
		// Shader-side readback of exactly the fields the cull guard consumes.
		dbg.data[0] = uint(descs.desc[3u * 0u + 1u].z); // lod0 lod_mc
		dbg.data[1] = uint(descs.desc[3u * 1u + 1u].z); // lod1 lod_mc
		dbg.data[2] = uint(descs.desc[3u * 2u + 1u].z); // lod2 lod_mc
		dbg.data[3] = 12345u; // sanity marker
		dbg.data[4] = uint(descs.desc[3u * 0u + 0u].z); // lod0 ordinal
		dbg.data[5] = params.cfg.y; // ml0_max (cfg)
		dbg.data[6] = STATE_HEADER;
		dbg.data[7] = 0u;
		// Test B2: raw desc[0..1] dump (uvec4 per std430 vec4).
		dbg.data[8] = uint(descs.desc[0u].x); // LOD0 a[0] vert_base
		dbg.data[9] = uint(descs.desc[0u].y); // LOD0 a[1] tri_base
		dbg.data[10] = uint(descs.desc[0u].z); // LOD0 a[2] ordinal
		dbg.data[11] = uint(descs.desc[0u].w); // LOD0 a[3] pad
		dbg.data[12] = uint(descs.desc[1u].x); // LOD0 b[0] vc
		dbg.data[13] = uint(descs.desc[1u].y); // LOD0 b[1] tc
		dbg.data[14] = uint(descs.desc[1u].z); // LOD0 b[2] mc (expected 10905)
		dbg.data[15] = uint(descs.desc[1u].w); // LOD0 b[3] pad
	}
	uint gi = gl_GlobalInvocationID.x;
	uint ic = params.cfg.x;
	uint ml0 = params.cfg.y;
	uint lc = params.cfg.z;
	if (gi >= ic * ml0) {
		return;
	}
	uint i = gi / ml0;
	uint m = gi % ml0;

	vec4 ps = transforms.position_scale[i];
	vec3 inst_pos = ps.xyz;
	float inst_scale = ps.w;

	float dist = length(inst_pos - mv.cam.xyz);
	uint lod = dist < params.thr.x ? 0u : (dist < params.thr.y ? 1u : 2u);
	lod = min(lod, lc - 1u);

	if (m == 0u) {
		state.data[STATE_HEADER + i] = lod;
	}

	uint lod_mc = uint(descs.desc[3u * lod + 1u].z);
	if (m >= lod_mc) {
		return;
	}

	uint mn = uint(descs.desc[3u * lod + 0u].z);
	uvec4 da = descs.desc[3u * mn + 3u * m + 0u];
	vec4 db = uintBitsToFloat(descs.desc[3u * mn + 3u * m + 1u]);
	vec4 dc = uintBitsToFloat(descs.desc[3u * mn + 3u * m + 2u]);

	vec3 wc = inst_pos + inst_scale * db.xyz;
	float wr = inst_scale * db.w;

	bool visible = true;
	for (uint p = 0u; p < 6u; p++) {
		vec4 pl = mv.planes[p];
		if (dot(pl.xyz, wc) + pl.w < -wr) {
			visible = false;
		}
	}

	if (visible) {
		vec3 ctoc = wc - mv.cam.xyz;
		float clen = length(ctoc);
		float cd = dot(ctoc, dc.xyz) - (dc.w * clen + wr);
		if (cd >= 0.0) {
			visible = false;
		}
	}

	atomicAdd(state.data[0u], 1u);
	if (visible) {
		atomicAdd(state.data[1u], 1u);
		atomicAdd(state.data[4u + lod], 1u);
		state.data[STATE_HEADER + ic + gi] = 1u;
	}
}
)";

// Software rasterizer over the SURVIVING clusters. Passes (self-contained, only
// the surviving flag set from the last cull is shared):
//   pass 0 select:  per covered pixel atomicMax(z key) + atomicMax(~id key);
//                   sub-pixel triangles (no pixel-center hit) force-cover their
//                   centroid and bump the subpixel counter (SPEC 013 criterion 6).
//   pass 1 commit:  owner (z+id match) writes barycentric + lod and counts covered.
//   pass 2 cover:   owner per-LOD pixel counts (LOD evidence on the raster side).
// Each _run_compute_pass() ends with submit+sync, so pass N reads pass N-1's
// final atomics. Deterministic: ids are unique per (i,m,t), the per-pixel winner
// is the max ~id (min id), and every counter is a value-independent atomicAdd.
const char *gpu_meshlet_raster_glsl = R"(
#version 450
#extension GL_EXT_samplerless_texture_functions : enable
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : enable
#extension GL_EXT_shader_atomic_int64 : enable

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform RasterParams {
	uvec4 cfg;  // x = instance_count, y = ml0_max, z = vis_w, w = vis_h
	uvec4 pass; // x = 0|1|2
}
params;

layout(std140, set = 0, binding = 1) uniform MeshletView {
	mat4 vp;
	vec4 viewport;
	vec4 planes[6];
	vec4 cam;
	float far_plane;
	vec3 pad0;
}
mv;

layout(std430, set = 0, binding = 0) buffer TransformBuffer {
	vec4 position_scale[];
}
transforms;

layout(std430, set = 0, binding = 2) buffer DescBuffer {
	uvec4 desc[];
}
descs;

layout(std430, set = 0, binding = 3) buffer StateBuffer {
	uint data[];
}
state;

layout(std430, set = 0, binding = 4) buffer VertexBuffer {
	vec4 vdata[];
}
verts;

layout(std430, set = 0, binding = 5) buffer TriangleBuffer {
	uint tdata[];
}
tris;

layout(std430, set = 0, binding = 6) buffer VisBuffer {
	uint64_t key[]; // winner = single atomicMax((zkey<<32)|~idkey)
}
visbuf;

layout(std430, set = 0, binding = 7) buffer DebugBuffer {
	uint data[];
}
dbg;

const uint STATE_HEADER = 10u;

bool project_to_screen(vec3 wc, out vec2 sp, out float cw) {
	vec4 clip = mv.vp * vec4(wc, 1.0);
	cw = clip.w;
	if (cw <= 1e-4) {
		return false;
	}
	vec3 ndc = clip.xyz / cw;
	sp = vec2(ndc.x * 0.5 + 0.5, ndc.y * 0.5 + 0.5);
	return true;
}

float edge2(vec2 a, vec2 b, vec2 p) {
	return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

void main() {
	uint gi = gl_GlobalInvocationID.x;
	uint ic = params.cfg.x;
	uint ml0 = params.cfg.y;
	uint vis_w = params.cfg.z;
	uint vis_h = params.cfg.w;
	uint pass = params.pass.x;
	uint ivw = vis_w > 0u ? vis_w : 1u;

	if (gi >= ic * ml0) {
		return;
	}
	uint i = gi / ml0;
	uint m = gi % ml0;

	uint lod = state.data[STATE_HEADER + i];
	if (state.data[STATE_HEADER + ic + gi] == 0u) {
		return;
	}

	uint lod_vert_base = uint(descs.desc[3u * lod + 0u].x);
	uint mn = uint(descs.desc[3u * lod + 0u].z);
	uvec4 da = descs.desc[3u * mn + 3u * m + 0u];
	uint tri_base = uint(da.x);
	uint tri_count = uint(da.z);
	if (pass == 0u) atomicAdd(dbg.data[16u], tri_count);

	vec4 ps = transforms.position_scale[i];
	vec3 inst_pos = ps.xyz;
	float inst_scale = ps.w;

	for (uint t = 0u; t < tri_count; t++) {
		uint ti = tri_base + 3u * t;
		vec3 w0 = inst_pos + inst_scale * verts.vdata[tris.tdata[ti + 0u] + lod_vert_base].xyz;
		vec3 w1 = inst_pos + inst_scale * verts.vdata[tris.tdata[ti + 1u] + lod_vert_base].xyz;
		vec3 w2 = inst_pos + inst_scale * verts.vdata[tris.tdata[ti + 2u] + lod_vert_base].xyz;

		vec2 s0, s1, s2;
		float cw0, cw1, cw2;
		if (!project_to_screen(w0, s0, cw0) || !project_to_screen(w1, s1, cw1) || !project_to_screen(w2, s2, cw2)) {
			continue;
		}
		if (pass == 0u) atomicAdd(dbg.data[17u], 1u);
		s0 *= vec2(float(vis_w), float(vis_h)); // UV -> pixel
		s1 *= vec2(float(vis_w), float(vis_h));
		s2 *= vec2(float(vis_w), float(vis_h));

		if (pass == 0u && lod == 2u) {
			dbg.data[24u] = 1u;
			if (gl_GlobalInvocationID.x % 97u == 0u) {
				vec2 sp = (s0 + s1 + s2) / 3.0;
				dbg.data[25u] = uint(sp.x);
				dbg.data[26u] = uint(sp.y);
			}
		}

		vec2 smin = min(min(s0, s1), s2);
		vec2 smax = max(max(s0, s1), s2);
		int pxi0 = max(0, min(int(vis_w) - 1, int(floor(smin.x))));
		int pxi1 = max(0, min(int(vis_w) - 1, int(ceil(smax.x))));
		int pyi0 = max(0, min(int(vis_h) - 1, int(floor(smin.y))));
		int pyi1 = max(0, min(int(vis_h) - 1, int(ceil(smax.y))));

		bool hit = false;
		for (int py = pyi0; py <= pyi1; py++) {
			for (int px = pxi0; px <= pxi1; px++) {
				vec2 p = vec2(float(px) + 0.5, float(py) + 0.5);
				float e0 = edge2(s0, s1, p);
				float e1 = edge2(s1, s2, p);
				float e2 = edge2(s2, s0, p);
				bool inside = (e0 >= 0.0 && e1 >= 0.0 && e2 >= 0.0) || (e0 <= 0.0 && e1 <= 0.0 && e2 <= 0.0);
				if (!inside) {
					continue;
				}
				hit = true;
				float area = e0 + e1 + e2;
				float inv = area == 0.0 ? 0.0 : 1.0 / area;
				float b1 = clamp(e1 * inv, 0.0, 1.0);
				float b2 = clamp(e2 * inv, 0.0, 1.0);
				float wpx = (1.0 - b1 - b2) * cw0 + b1 * cw1 + b2 * cw2;
				uint pix = uint(py) * ivw + uint(px);
				uint zkey = floatBitsToUint(mv.far_plane + wpx);
				uint idkey = gi * 256u + t;
				if (pass == 0u) {
					atomicMax(visbuf.key[pix], (uint64_t(zkey) << 32) | uint64_t(~idkey));
					atomicAdd(state.data[7u + lod], 1u);
					atomicAdd(dbg.data[18u], 1u);
				} else {
					if (visbuf.key[pix] == ((uint64_t(zkey) << 32) | uint64_t(~idkey))) {
						if (pass == 1u) {
							atomicAdd(dbg.data[19u], 1u);
							atomicAdd(state.data[3u], 1u);
						} else {
							atomicAdd(state.data[7u + lod], 1u);
						}
					}
				}
			}
		}

		if (!hit) {
			vec2 centro = (s0 + s1 + s2) / 3.0;
			ivec2 cpx = ivec2(floor(centro));
			if (cpx.x >= 0 && cpx.x < int(vis_w) && cpx.y >= 0 && cpx.y < int(vis_h)) {
				uint pix = uint(cpx.y) * ivw + uint(cpx.x);
				float wpx = (cw0 + cw1 + cw2) / 3.0;
				uint zkey = floatBitsToUint(mv.far_plane + wpx);
				uint idkey = gi * 256u + t;
				if (pass == 0u) {
					atomicAdd(state.data[2u], 1u);
					atomicMax(visbuf.key[pix], (uint64_t(zkey) << 32) | uint64_t(~idkey));
					atomicAdd(state.data[7u + lod], 1u);
					atomicAdd(dbg.data[18u], 1u);
				} else {
					if (visbuf.key[pix] == ((uint64_t(zkey) << 32) | uint64_t(~idkey))) {
						if (pass == 1u) {
							atomicAdd(dbg.data[19u], 1u);
							atomicAdd(state.data[3u], 1u);
						} else {
							atomicAdd(state.data[7u + lod], 1u);
						}
					}
				}
			}
		}
	}
}
)";
} // namespace

void GototRenderServer::_destroy_meshlet() {
	if (rendering_device == nullptr) {
		gpu_meshlet_valid = false;
		return;
	}
	if (ml_raster_uniform_set.is_valid()) {
		rendering_device->free_rid(ml_raster_uniform_set);
		ml_raster_uniform_set = RID();
	}
	if (ml_cull_uniform_set.is_valid()) {
		rendering_device->free_rid(ml_cull_uniform_set);
		ml_cull_uniform_set = RID();
	}
	if (ml_raster_pipeline.is_valid()) {
		rendering_device->free_rid(ml_raster_pipeline);
		ml_raster_pipeline = RID();
	}
	if (ml_cull_pipeline.is_valid()) {
		rendering_device->free_rid(ml_cull_pipeline);
		ml_cull_pipeline = RID();
	}
	if (ml_raster_shader.is_valid()) {
		rendering_device->free_rid(ml_raster_shader);
		ml_raster_shader = RID();
	}
	if (ml_cull_shader.is_valid()) {
		rendering_device->free_rid(ml_cull_shader);
		ml_cull_shader = RID();
	}
	if (ml_vis_buffer.is_valid()) {
		rendering_device->free_rid(ml_vis_buffer);
		ml_vis_buffer = RID();
	}
	if (ml_state_buffer.is_valid()) {
		rendering_device->free_rid(ml_state_buffer);
		ml_state_buffer = RID();
	}
	if (ml_debug_buffer.is_valid()) {
		rendering_device->free_rid(ml_debug_buffer);
		ml_debug_buffer = RID();
	}
	if (ml_desc_buffer.is_valid()) {
		rendering_device->free_rid(ml_desc_buffer);
		ml_desc_buffer = RID();
	}
	if (ml_tri_buffer.is_valid()) {
		rendering_device->free_rid(ml_tri_buffer);
		ml_tri_buffer = RID();
	}
	if (ml_vert_buffer.is_valid()) {
		rendering_device->free_rid(ml_vert_buffer);
		ml_vert_buffer = RID();
	}
	if (ml_ubo.is_valid()) {
		rendering_device->free_rid(ml_ubo);
		ml_ubo = RID();
	}
	gpu_meshlet_valid = false;
	ml_lod_count = 0;
	ml0_max = 0;
	ml_instance_count = 0;
	ml_total_vertices = 0;
	ml_total_tris = 0;
	ml_total_meshlets = 0;
	ml_lod0_tri_count = 0;
	ml_lod0_meshlet_count = 0;
	ml_state_uint_count = 0;
}

bool GototRenderServer::_upload_meshlet_view() {
	GOTOTMeshletViewData vd;
	memset(&vd, 0, sizeof(vd));
	for (int i = 0; i < 16; i++) {
		vd.vp[i] = last_vp[i];
	}
	vd.viewport[0] = hzb_viewport_w;
	vd.viewport[1] = hzb_viewport_h;
	for (int i = 0; i < 6; i++) {
		vd.planes[i][0] = frustum_planes[i].normal.x;
		vd.planes[i][1] = frustum_planes[i].normal.y;
		vd.planes[i][2] = frustum_planes[i].normal.z;
		vd.planes[i][3] = frustum_planes[i].d;
	}
	vd.cam[0] = meshlet_camera_position[0];
	vd.cam[1] = meshlet_camera_position[1];
	vd.cam[2] = meshlet_camera_position[2];
	vd.far_plane = far_plane;
	return rendering_device->buffer_update(ml_ubo, 0, sizeof(GOTOTMeshletViewData), &vd) == OK;
}

bool GototRenderServer::gpu_meshlet_load(const PackedByteArray &p_data) {
	if (!ensure_gpu_device()) {
		print_error("[GOTOT-NEXT] gpu_meshlet_load: no RenderingDevice.");
		return false;
	}
	if (!gpu_scene_valid) {
		print_error("[GOTOT-NEXT] gpu_meshlet_load: no GPU scene. Call gpu_scene_create first.");
		return false;
	}

	_destroy_meshlet();

	const uint8_t *D = p_data.ptr();
	const int64_t bytes_size = p_data.size();
	if (bytes_size < 20) {
		print_error("[GOTOT-NEXT] gpu_meshlet_load: file too small.");
		return false;
	}
	if (memcmp(D, "GOTOML11", 8) != 0) {
		print_error("[GOTOT-NEXT] gpu_meshlet_load: bad magic (not a GOTOML11 file).");
		return false;
	}
	uint32_t version = 0, lod_count = 0;
	memcpy(&version, D + 8, 4);
	memcpy(&lod_count, D + 12, 4);
	if (version != 1) {
		print_error("[GOTOT-NEXT] gpu_meshlet_load: unsupported version.");
		return false;
	}
	if (lod_count < 1 || lod_count > ML_MAX_LODS) {
		print_error("[GOTOT-NEXT] gpu_meshlet_load: lod_count out of range.");
		return false;
	}

		struct MlLod {
		uint32_t vc, tc, mc, rc;
		uint32_t vert_base;
		uint32_t tri_base;
		uint32_t ordinal;
		int64_t pos_off, refs_off, micro_off, desc_off;
	};
	MlLod lods[ML_MAX_LODS];
	// Each LOD's 16-byte header [vc,tc,mc,rc] lives at the start of its own
	// strip (the tool writes header, then positions vc*16, refs rc*4, micro
	// 3*tc, meshlet descs mc*48, back-to-back). Walk strips to read headers.
	int64_t strip = 20;
	for (uint32_t l = 0; l < lod_count; l++) {
		if (strip + 16 > bytes_size) {
			print_error("[GOTOT-NEXT] gpu_meshlet_load: truncated LOD headers.");
			return false;
		}
		memcpy(&lods[l].vc, D + strip, 4);
		memcpy(&lods[l].tc, D + strip + 4, 4);
		memcpy(&lods[l].mc, D + strip + 8, 4);
		memcpy(&lods[l].rc, D + strip + 12, 4);
		if (lods[l].vc == 0 || lods[l].tc == 0 || lods[l].mc == 0) {
			print_error("[GOTOT-NEXT] gpu_meshlet_load: zero LOD metric.");
			return false;
		}
		// Record the exact byte offsets the build loop below relies on, then
		// advance to the next strip (header + positions + refs + micro + descs).
		lods[l].pos_off = strip + 16;
		if (lods[l].pos_off + (int64_t)lods[l].vc * 16 > bytes_size) {
			print_error("[GOTOT-NEXT] gpu_meshlet_load: LOD positions out of range.");
			print_error(String("[GOTOT-NEXT] 013dbg bytes=") + itos(bytes_size) + " lod=" + itos((int)l) + " vc=" + itos((int32_t)lods[l].vc) + " pos_off=" + itos((int64_t)(lods[l].pos_off)) + " need=" + itos((int64_t)(lods[l].pos_off + (int64_t)lods[l].vc * 16)));
			return false;
		}
		lods[l].refs_off = lods[l].pos_off + (int64_t)lods[l].vc * 16;
		if (lods[l].refs_off + (int64_t)lods[l].rc * 4 > bytes_size) {
			print_error("[GOTOT-NEXT] gpu_meshlet_load: LOD refs out of range.");
			return false;
		}
		lods[l].micro_off = lods[l].refs_off + (int64_t)lods[l].rc * 4;
		if (lods[l].micro_off + (int64_t)lods[l].tc * 3 > bytes_size) {
			print_error("[GOTOT-NEXT] gpu_meshlet_load: LOD micro tris out of range.");
			return false;
		}
		lods[l].desc_off = lods[l].micro_off + (int64_t)lods[l].tc * 3;
		if (lods[l].desc_off + (int64_t)lods[l].mc * 48 > bytes_size) {
			print_error("[GOTOT-NEXT] gpu_meshlet_load: LOD descs out of range.");
			return false;
		}
		strip += 16;
		int64_t end = strip;
		end += (int64_t)lods[l].vc * 16;
		end += (int64_t)lods[l].rc * 4;
		end += (int64_t)lods[l].tc * 3;
		end += (int64_t)lods[l].mc * 48;
		if (end > bytes_size) {
			print_error("[GOTOT-NEXT] gpu_meshlet_load: LOD strip out of range.");
			return false;
		}
		strip = end;
	}

	uint32_t vert_cum = 0, tri_cum = 0, ord_cum = lod_count;
	for (uint32_t l = 0; l < lod_count; l++) {
		lods[l].vert_base = vert_cum;
		lods[l].tri_base = tri_cum;
		lods[l].ordinal = ord_cum;
		vert_cum += lods[l].vc;
		tri_cum += lods[l].tc * 3;
		ord_cum += lods[l].mc;
	}

	ml_lod_count = (int)lod_count;
	ml0_max = (int)lods[0].mc;
	ml_instance_count = gpu_instance_count;
	ml_total_vertices = (int)vert_cum;
	ml_total_tris = 0;
	ml_total_meshlets = 0;
	ml_lod0_tri_count = (int)lods[0].tc;
	ml_lod0_meshlet_count = (int)lods[0].mc;
	for (uint32_t l = 0; l < lod_count; l++) {
		ml_total_tris += (int)lods[l].tc;
		ml_total_meshlets += (int)lods[l].mc;
	}
	if (ml0_max <= 0 || ml_instance_count <= 0) {
		print_error("[GOTOT-NEXT] gpu_meshlet_load: zero meshlet or instance count.");
		return false;
	}
	if ((int64_t)ml_instance_count * (int64_t)ml0_max > (int64_t)1 << 27) {
		print_error("[GOTOT-NEXT] gpu_meshlet_load: instance_count x ml0_max grid too large.");
		return false;
	}
	ml_state_uint_count = ML_STATE_HEADER + ml_instance_count + ml_instance_count * ml0_max;

	// Build the flat CPU buffers.
	Vector<uint8_t> vert_cpu;
	vert_cpu.resize((int64_t)ml_total_vertices * 16);
	Vector<uint8_t> tri_cpu;
	tri_cpu.resize((int64_t)ml_total_tris * 3 * 4);
	Vector<uint8_t> desc_cpu;
	desc_cpu.resize((int64_t)(lod_count + ml_total_meshlets) * 48);

	int64_t vp = 0;
	uint32_t tri_writer = 0;
	for (uint32_t l = 0; l < lod_count; l++) {
		MlLod &lod = lods[l];
		int64_t pos_off = lod.pos_off;
		int64_t refs_off = lod.refs_off;
		int64_t micro_off = lod.micro_off;
		int64_t desc_off = lod.desc_off;

		memcpy(vert_cpu.ptrw() + vp, D + pos_off, (size_t)lod.vc * 16);
		vp += (int64_t)lod.vc * 16;

		// LOD descriptor slot.
		GpuMlLodDesc *ld = (GpuMlLodDesc *)(desc_cpu.ptrw() + (int64_t)l * 48);
		ld->a[0] = lod.vert_base;
		ld->a[1] = lod.tri_base;
		ld->a[2] = lod.ordinal;
		ld->a[3] = 0;
		ld->b[0] = lod.vc;
		ld->b[1] = lod.tc;
		ld->b[2] = lod.mc;
		ld->b[3] = 0;
		memset(ld->c, 0, sizeof(ld->c));

		for (uint32_t m = 0; m < lod.mc; m++) {
			const uint8_t *md = D + desc_off + (int64_t)m * 48;
			uint32_t mo_vertex_offset, mo_vertex_count, mo_triangle_offset, mo_triangle_count;
			memcpy(&mo_vertex_offset, md, 4);
			memcpy(&mo_vertex_count, md + 4, 4);
			memcpy(&mo_triangle_offset, md + 8, 4);
			memcpy(&mo_triangle_count, md + 12, 4);
			const float *mo_center = (const float *)(md + 16);
			float mo_radius;
			memcpy(&mo_radius, md + 28, 4);
			const float *mo_cone = (const float *)(md + 32);
			float mo_cone_cutoff;
			memcpy(&mo_cone_cutoff, md + 44, 4);

			GpuMlMeshletDesc *gd = (GpuMlMeshletDesc *)(desc_cpu.ptrw() + (int64_t)(lod.ordinal + m) * 48);
			gd->a[0] = lod.tri_base + mo_triangle_offset;
			gd->a[1] = mo_vertex_count;
			gd->a[2] = mo_triangle_count;
			gd->a[3] = 0;
			memcpy(gd->b, mo_center, 3 * sizeof(float));
			gd->b[3] = mo_radius;
			memcpy(gd->c, mo_cone, 3 * sizeof(float));
			gd->c[3] = mo_cone_cutoff;

			for (uint32_t t = 0; t < mo_triangle_count; t++) {
				for (uint32_t k = 0; k < 3; k++) {
					uint8_t local = D[micro_off + (int64_t)mo_triangle_offset + (int64_t)t * 3 + k];
					uint32_t ref;
					memcpy(&ref, D + refs_off + (int64_t)(mo_vertex_offset + local) * 4, 4);
					memcpy(tri_cpu.ptrw() + (int64_t)(lod.tri_base + mo_triangle_offset + t * 3 + k) * 4, &ref, 4);
				}
			}
			tri_writer += mo_triangle_count * 3;
		}
	}
	if (tri_writer != tri_cum) {
		print_error("[GOTOT-NEXT] gpu_meshlet_load: triangle expansion mismatch.");
		return false;
	}

	// Upload.
	ml_ubo = rendering_device->uniform_buffer_create(sizeof(GOTOTMeshletViewData));
	if (!_upload_meshlet_view()) {
		print_error("[GOTOT-NEXT] gpu_meshlet_load: view UBO upload failed.");
		_destroy_meshlet();
		return false;
	}
	ml_vert_buffer = rendering_device->storage_buffer_create((uint32_t)((int64_t)ml_total_vertices * 16));
	rendering_device->buffer_update(ml_vert_buffer, 0, (uint32_t)((int64_t)ml_total_vertices * 16), vert_cpu.ptr());
	ml_tri_buffer = rendering_device->storage_buffer_create((uint32_t)((int64_t)ml_total_tris * 3 * 4));
	rendering_device->buffer_update(ml_tri_buffer, 0, (uint32_t)((int64_t)ml_total_tris * 3 * 4), tri_cpu.ptr());
	ml_desc_buffer = rendering_device->storage_buffer_create((uint32_t)((int64_t)(lod_count + ml_total_meshlets) * 48));
	rendering_device->buffer_update(ml_desc_buffer, 0, (uint32_t)((int64_t)(lod_count + ml_total_meshlets) * 48), desc_cpu.ptr());
	// Test A: CPU-side descriptor readback (proves the GPU descriptor buffer holds
	// the LOD headers the cull guard consumes: ord at +8, vc +16, tc +20, mc +24).
	Vector<uint8_t> desc_rb = rendering_device->buffer_get_data(ml_desc_buffer, 0, (uint32_t)lod_count * 48);
	for (uint32_t l = 0; l < lod_count; l++) {
		int32_t ord, vc, tc, mc;
		memcpy(&ord, desc_rb.ptr() + (int64_t)l * 48 + 8, 4);
		memcpy(&vc, desc_rb.ptr() + (int64_t)l * 48 + 16, 4);
		memcpy(&tc, desc_rb.ptr() + (int64_t)l * 48 + 20, 4);
		memcpy(&mc, desc_rb.ptr() + (int64_t)l * 48 + 24, 4);
		print_line("[GOTOT-NEXT] desc_cpu lod=" + itos(l) + " ord=" + itos(ord) + " vc=" + itos(vc) + " tc=" + itos(tc) + " mc=" + itos(mc));
	}
	ml_state_buffer = rendering_device->storage_buffer_create((uint32_t)ml_state_uint_count * 4);
	ml_debug_buffer = rendering_device->storage_buffer_create(128);
	ml_vis_buffer = rendering_device->storage_buffer_create((uint32_t)ml_vis_w * (uint32_t)ml_vis_h * 8);

	String cull_err, raster_err;
	Vector<uint8_t> cull_spv = rendering_device->shader_compile_spirv_from_source(
			RD::SHADER_STAGE_COMPUTE, String(gpu_meshlet_cull_glsl), RD::SHADER_LANGUAGE_GLSL, &cull_err);
	Vector<uint8_t> raster_spv = rendering_device->shader_compile_spirv_from_source(
			RD::SHADER_STAGE_COMPUTE, String(gpu_meshlet_raster_glsl), RD::SHADER_LANGUAGE_GLSL, &raster_err);
	if (cull_spv.is_empty() || raster_spv.is_empty()) {
		print_error("[GOTOT-NEXT] gpu_meshlet_load: shader compile failed:");
		print_error(cull_spv.is_empty() ? cull_err : raster_err);
		_destroy_meshlet();
		return false;
	}
	RD::ShaderStageSPIRVData stage;
	stage.shader_stage = RD::SHADER_STAGE_COMPUTE;
	stage.spirv = cull_spv;
	Vector<RD::ShaderStageSPIRVData> stages;
	stages.push_back(stage);
	ml_cull_shader = rendering_device->shader_create_from_spirv(stages, "gotot_meshlet_cull");
	if (ml_cull_shader.is_null()) {
		_destroy_meshlet();
		return false;
	}
	stage.spirv = raster_spv;
	Vector<RD::ShaderStageSPIRVData> stages2;
	stages2.push_back(stage);
	ml_raster_shader = rendering_device->shader_create_from_spirv(stages2, "gotot_meshlet_raster");
	if (ml_raster_shader.is_null()) {
		_destroy_meshlet();
		return false;
	}

	ml_cull_pipeline = rendering_device->compute_pipeline_create(ml_cull_shader);
	ml_raster_pipeline = rendering_device->compute_pipeline_create(ml_raster_shader);

	Vector<RD::Uniform> cull_uniforms;
	RD::Uniform cu0;
	cu0.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	cu0.binding = 0;
	cu0.append_id(transform_buffer);
	cull_uniforms.push_back(cu0);
	RD::Uniform cu1;
	cu1.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
	cu1.binding = 1;
	cu1.append_id(ml_ubo);
	cull_uniforms.push_back(cu1);
	RD::Uniform cu2;
	cu2.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	cu2.binding = 2;
	cu2.append_id(ml_desc_buffer);
	cull_uniforms.push_back(cu2);
	RD::Uniform cu3;
	cu3.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	cu3.binding = 3;
	cu3.append_id(ml_state_buffer);
	cull_uniforms.push_back(cu3);
	RD::Uniform cu4;
	cu4.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	cu4.binding = 4;
	cu4.append_id(ml_debug_buffer);
	cull_uniforms.push_back(cu4);
	ml_cull_uniform_set = rendering_device->uniform_set_create(cull_uniforms, ml_cull_shader, 0);
	if (ml_cull_uniform_set.is_null()) {
		print_error("[GOTOT-NEXT] gpu_meshlet_load: cull uniform_set_create failed.");
		_destroy_meshlet();
		return false;
	}

	Vector<RD::Uniform> raster_uniforms;
	const RID raster_buffers[8] = { transform_buffer, ml_ubo, ml_desc_buffer, ml_state_buffer, ml_vert_buffer, ml_tri_buffer, ml_vis_buffer, ml_debug_buffer };
	for (uint32_t b = 0; b < 8; b++) {
		RD::Uniform ru;
		ru.uniform_type = (b == 1) ? RD::UNIFORM_TYPE_UNIFORM_BUFFER : RD::UNIFORM_TYPE_STORAGE_BUFFER;
		ru.binding = b;
		ru.append_id(raster_buffers[b]);
		raster_uniforms.push_back(ru);
	}
	ml_raster_uniform_set = rendering_device->uniform_set_create(raster_uniforms, ml_raster_shader, 0);
	if (ml_raster_uniform_set.is_null()) {
		print_error("[GOTOT-NEXT] gpu_meshlet_load: raster uniform_set_create failed.");
		_destroy_meshlet();
		return false;
	}

	gpu_meshlet_valid = true;
	print_line("[GOTOT-NEXT] gpu_meshlet_load: lods=", ml_lod_count, " lod0_tris=", ml_lod0_tri_count,
			" lod0_meshlets=", ml_lod0_meshlet_count, " total_meshlets=", ml_total_meshlets,
			" verts=", ml_total_vertices, " tris=", ml_total_tris, " instances=", ml_instance_count);
	return true;
}

bool GototRenderServer::gpu_meshlet_load_path(const String &p_path) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
	if (f.is_null() || !f->is_open()) {
		print_error(String("[GOTOT-NEXT] gpu_meshlet_load_path: cannot open ") + p_path);
		return false;
	}
	Vector<uint8_t> bytes = f->get_buffer(f->get_length());
	f->close();
	PackedByteArray data;
	data.resize(bytes.size());
	if (bytes.size() > 0) {
		memcpy(data.ptrw(), bytes.ptr(), bytes.size());
	}
	return gpu_meshlet_load(data);
}

bool GototRenderServer::gpu_meshlet_set_lod_thresholds(float p_t0, float p_t1) {
	if (!(p_t0 > 0.0f && p_t1 > p_t0)) {
		print_error("[GOTOT-NEXT] gpu_meshlet_set_lod_thresholds: need 0 < t0 < t1.");
		return false;
	}
	ml_lod_t0 = p_t0;
	ml_lod_t1 = p_t1;
	return true;
}

bool GototRenderServer::gpu_meshlet_cull_dispatch() {
	if (!gpu_meshlet_valid || !gpu_scene_valid) {
		print_error("[GOTOT-NEXT] gpu_meshlet_cull_dispatch: meshlet not loaded or no scene.");
		return false;
	}
	if (!frustum_valid) {
		print_error("[GOTOT-NEXT] gpu_meshlet_cull_dispatch: no camera. Call gpu_scene_set_camera first.");
		return false;
	}
	_upload_meshlet_view();
	rendering_device->buffer_clear(ml_state_buffer, 0, (uint32_t)ml_state_uint_count * 4);

	MlCullPush push;
	push.ic = (uint32_t)ml_instance_count;
	push.ml0 = (uint32_t)ml0_max;
	push.lod_count = (uint32_t)ml_lod_count;
	push.pad = 0;
	push.lod_t0 = ml_lod_t0;
	push.lod_t1 = ml_lod_t1;
	push.pad1 = 0;
	push.pad2 = 0;
	uint64_t total = (uint64_t)ml_instance_count * (uint64_t)ml0_max;
	uint32_t groups = (uint32_t)((total + 63) / 64);
	_run_compute_pass(ml_cull_pipeline, ml_cull_uniform_set, &push, sizeof(MlCullPush), groups, 1, 1);
	return true;
}

bool GototRenderServer::gpu_meshlet_raster_dispatch() {
	if (!gpu_meshlet_valid || !gpu_scene_valid) {
		print_error("[GOTOT-NEXT] gpu_meshlet_raster_dispatch: meshlet not loaded or no scene.");
		return false;
	}
	uint64_t total = (uint64_t)ml_instance_count * (uint64_t)ml0_max;
	uint32_t groups = (uint32_t)((total + 63) / 64);
	MlRasterPush push;
	push.ic = (uint32_t)ml_instance_count;
	push.ml0 = (uint32_t)ml0_max;
	push.vis_w = (uint32_t)ml_vis_w;
	push.vis_h = (uint32_t)ml_vis_h;
	push.pad0 = push.pad1 = push.pad2 = 0;
	push.pass = 0;
	rendering_device->buffer_clear(ml_vis_buffer, 0, (uint32_t)ml_vis_w * (uint32_t)ml_vis_h * 8);
	rendering_device->buffer_clear(ml_debug_buffer, 16 * 4, 8 * 4);
	_run_compute_pass(ml_raster_pipeline, ml_raster_uniform_set, &push, sizeof(MlRasterPush), groups, 1, 1);
	push.pass = 1;
	_run_compute_pass(ml_raster_pipeline, ml_raster_uniform_set, &push, sizeof(MlRasterPush), groups, 1, 1);
	push.pass = 2;
	_run_compute_pass(ml_raster_pipeline, ml_raster_uniform_set, &push, sizeof(MlRasterPush), groups, 1, 1);
	return true;
}

int GototRenderServer::gpu_meshlet_get_total_meshlets() const {
	return ml_total_meshlets;
}

int GototRenderServer::gpu_meshlet_get_lod0_tri_count() const {
	return ml_lod0_tri_count;
}

int GototRenderServer::gpu_meshlet_get_lod0_meshlet_count() const {
	return ml_lod0_meshlet_count;
}

PackedInt32Array GototRenderServer::gpu_meshlet_stats() {
	PackedInt32Array ret;
	ret.append(ml_total_meshlets);
	ret.append(ml_lod0_tri_count);
	ret.append(ml_lod0_meshlet_count);
	ret.append(ml_total_vertices);
	ret.append(ml_total_tris);
	return ret;
}

PackedInt32Array GototRenderServer::gpu_meshlet_get_instance_lods() {
	PackedInt32Array ret;
	if (!gpu_meshlet_valid) {
		return ret;
	}
	Vector<uint8_t> bytes = rendering_device->buffer_get_data(ml_state_buffer, (uint32_t)ML_STATE_HEADER * 4, (uint32_t)ml_instance_count * 4);
	for (int i = 0; i < ml_instance_count; i++) {
		int32_t v;
		memcpy(&v, bytes.ptr() + i * 4, 4);
		ret.append(v);
	}
	return ret;
}

PackedInt32Array GototRenderServer::gpu_meshlet_get_cull_debug() {
	// binding-4 readback: [0..2]=lod_mc, [3]=12345 sanity, [4]=lod0 ordinal,
	// [5]=ml0_max cfg.y, [6]=STATE_HEADER, [7]=0u (all written on gl_GlobalInvocationID.x==0).
	PackedInt32Array ret;
	ret.resize(32);
	if (!gpu_meshlet_valid) {
		return ret;
	}
	Vector<uint8_t> bytes = rendering_device->buffer_get_data(ml_debug_buffer, 0, 128);
	for (int i = 0; i < 32; i++) {
		int32_t v;
		memcpy(&v, bytes.ptr() + i * 4, 4);
		ret.set(i, v);
	}
	return ret;
}

PackedInt32Array GototRenderServer::gpu_meshlet_get_cluster_counts() {
	PackedInt32Array ret;
	ret.resize(ML_STATE_HEADER);
	if (!gpu_meshlet_valid) {
		return ret;
	}
	Vector<uint8_t> bytes = rendering_device->buffer_get_data(ml_state_buffer, 0, ML_STATE_HEADER * 4);
	for (int i = 0; i < ML_STATE_HEADER; i++) {
		int32_t v;
		memcpy(&v, bytes.ptr() + i * 4, 4);
		ret.set(i, v);
	}
	return ret;
}

PackedFloat32Array GototRenderServer::gpu_meshlet_raster_evidence() {
	PackedFloat32Array ret;
	ret.resize(4);
	if (!gpu_meshlet_valid) {
		return ret;
	}
	Vector<uint8_t> hbytes = rendering_device->buffer_get_data(ml_state_buffer, 0, ML_STATE_HEADER * 4);
	int32_t covered = 0, subpixel = 0;
	memcpy(&subpixel, hbytes.ptr() + 2 * 4, 4);
	memcpy(&covered, hbytes.ptr() + 3 * 4, 4);

	Vector<uint8_t> vbytes = rendering_device->buffer_get_data(ml_vis_buffer, 0, (uint32_t)ml_vis_w * (uint32_t)ml_vis_h * 8);
	uint32_t fnv = 2166136261u;
	int32_t winner = 0;
	const int32_t count = ml_vis_w * ml_vis_h;
	for (int32_t i = 0; i < count; i++) {
		uint64_t c;
		memcpy(&c, vbytes.ptr() + (int64_t)i * 8, 8);
		uint32_t id = (uint32_t)(c & 0xFFFFFFFFu);
		if (id != 0) {
			winner++;
			fnv ^= id;
			fnv *= 16777619u;
		}
	}
	ret.set(0, (float)covered);
	ret.set(1, (float)subpixel);
	ret.set(2, (float)winner);
	ret.set(3, (float)fnv);
	return ret;
}

void GototRenderServer::gpu_meshlet_destroy() {
	_destroy_meshlet();
}
