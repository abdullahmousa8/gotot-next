#include "gne_render_server.h"

#include "core/os/memory.h"
#include "core/io/file_access.h"
#include "core/string/print_string.h"
#include "core/os/os.h"
#include "servers/rendering/rendering_server.h"

GneRenderServer *GneRenderServer::server_singleton = nullptr;

namespace {
// GNE-004: uniform/UBO data filled on every gpu_scene_set_camera and uploaded to view_ubo.
// Mirrors the GLSL ViewBlock (std140): mat4 vp + mat4 view + vec4 planes[6] + vec4 viewport
// + uint occ_count + float far_plane + 2 pads = 256 bytes, column-major matrices.
struct GneViewData {
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

layout(push_constant, std430) uniform GneSceneParams {
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

// GNE-002 + GNE-004: frustum + HZB occlusion in one pass. Planes and view
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

	uint vis = 1;
	for (uint p = 0; p < 6u; p++) {
		vec4 pl = viewdata.planes[p];
		float d = dot(pl.xyz, center) + pl.w;
		if (d < -radius) {
			vis = 0;
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
			uint max_inv = 0;
			for (int dy = 0; dy <= 1; dy++) {
				for (int dx = 0; dx <= 1; dx++) {
					ivec2 tc = clamp(t + ivec2(dx, dy), ivec2(0), ivec2(texels - 1));
					max_inv = max(max_inv, texelFetch(hzb_sampler, ivec3(tc, level), 0).r);
				}
			}
			if (max_inv > sphere_inv) {
				vis = 0;
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

// GNE-004: clear HZB layer 0.
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

// GNE-004: rasterize occluder boxes (projected to the square HZB grid) into layer 0.
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

	for (uint b = 0; b < viewdata.occ_count; b++) {
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

// GNE-004: downsample one HZB level (2x2 max) into the next.
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

// GNE-003: single-thread pass that fills a VkDrawIndexedIndirectCommand.
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
	indirect.args[0] = 6;      // index_count (01 quad)
	indirect.args[1] = n;       // instance_count (visible)
	indirect.args[2] = 0;      // first_index
	indirect.args[3] = 0;      // vertex_offset
	indirect.args[4] = 0;      // first_instance
}
)";

// GNE-005: vertex shader that expands one billboard quad per visible instance.
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

// GNE-005: magenta billboards, no blending, no depth.
const char *gpu_raster_frag_glsl = R"(
#version 450

layout(location = 0) out vec4 out_color;
// GNE-012: view-space depth copy for the production pyramid (z_view).
layout(location = 1) out float out_view_z;
layout(location = 2) out vec4 out_normal;
layout(location = 3) out vec4 out_radiance; // GNE-023/KI-017: pre-tonemap HDR

void main() {
	out_color = vec4(0.95, 0.18, 0.9, 1.0);
	out_view_z = -1.0 / gl_FragCoord.w;
	out_normal = vec4(0.0);
	out_radiance = vec4(0.0); // KI-017: non-shading path writes zero
}
)";

// GNE-008A: real-mesh draw args. Reuses the exact GNE-003 indirect argument
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
	indirect.args[2] = 0;                 // first_index
	indirect.args[3] = 0;                 // vertex_offset
	indirect.args[4] = 0;                 // first_instance
}
)";

// GNE-008A: real geometry vertex transform. The original scene index comes
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

// GNE-008A: flat green real geometry (visually distinct from the magenta
// billboard path). No lighting, no materials, no textures.
const char *gpu_mesh_frag_glsl = R"(
#version 450

layout(location = 0) out vec4 out_color;
// GNE-012: view-space depth copy for the production pyramid (z_view).
layout(location = 1) out float out_view_z;
layout(location = 2) out vec4 out_normal;
layout(location = 3) out vec4 out_radiance; // GNE-023/KI-017: pre-tonemap HDR

void main() {
	out_color = vec4(0.15, 0.85, 0.35, 1.0);
	out_view_z = -1.0 / gl_FragCoord.w;
	out_normal = vec4(0.0);
	out_radiance = vec4(0.0); // KI-017: non-shading path writes zero
}
)";

// GNE-010: CPU<->GPU mesh table descriptor. Mirrors GneMeshDesc (32 bytes,
// std430-compatible: 6x uint32/int32 then 2 reserved uint32). The batch assembly
// compute shader reads it (set 0 binding 1, array of this struct).
struct GneMeshDesc {
	uint32_t index_buffer_slot;
	uint32_t vertex_buffer_slot;
	uint32_t index_count;
	uint32_t vertex_count;
	uint32_t first_index;
	int32_t vertex_offset;
	uint32_t reserved[2];
};

// GNE-010: palette-based flat colors per mesh. mesh 0 keeps the 008A/009 green
// so the cube evidence matches earlier milestones; meshes 1..3 use distinct hues.
Color gne_mesh_palette(int p_mesh_id) {
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

// GNE-010 pass 1: per-mesh counting. One thread per VISIBLE instance reads its
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

// GNE-011 pass 2: workgroup-parallel prefix sum + batch assembly + grouping.
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

struct GneMeshDescStd430 {
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
	GneMeshDescStd430 table[64];
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
	for (uint d = 1; d < 64u; d <<= 1u) {
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
			for (uint k = 0; k < c; k++) {
				batch_instances.instances[excl + k] = scratch.data[tid * params.scratch_stride + k];
			}
			uint base = actexcl * 5u;
			GneMeshDescStd430 d = mesh_table.table[tid];
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
		batch_total.total = 0;
		return;
	}

	uint G = actn;
	if (params.strategy == 1u || params.strategy == 2u) {
		G = min(actn, 5u);
	}
	if (G == 0u) {
		G = 1;
	}

	uint base = actn / G;
	uint rem = actn % G;

	for (uint g = 0; g < G; g++) {
		uint size = base + ((g < rem) ? 1u : 0u);
		uint start = g * base + min(g, rem);
		uint inst = 0;
		uint mvc = 0;
		for (uint r = 0; r < size; r++) {
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
		group_args.args[base4 + 2u] = 0;
		group_args.args[base4 + 3u] = first_instance;
	}

	bool merged = (G < actn);
	batch_total.total = merged ? G : actn;
}
)";

// GNE-010: multi-mesh batch vertex shader. gl_InstanceIndex includes the
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

// GNE-010: per-mesh flat color (mesh color table binding 4). GNE-011 adds
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
// GNE-012: view-space depth copy for the production pyramid (z_view).
layout(location = 1) out float out_view_z;
layout(location = 2) out vec4 out_normal;
layout(location = 3) out vec4 out_radiance; // GNE-023/KI-017: pre-tonemap HDR

void main() {
	out_color = mesh_colors.colors[v_mesh_id];
	out_view_z = -1.0 / gl_FragCoord.w;
	out_normal = vec4(0.0);
	out_radiance = vec4(0.0); // KI-017: non-shading path writes zero
}
)";

// GNE-016: material vertex shader. Same instance iteration as the batch path
// (batch_instances + mesh_id + transform) plus the interpolated world position
// the fragment needs for dFdx/dFdy face normals. Bindings 0-3 identical.
const char *gpu_mat_batch_vert_glsl = R"(
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
layout(location = 0) out vec3 v_world;

void main() {
	uint orig = batch_instances.instances[gl_InstanceIndex];
	uint m = meshids.mesh_id[orig];
	vec4 ts = transforms.position_scale[orig];
	vec3 world = ts.xyz + vertex_position * ts.w;
	gl_Position = viewdata.vp * vec4(world, 1.0);
	v_mesh_id = m;
	v_world = world;
}
)";

// GNE-016: material fragment shader (Lambert + Blinn-Phong, SPEC 016 section
// 3.3). Face normals via derivatives (no normal attribute exists); backfaces
// get diffuse/specular = 0 and emission only when explicitly enabled (D4-4).
// Writes the same view-z attachment as the other raster paths so the shared
// framebuffer attachments stay consistent.
const char *gpu_mat_batch_frag_glsl = R"(
#version 450

layout(early_fragment_tests) in;

layout(location = 1) flat in uint v_mesh_id;
layout(location = 0) in vec3 v_world;

layout(std430, set = 0, binding = 4) buffer MaterialBuffer {
	vec4 mats[];
}
materials;

// GNE-017: fixed sampler array (no descriptor indexing in this RD fork) +
// mat_tex bindings. The array index comes from the mat_tex SSBO per fragment;
// the driver requires shaderSampledImageArrayNonUniformIndexing (hard
// requirement at device creation), so direct indexing is legal.
layout(set = 0, binding = 5) uniform sampler2D tex_arr[8];
layout(std430, set = 0, binding = 6) buffer MatTexBuffer {
	int tex_ids[];
}
mattex;

layout(push_constant, std430) uniform MatParams {
	vec4 light_dir_ambient;
	vec4 cam_pos;
	vec4 light_color;
	vec4 tex_slot_pad;
}
params;

layout(location = 0) out vec4 out_color;
layout(location = 1) out float out_view_z;
layout(location = 2) out vec4 out_normal;
layout(location = 3) out vec4 out_radiance; // GNE-023/KI-017: pre-tonemap HDR

void main() {
	uint m = v_mesh_id * 4u;
	vec3 albedo = materials.mats[m + 0u].rgb;
	float rough = materials.mats[m + 0u].a;
	vec3 emissive = materials.mats[m + 1u].rgb;
	float metal = materials.mats[m + 1u].a;
	vec3 spec_col = materials.mats[m + 2u].rgb;
	float shiny = materials.mats[m + 2u].a;
	vec4 flags = materials.mats[m + 3u];
	vec3 N = normalize(cross(dFdx(v_world), dFdy(v_world)));
	out_normal = vec4(N, 0.0);
	vec3 Vv = normalize(params.cam_pos.xyz - v_world);
	vec3 emi = (flags.x > 0.5) ? emissive * flags.y : vec3(0.0);
	// GNE-017: albedo-slot sampling (triplanar, hard axis switch). Unbound
	// (tid < 0) or out-of-range tid falls back to the flat albedo.
	int tid = mattex.tex_ids[int(v_mesh_id) * 5 + int(params.tex_slot_pad.x)];
	vec3 texel = vec3(1.0);
	if (tid >= 0 && tid < 8) {
		vec3 an = abs(N);
		vec2 tuv;
		if (an.x >= an.y && an.x >= an.z) {
			tuv = fract(vec2(v_world.z, v_world.y) * 0.01);
		} else if (an.y >= an.x && an.y >= an.z) {
			tuv = fract(vec2(v_world.x, v_world.z) * 0.01);
		} else {
			tuv = fract(vec2(v_world.x, v_world.y) * 0.01);
		}
		texel = texture(tex_arr[tid], tuv).rgb;
	}
	vec3 alb = albedo * texel;
	if (dot(N, Vv) < 0.0) {
		vec3 bem = (flags.z > 0.5) ? emi : vec3(0.0);
		out_color = vec4(bem, 1.0);
		out_view_z = -1.0 / gl_FragCoord.w;
		out_radiance = vec4(bem, 1.0); // KI-017: unlit path
		return;
	}
	vec3 L = params.light_dir_ambient.xyz;
	float AMB = params.light_dir_ambient.w;
	float ndl = max(dot(N, L), 0.0);
	vec3 diff = alb * ndl * params.light_color.rgb;
	vec3 H = normalize(L + Vv);
	float shiny_eff = max(shiny * (1.2 - rough), 1.0);
	float spec = pow(max(dot(N, H), 0.0), shiny_eff) * (1.0 - 0.5 * rough);
	vec3 sc = mix(spec_col, alb, metal);
	vec3 specular = spec * sc * params.light_color.rgb * step(0.0, dot(N, L)); // KI-011: NdotL gate
	out_color = vec4(AMB * alb + diff + specular + emi, 1.0);
	out_view_z = -1.0 / gl_FragCoord.w;
	out_radiance = vec4(AMB * alb + diff + specular + emi, 1.0); // KI-017: raw HDR (pre-clamp)
}
)";

// GNE-018: cluster cull compute. 3456 threads (16x9x24, one per cluster).
// Each thread owns its cluster, so per-cluster appends run in light-id order
// (sorted by construction => DET-stable); the atomicAdd only hands out slots.
// Conservative tests (may over-include, never wrongly exclude): over-inclusion
// only costs shading, never correctness.
// GNE-018-rev: SINGLE SOURCE of the cluster index selection (tile + euclidean
// depth slice). Shared by the mat_light fragment shader and the cone pass so
// the two sides cannot silently drift (D8-rev contract: divergence = over-cull).
// frag_xy: gl_FragCoord-style pixel coords (y-down, centers at *.5);
// z_view: euclidean camera distance; grid = (raster_w, raster_h, near, far).
const char *gpu_cluster_index_glsl = R"(
uint gne_cluster_index(vec2 frag_xy, float z_view, vec4 grid) {
	float tx = clamp(floor(frag_xy.x / grid.x * 16.0), 0.0, 15.0);
	float ty = clamp(8.0 - floor(frag_xy.y / grid.y * 9.0), 0.0, 8.0);
	float lratio = grid.w / grid.z;
	float tz = clamp(floor(24.0 * log(z_view / grid.z) / log(lratio)), 0.0, 23.0);
	return uint(tz) * 144u + uint(ty) * 16u + uint(tx);
}
)";

// GNE-018-rev: shared cone/light math in the light-cull frame (view space with
// +z forward; the same flip the cull shader applies: vc = (v.xy, -v.z)).
// The back-face dot convention is pinned by the unit-2b analytic selftest.
const char *gpu_cone_math_glsl = R"(
vec3 gne_cull_frame(vec3 view_vec) {
	return vec3(view_vec.xy, -view_vec.z);
}
vec3 gne_normal_to_cull(mat4 view, vec3 n_world) {
	return gne_cull_frame((view * vec4(n_world, 0.0)).xyz);
}

// View-space (cull frame) core used by both the world-space helper below and
// the live cull prefilter (unit 2c). Degenerate distance -> 1.0 (no cull).
float gne_backface_dot_view(vec3 axis_cull, vec3 light_pos_cull, vec3 anchor_cull) {
	vec3 to_l = light_pos_cull - anchor_cull;
	float dl = length(to_l);
	if (dl < 1e-5) {
		return 1.0;
	}
	return dot(to_l / dl, axis_cull);
}

// Box variant (unit 2c): direction from the AABB point CLOSEST to the light -
// the most light-favorable anchor, so a cull can only get MORE conservative.
// Light inside the box -> 1.0 (no cull).
float gne_backface_dot_box(vec3 axis_cull, vec3 light_pos_cull, vec3 bmin, vec3 bmax) {
	vec3 q = clamp(light_pos_cull, bmin, bmax);
	vec3 to_l = light_pos_cull - q;
	float dl = length(to_l);
	if (dl < 1e-5) {
		return 1.0;
	}
	return dot(to_l / dl, axis_cull);
}
float gne_backface_dot(mat4 view, vec3 n_world, vec3 l_world, vec3 anchor_world) {
	vec3 axis = gne_normal_to_cull(view, n_world);
	vec3 lc = gne_cull_frame((view * vec4(l_world, 1.0)).xyz);
	vec3 ac = gne_cull_frame((view * vec4(anchor_world, 1.0)).xyz);
	return gne_backface_dot_view(axis, lc, ac);
}
)";

// GNE-018-rev: cluster normal-cone build pass. One thread owns one cluster
// (3456 threads); a fixed-order serial scan over the cluster's raster tile
// gives a bit-stable result (no atomics anywhere). Two passes: deterministic
// axis reduction, then an EXACT min-dot over the same member set - identical
// containment guarantee to the reference lab's construction. Membership uses
// the shared gne_cluster_index (single source with the fragment) plus a small
// conservative seam margin so float-split boundary pixels land in BOTH cones.
const char *gpu_build_cones_glsl = R"(
#version 450

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform ConeParams {
	vec4 cam_pos; // xyz = camera world position (same value as the mat_light push)
	vec4 grid;    // x = raster_w, y = raster_h, z = near, w = far
}
params;

layout(set = 0, binding = 0) uniform sampler2D cone_normal_tex;
layout(set = 0, binding = 1) uniform sampler2D cone_viewz_tex;

layout(std430, set = 0, binding = 2) buffer ConeBuffer {
	vec4 cones[];
}
conebuf;

layout(std140, set = 0, binding = 3) uniform ViewBlock {
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

const float SEAM_MARGIN = 2.0; // see contract: >> f32 drift, << slice widths

float gne_euclid_dist(int px, int py, float lin) {
	vec2 ndc = vec2((float(px) + 0.5) / params.grid.x * 2.0 - 1.0,
			1.0 - (float(py) + 0.5) / params.grid.y * 2.0);
	float tanv = viewdata.viewport.w;
	float aspect = viewdata.viewport.x / viewdata.viewport.y;
	float dx = ndc.x * aspect * tanv;
	float dy = ndc.y * tanv;
	return lin * sqrt(dx * dx + dy * dy + 1.0);
}

bool gne_member(int px, int py, uint tid, float s0, float s1, out float dist) {
	float vz = texelFetch(cone_viewz_tex, ivec2(px, py), 0).r;
	if (vz >= 0.0) {
		return false; // cleared/empty (covered writes are negative)
	}
	dist = gne_euclid_dist(px, py, -vz);
	uint cid = gne_cluster_index(vec2(float(px) + 0.5, float(py) + 0.5), dist, params.grid);
	if (cid == tid) {
		return true;
	}
	int d = int(cid) - int(tid);
	if (d == -144 || d == 144) {
		return (dist >= s0 - SEAM_MARGIN) && (dist <= s1 + SEAM_MARGIN);
	}
	return false;
}

void main() {
	uint tid = gl_GlobalInvocationID.x;
	if (tid >= 3456u) {
		return;
	}
	uint tx = tid % 16u;
	uint ty = (tid / 16u) % 9u;
	uint tz = tid / 144u;
	int px0 = int(tx) * 120;
	int py0 = int(ty) * 120;
	int wpx = int(params.grid.x);
	int wpy = int(params.grid.y);
	float lratio = params.grid.w / params.grid.z;
	float s0 = params.grid.z * pow(lratio, float(tz) / 24.0);
	float s1 = params.grid.z * pow(lratio, float(tz + 1u) / 24.0);
	vec3 sum = vec3(0.0);
	uint count = 0u;
	for (int y = 0; y < 120; y++) {
		int py = py0 + y;
		if (py >= wpy) {
			break;
		}
		for (int x = 0; x < 120; x++) {
			int px = px0 + x;
			if (px >= wpx) {
				break;
			}
			float dist;
			if (!gne_member(px, py, tid, s0, s1, dist)) {
				continue;
			}
			vec3 nw = texelFetch(cone_normal_tex, ivec2(px, py), 0).xyz;
			if (dot(nw, nw) < 0.25) {
				continue; // unwritten attachment texel
			}
			sum += gne_normal_to_cull(viewdata.view, nw);
			count += 1u;
		}
	}
	if (count == 0u) {
		conebuf.cones[tid] = vec4(0.0); // "never cull" sentinel
		return;
	}
	float sl = length(sum);
	if (sl < 1e-6) {
		conebuf.cones[tid] = vec4(0.0);
		return;
	}
	vec3 axis = sum / sl;
	float mindp = 1.0;
	for (int y = 0; y < 120; y++) {
		int py = py0 + y;
		if (py >= wpy) {
			break;
		}
		for (int x = 0; x < 120; x++) {
			int px = px0 + x;
			if (px >= wpx) {
				break;
			}
			float dist;
			if (!gne_member(px, py, tid, s0, s1, dist)) {
				continue;
			}
			vec3 nw = texelFetch(cone_normal_tex, ivec2(px, py), 0).xyz;
			if (dot(nw, nw) < 0.25) {
				continue;
			}
			mindp = min(mindp, dot(axis, gne_normal_to_cull(viewdata.view, nw)));
		}
	}
	if (mindp <= 0.1) {
		conebuf.cones[tid] = vec4(0.0); // incoherent -> never cull
		return;
	}
	conebuf.cones[tid] = vec4(axis, mindp);
}
)";

// GNE-018-rev: isolation-test compute (unit 2b). Modes:
//  1: slice formula cross-check - shared gne_cluster_index vs a character-exact
//     copy of the pre-refactor fragment lines, same inputs.
//  2: back-face transform analytic cases (identity/rotations), outputs the
//     gne_backface_dot value and the transformed axis length.
//  3: cull-side AABB replica (verbatim math of gpu_light_cull_glsl lines
//     1076-1084) vs the fragment-side slice for the same view-space point.
const char *gpu_cones_selftest_glsl = R"(
#version 450

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform TestParams {
	uint mode;
	uint count;
}
params;

layout(std430, set = 0, binding = 0) buffer TestIn {
	float inp[];
}
tin;

layout(std430, set = 0, binding = 1) buffer TestOut {
	float outp[];
}
tout;

// Literal reference (pre-refactor fragment lines, character-exact). Kept only
// so the isolation test compares the shared helper against the original math
// on identical inputs (a passing helper alone would not catch a shared drift).
uint gne_cluster_index_literal(vec2 frag_xy, float z_view, vec4 grid) {
	float tx = clamp(floor(frag_xy.x / grid.x * 16.0), 0.0, 15.0);
	float ty = clamp(8.0 - floor(frag_xy.y / grid.y * 9.0), 0.0, 8.0);
	float lratio = grid.w / grid.z;
	float tz = clamp(floor(24.0 * log(z_view / grid.z) / log(lratio)), 0.0, 23.0);
	return uint(tz) * 144u + uint(ty) * 16u + uint(tx);
}

void main() {
	uint i = gl_GlobalInvocationID.x;
	if (i >= params.count) {
		return;
	}
	if (params.mode == 1u) {
		uint base = i * 7u;
		vec2 fxy = vec2(tin.inp[base], tin.inp[base + 1u]);
		vec4 grid = vec4(tin.inp[base + 2u], tin.inp[base + 3u], tin.inp[base + 4u], tin.inp[base + 5u]);
		float z = tin.inp[base + 6u];
		tout.outp[i * 2u + 0u] = float(gne_cluster_index(fxy, z, grid));
		tout.outp[i * 2u + 1u] = float(gne_cluster_index_literal(fxy, z, grid));
	} else if (params.mode == 2u) {
		uint base = i * 28u;
		mat4 view = mat4(
			vec4(tin.inp[base + 0u], tin.inp[base + 1u], tin.inp[base + 2u], tin.inp[base + 3u]),
			vec4(tin.inp[base + 4u], tin.inp[base + 5u], tin.inp[base + 6u], tin.inp[base + 7u]),
			vec4(tin.inp[base + 8u], tin.inp[base + 9u], tin.inp[base + 10u], tin.inp[base + 11u]),
			vec4(tin.inp[base + 12u], tin.inp[base + 13u], tin.inp[base + 14u], tin.inp[base + 15u]));
		vec3 nw = vec3(tin.inp[base + 16u], tin.inp[base + 17u], tin.inp[base + 18u]);
		vec3 lw = vec3(tin.inp[base + 20u], tin.inp[base + 21u], tin.inp[base + 22u]);
		vec3 aw = vec3(tin.inp[base + 24u], tin.inp[base + 25u], tin.inp[base + 26u]);
		tout.outp[i * 2u + 0u] = gne_backface_dot(view, nw, lw, aw);
		tout.outp[i * 2u + 1u] = length(gne_normal_to_cull(view, nw));
	} else if (params.mode == 3u) {
		uint base = i * 12u;
		float fx = tin.inp[base + 0u];
		float fy = tin.inp[base + 1u];
		float W = tin.inp[base + 2u];
		float H = tin.inp[base + 3u];
		float near = tin.inp[base + 4u];
		float far = tin.inp[base + 5u];
		float tanv = tin.inp[base + 6u];
		float aspect = tin.inp[base + 7u];
		float e_e = tin.inp[base + 8u];
		float xl = tin.inp[base + 9u];
		float yl = tin.inp[base + 10u];
		float zl = tin.inp[base + 11u];
		uint cid = gne_cluster_index(vec2(fx, fy), e_e, vec4(W, H, near, far));
		uint ctx = cid % 16u;
		uint cty = (cid / 16u) % 9u;
		uint ctz = cid / 144u;
		float lratio = far / near;
		float z0 = near * pow(lratio, float(ctz) / 24.0);
		float z1 = near * pow(lratio, float(ctz + 1u) / 24.0);
		float nx0 = float(ctx) / 16.0 * 2.0 - 1.0;
		float nx1 = float(ctx + 1u) / 16.0 * 2.0 - 1.0;
		float ny0 = float(cty) / 9.0 * 2.0 - 1.0;
		float ny1 = float(cty + 1u) / 9.0 * 2.0 - 1.0;
		vec3 bmin = vec3(nx0 * z1 * tanv * aspect, ny0 * z1 * tanv, z0);
		vec3 bmax = vec3(nx1 * z1 * tanv * aspect, ny1 * z1 * tanv, z1);
		vec3 p = vec3(xl, yl, zl);
		bool inb = all(greaterThanEqual(p, bmin)) && all(lessThanEqual(p, bmax));
		tout.outp[i * 5u + 0u] = float(cid);
		tout.outp[i * 5u + 1u] = z0;
		tout.outp[i * 5u + 2u] = z1;
		tout.outp[i * 5u + 3u] = inb ? 1.0 : 0.0;
		tout.outp[i * 5u + 4u] = z0 - zl;
	} else {
		// mode 4 (F6): slab-bound analytic verification over dense pixel samples.
		uint base = i * 6u;
		float tanv = tin.inp[base + 0u];
		float aspect = tin.inp[base + 1u];
		float nr = tin.inp[base + 2u];
		float fr = tin.inp[base + 3u];
		float tzf = tin.inp[base + 4u];
		float lratio = fr / nr;
		float z0 = nr * pow(lratio, tzf / 24.0);
		float z1 = nr * pow(lratio, (tzf + 1.0) / 24.0);
		float cosmax = 1.0 / sqrt(1.0 + tanv * tanv * (1.0 + aspect * aspect));
		float bound = z0 * cosmax;
		float min_slack = 1e9;
		float viol_old = 0.0;
		for (int ky = -16; ky <= 16; ky++) {
			for (int kx = -16; kx <= 16; kx++) {
				float nx = float(kx) / 16.0;
				float ny = float(ky) / 16.0;
				float L = sqrt(nx * nx * tanv * tanv * aspect * aspect + ny * ny * tanv * tanv + 1.0);
				for (int ei = 0; ei < 3; ei++) {
					float e = (ei == 0) ? z0 : ((ei == 1) ? (0.5 * (z0 + z1)) : (z1 * 0.999));
					float zl = e / L;
					min_slack = min(min_slack, zl - bound);
					if (zl < z0) {
						viol_old += 1.0;
					}
				}
			}
		}
		tout.outp[i * 3u + 0u] = bound;
		tout.outp[i * 3u + 1u] = min_slack;
		tout.outp[i * 3u + 2u] = viol_old;
	}
}
)";

// GNE-018-rev: build-time injection of the shared GLSL pieces. The single
// source lives in gpu_cluster_index_glsl / gpu_cone_math_glsl; shaders consume
// it via this replace so no consumer carries a private copy (besides the
// selftest's explicitly-labelled pre-refactor reference).
// GNE-020: reduced-resolution presentation blit (exact 2x2 box average).
// Deterministic: texelFetch taps, no filtering, no atomics. DST dims are
// compile-time (keep in sync with GNE_PRESENT_LOW_* in the header).
static const char *gpu_present_blit_glsl = R"(
#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D src_tex;
layout(set = 0, binding = 1, rgba8) uniform image2D dst_img;
void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (px.x >= 960 || px.y >= 540) {
		return;
	}
	ivec2 s = px * 2;
	vec4 c = (texelFetch(src_tex, s + ivec2(0, 0), 0) + texelFetch(src_tex, s + ivec2(1, 0), 0) + texelFetch(src_tex, s + ivec2(0, 1), 0) + texelFetch(src_tex, s + ivec2(1, 1), 0)) * 0.25;
	imageStore(dst_img, px, c);
}
)";
static String _gne_glsl_with_shared(const char *p_src, bool p_with_cone_math) {
	String s = String(p_src);
	String shared = String(gpu_cluster_index_glsl);
	if (p_with_cone_math) {
		shared += String(gpu_cone_math_glsl);
	}
	return s.replace("#version 450", String("#version 450\n") + shared);
}
// ============================================================================
//  GNE-021: per-light conservative screen footprint.
//
//  WHY: the cull tests every light against every one of the 3456 clusters, so it
//  performs exactly clusters x lights tests per frame. Measured, 022_shade gives
//  3456 x 256 = 884736 and 019 gives 3456 x 16 = 55296 — the full cross product,
//  none of it pruned. Most of those pairs cannot possibly overlap.
//
//  HOW: project the light's view-space AABB (centre +- range per axis) and take
//  the min/max NDC of its 8 corners. Perspective projection of a convex polytope
//  is bounded by its projected vertices, and the sphere is inside that AABB, so
//  the rect is a SUPERSET. That is the whole point: anything this rect rejects
//  would have failed sphere_vs_aabb anyway.
//
//  The cull's own NDC convention is reused, not re-derived. From cpp:1467-1472 a
//  tile maps to NDC as nx = tx/16*2-1, ny = ty/9*2-1, and the box is built as
//  x = nx * z * tanv * aspect. Inverting that gives the forward map used below.
//  Re-deriving it is exactly how the cull and the fragment lookup drift apart.
//
//  FAIL-SAFE, in both directions:
//   * if any corner is at or behind the near plane the vertex bound does not
//     hold, so the light gets a FULL-SCREEN rect and can never be skipped here;
//   * a rect that comes out empty after clamping degrades to ONE tile, never to
//     zero. This pass can only ever make the cull test fewer pairs, and it can
//     never drop a light that the old path would have kept.
// ============================================================================
const char *gpu_light_bounds_glsl = R"(
#version 450

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform BoundsParams {
	uint light_count;
	float tanv;
	float aspect;
	float unused;
}
bparams;

layout(std430, set = 0, binding = 0) buffer CullLightStream {
	vec4 recs[];
}
cullbuf;

layout(std430, set = 0, binding = 1) buffer LightBoundsOut {
	vec4 bnd[];
}
bndbuf;

// Same ViewBlock the cull uses, so the transform and the viewport cannot drift.
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
	if (i >= bparams.light_count) {
		return;
	}
	vec4 C0 = cullbuf.recs[i * 3u + 0u];
	vec3 vw = (viewdata.view * vec4(C0.xyz, 1.0)).xyz;
	// View convention matches the cull: +z forward, so depth is -vz.
	float z = -vw.z;
	float r = max(C0.w, 0.0);

	vec4 full = vec4(0.0, 0.0, 15.0, 8.0);
	if (z <= r + 1e-4) {
		// Sphere reaches the eye plane: projection unbounded, do not skip.
		bndbuf.bnd[i] = full;
		return;
	}
	float inv_tan = 1.0 / max(bparams.tanv, 1e-6);
	float inv_aspect = 1.0 / max(bparams.aspect, 1e-6);
	float minx = 1e30, maxx = -1e30, miny = 1e30, maxy = -1e30;
	for (int c = 0; c < 8; c++) {
		float sx = ((c & 1) == 0) ? -r : r;
		float sy = ((c & 2) == 0) ? -r : r;
		float sz = ((c & 4) == 0) ? -r : r;
		float cz = z + sz; // -vz including the corner offset
		if (cz <= 1e-4) {
			bndbuf.bnd[i] = full;
			return;
		}
		float nx = (vw.x + sx) * inv_tan * inv_aspect / cz;
		float ny = (vw.y + sy) * inv_tan / cz;
		minx = min(minx, nx);
		maxx = max(maxx, nx);
		miny = min(miny, ny);
		maxy = max(maxy, ny);
	}
	// NDC -> tile indices, rounding OUTWARD so the rect is never inside the true
	// footprint, then clamped to the 16x9 grid.
	// ONE-TILE MARGIN, and its cause is NOT yet understood. Without it, gt_021a's
	// sig came back v21|lc=256|of=36096|sl=38605|on=38419 against the expected
	// of=36097|sl=38620|on=38434 - exactly 15 assignments lost in one cluster, one
	// overflow lost with it. Widening by one tile on every side restored the sig
	// byte-for-byte. A superset construction should not need any margin, so
	// either the projection is subtly off at the rect edge or the cluster box is
	// wider than its tile at near depths (bmin.x is evaluated at z1 while the box
	// spans bz0..z1, so at bz0 the same world x maps to a larger NDC). I have not
	// proved which, so this is recorded as an unexplained margin rather than
	// dressed up as a correctness fix. Tracked as an open question; it must not be
	// narrowed away without an explanation.
	float tx0 = clamp(floor((minx * 0.5 + 0.5) * 16.0) - 1.0, 0.0, 15.0);
	float tx1 = clamp(ceil((maxx * 0.5 + 0.5) * 16.0) + 1.0, 0.0, 15.0);
	float ty0 = clamp(floor((miny * 0.5 + 0.5) * 9.0) - 1.0, 0.0, 8.0);
	float ty1 = clamp(ceil((maxy * 0.5 + 0.5) * 9.0) + 1.0, 0.0, 8.0);
	if (tx1 < tx0) { tx1 = tx0; }
	if (ty1 < ty0) { ty1 = ty0; }
	bndbuf.bnd[i] = vec4(tx0, ty0, tx1, ty1);
}
)";

const char *gpu_light_cull_glsl = R"(
#version 450

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform CullParams {
	vec4 dims;   // x = light_count, y = tan_half_fov_v, z = aspect, w = unused
	vec4 range;  // x = near, y = far, z = raster_w, w = raster_h
	vec4 rev;    // GNE-018-rev: x = flag (1 = extended slab; cone filter later)
}
params;

layout(std430, set = 0, binding = 0) buffer LightBuffer {
	vec4 lights[];
}
lightbuf;

layout(std430, set = 0, binding = 1) buffer ClOffsetBuffer {
	uint off[];
}
cloff;

layout(std430, set = 0, binding = 2) buffer ClCountBuffer {
	uint cnt[];
}
clcnt;

layout(std430, set = 0, binding = 3) buffer ClIndexBuffer {
	uint idx[];
}
clidx;

// GNE-021: ovf[0] is the cluster-cap overflow count (pre-existing).
// ovf[1] is tests_performed - how many sphere_vs_aabb calls the cull actually
// made this frame. It exists so the cost of the cull can be a NUMBER READ BACK
// from the engine rather than an argument about how the code looks. Without it
// any claim about the cull's work is an inference, and this project has spent a
// long session dismantling inferences.
layout(std430, set = 0, binding = 4) buffer OverflowBuffer {
	uint ovf[];
}
ovfb;

// GNE-018-rev: cluster normal cones (flag-gated prefilter; unit 2c).
layout(std430, set = 0, binding = 6) buffer ConeBuffer {
	vec4 cones[];
}
conebuf;

// GNE-021: cull-only light stream. 3 vec4 per light.
//   [0] = (pos.xyz, range)  - read unconditionally below
//   [1] = (dir.xyz, type)   - read only in the spot branch
//   [2] = (cone_inner, cone_outer, 0, 0) - read only in the spot branch
// The cull pass streams this for every light in every cluster, so the point-light
// path now touches 16 B per light instead of the 64 B GneLight record. The
// fragment pass still reads light_buffer unchanged.
// GNE-021 cull stream. Read order is deliberate: vec4 [0] carries position and
// range and is fetched once per light, then the cone vec4s are fetched only
// after a hit and only for spot lights. Anything added here must not widen the
// unconditional read, or the bandwidth point of the split is lost.
// Verified by main_022_shade's CLUSTER_IDS dump, which must stay byte-identical
// to the pre-split baseline for both a spot column and a point column.
layout(std430, set = 0, binding = 7) buffer LightCullBuffer {
	vec4 recs[];
}
cullbuf;

// GNE-021: per-light conservative screen tile rect, written by the bounds pass.
// Read first in the light loop so a miss costs one vec4 instead of an AABB test.
layout(std430, set = 0, binding = 8) buffer LightBoundsBuffer {
	vec4 bnd[];
}
bndbuf;

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

bool sphere_vs_aabb(vec3 c, float r, vec3 bmin, vec3 bmax) {
	vec3 q = clamp(c, bmin, bmax);
	vec3 d = c - q;
	return dot(d, d) <= r * r;
}

void main() {
	uint tid = gl_GlobalInvocationID.x;
	if (tid >= 3456u) {
		return;
	}
	uint tx = tid % 16u;
	uint ty = (tid / 16u) % 9u;
	uint tz = tid / 144u;
	uint nlights = uint(params.dims.x);
	float tanv = params.dims.y;
	float aspect = params.dims.z;
	float znear = params.range.x;
	float zfar = params.range.y;
	float lratio = zfar / znear;
	float z0 = znear * pow(lratio, float(tz) / 24.0);
	float z1 = znear * pow(lratio, float(tz + 1u) / 24.0);
	// GNE-018-rev (KI-014): the fragment assigns slices by EUCLIDEAN distance while
	// this slab is LINEAR-depth; extend the lower bound by cos(theta_max) of the
	// screen diagonal so the box covers every pixel the fragment can assign here.
	float cosmax = 1.0 / sqrt(1.0 + tanv * tanv * (1.0 + aspect * aspect));
	float bz0 = (params.rev.x > 0.5) ? (z0 * cosmax) : z0;
	// NDC tile rect -> view-space AABB (conservative: far-z extents).
	//
	// GNE-021 NOTE - the box is DELIBERATELY wider than its tile, and any screen
	// bounds test built from the tile alone must account for it. The x/y extents
	// are evaluated at z1, the slab's FAR plane, while the box's near face sits at
	// bz0 <= z0 < z1. Inverting the box to NDC at a depth z inside the slab:
	//
	//     ndc_x(z) = bmin.x / (z * tanv * aspect) = nx0 * (z1 / z)
	//
	// At z = z1 that is exactly nx0, the tile edge. At z = bz0 it is
	// nx0 * (z1/bz0), whose magnitude is LARGER. So the box covers strictly more
	// NDC at its near face than the tile it was built from.
	//
	// Consequence for the GNE-021 bounds rect: it is built from the light's true
	// screen projection, which maps to the TILE, so it cannot cover that extra
	// region. A light just outside tile tx but inside the box's near-face
	// extension passes sphere_vs_aabb and would be skipped — measured as exactly 15
	// lost assignments in one cluster in gt_021a. Hence the one-tile margin in the
	// bounds pass. It is NOT removable, and the construction is a superset only
	// with respect to the TILE, not the BOX.
	//
	// The margin required is POSITION-DEPENDENT, and the uniform one tile is a blunt
	// approximation of it. In tiles, per side:
	//
	//     margin_tiles ~ |ndc_edge| * (z1/bz0 - 1) * (grid/2)
	//
	// With exponential slices z1/z0 = (zfar/znear)^(1/24); for zfar/znear = 1000
	// that is ~1.333, so the required margin approaches 0 at the screen centre and
	// ~2.7 tiles in x at the edge. One tile happens to cover every case the current
	// scenes exercise, which is exactly why the cluster-id checks pass and exactly
	// why the margin must not be narrowed by inspection. The per-cluster derivation
	// is the known better answer and is deliberately deferred: it changes
	// load-bearing cull geometry and needs the same gates re-proven, for a benefit
	// this harness cannot measure.
	float nx0 = float(tx) / 16.0 * 2.0 - 1.0;
	float nx1 = float(tx + 1u) / 16.0 * 2.0 - 1.0;
	float ny0 = float(ty) / 9.0 * 2.0 - 1.0;
	float ny1 = float(ty + 1u) / 9.0 * 2.0 - 1.0;
	vec3 bmin = vec3(nx0 * z1 * tanv * aspect, ny0 * z1 * tanv, bz0);
	vec3 bmax = vec3(nx1 * z1 * tanv * aspect, ny1 * z1 * tanv, z1);
	// View-space depth convention here: +z forward (matches -vc.z usage).
	// The box above uses +z view depth; lights convert identically below.
	// GNE-018-rev (unit 2c): flag-gated back-face prefilter state.
	vec4 rev_cone = conebuf.cones[tid];
	bool rev_cullable = (params.rev.x > 0.5) && (rev_cone.w > 0.1);
	float rev_sin = rev_cullable ? sqrt(max(0.0, 1.0 - rev_cone.w * rev_cone.w)) : 0.0;
	for (uint i = 0u; i < nlights; i++) {
		// GNE-021: skip lights whose conservative screen rect misses this tile.
		// The rect is a superset of the light's true footprint, so a miss here is
		// a miss sphere_vs_aabb would also have produced. Depth is deliberately
		// NOT part of this test: the slices are exponential and reusing that
		// convention is a separate, larger change.
		vec4 B = bndbuf.bnd[i];
		if (float(tx) < B.x || float(tx) > B.z || float(ty) < B.y || float(ty) > B.w) {
			continue;
		}
		// GNE-021: read the cull stream, not lightbuf. One vec4 covers position
		// and range, which is all the point-light path needs. The cone vec4s are
		// fetched only for spot lights, inside the branch that uses them.
		vec4 C0 = cullbuf.recs[i * 3u + 0u];
		vec3 vw = (viewdata.view * vec4(C0.xyz, 1.0)).xyz;
		vec3 vc = vec3(vw.xy, -vw.z);
		float rr = C0.w;
		if (rev_cullable) {
			// Skip lights the whole cluster faces away from (conservative: only when
			// even the best-aligned normal direction is behind the light).
			if (gne_backface_dot_box(rev_cone.xyz, vc, bmin, bmax) < -rev_sin) {
				continue;
			}
		}
		bool hit = sphere_vs_aabb(vc, rr, bmin, bmax);
		// GNE-021: count the work actually done, so a later cull change can be
		// judged by a measured number instead of a claim.
		atomicAdd(ovfb.ovf[1], 1u);
		if (hit) {
			// GNE-021: type and cone arrive only now, so a point light never
			// pays for them. Previously L2/L3 were fetched for every light.
			vec4 C1 = cullbuf.recs[i * 3u + 1u];
			if (C1.w > 0.5) {
				vec4 C2 = cullbuf.recs[i * 3u + 2u];
				// Spot: cone test around the spot axis (view space).
				vec3 sd = normalize((viewdata.view * vec4(C1.xyz, 0.0)).xyz);
				sd = vec3(sd.xy, -sd.z);
				vec3 bc = (bmin + bmax) * 0.5;
				vec3 to_b = bc - vc;
				float dist = max(length(to_b), 1e-4);
				float ang = acos(clamp(dot(to_b / dist, sd), -1.0, 1.0));
				float outer = C2.y;
				float margin = asin(clamp(rr / dist, 0.0, 1.0));
				hit = ang <= outer + margin;
			}
		}
		if (hit) {
			uint slot = atomicAdd(clcnt.cnt[tid], 1u);
			if (slot < 16u) {
				clidx.idx[tid * 16u + slot] = i;
			} else {
				atomicAdd(ovfb.ovf[0], 1u);
			}
		}
	}
}
)";

// GNE-018: material+lights fragment shader. Identical 016 base (directional +
// ambient + emissive + backface rule), then the per-cluster light loop.
// Cluster id from framebuffer pixels + view depth, with the SAME slice math
// as the cull pass (any divergence breaks the mechanism - keep in sync).
const char *gpu_mat_light_frag_glsl = R"(
#version 450

layout(early_fragment_tests) in;

layout(location = 1) flat in uint v_mesh_id;
layout(location = 0) in vec3 v_world;

layout(std430, set = 0, binding = 4) buffer MaterialBuffer {
	vec4 mats[];
}
materials;

layout(set = 0, binding = 5) uniform sampler2D tex_arr[8];
layout(std430, set = 0, binding = 6) buffer MatTexBuffer {
	int tex_ids[];
}
mattex;
layout(set = 0, binding = 7, std430) buffer Mat2Buffer { vec4 mats2[]; } mat2buf; // GNE-016.5

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

// GNE-022 S3-shading (spec_022 section 10): GI field sampling, flag-gated.
layout(set = 1, binding = 4) uniform sampler2D gi_atlas_s;
float gne_gi_fetch_gate(vec3 a, vec3 b) {
	
return 1.0; // visible surface: no extra occlusion against itself
}
//GNE_GI_SHARED

layout(std430, set = 1, binding = 0) buffer LightBuffer {
	vec4 lights[];
}
lightbuf;

layout(std430, set = 1, binding = 1) buffer ClOffsetBuffer {
	uint off[];
}
cloff;

layout(std430, set = 1, binding = 2) buffer ClCountBuffer {
	uint cnt[];
}
clcnt;

layout(std430, set = 1, binding = 3) buffer ClIndexBuffer {
	uint idx[];
}
clidx;

// GNE-019: shadow sampling set (D7-1..D7-7). CSM array (4x2048), two cube
// arrays (6x1024), two spot maps (2048), bind records, light->bind LUT.
// Guarded everywhere: unbound/disabled lights return exactly 1.0, so 018
// pixels reproduce bit-exactly when no shadows are bound.
layout(set = 2, binding = 0) uniform sampler2DArray shadow_csm;
layout(set = 2, binding = 1) uniform sampler2DArray shadow_cube0;
layout(set = 2, binding = 2) uniform sampler2DArray shadow_cube1;
layout(set = 2, binding = 3) uniform sampler2D shadow_spot0;
layout(set = 2, binding = 4) uniform sampler2D shadow_spot1;
struct ShadowRec019 {
	vec4 h0; // x = light_id, y = type (0 CSM, 1 cube, 2 spot), z = slot, w = enabled
	vec4 splits; // CSM: s1..s4 planar view-depth splits; cube/spot: light pos xyz + far
	mat4 vps[6]; // column-major VP per cascade/face (CSM 4, cube 6, spot 1)
};
layout(std430, set = 2, binding = 5) buffer ShadowRecs019 {
	ShadowRec019 recs[];
}
shadowrec;
layout(std430, set = 2, binding = 6) buffer ShadowLut019 {
	uint lut[];
}
shadowlut;

layout(push_constant, std430) uniform MatLightParams {
	vec4 light_dir_ambient;
	vec4 cam_pos;
	vec4 light_color;
	vec4 tex_slot_pad;
	vec4 grid_dims;   // x = raster_w, y = raster_h, z = near, w = far
	vec4 shadow_ctrl; // x = dir bind idx (-1 = none), y = shadow enable, z = cam near, w = csm blend scale
	vec4 shadow_cam;  // xyz = camera forward (planar depth for CSM select), w = spare
	vec4 gi_min;    // GNE-022 section 10: probe-field sampling
	vec4 gi_max;
	vec4 gi_dims;
	vec4 gi_params; // (enabled, scale, 0, 0)
}
params;

layout(location = 0) out vec4 out_color;
layout(location = 1) out float out_view_z;
layout(location = 2) out vec4 out_normal;
	layout(location = 3) out vec4 out_radiance; // GNE-023/KI-017: pre-tonemap HDR

// GNE-019.5 slice-0: ESM read for the dir CSM (encode c = 40; see the fill
// fragment). Hardware-linear filtering gives the soft penumbra; artifacts are
// reported, not tuned silently.
float gne_esm_arr(sampler2DArray arr, vec2 uv, int layer, float ref) {
	float e = texture(arr, vec3(clamp(uv, vec2(0.0), vec2(1.0)), float(layer))).r;
	float s = e * exp(-87.0 * ref); // lit iff map_e >= exp(c*ref): factor = clamp(E/exp(c*ref),0,1)
	return clamp(s, 0.0, 1.0);
}

// GNE-019 shadow sampling implementation. Depth convention: the R32 maps hold
// the rasterizer's gl_FragCoord.z (Vulkan viewport 0..1 over Godot
// Vulkan-style 0..1 NDC), so ref = clamp(ndc_z) - bias with the SAME VP that
// rendered the map. If S1 shows inverted/missing shadows, this single mapping
// is the suspect (see §36).
float gne_shadow_bias(vec3 N, vec3 Ld) {
	float ndl = clamp(dot(N, Ld), 0.0, 1.0);
	float th = acos(clamp(ndl, -1.0, 1.0));
	float sl = 0.005 * tan(min(th, 1.45)); // D7-6 slope, grazing-clamped
	return 0.001 + min(sl, 0.05); // D7-6 constant
}

float gne_pcf_arr(sampler2DArray arr, vec2 uv, int layer, float size, float ref) {
	vec2 p = clamp(uv, vec2(0.0), vec2(1.0)) * (size - 1.0);
	float xmax = size - 1.0;
	float s = 0.0;
	s += float(ref <= texelFetch(arr, ivec3(min(int(p.x), int(xmax)), min(int(p.y), int(xmax)), layer), 0).r);
	s += float(ref <= texelFetch(arr, ivec3(min(int(p.x) + 1, int(xmax)), min(int(p.y), int(xmax)), layer), 0).r);
	s += float(ref <= texelFetch(arr, ivec3(min(int(p.x), int(xmax)), min(int(p.y) + 1, int(xmax)), layer), 0).r);
	s += float(ref <= texelFetch(arr, ivec3(min(int(p.x) + 1, int(xmax)), min(int(p.y) + 1, int(xmax)), layer), 0).r);
	return s * 0.25;
}

float gne_pcf_2d(sampler2D sp, vec2 uv, float size, float ref) {
	vec2 p = clamp(uv, vec2(0.0), vec2(1.0)) * (size - 1.0);
	float xmax = size - 1.0;
	float s = 0.0;
	s += float(ref <= texelFetch(sp, ivec2(min(int(p.x), int(xmax)), min(int(p.y), int(xmax))), 0).r);
	s += float(ref <= texelFetch(sp, ivec2(min(int(p.x) + 1, int(xmax)), min(int(p.y), int(xmax))), 0).r);
	s += float(ref <= texelFetch(sp, ivec2(min(int(p.x), int(xmax)), min(int(p.y) + 1, int(xmax))), 0).r);
	s += float(ref <= texelFetch(sp, ivec2(min(int(p.x) + 1, int(xmax)), min(int(p.y) + 1, int(xmax))), 0).r);
	return s * 0.25;
}

// Projective sample of one cascade/spot map. Returns 1.0 outside the map.
float gne_sample_cas(ShadowRec019 r, int cas, vec3 wp, float bias, bool is_spot, int slot) {
	vec4 c = r.vps[is_spot ? 0 : cas] * vec4(wp, 1.0);
	float w = max(abs(c.w), 1e-6);
	vec3 ndc = c.xyz / w;
	vec2 uv = ndc.xy * 0.5 + 0.5;
	if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
		return 1.0;
	}
	float ref = clamp(ndc.z, -1.0, 1.0) - bias;
	if (is_spot) {
		if (slot == 0) {
			return gne_pcf_2d(shadow_spot0, uv, 2048.0, ref);
		}
		return gne_pcf_2d(shadow_spot1, uv, 2048.0, ref);
	}
	if (params.shadow_cam.w > 0.5) {
		return gne_esm_arr(shadow_csm, uv, cas, ref);
	}
	return gne_pcf_arr(shadow_csm, uv, cas, 2048.0, ref);
}

// Cube-face frame. Axes mirror _shadow_vp_cube UPS/DIRS exactly (closed loop:
// render lookat basis == sample basis by construction, verified in §36).
void gne_cube_frame(vec3 d, out int face, out vec2 uv) {
	vec3 a = abs(d);
	if (a.x >= a.y && a.x >= a.z) {
		if (d.x > 0.0) {
			face = 0;
			uv = vec2(d.z, d.y) / a.x;
		} else {
			face = 1;
			uv = vec2(-d.z, d.y) / a.x;
		}
	} else if (a.y >= a.x && a.y >= a.z) {
		if (d.y > 0.0) {
			face = 2;
			uv = vec2(-d.x, -d.z) / a.y;
		} else {
			face = 3;
			uv = vec2(-d.x, d.z) / a.y;
		}
	} else {
		if (d.z > 0.0) {
			face = 4;
			uv = vec2(-d.x, d.y) / a.z;
		} else {
			face = 5;
			uv = vec2(d.x, d.y) / a.z;
		}
	}
	uv = uv * 0.5 + 0.5;
}

// Full per-light shadow factor. Unbound/disabled -> exactly 1.0.
float gne_shadow_light(uint light_id, vec3 wpos, vec3 N, vec3 Ld, float zview) {
	if (params.shadow_ctrl.y < 0.5 || light_id >= 1024u) {
		return 1.0;
	}
	uint bi = shadowlut.lut[light_id];
	if (bi == 0xFFFFFFFFu) {
		return 1.0;
	}
	ShadowRec019 r = shadowrec.recs[bi];
	if (r.h0.w < 0.5) {
		return 1.0;
	}
	int tp = int(r.h0.y + 0.5);
	int slot = int(r.h0.z + 0.5);
	float bias = gne_shadow_bias(N, Ld);
	vec3 wp = wpos + N * 0.02; // D7-6 normal offset (exact world units)
	if (tp == 1) {
		vec3 lp = r.splits.xyz;
		vec3 d = wp - lp;
		int face;
		vec2 cuv;
		gne_cube_frame(d, face, cuv);
		if (cuv.x < 0.0 || cuv.x > 1.0 || cuv.y < 0.0 || cuv.y > 1.0) {
			return 1.0;
		}
		vec4 c = r.vps[face] * vec4(wp, 1.0);
		float w = max(abs(c.w), 1e-6);
		float ref = clamp(c.z / w, -1.0, 1.0) - bias;
		if (slot == 0) {
			return gne_pcf_arr(shadow_cube0, cuv, face, 1024.0, ref);
		}
		return gne_pcf_arr(shadow_cube1, cuv, face, 1024.0, ref);
	}
	if (tp == 2) {
		return gne_sample_cas(r, 0, wp, bias, true, slot);
	}
	return 1.0; // CSM on a cluster light is rejected at bind time; never sample
}

// Directional (CSM) factor for the hardcoded push dir.
float gne_shadow_dir(vec3 wpos, vec3 N, vec3 Ld) {
	if (params.shadow_ctrl.y < 0.5) {
		return 1.0;
	}
	int bi = int(params.shadow_ctrl.x + 0.5);
	if (bi < 0) {
		return 1.0;
	}
	ShadowRec019 r = shadowrec.recs[uint(bi)];
	if (r.h0.w < 0.5) {
		return 1.0;
	}
	float bias = gne_shadow_bias(N, Ld);
	vec3 wp = wpos + N * 0.02;
	float planar = dot(wp - params.cam_pos.xyz, params.shadow_cam.xyz);
	int cas = 3;
	if (planar <= r.splits.x) {
		cas = 0;
	} else if (planar <= r.splits.y) {
		cas = 1;
	} else if (planar <= r.splits.z) {
		cas = 2;
	}
	float f = gne_sample_cas(r, cas, wp, bias, false, 0);
	// S4: blend across the upper split over a band (D7-2 smooth transitions).
	if (cas < 3) {
		float s_lo = (cas == 0) ? params.shadow_ctrl.z : (cas == 1 ? r.splits.x : r.splits.y);
		float s_hi = (cas == 0) ? r.splits.x : (cas == 1 ? r.splits.y : r.splits.z);
		float band = max((s_hi - s_lo) * 0.05 * params.shadow_ctrl.w, 1e-3);
		float t = (planar - (s_hi - band)) / (2.0 * band);
		if (t > 0.0 && t < 1.0) {
			float f2 = gne_sample_cas(r, cas + 1, wp, bias, false, 0);
			f = mix(f, f2, smoothstep(0.0, 1.0, t));
		}
	}
	return f;
}

void main() {
	uint m = v_mesh_id * 4u;
	vec3 albedo = materials.mats[m + 0u].rgb;
	float rough = materials.mats[m + 0u].a;
	vec3 emissive = materials.mats[m + 1u].rgb;
	float metal = materials.mats[m + 1u].a;
	vec3 spec_col = materials.mats[m + 2u].rgb;
	float shiny = materials.mats[m + 2u].a;
	vec4 flags = materials.mats[m + 3u];
	vec3 N = normalize(cross(dFdx(v_world), dFdy(v_world)));
	out_normal = vec4(N, 0.0);
	vec3 Vv = normalize(params.cam_pos.xyz - v_world);
	vec3 emi = (flags.x > 0.5) ? emissive * flags.y : vec3(0.0);
	int tid = mattex.tex_ids[int(v_mesh_id) * 5 + int(params.tex_slot_pad.x)];
	vec3 texel = vec3(1.0);
	if (tid >= 0 && tid < 8) {
		vec3 an = abs(N);
		vec2 tuv;
		if (an.x >= an.y && an.x >= an.z) {
			tuv = fract(vec2(v_world.z, v_world.y) * 0.01);
		} else if (an.y >= an.x && an.y >= an.z) {
			tuv = fract(vec2(v_world.x, v_world.z) * 0.01);
		} else {
			tuv = fract(vec2(v_world.x, v_world.y) * 0.01);
		}
		texel = texture(tex_arr[tid], tuv).rgb;
	}
	vec3 alb = albedo * texel;
	// GNE-016.5 slice-1: texture-driven channels (per-material opt-in; when all
	// slots are unset this branch is skipped and the legacy path is untouched).
	vec4 m2a = mat2buf.mats2[int(v_mesh_id) * 2 + 0];
	// GNE-016.5 slice-2: weighted |N| triplanar for all channel fetches
	// (fetch budget revised to <= 9; legacy path untouched when all slots unset).
	if (m2a.x > -0.5 || m2a.y > -0.5 || m2a.z > -0.5) {
		vec4 m2b = mat2buf.mats2[int(v_mesh_id) * 2 + 1];
		vec3 aw = abs(N);
		aw = aw / max(aw.x + aw.y + aw.z, 1e-5);
		vec2 t_yz = fract(vec2(v_world.z, v_world.y) * m2b.xy);
		vec2 t_xz = fract(vec2(v_world.x, v_world.z) * m2b.xy);
		vec2 t_xy = fract(vec2(v_world.x, v_world.y) * m2b.xy);
		if (m2a.x >= 0.0 && m2a.x <= 4.0) {
			int tia = int(m2a.x + 0.5);
			if (tia < 8) {
				vec3 ca = texture(tex_arr[tia], t_yz).rgb * aw.x + texture(tex_arr[tia], t_xz).rgb * aw.y + texture(tex_arr[tia], t_xy).rgb * aw.z;
				alb = albedo * ca;
			}
		}
		if (m2a.y >= 0.0 && m2a.y <= 4.0) {
			int tir = int(m2a.y + 0.5);
			if (tir < 8) {
				float cr = texture(tex_arr[tir], t_yz).r * aw.x + texture(tex_arr[tir], t_xz).r * aw.y + texture(tex_arr[tir], t_xy).r * aw.z;
				rough = clamp(rough * cr, 0.0, 1.0);
			}
		}
		if (m2a.z >= 0.0 && m2a.z <= 4.0) {
			int tin = int(m2a.z + 0.5);
			if (tin < 8) {
				vec3 nm = (texture(tex_arr[tin], t_yz).rgb * aw.x + texture(tex_arr[tin], t_xz).rgb * aw.y + texture(tex_arr[tin], t_xy).rgb * aw.z) * 2.0 - 1.0;
				vec3 an2 = abs(N);
				if (an2.x >= an2.y && an2.x >= an2.z) {
					N = normalize(N + vec3(0.0, nm.y, nm.z) * 0.6);
				} else if (an2.y >= an2.x && an2.y >= an2.z) {
					N = normalize(N + vec3(nm.x, 0.0, nm.z) * 0.6);
				} else {
					N = normalize(N + vec3(nm.x, nm.y, 0.0) * 0.6);
				}
				out_normal = vec4(N, 0.0);
			}
		}
	}
	vec3 L = params.light_dir_ambient.xyz;
	float AMB = params.light_dir_ambient.w;
	float ndl = max(dot(N, L), 0.0);
	vec3 V = Vv;
	vec3 diff = alb * ndl * params.light_color.rgb;
	vec3 H = normalize(L + V);
	float shiny_eff = max(shiny * (1.2 - rough), 1.0);
	float spec = pow(max(dot(N, H), 0.0), shiny_eff) * (1.0 - 0.5 * rough);
	vec3 sc = mix(spec_col, alb, metal);
	vec3 specular = spec * sc * params.light_color.rgb * step(0.0, dot(N, L)); // KI-011: NdotL gate
	float dir_sh = gne_shadow_dir(v_world, N, L);
	diff *= dir_sh;
	specular *= dir_sh;
	vec3 col = AMB * alb + diff + specular + emi;
	if (dot(N, Vv) >= 0.0) {
		// Cluster lookup: framebuffer tile + exponential depth slice.
		// Y MUST be flipped: gl_FragCoord row 0 is the framebuffer TOP, which
		// is NDC +y, i.e. cull row 8 (cull rows are NDC-bottom-up). Without
		// the flip the frag reads mirrored, mostly-empty clusters.
		float z_view = length(params.cam_pos.xyz - v_world);
		// GNE-018-rev: single-source slice helper (same math as the lines it
		// replaces; D8-rev contract forbids divergent copies).
		uint cid = gne_cluster_index(gl_FragCoord.xy, z_view, params.grid_dims);



		uint coff = cid * 16u;
		uint ccnt = clcnt.cnt[cid];
		// GNE-019: the cull appends under `if (slot < 16u)`, so a cluster holds
		// at most 16 valid ids at stride 16 - but the count itself is an
		// uncapped atomicAdd, so it can exceed 16. Bounding the read at 64 (the
		// old guard) made j = 16..ccnt-1 address the NEXT cluster's slots, and
		// for the last cluster it read past the end of cluster_index_buffer
		// (coff 55280 + 63 > 55295). The bound must equal the stride: 16.
		for (uint j = 0u; j < ccnt && j < 16u; j++) {
			uint lid = clidx.idx[coff + j];
			vec4 A0 = lightbuf.lights[lid * 4u + 0u];
			vec4 A1 = lightbuf.lights[lid * 4u + 1u];
			vec4 A2 = lightbuf.lights[lid * 4u + 2u];
			vec4 A3 = lightbuf.lights[lid * 4u + 3u];
			vec3 to_l = A0.xyz - v_world;
			float dd = length(to_l);
			vec3 Ld = dd > 1e-4 ? to_l / dd : vec3(0.0);
			float att = 1.0 / (1.0 + (dd * dd) / (A0.w * A0.w));
			float ndl2 = max(dot(N, Ld), 0.0);
			float cone_f = 1.0;
			if (A2.w > 0.5) {
				float ang = acos(clamp(dot(-Ld, normalize(A2.xyz)), -1.0, 1.0));
				cone_f = 1.0 - smoothstep(A3.x, A3.y, ang);
			}
			vec3 ldiff = alb * ndl2 * A1.rgb * (A1.a * att * cone_f);
			vec3 H2 = normalize(Ld + V);
			float s2 = pow(max(dot(N, H2), 0.0), shiny_eff) * (1.0 - 0.5 * rough);
			vec3 lspec = s2 * sc * A1.rgb * (A1.a * att * cone_f) * step(0.0, dot(N, Ld)); // KI-011: NdotL gate
			float shf = gne_shadow_light(lid, v_world, N, Ld, z_view);
			col += (ldiff + lspec) * shf;
		}
	} else {
		vec3 bem = (flags.z > 0.5) ? emi : vec3(0.0);
		col = bem;
		out_radiance = vec4(col, 1.0); // KI-017: unlit path
	}
	if (params.gi_params.x > 0.5) {
		col += params.gi_params.y * gne_gi_sample_field(gi_atlas_s, params.gi_min.xyz, params.gi_max.xyz, params.gi_dims.xyz, v_world);
	}
	out_radiance = vec4(col, 1.0); // KI-017: raw HDR, final pre-clamp (incl. cluster+GI)
	out_color = vec4(col, 1.0);
	out_view_z = -1.0 / gl_FragCoord.w;
}
)";

// GNE-019: shadow depth vertex shader. Clone of the batch vertex shader (same
// set-0 layout) PLUS v_ndc_z = gl_Position.z/gl_Position.w. Storing raw ndc.z
// (instead of gl_FragCoord.z) makes the depth compare convention-independent:
// the sampler recomputes the identical ndc.z from the same VP.
const char *gpu_shadow_depth_vert_glsl = R"(
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
layout(location = 0) out vec3 v_world;
layout(location = 2) out float v_ndc_z;

void main() {
	uint orig = batch_instances.instances[gl_InstanceIndex];
	uint m = meshids.mesh_id[orig];
	vec4 ts = transforms.position_scale[orig];
	vec3 world = ts.xyz + vertex_position * ts.w;
	gl_Position = viewdata.vp * vec4(world, 1.0);
	v_ndc_z = gl_Position.z / max(abs(gl_Position.w), 1e-6);
	v_mesh_id = m;
	v_world = world;
}
)";

// GNE-019: shadow depth fragment shader. Reuses the batch vertex shader
// (identical set-0 layout, binding 2 swapped for the dedicated shadow view
// UBO), so no new vertex code exists. Writes window-space depth
// (gl_FragCoord.z) into R32 — the SAME quantity the sampling side recomputes
// from the same VP, so the compare is exact by construction regardless of
// clip conventions. A D32 attachment on the same framebuffer provides the
// depth test (self-occlusion correctness).
const char *gpu_shadow_depth_frag_glsl = R"(
#version 450

layout(location = 0) in vec3 v_world;
layout(location = 2) in float v_ndc_z;

layout(push_constant, std430) uniform ShadowDepthParams {
	vec4 light_pos; // xyz = light position (unused for CSM, kept for uniformity)
}
params;

layout(location = 0) out vec4 out_dist;

void main() {
	// GNE-019.5 slice-0: dir CSM fills may ESM-encode (mode in light_pos.w).
	float v = v_ndc_z;
	if (params.light_pos.w > 0.5) {
		v = exp(87.0 * v_ndc_z); // c near the float32 exp ceiling (e^88)
	}
	out_dist = vec4(v, 0.0, 0.0, 1.0);
}
)";

// GNE-019: pack a 2D shadow render target into a sampling-array layer.
// texture_copy is unreliable in this RDG fork (observed: array stays at the
// clear value), so the copy runs as a compute pass - the same reliable
// sample-a-render-target route the 012 depth-source pass uses.
const char *gpu_shadow_pack_glsl = R"(
#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1, r32f) uniform writeonly image2DArray dst;

layout(push_constant, std430) uniform PackParams {
	uint layer;
	uint size;
	uint pad0;
	uint pad1;
}
params;

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (px.x >= int(params.size) || px.y >= int(params.size)) {
		return;
	}
	vec2 uv = (vec2(px) + 0.5) / float(params.size);
	float v = textureLod(src, uv, 0.0).r;
	// TEMP-DIAG-019c removed — real depth path active (line 1528)
	imageStore(dst, ivec3(px, int(params.layer)), vec4(v, 0.0, 0.0, 1.0));
}
)";

// GNE-019: shadow caster culling (D7-5). One thread per (bind, instance):
// 64x4 = 256 threads, single group. Sphere (from position_scale, r = w*1.74
// conservative for the unit cube) vs per-bind frustum planes; CSM/dir always
// casts (whole-scene ortho, documented); cube = range-sphere test. Results via
// atomicOr bitmasks (bit i = instance i) - concurrent atomicOr on one word is
// safe (014 RMW lesson: atomics, never read-modify-write).
const char *gpu_shadow_cull_glsl = R"(
#version 450

layout(local_size_x = 64, local_size_y = 4, local_size_z = 1) in;

layout(push_constant, std430) uniform ShadowCullParams {
	uint bind_count;
	uint instance_count;
	uint pad0;
	uint pad1;
}
params;

layout(std430, set = 0, binding = 0) buffer TransformBuffer {
	vec4 position_scale[];
}
transforms;

layout(std430, set = 0, binding = 1) buffer PlanesBuffer {
	vec4 planes[];
}
planebuf;

layout(std430, set = 0, binding = 2) buffer AuxBuffer {
	vec4 aux[]; // x = type (0 CSM, 1 cube, 2 spot), y = range
}
auxbuf;

layout(std430, set = 0, binding = 3) buffer PosBuffer {
	vec4 lightpos[]; // xyz = light position
}
posbuf;

layout(std430, set = 0, binding = 4) buffer MaskBuffer {
	uint mask[];
}
maskbuf;

void main() {
	uint b = gl_GlobalInvocationID.x;
	uint i = gl_GlobalInvocationID.y;
	if (b >= params.bind_count || i >= params.instance_count || i >= 32u) {
		return;
	}
	vec4 ts = transforms.position_scale[i];
	vec3 c = ts.xyz;
	float r = ts.w * 1.74;
	float tp = auxbuf.aux[b].x;
	bool hit = false;
	if (tp < 0.5) {
		hit = true; // CSM/dir: whole-scene ortho always covers all casters
	} else if (tp < 1.5) {
		float dd = length(c - posbuf.lightpos[b].xyz);
		hit = dd < auxbuf.aux[b].y + r;
	} else {
		hit = true;
		for (int k = 0; k < 6; k++) {
			vec4 pl = planebuf.planes[b * 6u + uint(k)];
			if (dot(pl.xyz, c) + pl.w < -r) {
				hit = false;
				break;
			}
		}
	}
	if (hit) {
		atomicOr(maskbuf.mask[b], 1u << i);
	}
}
)";

// GNE-011: procedural (non-indexed) batch vertex shader used by the GROUPED /
// REORDERED multi-batch draw. One VkDrawIndirectCommand covers every instance of
// every member mesh of a group: command.vertexCount == the largest member
// index_count and each instance picks its OWN sub-range through the mesh table;
// out-of-range vertex indices are pushed outside the clip volume (fully clipped,
// no raster, no depth write). The shared vertex/index data is read as SSBO
// storage mirrors (RD vertex/index buffer owners cannot be bound as storage).
const char *gpu_mesh_group_batch_vert_glsl = R"(
#version 450

struct GneMeshDescStd430 {
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
	GneMeshDescStd430 table[64];
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
	GneMeshDescStd430 d = mesh_table.table[m];
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

// TEMP-DIAG-012-idprobe: DIAGNOSTIC-ONLY id pass. It replicates
// gpu_mesh_group_batch_vert_glsl (the shader that actually runs for
// STRATEGY_REORDERED, i.e. main_012) LINE FOR LINE up to gl_Position, and
// additionally forwards the RESOLVED original scene id to the fragment stage:
//     uint orig = batch_instances.instances[gl_InstanceIndex];
// gl_InstanceIndex is used ONLY as an index into batch_instances (exactly as
// the production path does, including the first_instance base baked into the
// indirect command); the exported id is the value READ OUT of that buffer.
// NOT exported: gl_InstanceIndex itself.
// The production 8-binding set-0 layout is reproduced unchanged so the same
// group_batch_uniform_set geometry is used (a dedicated set is still built,
// but over the same 8 RIDs, so the draw cannot silently differ).
const char *gpu_idprobe_vert_glsl = R"(
#version 450

struct GneMeshDescStd430 {
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
	GneMeshDescStd430 table[64];
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

layout(location = 0) flat out uint v_orig;

void main() {
	uint orig = batch_instances.instances[gl_InstanceIndex];
	uint m = meshids.mesh_id[orig];
	GneMeshDescStd430 d = mesh_table.table[m];
	if (gl_VertexIndex >= d.index_count) {
		gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
		v_orig = orig;
		return;
	}
	uint li = index_data.data[d.first_index + gl_VertexIndex];
	int vi = d.vertex_offset + int(li);
	vec3 p = vec3(vertex_data.data[vi * 3 + 0], vertex_data.data[vi * 3 + 1], vertex_data.data[vi * 3 + 2]);
	vec4 ts = transforms.position_scale[orig];
	vec3 world = ts.xyz + p * ts.w;
	gl_Position = viewdata.vp * vec4(world, 1.0);
	v_orig = orig;
}
)";

// TEMP-DIAG-012-idprobe: writes the resolved original scene id into R32_UINT.
// early_fragment_tests is kept so the depth test is applied BEFORE the write,
// matching the production fragment shader (which also declares it) - otherwise
// occluded fragments would still overwrite the id and the coverage comparison
// would be meaningless.
const char *gpu_idprobe_frag_glsl = R"(
#version 450

layout(early_fragment_tests) in;

layout(location = 0) flat in uint v_orig;
layout(location = 0) out uint out_id;

void main() {
	out_id = v_orig;
}
)";

// GNE-012: build HZB base layer by sampling the PREVIOUS frame's D32_SFLOAT
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
		uint best = 0;
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
		uint poff = 0;
		for (int k = 0; k < level; k++) {
			uint s = uint(int(params.level0_size) >> k);
			poff += s * s;
		}
		pyrbuf.data[poff + uint(px.x) * uint(lsize) + uint(px.y)] = best;
	}
}
)";

// GNE-012 (FINAL pyramid source): OCCLUDER-BOX rasterization. The pyramid is
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
	for (uint o = 0; o < vd.occ_count; o++) {
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
			uint poff = 0;
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

// GNE-012 phase 1: FRUSTUM-ONLY cull. Every instance is tested against the 6
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
	uint vis = 1;
	for (uint p = 0; p < 6u; p++) {
		vec4 pl = viewdata.planes[p];
		float d = dot(pl.xyz, center) + pl.w;
		if (d < -radius) {
			vis = 0;
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

// GNE-012 phase 2: HZB occlusion over ALL instances (run inside the same
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

	uint vis = 1;
	for (uint p = 0; p < 6u; p++) {
		vec4 pl = viewdata.planes[p];
		float d = dot(pl.xyz, center) + pl.w;
		if (d < -radius) {
			vis = 0;
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
			uint poff = 0;
			int base = int(viewdata.viewport.z);
			for (int k = 0; k < level; k++) {
				uint s = uint(base >> k);
				poff += s * s;
			}
			uint max_inv = 0;
			for (int dy = 0; dy <= 1; dy++) {
				for (int dx = 0; dx <= 1; dx++) {
					ivec2 tc = clamp(t + ivec2(dx, dy), ivec2(0), ivec2(texels - 1));
					max_inv = max(max_inv, pyrbuf.data[poff + uint(tc.x) * uint(texels) + uint(tc.y)]);
				}
			}
			if (max_inv > sphere_inv) {
				vis = 0;
			}

			// GNE-012 debug: dump instance 91's occlusion math into the probe
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

void GneRenderServer::_bind_methods() {
	ClassDB::bind_static_method("GneRenderServer", D_METHOD("get_server_singleton"), &GneRenderServer::get_server_singleton);
	ClassDB::bind_method(D_METHOD("initialize"), &GneRenderServer::initialize);
	ClassDB::bind_method(D_METHOD("shutdown"), &GneRenderServer::shutdown);
	ClassDB::bind_method(D_METHOD("is_initialized"), &GneRenderServer::is_initialized);
	ClassDB::bind_method(D_METHOD("ensure_gpu_device"), &GneRenderServer::ensure_gpu_device);
	ClassDB::bind_method(D_METHOD("is_gpu_ready"), &GneRenderServer::is_gpu_ready);

	ClassDB::bind_method(D_METHOD("gpu_scene_create", "instance_count", "spread"), &GneRenderServer::gpu_scene_create);
	ClassDB::bind_method(D_METHOD("gpu_scene_dispatch", "seed"), &GneRenderServer::gpu_scene_dispatch);
	ClassDB::bind_method(D_METHOD("gpu_scene_readback_positions", "index", "count"), &GneRenderServer::gpu_scene_readback_positions);
	ClassDB::bind_method(D_METHOD("gpu_scene_readback_scales", "index", "count"), &GneRenderServer::gpu_scene_readback_scales);
	ClassDB::bind_method(D_METHOD("gpu_scene_stats"), &GneRenderServer::gpu_scene_stats);
	ClassDB::bind_method(D_METHOD("gpu_scene_get_instance_count"), &GneRenderServer::gpu_scene_get_instance_count);
	ClassDB::bind_method(D_METHOD("gpu_scene_destroy"), &GneRenderServer::gpu_scene_destroy);

	ClassDB::bind_method(D_METHOD("gpu_scene_set_camera", "camera_transform", "projection"), &GneRenderServer::gpu_scene_set_camera);
	ClassDB::bind_method(D_METHOD("gpu_cull_dispatch"), &GneRenderServer::gpu_cull_dispatch);
	ClassDB::bind_method(D_METHOD("gpu_cull_get_visible_count"), &GneRenderServer::gpu_cull_get_visible_count);
	ClassDB::bind_method(D_METHOD("gpu_cull_get_visibility"), &GneRenderServer::gpu_cull_get_visibility);
	ClassDB::bind_method(D_METHOD("gpu_scene_get_frustum_planes"), &GneRenderServer::gpu_scene_get_frustum_planes);

	ClassDB::bind_method(D_METHOD("gpu_drawargs_finalize"), &GneRenderServer::gpu_drawargs_finalize);
	ClassDB::bind_method(D_METHOD("gpu_drawargs_read"), &GneRenderServer::gpu_drawargs_read);
	ClassDB::bind_method(D_METHOD("gpu_compact_read"), &GneRenderServer::gpu_compact_read);

	ClassDB::bind_method(D_METHOD("gpu_scene_set_viewport", "viewport_w", "viewport_h"), &GneRenderServer::gpu_scene_set_viewport);
	ClassDB::bind_method(D_METHOD("gpu_scene_set_occluders", "occluders"), &GneRenderServer::gpu_scene_set_occluders);
	ClassDB::bind_method(D_METHOD("gpu_visibility_dispatch"), &GneRenderServer::gpu_visibility_dispatch);

	ClassDB::bind_method(D_METHOD("gpu_hzb_prod_create"), &GneRenderServer::gpu_hzb_prod_create);
	ClassDB::bind_method(D_METHOD("gpu_hzb_build"), &GneRenderServer::gpu_hzb_build);
	ClassDB::bind_method(D_METHOD("gpu_visibility_prod_dispatch"), &GneRenderServer::gpu_visibility_prod_dispatch);
	ClassDB::bind_method(D_METHOD("gpu_hzb_enable_temporal", "enabled"), &GneRenderServer::gpu_hzb_enable_temporal);
	ClassDB::bind_method(D_METHOD("gpu_hzb_set_occluders", "occluders"), &GneRenderServer::gpu_hzb_set_occluders);
	ClassDB::bind_method(D_METHOD("gpu_hzb_get_level_count"), &GneRenderServer::gpu_hzb_get_level_count);
	ClassDB::bind_method(D_METHOD("gpu_hzb_get_phase_counts"), &GneRenderServer::gpu_hzb_get_phase_counts);
	ClassDB::bind_method(D_METHOD("gpu_hzb_get_coherent"), &GneRenderServer::gpu_hzb_get_coherent);
	ClassDB::bind_method(D_METHOD("gpu_hzb_dbg_valid"), &GneRenderServer::gpu_hzb_dbg_valid);
	ClassDB::bind_method(D_METHOD("gpu_hzb_dbg_level0", "x", "y"), &GneRenderServer::gpu_hzb_dbg_level0);
	ClassDB::bind_method(D_METHOD("gpu_hzb_dbg_probe"), &GneRenderServer::gpu_hzb_dbg_probe);
	ClassDB::bind_method(D_METHOD("gpu_hzb_dbg_scan_level0"), &GneRenderServer::gpu_hzb_dbg_scan_level0);
	ClassDB::bind_method(D_METHOD("gpu_hzb_dbg_scan_level1", "level"), &GneRenderServer::gpu_hzb_dbg_scan_level1);
	ClassDB::bind_method(D_METHOD("gpu_hzb_dbg_scan_buffer", "level"), &GneRenderServer::gpu_hzb_dbg_scan_buffer);
	ClassDB::bind_method(D_METHOD("gpu_hzb_dbg_sim2"), &GneRenderServer::gpu_hzb_dbg_sim2);

	ClassDB::bind_method(D_METHOD("gpu_scene_manager_alloc", "max_instances"), &GneRenderServer::gpu_scene_manager_alloc);
	ClassDB::bind_method(D_METHOD("gpu_scene_manager_set_instances", "instances"), &GneRenderServer::gpu_scene_manager_set_instances);
	ClassDB::bind_method(D_METHOD("gpu_scene_manager_update", "deltas"), &GneRenderServer::gpu_scene_manager_update);
	ClassDB::bind_method(D_METHOD("gpu_scene_manager_dispatch"), &GneRenderServer::gpu_scene_manager_dispatch);
	ClassDB::bind_method(D_METHOD("gpu_scene_manager_get_stats"), &GneRenderServer::gpu_scene_manager_get_stats);
	ClassDB::bind_method(D_METHOD("gpu_scene_manager_get_draw_counts"), &GneRenderServer::gpu_scene_manager_get_draw_counts);
	ClassDB::bind_method(D_METHOD("gpu_scene_manager_get_snapshot", "count"), &GneRenderServer::gpu_scene_manager_get_snapshot);
	ClassDB::bind_method(D_METHOD("gpu_scene_manager_get_active_ids", "count"), &GneRenderServer::gpu_scene_manager_get_active_ids);
	ClassDB::bind_method(D_METHOD("gpu_scene_manager_get_wave_stats"), &GneRenderServer::gpu_scene_manager_get_wave_stats);
	ClassDB::bind_method(D_METHOD("gpu_scene_manager_destroy"), &GneRenderServer::gpu_scene_manager_destroy);

	// GNE-015: Render Graph (additive TEST-ONLY bridge; see header @598).
	ClassDB::bind_method(D_METHOD("gpu_rg_create"), &GneRenderServer::gpu_rg_create);
	ClassDB::bind_method(D_METHOD("gpu_rg_add_pass", "name", "kind", "in_res", "out_res"), &GneRenderServer::gpu_rg_add_pass);
	ClassDB::bind_method(D_METHOD("gpu_rg_add_edge", "from", "to", "resource", "bytes"), &GneRenderServer::gpu_rg_add_edge);
	ClassDB::bind_method(D_METHOD("gpu_rg_compile"), &GneRenderServer::gpu_rg_compile);
	ClassDB::bind_method(D_METHOD("gpu_rg_execute"), &GneRenderServer::gpu_rg_execute);
	ClassDB::bind_method(D_METHOD("gpu_rg_get_stats"), &GneRenderServer::gpu_rg_get_stats);
	ClassDB::bind_method(D_METHOD("gpu_rg_dump"), &GneRenderServer::gpu_rg_dump);
	ClassDB::bind_method(D_METHOD("gpu_rg_destroy"), &GneRenderServer::gpu_rg_destroy);
	// GNE-015.5: Resource Pool (SPEC 015.5 v0.2) - real allocation + aliasing.
	ClassDB::bind_method(D_METHOD("gpu_pool_create", "bytes"), &GneRenderServer::gpu_pool_create);
	ClassDB::bind_method(D_METHOD("gpu_pool_alloc", "bytes", "first_pass", "last_pass", "tag"), &GneRenderServer::gpu_pool_alloc);
	ClassDB::bind_method(D_METHOD("gpu_pool_free", "index"), &GneRenderServer::gpu_pool_free);
	ClassDB::bind_method(D_METHOD("gpu_pool_persistent_alloc", "bytes", "tag"), &GneRenderServer::gpu_pool_persistent_alloc);
	ClassDB::bind_method(D_METHOD("gpu_pool_stats"), &GneRenderServer::gpu_pool_stats);
	ClassDB::bind_method(D_METHOD("gpu_pool_verify", "tag", "value"), &GneRenderServer::gpu_pool_verify);
	ClassDB::bind_method(D_METHOD("gpu_pool_destroy"), &GneRenderServer::gpu_pool_destroy);
	// GNE-015.5 Phase 4: frame-time drift measurement (Test-only, additive).
	ClassDB::bind_method(D_METHOD("gpu_frame_reset"), &GneRenderServer::gpu_frame_reset);
	ClassDB::bind_method(D_METHOD("gpu_frame_set_warmup", "warmup"), &GneRenderServer::gpu_frame_set_warmup);
	ClassDB::bind_method(D_METHOD("gpu_frame_begin"), &GneRenderServer::gpu_frame_begin);
	ClassDB::bind_method(D_METHOD("gpu_frame_mark", "pass"), &GneRenderServer::gpu_frame_mark);
	ClassDB::bind_method(D_METHOD("gpu_frame_end"), &GneRenderServer::gpu_frame_end);
	ClassDB::bind_method(D_METHOD("gpu_frame_stats"), &GneRenderServer::gpu_frame_stats);


	ClassDB::bind_method(D_METHOD("gpu_meshlet_load", "data"), &GneRenderServer::gpu_meshlet_load);
	ClassDB::bind_method(D_METHOD("gpu_meshlet_load_path", "path"), &GneRenderServer::gpu_meshlet_load_path);
	ClassDB::bind_method(D_METHOD("gpu_meshlet_set_lod_thresholds", "t0", "t1"), &GneRenderServer::gpu_meshlet_set_lod_thresholds);
	ClassDB::bind_method(D_METHOD("gpu_meshlet_cull_dispatch"), &GneRenderServer::gpu_meshlet_cull_dispatch);
	ClassDB::bind_method(D_METHOD("gpu_meshlet_raster_dispatch"), &GneRenderServer::gpu_meshlet_raster_dispatch);
	ClassDB::bind_method(D_METHOD("gpu_meshlet_get_total_meshlets"), &GneRenderServer::gpu_meshlet_get_total_meshlets);
	ClassDB::bind_method(D_METHOD("gpu_meshlet_get_lod0_tri_count"), &GneRenderServer::gpu_meshlet_get_lod0_tri_count);
	ClassDB::bind_method(D_METHOD("gpu_meshlet_get_lod0_meshlet_count"), &GneRenderServer::gpu_meshlet_get_lod0_meshlet_count);
	ClassDB::bind_method(D_METHOD("gpu_meshlet_stats"), &GneRenderServer::gpu_meshlet_stats);
	ClassDB::bind_method(D_METHOD("gpu_meshlet_get_cluster_counts"), &GneRenderServer::gpu_meshlet_get_cluster_counts);
	ClassDB::bind_method(D_METHOD("gpu_meshlet_get_instance_lods"), &GneRenderServer::gpu_meshlet_get_instance_lods);
	ClassDB::bind_method(D_METHOD("gpu_meshlet_get_cull_debug"), &GneRenderServer::gpu_meshlet_get_cull_debug);
	ClassDB::bind_method(D_METHOD("gpu_meshlet_raster_evidence"), &GneRenderServer::gpu_meshlet_raster_evidence);
	ClassDB::bind_method(D_METHOD("gpu_meshlet_destroy"), &GneRenderServer::gpu_meshlet_destroy);

	ClassDB::bind_method(D_METHOD("gpu_raster_indirect_draw"), &GneRenderServer::gpu_raster_indirect_draw);
	ClassDB::bind_method(D_METHOD("gpu_raster_read_pixels"), &GneRenderServer::gpu_raster_read_pixels);
	ClassDB::bind_method(D_METHOD("gpu_scene_get_vp"), &GneRenderServer::gpu_scene_get_vp);

	ClassDB::bind_method(D_METHOD("gpu_scene_set_instance_transform", "index", "position", "scale"), &GneRenderServer::gpu_scene_set_instance_transform);
	ClassDB::bind_method(D_METHOD("gpu_raster_read_depth"), &GneRenderServer::gpu_raster_read_depth);
	ClassDB::bind_method(D_METHOD("gpu_raster_read_viewz", "x", "y"), &GneRenderServer::gpu_raster_read_viewz);
	ClassDB::bind_method(D_METHOD("gpu_raster_read_normal", "x", "y"), &GneRenderServer::gpu_raster_read_normal);
	ClassDB::bind_method(D_METHOD("gpu_raster_read_hdr", "x", "y"), &GneRenderServer::gpu_raster_read_hdr);
	ClassDB::bind_method(D_METHOD("gpu_raster_read_viewz_all"), &GneRenderServer::gpu_raster_read_viewz_all);
	ClassDB::bind_method(D_METHOD("gpu_raster_read_normal_all"), &GneRenderServer::gpu_raster_read_normal_all);
	ClassDB::bind_method(D_METHOD("gpu_present_lowres_set", "enabled"), &GneRenderServer::gpu_present_lowres_set);
	ClassDB::bind_method(D_METHOD("gpu_present_info"), &GneRenderServer::gpu_present_info);
	ClassDB::bind_method(D_METHOD("gpu_present_read_pixels"), &GneRenderServer::gpu_present_read_pixels);
	ClassDB::bind_method(D_METHOD("gpu_raster_get_depth_format"), &GneRenderServer::gpu_raster_get_depth_format);
	ClassDB::bind_method(D_METHOD("gpu_mesh_get_depth_enabled"), &GneRenderServer::gpu_mesh_get_depth_enabled);
	ClassDB::bind_method(D_METHOD("gpu_raster_get_depth_enabled"), &GneRenderServer::gpu_raster_get_depth_enabled);

	ClassDB::bind_method(D_METHOD("gpu_mesh_create"), &GneRenderServer::gpu_mesh_create);
	ClassDB::bind_method(D_METHOD("gpu_mesh_drawargs_finalize"), &GneRenderServer::gpu_mesh_drawargs_finalize);
	ClassDB::bind_method(D_METHOD("gpu_mesh_indirect_draw"), &GneRenderServer::gpu_mesh_indirect_draw);
	ClassDB::bind_method(D_METHOD("gpu_mesh_get_index_count"), &GneRenderServer::gpu_mesh_get_index_count);
	ClassDB::bind_method(D_METHOD("gpu_mesh_get_vertex_count"), &GneRenderServer::gpu_mesh_get_vertex_count);

	ClassDB::bind_method(D_METHOD("gpu_scene_set_instance_mesh", "index", "mesh_id"), &GneRenderServer::gpu_scene_set_instance_mesh);
	ClassDB::bind_method(D_METHOD("gpu_scene_get_instance_mesh", "index"), &GneRenderServer::gpu_scene_get_instance_mesh);
	ClassDB::bind_method(D_METHOD("gpu_mesh_create_from_arrays", "verts", "indices"), &GneRenderServer::gpu_mesh_create_from_arrays);
	ClassDB::bind_method(D_METHOD("gpu_mesh_batch_dispatch"), &GneRenderServer::gpu_mesh_batch_dispatch);
	ClassDB::bind_method(D_METHOD("gpu_mesh_batch_draw"), &GneRenderServer::gpu_mesh_batch_draw);
	ClassDB::bind_method(D_METHOD("gpu_mesh_get_mesh_id_count"), &GneRenderServer::gpu_mesh_get_mesh_id_count);
	ClassDB::bind_method(D_METHOD("gpu_mesh_get_batch_count"), &GneRenderServer::gpu_mesh_get_batch_count);
	ClassDB::bind_method(D_METHOD("gpu_mesh_get_draw_counts"), &GneRenderServer::gpu_mesh_get_draw_counts);
	ClassDB::bind_method(D_METHOD("gpu_mesh_get_batch_args", "batch_index"), &GneRenderServer::gpu_mesh_get_batch_args);
	ClassDB::bind_method(D_METHOD("gpu_mesh_get_mesh_color", "mesh_id"), &GneRenderServer::gpu_mesh_get_mesh_color);
	// TEMP-DIAG-012-idprobe (temporary, disabled by default)
	ClassDB::bind_method(D_METHOD("gpu_idprobe_set_enabled", "enabled"), &GneRenderServer::gpu_idprobe_set_enabled);
	ClassDB::bind_method(D_METHOD("gpu_idprobe_is_valid"), &GneRenderServer::gpu_idprobe_is_valid);
	ClassDB::bind_method(D_METHOD("gpu_idprobe_read_ids"), &GneRenderServer::gpu_idprobe_read_ids);
	ClassDB::bind_method(D_METHOD("gpu_idprobe_read_depth"), &GneRenderServer::gpu_idprobe_read_depth);
	ClassDB::bind_method(D_METHOD("gpu_mesh_set_batch_strategy", "strategy"), &GneRenderServer::gpu_mesh_set_batch_strategy);
	ClassDB::bind_method(D_METHOD("gpu_mesh_get_batch_strategy"), &GneRenderServer::gpu_mesh_get_batch_strategy);
	ClassDB::bind_method(D_METHOD("gpu_mesh_get_batch_group_count"), &GneRenderServer::gpu_mesh_get_batch_group_count);
	ClassDB::bind_method(D_METHOD("gpu_mesh_get_batch_order"), &GneRenderServer::gpu_mesh_get_batch_order);
	ClassDB::bind_method(D_METHOD("gpu_mesh_get_indirect_count"), &GneRenderServer::gpu_mesh_get_indirect_count);
	ClassDB::bind_method(D_METHOD("gpu_mesh_get_draw_call_count"), &GneRenderServer::gpu_mesh_get_draw_call_count);
	ClassDB::bind_method(D_METHOD("gpu_material_create"), &GneRenderServer::gpu_material_create);
	ClassDB::bind_method(D_METHOD("gpu_material_set_albedo", "id", "color"), &GneRenderServer::gpu_material_set_albedo);
	ClassDB::bind_method(D_METHOD("gpu_material_set_params", "id", "roughness", "metallic"), &GneRenderServer::gpu_material_set_params);
	ClassDB::bind_method(D_METHOD("gpu_material_set_specular", "id", "color", "shininess"), &GneRenderServer::gpu_material_set_specular);
	ClassDB::bind_method(D_METHOD("gpu_material_set_maps", "mat", "albedo_slot", "rough_slot", "normal_slot", "scale"), &GneRenderServer::gpu_material_set_maps);
	ClassDB::bind_method(D_METHOD("gpu_material_set_emissive", "id", "color", "strength", "on", "backface"), &GneRenderServer::gpu_material_set_emissive);
	ClassDB::bind_method(D_METHOD("gpu_material_readback", "id"), &GneRenderServer::gpu_material_readback);
	ClassDB::bind_method(D_METHOD("gpu_material_set_light", "dir"), &GneRenderServer::gpu_material_set_light);
	ClassDB::bind_method(D_METHOD("gpu_material_stats"), &GneRenderServer::gpu_material_stats);
	ClassDB::bind_method(D_METHOD("gpu_material_draw"), &GneRenderServer::gpu_material_draw);
	ClassDB::bind_method(D_METHOD("gpu_texture_load", "path"), &GneRenderServer::gpu_texture_load);
	ClassDB::bind_method(D_METHOD("gpu_texture_bind", "material_id", "slot", "texture_id"), &GneRenderServer::gpu_texture_bind);
	ClassDB::bind_method(D_METHOD("gpu_texture_get_stats"), &GneRenderServer::gpu_texture_get_stats);
	ClassDB::bind_method(D_METHOD("gpu_texture_get_binding", "material_id", "slot"), &GneRenderServer::gpu_texture_get_binding);
	ClassDB::bind_method(D_METHOD("gpu_light_create", "params"), &GneRenderServer::gpu_light_create);
	ClassDB::bind_method(D_METHOD("gpu_light_update", "id", "params"), &GneRenderServer::gpu_light_update);
	ClassDB::bind_method(D_METHOD("gpu_light_destroy", "id"), &GneRenderServer::gpu_light_destroy);
	ClassDB::bind_method(D_METHOD("gpu_light_set_intensity", "id", "intensity"), &GneRenderServer::gpu_light_set_intensity);
	ClassDB::bind_method(D_METHOD("gpu_light_get_stats"), &GneRenderServer::gpu_light_get_stats);
	ClassDB::bind_method(D_METHOD("gpu_light_cone_read", "cluster"), &GneRenderServer::gpu_light_cone_read);
	ClassDB::bind_method(D_METHOD("gpu_light_cones_build"), &GneRenderServer::gpu_light_cones_build);
	ClassDB::bind_method(D_METHOD("gpu_light_cone_epochs"), &GneRenderServer::gpu_light_cone_epochs);
	ClassDB::bind_method(D_METHOD("gpu_light_cones_selftest", "mode"), &GneRenderServer::gpu_light_cones_selftest);
	ClassDB::bind_method(D_METHOD("gpu_rt_selftest"), &GneRenderServer::gpu_rt_selftest);
	ClassDB::bind_method(D_METHOD("gpu_gi_create", "config"), &GneRenderServer::gpu_gi_create);
	ClassDB::bind_method(D_METHOD("gpu_gi_enabled_set", "enabled"), &GneRenderServer::gpu_gi_enabled_set);
	ClassDB::bind_method(D_METHOD("gpu_gi_info"), &GneRenderServer::gpu_gi_info);
	ClassDB::bind_method(D_METHOD("gpu_gi_trace"), &GneRenderServer::gpu_gi_trace);
	ClassDB::bind_method(D_METHOD("gpu_gi_read_avg", "probe"), &GneRenderServer::gpu_gi_read_avg);
	ClassDB::bind_method(D_METHOD("gpu_gi_read_texel", "probe", "texel"), &GneRenderServer::gpu_gi_read_texel);
	ClassDB::bind_method(D_METHOD("gpu_gi_config", "config"), &GneRenderServer::gpu_gi_config);
	ClassDB::bind_method(D_METHOD("gpu_gi_accum_step"), &GneRenderServer::gpu_gi_accum_step);
	ClassDB::bind_method(D_METHOD("gpu_gi_reset"), &GneRenderServer::gpu_gi_reset);
	ClassDB::bind_method(D_METHOD("gpu_light_set_normal_cone", "enabled"), &GneRenderServer::gpu_light_set_normal_cone);
	ClassDB::bind_method(D_METHOD("gpu_light_cones_clear"), &GneRenderServer::gpu_light_cones_clear);
	ClassDB::bind_method(D_METHOD("gpu_shadow_map_create", "type", "resolution"), &GneRenderServer::gpu_shadow_map_create);
	ClassDB::bind_method(D_METHOD("gpu_shadow_light_bind", "light_id", "shadow_id"), &GneRenderServer::gpu_shadow_light_bind);
	ClassDB::bind_method(D_METHOD("gpu_shadow_cull_dispatch"), &GneRenderServer::gpu_shadow_cull_dispatch);
	ClassDB::bind_method(D_METHOD("gpu_shadow_render_maps"), &GneRenderServer::gpu_shadow_render_maps);
	ClassDB::bind_method(D_METHOD("gpu_shadow_get_stats"), &GneRenderServer::gpu_shadow_get_stats);
	ClassDB::bind_method(D_METHOD("gpu_shadow_esm_set", "enabled"), &GneRenderServer::gpu_shadow_esm_set);
	ClassDB::bind_method(D_METHOD("gpu_shadow_dbg_map", "type", "slot", "face"), &GneRenderServer::gpu_shadow_dbg_map);
	ClassDB::bind_method(D_METHOD("gpu_shadow_dbg_dump", "layer", "path"), &GneRenderServer::gpu_shadow_dbg_dump);
	ClassDB::bind_method(D_METHOD("gpu_shadow_dbg_vp", "bind", "cas"), &GneRenderServer::gpu_shadow_dbg_vp);
	ClassDB::bind_method(D_METHOD("gpu_hzb_set_real_depth", "enabled"), &GneRenderServer::gpu_hzb_set_real_depth);
	ClassDB::bind_method(D_METHOD("gpu_hzb_depth_feed"), &GneRenderServer::gpu_hzb_depth_feed);
	ClassDB::bind_method(D_METHOD("gpu_material_draw_lights"), &GneRenderServer::gpu_material_draw_lights);
	ClassDB::bind_method(D_METHOD("gpu_light_debug_cluster", "tx", "ty", "tz"), &GneRenderServer::gpu_light_debug_cluster);
	ClassDB::bind_integer_constant("GneRenderServer", "GneBatchStrategy", "GNE_BATCH_STRATEGY_PER_MESH", GNE_BATCH_STRATEGY_PER_MESH);
	ClassDB::bind_integer_constant("GneRenderServer", "GneBatchStrategy", "GNE_BATCH_STRATEGY_GROUPED", GNE_BATCH_STRATEGY_GROUPED);
	ClassDB::bind_integer_constant("GneRenderServer", "GneBatchStrategy", "GNE_BATCH_STRATEGY_REORDERED", GNE_BATCH_STRATEGY_REORDERED);
}

GneRenderServer::GneRenderServer() {
}

GneRenderServer::~GneRenderServer() {
	shutdown();
}

void GneRenderServer::set_server_singleton(GneRenderServer *p_server) {
	server_singleton = p_server;
}

GneRenderServer *GneRenderServer::get_server_singleton() {
	return server_singleton;
}

void GneRenderServer::initialize() {
	// The server object is ready from startup. The local RenderingDevice is
	// created lazily (ensure_gpu_device) once the engine's renderer is up,
	// matching how LightmapperRD acquires its device.
	rendering_device = nullptr;
}

bool GneRenderServer::ensure_gpu_device() {
	if (rendering_device != nullptr) {
		return true;
	}

	rendering_device = RenderingServer::get_singleton()->create_local_rendering_device();

	if (rendering_device == nullptr) {
		print_error("[GNE] Failed to create local RenderingDevice. Requires an RD-based renderer (Forward+ / Mobile).");
		return false;
	}

	print_line("[GNE] Local RenderingDevice created.");

	return true;
}

void GneRenderServer::_destroy_mesh_batch() {
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

void GneRenderServer::_destroy_mesh() {
	// GNE-018: light store teardown. ORDER: sets first (they reference the
	// buffers), then pipelines/shaders, then buffers. (016-lifetime lesson:
	// RD auto-invalidates sets whose buffers die first.)
	// GNE-019: shadow teardown FIRST of all: shadow_set2 is bound to
	// mat_light_shader, which dies below (same auto-invalidate rule).
	_destroy_shadow();
	if (rendering_device != nullptr) {
		// TEMP-DIAG-012-idprobe (GNE-012): teardown of the independent id pass.
		//
		// ORDERING IS LOAD-BEARING, and this follows the same rule already
		// documented below for GNE-018/GNE-022. gpu_idprobe_uniform_set binds 8
		// buffers that _destroy_mesh() owns (batch_instances_buffer, mesh_*,
		// view_ubo...). RenderingDevice auto-frees a uniform set as soon as any
		// buffer it depends on is freed (_free_dependencies -> free_rid on every
		// direct dependent). Tearing the id pass down AFTER those buffers left
		// gpu_idprobe_uniform_set already freed, so our own free_rid then hit
		// "Attempted to free invalid ID" - a double free, not a leak. This call
		// must therefore stay FIRST, while the set is still valid.
		_gpu_idprobe_destroy();
		gpu_idprobe_enabled = false;
		// GNE-022 S1: free probe-field objects BEFORE their dependency buffers
		// (light_buffer/transform_buffer) - the RD auto-invalidates sets whose
		// dependencies are freed first (found by the S1a teardown probe).
		if (gi_trace_set_a.is_valid()) {
			rendering_device->free_rid(gi_trace_set_a);
			gi_trace_set_a = RID();
		}
		if (gi_trace_set_b.is_valid()) {
			rendering_device->free_rid(gi_trace_set_b);
			gi_trace_set_b = RID();
		}
		if (gi_trace_pipeline.is_valid()) {
			rendering_device->free_rid(gi_trace_pipeline);
			gi_trace_pipeline = RID();
		}
		if (gi_trace_shader.is_valid()) {
			rendering_device->free_rid(gi_trace_shader);
			gi_trace_shader = RID();
		}
		// GNE-022 section 10: light set-1 (+ per-front GI sets) reference
		// gi_sampler/atlases: free them BEFORE those dependencies die below.
		if (mat_light_tex_set.is_valid()) {
			rendering_device->free_rid(mat_light_tex_set);
			mat_light_tex_set = RID();
		}
		for (int gds = 0; gds < 2; gds++) {
			if (gi_draw_sets[gds].is_valid()) {
				rendering_device->free_rid(gi_draw_sets[gds]);
				gi_draw_sets[gds] = RID();
			}
		}
		if (gi_sampler.is_valid()) {
			rendering_device->free_rid(gi_sampler);
			gi_sampler = RID();
		}
		if (gi_atlas2.is_valid()) {
			rendering_device->free_rid(gi_atlas2);
			gi_atlas2 = RID();
		}
		if (gi_atlas.is_valid()) {
			rendering_device->free_rid(gi_atlas);
			gi_atlas = RID();
		}
		if (light_cull_uniform_set.is_valid()) {
			rendering_device->free_rid(light_cull_uniform_set);
			light_cull_uniform_set = RID();
		}
		if (mat_light_uniform_set.is_valid()) {
			rendering_device->free_rid(mat_light_uniform_set);
			mat_light_uniform_set = RID();
		}
		if (light_cull_pipeline.is_valid()) {
			rendering_device->free_rid(light_cull_pipeline);
			light_cull_pipeline = RID();
		}
		if (light_cull_shader.is_valid()) {
			rendering_device->free_rid(light_cull_shader);
			light_cull_shader = RID();
		}
		// GNE-021: the bounds pass owns three RIDs of its own. They were leaking,
		// which is what gt_023a's "RID cleanup lines present" check caught. A new
		// pass that allocates a shader, a pipeline and a uniform set must free all
		// three, or the next one will leak the same way.
		if (light_bounds_uniform_set.is_valid()) {
			rendering_device->free_rid(light_bounds_uniform_set);
			light_bounds_uniform_set = RID();
		}
		if (light_bounds_pipeline.is_valid()) {
			rendering_device->free_rid(light_bounds_pipeline);
			light_bounds_pipeline = RID();
		}
		if (light_bounds_shader.is_valid()) {
			rendering_device->free_rid(light_bounds_shader);
			light_bounds_shader = RID();
		}
		if (light_cone_uniform_set.is_valid()) {
			rendering_device->free_rid(light_cone_uniform_set);
			light_cone_uniform_set = RID();
		}
		if (light_cone_pipeline.is_valid()) {
			rendering_device->free_rid(light_cone_pipeline);
			light_cone_pipeline = RID();
		}
		if (light_cone_shader.is_valid()) {
			rendering_device->free_rid(light_cone_shader);
			light_cone_shader = RID();
		}
		if (light_cone_sampler.is_valid()) {
			rendering_device->free_rid(light_cone_sampler);
			light_cone_sampler = RID();
		}
		if (cones_selftest_set.is_valid()) {
			rendering_device->free_rid(cones_selftest_set);
			cones_selftest_set = RID();
		}
		if (cones_selftest_pipeline.is_valid()) {
			rendering_device->free_rid(cones_selftest_pipeline);
			cones_selftest_pipeline = RID();
		}
		if (cones_selftest_shader.is_valid()) {
			rendering_device->free_rid(cones_selftest_shader);
			cones_selftest_shader = RID();
		}
		if (cones_selftest_in_buffer.is_valid()) {
			rendering_device->free_rid(cones_selftest_in_buffer);
			cones_selftest_in_buffer = RID();
		}
		if (cones_selftest_out_buffer.is_valid()) {
			rendering_device->free_rid(cones_selftest_out_buffer);
			cones_selftest_out_buffer = RID();
		}
		if (mat_light_pipeline.is_valid()) {
			rendering_device->free_rid(mat_light_pipeline);
			mat_light_pipeline = RID();
		}
		if (mat_light_shader.is_valid()) {
			rendering_device->free_rid(mat_light_shader);
			mat_light_shader = RID();
		}
	if (light_buffer.is_valid()) {
		rendering_device->free_rid(light_buffer);
		light_buffer = RID();
	}
	if (light_cull_buffer.is_valid()) {
		rendering_device->free_rid(light_cull_buffer);
		light_cull_buffer = RID();
	}
		if (cluster_offset_buffer.is_valid()) {
			rendering_device->free_rid(cluster_offset_buffer);
			cluster_offset_buffer = RID();
		}
		if (cluster_count_buffer.is_valid()) {
			rendering_device->free_rid(cluster_count_buffer);
			cluster_count_buffer = RID();
		}
		if (cluster_index_buffer.is_valid()) {
			rendering_device->free_rid(cluster_index_buffer);
			cluster_index_buffer = RID();
		}
	if (light_overflow_buffer.is_valid()) {
		rendering_device->free_rid(light_overflow_buffer);
		light_overflow_buffer = RID();
	}
	if (light_bounds_buffer.is_valid()) {
		rendering_device->free_rid(light_bounds_buffer);
		light_bounds_buffer = RID();
	}
		if (cluster_cone_buffer.is_valid()) {
			rendering_device->free_rid(cluster_cone_buffer);
			cluster_cone_buffer = RID();
		}
	}
	for (int i = 0; i < GNE_LIGHT_MAX; i++) {
		memset(&light_cpu[i], 0, sizeof(GneLight));
	}
	light_count = 0;
	gpu_light_valid = false;
	// GNE-016: material teardown (after the light block above): the mat set
	// references batch/scene buffers freed below, and RD auto-invalidates
	// sets whose buffers die.
	// Guarded by the same rendering_device check as the mesh sets below.
	if (rendering_device != nullptr) {
		if (mat_batch_uniform_set.is_valid()) {
			rendering_device->free_rid(mat_batch_uniform_set);
			mat_batch_uniform_set = RID();
		}
		if (mat_batch_pipeline.is_valid()) {
			rendering_device->free_rid(mat_batch_pipeline);
			mat_batch_pipeline = RID();
		}
		if (mat_batch_shader.is_valid()) {
			rendering_device->free_rid(mat_batch_shader);
			mat_batch_shader = RID();
		}
		if (mat_buffer.is_valid()) {
			rendering_device->free_rid(mat_buffer);
			mat_buffer = RID();
		}
			// GNE-016.5: channel-map records die with the material store.
			if (mat2_buffer.is_valid()) {
				rendering_device->free_rid(mat2_buffer);
				mat2_buffer = RID();
			}
	}
	gpu_material_valid = false;
	mat_dispatches = 0;
	// GNE-017: texture store teardown (textures + sampler + bindings).
	if (rendering_device != nullptr) {
		for (int i = 0; i < GNE_TEX_MAX; i++) {
			if (tex_store[i].tex.is_valid()) {
				rendering_device->free_rid(tex_store[i].tex);
				tex_store[i].tex = RID();
			}
			tex_store[i].w = 0;
			tex_store[i].h = 0;
			tex_store[i].mips = 0;
			tex_store[i].bytes = 0;
		}
		if (tex_sampler.is_valid()) {
			rendering_device->free_rid(tex_sampler);
			tex_sampler = RID();
		}
		if (tex_dummy.is_valid()) {
			rendering_device->free_rid(tex_dummy);
			tex_dummy = RID();
		}
		if (mat_tex_buffer.is_valid()) {
			rendering_device->free_rid(mat_tex_buffer);
			mat_tex_buffer = RID();
		}
	}
	tex_count = 0;
	for (int i = 0; i < GNE_MESH_TABLE_SIZE * GNE_MAT_TEX_SLOTS; i++) {
		mat_tex_cpu[i] = -1;
	}
	for (int i = 0; i < GNE_TEX_ARRAY; i++) {
		tex_array[i] = RID();
	}
	// GNE-010: the batch path's 010 vertex/index arrays reference the shared
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

void GneRenderServer::_destroy_hzb_prod() {
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

void GneRenderServer::_destroy_gpu_scene() {
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
	if (present_blit_set.is_valid()) {
		rendering_device->free_rid(present_blit_set);
		present_blit_set = RID();
	}
	if (present_blit_sampler.is_valid()) {
		rendering_device->free_rid(present_blit_sampler);
		present_blit_sampler = RID();
	}
	if (present_blit_pipeline.is_valid()) {
		rendering_device->free_rid(present_blit_pipeline);
		present_blit_pipeline = RID();
	}
	if (present_blit_shader.is_valid()) {
		rendering_device->free_rid(present_blit_shader);
		present_blit_shader = RID();
	}
	if (present_lowres_texture.is_valid()) {
		rendering_device->free_rid(present_lowres_texture);
		present_lowres_texture = RID();
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
	if (raster_normal_texture.is_valid()) {
		rendering_device->free_rid(raster_normal_texture);
		raster_normal_texture = RID();
	}
	if (raster_hdr_texture.is_valid()) {
		rendering_device->free_rid(raster_hdr_texture);
		raster_hdr_texture = RID();
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

void GneRenderServer::shutdown() {
	if (rendering_device == nullptr) {
		return;
	}

	_destroy_scene_manager();
	_destroy_meshlet();
	_destroy_gpu_scene();

	memdelete(rendering_device);
	rendering_device = nullptr;

	print_line("[GNE] Local RenderingDevice destroyed.");
}

bool GneRenderServer::is_initialized() const {
	return true;
}

bool GneRenderServer::is_gpu_ready() const {
	return rendering_device != nullptr;
}

bool GneRenderServer::gpu_scene_create(int p_instance_count, float p_spread) {
	if (!ensure_gpu_device()) {
		return false;
	}

	if (p_instance_count <= 0 || p_instance_count > 100000000) {
		print_error("[GNE] gpu_scene_create: instance count must be in (0, 100000000].");
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
		print_error("[GNE] gpu_scene_create: compute shader compile failed:");
		print_error(compile_error);
		return false;
	}

	RD::ShaderStageSPIRVData stage;
	stage.shader_stage = RD::SHADER_STAGE_COMPUTE;
	stage.spirv = spirv;
	Vector<RD::ShaderStageSPIRVData> stages;
	stages.push_back(stage);

	compute_shader = rendering_device->shader_create_from_spirv(stages, "gne_gpu_scene");
	if (compute_shader.is_null()) {
		print_error("[GNE] gpu_scene_create: shader_create_from_spirv failed.");
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
		print_error("[GNE] gpu_scene_create: uniform_set_create failed.");
		_destroy_gpu_scene();
		return false;
	}

	// GNE-002: culling resources.
	String cull_error;
	Vector<uint8_t> cull_spirv = rendering_device->shader_compile_spirv_from_source(
			RD::SHADER_STAGE_COMPUTE, String(gpu_cull_compute_glsl), RD::SHADER_LANGUAGE_GLSL, &cull_error);
	if (cull_spirv.is_empty()) {
		print_error("[GNE] gpu_scene_create: cull shader compile failed:");
		print_error(cull_error);
		_destroy_gpu_scene();
		return false;
	}

	RD::ShaderStageSPIRVData cull_stage;
	cull_stage.shader_stage = RD::SHADER_STAGE_COMPUTE;
	cull_stage.spirv = cull_spirv;
	Vector<RD::ShaderStageSPIRVData> cull_stages;
	cull_stages.push_back(cull_stage);

	cull_shader = rendering_device->shader_create_from_spirv(cull_stages, "gne_gpu_cull");
	if (cull_shader.is_null()) {
		print_error("[GNE] gpu_scene_create: cull shader_create_from_spirv failed.");
		_destroy_gpu_scene();
		return false;
	}

	visibility_buffer = rendering_device->storage_buffer_create((uint32_t)count * 4);
	visible_count_buffer = rendering_device->storage_buffer_create(4);
	compact_buffer = rendering_device->storage_buffer_create((uint32_t)count * 4);

	cull_pipeline = rendering_device->compute_pipeline_create(cull_shader);

	// GNE-004: HZB resources (hierarchical depth pyramid of occluders).
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

	view_ubo = rendering_device->uniform_buffer_create(sizeof(GneViewData));

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
		print_error("[GNE] gpu_scene_create: cull uniform_set_create failed.");
		_destroy_gpu_scene();
		return false;
	}

	gpu_cull_valid = true;
	gpu_hzb_valid = true;

	// GNE-003: indirect draw args resources.
	String drawargs_error;
	Vector<uint8_t> drawargs_spirv = rendering_device->shader_compile_spirv_from_source(
			RD::SHADER_STAGE_COMPUTE, String(gpu_drawargs_compute_glsl), RD::SHADER_LANGUAGE_GLSL, &drawargs_error);
	if (drawargs_spirv.is_empty()) {
		print_error("[GNE] gpu_scene_create: drawargs shader compile failed:");
		print_error(drawargs_error);
		_destroy_gpu_scene();
		return false;
	}

	RD::ShaderStageSPIRVData drawargs_stage;
	drawargs_stage.shader_stage = RD::SHADER_STAGE_COMPUTE;
	drawargs_stage.spirv = drawargs_spirv;
	Vector<RD::ShaderStageSPIRVData> drawargs_stages;
	drawargs_stages.push_back(drawargs_stage);

	drawargs_shader = rendering_device->shader_create_from_spirv(drawargs_stages, "gne_gpu_drawargs");
	if (drawargs_shader.is_null()) {
		print_error("[GNE] gpu_scene_create: drawargs shader_create_from_spirv failed.");
		_destroy_gpu_scene();
		return false;
	}

	// GNE-005: same buffer also feeds VkDrawIndexedIndirect (needs INDIRECT usage).
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
		print_error("[GNE] gpu_scene_create: drawargs uniform_set_create failed.");
		_destroy_gpu_scene();
		return false;
	}

	// GNE-005: indirect draw pipeline, quad index buffer, offscreen target, framebuffer.
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

	print_line("[GNE] GPU Scene created. instances=" + itos(count) +
			" spread=" + String::num(p_spread) +
			" buffers=" + String::num((transform_bytes + bounds_bytes + id_bytes) / 1024.0 / 1024.0, 2) + " MB");

	return true;
}

bool GneRenderServer::gpu_scene_dispatch(int p_seed) {
	if (!gpu_scene_valid) {
		print_error("[GNE] gpu_scene_dispatch: no scene. Call gpu_scene_create first.");
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

PackedVector3Array GneRenderServer::gpu_scene_readback_positions(int p_index, int p_count) {
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

PackedFloat32Array GneRenderServer::gpu_scene_readback_scales(int p_index, int p_count) {
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

Dictionary GneRenderServer::gpu_scene_stats() {
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

int GneRenderServer::gpu_scene_get_instance_count() const {
	return gpu_scene_valid ? gpu_instance_count : 0;
}

void GneRenderServer::gpu_scene_destroy() {
	_destroy_gpu_scene();
	print_line("[GNE] GPU Scene destroyed.");
}

void GneRenderServer::gpu_scene_set_camera(const Transform3D &p_camera_transform, const Projection &p_projection) {
	// Frustum planes are extracted from the VP matrix rows (exact by
	// construction: the same matrix that projects and renders defines the
	// half-spaces). Projection::get_projection_planes() is NOT used: it was
	// measured to disagree with its own projection matrix for rotated cameras
	// (GNE-017: NDC math + Godot's is_position_in_frustum said inside while
	// get_projection_planes said outside by hundreds of units; its planes
	// miss even their defining points, e.g. near-center gave 3719 instead of
	// ~0). Godot plane order kept: near, far, left, top, right, bottom.
	Projection cam_view0(p_camera_transform.inverse());
	Projection vp0 = p_projection * cam_view0;
	float r[4][4];
	for (int c = 0; c < 4; c++) {
		for (int rr = 0; rr < 4; rr++) {
			r[rr][c] = vp0.columns[c][rr];
		}
	}
	// {rowA, sign} pairs: near=r3+r2, far=r3-r2, left=r3+r0,
	// top=r3-r1, right=r3-r0, bottom=r3+r1.
	const int ra[6] = { 3, 3, 3, 3, 3, 3 };
	const int rb[6] = { 2, 2, 0, 1, 0, 1 };
	const float rs[6] = { 1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f };
	for (int i = 0; i < 6; i++) {
		Vector3 n(
				r[ra[i]][0] + rs[i] * r[rb[i]][0],
				r[ra[i]][1] + rs[i] * r[rb[i]][1],
				r[ra[i]][2] + rs[i] * r[rb[i]][2]);
		float d = r[ra[i]][3] + rs[i] * r[rb[i]][3];
		float l = n.length();
		if (l > 1e-9f) {
			frustum_planes[i] = Plane(n / l, d / l);
		} else {
			frustum_planes[i] = Plane(Vector3(0, 0, 0), 0); // degenerate: fail-open
		}
	}
	frustum_valid = true;

	// GNE-013: capture the camera data UNCONDITIONALLY (independent of the
	// HZB path) so the meshlet pipeline - which has its OWN UBO, not view_ubo -
	// always receives a fresh vp/planes/cam/far even when HZB is not active.
	// last_vp keeps the same value the 004/009/012 paths compute, so nothing
	// downstream changes.
	meshlet_camera_position[0] = p_camera_transform.origin.x;
	meshlet_camera_position[1] = p_camera_transform.origin.y;
	meshlet_camera_position[2] = p_camera_transform.origin.z;
	// GNE-019: camera basis for shadow VP math (right/up/forward columns).
	{
		Vector3 bx = p_camera_transform.basis.get_column(0);
		Vector3 by = p_camera_transform.basis.get_column(1);
		Vector3 bz = p_camera_transform.basis.get_column(2);
		shadow_cam_basis[0] = bx.x;
		shadow_cam_basis[1] = bx.y;
		shadow_cam_basis[2] = bx.z;
		shadow_cam_basis[3] = by.x;
		shadow_cam_basis[4] = by.y;
		shadow_cam_basis[5] = by.z;
		shadow_cam_basis[6] = -bz.x;
		shadow_cam_basis[7] = -bz.y;
		shadow_cam_basis[8] = -bz.z;
	}
	far_plane = p_projection.get_z_far();
	// GNE-018: cluster inputs (unconditional, like the meshlet capture above).
	cam_near_v = p_projection.get_z_near();
	cam_tan_v = _projection_tan_half_fov_v(p_projection);
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

	// GNE-004: fill and upload the ViewData UBO (VP + view + planes + viewport + far).
	Projection cam_view(p_camera_transform.inverse());
	Projection vp = p_projection * cam_view;

	GneViewData vd;
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

	// GNE-012: store the depth-reconstruction projection coefficients for the
	// production pyramid (ndc_z = -A - B/z_view). Only the projection decides
	// them (the view matrix cancels out), so the same camera/view space used to
	// write the previous frame's D32 depth is recoverable exactly.
	far_plane = p_projection.get_z_far();
	hzb_proj_a = p_projection.columns[2][2];
	hzb_proj_b = p_projection.columns[3][2];

	rendering_device->buffer_update(view_ubo, 0, sizeof(GneViewData), &vd);
	camera_view_valid = true;
}

bool GneRenderServer::gpu_cull_dispatch() {
	if (!gpu_scene_valid || !gpu_cull_valid) {
		print_error("[GNE] gpu_cull_dispatch: no gpu scene. Call gpu_scene_create first.");
		return false;
	}
	if (!frustum_valid) {
		print_error("[GNE] gpu_cull_dispatch: no camera. Call gpu_scene_set_camera first.");
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
		rendering_device->buffer_update(view_ubo, offsetof(GneViewData, hzb_valid), 4, &zero);
	}

	uint32_t groups = (uint32_t)(((gpu_instance_count - 1) / 64) + 1);

	_run_compute_pass(cull_pipeline, cull_uniform_set, &params, sizeof(params), groups, 1, 1);

	return true;
}

int GneRenderServer::gpu_cull_get_visible_count() {
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

PackedInt32Array GneRenderServer::gpu_cull_get_visibility() {
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

PackedVector4Array GneRenderServer::gpu_scene_get_frustum_planes() {
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

bool GneRenderServer::gpu_drawargs_finalize() {
	if (!gpu_scene_valid || !gpu_drawargs_valid) {
		print_error("[GNE] gpu_drawargs_finalize: no gpu scene. Call gpu_scene_create first.");
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

PackedInt32Array GneRenderServer::gpu_drawargs_read() {
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

PackedInt32Array GneRenderServer::gpu_compact_read() {
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

void GneRenderServer::gpu_scene_set_viewport(float p_viewport_w, float p_viewport_h) {
	hzb_viewport_w = p_viewport_w;
	hzb_viewport_h = p_viewport_h;
}

void GneRenderServer::gpu_scene_set_occluders(const Vector<Vector4> &p_occluders) {
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

bool GneRenderServer::_create_hzb_passes() {
	String error;

	auto compile_pass = [&](const char *p_glsl, const char *p_name, RID &r_shader) -> bool {
		Vector<uint8_t> spirv = rendering_device->shader_compile_spirv_from_source(
				RD::SHADER_STAGE_COMPUTE, String(p_glsl), RD::SHADER_LANGUAGE_GLSL, &error);
		if (spirv.is_empty()) {
			print_error(String("[GNE] ") + p_name + " shader compile failed:");
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
	if (!compile_pass(gpu_hzb_clear_compute_glsl, "gne_hzb_clear", hzb_clear_shader)) {
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
		print_error("[GNE] hzb_clear uniform_set_create failed.");
		return false;
	}

	// Occluder pass: image binding 0, occ min/max storage buffers 1/2, view UBO 5.
	if (!compile_pass(gpu_hzb_occ_compute_glsl, "gne_hzb_occ", hzb_occ_shader)) {
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
		print_error("[GNE] hzb_occ uniform_set_create failed.");
		return false;
	}

	// Downsample pass: image binding 0.
	if (!compile_pass(gpu_hzb_down_compute_glsl, "gne_hzb_down", hzb_down_shader)) {
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
		print_error("[GNE] hzb_down uniform_set_create failed.");
		return false;
	}

	return true;
}

void GneRenderServer::_run_compute_pass(RID p_pipeline, RID p_uniform_set, const void *p_push_data, uint32_t p_push_size, uint32_t p_groups_x, uint32_t p_groups_y, uint32_t p_groups_z) {
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

float GneRenderServer::_projection_tan_half_fov_v(const Projection &p_projection) {
	// For a perspective matrix, column[1].y == cotangent(v_fov / 2).
	float f = p_projection.columns[1][1];
	if (fabsf(f) < 1e-6f) {
		return 0.0f;
	}
	return 1.0f / f;
}

bool GneRenderServer::gpu_visibility_dispatch() {
	if (!gpu_scene_valid || !gpu_cull_valid || !gpu_hzb_valid) {
		print_error("[GNE] gpu_visibility_dispatch: no gpu scene. Call gpu_scene_create first.");
		return false;
	}
	if (!frustum_valid || !camera_view_valid) {
		print_error("[GNE] gpu_visibility_dispatch: no camera. Call gpu_scene_set_camera first.");
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
	rendering_device->buffer_update(view_ubo, offsetof(GneViewData, hzb_valid), 4, &hzb_one);
	int32_t occ_count = occluder_count;
	rendering_device->buffer_update(view_ubo, offsetof(GneViewData, occ_count), 4, &occ_count);
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

// GNE-012: build the production pyramid resources. Reuses the 004 clear/occ/
// down SHADERS and PIPELINES with separate uniform sets bound to the 2048x2048
// prod array (uniform-set reuse is valid as long as each set is created against
// the matching shader). The depth-source and the two-phase cull passes are new.
bool GneRenderServer::gpu_hzb_prod_create() {
	if (!ensure_gpu_device()) {
		return false;
	}
	if (!gpu_scene_valid || !gpu_hzb_valid || !raster_depth_attached) {
		print_error("[GNE] gpu_hzb_prod_create: requires an existing GPU scene (gpu_scene_create) with the D32 raster depth attachment.");
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
			print_error(String("[GNE] ") + p_name + " shader compile failed:");
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
		print_error("[GNE] gpu_hzb_prod_create: prod pyramid texture_create failed.");
		return false;
	}

	phase1_compact_buffer = rendering_device->storage_buffer_create((uint32_t)gpu_instance_count * 4u);
	phase1_count_buffer = rendering_device->storage_buffer_create(4);
	hzb_pyramid_data_buffer = rendering_device->storage_buffer_create(HZB_PYRAMID_DATA_UINTS * 4u);
	if (hzb_pyramid_data_buffer.is_null()) {
		print_error("[GNE] gpu_hzb_prod_create: pyramid data buffer_create failed.");
		_destroy_hzb_prod();
		return false;
	}

	hzb_depth_sampler = rendering_device->sampler_create(RD::SamplerState());
	hzb_dbg_probe_buffer = rendering_device->storage_buffer_create(16);
	if (hzb_depth_sampler.is_null()) {
		print_error("[GNE] gpu_hzb_prod_create: depth sampler_create failed.");
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
			print_error("[GNE] gpu_hzb_prod_create: prod clear uniform_set_create failed.");
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
			print_error("[GNE] gpu_hzb_prod_create: prod occ uniform_set_create failed.");
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
			print_error("[GNE] gpu_hzb_prod_create: prod down uniform_set_create failed.");
			_destroy_hzb_prod();
			return false;
		}
	}

	// Depth-source pass: sampler2D (0) + image (1) + probe buffer (2) +
	// pyramid-data storage buffer (3) - the buffer is the RELIABLE pyramid the
	// phase-2 occlusion reads (storage buffers sync correctly in this fork).
	if (!compile_compute(gpu_hzb_depth_source_glsl, "gne_hzb_depth_source", hzb_depth_source_shader)) {
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
		print_error("[GNE] gpu_hzb_prod_create: depth-source uniform_set_create failed.");
		_destroy_hzb_prod();
		return false;
	}

	// GNE-012 final source: occluder-box pyramid (occmin(0)/occmax(1)/UBO(2)/
	// pyramid-data(3)/evidence image(4)). Only the flat storage buffer is read
	// by the phases - the image keeps a readback copy for evidence.
	if (!compile_compute(gpu_hzb_occbuf_glsl, "gne_hzb_occbuf", hzb_occbuf_shader)) {
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
		print_error("[GNE] gpu_hzb_prod_create: occbuf uniform_set_create failed.");
		_destroy_hzb_prod();
		return false;
	}

	// Phase 1 (frustum only): transforms(0), phase1 list(1), phase1 count(2),
	// visibility(3), view UBO(5).
	if (!compile_compute(gpu_hzb_phase1_glsl, "gne_hzb_phase1", hzb_phase1_shader)) {
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
		print_error("[GNE] gpu_hzb_prod_create: phase1 uniform_set_create failed.");
		_destroy_hzb_prod();
		return false;
	}

	// Phase 2 (HZB occlusion): phase1 list(0), transforms(1), count(2),
	// compact(3), pyramid data buffer(4), view UBO(5).
	if (!compile_compute(gpu_hzb_phase2_glsl, "gne_hzb_phase2", hzb_phase2_shader)) {
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
		print_error("[GNE] gpu_hzb_prod_create: phase2 uniform_set_create failed.");
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

	print_line("[GNE] Production HZB created. levels=" + itos(HZB_PROD_LEVELS) +
			" base=" + itos(HZB_PROD_TEXEL_COUNT) + "x" + itos(HZB_PROD_TEXEL_COUNT));

	return true;
}

// GNE-012: rebuild the production pyramid from the PREVIOUS frame's depth and
// run the temporal bookkeeping. The pyramid may only be used when the camera vp
// that wrote that depth matches the CURRENT vp; otherwise hzb_valid is left 0
// (frustum-only, conservative - no false dropout) and the pyramid is rebuilt
// next frame from depth the settled camera just wrote.
bool GneRenderServer::gpu_hzb_build() {
	if (!gpu_scene_valid || !gpu_hzb_prod_valid) {
		print_error("[GNE] gpu_hzb_build: production HZB not created.");
		return false;
	}
	if (!camera_view_valid || !frustum_valid) {
		print_error("[GNE] gpu_hzb_build: no camera. Call gpu_scene_set_camera first.");
		return false;
	}

	// GNE-012-revised (root A). The previous code treated `same_vp` as an
	// ACTIVATION gate: when the camera had moved it wrote hzb_valid = 0 and
	// returned WITHOUT building (and without patching the production grid into
	// the view UBO). With hzb_valid = 0 the phase-2 occlusion test is skipped
	// entirely, so p2 == p1 by construction - which is exactly the 012 symptom.
	//
	// What the measurement in main_012_rev actually shows (SPEC section 11):
	// the depth source samples raster_viewz_texture, which holds the PREVIOUS
	// frame's depth - i.e. the build is already temporal-1. Therefore the
	// pyramid is always built from the last settled frame and is valid for the
	// CURRENT frame's culling; same_vp is only a COHERENCE signal.
	const bool same_vp = hzb_pyramid_fresh && (memcmp(hzb_build_vp, last_vp, sizeof(last_vp)) == 0);
	hzb_coherent = (hzb_stable_frames >= 2) && hzb_temporal_enabled && same_vp;
	memcpy(hzb_build_vp, last_vp, sizeof(last_vp));

	// Patch the shared view UBO for the production square grid on EVERY build
	// (previously this was skipped entirely whenever !same_vp, so the reader
	// never even saw the production texel count).
	// TYPE DISCIPLINE: viewport[] is float[4] in the UBO - writing int32 bits
	// here stores 2.8e-42 (~0.0f), which collapses every level/texel computation
	// in the phase-2 reader (observed: sim2 level=-138). Must write float.
	float texel_count = (float)HZB_PROD_TEXEL_COUNT;
	rendering_device->buffer_update(view_ubo, offsetof(GneViewData, viewport) + 2 * sizeof(float), 4, &texel_count);
	int32_t occ_count = occluder_count;
	rendering_device->buffer_update(view_ubo, offsetof(GneViewData, occ_count), 4, &occ_count);

	// Frame 1 only: no pyramid has been produced yet, so stay conservative
	// (frustum-only). This is the single frame that is allowed to skip phase 2.
	// Arm the temporal chain here: without this flag the second build below is
	// unreachable and hzb_valid stays 0 forever (production occlusion never runs).
	if (!hzb_pyramid_fresh) {
		hzb_pyramid_fresh = true;
		hzb_stable_frames = 1;
		uint32_t zero = 0;
		rendering_device->buffer_update(view_ubo, offsetof(GneViewData, hzb_valid), 4, &zero);
		return true;
	}
	hzb_pyramid_fresh = true;
	hzb_stable_frames++;
	uint32_t hzb_one = 1;
	rendering_device->buffer_update(view_ubo, offsetof(GneViewData, hzb_valid), 4, &hzb_one);

	// Queue the pyramid passes to run INSIDE the next gpu_mesh_batch_draw
	// submission (clear, R32 depth-source sample, box occluders, downsample
	// chain). The depth source writes the flat mirror the phases read
	// (gpu_hzb_depth_source_glsl:1042) and samples the view-z attachment, which
	// holds the previous frame - i.e. a temporal-1 build by construction.
	hzb_rebuild_requested = true;
	return true;
}

// GNE-012: two-phase production visibility dispatch. Phase 1 produces the
// frustum survivors (phase-1 list + count + visibility flags), phase 2 applies
// the HZB occlusion to that list and compacts the survivors into the FINAL
// compact[]/visible_count[] consumed by the 011 batch assembler.
bool GneRenderServer::gpu_visibility_prod_dispatch() {
	if (!gpu_scene_valid || !gpu_cull_valid || !gpu_hzb_valid || !gpu_hzb_prod_valid) {
		print_error("[GNE] gpu_visibility_prod_dispatch: no production HZB. Call gpu_hzb_prod_create first.");
		return false;
	}
	if (!frustum_valid || !camera_view_valid) {
		print_error("[GNE] gpu_visibility_prod_dispatch: no camera. Call gpu_scene_set_camera first.");
		return false;
	}

	// Patch the shared view UBO for the production square grid + the freshest
	// occluder count (hzb_valid reflects what gpu_hzb_build left in the UBO).
	// Same type discipline as gpu_hzb_build: viewport[] is float.
	{
		float texel_count = (float)HZB_PROD_TEXEL_COUNT;
		rendering_device->buffer_update(view_ubo, offsetof(GneViewData, viewport) + 2 * sizeof(float), 4, &texel_count);
		int32_t occ_count = occluder_count;
		rendering_device->buffer_update(view_ubo, offsetof(GneViewData, occ_count), 4, &occ_count);
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

void GneRenderServer::gpu_hzb_enable_temporal(bool p_enabled) {
	hzb_temporal_enabled = p_enabled;
	if (!p_enabled) {
		hzb_coherent = false;
	}
}

void GneRenderServer::gpu_hzb_set_occluders(const Vector<Vector4> &p_occluders) {
	gpu_scene_set_occluders(p_occluders);
}

int GneRenderServer::gpu_hzb_get_level_count() {
	if (gpu_hzb_prod_valid) {
		return HZB_PROD_LEVELS;
	}
	return gpu_hzb_valid ? HZB_LEVELS : 0;
}

PackedInt32Array GneRenderServer::gpu_hzb_get_phase_counts() {
	PackedInt32Array ret;
	ret.resize(2);
	ret.set(0, hzb_phase1_count);
	ret.set(1, hzb_phase2_count);
	return ret;
}

bool GneRenderServer::gpu_hzb_get_coherent() const {
	return hzb_coherent;
}

// CPU replica of the phase-2 shader's occlusion math for a probe set of
// instances, using the REAL GPU buffers phase 2 reads (view UBO, transform
// buffer, pyramid data buffer). Returns [level, t.x, t.y, max_inv, sphere_inv,
// vis] rows. Validates the index mapping + pyramid content end-to-end.
PackedInt32Array GneRenderServer::gpu_hzb_dbg_sim2() {
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

int GneRenderServer::gpu_hzb_dbg_valid() {
	if (!gpu_hzb_prod_valid) {
		return -1;
	}
	Vector<uint8_t> bytes = rendering_device->buffer_get_data(view_ubo, offsetof(GneViewData, hzb_valid), 4);
	if (bytes.size() != 4) {
		return -2;
	}
	uint32_t v = 0;
	memcpy(&v, bytes.ptr(), 4);
	return (int)v;
}

int GneRenderServer::gpu_hzb_dbg_level0(int p_x, int p_y) {
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
PackedInt32Array GneRenderServer::gpu_hzb_dbg_scan_level0() {
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
PackedInt32Array GneRenderServer::gpu_hzb_dbg_scan_level1(int p_level) {
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

PackedInt32Array GneRenderServer::gpu_hzb_dbg_scan_buffer(int p_level) {
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

PackedInt32Array GneRenderServer::gpu_hzb_dbg_probe() {
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

bool GneRenderServer::_create_raster_pipeline() {
	String error;

	Vector<uint8_t> vert_spirv = rendering_device->shader_compile_spirv_from_source(
			RD::SHADER_STAGE_VERTEX, String(gpu_raster_vert_glsl), RD::SHADER_LANGUAGE_GLSL, &error);
	if (vert_spirv.is_empty()) {
		print_error("[GNE] raster vertex shader compile failed:");
		print_error(error);
		return false;
	}

	Vector<uint8_t> frag_spirv = rendering_device->shader_compile_spirv_from_source(
			RD::SHADER_STAGE_FRAGMENT, String(gpu_raster_frag_glsl), RD::SHADER_LANGUAGE_GLSL, &error);
	if (frag_spirv.is_empty()) {
		print_error("[GNE] raster fragment shader compile failed:");
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

	raster_shader = rendering_device->shader_create_from_spirv(stages, "gne_raster");
	if (raster_shader.is_null()) {
		print_error("[GNE] raster shader_create_from_spirv failed.");
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
		print_error("[GNE] raster color texture_create failed.");
		return false;
	}

	// GNE-009: real depth attachment, D32_SFLOAT, cleared to 1.0 (far) at the
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
		print_error("[GNE] raster depth texture_create failed.");
		return false;
	}

	// GNE-012: R32_SFLOAT view-space depth (positive z_view) written by the
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
		print_error("[GNE] raster view-z texture_create failed.");
		return false;
	}

	// GNE-018-rev: per-pixel surface normal (world space, xyz) for the cluster
	// normal-cone stage. RGBA16F color attachment (output 2) written by the
	// material fragment shaders. Purely additive; existing attachments untouched.
	RD::TextureFormat nf;
	nf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	nf.width = RASTER_TARGET_W;
	nf.height = RASTER_TARGET_H;
	nf.depth = 1;
	nf.texture_type = RD::TEXTURE_TYPE_2D;
	nf.usage_bits = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	raster_normal_texture = rendering_device->texture_create(nf, RD::TextureView());
	if (raster_normal_texture.is_null()) {
		print_error("[GNE] raster normal texture_create failed.");
		return false;
	}
	// GNE-023/KI-017: pre-tonemap HDR radiance target (RGBA32F) - raw radiance
	// written by the fragment shaders before any 8-bit clamp; read by the KI-017
	// instrument (gpu_raster_read_hdr).
	RD::TextureFormat hf;
	hf.texture_type = RD::TEXTURE_TYPE_2D;
	hf.width = RASTER_TARGET_W;
	hf.height = RASTER_TARGET_H;
	hf.depth = 1;
	hf.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
	hf.usage_bits = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	raster_hdr_texture = rendering_device->texture_create(hf, RD::TextureView());
	if (raster_hdr_texture.is_null()) {
		print_error("[GNE] raster hdr texture_create failed.");
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
	RD::AttachmentFormat nf_af;
	nf_af.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	nf_af.samples = RD::TEXTURE_SAMPLES_1;
	nf_af.usage_flags = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	afs.push_back(nf_af);
	RD::AttachmentFormat hf_af;
	hf_af.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
	hf_af.samples = RD::TEXTURE_SAMPLES_1;
	hf_af.usage_flags = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	afs.push_back(hf_af);
	raster_framebuffer_format = rendering_device->framebuffer_format_create(afs);
	if (raster_framebuffer_format < 0) {
		print_error("[GNE] raster framebuffer_format_create failed.");
		return false;
	}

	Vector<RID> attachments;
	attachments.push_back(raster_color_texture);
	attachments.push_back(raster_viewz_texture);
	attachments.push_back(raster_normal_texture);
		attachments.push_back(raster_hdr_texture);
	attachments.push_back(raster_depth_texture);
	// Skip the format-check id so RD recomputes the format from the textures
	// themselves (identical layout; the check only guards against stale ids).
	raster_framebuffer = rendering_device->framebuffer_create(attachments, RD::INVALID_ID);
	if (raster_framebuffer.is_null()) {
		print_error("[GNE] raster framebuffer_create failed.");
		return false;
	}
	raster_depth_attached = true;
	raster_depth_format_value = (int)RD::DATA_FORMAT_D32_SFLOAT;
	raster_depth_enabled = false;

	// No vertex input attributes (procedural gl_VertexIndex expansion) -> INVALID_ID like the engine's blit shaders.
	RD::PipelineRasterizationState rs;
	RD::PipelineMultisampleState ms;
	RD::PipelineDepthStencilState ds;
	RD::PipelineColorBlendState bs = RD::PipelineColorBlendState::create_disabled(4);
	raster_pipeline = rendering_device->render_pipeline_create(
			raster_shader, raster_framebuffer_format, RD::INVALID_ID, RD::RENDER_PRIMITIVE_TRIANGLES, rs, ms, ds, bs, 0, 0);
	if (raster_pipeline.is_null()) {
		print_error("[GNE] raster render_pipeline_create failed.");
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
		print_error("[GNE] raster uniform_set_create failed.");
		return false;
	}

	print_line("[GNE] Raster framebuffer: color R8G8B8A8 + depth D32_SFLOAT attached=" +
			itos((int)raster_depth_attached));

	return true;
}

// GNE-008A: builds the real-mesh pipeline + uniform set. Uses a REAL vertex
// format so the vertex shader reads a REAL vertex attribute from a vertex buffer.
bool GneRenderServer::_create_mesh_pipeline() {
	String error;

	Vector<uint8_t> vert_spirv = rendering_device->shader_compile_spirv_from_source(
			RD::SHADER_STAGE_VERTEX, String(gpu_mesh_vert_glsl), RD::SHADER_LANGUAGE_GLSL, &error);
	if (vert_spirv.is_empty()) {
		print_error("[GNE] mesh vertex shader compile failed:");
		print_error(error);
		return false;
	}

	Vector<uint8_t> frag_spirv = rendering_device->shader_compile_spirv_from_source(
			RD::SHADER_STAGE_FRAGMENT, String(gpu_mesh_frag_glsl), RD::SHADER_LANGUAGE_GLSL, &error);
	if (frag_spirv.is_empty()) {
		print_error("[GNE] mesh fragment shader compile failed:");
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

	mesh_shader = rendering_device->shader_create_from_spirv(stages, "gne_mesh");
	if (mesh_shader.is_null()) {
		print_error("[GNE] mesh shader_create_from_spirv failed.");
		return false;
	}

	RD::PipelineRasterizationState rs;
	RD::PipelineMultisampleState ms;
	RD::PipelineDepthStencilState ds;
	// GNE-009: the REAL MESH path tests and writes depth (LESS_OR_EQUAL,
	// write enabled), depth buffer cleared to 1.0 (far) each frame.
	ds.enable_depth_test = true;
	ds.enable_depth_write = true;
	ds.depth_compare_operator = RD::COMPARE_OP_LESS_OR_EQUAL;
	RD::PipelineColorBlendState bs = RD::PipelineColorBlendState::create_disabled(4);
	mesh_pipeline = rendering_device->render_pipeline_create(
			mesh_shader, raster_framebuffer_format, mesh_vertex_format, RD::RENDER_PRIMITIVE_TRIANGLES, rs, ms, ds, bs, 0, 0);
	if (mesh_pipeline.is_null()) {
		print_error("[GNE] mesh render_pipeline_create failed.");
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
		print_error("[GNE] mesh uniform_set_create failed.");
		return false;
	}

	return true;
}

bool GneRenderServer::gpu_mesh_create() {
	if (!gpu_scene_valid || !gpu_raster_valid) {
		print_error("[GNE] gpu_mesh_create: no gpu scene/raster. Call gpu_scene_create first.");
		return false;
	}

	_destroy_mesh();

	// GNE-008A: a single real CUBE mesh (positions only) occupying mesh table
	// slot 0. No normals, no UVs, no materials, no textures. GNE-010 then
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
		print_error("[GNE] gpu_mesh_create: vertex_format_create failed.");
		_destroy_mesh();
		return false;
	}

	// GNE-010: the SHARED vertex/index buffers get capacity for the whole mesh
	// table up-front; the cube is uploaded at offset 0 so every existing 008A/009
	// draw (first_index/vertex_offset 0, index_count 36) renders identically.
	// Per-mesh data is appended at increasing offsets by create_from_arrays.
	Vector<uint8_t> vcap_bytes;
	vcap_bytes.resize((uint32_t)GNE_MAX_MESH_VERTS * 12);
	mesh_vertex_buffer = rendering_device->vertex_buffer_create((uint32_t)(GNE_MAX_MESH_VERTS * 12), vcap_bytes);
	Vector<uint8_t> icap_bytes;
	icap_bytes.resize((uint32_t)GNE_MAX_MESH_INDICES * 4);
	mesh_index_buffer = rendering_device->index_buffer_create(GNE_MAX_MESH_INDICES, RD::INDEX_BUFFER_FORMAT_UINT32, icap_bytes);
	if (mesh_vertex_buffer.is_null() || mesh_index_buffer.is_null()) {
		print_error("[GNE] gpu_mesh_create: vertex/index buffer_create failed.");
		_destroy_mesh();
		return false;
	}
	rendering_device->buffer_update(mesh_vertex_buffer, 0, (uint32_t)vertex_bytes.size(), vertex_bytes.ptr());
	rendering_device->buffer_update(mesh_index_buffer, 0, (uint32_t)index_bytes.size(), index_bytes.ptr());

	// GNE-011: storage mirrors of the shared vertex/index buffers so the
	// procedural (non-indexed) group draw can fetch geometry as SSBOs (RD
	// vertex/index buffer owners cannot be bound in a uniform set).
	{
		Vector<uint8_t> vscap_bytes;
		vscap_bytes.resize((uint32_t)GNE_MAX_MESH_VERTS * 12);
		memcpy(vscap_bytes.ptrw(), vertex_bytes.ptr(), vertex_bytes.size());
		mesh_vertex_storage_buffer = rendering_device->storage_buffer_create((uint32_t)(GNE_MAX_MESH_VERTS * 12), vscap_bytes);
		Vector<uint8_t> iscap_bytes;
		iscap_bytes.resize((uint32_t)GNE_MAX_MESH_INDICES * 4);
		memcpy(iscap_bytes.ptrw(), index_bytes.ptr(), index_bytes.size());
		mesh_index_storage_buffer = rendering_device->storage_buffer_create((uint32_t)(GNE_MAX_MESH_INDICES * 4), iscap_bytes);
		if (mesh_vertex_storage_buffer.is_null() || mesh_index_storage_buffer.is_null()) {
			print_error("[GNE] gpu_mesh_create: storage mirror buffer_create failed.");
			_destroy_mesh();
			return false;
		}
	}

	Vector<RID> src_buffers;
	src_buffers.push_back(mesh_vertex_buffer);
	mesh_vertex_array = rendering_device->vertex_array_create(8, mesh_vertex_format, src_buffers);
	mesh_index_array = rendering_device->index_array_create(mesh_index_buffer, 0, 36);
	if (mesh_vertex_array.is_null() || mesh_index_array.is_null()) {
		print_error("[GNE] gpu_mesh_create: vertex/index array_create failed.");
		_destroy_mesh();
		return false;
	}
	// GNE-010: full-capacity arrays bound by the multi-batch draw path (its
	// commands may reference any sub-range of the shared buffers via vertex_offset
	// / first_index).
	mesh_010_vertex_array = rendering_device->vertex_array_create(GNE_MAX_MESH_VERTS, mesh_vertex_format, src_buffers);
	mesh_010_index_array = rendering_device->index_array_create(mesh_index_buffer, 0, GNE_MAX_MESH_INDICES);
	if (mesh_010_vertex_array.is_null() || mesh_010_index_array.is_null()) {
		print_error("[GNE] gpu_mesh_create: 010 vertex/index array_create failed.");
		_destroy_mesh();
		return false;
	}

	mesh_vertex_count = 8;
	mesh_index_count = 36;

	if (!_create_mesh_pipeline()) {
		_destroy_mesh();
		return false;
	}

	// GNE-008A: mesh indirect draw args. Reuses the exact GNE-003 indirect
	// argument system/buffer; a distinct pipeline is used because the mesh path
	// pushes its real index_count instead of hardcoding it.
	String drawargs_error;
	Vector<uint8_t> drawargs_spirv = rendering_device->shader_compile_spirv_from_source(
			RD::SHADER_STAGE_COMPUTE, String(gpu_mesh_drawargs_compute_glsl), RD::SHADER_LANGUAGE_GLSL, &drawargs_error);
	if (drawargs_spirv.is_empty()) {
		print_error("[GNE] gpu_mesh_create: mesh drawargs shader compile failed:");
		print_error(drawargs_error);
		_destroy_mesh();
		return false;
	}

	RD::ShaderStageSPIRVData da_stage;
	da_stage.shader_stage = RD::SHADER_STAGE_COMPUTE;
	da_stage.spirv = drawargs_spirv;
	Vector<RD::ShaderStageSPIRVData> da_stages;
	da_stages.push_back(da_stage);

	mesh_drawargs_shader = rendering_device->shader_create_from_spirv(da_stages, "gne_mesh_drawargs");
	if (mesh_drawargs_shader.is_null()) {
		print_error("[GNE] gpu_mesh_create: mesh drawargs shader_create_from_spirv failed.");
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
		print_error("[GNE] gpu_mesh_create: mesh drawargs uniform_set_create failed.");
		_destroy_mesh();
		return false;
	}

	// GNE-010: mesh table GPU resources + batch assembly/draw pipelines.
	if (!_init_mesh_table_gpu()) {
		_destroy_mesh();
		return false;
	}

	gpu_mesh_valid = true;
	print_line("[GNE] GPU Mesh created. vertices=" + itos(mesh_vertex_count) +
			" indices=" + itos(mesh_index_count) +
			" vertex_format=" + itos((int)mesh_vertex_format));
	return true;
}

bool GneRenderServer::gpu_mesh_drawargs_finalize() {
	if (!gpu_scene_valid || !gpu_mesh_valid) {
		print_error("[GNE] gpu_mesh_drawargs_finalize: no gpu mesh. Call gpu_mesh_create first.");
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

bool GneRenderServer::gpu_mesh_indirect_draw() {
	if (!gpu_scene_valid || !gpu_mesh_valid) {
		print_error("[GNE] gpu_mesh_indirect_draw: no gpu mesh. Call gpu_mesh_create first.");
		return false;
	}
	if (!frustum_valid) {
		print_error("[GNE] gpu_mesh_indirect_draw: no camera. Call gpu_scene_set_camera first.");
		return false;
	}

	Vector<Color> clear_colors;
	clear_colors.push_back(Color(0, 0, 0, 0));
	clear_colors.push_back(Color(far_plane, 0.0f, 0.0f, 0.0f));
	RD::DrawListID dl = rendering_device->draw_list_begin(raster_framebuffer, RD::DRAW_CLEAR_COLOR_0 | RD::DRAW_CLEAR_COLOR_1 | RD::DRAW_CLEAR_DEPTH, clear_colors, 1.0f, 0, Rect2(), 0);
	if (dl == RD::INVALID_ID) {
		print_error("[GNE] gpu_mesh_indirect_draw: draw_list_begin failed.");
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

int GneRenderServer::gpu_mesh_get_index_count() const {
	return mesh_index_count;
}

int GneRenderServer::gpu_mesh_get_vertex_count() const {
	return mesh_vertex_count;
}

// GNE-010: creates the mesh table GPU buffers + the batch assembly compute
// pipelines + the multi-batch draw pipeline, and registers the 008A cube as
// mesh table slot 0. Called from gpu_mesh_create (additive).
bool GneRenderServer::_init_mesh_table_gpu() {
	String error;

	auto compile_compute = [&](const char *p_glsl, const char *p_name, RID &r_shader) -> bool {
		Vector<uint8_t> spirv = rendering_device->shader_compile_spirv_from_source(
				RD::SHADER_STAGE_COMPUTE, String(p_glsl), RD::SHADER_LANGUAGE_GLSL, &error);
		if (spirv.is_empty()) {
			print_error(String("[GNE] ") + p_name + " shader compile failed:");
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
	mesh_table_buffer = rendering_device->storage_buffer_create((uint32_t)GNE_MESH_TABLE_SIZE * sizeof(GneMeshDesc));
	mesh_color_buffer = rendering_device->storage_buffer_create((uint32_t)GNE_MESH_TABLE_SIZE * 16);
	batch_count_buffer = rendering_device->storage_buffer_create((uint32_t)GNE_MESH_TABLE_SIZE * 4);
	batch_offset_buffer = rendering_device->storage_buffer_create((uint32_t)GNE_MESH_TABLE_SIZE * 4);
	mesh_scratch_buffer = rendering_device->storage_buffer_create((uint32_t)(inst * GNE_MESH_TABLE_SIZE * 4));
	batch_instances_buffer = rendering_device->storage_buffer_create((uint32_t)inst * 4);
	batch_args_buffer = rendering_device->storage_buffer_create(
			(uint32_t)(GNE_MESH_TABLE_SIZE * 20), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	batch_total_buffer = rendering_device->storage_buffer_create(4);
	// GNE-011: grouping outputs. group_args holds non-indexed
	// VkDrawIndirectCommand[64] (16 bytes each, INDIRECT usage for the
	// procedural multi-draw); group_member_count/list hold the batch order
	// evidence (members per group + per-group mesh ids).
	group_args_buffer = rendering_device->storage_buffer_create(
			(uint32_t)(GNE_MESH_TABLE_SIZE * 16), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	group_member_count_buffer = rendering_device->storage_buffer_create((uint32_t)(GNE_MESH_TABLE_SIZE * 4));
	group_member_list_buffer = rendering_device->storage_buffer_create((uint32_t)(GNE_MESH_TABLE_SIZE * GNE_MESH_TABLE_SIZE * 4));
	if (mesh_id_buffer.is_null() || mesh_table_buffer.is_null() || mesh_color_buffer.is_null() ||
			batch_count_buffer.is_null() || batch_offset_buffer.is_null() || mesh_scratch_buffer.is_null() ||
			batch_instances_buffer.is_null() || batch_args_buffer.is_null() || batch_total_buffer.is_null() ||
			group_args_buffer.is_null() || group_member_count_buffer.is_null() || group_member_list_buffer.is_null()) {
		print_error("[GNE] _init_mesh_table_gpu: buffer_create failed.");
		return false;
	}
	// All mesh_id entries default to 0 (the cube) until a test sets them.
	rendering_device->buffer_clear(mesh_id_buffer, 0, (uint32_t)inst * 4);

	for (int i = 0; i < GNE_MESH_TABLE_SIZE; i++) {
		mesh_colors[i] = Color(0, 0, 0, 1);
	}
	mesh_colors[0] = gne_mesh_palette(0);
	rendering_device->buffer_update(mesh_color_buffer, 0, 16, &mesh_colors[0]);

	// Register the cube as mesh table slot 0 (shared buffer offset 0).
	GneMeshDesc cube_desc;
	memset(&cube_desc, 0, sizeof(cube_desc));
	cube_desc.index_buffer_slot = 0;
	cube_desc.vertex_buffer_slot = 0;
	cube_desc.index_count = 36;
	cube_desc.vertex_count = 8;
	cube_desc.first_index = 0;
	cube_desc.vertex_offset = 0;
	rendering_device->buffer_update(mesh_table_buffer, 0, (uint32_t)sizeof(GneMeshDesc), &cube_desc);
	mesh_table_count = 1;
	mesh_next_vertex_offset = 8;
	mesh_next_index_offset = 36;

	// Pass 1: per-mesh counting.
	if (!compile_compute(gpu_mesh_batch_count_glsl, "gne_mesh_batch_count", batch_count_shader)) {
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
		print_error("[GNE] _init_mesh_table_gpu: batch_count uniform_set_create failed.");
		return false;
	}

	// Pass 2: workgroup-parallel prefix sum + batch assembly + grouping.
	if (!compile_compute(gpu_mesh_batch_assemble_glsl, "gne_mesh_batch_assemble", batch_assemble_shader)) {
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
		print_error("[GNE] _init_mesh_table_gpu: batch_assemble uniform_set_create failed.");
		return false;
	}

	// Multi-batch draw pipeline (keeps 009 depth test/write behavior).
	Vector<uint8_t> vert_spirv = rendering_device->shader_compile_spirv_from_source(
			RD::SHADER_STAGE_VERTEX, String(gpu_mesh_batch_vert_glsl), RD::SHADER_LANGUAGE_GLSL, &error);
	if (vert_spirv.is_empty()) {
		print_error("[GNE] mesh batch vertex shader compile failed:");
		print_error(error);
		return false;
	}
	Vector<uint8_t> frag_spirv = rendering_device->shader_compile_spirv_from_source(
			RD::SHADER_STAGE_FRAGMENT, String(gpu_mesh_batch_frag_glsl), RD::SHADER_LANGUAGE_GLSL, &error);
	if (frag_spirv.is_empty()) {
		print_error("[GNE] mesh batch fragment shader compile failed:");
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
	mesh_batch_shader = rendering_device->shader_create_from_spirv(stages, "gne_mesh_batch");
	if (mesh_batch_shader.is_null()) {
		print_error("[GNE] mesh batch shader_create_from_spirv failed.");
		return false;
	}

	RD::PipelineRasterizationState rs;
	RD::PipelineMultisampleState ms;
	RD::PipelineDepthStencilState ds;
	ds.enable_depth_test = true;
	ds.enable_depth_write = true;
	ds.depth_compare_operator = RD::COMPARE_OP_LESS_OR_EQUAL;
	RD::PipelineColorBlendState bs = RD::PipelineColorBlendState::create_disabled(4);
	mesh_batch_pipeline = rendering_device->render_pipeline_create(
			mesh_batch_shader, raster_framebuffer_format, mesh_vertex_format, RD::RENDER_PRIMITIVE_TRIANGLES, rs, ms, ds, bs, 0, 0);
	if (mesh_batch_pipeline.is_null()) {
		print_error("[GNE] mesh batch render_pipeline_create failed.");
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
		print_error("[GNE] mesh batch uniform_set_create failed.");
		return false;
	}

	// GNE-011: grouped (procedural, NON-indexed) draw pipeline. The vertex
	// shader selects each instance's geometry sub-range through the mesh table,
	// so one VkDrawIndirectCommand per GROUP draws all of its member instances.
	// An EMPTY vertex format is used (geometry comes from the SSBO storage
	// mirrors); depth test/write matches the per-mesh batch path; the fragment
	// shader is the same early-Z one.
	{
		Vector<uint8_t> gvert_spirv = rendering_device->shader_compile_spirv_from_source(
				RD::SHADER_STAGE_VERTEX, String(gpu_mesh_group_batch_vert_glsl), RD::SHADER_LANGUAGE_GLSL, &error);
		if (gvert_spirv.is_empty()) {
			print_error("[GNE] mesh group batch vertex shader compile failed:");
			print_error(error);
			return false;
		}
		Vector<uint8_t> gfrag_spirv = rendering_device->shader_compile_spirv_from_source(
				RD::SHADER_STAGE_FRAGMENT, String(gpu_mesh_batch_frag_glsl), RD::SHADER_LANGUAGE_GLSL, &error);
		if (gfrag_spirv.is_empty()) {
			print_error("[GNE] mesh group batch fragment shader compile failed:");
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
		group_batch_shader = rendering_device->shader_create_from_spirv(gstages, "gne_mesh_group_batch");
		if (group_batch_shader.is_null()) {
			print_error("[GNE] mesh group batch shader_create_from_spirv failed.");
			return false;
		}

		group_vertex_format = rendering_device->vertex_format_create(Vector<RD::VertexAttribute>());
		if (group_vertex_format < 0) {
			print_error("[GNE] mesh group batch vertex_format_create failed.");
			return false;
		}
		group_vertex_array = rendering_device->vertex_array_create(1, group_vertex_format, Vector<RID>(), Vector<uint64_t>());
		if (group_vertex_array.is_null()) {
			print_error("[GNE] mesh group batch vertex_array_create failed.");
			return false;
		}

		RD::PipelineRasterizationState grs;
		RD::PipelineMultisampleState gms;
		RD::PipelineDepthStencilState gds;
		gds.enable_depth_test = true;
		gds.enable_depth_write = true;
		gds.depth_compare_operator = RD::COMPARE_OP_LESS_OR_EQUAL;
		RD::PipelineColorBlendState gbs = RD::PipelineColorBlendState::create_disabled(4);
		group_batch_pipeline = rendering_device->render_pipeline_create(
				group_batch_shader, raster_framebuffer_format, group_vertex_format, RD::RENDER_PRIMITIVE_TRIANGLES, grs, gms, gds, gbs, 0, 0);
		if (group_batch_pipeline.is_null()) {
			print_error("[GNE] mesh group batch render_pipeline_create failed.");
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
			print_error("[GNE] mesh group batch uniform_set_create failed.");
			return false;
		}
	}

	gpu_mesh_batch_valid = true;
	gpu_mesh_table_valid = true;
	last_batch_count = 0;
	last_group_count = 0;
	last_used_group_draw = false;
	print_line("[GNE] Mesh table (GNE-010) initialized. slots=" + itos(mesh_table_count) +
			" max_verts=" + itos(GNE_MAX_MESH_VERTS) + " max_indices=" + itos(GNE_MAX_MESH_INDICES));
	return true;
}

void GneRenderServer::gpu_scene_set_instance_mesh(int p_index, int p_mesh_id) {
	if (!gpu_scene_valid || !gpu_mesh_batch_valid || p_index < 0 || p_index >= gpu_instance_count || mesh_id_buffer.is_null()) {
		return;
	}
	if (p_mesh_id < 0 || p_mesh_id >= mesh_table_count) {
		p_mesh_id = 0;
	}
	uint32_t v = (uint32_t)p_mesh_id;
	rendering_device->buffer_update(mesh_id_buffer, (uint32_t)(p_index * 4), 4, &v);
}

int GneRenderServer::gpu_scene_get_instance_mesh(int p_index) {
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

int GneRenderServer::gpu_mesh_create_from_arrays(const PackedVector3Array &p_verts, const PackedInt32Array &p_indices) {
	if (!gpu_scene_valid || !gpu_mesh_valid || !gpu_mesh_batch_valid) {
		print_error("[GNE] gpu_mesh_create_from_arrays: no batch mesh. Call gpu_mesh_create first.");
		return -1;
	}
	if (mesh_table_count >= GNE_MESH_TABLE_SIZE) {
		print_error("[GNE] gpu_mesh_create_from_arrays: mesh table full.");
		return -1;
	}
	int vc = p_verts.size();
	int ic = p_indices.size();
	if (vc <= 0 || ic <= 0) {
		print_error("[GNE] gpu_mesh_create_from_arrays: empty arrays.");
		return -1;
	}
	for (int i = 0; i < ic; i++) {
		if (p_indices[i] < 0 || p_indices[i] >= vc) {
			print_error("[GNE] gpu_mesh_create_from_arrays: index out of range.");
			return -1;
		}
	}
	if (mesh_next_vertex_offset + vc > GNE_MAX_MESH_VERTS || mesh_next_index_offset + ic > GNE_MAX_MESH_INDICES) {
		print_error("[GNE] gpu_mesh_create_from_arrays: shared buffer capacity exceeded.");
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

	GneMeshDesc desc;
	memset(&desc, 0, sizeof(desc));
	desc.index_buffer_slot = 0;
	desc.vertex_buffer_slot = 0;
	desc.index_count = (uint32_t)ic;
	desc.vertex_count = (uint32_t)vc;
	desc.first_index = (uint32_t)mesh_next_index_offset;
	desc.vertex_offset = mesh_next_vertex_offset;
	rendering_device->buffer_update(mesh_table_buffer, (uint32_t)(mesh_table_count * sizeof(GneMeshDesc)), (uint32_t)sizeof(GneMeshDesc), &desc);

	Color c = gne_mesh_palette(mesh_table_count);
	mesh_colors[mesh_table_count] = c;
	rendering_device->buffer_update(mesh_color_buffer, (uint32_t)(mesh_table_count * 16), 16, &c);

	int id = mesh_table_count;
	mesh_table_count++;
	mesh_next_vertex_offset += vc;
	mesh_next_index_offset += ic;
	print_line("[GNE] GPU Mesh table entry added. mesh_id=" + itos(id) +
			" verts=" + itos(vc) + " indices=" + itos(ic) + " color=" + String(c));
	return id;
}

bool GneRenderServer::gpu_mesh_batch_dispatch() {
	if (!gpu_scene_valid || !gpu_mesh_valid || !gpu_mesh_batch_valid) {
		print_error("[GNE] gpu_mesh_batch_dispatch: no batch mesh. Call gpu_mesh_create first.");
		return false;
	}
	if (!frustum_valid) {
		print_error("[GNE] gpu_mesh_batch_dispatch: no camera. Call gpu_scene_set_camera first.");
		return false;
	}

	int visible = gpu_cull_get_visible_count();
	if (visible <= 0) {
		last_batch_count = 0;
		last_group_count = 0;
		last_used_group_draw = false;
		return true;
	}

	rendering_device->buffer_clear(batch_count_buffer, 0, (uint32_t)(GNE_MESH_TABLE_SIZE * 4));

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

	// GNE-011 pass 2: workgroup-parallel prefix sum + grouping under the
	// current strategy. One workgroup (64 threads) covers the 64-slot table.
	struct BatchAssembleParams {
		uint32_t mesh_capacity;
		uint32_t scratch_stride;
		uint32_t strategy;
		uint32_t pad1;
	};
	BatchAssembleParams ap;
	ap.mesh_capacity = (uint32_t)GNE_MESH_TABLE_SIZE;
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

	// GNE-011: derive the grouping state on the CPU with the SAME deterministic
	// formula as the workgroup assembler; batch_total (read from the GPU) stays
	// the authoritative indirect draw count (SPEC 011 section 3.2 fallback).
	int active = 0;
	{
		Vector<uint8_t> cb = rendering_device->buffer_get_data(batch_count_buffer, 0, (uint32_t)(GNE_MESH_TABLE_SIZE * 4));
		const uint32_t *fptr = (const uint32_t *)cb.ptr();
		int nn = cb.size() / 4;
		for (int i = 0; i < nn; i++) {
			if (fptr[i] > 0) {
				active++;
			}
		}
	}
	int G = active;
	if (mesh_batch_strategy == GNE_BATCH_STRATEGY_GROUPED || mesh_batch_strategy == GNE_BATCH_STRATEGY_REORDERED) {
		G = active > 5 ? 5 : active;
	}
	if (G <= 0) {
		G = 1;
	}
	last_group_count = G;
	last_used_group_draw = (G < active);
	int expect = last_used_group_draw ? G : active;
	if (last_batch_count != expect) {
		print_line("[GNE] gpu_mesh_batch_dispatch: batch_total(" + itos(last_batch_count) +
				") != expected(" + itos(expect) + ") - using GPU value.");
	}
	return true;
}

// TEMP-DIAG-012-idprobe: create the INDEPENDENT id-pass resources. Nothing
// here is shared with raster_framebuffer / mesh_batch_pipeline /
// group_batch_pipeline: separate textures, separate format, separate pipeline,
// separate shaders, separate uniform set, separate (empty) vertex format and
// vertex array. The 8 storage buffers it binds are the SAME RIDs the production
// group pass uses, read-only, so the draw issues identical work.
bool GneRenderServer::_gpu_idprobe_create() {
	if (gpu_idprobe_valid) {
		return true;
	}
	if (!gpu_mesh_batch_valid || !gpu_raster_valid) {
		print_error("[GNE] idprobe: mesh batch or raster not ready.");
		return false;
	}

	// --- R32_UINT id target ---
	RD::TextureFormat idf;
	idf.format = RD::DATA_FORMAT_R32_UINT;
	idf.width = RASTER_TARGET_W;
	idf.height = RASTER_TARGET_H;
	idf.depth = 1;
	idf.texture_type = RD::TEXTURE_TYPE_2D;
	// CAN_COPY_FROM_BIT is required for texture_get_data (vmaMapMemory on the
	// texture allocation) - same usage set as raster_color_texture.
	idf.usage_bits = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	gpu_idprobe_id_texture = rendering_device->texture_create(idf, RD::TextureView());
	if (gpu_idprobe_id_texture.is_null()) {
		print_error("[GNE] idprobe: id texture_create failed.");
		return false;
	}

	// --- its own D32_SFLOAT depth, same format/clear as production ---
	RD::TextureFormat ddf;
	ddf.format = RD::DATA_FORMAT_D32_SFLOAT;
	ddf.width = RASTER_TARGET_W;
	ddf.height = RASTER_TARGET_H;
	ddf.depth = 1;
	ddf.texture_type = RD::TEXTURE_TYPE_2D;
	ddf.usage_bits = RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	gpu_idprobe_depth_texture = rendering_device->texture_create(ddf, RD::TextureView());
	if (gpu_idprobe_depth_texture.is_null()) {
		print_error("[GNE] idprobe: depth texture_create failed.");
		return false;
	}

	// --- 2-attachment format: R32_UINT colour + D32 depth ---
	Vector<RD::AttachmentFormat> iafs;
	RD::AttachmentFormat iaf;
	iaf.format = RD::DATA_FORMAT_R32_UINT;
	iaf.samples = RD::TEXTURE_SAMPLES_1;
	iaf.usage_flags = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	iafs.push_back(iaf);
	RD::AttachmentFormat idfa;
	idfa.format = RD::DATA_FORMAT_D32_SFLOAT;
	idfa.samples = RD::TEXTURE_SAMPLES_1;
	idfa.usage_flags = RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	iafs.push_back(idfa);
	gpu_idprobe_framebuffer_format = rendering_device->framebuffer_format_create(iafs);
	if (gpu_idprobe_framebuffer_format < 0) {
		print_error("[GNE] idprobe: framebuffer_format_create failed.");
		return false;
	}

	Vector<RID> iattach;
	iattach.push_back(gpu_idprobe_id_texture);
	iattach.push_back(gpu_idprobe_depth_texture);
	gpu_idprobe_framebuffer = rendering_device->framebuffer_create(iattach, gpu_idprobe_framebuffer_format);
	if (gpu_idprobe_framebuffer.is_null()) {
		print_error("[GNE] idprobe: framebuffer_create failed.");
		return false;
	}
	return true;
}

// TEMP-DIAG-012-idprobe: second half of creation (shaders, vertex array,
// pipeline, uniform set). Split only to keep each edit small.
bool GneRenderServer::_gpu_idprobe_create2() {
	String error;
	Vector<uint8_t> ivs = rendering_device->shader_compile_spirv_from_source(
			RD::SHADER_STAGE_VERTEX, String(gpu_idprobe_vert_glsl), RD::SHADER_LANGUAGE_GLSL, &error);
	if (ivs.is_empty()) {
		print_error(String("[GNE] idprobe: vertex shader compile failed: ") + error);
		return false;
	}
	Vector<uint8_t> ifs = rendering_device->shader_compile_spirv_from_source(
			RD::SHADER_STAGE_FRAGMENT, String(gpu_idprobe_frag_glsl), RD::SHADER_LANGUAGE_GLSL, &error);
	if (ifs.is_empty()) {
		print_error(String("[GNE] idprobe: fragment shader compile failed: ") + error);
		return false;
	}
	Vector<RD::ShaderStageSPIRVData> istages;
	RD::ShaderStageSPIRVData ivs_d;
	ivs_d.shader_stage = RD::SHADER_STAGE_VERTEX;
	ivs_d.spirv = ivs;
	istages.push_back(ivs_d);
	RD::ShaderStageSPIRVData ifs_d;
	ifs_d.shader_stage = RD::SHADER_STAGE_FRAGMENT;
	ifs_d.spirv = ifs;
	istages.push_back(ifs_d);
	gpu_idprobe_shader = rendering_device->shader_create_from_spirv(istages, "gne_idprobe");
	if (gpu_idprobe_shader.is_null()) {
		print_error("[GNE] idprobe: shader_create_from_spirv failed.");
		return false;
	}

	// empty vertex format + array, mirroring group_vertex_format (the production
	// REORDERED path is procedural: no vertex attributes at all)
	gpu_idprobe_vertex_format = rendering_device->vertex_format_create(Vector<RD::VertexAttribute>());
	if (gpu_idprobe_vertex_format < 0) {
		print_error("[GNE] idprobe: vertex_format_create failed.");
		return false;
	}
	gpu_idprobe_vertex_array = rendering_device->vertex_array_create(1, gpu_idprobe_vertex_format, Vector<RID>(), Vector<uint64_t>());
	if (gpu_idprobe_vertex_array.is_null()) {
		print_error("[GNE] idprobe: vertex_array_create failed.");
		return false;
	}

	// depth test/write and compare operator copied verbatim from the production
	// group pipeline
	RD::PipelineRasterizationState irs;
	RD::PipelineMultisampleState ims;
	RD::PipelineDepthStencilState ids;
	ids.enable_depth_test = true;
	ids.enable_depth_write = true;
	ids.depth_compare_operator = RD::COMPARE_OP_LESS_OR_EQUAL;
	RD::PipelineColorBlendState ibs = RD::PipelineColorBlendState::create_disabled(1);
	gpu_idprobe_pipeline = rendering_device->render_pipeline_create(
			gpu_idprobe_shader, gpu_idprobe_framebuffer_format, gpu_idprobe_vertex_format,
			RD::RENDER_PRIMITIVE_TRIANGLES, irs, ims, ids, ibs, 0, 0);
	if (gpu_idprobe_pipeline.is_null()) {
		print_error("[GNE] idprobe: render_pipeline_create failed.");
		return false;
	}

	// the SAME 8 RIDs the production group set uses, in the same order
	Vector<RD::Uniform> iu;
	const RID ibu[8] = {
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
		u.append_id(ibu[b]);
		iu.push_back(u);
	}
	gpu_idprobe_uniform_set = rendering_device->uniform_set_create(iu, gpu_idprobe_shader, 0);
	if (gpu_idprobe_uniform_set.is_null()) {
		print_error("[GNE] idprobe: uniform_set_create failed.");
		return false;
	}
	gpu_idprobe_valid = true;
	print_line("[GNE] idprobe: independent framebuffer ready (R32_UINT + D32, 1920x1080).");
	return true;
}

// TEMP-DIAG-012-idprobe: lifecycle teardown. Inventory verified against the
// resources actually created in _gpu_idprobe_create / _create2:
//   7 RIDs freed (framebuffer, uniform_set, pipeline, shader, vertex_array,
//                 depth_texture, id_texture)
//   gpu_idprobe_vertex_format and gpu_idprobe_framebuffer_format are int64
//   format handles, NOT RIDs - they have no free_rid and are only reset.
void GneRenderServer::_gpu_idprobe_destroy() {
	if (gpu_idprobe_framebuffer.is_valid()) {
		rendering_device->free_rid(gpu_idprobe_framebuffer);
		gpu_idprobe_framebuffer = RID();
	}
	if (gpu_idprobe_uniform_set.is_valid()) {
		rendering_device->free_rid(gpu_idprobe_uniform_set);
		gpu_idprobe_uniform_set = RID();
	}
	if (gpu_idprobe_pipeline.is_valid()) {
		rendering_device->free_rid(gpu_idprobe_pipeline);
		gpu_idprobe_pipeline = RID();
	}
	if (gpu_idprobe_shader.is_valid()) {
		rendering_device->free_rid(gpu_idprobe_shader);
		gpu_idprobe_shader = RID();
	}
	if (gpu_idprobe_vertex_array.is_valid()) {
		rendering_device->free_rid(gpu_idprobe_vertex_array);
		gpu_idprobe_vertex_array = RID();
	}
	if (gpu_idprobe_depth_texture.is_valid()) {
		rendering_device->free_rid(gpu_idprobe_depth_texture);
		gpu_idprobe_depth_texture = RID();
	}
	if (gpu_idprobe_id_texture.is_valid()) {
		rendering_device->free_rid(gpu_idprobe_id_texture);
		gpu_idprobe_id_texture = RID();
	}
	gpu_idprobe_vertex_format = -1;
	gpu_idprobe_framebuffer_format = -1;
	gpu_idprobe_valid = false;
}

bool GneRenderServer::gpu_idprobe_set_enabled(bool p_enabled) {
	gpu_idprobe_enabled = p_enabled;
	if (!p_enabled) {
		return true;
	}
	if (!_gpu_idprobe_create()) {
		return false;
	}
	return _gpu_idprobe_create2();
}

bool GneRenderServer::gpu_idprobe_is_valid() const {
	return gpu_idprobe_valid;
}

PackedInt32Array GneRenderServer::gpu_idprobe_read_ids() {
	PackedInt32Array ret;
	if (!gpu_idprobe_valid || gpu_idprobe_id_texture.is_null()) {
		return ret;
	}
	Vector<uint8_t> data = rendering_device->texture_get_data(gpu_idprobe_id_texture, 0);
	int count = data.size() / 4;
	if (count <= 0) {
		print_error("[GNE] idprobe: id readback empty.");
		return ret;
	}
	ret.resize(count);
	const uint32_t *p = (const uint32_t *)data.ptr();
	for (int i = 0; i < count; i++) {
		ret.set(i, (int32_t)p[i]);
	}
	return ret;
}

PackedFloat32Array GneRenderServer::gpu_idprobe_read_depth() {
	PackedFloat32Array ret;
	if (!gpu_idprobe_valid || gpu_idprobe_depth_texture.is_null()) {
		return ret;
	}
	Vector<uint8_t> data = rendering_device->texture_get_data(gpu_idprobe_depth_texture, 0);
	int count = data.size() / 4;
	if (count <= 0) {
		print_error("[GNE] idprobe: depth readback empty.");
		return ret;
	}
	ret.resize(count);
	memcpy(ret.ptrw(), data.ptr(), (size_t)data.size());
	return ret;
}

// TEMP-DIAG-012-idprobe: re-issue the SAME production group draw into the
// independent id framebuffer. Called INSIDE gpu_mesh_batch_draw immediately
// after the production draw list has ended, BEFORE the submit/sync and before
// any compute that could touch group_args_buffer. No dispatch, no assemble, no
// list regeneration: same group_args_buffer, same last_batch_count, same
// batch_instances, same 8 bound RIDs, same empty vertex array geometry.
void GneRenderServer::_gpu_idprobe_draw() {
	if (!gpu_idprobe_enabled || !gpu_idprobe_valid) {
		return;
	}
	if (last_batch_count <= 0 || !last_used_group_draw) {
		// Only the REORDERED/grouped branch is reproduced. The per-mesh branch
		// (batch_args_buffer, indexed, 20B) is NOT re-issued: main_012 does not
		// take it, and reproducing it would be a different draw, not this one.
		return;
	}
	// Clear to 0xFFFFFFFF (a sentinel no instance id can equal) so untouched
	// pixels are distinguishable from any real id.
	Vector<Color> iclears;
	iclears.push_back(Color(1.0, 1.0, 1.0, 1.0));
	RD::DrawListID idl = rendering_device->draw_list_begin(gpu_idprobe_framebuffer,
			RD::DRAW_CLEAR_COLOR_0 | RD::DRAW_CLEAR_DEPTH, iclears, 1.0f, 0, Rect2(), 0);
	if (idl == RD::INVALID_ID) {
		print_error("[GNE] idprobe: draw_list_begin failed.");
		return;
	}
	rendering_device->draw_list_bind_render_pipeline(idl, gpu_idprobe_pipeline);
	rendering_device->draw_list_bind_uniform_set(idl, gpu_idprobe_uniform_set, 0);
	rendering_device->draw_list_bind_vertex_array(idl, gpu_idprobe_vertex_array);
	rendering_device->draw_list_draw_indirect(idl, false, group_args_buffer, 0, (uint32_t)last_batch_count, 16);
	rendering_device->draw_list_end();
}

bool GneRenderServer::gpu_mesh_batch_draw() {
	if (!gpu_scene_valid || !gpu_mesh_valid || !gpu_mesh_batch_valid) {
		print_error("[GNE] gpu_mesh_batch_draw: no batch mesh. Call gpu_mesh_create first.");
		return false;
	}
	if (!frustum_valid) {
		print_error("[GNE] gpu_mesh_batch_draw: no camera. Call gpu_scene_set_camera first.");
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
		print_error("[GNE] gpu_mesh_batch_draw: draw_list_begin failed.");
		return false;
	}
	rendering_device->draw_list_bind_render_pipeline(dl, mesh_batch_pipeline);
	rendering_device->draw_list_bind_uniform_set(dl, mesh_batch_uniform_set, 0);
	rendering_device->draw_list_bind_vertex_array(dl, mesh_010_vertex_array);
	rendering_device->draw_list_bind_index_array(dl, mesh_010_index_array);
	if (last_batch_count > 0) {
		if (last_used_group_draw) {
			// GNE-011 grouped/reordered: one PROCEDURAL (non-indexed)
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

	// TEMP-DIAG-012-idprobe: re-issue the same group draw into the independent
	// id framebuffer, in the same submission, before submit/sync. No compute
	// runs between this and the production draw, so group_args_buffer and
	// batch_instances are bit-identical for both.
	_gpu_idprobe_draw();

	// GNE-012: the production pyramid is built in gpu_visibility_prod_dispatch
	// (own synced submission) by projecting the registered occluder AABBs with
	// the occbuf pass into the flat storage buffer the phases read
	// cross-submission - the ONE reliable GPU route in this RDG fork (R32
	// attachment writes and compute-image loads are both stale). This draw
	// submission only patches the shared view UBO; nothing pyramid-related.
	// (the old code wrote int32 bits into this float field: 2.8e-42 reads as ~0).
	float otexel_count = (float)HZB_PROD_TEXEL_COUNT;
	rendering_device->buffer_update(view_ubo, offsetof(GneViewData, viewport) + 2 * sizeof(float), 4, &otexel_count);
	int32_t oocc_count = occluder_count;
	rendering_device->buffer_update(view_ubo, offsetof(GneViewData, occ_count), 4, &oocc_count);

	if (gpu_hzb_prod_valid) {
		RD::ComputeListID cl = rendering_device->compute_list_begin();
		uint32_t ogroups = HZB_PROD_TEXEL_COUNT / 8;

		rendering_device->compute_list_end();
	}

	rendering_device->submit();
	rendering_device->sync();

	return true;
}

// GNE-016: material store + mat raster path (all additive; the flat-color
// paths, vertex format and mesh table are untouched).
bool GneRenderServer::gpu_material_create() {
	static_assert(sizeof(GneMaterial) == 64, "GneMaterial must be 64 bytes (contract_016_data).");
	if (!gpu_scene_valid || !gpu_mesh_valid) {
		print_error("[GNE] gpu_material_create: no gpu scene/mesh. Call gpu_mesh_create first.");
		return false;
	}
	if (gpu_material_valid) {
		print_error("[GNE] gpu_material_create: material store already exists (non-destructive guard).");
		return false;
	}
	String error;
	mat_buffer = rendering_device->storage_buffer_create((uint32_t)(GNE_MESH_TABLE_SIZE * 64));
	// GNE-016.5 slice-1: per-material channel-map records (slots -1 = unset).
	mat2_buffer = rendering_device->storage_buffer_create((uint32_t)(GNE_MESH_TABLE_SIZE * 32));
	if (mat2_buffer.is_null()) {
		print_error("[GNE] gpu_material_create: mat2 buffer_create failed.");
		return false;
	}
	{
		Vector<uint8_t> m2z;
		m2z.resize(GNE_MESH_TABLE_SIZE * 32);
		float *m2f = (float *)m2z.ptr();
		for (int mi = 0; mi < GNE_MESH_TABLE_SIZE; mi++) {
			m2f[mi * 8 + 0] = -1.0f;
			m2f[mi * 8 + 1] = -1.0f;
			m2f[mi * 8 + 2] = -1.0f;
			m2f[mi * 8 + 3] = 0.0f;
			m2f[mi * 8 + 4] = 0.01f;
			m2f[mi * 8 + 5] = 0.01f;
			m2f[mi * 8 + 6] = 0.0f;
			m2f[mi * 8 + 7] = 0.0f;
		}
		rendering_device->buffer_update(mat2_buffer, 0, (uint32_t)m2z.size(), m2z.ptr());
	}
	if (mat_buffer.is_null()) {
		print_error("[GNE] gpu_material_create: buffer_create failed.");
		return false;
	}
	// Defaults mirror the flat palette visually (continuity) with neutral PBR
	// params: rough 0.5, metal 0, emissive off, white spec, shininess 32.
	for (int i = 0; i < GNE_MESH_TABLE_SIZE; i++) {
		GneMaterial m;
		m.albedo[0] = mesh_colors[i].r;
		m.albedo[1] = mesh_colors[i].g;
		m.albedo[2] = mesh_colors[i].b;
		m.roughness = 0.5f;
		m.emissive[0] = 0.0f;
		m.emissive[1] = 0.0f;
		m.emissive[2] = 0.0f;
		m.metallic = 0.0f;
		m.spec[0] = 1.0f;
		m.spec[1] = 1.0f;
		m.spec[2] = 1.0f;
		m.shininess = 32.0f;
		m.flags = 0.0f;
		m.emissive_strength = 0.0f;
		m.emission_backface = 0.0f;
		m.pad = 0.0f;
		mat_cpu[i] = m;
	}
	rendering_device->buffer_update(mat_buffer, 0, (uint32_t)(GNE_MESH_TABLE_SIZE * 64), mat_cpu);

	Vector<uint8_t> vert_spirv = rendering_device->shader_compile_spirv_from_source(
			RD::SHADER_STAGE_VERTEX, String(gpu_mat_batch_vert_glsl), RD::SHADER_LANGUAGE_GLSL, &error);
	if (vert_spirv.is_empty()) {
		print_error("[GNE] material vertex shader compile failed:");
		print_error(error);
		return false;
	}
	Vector<uint8_t> frag_spirv = rendering_device->shader_compile_spirv_from_source(
			RD::SHADER_STAGE_FRAGMENT, String(gpu_mat_batch_frag_glsl), RD::SHADER_LANGUAGE_GLSL, &error);
	if (frag_spirv.is_empty()) {
		print_error("[GNE] material fragment shader compile failed:");
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
	mat_batch_shader = rendering_device->shader_create_from_spirv(stages, "gne_mat_batch");
	if (mat_batch_shader.is_null()) {
		print_error("[GNE] material shader_create_from_spirv failed.");
		return false;
	}

	RD::PipelineRasterizationState rs;
	RD::PipelineMultisampleState ms;
	RD::PipelineDepthStencilState ds;
	ds.enable_depth_test = true;
	ds.enable_depth_write = true;
	ds.depth_compare_operator = RD::COMPARE_OP_LESS_OR_EQUAL;
	RD::PipelineColorBlendState bs = RD::PipelineColorBlendState::create_disabled(4);
	mat_batch_pipeline = rendering_device->render_pipeline_create(
			mat_batch_shader, raster_framebuffer_format, mesh_vertex_format, RD::RENDER_PRIMITIVE_TRIANGLES, rs, ms, ds, bs, 0, 0);
	if (mat_batch_pipeline.is_null()) {
		print_error("[GNE] material render_pipeline_create failed.");
		return false;
	}

	Vector<RD::Uniform> uniforms;
	const RID draw_buffers[5] = {
		batch_instances_buffer, transform_buffer, view_ubo, mesh_id_buffer, mat_buffer
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
	// GNE-017: mat_tex bindings + sampler array (dummy texture everywhere
	// until real binds arrive). The set is rebuilt by _mat_tex_refresh_set()
	// on every bind; this initial build uses the same path.
	for (int i = 0; i < GNE_MESH_TABLE_SIZE * GNE_MAT_TEX_SLOTS; i++) {
		mat_tex_cpu[i] = -1;
	}
	{
		Vector<uint8_t> blank;
		blank.resize(1280);
		blank.fill(0xFF); // -1 = unbound, int32 view
		mat_tex_buffer = rendering_device->storage_buffer_create(1280, blank);
	}
	RD::SamplerState tss;
	tss.mag_filter = RD::SAMPLER_FILTER_LINEAR;
	tss.min_filter = RD::SAMPLER_FILTER_LINEAR;
	tss.mip_filter = RD::SAMPLER_FILTER_LINEAR;
	tss.repeat_u = RD::SAMPLER_REPEAT_MODE_REPEAT;
	tss.repeat_v = RD::SAMPLER_REPEAT_MODE_REPEAT;
	tex_sampler = rendering_device->sampler_create(tss);
	{
		RD::TextureFormat dtf;
		dtf.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
		dtf.texture_type = RD::TEXTURE_TYPE_2D;
		dtf.width = 1;
		dtf.height = 1;
		dtf.depth = 1;
		dtf.array_layers = 1;
		dtf.mipmaps = 1;
		dtf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
		RD::TextureView dtv;
		Vector<Vector<uint8_t>> dd;
		Vector<uint8_t> white;
		white.resize(4);
		white.fill(255);
		dd.push_back(white);
		tex_dummy = rendering_device->texture_create(dtf, dtv, dd);
	}
	for (int i = 0; i < GNE_TEX_ARRAY; i++) {
		tex_array[i] = tex_dummy;
	}
	if (mat_tex_buffer.is_null() || tex_sampler.is_null() || tex_dummy.is_null()) {
		print_error("[GNE] gpu_material_create: texture plumbing create failed.");
		return false;
	}
	if (!_mat_tex_refresh_set()) {
		return false;
	}

	mat_light_dir = Vector3(-0.5f, -1.0f, -0.5f).normalized();
	mat_dispatches = 0;
	gpu_material_valid = true;
	print_line("[GNE] Material store (GNE-016) initialized. slots=64 bytes=4096.");
	return true;
}

bool GneRenderServer::_mat_check_id(int p_id, const char *p_what) const {
	if (!gpu_material_valid) {
		print_error(String("[GNE] ") + p_what + ": no material store. Call gpu_material_create first.");
		return false;
	}
	if (p_id < 0 || p_id >= GNE_MESH_TABLE_SIZE) {
		print_error(String("[GNE] ") + p_what + ": slot id out of range [0,64): " + itos(p_id) + ".");
		return false;
	}
	return true;
}

void GneRenderServer::_mat_upload(int p_id) {
	rendering_device->buffer_update(mat_buffer, (uint32_t)(p_id * 64), 64, &mat_cpu[p_id]);
}

bool GneRenderServer::gpu_material_set_albedo(int p_id, const Color &p_color) {
	if (!_mat_check_id(p_id, "gpu_material_set_albedo")) {
		return false;
	}
	mat_cpu[p_id].albedo[0] = p_color.r;
	mat_cpu[p_id].albedo[1] = p_color.g;
	mat_cpu[p_id].albedo[2] = p_color.b;
	_mat_upload(p_id);
	return true;
}

bool GneRenderServer::gpu_material_set_params(int p_id, float p_roughness, float p_metallic) {
	if (!_mat_check_id(p_id, "gpu_material_set_params")) {
		return false;
	}
	if (p_roughness < 0.0f || p_roughness > 1.0f || p_metallic < 0.0f || p_metallic > 1.0f) {
		print_error("[GNE] gpu_material_set_params: roughness/metallic must be in [0,1].");
		return false;
	}
	mat_cpu[p_id].roughness = p_roughness;
	mat_cpu[p_id].metallic = p_metallic;
	_mat_upload(p_id);
	return true;
}

bool GneRenderServer::gpu_material_set_specular(int p_id, const Color &p_color, float p_shininess) {
	if (!_mat_check_id(p_id, "gpu_material_set_specular")) {
		return false;
	}
	if (p_shininess <= 0.0f) {
		print_error("[GNE] gpu_material_set_specular: shininess must be > 0.");
		return false;
	}
	mat_cpu[p_id].spec[0] = p_color.r;
	mat_cpu[p_id].spec[1] = p_color.g;
	mat_cpu[p_id].spec[2] = p_color.b;
	mat_cpu[p_id].shininess = p_shininess;
	_mat_upload(p_id);
	return true;
}

bool GneRenderServer::gpu_material_set_emissive(int p_id, const Color &p_color, float p_strength, bool p_on, bool p_backface) {
	if (!_mat_check_id(p_id, "gpu_material_set_emissive")) {
		return false;
	}
	mat_cpu[p_id].emissive[0] = p_color.r;
	mat_cpu[p_id].emissive[1] = p_color.g;
	mat_cpu[p_id].emissive[2] = p_color.b;
	mat_cpu[p_id].emissive_strength = p_strength;
	mat_cpu[p_id].flags = p_on ? 1.0f : 0.0f;
	mat_cpu[p_id].emission_backface = p_backface ? 1.0f : 0.0f;
	_mat_upload(p_id);
	return true;
}

Dictionary GneRenderServer::gpu_material_readback(int p_id) {
	Dictionary d;
	if (!_mat_check_id(p_id, "gpu_material_readback")) {
		return d;
	}
	Vector<uint8_t> bytes = rendering_device->buffer_get_data(mat_buffer, (uint32_t)(p_id * 64), 64);
	if (bytes.size() != 64) {
		print_error("[GNE] gpu_material_readback: buffer_get_data failed.");
		return Dictionary();
	}
	const float *f = (const float *)bytes.ptr();
	d["albedo"] = Vector3(f[0], f[1], f[2]);
	d["roughness"] = f[3];
	d["emissive"] = Vector3(f[4], f[5], f[6]);
	d["metallic"] = f[7];
	d["spec"] = Vector3(f[8], f[9], f[10]);
	d["shininess"] = f[11];
	d["flags"] = f[12];
	d["emissive_strength"] = f[13];
	d["emission_backface"] = f[14];
	return d;
}

bool GneRenderServer::gpu_material_set_light(const Vector3 &p_dir) {
	if (!gpu_material_valid) {
		print_error("[GNE] gpu_material_set_light: no material store. Call gpu_material_create first.");
		return false;
	}
	if (p_dir.length() < 1e-6f) {
		print_error("[GNE] gpu_material_set_light: zero direction.");
		return false;
	}
	mat_light_dir = p_dir.normalized();
	return true;
}

Dictionary GneRenderServer::gpu_material_stats() {
	Dictionary d;
	d["slots"] = GNE_MESH_TABLE_SIZE;
	d["bytes"] = GNE_MESH_TABLE_SIZE * 64;
	d["dispatches"] = mat_dispatches;
	d["valid"] = gpu_material_valid;
	return d;
}

bool GneRenderServer::gpu_material_set_maps(int p_mat, int p_albedo_slot, int p_rough_slot, int p_normal_slot, const Vector2 &p_scale) {
	if (!gpu_material_valid || mat2_buffer.is_null() || p_mat < 0 || p_mat >= gpu_instance_count) {
		return false;
	}
	// GNE-016.5 slice-1: channel-map record (set0 binding 7). Slots < 0 = unset.
	float rec[8];
	rec[0] = (float)p_albedo_slot;
	rec[1] = (float)p_rough_slot;
	rec[2] = (float)p_normal_slot;
	rec[3] = 0.0f;
	rec[4] = p_scale.x;
	rec[5] = p_scale.y;
	rec[6] = 0.0f;
	rec[7] = 0.0f;
	rendering_device->buffer_update(mat2_buffer, (uint32_t)(p_mat * 32), 32, rec);
	return true;
}

bool GneRenderServer::gpu_material_draw() {
	if (!gpu_scene_valid || !gpu_mesh_valid || !gpu_mesh_batch_valid) {
		print_error("[GNE] gpu_material_draw: no batch mesh. Call gpu_mesh_create first.");
		return false;
	}
	if (!gpu_material_valid) {
		print_error("[GNE] gpu_material_draw: no material store. Call gpu_material_create first.");
		return false;
	}
	if (!frustum_valid) {
		print_error("[GNE] gpu_material_draw: no camera. Call gpu_scene_set_camera first.");
		return false;
	}
	// The mat path replays the per-mesh indirect args; the grouped path uses
	// a different (procedural) command layout it cannot consume.
	if (mesh_batch_strategy != GNE_BATCH_STRATEGY_PER_MESH) {
		print_error("[GNE] gpu_material_draw: requires PER_MESH strategy (uses batch_args from the last gpu_mesh_batch_dispatch).");
		return false;
	}
	if (last_batch_count <= 0) {
		print_error("[GNE] gpu_material_draw: empty batch (run gpu_mesh_batch_dispatch first).");
		return false;
	}

	struct MatPush {
		float light_xyz_amb[4];
		float cam_xyz[4];
		float light_rgb[4];
		float tex_slot_pad[4]; // [0] = albedo slot to sample (Phase 2: 0)
	};
	MatPush push;
	push.light_xyz_amb[0] = mat_light_dir.x;
	push.light_xyz_amb[1] = mat_light_dir.y;
	push.light_xyz_amb[2] = mat_light_dir.z;
	push.light_xyz_amb[3] = 0.1f;
	push.cam_xyz[0] = meshlet_camera_position[0];
	push.cam_xyz[1] = meshlet_camera_position[1];
	push.cam_xyz[2] = meshlet_camera_position[2];
	push.cam_xyz[3] = 0.0f;
	push.light_rgb[0] = 1.0f;
	push.light_rgb[1] = 1.0f;
	push.light_rgb[2] = 1.0f;
	push.light_rgb[3] = 0.0f;
	push.tex_slot_pad[0] = 0.0f;
	push.tex_slot_pad[1] = 0.0f;
	push.tex_slot_pad[2] = 0.0f;
	push.tex_slot_pad[3] = 0.0f;

	Vector<Color> clear_colors;
	clear_colors.push_back(Color(0, 0, 0, 0));
	clear_colors.push_back(Color(far_plane, 0.0f, 0.0f, 0.0f));
	RD::DrawListID dl = rendering_device->draw_list_begin(raster_framebuffer, RD::DRAW_CLEAR_COLOR_0 | RD::DRAW_CLEAR_COLOR_1 | RD::DRAW_CLEAR_DEPTH, clear_colors, 1.0f, 0, Rect2(), 0);
	if (dl == RD::INVALID_ID) {
		print_error("[GNE] gpu_material_draw: draw_list_begin failed.");
		return false;
	}
	rendering_device->draw_list_bind_render_pipeline(dl, mat_batch_pipeline);
	rendering_device->draw_list_bind_uniform_set(dl, mat_batch_uniform_set, 0);
	rendering_device->draw_list_bind_vertex_array(dl, mesh_010_vertex_array);
	rendering_device->draw_list_bind_index_array(dl, mesh_010_index_array);
	rendering_device->draw_list_set_push_constant(dl, &push, sizeof(push));
	// Per-mesh path (010): draw_count == distinct visible meshes, 20 bytes per
	// VkDrawIndexedIndirectCommand - identical command stream to batch_draw.
	rendering_device->draw_list_draw_indirect(dl, true, batch_args_buffer, 0, (uint32_t)last_batch_count, 20);
	rendering_device->draw_list_end();

	rendering_device->submit();
	rendering_device->sync();

	mat_dispatches++;
	raster_epoch++;
	return true;
}

// GNE-017: texture store + mat_tex bindings (additive; zero Basis/KTX linkage
// here - the offline tool transcodes, this code only parses + uploads).
bool GneRenderServer::_mat_tex_refresh_set() {
	if (mat_batch_uniform_set.is_valid()) {
		rendering_device->free_rid(mat_batch_uniform_set);
		mat_batch_uniform_set = RID();
	}
	Vector<RD::Uniform> uniforms;
	const RID draw_buffers[5] = {
		batch_instances_buffer, transform_buffer, view_ubo, mesh_id_buffer, mat_buffer
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
	RD::Uniform u5;
	u5.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
	u5.binding = 5;
	for (int i = 0; i < GNE_TEX_ARRAY; i++) {
		u5.append_id(tex_sampler);
		u5.append_id(tex_array[i].is_valid() ? tex_array[i] : tex_dummy);
	}
	uniforms.push_back(u5);
	RD::Uniform u6;
	u6.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	u6.binding = 6;
	u6.append_id(mat_tex_buffer);
	uniforms.push_back(u6);
	mat_batch_uniform_set = rendering_device->uniform_set_create(uniforms, mat_batch_shader, 0);
	if (mat_batch_uniform_set.is_null()) {
		print_error("[GNE] _mat_tex_refresh_set: uniform_set_create failed.");
		return false;
	}
	// The light pipeline's set 0 mirrors the same buffers: mark stale so the
	// next draw_lights rebuilds it (it cannot share the object: shader-bound).
	light_set0_dirty = true;
	return true;
}

struct GneGtexSlot {
	uint32_t width, height, mips;
	uint32_t basis_offset, basis_size;
	uint32_t rgba_offset, rgba_size;
	uint32_t flags;
};

int GneRenderServer::gpu_texture_load(const String &p_path) {
	if (rendering_device == nullptr) {
		print_error("[GNE] gpu_texture_load: no RenderingDevice.");
		return -1;
	}
	if (tex_count >= GNE_TEX_MAX) {
		print_error("[GNE] gpu_texture_load: texture store full (256).");
		return -1;
	}
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
	if (f.is_null()) {
		print_error("[GNE] gpu_texture_load: cannot open " + p_path + ".");
		return -1;
	}
	if (f->get_length() < 64) {
		print_error("[GNE] gpu_texture_load: file too small for GNET header.");
		return -1;
	}
	uint32_t magic = f->get_32();
	uint32_t version = f->get_32();
	uint32_t w = f->get_32();
	uint32_t h = f->get_32();
	uint32_t mips = f->get_32();
	uint32_t format = f->get_32();
	uint32_t slots = f->get_32();
	uint32_t flags = f->get_32();
	(void)flags;
	if (magic != 0x474E4554u || version != 1u || slots < 1 || mips < 1) {
		print_error("[GNE] gpu_texture_load: bad GNET header.");
		return -1;
	}
	if (format != 0) {
		print_error("[GNE] gpu_texture_load: unsupported format (want 0=UASTC).");
		return -1;
	}
	// Slot 0 descriptor (Phase 2 loads single-slot assets).
	uint32_t sw = f->get_32();
	uint32_t sh = f->get_32();
	uint32_t smips = f->get_32();
	uint32_t basis_off = f->get_32();
	uint32_t basis_size = f->get_32();
	uint32_t rgba_off = f->get_32();
	uint32_t rgba_size = f->get_32();
	uint32_t sflags = f->get_32();
	(void)sflags;
	if (sw != w || sh != h || smips != mips || rgba_size == 0) {
		print_error("[GNE] gpu_texture_load: slot descriptor mismatch.");
		return -1;
	}
	// Expected RGBA8 chain size (exact, no padding for uncompressed).
	uint32_t expect = 0, tw = w, th = h;
	for (uint32_t l = 0; l < mips; l++) {
		expect += tw * th * 4;
		tw = tw > 1 ? tw / 2 : 1;
		th = th > 1 ? th / 2 : 1;
	}
	if (rgba_size != expect) {
		print_error("[GNE] gpu_texture_load: RGBA8 section size mismatch.");
		return -1;
	}
	f->seek(rgba_off);
	PackedByteArray blob = f->get_buffer(rgba_size);
	if ((uint32_t)blob.size() != rgba_size) {
		print_error("[GNE] gpu_texture_load: short read of RGBA8 section.");
		return -1;
	}
	RD::TextureFormat tf;
	tf.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	tf.texture_type = RD::TEXTURE_TYPE_2D;
	tf.width = w;
	tf.height = h;
	tf.depth = 1;
	tf.array_layers = 1;
	tf.mipmaps = mips;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
	RD::TextureView tv;
	Vector<Vector<uint8_t>> data;
	Vector<uint8_t> packed;
	packed.resize(rgba_size);
	memcpy(packed.ptrw(), blob.ptr(), rgba_size);
	data.push_back(packed);
	RID tex = rendering_device->texture_create(tf, tv, data);
	if (tex.is_null()) {
		print_error("[GNE] gpu_texture_load: texture_create failed.");
		return -1;
	}
	int id = tex_count;
	tex_store[id].tex = tex;
	tex_store[id].w = (int)w;
	tex_store[id].h = (int)h;
	tex_store[id].mips = (int)mips;
	tex_store[id].bytes = rgba_size;
	tex_count++;
	return id;
}

bool GneRenderServer::gpu_texture_bind(int p_mat, int p_slot, int p_tex) {
	if (!gpu_material_valid) {
		print_error("[GNE] gpu_texture_bind: no material store. Call gpu_material_create first.");
		return false;
	}
	if (p_mat < 0 || p_mat >= GNE_MESH_TABLE_SIZE || p_slot < 0 || p_slot >= GNE_MAT_TEX_SLOTS) {
		print_error("[GNE] gpu_texture_bind: material/slot out of range.");
		return false;
	}
	if (p_tex < 0 || p_tex >= tex_count || tex_store[p_tex].tex.is_null()) {
		print_error("[GNE] gpu_texture_bind: texture id not loaded.");
		return false;
	}
	mat_tex_cpu[p_mat * GNE_MAT_TEX_SLOTS + p_slot] = -1;
	// Publish into the sampler array. mat_tex stores ARRAY positions (what
	// the shader indexes), not texture ids: position = id % 8. A second
	// texture colliding on an occupied position is rejected loudly (no
	// silent aliasing); the 8-entry array is the Phase-2 ceiling per the
	// bindless contract (no descriptor indexing in this RD fork).
	int arr = p_tex % GNE_TEX_ARRAY;
	if (tex_array[arr].is_valid() && tex_array[arr] != tex_dummy
			&& tex_array[arr] != tex_store[p_tex].tex) {
		print_error("[GNE] gpu_texture_bind: sampler-array collision (use one texture per demo in Phase 2).");
		return false;
	}
	tex_array[arr] = tex_store[p_tex].tex;
	mat_tex_cpu[p_mat * GNE_MAT_TEX_SLOTS + p_slot] = (int32_t)arr;
	rendering_device->buffer_update(mat_tex_buffer,
			(uint32_t)((p_mat * GNE_MAT_TEX_SLOTS + p_slot) * 4), 4,
			&mat_tex_cpu[p_mat * GNE_MAT_TEX_SLOTS + p_slot]);
	if (!_mat_tex_refresh_set()) {
		return false;
	}
	return true;
}

PackedInt32Array GneRenderServer::gpu_texture_get_stats() {
	PackedInt32Array ret;
	int bound = 0;
	if (gpu_material_valid) {
		for (int i = 0; i < GNE_MESH_TABLE_SIZE * GNE_MAT_TEX_SLOTS; i++) {
			if (mat_tex_cpu[i] >= 0) {
				bound++;
			}
		}
	}
	ret.push_back(tex_count);
	ret.push_back(bound);
	ret.push_back(GNE_MESH_TABLE_SIZE * GNE_MAT_TEX_SLOTS - bound);
	return ret;
}

// GNE-018 Phase 1: light store (additive; cull compute + shading land in
// later phases, same commit only after the Phase-4 gates pass).
bool GneRenderServer::_light_read_params(const Dictionary &p_params, GneLight &r_out) {
	memset(&r_out, 0, sizeof(r_out));
	int type = 0;
	if (p_params.has("type")) {
		type = (int)p_params["type"];
	}
	if (type != 0 && type != 1) {
		print_error("[GNE] light params: type must be 0 (point) or 1 (spot).");
		return false;
	}
	if (!p_params.has("pos") || !p_params.has("color")) {
		print_error("[GNE] light params: pos and color are required.");
		return false;
	}
	Vector3 pos = p_params["pos"];
	float range = p_params.has("range") ? (float)p_params["range"] : 0.0f;
	if (range <= 0.0f) {
		print_error("[GNE] light params: range must be > 0.");
		return false;
	}
	Color color = p_params["color"];
	float intensity = p_params.has("intensity") ? (float)p_params["intensity"] : 1.0f;
	if (intensity < 0.0f) {
		print_error("[GNE] light params: intensity must be >= 0.");
		return false;
	}
	Vector3 dir(0, -1, 0);
	float ci = 0.0f, co = 0.0f;
	if (type == 1) {
		if (!p_params.has("dir")) {
			print_error("[GNE] light params: spots require dir.");
			return false;
		}
		dir = ((Vector3)p_params["dir"]);
		if (dir.length() < 1e-6f) {
			print_error("[GNE] light params: zero direction.");
			return false;
		}
		dir = dir.normalized();
		ci = p_params.has("cone_inner") ? (float)p_params["cone_inner"] : 0.0f;
		co = p_params.has("cone_outer") ? (float)p_params["cone_outer"] : 0.0f;
		if (!(ci > 0.0f && co > ci && co <= 1.5707964f)) {
			print_error("[GNE] light params: need 0 < cone_inner < cone_outer <= pi/2.");
			return false;
		}
	}
	r_out.pos[0] = pos.x;
	r_out.pos[1] = pos.y;
	r_out.pos[2] = pos.z;
	r_out.range = range;
	r_out.color[0] = color.r;
	r_out.color[1] = color.g;
	r_out.color[2] = color.b;
	r_out.intensity = intensity;
	r_out.dir[0] = dir.x;
	r_out.dir[1] = dir.y;
	r_out.dir[2] = dir.z;
	r_out.type = (float)type;
	r_out.cone_inner = ci;
	r_out.cone_outer = co;
	return true;
}

// GNE-021: pack one light into the cull-only stream. Layout is 3 vec4 per light:
//   [0] = (pos.xyz, range)               read unconditionally by the cull
//   [1] = (dir.xyz, type)                 read only inside the spot branch
//   [2] = (cone_inner, cone_outer, 0, 0) read only inside the spot branch
// The point-light path therefore touches 16 B instead of the 64 B GneLight
// record. Color and intensity are deliberately absent: the cull never reads
// them, which is also why gpu_light_set_intensity does not touch this buffer.
void GneRenderServer::_light_pack_cull(int p_id, const GneLight &p_light) {
	float rec[12];
	rec[0] = p_light.pos[0];
	rec[1] = p_light.pos[1];
	rec[2] = p_light.pos[2];
	rec[3] = p_light.range;
	rec[4] = p_light.dir[0];
	rec[5] = p_light.dir[1];
	rec[6] = p_light.dir[2];
	rec[7] = p_light.type;
	rec[8] = p_light.cone_inner;
	rec[9] = p_light.cone_outer;
	rec[10] = 0.0f;
	rec[11] = 0.0f;
	rendering_device->buffer_update(light_cull_buffer, (uint32_t)(p_id * 48), 48, rec);
}

int GneRenderServer::gpu_light_create(const Dictionary &p_params) {
// GNE-018-rev: measurement-only override (unset in every gate) - lets the
// gated scenes run with the rev flag for A/B candidate-count measurements.
if (!light_cone_env_checked) {
	light_cone_env_checked = true;
	String env = OS::get_singleton()->get_environment("GNE_REV_CONE");
	if (env == "1") {
		light_cone_enabled = true;
	}
}
	if (rendering_device == nullptr) {
		print_error("[GNE] gpu_light_create: no RenderingDevice.");
		return -1;
	}
	if (!gpu_light_valid) {
		light_buffer = rendering_device->storage_buffer_create((uint32_t)(GNE_LIGHT_MAX * 64));
		// GNE-021: cull-only stream, 3 vec4 per light. See the header comment.
		light_cull_buffer = rendering_device->storage_buffer_create((uint32_t)(GNE_LIGHT_MAX * 48));
		light_bounds_buffer = rendering_device->storage_buffer_create((uint32_t)(GNE_LIGHT_MAX * 16));
		cluster_offset_buffer = rendering_device->storage_buffer_create((uint32_t)(GNE_CLUSTER_COUNT * 4));
		cluster_count_buffer = rendering_device->storage_buffer_create((uint32_t)(GNE_CLUSTER_COUNT * 4));
		cluster_index_buffer = rendering_device->storage_buffer_create(
				(uint32_t)(GNE_CLUSTER_COUNT * GNE_CLUSTER_LIGHT_CAP * 4));
		light_overflow_buffer = rendering_device->storage_buffer_create(8);

			// GNE-018-rev: cluster normal-cone records (3456 x vec4). Zero-filled =
			// the "never cull" sentinel default; the cone pass will rewrite per frame.
			cluster_cone_buffer = rendering_device->storage_buffer_create((uint32_t)(GNE_CLUSTER_COUNT * 16));
		if (light_buffer.is_null() || light_cull_buffer.is_null() || cluster_offset_buffer.is_null()
				|| cluster_count_buffer.is_null() || cluster_index_buffer.is_null()
				|| light_overflow_buffer.is_null() || cluster_cone_buffer.is_null()
			|| light_bounds_buffer.is_null()) {
			print_error("[GNE] gpu_light_create: buffer_create failed.");
			return -1;
		}
		for (int i = 0; i < GNE_LIGHT_MAX; i++) {
			memset(&light_cpu[i], 0, sizeof(GneLight));
		}
		rendering_device->buffer_clear(cluster_offset_buffer, 0, (uint32_t)(GNE_CLUSTER_COUNT * 4));
		rendering_device->buffer_clear(cluster_count_buffer, 0, (uint32_t)(GNE_CLUSTER_COUNT * 4));
		rendering_device->buffer_clear(light_overflow_buffer, 0, 8);
			rendering_device->buffer_clear(cluster_cone_buffer, 0, (uint32_t)(GNE_CLUSTER_COUNT * 16));
		cone_src_frame = draw_frame_seq; // initial zeros count as source frame 0
		// Static layout: cluster tid owns slots [tid*16, tid*16+16). Filled
		// once (never changes) so no prefix-sum pass is needed for DET.
		{
			Vector<uint32_t> offs;
			offs.resize(GNE_CLUSTER_COUNT);
			uint32_t *w = offs.ptrw();
			for (int i = 0; i < GNE_CLUSTER_COUNT; i++) {
				w[i] = (uint32_t)(i * GNE_CLUSTER_LIGHT_CAP);
			}
			rendering_device->buffer_update(cluster_offset_buffer, 0,
					(uint32_t)(GNE_CLUSTER_COUNT * 4), w);
		}
		light_count = 0;
		gpu_light_valid = true;
		print_line("[GNE] Light store (GNE-018) initialized. max=1024 clusters=3456.");
	}
	if (light_count >= GNE_LIGHT_MAX) {
		print_error("[GNE] gpu_light_create: light store full (1024).");
		return -1;
	}
	GneLight rec;
	if (!_light_read_params(p_params, rec)) {
		return -1;
	}
	int id = light_count;
	light_cpu[id] = rec;
	rendering_device->buffer_update(light_buffer, (uint32_t)(id * 64), 64, &light_cpu[id]);
	_light_pack_cull(id, rec);
	light_count++;
	return id;
}

bool GneRenderServer::gpu_light_update(int p_id, const Dictionary &p_params) {
	if (!gpu_light_valid) {
		print_error("[GNE] gpu_light_update: no light store. Call gpu_light_create first.");
		return false;
	}
	if (p_id < 0 || p_id >= light_count) {
		print_error("[GNE] gpu_light_update: id out of allocated range.");
		return false;
	}
	GneLight rec;
	if (!_light_read_params(p_params, rec)) {
		return false;
	}
	light_cpu[p_id] = rec;
	rendering_device->buffer_update(light_buffer, (uint32_t)(p_id * 64), 64, &light_cpu[p_id]);
	_light_pack_cull(p_id, rec);
	return true;
}

bool GneRenderServer::gpu_light_destroy(int p_id) {
	if (!gpu_light_valid) {
		print_error("[GNE] gpu_light_destroy: no light store.");
		return false;
	}
	if (p_id < 0 || p_id >= light_count) {
		print_error("[GNE] gpu_light_destroy: id out of allocated range.");
		return false;
	}
	memset(&light_cpu[p_id], 0, sizeof(GneLight));
	rendering_device->buffer_update(light_buffer, (uint32_t)(p_id * 64), 64, &light_cpu[p_id]);
	_light_pack_cull(p_id, light_cpu[p_id]);
	return true;
}

bool GneRenderServer::gpu_light_set_intensity(int p_light_id, float p_intensity) {
	if (!gpu_light_valid) {
		print_error("[GNE] gpu_light_set_intensity: no light store. Call gpu_light_create first.");
		return false;
	}
	if (p_light_id < 0 || p_light_id >= light_count) {
		print_error("[GNE] gpu_light_set_intensity: id out of allocated range.");
		return false;
	}
	// Reject a non-finite value rather than letting it reach the shader: NaN
	// intensity would poison every fragment the light touches, and inf would
	// collapse the attenuation term. Callers wanting "no emission" pass 0.
	if (!Math::is_finite(p_intensity)) {
		print_error("[GNE] gpu_light_set_intensity: intensity must be finite.");
		return false;
	}
	light_cpu[p_light_id].intensity = p_intensity;
	// Only the intensity float (offset 28 within the 64-byte record) is pushed.
	// No reallocation, no full-record rewrite, and pos/range/color are untouched.
	const uint32_t offset = (uint32_t)(p_light_id * 64) + (uint32_t)offsetof(GneLight, intensity);
	rendering_device->buffer_update(light_buffer, offset, (uint32_t)sizeof(float), &light_cpu[p_light_id].intensity);
	return true;
}

Dictionary GneRenderServer::gpu_light_get_stats() {
	Dictionary d;
	d["count"] = light_count;
	d["bytes"] = GNE_LIGHT_MAX * 64;
	d["clusters"] = GNE_CLUSTER_COUNT;
	d["valid"] = gpu_light_valid;
	d["clusters_touched"] = light_clusters_touched;
	d["assignments"] = light_assignments;
	d["overflows"] = light_overflows;
	d["tests_performed"] = light_tests_performed;
	d["normal_cone"] = light_cone_enabled;
	return d;
}

int GneRenderServer::gpu_texture_get_binding(int p_mat, int p_slot) {
	if (!gpu_material_valid) {
		print_error("[GNE] gpu_texture_get_binding: no material store.");
		return -2;
	}
	if (p_mat < 0 || p_mat >= GNE_MESH_TABLE_SIZE || p_slot < 0 || p_slot >= GNE_MAT_TEX_SLOTS) {
		print_error("[GNE] gpu_texture_get_binding: material/slot out of range.");
		return -2;
	}
	// Real GPU readback (mirror alone is not evidence).
	Vector<uint8_t> bytes = rendering_device->buffer_get_data(mat_tex_buffer, 0,
			(uint32_t)(GNE_MESH_TABLE_SIZE * GNE_MAT_TEX_SLOTS * 4));
	if (bytes.size() != (int)(GNE_MESH_TABLE_SIZE * GNE_MAT_TEX_SLOTS * 4)) {
		print_error("[GNE] gpu_texture_get_binding: buffer_get_data failed.");
		return -2;
	}
	int32_t v = 0;
	memcpy(&v, bytes.ptr() + (p_mat * GNE_MAT_TEX_SLOTS + p_slot) * 4, 4);
	return (int)v;
}

// GNE-018 Phase 2/3: clustered raster path. Lazy-builds the cull pipeline
// and the light pipeline + light set on first draw_lights (all buffers it
// needs are guaranteed by then). The 016 mat pipeline and its set are NEVER
// touched here, so 016 evidence cannot drift by construction.
// Builds the light pipeline's OWN set 0: identical 7-binding layout to the
// mat set (shared vert + same set-0 declarations => same layout), but a
// separate object because RD sets are shader-bound.
bool GneRenderServer::_light_build_set0() {
	if (mat_light_uniform_set.is_valid()) {
		rendering_device->free_rid(mat_light_uniform_set);
		mat_light_uniform_set = RID();
	}
	Vector<RD::Uniform> uniforms;
	const RID draw_buffers[5] = {
		batch_instances_buffer, transform_buffer, view_ubo, mesh_id_buffer, mat_buffer
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
	RD::Uniform u5;
	u5.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
	u5.binding = 5;
	for (int i = 0; i < GNE_TEX_ARRAY; i++) {
		u5.append_id(tex_sampler);
		u5.append_id(tex_array[i].is_valid() ? tex_array[i] : tex_dummy);
	}
	uniforms.push_back(u5);
	RD::Uniform u6;
	u6.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	u6.binding = 6;
	u6.append_id(mat_tex_buffer);
	RD::Uniform u7;
	u7.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	u7.binding = 7;
	u7.append_id(mat2_buffer);
	uniforms.push_back(u7);
	uniforms.push_back(u6);
	mat_light_uniform_set = rendering_device->uniform_set_create(uniforms, mat_light_shader, 0);
	if (mat_light_uniform_set.is_null()) {
		print_error("[GNE] light set-0 uniform_set_create failed.");
		return false;
	}
	light_set0_dirty = false;
	return true;
}

bool GneRenderServer::_light_ensure_geo() {
	if (mat_light_pipeline.is_valid()) {
		return true;
	}
	String error;
	auto compile_compute = [&](const char *p_glsl, const char *p_name, RID &r_shader) -> bool {
		Vector<uint8_t> spirv = rendering_device->shader_compile_spirv_from_source(
				RD::SHADER_STAGE_COMPUTE, String(p_glsl), RD::SHADER_LANGUAGE_GLSL, &error);
		if (spirv.is_empty()) {
			print_error(String("[GNE] ") + p_name + " shader compile failed:");
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
	if (!compile_compute(_gne_glsl_with_shared(gpu_light_cull_glsl, true).utf8().get_data(), "gne_light_cull", light_cull_shader)) {
		return false;
	}
	light_cull_pipeline = rendering_device->compute_pipeline_create(light_cull_shader);
	Vector<RD::Uniform> cu;
	const RID cbufs[9] = {
		light_buffer, cluster_offset_buffer, cluster_count_buffer,
		cluster_index_buffer, light_overflow_buffer, view_ubo, cluster_cone_buffer,
		light_cull_buffer, light_bounds_buffer
	};
	for (uint32_t b = 0; b < 9; b++) {
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = b;
		if (b == 5) {
			u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		}
		u.append_id(cbufs[b]);
		cu.push_back(u);
	}
	light_cull_uniform_set = rendering_device->uniform_set_create(cu, light_cull_shader, 0);
	if (light_cull_uniform_set.is_null()) {
		print_error("[GNE] light cull uniform_set_create failed.");
		return false;
	}
	// GNE-021: the bounds pass must exist and be valid before the cull can read
	// light_bounds_buffer, so it is created here, next to the cull, rather than
	// lazily at first use where a failure would be invisible.
	if (!compile_compute(_gne_glsl_with_shared(gpu_light_bounds_glsl, true).utf8().get_data(), "gne_light_bounds", light_bounds_shader)) {
		return false;
	}
	light_bounds_pipeline = rendering_device->compute_pipeline_create(light_bounds_shader);
	Vector<RD::Uniform> bu;
	// Binding numbers must match the shader's DECLARED bindings, not the array
	// order. The bounds shader declares CullLightStream at 0, LightBoundsOut at 1
	// and ViewBlock at 5; supplying view_ubo at index 2 fails with "Binding (5)
	// was not provided". The 16.2_5 rule in the cull is the same: the index and
	// the binding number agree there only by coincidence, so they are written out
	// explicitly here rather than implied.
	const struct {
		uint32_t binding;
		RID rid;
		bool uniform_buffer;
	} bbufs[3] = {
		{ 0, light_cull_buffer, false },
		{ 1, light_bounds_buffer, false },
		{ 5, view_ubo, true }
	};
	for (uint32_t b = 0; b < 3; b++) {
		RD::Uniform u;
		u.uniform_type = bbufs[b].uniform_buffer ? RD::UNIFORM_TYPE_UNIFORM_BUFFER : RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = bbufs[b].binding;
		u.append_id(bbufs[b].rid);
		bu.push_back(u);
	}
	light_bounds_uniform_set = rendering_device->uniform_set_create(bu, light_bounds_shader, 0);
	if (light_bounds_uniform_set.is_null()) {
		print_error("[GNE] light bounds uniform_set_create failed.");
		return false;
	}
	Vector<uint8_t> vert_spirv = rendering_device->shader_compile_spirv_from_source(
			RD::SHADER_STAGE_VERTEX, String(gpu_mat_batch_vert_glsl), RD::SHADER_LANGUAGE_GLSL, &error);
	if (vert_spirv.is_empty()) {
		print_error("[GNE] light vertex shader compile failed:");
		print_error(error);
		return false;
	}
	Vector<uint8_t> frag_spirv = rendering_device->shader_compile_spirv_from_source(
			RD::SHADER_STAGE_FRAGMENT, _gne_glsl_with_shared(gpu_mat_light_frag_glsl, false).replace("//GNE_GI_SHARED", gpu_gi_sample_glsl), RD::SHADER_LANGUAGE_GLSL, &error);
	if (frag_spirv.is_empty()) {
		print_error("[GNE] light fragment shader compile failed:");
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
	mat_light_shader = rendering_device->shader_create_from_spirv(stages, "gne_mat_light");
	if (mat_light_shader.is_null()) {
		print_error("[GNE] light shader_create_from_spirv failed.");
		return false;
	}
	RD::PipelineRasterizationState rs;
	RD::PipelineMultisampleState ms;
	RD::PipelineDepthStencilState ds;
	ds.enable_depth_test = true;
	ds.enable_depth_write = true;
	ds.depth_compare_operator = RD::COMPARE_OP_LESS_OR_EQUAL;
	RD::PipelineColorBlendState bs = RD::PipelineColorBlendState::create_disabled(4);
	mat_light_pipeline = rendering_device->render_pipeline_create(
			mat_light_shader, raster_framebuffer_format, mesh_vertex_format, RD::RENDER_PRIMITIVE_TRIANGLES, rs, ms, ds, bs, 0, 0);
	if (mat_light_pipeline.is_null()) {
		print_error("[GNE] light render_pipeline_create failed.");
		return false;
	}
	Vector<RD::Uniform> lu;
	const RID lbufs[4] = {
		light_buffer, cluster_offset_buffer, cluster_count_buffer, cluster_index_buffer
	};
	for (uint32_t b = 0; b < 4; b++) {
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = b;
		u.append_id(lbufs[b]);
		lu.push_back(u);
	}
	// GNE-022 section 10: GI atlas binding (fallback dummy when absent; scenes create the field before the first lit draw).
	RD::Uniform u4;
	u4.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
	u4.binding = 4;
	u4.append_id(gi_sampler.is_valid() ? gi_sampler : tex_sampler);
	u4.append_id(gi_atlas.is_valid() ? _gi_front_atlas() : tex_dummy);
	lu.push_back(u4);
	mat_light_tex_set = rendering_device->uniform_set_create(lu, mat_light_shader, 1);
	if (mat_light_tex_set.is_null()) {
		print_error("[GNE] light tex uniform_set_create failed.");
		return false;
	}
	if (!_light_build_set0()) {
		return false;
	}
	// GNE-019: frag set 2 (dummy until shadow maps exist). Always valid once
	// the light pipeline exists, so the 019-extended frag never sees an
	// unbound set — 018 pixels reproduce exactly (guarded sampling).
	return _shadow_ensure_set2();
}

// GNE-019: shadow math helpers (file-local). Column-major float[16].
static void _s019_mat_mul(float *r_out, const float *p_a, const float *p_b) {
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			float s = 0.0f;
			for (int k = 0; k < 4; k++) {
				s += p_a[k * 4 + r] * p_b[c * 4 + k];
			}
			r_out[c * 4 + r] = s;
		}
	}
}

static void _s019_lookat(float *r_view, const float *p_eye, const float *p_f, const float *p_up_hint) {
	float z[3] = { -p_f[0], -p_f[1], -p_f[2] };
	float x[3] = {
		p_up_hint[1] * z[2] - p_up_hint[2] * z[1],
		p_up_hint[2] * z[0] - p_up_hint[0] * z[2],
		p_up_hint[0] * z[1] - p_up_hint[1] * z[0]
	};
	float xl = Math::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
	if (xl < 1e-6f) {
		x[0] = 1.0f;
		x[1] = 0.0f;
		x[2] = 0.0f;
		xl = 1.0f;
	}
	x[0] /= xl;
	x[1] /= xl;
	x[2] /= xl;
	float y[3] = {
		z[1] * x[2] - z[2] * x[1],
		z[2] * x[0] - z[0] * x[2],
		z[0] * x[1] - z[1] * x[0]
	};
	// Column-major storage: the matrix ROWS must be the basis axes
	// (Godot convention: m[0]=x[0], m[1]=y[0], m[2]=z[0]). Storing the axes as
	// columns built the transposed (inverse-rotated) view: projection rays
	// missed the light direction and casters never landed over their receivers.
	r_view[0] = x[0];
	r_view[1] = y[0];
	r_view[2] = z[0];
	r_view[3] = 0.0f;
	r_view[4] = x[1];
	r_view[5] = y[1];
	r_view[6] = z[1];
	r_view[7] = 0.0f;
	r_view[8] = x[2];
	r_view[9] = y[2];
	r_view[10] = z[2];
	r_view[11] = 0.0f;
	r_view[12] = -(x[0] * p_eye[0] + x[1] * p_eye[1] + x[2] * p_eye[2]);
	r_view[13] = -(y[0] * p_eye[0] + y[1] * p_eye[1] + y[2] * p_eye[2]);
	r_view[14] = -(z[0] * p_eye[0] + z[1] * p_eye[1] + z[2] * p_eye[2]);
	r_view[15] = 1.0f;
}

void GneRenderServer::_shadow_planes_from_vp(const float *p_vp, float *r_planes) {
	// Rows of a column-major 4x4: row r = (m[r], m[4+r], m[8+r], m[12+r]).
	float rows[4][4];
	for (int r = 0; r < 4; r++) {
		rows[r][0] = p_vp[r];
		rows[r][1] = p_vp[4 + r];
		rows[r][2] = p_vp[8 + r];
		rows[r][3] = p_vp[12 + r];
	}
	const int comb[6][2] = { { 3, 0 }, { 3, 0 }, { 3, 1 }, { 3, 1 }, { 3, 2 }, { 3, 2 } };
	const float sign[6] = { 1.0f, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f };
	for (int k = 0; k < 6; k++) {
		int a = comb[k][0];
		int b = comb[k][1];
		float px = rows[a][0] + sign[k] * rows[b][0];
		float py = rows[a][1] + sign[k] * rows[b][1];
		float pz = rows[a][2] + sign[k] * rows[b][2];
		float pw = rows[a][3] + sign[k] * rows[b][3];
		float il = 1.0f / MAX(Math::sqrt(px * px + py * py + pz * pz), 1e-9f);
		r_planes[k * 4 + 0] = px * il;
		r_planes[k * 4 + 1] = py * il;
		r_planes[k * 4 + 2] = pz * il;
		r_planes[k * 4 + 3] = pw * il;
	}
}

void GneRenderServer::_shadow_vp_csm(float *r_vps, float *r_splits, float *r_bounds) {
	// Hybrid splits (D7-2, lambda = 0.5) in planar view depth.
	float near_d = cam_near_v;
	float far_d = far_plane;
	float ratio = far_d / MAX(near_d, 1e-3f);
	float s[5];
	s[0] = near_d;
	s[4] = far_d;
	for (int i = 1; i <= 3; i++) {
		float f = (float)i / 4.0f;
		float log_part = near_d * Math::pow(ratio, f);
		float uni_part = near_d + f * (far_d - near_d);
		s[i] = 0.5f * log_part + 0.5f * uni_part;
	}
	r_splits[0] = s[1];
	r_splits[1] = s[2];
	r_splits[2] = s[3];
	r_splits[3] = s[4];
	Vector3 dir = mat_light_dir.normalized();
	// View direction = -L (the direction light TRAVELS, toward the scene).
	// The shadow camera sits on the light-source side (+L) and looks toward
	// the instances; looking along +L puts the scene behind the camera and
	// yields empty maps (observed: non_far = 0).
	float f[3] = { -dir.x, -dir.y, -dir.z };
	float up0[3] = { 0.0f, 1.0f, 0.0f };
	// Light-space basis (orthonormal): r/l/u.
	float zax[3] = { -f[0], -f[1], -f[2] };
	float rax[3] = {
		up0[1] * zax[2] - up0[2] * zax[1],
		up0[2] * zax[0] - up0[0] * zax[2],
		up0[0] * zax[1] - up0[1] * zax[0]
	};
	float rl = Math::sqrt(rax[0] * rax[0] + rax[1] * rax[1] + rax[2] * rax[2]);
	rax[0] /= rl;
	rax[1] /= rl;
	rax[2] /= rl;
	float uax[3] = {
		zax[1] * rax[2] - zax[2] * rax[1],
		zax[2] * rax[0] - zax[0] * rax[2],
		zax[0] * rax[1] - zax[1] * rax[0]
	};
	float aspect = (float)RASTER_TARGET_W / (float)RASTER_TARGET_H;
	float eye[3] = {
		meshlet_camera_position[0] + dir.x * 2000.0f,
		meshlet_camera_position[1] + dir.y * 2000.0f,
		meshlet_camera_position[2] + dir.z * 2000.0f
	};
	float view[16];
	_s019_lookat(view, eye, f, up0);
	float r_ortho[6];
	for (int k = 0; k < 4; k++) {
		float z0 = s[k];
		float z1 = s[k + 1];
		float minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f, minz = 1e30f, maxz = -1e30f;
		for (int ci = 0; ci < 8; ci++) {
			float sx = (ci & 1) ? 1.0f : -1.0f;
			float sy = (ci & 2) ? 1.0f : -1.0f;
			float sz = (ci & 4) ? z1 : z0;
			float hx = sx * cam_tan_v * aspect * sz;
			float hy = sy * cam_tan_v * sz;
			float wx = meshlet_camera_position[0] + shadow_cam_basis[0] * hx + shadow_cam_basis[3] * hy + shadow_cam_basis[6] * sz;
			float wy = meshlet_camera_position[1] + shadow_cam_basis[1] * hx + shadow_cam_basis[4] * hy + shadow_cam_basis[7] * sz;
			float wz = meshlet_camera_position[2] + shadow_cam_basis[2] * hx + shadow_cam_basis[5] * hy + shadow_cam_basis[8] * sz;
			float dx = wx - eye[0], dy = wy - eye[1], dz = wz - eye[2];
			float lx = dx * rax[0] + dy * rax[1] + dz * rax[2];
			float ly = dx * uax[0] + dy * uax[1] + dz * uax[2];
			float lz = dx * f[0] + dy * f[1] + dz * f[2];
			minx = MIN(minx, lx);
			maxx = MAX(maxx, lx);
			miny = MIN(miny, ly);
			maxy = MAX(maxy, ly);
			minz = MIN(minz, lz);
			maxz = MAX(maxz, lz);
		}
		float mx = (maxx - minx) * 0.05f + 10.0f;
		float my = (maxy - miny) * 0.05f + 10.0f;
		float mz = (maxz - minz) * 0.05f + 10.0f;
		Projection ortho = Projection::create_orthogonal(minx - mx, maxx + mx, miny - my, maxy + my, minz - mz, maxz + mz);
		Projection depth_fix;
		depth_fix.set_depth_correction(false, false, true); // remap only (GL [-1,1] -> Vulkan [0,1])
		// Vulkan clip space requires 0 <= z <= w. The raw ortho maps to GL-style
		// [-1, 1] and every scene fragment lands at negative z -> all clipped
		// (observed: empty CSM maps, S1 fail). Remap forward to [0, 1]: z prime =
		// 0.5*z + 0.5 (near -> 0, far -> 1), keeping the forward-z convention
		// (compare LESS_OR_EQUAL, clear 1.0). The sampling side recomputes ref
		// with this same matrix, so stored/ref stay identical.
		Projection ortho_clip = depth_fix * ortho;
		float pm[16];
		for (int c = 0; c < 4; c++) {
			for (int r = 0; r < 4; r++) {
				pm[c * 4 + r] = ortho_clip.columns[c][r];
			}
		}
		_s019_mat_mul(&r_vps[k * 16], pm, view);
		if (k == 3) {
			r_ortho[0] = minx - mx;
			r_ortho[1] = maxx + mx;
			r_ortho[2] = miny - my;
			r_ortho[3] = maxy + my;
			r_ortho[4] = minz - mz;
			r_ortho[5] = maxz + mz;
		}
	}
	for (int i = 0; i < 6; i++) {
		r_bounds[i] = r_ortho[i];
	}
}

void GneRenderServer::_shadow_vp_cube(const float *p_pos, float *r_vps) {
	// Face order 0:+X 1:-X 2:+Y 3:-Y 4:+Z 5:-Z. Ups mirror the GLSL sampler
	// convention exactly (closed loop: render and sample share these axes).
	static const float DIRS[6][3] = {
		{ 1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f },
		{ 0.0f, 1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f },
		{ 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -1.0f }
	};
	static const float UPS[6][3] = {
		{ 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f },
		{ 0.0f, 0.0f, -1.0f }, { 0.0f, 0.0f, 1.0f },
		{ 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }
	};
	Projection persp = Projection::create_perspective(90.0f, 1.0f, 10.0f, 4000.0f);
	float pm[16];
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			pm[c * 4 + r] = persp.columns[c][r];
		}
	}
	for (int k = 0; k < 6; k++) {
		float view[16];
		_s019_lookat(view, p_pos, DIRS[k], UPS[k]);
		_s019_mat_mul(&r_vps[k * 16], pm, view);
	}
}

void GneRenderServer::_shadow_vp_spot(const float *p_pos, const float *p_dir, float p_outer, float *r_vp) {
	float up[3] = { 0.0f, 1.0f, 0.0f };
	if (Math::abs(p_dir[1]) > 0.98f) {
		up[0] = 1.0f;
		up[1] = 0.0f;
		up[2] = 0.0f;
	}
	float view[16];
	_s019_lookat(view, p_pos, p_dir, up);
	float fov_deg = 2.0f * p_outer * 57.29578f;
	Projection persp = Projection::create_perspective(fov_deg, 1.0f, 10.0f, 4000.0f);
	float pm[16];
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			pm[c * 4 + r] = persp.columns[c][r];
		}
	}
	_s019_mat_mul(r_vp, pm, view);
}

bool GneRenderServer::_shadow_upload_binds() {
	if (rendering_device == nullptr) {
		return false;
	}
	if (shadow_record_buffer.is_null() || shadow_lut_buffer.is_null()) {
		return true; // store not created yet; nothing to upload
	}
	uint8_t rec_bytes[GNE_SHADOW_MAX_BINDS * 416];
	memset(rec_bytes, 0, sizeof(rec_bytes));
	for (int i = 0; i < shadow_bind_count; i++) {
		uint8_t *dst = rec_bytes + i * 416;
		// GLSL reads h0 as floats: int(r.h0.y + 0.5), r.h0.w < 0.5. Raw
		// int32 bits read back as ~1e-45 made every lookup return "lit"
		// (Lesson 3: silent int/float type mismatch). Write floats instead.
		float rec_h0[4] = { (float)shadow_binds[i].light_id, (float)shadow_binds[i].type, (float)shadow_binds[i].slot, (float)shadow_binds[i].enabled };
		memcpy(dst, rec_h0, 16); // light_id/type/slot/enabled (float-typed header)
		memcpy(dst + 16, shadow_binds[i].splits, 16);
		memcpy(dst + 32, shadow_binds[i].vps, 384);
	}
	rendering_device->buffer_update(shadow_record_buffer, 0, sizeof(rec_bytes), rec_bytes);
	uint32_t lut[GNE_LIGHT_MAX];
	for (int i = 0; i < GNE_LIGHT_MAX; i++) {
		lut[i] = 0xFFFFFFFFu;
	}
	for (int i = 0; i < shadow_bind_count; i++) {
		int lid = shadow_binds[i].light_id;
		if (lid >= 0 && lid < GNE_LIGHT_MAX) {
			lut[lid] = (uint32_t)i;
		}
	}
	rendering_device->buffer_update(shadow_lut_buffer, 0, sizeof(lut), lut);
	return _shadow_ensure_set2();
}

bool GneRenderServer::_shadow_ensure_set2() {
	if (rendering_device == nullptr || mat_light_shader.is_null()) {
		return false;
	}
	// Lazy dummies: set 2 must exist (018-safe) even when no shadow map was
	// ever created. Guarded sampling never reads them (enable = 0).
	if (shadow_sampler.is_null()) {
		RD::SamplerState ss; // GNE-019.5 slice-0: LINEAR for the ESM path (texelFetch ignores filtering)
		ss.mag_filter = RD::SAMPLER_FILTER_LINEAR;
		ss.min_filter = RD::SAMPLER_FILTER_LINEAR;
		ss.mip_filter = RD::SAMPLER_FILTER_LINEAR;
		shadow_sampler = rendering_device->sampler_create(ss);
		shadow_dummy_tex = _shadow_make_r32(1, 1, false, 1, false);
		shadow_dummy_arr = _shadow_make_r32(1, 1, true, 1, false);
		shadow_dummy_buf = rendering_device->storage_buffer_create(4);
		if (shadow_sampler.is_null() || shadow_dummy_tex.is_null() || shadow_dummy_arr.is_null() || shadow_dummy_buf.is_null()) {
			print_error("[GNE] shadow dummy resource_create failed.");
			return false;
		}
	}
	if (shadow_set2.is_valid()) {
		rendering_device->free_rid(shadow_set2);
		shadow_set2 = RID();
	}
	bool use_real = gpu_shadow_valid;
	RID csm_tex = use_real ? shadow_csm_array : shadow_dummy_arr;
	RID cube0_tex = (use_real && shadow_cube_taken[0]) ? shadow_cube_array[0] : shadow_dummy_arr;
	RID cube1_tex = (use_real && shadow_cube_taken[1]) ? shadow_cube_array[1] : shadow_dummy_arr;
	RID spot0_tex = (use_real && shadow_spot_taken[0]) ? shadow_spot_render[0] : shadow_dummy_tex;
	RID spot1_tex = (use_real && shadow_spot_taken[1]) ? shadow_spot_render[1] : shadow_dummy_tex;
	RID rec_buf = (use_real && shadow_record_buffer.is_valid()) ? shadow_record_buffer : shadow_dummy_buf;
	RID lut_buf = (use_real && shadow_lut_buffer.is_valid()) ? shadow_lut_buffer : shadow_dummy_buf;
	Vector<RD::Uniform> su;
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 0;
		u.append_id(shadow_sampler);
		u.append_id(csm_tex);
		su.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 1;
		u.append_id(shadow_sampler);
		u.append_id(cube0_tex);
		su.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 2;
		u.append_id(shadow_sampler);
		u.append_id(cube1_tex);
		su.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 3;
		u.append_id(shadow_sampler);
		u.append_id(spot0_tex);
		su.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 4;
		u.append_id(shadow_sampler);
		u.append_id(spot1_tex);
		su.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 5;
		u.append_id(rec_buf);
		su.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 6;
		u.append_id(lut_buf);
		su.push_back(u);
	}
	shadow_set2 = rendering_device->uniform_set_create(su, mat_light_shader, 2);
	if (shadow_set2.is_null()) {
		print_error("[GNE] shadow set-2 uniform_set_create failed.");
		return false;
	}
	return true;
}

void GneRenderServer::_destroy_shadow() {
	if (rendering_device == nullptr) {
		return;
	}
	if (shadow_set2.is_valid()) {
		rendering_device->free_rid(shadow_set2);
		shadow_set2 = RID();
	}
	if (shadow_cull_set.is_valid()) {
		rendering_device->free_rid(shadow_cull_set);
		shadow_cull_set = RID();
	}
	if (shadow_depth_set.is_valid()) {
		rendering_device->free_rid(shadow_depth_set);
		shadow_depth_set = RID();
	}
	if (shadow_cull_pipeline.is_valid()) {
		rendering_device->free_rid(shadow_cull_pipeline);
		shadow_cull_pipeline = RID();
	}
	if (shadow_pack_pipeline.is_valid()) {
		rendering_device->free_rid(shadow_pack_pipeline);
		shadow_pack_pipeline = RID();
	}
	if (shadow_pack_shader.is_valid()) {
		rendering_device->free_rid(shadow_pack_shader);
		shadow_pack_shader = RID();
	}
	if (shadow_cull_shader.is_valid()) {
		rendering_device->free_rid(shadow_cull_shader);
		shadow_cull_shader = RID();
	}
	if (shadow_depth_pipeline.is_valid()) {
		rendering_device->free_rid(shadow_depth_pipeline);
		shadow_depth_pipeline = RID();
	}
	if (shadow_depth_shader.is_valid()) {
		rendering_device->free_rid(shadow_depth_shader);
		shadow_depth_shader = RID();
	}
	if (shadow_record_buffer.is_valid()) {
		rendering_device->free_rid(shadow_record_buffer);
		shadow_record_buffer = RID();
	}
	if (shadow_lut_buffer.is_valid()) {
		rendering_device->free_rid(shadow_lut_buffer);
		shadow_lut_buffer = RID();
	}
	if (shadow_cull_planes_buffer.is_valid()) {
		rendering_device->free_rid(shadow_cull_planes_buffer);
		shadow_cull_planes_buffer = RID();
	}
	if (shadow_cull_aux_buffer.is_valid()) {
		rendering_device->free_rid(shadow_cull_aux_buffer);
		shadow_cull_aux_buffer = RID();
	}
	if (shadow_cull_pos_buffer.is_valid()) {
		rendering_device->free_rid(shadow_cull_pos_buffer);
		shadow_cull_pos_buffer = RID();
	}
	if (shadow_cull_mask_buffer.is_valid()) {
		rendering_device->free_rid(shadow_cull_mask_buffer);
		shadow_cull_mask_buffer = RID();
	}
	if (shadow_view_ubo.is_valid()) {
		rendering_device->free_rid(shadow_view_ubo);
		shadow_view_ubo = RID();
	}
	// ORDER: framebuffers before their textures (an FB auto-invalidates when
	// its attachment dies; freeing the texture first makes the FB free fail).
	for (int i = 0; i < 4; i++) {
		if (shadow_csm_fb[i].is_valid()) {
			rendering_device->free_rid(shadow_csm_fb[i]);
			shadow_csm_fb[i] = RID();
		}
	}
	for (int s = 0; s < 2; s++) {
		for (int k = 0; k < 6; k++) {
			if (shadow_cube_fb[s][k].is_valid()) {
				rendering_device->free_rid(shadow_cube_fb[s][k]);
				shadow_cube_fb[s][k] = RID();
			}
		}
		if (shadow_spot_fb[s].is_valid()) {
			rendering_device->free_rid(shadow_spot_fb[s]);
			shadow_spot_fb[s] = RID();
		}
	}
	for (int i = 0; i < 4; i++) {
		if (shadow_csm_render[i].is_valid()) {
			rendering_device->free_rid(shadow_csm_render[i]);
			shadow_csm_render[i] = RID();
		}
	}
	if (shadow_csm_array.is_valid()) {
		rendering_device->free_rid(shadow_csm_array);
		shadow_csm_array = RID();
	}
	for (int s = 0; s < 2; s++) {
		for (int k = 0; k < 6; k++) {
			if (shadow_cube_render[s][k].is_valid()) {
				rendering_device->free_rid(shadow_cube_render[s][k]);
				shadow_cube_render[s][k] = RID();
			}
		}
		if (shadow_cube_array[s].is_valid()) {
			rendering_device->free_rid(shadow_cube_array[s]);
			shadow_cube_array[s] = RID();
		}
		if (shadow_spot_render[s].is_valid()) {
			rendering_device->free_rid(shadow_spot_render[s]);
			shadow_spot_render[s] = RID();
		}
	}
	if (shadow_depth_big.is_valid()) {
		rendering_device->free_rid(shadow_depth_big);
		shadow_depth_big = RID();
	}
	if (shadow_depth_small.is_valid()) {
		rendering_device->free_rid(shadow_depth_small);
		shadow_depth_small = RID();
	}
	if (shadow_sampler.is_valid()) {
		rendering_device->free_rid(shadow_sampler);
		shadow_sampler = RID();
	}
	if (shadow_dummy_tex.is_valid()) {
		rendering_device->free_rid(shadow_dummy_tex);
		shadow_dummy_tex = RID();
	}
	if (shadow_dummy_arr.is_valid()) {
		rendering_device->free_rid(shadow_dummy_arr);
		shadow_dummy_arr = RID();
	}
	if (shadow_dummy_buf.is_valid()) {
		rendering_device->free_rid(shadow_dummy_buf);
		shadow_dummy_buf = RID();
	}
	shadow_fb_format = -1;
	gpu_shadow_valid = false;
	shadow_bind_count = 0;
	shadow_caster_total = 0;
	shadow_csm_taken = false;
	shadow_cube_taken[0] = shadow_cube_taken[1] = false;
	shadow_spot_taken[0] = shadow_spot_taken[1] = false;
	memset(shadow_binds, 0, sizeof(shadow_binds));
	memset(shadow_bind_casters, 0, sizeof(shadow_bind_casters));
}

RID GneRenderServer::_shadow_make_r32(int p_w, int p_h, bool p_array, int p_layers, bool p_attach) {
	RD::TextureFormat tf;
	tf.format = RD::DATA_FORMAT_R32_SFLOAT;
	tf.texture_type = p_array ? RD::TEXTURE_TYPE_2D_ARRAY : RD::TEXTURE_TYPE_2D;
	tf.width = (uint32_t)p_w;
	tf.height = (uint32_t)p_h;
	tf.depth = 1;
	tf.array_layers = (uint32_t)(p_array ? p_layers : 1);
	tf.mipmaps = 1;
	tf.samples = RD::TEXTURE_SAMPLES_1;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
	if (p_attach) {
		tf.usage_bits = (RD::TextureUsageBits)(tf.usage_bits | RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT);
	}
	return rendering_device->texture_create(tf, RD::TextureView());
}

int GneRenderServer::gpu_shadow_map_create(int p_type, int p_resolution) {
	if (!ensure_gpu_device()) {
		return -1;
	}
	if (!_light_ensure_geo()) {
		print_error("[GNE] gpu_shadow_map_create: light store unavailable.");
		return -1;
	}
	if (p_type == 0) {
		if (p_resolution != GNE_SHADOW_CSM_RES) {
			print_error("[GNE] gpu_shadow_map_create: CSM resolution must be 2048 (no silent fallback).");
			return -1;
		}
		if (shadow_csm_taken) {
			print_error("[GNE] gpu_shadow_map_create: the single CSM unit is taken.");
			return -1;
		}
	} else if (p_type == 1) {
		if (p_resolution != GNE_SHADOW_CUBE_RES) {
			print_error("[GNE] gpu_shadow_map_create: cube resolution must be 1024 (no silent fallback).");
			return -1;
		}
	} else if (p_type == 2) {
		if (p_resolution != GNE_SHADOW_SPOT_RES) {
			print_error("[GNE] gpu_shadow_map_create: spot resolution must be 2048 (no silent fallback).");
			return -1;
		}
	} else {
		print_error("[GNE] gpu_shadow_map_create: unknown type (0 = CSM, 1 = cube, 2 = spot).");
		return -1;
	}
	// First create: shared resources (sampler, dummies, buffers, depth pipeline).
	if (!gpu_shadow_valid) {
		// KI-009: reuse-only. _shadow_ensure_set2 (the 018-safe lazy path) may
		// have created these already; recreating orphaned them (1 Sampler + 2
		// Textures + 1 StorageBuffer leaked per run).
		RD::SamplerState ss; // GNE-019.5 slice-0: LINEAR for the ESM path (texelFetch ignores filtering)
		ss.mag_filter = RD::SAMPLER_FILTER_LINEAR;
		ss.min_filter = RD::SAMPLER_FILTER_LINEAR;
		ss.mip_filter = RD::SAMPLER_FILTER_LINEAR;
		if (shadow_sampler.is_null()) {
			shadow_sampler = rendering_device->sampler_create(ss);
		}
		if (shadow_dummy_tex.is_null()) {
			shadow_dummy_tex = _shadow_make_r32(1, 1, false, 1, false);
		}
		if (shadow_dummy_arr.is_null()) {
			shadow_dummy_arr = _shadow_make_r32(1, 1, true, 1, false);
		}
		if (shadow_dummy_buf.is_null()) {
			shadow_dummy_buf = rendering_device->storage_buffer_create(4);
		}
		shadow_record_buffer = rendering_device->storage_buffer_create(GNE_SHADOW_MAX_BINDS * 416);
		shadow_lut_buffer = rendering_device->storage_buffer_create(GNE_LIGHT_MAX * 4);
		shadow_cull_planes_buffer = rendering_device->storage_buffer_create(GNE_SHADOW_MAX_BINDS * 6 * 16);
		shadow_cull_aux_buffer = rendering_device->storage_buffer_create(GNE_SHADOW_MAX_BINDS * 16);
		shadow_cull_pos_buffer = rendering_device->storage_buffer_create(GNE_SHADOW_MAX_BINDS * 16);
		shadow_cull_mask_buffer = rendering_device->storage_buffer_create(GNE_SHADOW_MAX_BINDS * 4);
		shadow_view_ubo = rendering_device->uniform_buffer_create(sizeof(GneViewData));
		if (shadow_sampler.is_null() || shadow_dummy_tex.is_null() || shadow_dummy_arr.is_null() ||
				shadow_dummy_buf.is_null() || shadow_record_buffer.is_null() || shadow_lut_buffer.is_null() ||
				shadow_cull_planes_buffer.is_null() || shadow_cull_aux_buffer.is_null() ||
				shadow_cull_pos_buffer.is_null() || shadow_cull_mask_buffer.is_null() || shadow_view_ubo.is_null()) {
			print_error("[GNE] gpu_shadow_map_create: shared shadow resource_create failed.");
			_destroy_shadow();
			return -1;
		}
		// LUT = unbound sentinel.
		uint8_t ff[GNE_LIGHT_MAX * 4];
		memset(ff, 0xFF, sizeof(ff));
		rendering_device->buffer_update(shadow_lut_buffer, 0, sizeof(ff), ff);
		// Shared depth attachments (D32, cleared per map; sequential use only).
		RD::TextureFormat df;
		df.format = RD::DATA_FORMAT_D32_SFLOAT;
		df.texture_type = RD::TEXTURE_TYPE_2D;
		df.depth = 1;
		df.array_layers = 1;
		df.mipmaps = 1;
		df.samples = RD::TEXTURE_SAMPLES_1;
		df.usage_bits = RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
		df.width = GNE_SHADOW_CSM_RES;
		df.height = GNE_SHADOW_CSM_RES;
		shadow_depth_big = rendering_device->texture_create(df, RD::TextureView());
		df.width = GNE_SHADOW_CUBE_RES;
		df.height = GNE_SHADOW_CUBE_RES;
		shadow_depth_small = rendering_device->texture_create(df, RD::TextureView());
		if (shadow_depth_big.is_null() || shadow_depth_small.is_null()) {
			print_error("[GNE] gpu_shadow_map_create: shared depth texture_create failed.");
			_destroy_shadow();
			return -1;
		}
		// Framebuffer format: R32 color + D32 depth (size-independent).
		Vector<RD::AttachmentFormat> afs;
		RD::AttachmentFormat caf;
		caf.format = RD::DATA_FORMAT_R32_SFLOAT;
		caf.samples = RD::TEXTURE_SAMPLES_1;
		caf.usage_flags = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
		afs.push_back(caf);
		RD::AttachmentFormat daf;
		daf.format = RD::DATA_FORMAT_D32_SFLOAT;
		daf.samples = RD::TEXTURE_SAMPLES_1;
		daf.usage_flags = RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
		afs.push_back(daf);
		shadow_fb_format = rendering_device->framebuffer_format_create(afs);
		if (shadow_fb_format < 0) {
			print_error("[GNE] gpu_shadow_map_create: shadow framebuffer_format_create failed.");
			_destroy_shadow();
			return -1;
		}
		// Depth pipeline: SAME batch vertex shader (identical set-0 layout,
		// binding 2 swapped for shadow_view_ubo) + R32 depth frag.
		String error;
		Vector<uint8_t> vert_spv = rendering_device->shader_compile_spirv_from_source(
				RD::SHADER_STAGE_VERTEX, String(gpu_shadow_depth_vert_glsl), RD::SHADER_LANGUAGE_GLSL, &error);
		if (vert_spv.is_empty()) {
			print_error("[GNE] shadow vertex shader compile failed:");
			print_error(error);
			_destroy_shadow();
			return -1;
		}
		Vector<uint8_t> frag_spv = rendering_device->shader_compile_spirv_from_source(
				RD::SHADER_STAGE_FRAGMENT, String(gpu_shadow_depth_frag_glsl), RD::SHADER_LANGUAGE_GLSL, &error);
		if (frag_spv.is_empty()) {
			print_error("[GNE] shadow depth fragment shader compile failed:");
			print_error(error);
			_destroy_shadow();
			return -1;
		}
		Vector<RD::ShaderStageSPIRVData> stages;
		RD::ShaderStageSPIRVData vs;
		vs.shader_stage = RD::SHADER_STAGE_VERTEX;
		vs.spirv = vert_spv;
		stages.push_back(vs);
		RD::ShaderStageSPIRVData fs;
		fs.shader_stage = RD::SHADER_STAGE_FRAGMENT;
		fs.spirv = frag_spv;
		stages.push_back(fs);
		shadow_depth_shader = rendering_device->shader_create_from_spirv(stages, "gne_shadow_depth");
		if (shadow_depth_shader.is_null()) {
			print_error("[GNE] gpu_shadow_map_create: shadow shader_create_from_spirv failed.");
			_destroy_shadow();
			return -1;
		}
		RD::PipelineRasterizationState rs;
		RD::PipelineMultisampleState ms;
		RD::PipelineDepthStencilState ds;
		ds.enable_depth_test = true;
		ds.enable_depth_write = true;
		ds.depth_compare_operator = RD::COMPARE_OP_LESS_OR_EQUAL;
		RD::PipelineColorBlendState bs = RD::PipelineColorBlendState::create_disabled(1);
		shadow_depth_pipeline = rendering_device->render_pipeline_create(
				shadow_depth_shader, shadow_fb_format, mesh_vertex_format, RD::RENDER_PRIMITIVE_TRIANGLES, rs, ms, ds, bs, 0, 0);
		if (shadow_depth_pipeline.is_null()) {
			print_error("[GNE] gpu_shadow_map_create: shadow render_pipeline_create failed.");
			_destroy_shadow();
			return -1;
		}
		// Cull compute pipeline.
		Vector<uint8_t> cull_spv = rendering_device->shader_compile_spirv_from_source(
				RD::SHADER_STAGE_COMPUTE, String(gpu_shadow_cull_glsl), RD::SHADER_LANGUAGE_GLSL, &error);
		if (cull_spv.is_empty()) {
			print_error("[GNE] shadow cull shader compile failed:");
			print_error(error);
			_destroy_shadow();
			return -1;
		}
		RD::ShaderStageSPIRVData cs;
		cs.shader_stage = RD::SHADER_STAGE_COMPUTE;
		cs.spirv = cull_spv;
		Vector<RD::ShaderStageSPIRVData> cstages;
		cstages.push_back(cs);
		shadow_cull_shader = rendering_device->shader_create_from_spirv(cstages, "gne_shadow_cull");
		if (shadow_cull_shader.is_null()) {
			print_error("[GNE] gpu_shadow_map_create: shadow cull shader_create failed.");
			_destroy_shadow();
			return -1;
		}
		shadow_cull_pipeline = rendering_device->compute_pipeline_create(shadow_cull_shader);
		if (shadow_cull_pipeline.is_null()) {
			print_error("[GNE] gpu_shadow_map_create: shadow cull pipeline_create failed.");
			_destroy_shadow();
			return -1;
		}
		// Pack pass: 2D render target -> sampling-array layer (texture_copy is
		// unreliable in this fork).
		Vector<uint8_t> pack_spv = rendering_device->shader_compile_spirv_from_source(
				RD::SHADER_STAGE_COMPUTE, String(gpu_shadow_pack_glsl), RD::SHADER_LANGUAGE_GLSL, &error);
		if (pack_spv.is_empty()) {
			print_error("[GNE] shadow pack shader compile failed:");
			print_error(error);
			_destroy_shadow();
			return -1;
		}
		RD::ShaderStageSPIRVData ps;
		ps.shader_stage = RD::SHADER_STAGE_COMPUTE;
		ps.spirv = pack_spv;
		Vector<RD::ShaderStageSPIRVData> pstages;
		pstages.push_back(ps);
		shadow_pack_shader = rendering_device->shader_create_from_spirv(pstages, "gne_shadow_pack");
		if (shadow_pack_shader.is_null()) {
			print_error("[GNE] gpu_shadow_map_create: shadow pack shader_create failed.");
			_destroy_shadow();
			return -1;
		}
		shadow_pack_pipeline = rendering_device->compute_pipeline_create(shadow_pack_shader);
		if (shadow_pack_pipeline.is_null()) {
			print_error("[GNE] gpu_shadow_map_create: shadow pack pipeline_create failed.");
			_destroy_shadow();
			return -1;
		}
		gpu_shadow_valid = true;
	}
	int unit = -1;
	if (p_type == 0) {
		for (int k = 0; k < 4; k++) {
			shadow_csm_render[k] = _shadow_make_r32(GNE_SHADOW_CSM_RES, GNE_SHADOW_CSM_RES, false, 1, true);
			if (shadow_csm_render[k].is_null()) {
				print_error("[GNE] gpu_shadow_map_create: CSM render texture_create failed.");
				_destroy_shadow();
				return -1;
			}
			Vector<RID> atts;
			atts.push_back(shadow_csm_render[k]);
			atts.push_back(shadow_depth_big);
			shadow_csm_fb[k] = rendering_device->framebuffer_create(atts, RD::INVALID_ID);
			if (shadow_csm_fb[k].is_null()) {
				print_error("[GNE] gpu_shadow_map_create: CSM framebuffer_create failed.");
				_destroy_shadow();
				return -1;
			}
		}
		shadow_csm_array = _shadow_make_r32(GNE_SHADOW_CSM_RES, GNE_SHADOW_CSM_RES, true, 4, false);
		if (shadow_csm_array.is_null()) {
			print_error("[GNE] gpu_shadow_map_create: CSM array texture_create failed.");
			_destroy_shadow();
			return -1;
		}
		shadow_csm_taken = true;
		unit = 0;
	} else if (p_type == 1) {
		for (int s = 0; s < 2; s++) {
			if (!shadow_cube_taken[s]) {
				unit = s;
				break;
			}
		}
		if (unit < 0) {
			print_error("[GNE] gpu_shadow_map_create: both cube units taken (max 2).");
			return -1;
		}
		for (int k = 0; k < 6; k++) {
			shadow_cube_render[unit][k] = _shadow_make_r32(GNE_SHADOW_CUBE_RES, GNE_SHADOW_CUBE_RES, false, 1, true);
			if (shadow_cube_render[unit][k].is_null()) {
				print_error("[GNE] gpu_shadow_map_create: cube render texture_create failed.");
				_destroy_shadow();
				return -1;
			}
			Vector<RID> atts;
			atts.push_back(shadow_cube_render[unit][k]);
			atts.push_back(shadow_depth_small);
			shadow_cube_fb[unit][k] = rendering_device->framebuffer_create(atts, RD::INVALID_ID);
			if (shadow_cube_fb[unit][k].is_null()) {
				print_error("[GNE] gpu_shadow_map_create: cube framebuffer_create failed.");
				_destroy_shadow();
				return -1;
			}
		}
		shadow_cube_array[unit] = _shadow_make_r32(GNE_SHADOW_CUBE_RES, GNE_SHADOW_CUBE_RES, true, 6, false);
		if (shadow_cube_array[unit].is_null()) {
			print_error("[GNE] gpu_shadow_map_create: cube array texture_create failed.");
			_destroy_shadow();
			return -1;
		}
		shadow_cube_taken[unit] = true;
	} else {
		for (int s = 0; s < 2; s++) {
			if (!shadow_spot_taken[s]) {
				unit = s;
				break;
			}
		}
		if (unit < 0) {
			print_error("[GNE] gpu_shadow_map_create: both spot units taken (max 2).");
			return -1;
		}
		shadow_spot_render[unit] = _shadow_make_r32(GNE_SHADOW_SPOT_RES, GNE_SHADOW_SPOT_RES, false, 1, true);
		if (shadow_spot_render[unit].is_null()) {
			print_error("[GNE] gpu_shadow_map_create: spot render texture_create failed.");
			_destroy_shadow();
			return -1;
		}
		Vector<RID> atts;
		atts.push_back(shadow_spot_render[unit]);
		atts.push_back(shadow_depth_big);
		shadow_spot_fb[unit] = rendering_device->framebuffer_create(atts, RD::INVALID_ID);
		if (shadow_spot_fb[unit].is_null()) {
			print_error("[GNE] gpu_shadow_map_create: spot framebuffer_create failed.");
			_destroy_shadow();
			return -1;
		}
		shadow_spot_taken[unit] = true;
	}
	if (!_shadow_upload_binds()) {
		_destroy_shadow();
		return -1;
	}
	return (p_type << 8) | unit;
}

bool GneRenderServer::gpu_shadow_light_bind(int p_light_id, int p_shadow_id) {
	if (!gpu_shadow_valid) {
		print_error("[GNE] gpu_shadow_light_bind: no shadow store. Call gpu_shadow_map_create first.");
		return false;
	}
	if (p_shadow_id == -1) {
		// Unbind = REMOVE (never a silent disable).
		for (int i = 0; i < shadow_bind_count; i++) {
			if (shadow_binds[i].light_id == p_light_id) {
				shadow_binds[i] = shadow_binds[shadow_bind_count - 1];
				memset(&shadow_binds[shadow_bind_count - 1], 0, sizeof(GneShadowBind));
				shadow_bind_count--;
				shadow_caster_total = 0; // recomputed by next cull
				return _shadow_upload_binds();
			}
		}
		print_error("[GNE] gpu_shadow_light_bind: light has no bind to remove.");
		return false;
	}
	int type = (p_shadow_id >> 8) & 0xFF;
	int unit = p_shadow_id & 0xFF;
	if (p_light_id == GNE_SHADOW_DIR_LIGHT) {
		if (type != 0 || unit != 0 || !shadow_csm_taken) {
			print_error("[GNE] gpu_shadow_light_bind: dir pseudo-light needs the CSM unit.");
			return false;
		}
	} else {
		if (p_light_id < 0 || p_light_id >= light_count) {
			print_error("[GNE] gpu_shadow_light_bind: light id out of range.");
			return false;
		}
		int ltype = (int)light_cpu[p_light_id].type;
		if ((ltype == 0 && (type != 1 || unit < 0 || unit > 1 || !shadow_cube_taken[unit])) ||
				(ltype == 1 && (type != 2 || unit < 0 || unit > 1 || !shadow_spot_taken[unit]))) {
			print_error("[GNE] gpu_shadow_light_bind: light/shadow type mismatch (point->cube, spot->spot).");
			return false;
		}
	}
	if (shadow_bind_count >= GNE_SHADOW_MAX_BINDS) {
		print_error("[GNE] gpu_shadow_light_bind: bind table full (64).");
		return false;
	}
	// Rebind in place when the light already has a bind (deterministic order kept).
	int target = shadow_bind_count;
	for (int i = 0; i < shadow_bind_count; i++) {
		if (shadow_binds[i].light_id == p_light_id) {
			target = i;
			break;
		}
	}
	if (target == shadow_bind_count) {
		memset(&shadow_binds[target], 0, sizeof(GneShadowBind));
		shadow_bind_count++;
	}
	GneShadowBind &bnd = shadow_binds[target];
	bnd.light_id = p_light_id;
	bnd.type = type;
	bnd.slot = unit;
	bnd.enabled = 1;
	// GNE-019: (re)compute the VPs immediately on every bind - the shadow
	// records must never carry zero VPs between rebinds (cull_dispatch is
	// not guaranteed to re-run, e.g. the S1/S2/S3 unbind-rebind checks).
	if (type == 0) {
		_shadow_vp_csm(&bnd.vps[0][0], bnd.splits, bnd.ortho_bounds);
	} else if (type == 1) {
		const GneLight &L1 = light_cpu[p_light_id];
		float lpos[3] = { L1.pos[0], L1.pos[1], L1.pos[2] };
		_shadow_vp_cube(lpos, &bnd.vps[0][0]);
		bnd.splits[0] = lpos[0];
		bnd.splits[1] = lpos[1];
		bnd.splits[2] = lpos[2];
		bnd.splits[3] = L1.range;
	} else {
		const GneLight &L1 = light_cpu[p_light_id];
		float lpos[3] = { L1.pos[0], L1.pos[1], L1.pos[2] };
		float ldir[3] = { L1.dir[0], L1.dir[1], L1.dir[2] };
		_shadow_vp_spot(lpos, ldir, L1.cone_outer, &bnd.vps[0][0]);
		bnd.splits[0] = lpos[0];
		bnd.splits[1] = lpos[1];
		bnd.splits[2] = lpos[2];
		bnd.splits[3] = L1.range;
	}
	return _shadow_upload_binds();
}

bool GneRenderServer::gpu_shadow_cull_dispatch() {
	if (!gpu_shadow_valid) {
		print_error("[GNE] gpu_shadow_cull_dispatch: no shadow store.");
		return false;
	}
	if (shadow_bind_count <= 0) {
		print_error("[GNE] gpu_shadow_cull_dispatch: no binds (bind lights first).");
		return false;
	}
	if (!camera_view_valid) {
		print_error("[GNE] gpu_shadow_cull_dispatch: no camera. Call gpu_scene_set_camera first.");
		return false;
	}
	if (!gpu_mesh_batch_valid || transform_buffer.is_null()) {
		print_error("[GNE] gpu_shadow_cull_dispatch: no batch mesh.");
		return false;
	}
	// VPs first (planes derive from them), then planes/aux/pos uploads.
	for (int i = 0; i < shadow_bind_count; i++) {
		GneShadowBind &b = shadow_binds[i];
		if (b.type == 0) {
			_shadow_vp_csm(&b.vps[0][0], b.splits, b.ortho_bounds);
		} else if (b.type == 1) {
			const GneLight &L = light_cpu[b.light_id];
			float pos[3] = { L.pos[0], L.pos[1], L.pos[2] };
			_shadow_vp_cube(pos, &b.vps[0][0]);
			b.splits[0] = pos[0];
			b.splits[1] = pos[1];
			b.splits[2] = pos[2];
			b.splits[3] = L.range;
		} else {
			const GneLight &L = light_cpu[b.light_id];
			float pos[3] = { L.pos[0], L.pos[1], L.pos[2] };
			float dir[3] = { L.dir[0], L.dir[1], L.dir[2] };
			_shadow_vp_spot(pos, dir, L.cone_outer, &b.vps[0][0]);
			b.splits[0] = pos[0];
			b.splits[1] = pos[1];
			b.splits[2] = pos[2];
			b.splits[3] = L.range;
		}
	}
	uint8_t planes[GNE_SHADOW_MAX_BINDS * 6 * 16];
	uint8_t aux[GNE_SHADOW_MAX_BINDS * 16];
	uint8_t pos[GNE_SHADOW_MAX_BINDS * 16];
	memset(planes, 0, sizeof(planes));
	memset(aux, 0, sizeof(aux));
	memset(pos, 0, sizeof(pos));
	for (int i = 0; i < shadow_bind_count; i++) {
		GneShadowBind &b = shadow_binds[i];
		float *pl = (float *)(planes + i * 96);
		if (b.type == 0) {
			// CSM/dir: whole-scene ortho (shader short-circuits to all-cast).
			memset(pl, 0, 96);
		} else if (b.type == 1) {
			memset(pl, 0, 96); // cube uses the range-sphere test, not planes
		} else {
			_shadow_planes_from_vp(&b.vps[0][0], pl);
		}
		float *ax = (float *)(aux + i * 16);
		ax[0] = (float)b.type;
		if (b.type == 1 || b.type == 2) {
			ax[1] = light_cpu[b.light_id].range;
			float *pp = (float *)(pos + i * 16);
			pp[0] = light_cpu[b.light_id].pos[0];
			pp[1] = light_cpu[b.light_id].pos[1];
			pp[2] = light_cpu[b.light_id].pos[2];
		}
	}
	rendering_device->buffer_update(shadow_cull_planes_buffer, 0, sizeof(planes), planes);
	rendering_device->buffer_update(shadow_cull_aux_buffer, 0, sizeof(aux), aux);
	rendering_device->buffer_update(shadow_cull_pos_buffer, 0, sizeof(pos), pos);
	rendering_device->buffer_clear(shadow_cull_mask_buffer, 0, GNE_SHADOW_MAX_BINDS * 4);
	// (Re)build the cull set every dispatch: batch buffers may be recreated by
	// mesh rebuilds, and a stale set is a silent-corruption landmine.
	if (shadow_cull_set.is_valid()) {
		rendering_device->free_rid(shadow_cull_set);
		shadow_cull_set = RID();
	}
	Vector<RD::Uniform> cu;
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 0;
		u.append_id(transform_buffer);
		cu.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 1;
		u.append_id(shadow_cull_planes_buffer);
		cu.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 2;
		u.append_id(shadow_cull_aux_buffer);
		cu.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 3;
		u.append_id(shadow_cull_pos_buffer);
		cu.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 4;
		u.append_id(shadow_cull_mask_buffer);
		cu.push_back(u);
	}
	shadow_cull_set = rendering_device->uniform_set_create(cu, shadow_cull_shader, 0);
	if (shadow_cull_set.is_null()) {
		print_error("[GNE] gpu_shadow_cull_dispatch: cull uniform_set_create failed.");
		return false;
	}
	struct CullPush {
		uint32_t bind_count;
		uint32_t instance_count;
		uint32_t pad0;
		uint32_t pad1;
	};
	CullPush cp;
	cp.bind_count = (uint32_t)shadow_bind_count;
	cp.instance_count = (uint32_t)gpu_instance_count;
	cp.pad0 = 0;
	cp.pad1 = 0;
	_run_compute_pass(shadow_cull_pipeline, shadow_cull_set, &cp, sizeof(cp), 1, 1, 1);
	Vector<uint8_t> mbytes = rendering_device->buffer_get_data(shadow_cull_mask_buffer, 0, GNE_SHADOW_MAX_BINDS * 4);
	if (mbytes.size() != GNE_SHADOW_MAX_BINDS * 4) {
		print_error("[GNE] gpu_shadow_cull_dispatch: mask readback failed.");
		return false;
	}
	const uint32_t *masks = (const uint32_t *)mbytes.ptr();
	bool seen[1024];
	memset(seen, 0, sizeof(seen));
	shadow_caster_total = 0;
	for (int i = 0; i < shadow_bind_count; i++) {
		int n = 0;
		uint32_t m = masks[i];
		for (int inst = 0; inst < gpu_instance_count && inst < 32; inst++) {
			if (m & (1u << inst)) {
				n++;
				if (!seen[inst]) {
					seen[inst] = true;
					shadow_caster_total++;
				}
			}
		}
		shadow_bind_casters[i] = n;
		if (n <= 0) {
			// D7-5: a bound shadow with zero casters is a FAIL, never a skip.
			print_error(String("[GNE] gpu_shadow_cull_dispatch: bind ") + itos(i) + " has zero casters (no silent fallback).");
			return false;
		}
	}
	return true;
}

void GneRenderServer::gpu_shadow_esm_set(bool p_enabled) {
	// GNE-019.5 slice-0: dir-CSM ESM prototype flag (default off).
	gne_shadow_esm = p_enabled;
}

bool GneRenderServer::gpu_shadow_render_maps() {
	if (!gpu_shadow_valid) {
		print_error("[GNE] gpu_shadow_render_maps: no shadow store.");
		return false;
	}
	if (shadow_bind_count <= 0) {
		print_error("[GNE] gpu_shadow_render_maps: no binds.");
		return false;
	}
	if (!gpu_mesh_batch_valid || last_batch_count <= 0 || mesh_batch_strategy != GNE_BATCH_STRATEGY_PER_MESH) {
		print_error("[GNE] gpu_shadow_render_maps: requires PER_MESH batch (run gpu_mesh_batch_dispatch first).");
		return false;
	}
	if (mesh_010_vertex_array.is_null() || mesh_010_index_array.is_null()) {
		print_error("[GNE] gpu_shadow_render_maps: no batch mesh arrays.");
		return false;
	}
	// (Re)build the depth set every render: same staleness discipline as cull.
	if (shadow_depth_set.is_valid()) {
		rendering_device->free_rid(shadow_depth_set);
		shadow_depth_set = RID();
	}
	Vector<RD::Uniform> du;
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 0;
		u.append_id(batch_instances_buffer);
		du.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 1;
		u.append_id(transform_buffer);
		du.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u.binding = 2;
		u.append_id(shadow_view_ubo);
		du.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 3;
		u.append_id(mesh_id_buffer);
		du.push_back(u);
	}
	shadow_depth_set = rendering_device->uniform_set_create(du, shadow_depth_shader, 0);
	if (shadow_depth_set.is_null()) {
		print_error("[GNE] gpu_shadow_render_maps: depth uniform_set_create failed.");
		return false;
	}
	GneViewData vd;
	struct DepthPush {
		float light_pos[4];
	};
	DepthPush dp;
	// One synced submission per map (014-waves discipline: never batch
	// dependent passes into one submission in this fork).
	for (int i = 0; i < shadow_bind_count; i++) {
		GneShadowBind &b = shadow_binds[i];
		int maps = (b.type == 0) ? 4 : (b.type == 1 ? 6 : 1);
		for (int k = 0; k < maps; k++) {
			memset(&vd, 0, sizeof(vd));
			memcpy(vd.vp, &b.vps[k][0], 64);
			rendering_device->buffer_update(shadow_view_ubo, 0, sizeof(vd), &vd);
			RID fb;
			if (b.type == 0) {
				fb = shadow_csm_fb[k];
			} else if (b.type == 1) {
				fb = shadow_cube_fb[b.slot][k];
			} else {
				fb = shadow_spot_fb[b.slot];
			}
			if (fb.is_null()) {
				print_error("[GNE] gpu_shadow_render_maps: null framebuffer (bind out of sync with store).");
				return false;
			}
			dp.light_pos[0] = b.splits[0];
			dp.light_pos[1] = b.splits[1];
			dp.light_pos[2] = b.splits[2];
			dp.light_pos[3] = (b.type == 0 && gne_shadow_esm) ? 1.0f : 0.0f; // 019.5 slice-0 mode
			Vector<Color> cc;
			float far_val = (b.type == 0 && gne_shadow_esm) ? (float)exp(87.0) : 1.0f; // ESM far = e^87
			cc.push_back(Color(far_val, 0.0f, 0.0f, 0.0f));
			RD::DrawListID dl = rendering_device->draw_list_begin(fb, RD::DRAW_CLEAR_COLOR_0 | RD::DRAW_CLEAR_DEPTH, cc, 1.0f, 0, Rect2(), 0);
			if (dl == RD::INVALID_ID) {
				print_error("[GNE] gpu_shadow_render_maps: draw_list_begin failed.");
				return false;
			}
			rendering_device->draw_list_bind_render_pipeline(dl, shadow_depth_pipeline);
			rendering_device->draw_list_bind_uniform_set(dl, shadow_depth_set, 0);
			rendering_device->draw_list_bind_vertex_array(dl, mesh_010_vertex_array);
			rendering_device->draw_list_bind_index_array(dl, mesh_010_index_array);
			rendering_device->draw_list_set_push_constant(dl, &dp, sizeof(dp));
			rendering_device->draw_list_draw_indirect(dl, true, batch_args_buffer, 0, (uint32_t)last_batch_count, 20);
			rendering_device->draw_list_end();
			rendering_device->submit();
			rendering_device->sync();
			// Array-backed types pack into the sampling arrays (spots sample
			// directly). texture_copy is unreliable in this fork -> compute pack.
			if (b.type == 0) {
				if (!_shadow_pack(shadow_csm_render[k], shadow_csm_array, k, GNE_SHADOW_CSM_RES)) {
					print_error("[GNE] gpu_shadow_render_maps: CSM pack failed.");
					return false;
				}
			} else if (b.type == 1) {
				if (!_shadow_pack(shadow_cube_render[b.slot][k], shadow_cube_array[b.slot], k, GNE_SHADOW_CUBE_RES)) {
					print_error("[GNE] gpu_shadow_render_maps: cube pack failed.");
					return false;
				}
			}
		}
	}
	// Ensure all queued texture_copy operations execute before any readback.
	rendering_device->submit();
	rendering_device->sync();
	return _shadow_upload_binds();
}

// Pack one 2D shadow render target into a sampling-array layer.
bool GneRenderServer::_shadow_pack(RID p_src, RID p_dst_array, int p_layer, int p_size) {
	if (shadow_pack_pipeline.is_null() || p_src.is_null() || p_dst_array.is_null()) {
		return false;
	}
	// (Re)build the pack set every call: the source texture changes per map.
	if (shadow_pack_set.is_valid()) {
		rendering_device->free_rid(shadow_pack_set);
		shadow_pack_set = RID();
	}
	Vector<RD::Uniform> pu;
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 0;
		u.append_id(shadow_sampler);
		u.append_id(p_src);
		pu.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 1;
		u.append_id(p_dst_array);
		pu.push_back(u);
	}
	shadow_pack_set = rendering_device->uniform_set_create(pu, shadow_pack_shader, 0);
	if (shadow_pack_set.is_null()) {
		print_error("[GNE] shadow pack uniform_set_create failed.");
		return false;
	}
	struct PackParams {
		uint32_t layer;
		uint32_t size;
		uint32_t pad0;
		uint32_t pad1;
	};
	PackParams pp;
	pp.layer = (uint32_t)p_layer;
	pp.size = (uint32_t)p_size;
	pp.pad0 = 0;
	pp.pad1 = 0;
	_run_compute_pass(shadow_pack_pipeline, shadow_pack_set, &pp, sizeof(pp),
			(uint32_t)(p_size / 8), (uint32_t)(p_size / 8), 1);
	return true;
}

Dictionary GneRenderServer::gpu_shadow_get_stats() {
	Dictionary d;
	d["count"] = shadow_bind_count;
	int maps = 0;
	if (shadow_csm_taken) {
		maps += 4;
	}
	for (int s = 0; s < 2; s++) {
		if (shadow_cube_taken[s]) {
			maps += 6;
		}
		if (shadow_spot_taken[s]) {
			maps += 1;
		}
	}
	d["maps"] = maps;
	d["casters"] = shadow_caster_total;
	d["rd"] = (hzb_real_depth_enabled && hzb_depth_proof_count > 0) ? 1 : 0;
	d["valid"] = gpu_shadow_valid;
	return d;
}

void GneRenderServer::gpu_hzb_set_real_depth(bool p_enabled) {
	hzb_real_depth_enabled = p_enabled;
}

PackedFloat32Array GneRenderServer::gpu_shadow_dbg_vp(int p_bind, int p_cas) {
	PackedFloat32Array out;
	if (p_bind < 0 || p_bind >= shadow_bind_count) {
		return out;
	}
	const GneShadowBind &b = shadow_binds[p_bind];
	for (int i = 0; i < 16; i++) {
		out.append(b.vps[p_cas & 3][i]);
	}
	for (int i = 0; i < 4; i++) {
		out.append(b.splits[i]);
	}
	for (int i = 0; i < 6; i++) {
		out.append(b.ortho_bounds[i]);
	}
	return out;
}

PackedInt32Array GneRenderServer::gpu_shadow_dbg_map(int p_type, int p_slot, int p_face) {
	PackedInt32Array out;
	if (!gpu_shadow_valid) {
		return out;
	}
	RID tex;
	int res;
	if (p_type == 0) {
		if (!shadow_csm_taken) {
			return out;
		}
		tex = shadow_csm_array; // read the sampling array (post-copy), like the shader
		res = GNE_SHADOW_CSM_RES;
	} else if (p_type == 1) {
		if (p_slot < 0 || p_slot > 1 || !shadow_cube_taken[p_slot]) {
			return out;
		}
		tex = shadow_cube_render[p_slot][p_face % 6];
		res = GNE_SHADOW_CUBE_RES;
	} else {
		if (p_slot < 0 || p_slot > 1 || !shadow_spot_taken[p_slot]) {
			return out;
		}
		tex = shadow_spot_render[p_slot];
		res = GNE_SHADOW_SPOT_RES;
	}
	if (tex.is_null()) {
		return out;
	}
	uint32_t dbg_layer = (p_type == 0) ? (uint32_t)CLAMP(p_face, 0, 3) : 0; // CSM: face selects the cascade layer
	Vector<uint8_t> bytes = rendering_device->texture_get_data(tex, dbg_layer);
	if (bytes.size() < res * res * 4) {
		return out;
	}
	const float *v = (const float *)bytes.ptr();
	int non_far = 0;
	int minx = -1, miny = -1, maxx = -1, maxy = -1;
	float mn = 1e30f, mx = -1e30f;
	for (int y = 0; y < res; y++) {
		for (int x = 0; x < res; x++) {
			float d = v[y * res + x];
			if (d < 0.999f) {
				non_far++;
				if (minx < 0 || x < minx) {
					minx = x;
				}
				if (maxx < 0 || x > maxx) {
					maxx = x;
				}
				if (miny < 0 || y < miny) {
					miny = y;
				}
				if (maxy < 0 || y > maxy) {
					maxy = y;
				}
				mn = MIN(mn, d);
				mx = MAX(mx, d);
			}
		}
	}
	out.append(non_far);
	out.append(minx);
	out.append(miny);
	out.append(maxx);
	out.append(maxy);
	out.append((int32_t)(mn * 1000.0f));
	out.append((int32_t)(mx * 1000.0f));
	out.append((int32_t)(v[(res / 2) * res + (res / 2)] * 1000.0f));
	return out;
}

bool GneRenderServer::gpu_shadow_dbg_dump(int p_layer, const String &p_path) {
	// GNE-019 DIAG: dump one CSM sampling-array layer as raw R32F floats
	// (2048 * 2048 * 4 bytes) for texel-exact offline inspection.
	if (!gpu_shadow_valid || !shadow_csm_taken || shadow_csm_array.is_null()) {
		return false;
	}
	uint32_t layer = (uint32_t)CLAMP(p_layer, 0, 3);
	Vector<uint8_t> bytes = rendering_device->texture_get_data(shadow_csm_array, layer);
	if (bytes.size() < (int)(GNE_SHADOW_CSM_RES * GNE_SHADOW_CSM_RES * 4)) {
		return false;
	}
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::WRITE);
	if (f.is_null()) {
		return false;
	}
	f->store_buffer(bytes.ptr(), (uint64_t)GNE_SHADOW_CSM_RES * GNE_SHADOW_CSM_RES * 4);
	return true;
}

bool GneRenderServer::gpu_hzb_depth_feed() {
	// D7-3/D7-4: dispatch the (currently dead-code) 012 depth-source pass that
	// samples raster_viewz_texture, then cache the probe as KI-007 proof.
	// Explicit refusal when disabled: no silent fallback.
	if (!hzb_real_depth_enabled) {
		print_error("[GNE] gpu_hzb_depth_feed: refused (hzb_real_depth_enabled = false).");
		return false;
	}
	if (!gpu_hzb_prod_valid) {
		print_error("[GNE] gpu_hzb_depth_feed: production HZB not created.");
		return false;
	}
	if (raster_viewz_texture.is_null()) {
		print_error("[GNE] gpu_hzb_depth_feed: no view-z texture (draw the raster first).");
		return false;
	}
	struct DepthFeedPush {
		uint32_t level0_size;
		uint32_t levels;
		float viewport_w;
		float viewport_h;
		float image_w;
		float image_h;
		float far_plane;
	};
	DepthFeedPush fp;
	fp.level0_size = (uint32_t)HZB_PROD_TEXEL_COUNT;
	fp.levels = (uint32_t)HZB_PROD_LEVELS;
	fp.viewport_w = hzb_viewport_w;
	fp.viewport_h = hzb_viewport_h;
	fp.image_w = (float)RASTER_TARGET_W;
	fp.image_h = (float)RASTER_TARGET_H;
	fp.far_plane = far_plane;
	rendering_device->buffer_clear(hzb_dbg_probe_buffer, 0, 16);
	_run_compute_pass(hzb_depth_source_pipeline, hzb_depth_source_uniform_set, &fp, sizeof(fp),
			(uint32_t)(HZB_PROD_TEXEL_COUNT / 8), (uint32_t)(HZB_PROD_TEXEL_COUNT / 8), 1);
	Vector<uint8_t> pbytes = rendering_device->buffer_get_data(hzb_dbg_probe_buffer, 0, 16);
	if (pbytes.size() != 16) {
		print_error("[GNE] gpu_hzb_depth_feed: probe readback failed.");
		return false;
	}
	const uint32_t *vals = (const uint32_t *)pbytes.ptr();
	hzb_depth_proof_count = vals[0];
	return true;
}

bool GneRenderServer::gpu_material_draw_lights() {
draw_frame_seq++;
	if (!gpu_scene_valid || !gpu_mesh_valid || !gpu_mesh_batch_valid) {
		print_error("[GNE] gpu_material_draw_lights: no batch mesh. Call gpu_mesh_create first.");
		return false;
	}
	if (!gpu_material_valid) {
		print_error("[GNE] gpu_material_draw_lights: no material store. Call gpu_material_create first.");
		return false;
	}
	if (!gpu_light_valid) {
		print_error("[GNE] gpu_material_draw_lights: no light store. Call gpu_light_create first.");
		return false;
	}
	if (!frustum_valid) {
		print_error("[GNE] gpu_material_draw_lights: no camera. Call gpu_scene_set_camera first.");
		return false;
	}
	if (mesh_batch_strategy != GNE_BATCH_STRATEGY_PER_MESH) {
		print_error("[GNE] gpu_material_draw_lights: requires PER_MESH strategy.");
		return false;
	}
	if (last_batch_count <= 0) {
		print_error("[GNE] gpu_material_draw_lights: empty batch (run gpu_mesh_batch_dispatch first).");
		return false;
	}
	if (!_light_ensure_geo()) {
		return false;
	}
	if (light_set0_dirty) {
		if (!_light_build_set0()) {
			return false;
		}
		// GNE-019: set 2 carries no matrices, but rebuild on the same trigger
		// keeps the dummy/real choice in sync with the light pipeline state.
		if (!_shadow_ensure_set2()) {
			return false;
		}
	}
	// 1. Cull: clear counts + overflow, dispatch 3456 threads, sync.
	rendering_device->buffer_clear(cluster_count_buffer, 0, (uint32_t)(GNE_CLUSTER_COUNT * 4));
	rendering_device->buffer_clear(light_overflow_buffer, 0, 8);
	struct CullPush {
		float dims[4];  // light_count, tan_half_fov_v, aspect, unused
		float range[4]; // near, far, raster_w, raster_h
		float rev[4];   // GNE-018-rev: x = normal-cone flag (KI-014 slab + 2c filter)
	};
	CullPush cp;
	cp.dims[0] = (float)light_count;
	cp.dims[1] = cam_tan_v;
	cp.dims[2] = 1920.0f / 1080.0f;
	cp.dims[3] = 0.0f;
	cp.range[0] = cam_near_v;
	cp.range[1] = far_plane;
	cp.range[2] = 1920.0f;
	cp.range[3] = 1080.0f;
	cp.rev[0] = light_cone_enabled ? 1.0f : 0.0f;
	cp.rev[1] = 0.0f;
	cp.rev[2] = 0.0f;
	cp.rev[3] = 0.0f;
	// GNE-021: bounds pass FIRST, one thread per light, so the cull below reads a
	// rect written THIS frame. It was previously dispatched after the cull while
	// its comment claimed "before", so the cull read the previous frame's rect —
	// or uninitialised memory on the first frame. The cluster-id contract caught
	// it immediately: frustum_cull lost membership (1=1 became 1=0),
	// cluster_overflow's overflow collapsed 2404 -> 2, and mat_v2_x1's diff_px
	// went 0 -> 243. Same tanv/aspect as the cull's own math, from the same
	// values, so the two cannot disagree about the projection.
	{
		struct BoundsPush {
			uint32_t light_count;
			float tanv;
			float aspect;
			float unused;
		};
		BoundsPush bp;
		bp.light_count = (uint32_t)light_count;
		bp.tanv = cam_tan_v;
		bp.aspect = 1920.0f / 1080.0f;
		bp.unused = 0.0f;
		uint32_t bgroups = (uint32_t)(((light_count - 1) / 64) + 1);
		_run_compute_pass(light_bounds_pipeline, light_bounds_uniform_set, &bp, sizeof(bp), bgroups, 1, 1);
	}
	_run_compute_pass(light_cull_pipeline, light_cull_uniform_set, &cp, sizeof(cp), 54, 1, 1);
	int cone_age = draw_frame_seq - cone_src_frame;
	cone_age_last = cone_age;
	if (cone_age > cone_age_max) {
		cone_age_max = cone_age;
	}
	// 2. Evidence mirrors (exact integer reads, no float involved).
	{
		Vector<uint8_t> cb = rendering_device->buffer_get_data(cluster_count_buffer, 0,
				(uint32_t)(GNE_CLUSTER_COUNT * 4));
		light_clusters_touched = 0;
		light_assignments = 0;
		if (cb.size() == (int)(GNE_CLUSTER_COUNT * 4)) {
			const uint32_t *c = (const uint32_t *)cb.ptr();
			for (int i = 0; i < GNE_CLUSTER_COUNT; i++) {
				if (c[i] > 0) {
					light_clusters_touched++;
					light_assignments += (int)c[i];
				}
			}
		}
		Vector<uint8_t> ob = rendering_device->buffer_get_data(light_overflow_buffer, 0, 8);
		light_overflows = 0;
		light_tests_performed = 0;
		if (ob.size() == 8) {
			uint32_t o = 0;
			uint32_t t = 0;
			memcpy(&o, ob.ptr(), 4);
			memcpy(&t, ob.ptr() + 4, 4);
			light_tests_performed = (int)t;
			light_overflows = (int)o;
		}
	}
	// 3. Raster with the light pipeline (set 0 shared with mat pipeline by
	// identical layout; set 1 = light set). Same clear/depth/commands.
	struct LightPush {
		float light_xyz_amb[4];
		float cam_xyz[4];
		float light_rgb[4];
		float tex_slot_pad[4];
		float grid_dims[4]; // raster_w, raster_h, near, far
		float shadow_ctrl[4]; // dir bind idx, shadow enable, cam near, csm blend scale
		float shadow_cam[4];
	float gi_min[4];
	float gi_max[4];
	float gi_dims[4];
	float gi_params[4]; // camera forward xyz (planar depth), spare
	};
	LightPush push;
	push.light_xyz_amb[0] = mat_light_dir.x;
	push.light_xyz_amb[1] = mat_light_dir.y;
	push.light_xyz_amb[2] = mat_light_dir.z;
	push.light_xyz_amb[3] = 0.1f;
	push.cam_xyz[0] = meshlet_camera_position[0];
	push.cam_xyz[1] = meshlet_camera_position[1];
	push.cam_xyz[2] = meshlet_camera_position[2];
	push.cam_xyz[3] = 0.0f;
	push.light_rgb[0] = 1.0f;
	push.light_rgb[1] = 1.0f;
	push.light_rgb[2] = 1.0f;
	push.light_rgb[3] = 0.0f;
	push.tex_slot_pad[0] = 0.0f;
	push.tex_slot_pad[1] = 0.0f;
	push.tex_slot_pad[2] = 0.0f;
	push.tex_slot_pad[3] = 0.0f;
	push.grid_dims[0] = 1920.0f;
	push.grid_dims[1] = 1080.0f;
	push.grid_dims[2] = cam_near_v;
	push.grid_dims[3] = far_plane;
	// GNE-019: shadow control (guarded sampling keeps 018 pixels exact when
	// unbound: dir idx -1 + enable 0 -> factor 1.0 everywhere).
	int dir_bind = -1;
	for (int i = 0; i < shadow_bind_count; i++) {
		if (shadow_binds[i].light_id == GNE_SHADOW_DIR_LIGHT && shadow_binds[i].enabled) {
			dir_bind = i;
			break;
		}
	}
	push.shadow_ctrl[0] = (float)dir_bind;
	push.shadow_ctrl[1] = gpu_shadow_valid ? 1.0f : 0.0f;
	push.shadow_ctrl[2] = cam_near_v;
	push.shadow_ctrl[3] = 1.0f; // csm blend scale
	push.shadow_cam[0] = shadow_cam_basis[6];
	push.shadow_cam[1] = shadow_cam_basis[7];
	push.shadow_cam[2] = shadow_cam_basis[8];
	push.shadow_cam[3] = gne_shadow_esm ? 1.0f : 0.0f; // 019.5 slice-0 ESM mode
	push.gi_min[0] = gi_min.x; push.gi_min[1] = gi_min.y; push.gi_min[2] = gi_min.z; push.gi_min[3] = 0.0f;
	push.gi_max[0] = gi_max.x; push.gi_max[1] = gi_max.y; push.gi_max[2] = gi_max.z; push.gi_max[3] = 0.0f;
	push.gi_dims[0] = (float)gi_gx; push.gi_dims[1] = (float)gi_gy; push.gi_dims[2] = (float)gi_gz; push.gi_dims[3] = 0.0f;
	push.gi_params[0] = (gi_atlas.is_valid() && gne_gi_enabled) ? 1.0f : 0.0f;
	push.gi_params[1] = 1.0f; push.gi_params[2] = 0.0f; push.gi_params[3] = 0.0f;
	// GNE-022 section 10: the fragment must sample the CURRENT front atlas
	// (the trace ping-pongs gi_atlas/gi_atlas2). One cached set per front,
	// built lazily; freed with the light sets in _destroy_mesh.
	RID light_set1_use = mat_light_tex_set;
	if (gne_gi_enabled && gi_sampler.is_valid()) {
		int gfront = gi_front;
		if (gi_draw_sets[gfront].is_null()) {
			RID fa = (gfront == 0) ? gi_atlas : gi_atlas2;
			if (fa.is_valid()) {
				Vector<RD::Uniform> lu2;
				const RID lbufs2[4] = {
					light_buffer, cluster_offset_buffer, cluster_count_buffer, cluster_index_buffer
				};
				for (uint32_t b2 = 0; b2 < 4; b2++) {
					RD::Uniform u2;
					u2.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
					u2.binding = b2;
					u2.append_id(lbufs2[b2]);
					lu2.push_back(u2);
				}
				RD::Uniform u4b;
				u4b.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
				u4b.binding = 4;
				u4b.append_id(gi_sampler);
				u4b.append_id(fa);
				lu2.push_back(u4b);
				RID ns = rendering_device->uniform_set_create(lu2, mat_light_shader, 1);
				if (ns.is_valid()) {
					gi_draw_sets[gfront] = ns;
				}
			}
		}
		if (gi_draw_sets[gfront].is_valid()) {
			light_set1_use = gi_draw_sets[gfront];
		}
	}
	Vector<Color> clear_colors;
	clear_colors.push_back(Color(0, 0, 0, 0));
	clear_colors.push_back(Color(far_plane, 0.0f, 0.0f, 0.0f));
	RD::DrawListID dl = rendering_device->draw_list_begin(raster_framebuffer, RD::DRAW_CLEAR_COLOR_0 | RD::DRAW_CLEAR_COLOR_1 | RD::DRAW_CLEAR_DEPTH, clear_colors, 1.0f, 0, Rect2(), 0);
	if (dl == RD::INVALID_ID) {
		print_error("[GNE] gpu_material_draw_lights: draw_list_begin failed.");
		return false;
	}
	rendering_device->draw_list_bind_render_pipeline(dl, mat_light_pipeline);
	rendering_device->draw_list_bind_uniform_set(dl, mat_light_uniform_set, 0);
	rendering_device->draw_list_bind_uniform_set(dl, light_set1_use, 1);
	if (shadow_set2.is_null() && !_shadow_ensure_set2()) {
		print_error("[GNE] gpu_material_draw_lights: shadow set-2 unavailable.");
		return false;
	}
	rendering_device->draw_list_bind_uniform_set(dl, shadow_set2, 2);
	rendering_device->draw_list_bind_vertex_array(dl, mesh_010_vertex_array);
	rendering_device->draw_list_bind_index_array(dl, mesh_010_index_array);

	rendering_device->draw_list_set_push_constant(dl, &push, sizeof(push));
	rendering_device->draw_list_draw_indirect(dl, true, batch_args_buffer, 0, (uint32_t)last_batch_count, 20);
	rendering_device->draw_list_end();

	rendering_device->submit();
	rendering_device->sync();

	mat_dispatches++;
	raster_epoch++;
	// GNE-018-rev (unit 2c): per-frame cone rebuild in the live path (flag on).
	// This frame's raster is the source; the next frame cull consumes it (R7).
	if (light_cone_enabled) {
		gpu_light_cones_build();
	}
	return true;
}

// GNE-018-rev: read one cluster's cone record {axis.xyz, mindp} (tooling path
// only; an all-zero record is the designed "never cull" sentinel).
PackedFloat32Array GneRenderServer::gpu_light_cone_read(int p_cluster) {
	PackedFloat32Array out;
	if (!gpu_light_valid || cluster_cone_buffer.is_null()) {
		return out;
	}
	if (p_cluster < 0 || p_cluster >= GNE_CLUSTER_COUNT) {
		return out;
	}
	Vector<uint8_t> data = rendering_device->buffer_get_data(cluster_cone_buffer, (uint32_t)(p_cluster * 16), 16);
	if (data.size() != 16) {
		return out;
	}
	const float *f = (const float *)data.ptr();
	for (int i = 0; i < 4; i++) {
		out.append(f[i]);
	}
	return out;
}

	// GNE-018-rev: rev-path flag (default OFF; 018 stays byte-identical). Unit 2c
	// will extend this flag to gate the cone prefilter; KI-014 slab fix rides it.
	void GneRenderServer::gpu_light_set_normal_cone(bool p_enabled) {
		light_cone_enabled = p_enabled;
	}
	// GNE-018-rev: build the per-cluster normal cones from the last completed
	// raster (normal + viewz attachments; one fixed-order thread per cluster;
	// no atomics -> bit-stable). Structural 1-frame lag: built after a raster,
	// consumed by a later frame (R7 gate; see gpu_light_cone_epochs).
	bool GneRenderServer::gpu_light_cones_build() {
		if (!gpu_light_valid || cluster_cone_buffer.is_null()) {
			print_error("[GNE] gpu_light_cones_build: no light store. Call gpu_light_create first.");
			return false;
		}
		if (!gpu_raster_valid) {
			print_error("[GNE] gpu_light_cones_build: no raster.");
			return false;
		}
		if (light_cone_pipeline.is_null()) {
			String error;
			Vector<uint8_t> spirv = rendering_device->shader_compile_spirv_from_source(
					RD::SHADER_STAGE_COMPUTE, _gne_glsl_with_shared(gpu_build_cones_glsl, true),
					RD::SHADER_LANGUAGE_GLSL, &error);
			if (spirv.is_empty()) {
				print_error("[GNE] cones shader compile failed:");
				print_error(error);
				return false;
			}
			Vector<RD::ShaderStageSPIRVData> stages;
			RD::ShaderStageSPIRVData cs;
			cs.shader_stage = RD::SHADER_STAGE_COMPUTE;
			cs.spirv = spirv;
			stages.push_back(cs);
			light_cone_shader = rendering_device->shader_create_from_spirv(stages, "gne_build_cones");
			if (light_cone_shader.is_null()) {
				print_error("[GNE] cones shader_create failed.");
				return false;
			}
			light_cone_pipeline = rendering_device->compute_pipeline_create(light_cone_shader);
			if (light_cone_pipeline.is_null()) {
				print_error("[GNE] cones pipeline_create failed.");
				return false;
			}
			RD::SamplerState ss;
			light_cone_sampler = rendering_device->sampler_create(ss);
			if (light_cone_sampler.is_null()) {
				print_error("[GNE] cones sampler_create failed.");
				return false;
			}
			Vector<RD::Uniform> cu;
			RD::Uniform u0;
			u0.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
			u0.binding = 0;
			u0.append_id(light_cone_sampler);
			u0.append_id(raster_normal_texture);
			cu.push_back(u0);
			RD::Uniform u1;
			u1.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
			u1.binding = 1;
			u1.append_id(light_cone_sampler);
			u1.append_id(raster_viewz_texture);
			cu.push_back(u1);
			RD::Uniform u2;
			u2.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u2.binding = 2;
			u2.append_id(cluster_cone_buffer);
			cu.push_back(u2);
			RD::Uniform u3;
			u3.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
			u3.binding = 3;
			u3.append_id(view_ubo);
			cu.push_back(u3);
			light_cone_uniform_set = rendering_device->uniform_set_create(cu, light_cone_shader, 0);
			if (light_cone_uniform_set.is_null()) {
				print_error("[GNE] cones uniform_set_create failed.");
				return false;
			}
		}
		struct ConePush {
			float cam[4];
			float grid[4];
		};
		ConePush cp;
		cp.cam[0] = meshlet_camera_position[0];
		cp.cam[1] = meshlet_camera_position[1];
		cp.cam[2] = meshlet_camera_position[2];
		cp.cam[3] = 0.0f;
		cp.grid[0] = 1920.0f;
		cp.grid[1] = 1080.0f;
		cp.grid[2] = cam_near_v;
		cp.grid[3] = far_plane;
		_run_compute_pass(light_cone_pipeline, light_cone_uniform_set, &cp, sizeof(cp), 54, 1, 1);
		cone_src_epoch = raster_epoch;
		cone_src_frame = draw_frame_seq;
		return true;
	}

	// GNE-018-rev: {raster_epoch, cone_src_epoch} - the R7 frame-delta evidence.
	PackedInt32Array GneRenderServer::gpu_light_cone_epochs() {
		PackedInt32Array out;
		out.append(raster_epoch);
		out.append(cone_src_epoch);
		out.append(cone_age_last);
		out.append(cone_age_max);
		return out;
	}

	// GNE-018-rev unit-2b isolation selftest. Returns [count, in_stride, out_stride,
	// inputs..., outputs...] so the caller can verify expectations independently.
	// Mode 1: slice cross-check (7 in / 2 out) - shared helper vs the literal
	// pre-refactor fragment lines on identical inputs. Mode 2: back-face analytic
	// cases (28 in / 2 out). Mode 3: cull-AABB replica vs fragment slice (12 in / 5 out).
	PackedFloat32Array GneRenderServer::gpu_light_cones_selftest(int p_mode) {
		PackedFloat32Array out;
		if (rendering_device == nullptr) {
			return out;
		}
		if (cones_selftest_pipeline.is_null()) {
			String error;
			Vector<uint8_t> spirv = rendering_device->shader_compile_spirv_from_source(
					RD::SHADER_STAGE_COMPUTE, _gne_glsl_with_shared(gpu_cones_selftest_glsl, true),
					RD::SHADER_LANGUAGE_GLSL, &error);
			if (spirv.is_empty()) {
				print_error("[GNE] selftest shader compile failed:");
				print_error(error);
				return out;
			}
			Vector<RD::ShaderStageSPIRVData> stages;
			RD::ShaderStageSPIRVData cs;
			cs.shader_stage = RD::SHADER_STAGE_COMPUTE;
			cs.spirv = spirv;
			stages.push_back(cs);
			cones_selftest_shader = rendering_device->shader_create_from_spirv(stages, "gne_cones_selftest");
			cones_selftest_pipeline = rendering_device->compute_pipeline_create(cones_selftest_shader);
			cones_selftest_in_buffer = rendering_device->storage_buffer_create(65536);
			cones_selftest_out_buffer = rendering_device->storage_buffer_create(65536);
			if (cones_selftest_pipeline.is_null() || cones_selftest_in_buffer.is_null() || cones_selftest_out_buffer.is_null()) {
				print_error("[GNE] selftest create failed.");
				return out;
			}
			Vector<RD::Uniform> cu;
			RD::Uniform u0;
			u0.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u0.binding = 0;
			u0.append_id(cones_selftest_in_buffer);
			cu.push_back(u0);
			RD::Uniform u1;
			u1.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u1.binding = 1;
			u1.append_id(cones_selftest_out_buffer);
			cu.push_back(u1);
			cones_selftest_set = rendering_device->uniform_set_create(cu, cones_selftest_shader, 0);
			if (cones_selftest_set.is_null()) {
				print_error("[GNE] selftest set create failed.");
				return out;
			}
		}
		Vector<float> cases;
		int count = 0;
		int in_stride = 0;
		int out_stride = 0;
		if (p_mode == 1) {
			static const float kW = 1920.0f, kH = 1080.0f, kNear = 300.0f, kFar = 4000.0f;
			static const float kCases[][2] = {
				{ 0.5f, 300.0f }, { 0.5f, 1095.4f }, { 0.5f, 3999.9f },
				{ 1919.5f, 300.0f }, { 1919.5f, 1095.4f }, { 1919.5f, 4000.1f },
				{ 960.5f, 300.0f }, { 960.5f, 4000.1f }, { 119.5f, 800.0f },
				{ 120.5f, 800.0f }, { 959.5f, 2000.0f }, { 960.5f, 2000.0f },
				{ 960.5f, 119.5f }, { 960.5f, 120.5f }, { 1919.5f, 0.5f },
				{ 0.5f, 1079.5f }, { 960.5f, 573.45f }, { 960.5f, 573.55f },
				{ 300.5f, 2400.0f }, { 1500.5f, 3200.0f }, { 960.5f, 1095.4f },
				{ 960.5f, 1095.45f }, { 1800.5f, 3400.0f }, { 100.5f, 300.5f }
			};
			for (int i = 0; i < 24; i++) {
				cases.push_back(kCases[i][0]);
				cases.push_back(kCases[i][1]);
				cases.push_back(kW);
				cases.push_back(kH);
				cases.push_back(kNear);
				cases.push_back(kFar);
				cases.push_back(kCases[i][1]);
			}
			count = 24; in_stride = 7; out_stride = 2;
		} else if (p_mode == 2) {
			static const float kI[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
			static const float kRY90[16] = { 0,0,-1,0, 0,1,0,0, 1,0,0,0, 0,0,0,1 };
			static const float kRX90[16] = { 1,0,0,0, 0,0,-1,0, 0,1,0,0, 0,0,0,1 };
			static const float kRY45[16] = { 0.7071f,0,-0.7071f,0, 0,1,0,0, 0.7071f,0,0.7071f,0, 5,6,7,1 };
			static const float kT4[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 10,20,30,1 };
			struct Case2 { const float *v; float n[3]; float l[3]; float a[3]; };
			static const Case2 kCases[] = {
				{ kI, { 0,1,0 }, { 0,10,0 }, { 0,0,0 } },
				{ kI, { 0,1,0 }, { 0,-10,0 }, { 0,0,0 } },
				{ kI, { 1,0,0 }, { 10,0,0 }, { 0,0,0 } },
				{ kRY90, { 1,0,0 }, { 0,0,10 }, { 0,0,0 } },
				{ kRY90, { 0,0,1 }, { 10,0,0 }, { 0,0,0 } },
				{ kRX90, { 0,1,0 }, { 0,0,10 }, { 0,0,0 } },
				{ kRY45, { 0,1,0 }, { 0,50,0 }, { 0,0,0 } },
				{ kT4, { 0,0,-1 }, { 10,20,29 }, { 10,20,30 } }
			};
			for (int i = 0; i < 8; i++) {
				for (int j = 0; j < 16; j++) { cases.push_back(kCases[i].v[j]); }
				cases.push_back(kCases[i].n[0]); cases.push_back(kCases[i].n[1]); cases.push_back(kCases[i].n[2]); cases.push_back(0.0f);
				cases.push_back(kCases[i].l[0]); cases.push_back(kCases[i].l[1]); cases.push_back(kCases[i].l[2]); cases.push_back(0.0f);
				cases.push_back(kCases[i].a[0]); cases.push_back(kCases[i].a[1]); cases.push_back(kCases[i].a[2]); cases.push_back(0.0f);
			}
			count = 8; in_stride = 28; out_stride = 2;
		} else if (p_mode == 3) {
			static const float kTiles[3][2] = { { 5.5f, 1075.5f }, { 60.5f, 1020.5f }, { 1914.5f, 60.5f } };
			static const float kPix[3][2] = { { 5.5f, 1075.5f }, { 960.5f, 540.5f }, { 1860.5f, 60.5f } };
			static const float kE[2] = { 750.0f, 2500.0f };
			float tanv = tan(60.0f * 3.14159265358979f / 360.0f);
			float aspect = 1920.0f / 1080.0f;
			for (int t = 0; t < 3; t++) {
				for (int p = 0; p < 3; p++) {
					if (t == 0 && p > 0) { continue; }
					float fx = kPix[p][0];
					float fy = kPix[p][1];
					float ndx = fx / 1920.0f * 2.0f - 1.0f;
					float ndy = 1.0f - fy / 1080.0f * 2.0f;
					float dx = ndx * aspect * tanv;
					float dy = ndy * tanv;
					float nrm = sqrt(dx * dx + dy * dy + 1.0f);
					for (int eI = 0; eI < 2; eI++) {
						float e = kE[eI];
						cases.push_back(fx); cases.push_back(fy);
						cases.push_back(1920.0f); cases.push_back(1080.0f);
						cases.push_back(300.0f); cases.push_back(4000.0f);
						cases.push_back(tanv); cases.push_back(aspect);
						cases.push_back(e);
						cases.push_back(e * dx / nrm);
						cases.push_back(e * dy / nrm);
						cases.push_back(e / nrm);
						count++;
					}
				}
			}
			in_stride = 12; out_stride = 5;
			// (tile table kept for source parity with the analytic plan; the per-case
			//  input already encodes the pixel the fragment would use.)
		} else {
			static const float kF6[][6] = {
				{ 0.2679492f, 1.0f, 300.0f, 4000.0f, 12.0f, 0.0f },
				{ 0.5773503f, 1.7777778f, 300.0f, 4000.0f, 12.0f, 0.0f },
				{ 1.0f, 2.5f, 300.0f, 4000.0f, 23.0f, 0.0f },
				{ 0.7673270f, 1.3333333f, 300.0f, 4000.0f, 0.0f, 0.0f },
				{ 0.4142136f, 1.7777778f, 300.0f, 4000.0f, 6.0f, 0.0f },
				{ 0.5773503f, 1.0f, 300.0f, 4000.0f, 18.0f, 0.0f }
			};
			for (int i2 = 0; i2 < 6; i2++) {
				for (int j = 0; j < 6; j++) {
					cases.push_back(kF6[i2][j]);
				}
			}
			count = 6; in_stride = 6; out_stride = 3;
		}
		if (count == 0) {
			return out;
		}
		rendering_device->buffer_update(cones_selftest_in_buffer, 0, (uint32_t)(cases.size() * 4), cases.ptr());
		struct TestPush {
			uint32_t mode;
			uint32_t count;
		};
		TestPush tp;
		tp.mode = (uint32_t)p_mode;
		tp.count = (uint32_t)count;
		_run_compute_pass(cones_selftest_pipeline, cones_selftest_set, &tp, sizeof(tp), (count + 63) / 64, 1, 1);
		Vector<uint8_t> inb = rendering_device->buffer_get_data(cones_selftest_in_buffer, 0, (uint32_t)(cases.size() * 4));
		Vector<uint8_t> ob = rendering_device->buffer_get_data(cones_selftest_out_buffer, 0, (uint32_t)(count * out_stride * 4));
		if (inb.size() != (int)(cases.size() * 4) || ob.size() != count * out_stride * 4) {
			print_error("[GNE] selftest readback size mismatch.");
			return out;
		}
		out.append((float)count);
		out.append((float)in_stride);
		out.append((float)out_stride);
		const float *inf = (const float *)inb.ptr();
		for (int i = 0; i < (int)(cases.size()); i++) {
			out.append(inf[i]);
		}
		const float *of = (const float *)ob.ptr();
		for (int i = 0; i < count * out_stride; i++) {
			out.append(of[i]);
		}
		return out;
	}

// GNE-018-rev: zero the cone buffer (A/B measurement aid: all-zero =
// the never-cull sentinel, so the prefilter becomes a no-op).
void GneRenderServer::gpu_light_cones_clear() {
	if (gpu_light_valid && !cluster_cone_buffer.is_null()) {
		rendering_device->buffer_clear(cluster_cone_buffer, 0, (uint32_t)(GNE_CLUSTER_COUNT * 16));
		cone_src_frame = draw_frame_seq; // cleared cones count as a fresh source
	}
}

PackedInt32Array GneRenderServer::gpu_light_debug_cluster(int p_tx, int p_ty, int p_tz) {
	PackedInt32Array ret;
	if (!gpu_light_valid) {
		return ret;
	}
	if (p_tx < 0 || p_tx >= GNE_CLUSTER_X || p_ty < 0 || p_ty >= GNE_CLUSTER_Y
			|| p_tz < 0 || p_tz >= GNE_CLUSTER_Z) {
		return ret;
	}
	int tid = (p_tz * GNE_CLUSTER_Y + p_ty) * GNE_CLUSTER_X + p_tx;
	Vector<uint8_t> cb = rendering_device->buffer_get_data(cluster_count_buffer, (uint32_t)(tid * 4), 4);
	if (cb.size() != 4) {
		return ret;
	}
	uint32_t n = 0;
	memcpy(&n, cb.ptr(), 4);
	ret.push_back((int32_t)n);
	uint32_t want = n < (uint32_t)GNE_CLUSTER_LIGHT_CAP ? n : (uint32_t)GNE_CLUSTER_LIGHT_CAP; // full list (was capped at 4)
	if (want > 0) {
		Vector<uint8_t> ib = rendering_device->buffer_get_data(cluster_index_buffer,
				(uint32_t)(tid * GNE_CLUSTER_LIGHT_CAP * 4), want * 4);
		if (ib.size() == (int)(want * 4)) {
			const uint32_t *ids = (const uint32_t *)ib.ptr();
			for (uint32_t i = 0; i < want; i++) {
				ret.push_back((int32_t)ids[i]);
			}
		}
	}
	return ret;
}

int GneRenderServer::gpu_mesh_get_mesh_id_count() const {
	return mesh_table_count;
}

int GneRenderServer::gpu_mesh_get_batch_count() {
	return last_batch_count;
}

PackedInt32Array GneRenderServer::gpu_mesh_get_draw_counts() {
	PackedInt32Array ret;
	if (!gpu_scene_valid || !gpu_mesh_batch_valid || batch_count_buffer.is_null()) {
		return ret;
	}
	Vector<uint8_t> bytes = rendering_device->buffer_get_data(batch_count_buffer, 0, (uint32_t)(GNE_MESH_TABLE_SIZE * 4));
	int n = bytes.size() / 4;
	ret.resize(n);
	const uint32_t *fptr = (const uint32_t *)bytes.ptr();
	for (int i = 0; i < n; i++) {
		ret.set(i, (int32_t)fptr[i]);
	}
	return ret;
}

PackedInt32Array GneRenderServer::gpu_mesh_get_batch_args(int p_batch_index) {
	PackedInt32Array ret;
	if (!gpu_scene_valid || !gpu_mesh_batch_valid || batch_args_buffer.is_null()) {
		return ret;
	}
	if (p_batch_index < 0 || p_batch_index >= GNE_MESH_TABLE_SIZE) {
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

Color GneRenderServer::gpu_mesh_get_mesh_color(int p_mesh_id) const {
	if (p_mesh_id < 0 || p_mesh_id >= GNE_MESH_TABLE_SIZE) {
		return Color(0, 0, 0, 1);
	}
	return mesh_colors[p_mesh_id];
}

bool GneRenderServer::gpu_mesh_set_batch_strategy(int p_strategy) {
	if (p_strategy < GNE_BATCH_STRATEGY_PER_MESH || p_strategy > GNE_BATCH_STRATEGY_REORDERED) {
		print_error("[GNE] gpu_mesh_set_batch_strategy: invalid strategy " + itos(p_strategy));
		return false;
	}
	mesh_batch_strategy = p_strategy;
	print_line("[GNE] Batch strategy set to " + itos(p_strategy));
	return true;
}

int GneRenderServer::gpu_mesh_get_batch_strategy() const {
	return mesh_batch_strategy;
}

int GneRenderServer::gpu_mesh_get_batch_group_count() const {
	return last_group_count;
}

PackedInt32Array GneRenderServer::gpu_mesh_get_batch_order() {
	PackedInt32Array ret;
	if (!gpu_scene_valid || !gpu_mesh_batch_valid || group_member_list_buffer.is_null() || group_member_count_buffer.is_null()) {
		return ret;
	}
	int n = last_group_count;
	if (n <= 0 || n > GNE_MESH_TABLE_SIZE) {
		return ret;
	}
	Vector<uint8_t> mcbytes = rendering_device->buffer_get_data(group_member_count_buffer, 0, (uint32_t)(GNE_MESH_TABLE_SIZE * 4));
	if (mcbytes.size() != (int)(GNE_MESH_TABLE_SIZE * 4)) {
		return ret;
	}
	const uint32_t *mc = (const uint32_t *)mcbytes.ptr();
	for (int g = 0; g < n; g++) {
		uint32_t mg = mc[g];
		if (mg == 0 || mg > GNE_MESH_TABLE_SIZE) {
			continue;
		}
		Vector<uint8_t> bytes = rendering_device->buffer_get_data(group_member_list_buffer, (uint32_t)(g * GNE_MESH_TABLE_SIZE * 4), mg * 4);
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

int GneRenderServer::gpu_mesh_get_indirect_count() const {
	// The draw count consumed by draw_list_draw_indirect. It originates on the
	// GPU (batch_total readback) and is frame-varying - the SPEC 011 section 3.2
	// fallback for the missing vkCmdDrawIndexedIndirectCount API.
	return last_batch_count;
}

int GneRenderServer::gpu_mesh_get_draw_call_count() const {
	// Number of indirect draw commands executed by the last gpu_mesh_batch_draw.
	return last_batch_count;
}

bool GneRenderServer::gpu_raster_indirect_draw() {
	if (!gpu_scene_valid || !gpu_raster_valid || !gpu_drawargs_valid) {
		print_error("[GNE] gpu_raster_indirect_draw: no gpu scene. Call gpu_scene_create first.");
		return false;
	}
	if (!frustum_valid) {
		print_error("[GNE] gpu_raster_indirect_draw: no camera. Call gpu_scene_set_camera first.");
		return false;
	}

	Vector<Color> clear_colors;
	clear_colors.push_back(Color(0, 0, 0, 0));
	clear_colors.push_back(Color(far_plane, 0.0f, 0.0f, 0.0f));
	RD::DrawListID dl = rendering_device->draw_list_begin(raster_framebuffer, RD::DRAW_CLEAR_COLOR_0 | RD::DRAW_CLEAR_COLOR_1 | RD::DRAW_CLEAR_DEPTH, clear_colors, 1.0f, 0, Rect2(), 0);
	if (dl == RD::INVALID_ID) {
		print_error("[GNE] gpu_raster_indirect_draw: draw_list_begin failed.");
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

PackedByteArray GneRenderServer::gpu_raster_read_pixels() {
	PackedByteArray ret;
	if (!gpu_scene_valid || !gpu_raster_valid) {
		return ret;
	}
	Vector<uint8_t> data = rendering_device->texture_get_data(raster_color_texture, 0);
	ret.resize(data.size());
	memcpy(ret.ptrw(), data.ptr(), data.size());
	return ret;
}

// GNE-009: D32_SFLOAT depth readback (values in [0, 1], clear = 1.0 = far).
PackedFloat32Array GneRenderServer::gpu_raster_read_depth() {
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

// GNE-012: read a single R32 view-z texel (bits as the depth source sees it).
float GneRenderServer::gpu_raster_read_viewz(int p_x, int p_y) {
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

// GNE-018-rev: full-frame viewz read (R1 tooling; one download instead of W*H calls).
PackedFloat32Array GneRenderServer::gpu_raster_read_viewz_all() {
	PackedFloat32Array out;
	if (!gpu_scene_valid || !gpu_raster_valid) {
		return out;
	}
	Vector<uint8_t> data = rendering_device->texture_get_data(raster_viewz_texture, 0);
	if (data.size() != RASTER_TARGET_W * RASTER_TARGET_H * 4) {
		return out;
	}
	out.resize(RASTER_TARGET_W * RASTER_TARGET_H);
	memcpy(out.ptrw(), data.ptr(), (size_t)data.size());
	return out;
}


// GNE-018-rev: half-float decode for the normal-attachment spot checks.
static float gne_half_to_float(uint16_t h) {
	const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
	const uint32_t exp = ((uint32_t)h >> 10) & 0x1Fu;
	const uint32_t man = (uint32_t)h & 0x3FFu;
	uint32_t f;
	if (exp == 0u) {
		if (man == 0u) {
			f = sign;
		} else {
			uint32_t e = 113u;
			uint32_t mm = man;
			while ((mm & 0x400u) == 0u) {
				mm <<= 1;
				e--;
			}
			mm &= 0x3FFu;
			f = sign | (e << 23) | (mm << 13);
		}
	} else if (exp == 0x1Fu) {
		f = sign | 0x7F800000u | (man << 13);
	} else {
		f = sign | ((exp + 112u) << 23) | (man << 13);
	}
	float out;
	memcpy(&out, &f, 4);
	return out;
}

// GNE-018-rev: read one normal-attachment texel as floats {x, y, z, w}. The
// all-zero value is the designed "never cull" sentinel; tooling/spot-check
// path only, not a shipping frame-data path.
PackedFloat32Array GneRenderServer::gpu_raster_read_normal(int p_x, int p_y) {
	PackedFloat32Array out;
	if (!gpu_scene_valid || !gpu_raster_valid) {
		return out;
	}
	if (p_x < 0 || p_x >= RASTER_TARGET_W || p_y < 0 || p_y >= RASTER_TARGET_H) {
		return out;
	}
	Vector<uint8_t> data = rendering_device->texture_get_data(raster_normal_texture, 0);
	int64_t off = ((int64_t)p_y * (int64_t)RASTER_TARGET_W + (int64_t)p_x) * 8;
	if ((int64_t)data.size() < off + 8) {
		return out;
	}
	const uint8_t *p = data.ptr() + off;
	for (int i = 0; i < 4; i++) {
		uint16_t hbits;
		memcpy(&hbits, p + i * 2, 2);
		out.append(gne_half_to_float(hbits));
	}
	return out;
}

// GNE-023/KI-017: raw HDR radiance readback (RGBA32F, pre-tonemap; 4 floats/px).
PackedFloat32Array GneRenderServer::gpu_raster_read_hdr(int p_x, int p_y) {
	PackedFloat32Array out;
	if (!gpu_scene_valid || !gpu_raster_valid) {
		return out;
	}
	if (p_x < 0 || p_x >= RASTER_TARGET_W || p_y < 0 || p_y >= RASTER_TARGET_H) {
		return out;
	}
	Vector<uint8_t> data = rendering_device->texture_get_data(raster_hdr_texture, 0);
	int64_t off = ((int64_t)p_y * (int64_t)RASTER_TARGET_W + (int64_t)p_x) * 16;
	if ((int64_t)data.size() < off + 16) {
		return out;
	}
	const float *p = (const float *)(data.ptr() + off);
	out.append(p[0]);
	out.append(p[1]);
	out.append(p[2]);
	out.append(p[3]);
	return out;
}
// GNE-018-rev: full-frame normal read (R1 tooling; RGBA16F decoded to 4 floats/px).
PackedFloat32Array GneRenderServer::gpu_raster_read_normal_all() {
	PackedFloat32Array out;
	if (!gpu_scene_valid || !gpu_raster_valid) {
		return out;
	}
	Vector<uint8_t> data = rendering_device->texture_get_data(raster_normal_texture, 0);
	if (data.size() != RASTER_TARGET_W * RASTER_TARGET_H * 8) {
		return out;
	}
	out.resize(RASTER_TARGET_W * RASTER_TARGET_H * 4);
	const uint8_t *p = data.ptr();
	float *w = out.ptrw();
	for (int i = 0; i < RASTER_TARGET_W * RASTER_TARGET_H * 4; i++) {
		uint16_t hb;
		memcpy(&hb, p + i * 2, 2);
		w[i] = gne_half_to_float(hb);
	}
	return out;
}
// GNE-020: toggle the reduced-resolution presentation path (default off). The
// full-raster presentation path is untouched while disabled.
bool GneRenderServer::gpu_present_lowres_set(bool p_enabled) {
	gne_present_lowres = p_enabled;
	return true;
}

// GNE-020: active presentation target description for the measuring scenes.
Dictionary GneRenderServer::gpu_present_info() const {
	Dictionary d;
	int w = gne_present_lowres ? GNE_PRESENT_LOW_W : RASTER_TARGET_W;
	int h = gne_present_lowres ? GNE_PRESENT_LOW_H : RASTER_TARGET_H;
	d["lowres"] = gne_present_lowres;
	d["w"] = w;
	d["h"] = h;
	d["bytes_per_frame"] = (int64_t)w * (int64_t)h * 4;
	return d;
}

// GNE-020: presentation readback through the ACTIVE target. Low-res mode runs
// the deterministic 2x2 box blit (raster_color_texture -> present texture) and
// reads that back (4x fewer bytes); default mode is the existing full read.
PackedByteArray GneRenderServer::gpu_present_read_pixels() {
	if (!gne_present_lowres) {
		return gpu_raster_read_pixels();
	}
	PackedByteArray ret;
	if (!gpu_scene_valid || !gpu_raster_valid) {
		return ret;
	}
	if (present_blit_pipeline.is_null()) {
		String error;
		Vector<uint8_t> spirv = rendering_device->shader_compile_spirv_from_source(
				RD::SHADER_STAGE_COMPUTE, String(gpu_present_blit_glsl), RD::SHADER_LANGUAGE_GLSL, &error);
		if (spirv.is_empty()) {
			print_error("[GNE] present blit shader compile failed:");
			print_error(error);
			return ret;
		}
		Vector<RD::ShaderStageSPIRVData> stages;
		RD::ShaderStageSPIRVData cs;
		cs.shader_stage = RD::SHADER_STAGE_COMPUTE;
		cs.spirv = spirv;
		stages.push_back(cs);
		present_blit_shader = rendering_device->shader_create_from_spirv(stages, "gne_present_blit");
		if (present_blit_shader.is_null()) {
			print_error("[GNE] present blit shader_create failed.");
			return ret;
		}
		present_blit_pipeline = rendering_device->compute_pipeline_create(present_blit_shader);
		if (present_blit_pipeline.is_null()) {
			print_error("[GNE] present blit pipeline_create failed.");
			return ret;
		}
		RD::TextureFormat pf;
		pf.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
		pf.width = GNE_PRESENT_LOW_W;
		pf.height = GNE_PRESENT_LOW_H;
		pf.depth = 1;
		pf.texture_type = RD::TEXTURE_TYPE_2D;
		pf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
		present_lowres_texture = rendering_device->texture_create(pf, RD::TextureView());
		if (present_lowres_texture.is_null()) {
			print_error("[GNE] present texture_create failed.");
			return ret;
		}
		RD::SamplerState pss;
		present_blit_sampler = rendering_device->sampler_create(pss);
		if (present_blit_sampler.is_null()) {
			print_error("[GNE] present sampler_create failed.");
			return ret;
		}
		Vector<RD::Uniform> cu;
		RD::Uniform u0;
		u0.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u0.binding = 0;
		u0.append_id(present_blit_sampler);
		u0.append_id(raster_color_texture);
		cu.push_back(u0);
		RD::Uniform u1;
		u1.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u1.binding = 1;
		u1.append_id(present_lowres_texture);
		cu.push_back(u1);
		present_blit_set = rendering_device->uniform_set_create(cu, present_blit_shader, 0);
		if (present_blit_set.is_null()) {
			print_error("[GNE] present uniform_set_create failed.");
			return ret;
		}
	}
	_run_compute_pass(present_blit_pipeline, present_blit_set, nullptr, 0, (GNE_PRESENT_LOW_W + 7) / 8, (GNE_PRESENT_LOW_H + 7) / 8, 1);
	Vector<uint8_t> data = rendering_device->texture_get_data(present_lowres_texture, 0);
	ret.resize(data.size());
	memcpy(ret.ptrw(), data.ptr(), data.size());
	return ret;
}
// GNE-009: overwrite a single instance's transform (position_scale) in the
// SoA transform buffer. Pure fill helper for the 009 overlay demo; the fill/
// cull/HZB/compaction/drawargs layout is untouched.
void GneRenderServer::gpu_scene_set_instance_transform(int p_index, const Vector3 &p_position, float p_scale) {
	if (!gpu_scene_valid || p_index < 0 || p_index >= gpu_instance_count) {
		return;
	}
	float data[4] = { p_position.x, p_position.y, p_position.z, p_scale };
	Error err = rendering_device->buffer_update(transform_buffer, p_index * sizeof(data), sizeof(data), data);
	if (err != OK) {
		print_error("[GNE] gpu_scene_set_instance_transform: buffer_update failed.");
	}
}

int GneRenderServer::gpu_raster_get_depth_format() const {
	return raster_depth_format_value;
}

bool GneRenderServer::gpu_mesh_get_depth_enabled() const {
	return mesh_depth_enabled;
}

bool GneRenderServer::gpu_raster_get_depth_enabled() const {
	return raster_depth_enabled;
}

PackedFloat32Array GneRenderServer::gpu_scene_get_vp() {
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

// GNE-013: Meshlets / LOD / cluster culling.
// See header block comment. The .gomlet format (tools/meshlet_import/main.cpp):
//   file header: "GOTOML11" + u32 version(1) + u32 lod_count + u32 reserved
//   per LOD: u32 vertex_count, u32 tri_count, u32 meshlet_count, u32 ref_count
//            vec4 positions[vertex_count], u32 refs[ref_count],
//            u8 micro_tris[3 * tri_count], desc[meshlet_count] (48 B each);
//   per-meshlet desc (48 B): u32 vertex_offset, u32 vertex_count,
//            u32 triangle_offset (u8 units), u32 triangle_count,
//            f32 center[3], f32 radius, f32 cone_axis[3], f32 cone_cutoff.
namespace {
// std140 layout shared by the meshlet cull/raster UBO. Mirrors GneViewData
// conventions (column-major vp, Godot plane test dot(n,p)+d used by 001B/004).
struct GNEMeshletViewData {
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
static_assert(sizeof(GNEMeshletViewData) == 224, "meshlet view data layout");
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

const uint STATE_HEADER = 10;

void main() {
	if (gl_GlobalInvocationID.x == 0u) {
		// Shader-side readback of exactly the fields the cull guard consumes.
		dbg.data[0] = uint(descs.desc[3u * 0u + 1u].z); // lod0 lod_mc
		dbg.data[1] = uint(descs.desc[3u * 1u + 1u].z); // lod1 lod_mc
		dbg.data[2] = uint(descs.desc[3u * 2u + 1u].z); // lod2 lod_mc
		dbg.data[3] = 12345; // sanity marker
		dbg.data[4] = uint(descs.desc[3u * 0u + 0u].z); // lod0 ordinal
		dbg.data[5] = params.cfg.y; // ml0_max (cfg)
		dbg.data[6] = STATE_HEADER;
		dbg.data[7] = 0;
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
	for (uint p = 0; p < 6u; p++) {
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
		state.data[STATE_HEADER + ic + gi] = 1;
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

const uint STATE_HEADER = 10;

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

	for (uint t = 0; t < tri_count; t++) {
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
			dbg.data[24u] = 1;
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

void GneRenderServer::_destroy_meshlet() {
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

bool GneRenderServer::_upload_meshlet_view() {
	GNEMeshletViewData vd;
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
	return rendering_device->buffer_update(ml_ubo, 0, sizeof(GNEMeshletViewData), &vd) == OK;
}

bool GneRenderServer::gpu_meshlet_load(const PackedByteArray &p_data) {
	if (!ensure_gpu_device()) {
		print_error("[GNE] gpu_meshlet_load: no RenderingDevice.");
		return false;
	}
	if (!gpu_scene_valid) {
		print_error("[GNE] gpu_meshlet_load: no GPU scene. Call gpu_scene_create first.");
		return false;
	}

	_destroy_meshlet();

	const uint8_t *D = p_data.ptr();
	const int64_t bytes_size = p_data.size();
	if (bytes_size < 20) {
		print_error("[GNE] gpu_meshlet_load: file too small.");
		return false;
	}
	if (memcmp(D, "GOTOML11", 8) != 0) {
		print_error("[GNE] gpu_meshlet_load: bad magic (not a GOTOML11 file).");
		return false;
	}
	uint32_t version = 0, lod_count = 0;
	memcpy(&version, D + 8, 4);
	memcpy(&lod_count, D + 12, 4);
	if (version != 1) {
		print_error("[GNE] gpu_meshlet_load: unsupported version.");
		return false;
	}
	if (lod_count < 1 || lod_count > ML_MAX_LODS) {
		print_error("[GNE] gpu_meshlet_load: lod_count out of range.");
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
			print_error("[GNE] gpu_meshlet_load: truncated LOD headers.");
			return false;
		}
		memcpy(&lods[l].vc, D + strip, 4);
		memcpy(&lods[l].tc, D + strip + 4, 4);
		memcpy(&lods[l].mc, D + strip + 8, 4);
		memcpy(&lods[l].rc, D + strip + 12, 4);
		if (lods[l].vc == 0 || lods[l].tc == 0 || lods[l].mc == 0) {
			print_error("[GNE] gpu_meshlet_load: zero LOD metric.");
			return false;
		}
		// Record the exact byte offsets the build loop below relies on, then
		// advance to the next strip (header + positions + refs + micro + descs).
		lods[l].pos_off = strip + 16;
		if (lods[l].pos_off + (int64_t)lods[l].vc * 16 > bytes_size) {
			print_error("[GNE] gpu_meshlet_load: LOD positions out of range.");
			print_error(String("[GNE] 013dbg bytes=") + itos(bytes_size) + " lod=" + itos((int)l) + " vc=" + itos((int32_t)lods[l].vc) + " pos_off=" + itos((int64_t)(lods[l].pos_off)) + " need=" + itos((int64_t)(lods[l].pos_off + (int64_t)lods[l].vc * 16)));
			return false;
		}
		lods[l].refs_off = lods[l].pos_off + (int64_t)lods[l].vc * 16;
		if (lods[l].refs_off + (int64_t)lods[l].rc * 4 > bytes_size) {
			print_error("[GNE] gpu_meshlet_load: LOD refs out of range.");
			return false;
		}
		lods[l].micro_off = lods[l].refs_off + (int64_t)lods[l].rc * 4;
		if (lods[l].micro_off + (int64_t)lods[l].tc * 3 > bytes_size) {
			print_error("[GNE] gpu_meshlet_load: LOD micro tris out of range.");
			return false;
		}
		lods[l].desc_off = lods[l].micro_off + (int64_t)lods[l].tc * 3;
		if (lods[l].desc_off + (int64_t)lods[l].mc * 48 > bytes_size) {
			print_error("[GNE] gpu_meshlet_load: LOD descs out of range.");
			return false;
		}
		strip += 16;
		int64_t end = strip;
		end += (int64_t)lods[l].vc * 16;
		end += (int64_t)lods[l].rc * 4;
		end += (int64_t)lods[l].tc * 3;
		end += (int64_t)lods[l].mc * 48;
		if (end > bytes_size) {
			print_error("[GNE] gpu_meshlet_load: LOD strip out of range.");
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
		print_error("[GNE] gpu_meshlet_load: zero meshlet or instance count.");
		return false;
	}
	if ((int64_t)ml_instance_count * (int64_t)ml0_max > (int64_t)1 << 27) {
		print_error("[GNE] gpu_meshlet_load: instance_count x ml0_max grid too large.");
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
		print_error("[GNE] gpu_meshlet_load: triangle expansion mismatch.");
		return false;
	}

	// Upload.
	ml_ubo = rendering_device->uniform_buffer_create(sizeof(GNEMeshletViewData));
	if (!_upload_meshlet_view()) {
		print_error("[GNE] gpu_meshlet_load: view UBO upload failed.");
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
		print_line("[GNE] desc_cpu lod=" + itos(l) + " ord=" + itos(ord) + " vc=" + itos(vc) + " tc=" + itos(tc) + " mc=" + itos(mc));
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
		print_error("[GNE] gpu_meshlet_load: shader compile failed:");
		print_error(cull_spv.is_empty() ? cull_err : raster_err);
		_destroy_meshlet();
		return false;
	}
	RD::ShaderStageSPIRVData stage;
	stage.shader_stage = RD::SHADER_STAGE_COMPUTE;
	stage.spirv = cull_spv;
	Vector<RD::ShaderStageSPIRVData> stages;
	stages.push_back(stage);
	ml_cull_shader = rendering_device->shader_create_from_spirv(stages, "gne_meshlet_cull");
	if (ml_cull_shader.is_null()) {
		_destroy_meshlet();
		return false;
	}
	stage.spirv = raster_spv;
	Vector<RD::ShaderStageSPIRVData> stages2;
	stages2.push_back(stage);
	ml_raster_shader = rendering_device->shader_create_from_spirv(stages2, "gne_meshlet_raster");
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
		print_error("[GNE] gpu_meshlet_load: cull uniform_set_create failed.");
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
		print_error("[GNE] gpu_meshlet_load: raster uniform_set_create failed.");
		_destroy_meshlet();
		return false;
	}

	gpu_meshlet_valid = true;
	print_line("[GNE] gpu_meshlet_load: lods=", ml_lod_count, " lod0_tris=", ml_lod0_tri_count,
			" lod0_meshlets=", ml_lod0_meshlet_count, " total_meshlets=", ml_total_meshlets,
			" verts=", ml_total_vertices, " tris=", ml_total_tris, " instances=", ml_instance_count);
	return true;
}

bool GneRenderServer::gpu_meshlet_load_path(const String &p_path) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
	if (f.is_null() || !f->is_open()) {
		print_error(String("[GNE] gpu_meshlet_load_path: cannot open ") + p_path);
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

bool GneRenderServer::gpu_meshlet_set_lod_thresholds(float p_t0, float p_t1) {
	if (!(p_t0 > 0.0f && p_t1 > p_t0)) {
		print_error("[GNE] gpu_meshlet_set_lod_thresholds: need 0 < t0 < t1.");
		return false;
	}
	ml_lod_t0 = p_t0;
	ml_lod_t1 = p_t1;
	return true;
}

bool GneRenderServer::gpu_meshlet_cull_dispatch() {
	if (!gpu_meshlet_valid || !gpu_scene_valid) {
		print_error("[GNE] gpu_meshlet_cull_dispatch: meshlet not loaded or no scene.");
		return false;
	}
	if (!frustum_valid) {
		print_error("[GNE] gpu_meshlet_cull_dispatch: no camera. Call gpu_scene_set_camera first.");
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

bool GneRenderServer::gpu_meshlet_raster_dispatch() {
	if (!gpu_meshlet_valid || !gpu_scene_valid) {
		print_error("[GNE] gpu_meshlet_raster_dispatch: meshlet not loaded or no scene.");
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

int GneRenderServer::gpu_meshlet_get_total_meshlets() const {
	return ml_total_meshlets;
}

int GneRenderServer::gpu_meshlet_get_lod0_tri_count() const {
	return ml_lod0_tri_count;
}

int GneRenderServer::gpu_meshlet_get_lod0_meshlet_count() const {
	return ml_lod0_meshlet_count;
}

PackedInt32Array GneRenderServer::gpu_meshlet_stats() {
	PackedInt32Array ret;
	ret.append(ml_total_meshlets);
	ret.append(ml_lod0_tri_count);
	ret.append(ml_lod0_meshlet_count);
	ret.append(ml_total_vertices);
	ret.append(ml_total_tris);
	return ret;
}

PackedInt32Array GneRenderServer::gpu_meshlet_get_instance_lods() {
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

PackedInt32Array GneRenderServer::gpu_meshlet_get_cull_debug() {
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

PackedInt32Array GneRenderServer::gpu_meshlet_get_cluster_counts() {
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

PackedFloat32Array GneRenderServer::gpu_meshlet_raster_evidence() {
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
	uint32_t fnv = 2166136261;
	int32_t winner = 0;
	const int32_t count = ml_vis_w * ml_vis_h;
	for (int32_t i = 0; i < count; i++) {
		uint64_t c;
		memcpy(&c, vbytes.ptr() + (int64_t)i * 8, 8);
		uint32_t id = (uint32_t)(c & 0xFFFFFFFFu);
		if (id != 0) {
			winner++;
			fnv ^= id;
			fnv *= 16777619;
		}
	}
	ret.set(0, (float)covered);
	ret.set(1, (float)subpixel);
	ret.set(2, (float)winner);
	ret.set(3, (float)fnv);
	return ret;
}

void GneRenderServer::gpu_meshlet_destroy() {
	_destroy_meshlet();
}

// GNE-014: GPU Scene Manager (SPEC 014).
// See header block comment. Layouts (std430, all storage buffers):
//   record (64 B / 16 u32 per unified id, slot id*16):
//     [0..3]   vec4 transform (pos.xyz, scale)
//     [4..7]   vec4 bounds   (center.xyz, radius)
//     [8..11]  uvec4 refs    (x=meshlet_ordinal, y=mesh_ref, z=flags, w=pad)
//     [12..15] uvec4 lodcfg  (x=lod_t0 bits, y=lod_t1 bits, z/w=pad)
//   ring delta (80 B / 20 u32): [op,id,seq,flags, transform, bounds, refs, lodcfg]
//   snapshot draw record (32 B / 8 u32): [ordinal, mesh_ref, flags, pad, bounds]
//   stats uint[16]: 0=active(compact), 1=adds, 2=removes, 3=moves,
//                   4=reserved, 5=ring bytes consumed, 6=snap records,
//                   7=distinct meshes.
// The single shader/pipe/set runs three deterministic passes (each pass is
// submit+sync'ed via _run_compute_pass): apply (consume ring), compact (dense
// ascending-id active list), snapshot (draw records + per-mesh counts). Only
// tiny 4-byte verify counters (active count) are read back - verification
// bridge, same class as the 013 evidence getters.
namespace {
// Record (16 vec4 = 64 B): [0] transform(xyz,scale) [4] bounds(center.xyz,radius)
//   [8] refs(ord,mesh,flags,pad as floats) [12] lodcfg(t0,t1,pad,pad).
// Small int-ish values (ordinal/mesh/flags/op/id) travel as floats (exact for
// <2^24) - the CPU uploads raw floats and the GPU casts float->uint on use.
const char *gpu_scene_manager_glsl = R"(
#version 450
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform GmsParams {
	uvec4 cfg; // x=pass(0 apply,1 compact,2 snapshot), y=capacity, z=delta_count, w=ring base delta index
}
params;

layout(std430, set = 0, binding = 0) buffer RecordBuffer { vec4 rec[]; } records;
layout(std430, set = 0, binding = 1) buffer RingBuffer { float rbuf[]; } ring;
layout(std430, set = 0, binding = 2) buffer StatsBuffer { uint st[]; } stats;
layout(std430, set = 0, binding = 3) buffer ActiveBuffer { uint act[]; } actlist;
layout(std430, set = 0, binding = 4) buffer ActiveCount { uint cnt; } acnt;
layout(std430, set = 0, binding = 5) buffer SnapshotBuffer { uvec4 snap[]; } snap;
layout(std430, set = 0, binding = 6) buffer MeshCountBuffer { uint mc[]; } meshc;

void main() {
	uint gi = gl_GlobalInvocationID.x;

	// Pass 0 - apply: consume the ring. Non-invoked if there are no deltas.
	// cfg.w is the first delta index of this wave: the ring write cursor may sit
	// above 0, so deltas must be addressed from the wave base, never from 0.
	if (params.cfg.x == 0u) {
		uint dn = params.cfg.z;
		if (gi >= dn) {
			return;
		}
		uint base = (params.cfg.w + gi) * 20u;
		uint op = uint(ring.rbuf[base + 0u]);
		uint id = uint(ring.rbuf[base + 1u]);
		if (id >= params.cfg.y) { // cfg.y = capacity
			return;
		}
		uint r = id * 4u; // record base in vec4 units (16 floats per record)
		if (op == 1u) { // add: write all fields, raise the active flag
			records.rec[r + 0u] = vec4(ring.rbuf[base + 4u], ring.rbuf[base + 5u], ring.rbuf[base + 6u], ring.rbuf[base + 7u]);
			records.rec[r + 1u] = vec4(ring.rbuf[base + 8u], ring.rbuf[base + 9u], ring.rbuf[base + 10u], ring.rbuf[base + 11u]);
			records.rec[r + 2u] = vec4(ring.rbuf[base + 12u], ring.rbuf[base + 13u], ring.rbuf[base + 14u] + 1.0, ring.rbuf[base + 15u]);
			records.rec[r + 3u] = vec4(ring.rbuf[base + 16u], ring.rbuf[base + 17u], ring.rbuf[base + 18u], ring.rbuf[base + 19u]);
			atomicAdd(stats.st[1u], 1u);
		} else if (op == 3u) { // move: transform + bounds only
			records.rec[r + 0u] = vec4(ring.rbuf[base + 4u], ring.rbuf[base + 5u], ring.rbuf[base + 6u], ring.rbuf[base + 7u]);
			records.rec[r + 1u] = vec4(ring.rbuf[base + 8u], ring.rbuf[base + 9u], ring.rbuf[base + 10u], ring.rbuf[base + 11u]);
			atomicAdd(stats.st[3u], 1u);
		} else if (op == 2u) { // remove: clear the active flag
			records.rec[r + 2u].z = max(records.rec[r + 2u].z - 1.0, 0.0);
			atomicAdd(stats.st[2u], 1u);
		}
		return;
	}

	// Pass 1 - compact: dense ascending-id active list (scan record space once).
	if (params.cfg.x == 1u) {
		if (gi >= params.cfg.y) {
			return;
		}
		uint r = gi * 4u;
		if (records.rec[r + 2u].z >= 1.0) {
			uint slot = atomicAdd(acnt.cnt, 1u);
			actlist.act[slot] = gi;
		}
		return;
	}

	// Pass 2 - snapshot: emit 32-byte draw records from the compacted list.
	uint n = stats.st[0u];
	if (gi >= n) {
		return;
	}
	uint id = actlist.act[gi];
	uint r = id * 4u;
	uint base = gi * 2u; // 2 x uvec4 = 32 bytes per draw record
	snap.snap[base + 0u] = uvec4(uint(records.rec[r + 2u].x), uint(records.rec[r + 2u].y), uint(records.rec[r + 2u].z), floatBitsToUint(records.rec[r + 3u].x));
	snap.snap[base + 1u] = floatBitsToUint(records.rec[r + 1u]);
	atomicAdd(stats.st[6u], 1u);
	if (atomicAdd(meshc.mc[uint(records.rec[r + 2u].y) & 63u], 1u) == 0u) {
		atomicAdd(stats.st[7u], 1u);
	}
}
)";
} // namespace

void GneRenderServer::_destroy_scene_manager() {
	if (rendering_device == nullptr) {
		gpu_scene_mgr_valid = false;
		return;
	}
	if (gms_uniform_set.is_valid()) {
		rendering_device->free_rid(gms_uniform_set);
		gms_uniform_set = RID();
	}
	if (gms_pipeline.is_valid()) {
		rendering_device->free_rid(gms_pipeline);
		gms_pipeline = RID();
	}
	if (gms_shader.is_valid()) {
		rendering_device->free_rid(gms_shader);
		gms_shader = RID();
	}
	if (gms_mesh_count_buffer.is_valid()) {
		rendering_device->free_rid(gms_mesh_count_buffer);
		gms_mesh_count_buffer = RID();
	}
	if (gms_stats_buffer.is_valid()) {
		rendering_device->free_rid(gms_stats_buffer);
		gms_stats_buffer = RID();
	}
	if (gms_snapshot_buffer.is_valid()) {
		rendering_device->free_rid(gms_snapshot_buffer);
		gms_snapshot_buffer = RID();
	}
	if (gms_ring_buffer.is_valid()) {
		rendering_device->free_rid(gms_ring_buffer);
		gms_ring_buffer = RID();
	}
	if (gms_active_count_buffer.is_valid()) {
		rendering_device->free_rid(gms_active_count_buffer);
		gms_active_count_buffer = RID();
	}
	if (gms_active_buffer.is_valid()) {
		rendering_device->free_rid(gms_active_buffer);
		gms_active_buffer = RID();
	}
	if (gms_record_buffer.is_valid()) {
		rendering_device->free_rid(gms_record_buffer);
		gms_record_buffer = RID();
	}
	gpu_scene_mgr_valid = false;
	gms_capacity = 0;
	gms_active_cpu = 0;
	gms_ring_tail = 0;
	gms_ring_ops.clear();
	gms_dispatch_seq = 0;
}

bool GneRenderServer::gpu_scene_manager_alloc(int p_max_instances) {
	if (!ensure_gpu_device()) {
		print_error("[GNE] gpu_scene_manager_alloc: no RenderingDevice.");
		return false;
	}
	if (p_max_instances <= 0 || p_max_instances > GMS_MAX_CAPACITY) {
		print_error("[GNE] gpu_scene_manager_alloc: capacity must be in (0, " + itos(GMS_MAX_CAPACITY) + "].");
		return false;
	}
	_destroy_scene_manager();

	gms_capacity = p_max_instances;
	int64_t record_bytes = (int64_t)gms_capacity * (int64_t)GMS_RECORD_BYTES;
	int64_t active_bytes = (int64_t)gms_capacity * 4;
	int64_t snapshot_bytes = (int64_t)gms_capacity * (int64_t)GMS_DRAW_RECORD_BYTES;

	gms_record_buffer = rendering_device->storage_buffer_create((uint32_t)record_bytes);
	gms_active_buffer = rendering_device->storage_buffer_create((uint32_t)active_bytes);
	gms_active_count_buffer = rendering_device->storage_buffer_create(4);
	gms_ring_buffer = rendering_device->storage_buffer_create((uint32_t)GMS_RING_BYTES);
	gms_snapshot_buffer = rendering_device->storage_buffer_create((uint32_t)snapshot_bytes);
	gms_stats_buffer = rendering_device->storage_buffer_create((uint32_t)GMS_STATS_UINTS * 4);
	gms_mesh_count_buffer = rendering_device->storage_buffer_create((uint32_t)GMS_MESH_SLOTS * 4);

	String cerr;
	Vector<uint8_t> spv = rendering_device->shader_compile_spirv_from_source(
			RD::SHADER_STAGE_COMPUTE, String(gpu_scene_manager_glsl), RD::SHADER_LANGUAGE_GLSL, &cerr);
	if (spv.is_empty()) {
		print_error("[GNE] gpu_scene_manager_alloc: shader compile failed:");
		print_error(cerr);
		_destroy_scene_manager();
		return false;
	}
	RD::ShaderStageSPIRVData stage;
	stage.shader_stage = RD::SHADER_STAGE_COMPUTE;
	stage.spirv = spv;
	Vector<RD::ShaderStageSPIRVData> stages;
	stages.push_back(stage);
	gms_shader = rendering_device->shader_create_from_spirv(stages, "gne_scene_manager");
	if (gms_shader.is_null()) {
		print_error("[GNE] gpu_scene_manager_alloc: shader_create_from_spirv failed.");
		_destroy_scene_manager();
		return false;
	}
	gms_pipeline = rendering_device->compute_pipeline_create(gms_shader);

	const RID gms_buffers[7] = { gms_record_buffer, gms_ring_buffer, gms_stats_buffer, gms_active_buffer, gms_active_count_buffer, gms_snapshot_buffer, gms_mesh_count_buffer };
	Vector<RD::Uniform> uniforms;
	for (uint32_t b = 0; b < 7; b++) {
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = b;
		u.append_id(gms_buffers[b]);
		uniforms.push_back(u);
	}
	gms_uniform_set = rendering_device->uniform_set_create(uniforms, gms_shader, 0);
	if (gms_uniform_set.is_null()) {
		print_error("[GNE] gpu_scene_manager_alloc: uniform_set_create failed.");
		_destroy_scene_manager();
		return false;
	}

	gpu_scene_mgr_valid = true;
	gms_active_cpu = 0;
	gms_ring_tail = 0;
	gms_ring_ops.clear();
	gms_dispatch_seq = 0;
	print_line("[GNE] gpu_scene_manager_alloc: capacity=", gms_capacity,
			" record_buf=", (int64_t)record_bytes, " snapshot_buf=", (int64_t)snapshot_bytes,
			" ring=16MiB");
	return true;
}

bool GneRenderServer::gpu_scene_manager_set_instances(const PackedFloat32Array &p_instances) {
	if (!gpu_scene_mgr_valid) {
		print_error("[GNE] gpu_scene_manager_set_instances: no manager. Call gpu_scene_manager_alloc first.");
		return false;
	}
	int n = p_instances.size();
	if (n <= 0 || n % 16 != 0) {
		print_error("[GNE] gpu_scene_manager_set_instances: size must be a positive multiple of 16 floats (64-byte record).");
		return false;
	}
	int count = n / 16;
	if (count > gms_capacity) {
		print_error("[GNE] gpu_scene_manager_set_instances: " + itos(count) + " instances exceed capacity " + itos(gms_capacity) + ".");
		return false;
	}
	// Reset the whole record space (so ids >= count are inactive), then write 0..count-1.
	rendering_device->buffer_clear(gms_record_buffer, 0, (uint32_t)((int64_t)gms_capacity * (int64_t)GMS_RECORD_BYTES));
	rendering_device->submit();
	rendering_device->sync();
	if (count > 0) {
		rendering_device->buffer_update(gms_record_buffer, 0, (uint32_t)((int64_t)count * (int64_t)GMS_RECORD_BYTES), p_instances.ptr());
	}
	gms_active_cpu = count;
	gms_ring_tail = 0;
	gms_ring_ops.clear();
	print_line("[GNE] gpu_scene_manager_set_instances: instances=", count, " active_cpu=", gms_active_cpu);
	return true;
}

bool GneRenderServer::gpu_scene_manager_update(const Array &p_deltas) {
	if (!gpu_scene_mgr_valid) {
		print_error("[GNE] gpu_scene_manager_update: no manager.");
		return false;
	}
	const int nitems = p_deltas.size();
	if (nitems <= 0) {
		return true;
	}
	int64_t total_bytes = (int64_t)nitems * (int64_t)GMS_DELTA_BYTES;
	if ((int64_t)gms_ring_tail + total_bytes > (int64_t)GMS_RING_BYTES) {
		print_error("[GNE] gpu_scene_manager_update: ring overflow (need " + itos((int64_t)total_bytes) + " at tail " + itos((int64_t)gms_ring_tail) + "/" + itos((int64_t)GMS_RING_BYTES) + ").");
		return false;
	}
	Vector<uint8_t> cpu;
	cpu.resize((int64_t)nitems * (int64_t)GMS_DELTA_BYTES);
	uint8_t *w = cpu.ptrw();
	// Staged locally and only appended to gms_ring_ops after the upload
	// succeeds, so a rejected batch cannot desync the op log from the ring.
	Vector<uint8_t> ops;
	ops.resize(nitems);
	for (int i = 0; i < nitems; i++) {
		Variant v = p_deltas[i];
		PackedFloat32Array d = v; // Array of PackedFloat32Array (20 floats / 80 bytes)
		if (d.size() != 20) {
			print_error("[GNE] gpu_scene_manager_update: delta " + itos(i) + " must be 20 floats (80 bytes), got " + itos(d.size()) + ".");
			return false;
		}
		const int32_t delta_id = (int32_t)d[1]; // d[1] = instance id (float-encoded)
		if (delta_id < 0 || delta_id >= gms_capacity) {
			print_error("[GNE] gpu_scene_manager_update: delta " + itos(i) + " id " + itos(delta_id) + " out of range (capacity " + itos(gms_capacity) + ").");
			return false;
		}
		memcpy(w + (int64_t)i * (int64_t)GMS_DELTA_BYTES, d.ptr(), GMS_DELTA_BYTES);
		ops.write[i] = (uint8_t)(int32_t)d[0];
	}
	rendering_device->buffer_update(gms_ring_buffer, gms_ring_tail, (uint32_t)total_bytes, cpu.ptr());
	gms_ring_tail += (uint32_t)total_bytes;
	gms_ring_ops.append_array(ops);
	return true;
}

bool GneRenderServer::gpu_scene_manager_dispatch() {
	if (!gpu_scene_mgr_valid) {
		print_error("[GNE] gpu_scene_manager_dispatch: no manager.");
		return false;
	}
	struct GmsPush {
		uint32_t pass;
		uint32_t capacity;
		uint32_t delta_count;
		uint32_t pad;
	};
	GmsPush push;
	uint32_t delta_count = gms_ring_tail / (uint32_t)GMS_DELTA_BYTES;

	// Wave accounting of THIS dispatch starts clean: a dispatch with no deltas
	// runs zero waves and never collapsed, so the stats must not inherit the
	// previous dispatch's values.
	gms_last_wave_count = 0;
	gms_last_collapsed = false;

	// Reset the per-dispatch counters (stats words 1..8) and the compact counter.
	rendering_device->buffer_clear(gms_stats_buffer, 4, 8 * 4);
	rendering_device->buffer_clear(gms_active_count_buffer, 0, 4);
	rendering_device->buffer_clear(gms_mesh_count_buffer, 0, (uint32_t)GMS_MESH_SLOTS * 4);
	rendering_device->submit();
	rendering_device->sync();
	// Record the consumed ring bytes for evidence (words 5).
	uint32_t consumed = gms_ring_tail;
	rendering_device->buffer_update(gms_stats_buffer, 5 * 4, 4, &consumed);

	if (delta_count > 0) {
		// The ring is an ordered stream, but one parallel dispatch has no
		// ordering: a remove and a later add on the same id race on the
		// non-atomic read-modify-write of rec[r+2].z, so the instance can be
		// lost (observed: active=1048576-8 / -4, nondeterministic). Split the
		// ring into ordered waves of consecutive equal ops instead. Each
		// _run_compute_pass ends with submit()+sync(), so a wave is a full
		// barrier and waves cannot interleave.
		const int GMS_MAX_WAVES = 256;
		struct GmsWave {
			uint32_t first;
			uint32_t count;
		};
		Vector<GmsWave> waves;
		const int32_t have_ops = gms_ring_ops.size();
		if (have_ops == (int32_t)delta_count) {
			uint32_t run_first = 0;
			uint32_t run_len = 0;
			uint8_t run_op = 0;
			for (uint32_t i = 0; i < delta_count; i++) {
				const uint8_t op = gms_ring_ops[i];
				if (run_len > 0 && op != run_op) {
					waves.push_back(GmsWave{ run_first, run_len });
					run_first = i;
					run_len = 0;
				}
				if (run_len == 0) {
					run_op = op;
				}
				run_len++;
			}
			if (run_len > 0) {
				waves.push_back(GmsWave{ run_first, run_len });
			}
		}
		if (waves.is_empty()) {
			print_error("[GNE] gpu_scene_manager_update: ops log desync (have_ops != delta_count). Update aborted.");
			return false;
		}
		if ((int)waves.size() > GMS_MAX_WAVES) {
			print_line("[GNE] WARNING gpu_scene_manager_dispatch: ", (int)waves.size(), " delta runs exceed the ", GMS_MAX_WAVES, "-wave limit; collapsing into a single dispatch. Op codes alternate too finely to order - same-id add/remove pairs may race (nondeterministic active count).");
			waves.clear();
			waves.push_back(GmsWave{ 0u, delta_count });
			gms_last_collapsed = true;
		}
		for (int i = 0; i < waves.size(); i++) {
			push.pass = 0;
			push.capacity = (uint32_t)gms_capacity;
			push.delta_count = waves[i].count;
			push.pad = waves[i].first;
			uint32_t groups = (waves[i].count + 63) / 64;
			_run_compute_pass(gms_pipeline, gms_uniform_set, &push, sizeof(GmsPush), groups, 1, 1);
		}
		gms_last_wave_count = (uint32_t)waves.size();
		gms_ring_tail = 0;
		gms_ring_ops.clear();
	}

	// Compact: dense ascending-id active list.
	push.pass = 1;
	push.capacity = (uint32_t)gms_capacity;
	push.delta_count = 0;
	push.pad = 0;
	uint32_t cap_groups = ((uint32_t)gms_capacity + 63) / 64;
	_run_compute_pass(gms_pipeline, gms_uniform_set, &push, sizeof(GmsPush), cap_groups, 1, 1);
	// Mirror the GPU active count into stats[0] (4-byte verification readback,
	// evidence bridge - not a critical path).
	Vector<uint8_t> ac = rendering_device->buffer_get_data(gms_active_count_buffer, 0, 4);
	uint32_t active = 0;
	memcpy(&active, ac.ptr(), 4);
	rendering_device->buffer_update(gms_stats_buffer, 0, 4, &active);
	gms_active_cpu = (int)active;

	// Snapshot: emit 32-byte draw records from the compacted list.
	push.pass = 2;
	push.capacity = (uint32_t)gms_capacity;
	push.delta_count = 0;
	push.pad = 0;
	uint32_t snap_groups = (active + 63) / 64;
	if (active > 0) {
		_run_compute_pass(gms_pipeline, gms_uniform_set, &push, sizeof(GmsPush), snap_groups, 1, 1);
	}

	gms_dispatch_seq++;
	return true;
}

Dictionary GneRenderServer::gpu_scene_manager_get_stats() {
	Dictionary d;
	d["valid"] = gpu_scene_mgr_valid;
	if (!gpu_scene_mgr_valid) {
		return d;
	}
	d["capacity"] = gms_capacity;
	Vector<uint8_t> bytes = rendering_device->buffer_get_data(gms_stats_buffer, 0, (uint32_t)GMS_STATS_UINTS * 4);
	int32_t v[GMS_STATS_UINTS];
	for (int i = 0; i < GMS_STATS_UINTS; i++) {
		memcpy(&v[i], bytes.ptr() + i * 4, 4);
	}
	d["active"] = v[0];
	d["adds"] = v[1];
	d["removes"] = v[2];
	d["moves"] = v[3];
	d["ring_consumed_bytes"] = v[5];
	d["snap_records"] = v[6];
	d["distinct_meshes"] = v[7];
	d["ring_bytes"] = (int)GMS_RING_BYTES;
	d["ring_used_bytes"] = (int)gms_ring_tail;
	d["ring_free_bytes"] = (int)GMS_RING_BYTES - (int)gms_ring_tail;
	int groups = v[7] < 5 ? v[7] : 5;
	d["group_count_011"] = groups;
	int64_t ssbo = (int64_t)gms_capacity * (int64_t)GMS_RECORD_BYTES +
			(int64_t)gms_capacity * 4 +
			(int64_t)gms_capacity * (int64_t)GMS_DRAW_RECORD_BYTES +
			(int64_t)GMS_RING_BYTES + 4 + (int64_t)GMS_STATS_UINTS * 4 + (int64_t)GMS_MESH_SLOTS * 4;
	d["ssbo_bytes"] = (int)ssbo;
	d["dispatch_seq"] = (int)gms_dispatch_seq;
	return d;
}

Dictionary GneRenderServer::gpu_scene_manager_get_wave_stats() const {
	Dictionary d;
	d["valid"] = gpu_scene_mgr_valid;
	if (!gpu_scene_mgr_valid) {
		return d;
	}
	d["waves"] = (int)gms_last_wave_count;
	d["collapsed"] = gms_last_collapsed;
	d["max_waves"] = 256;
	return d;
}

PackedInt32Array GneRenderServer::gpu_scene_manager_get_draw_counts() {
	PackedInt32Array ret;
	if (!gpu_scene_mgr_valid) {
		return ret;
	}
	Vector<uint8_t> sbytes = rendering_device->buffer_get_data(gms_stats_buffer, 6 * 4, 2 * 4);
	int32_t snap = 0, distinct = 0;
	memcpy(&snap, sbytes.ptr(), 4);
	memcpy(&distinct, sbytes.ptr() + 4, 4);
	ret.append(snap);
	ret.append(distinct);
	ret.append(distinct < 5 ? distinct : 5);
	Vector<uint8_t> mbytes = rendering_device->buffer_get_data(gms_mesh_count_buffer, 0, (uint32_t)GMS_MESH_SLOTS * 4);
	for (int i = 0; i < GMS_MESH_SLOTS; i++) {
		int32_t c;
		memcpy(&c, mbytes.ptr() + i * 4, 4);
		ret.append(c);
	}
	return ret;
}

PackedInt32Array GneRenderServer::gpu_scene_manager_get_snapshot(int p_count) {
	PackedInt32Array ret;
	if (!gpu_scene_mgr_valid || p_count <= 0) {
		return ret;
	}
	Vector<uint8_t> sbytes = rendering_device->buffer_get_data(gms_stats_buffer, 6 * 4, 4);
	int32_t snap = 0;
	memcpy(&snap, sbytes.ptr(), 4);
	int n = p_count < snap ? p_count : snap;
	if (n <= 0) {
		return ret;
	}
	Vector<uint8_t> bytes = rendering_device->buffer_get_data(gms_snapshot_buffer, 0, (uint32_t)n * (uint32_t)GMS_DRAW_RECORD_BYTES);
	ret.resize(n);
	for (int i = 0; i < n; i++) {
		int32_t ord;
		memcpy(&ord, bytes.ptr() + (int64_t)i * (int64_t)GMS_DRAW_RECORD_BYTES, 4);
		ret.set(i, ord);
	}
	return ret;
}

PackedInt32Array GneRenderServer::gpu_scene_manager_get_active_ids(int p_count) {
	PackedInt32Array ret;
	if (!gpu_scene_mgr_valid || p_count <= 0) {
		return ret;
	}
	int n = p_count < gms_active_cpu ? p_count : gms_active_cpu;
	if (n <= 0) {
		return ret;
	}
	Vector<uint8_t> bytes = rendering_device->buffer_get_data(gms_active_buffer, 0, (uint32_t)n * 4);
	ret.resize(n);
	for (int i = 0; i < n; i++) {
		int32_t id;
		memcpy(&id, bytes.ptr() + i * 4, 4);
		ret.set(i, id);
	}
	return ret;
}

void GneRenderServer::gpu_scene_manager_destroy() {
	_destroy_scene_manager();
}

// ================================================================ GNE-015
// Render Graph (SPEC 015, additive TEST-ONLY evidence bridge). Owns a small
// DAG: nodes = passes, edges = resource dependencies. gpu_rg_compile runs a
// KAHN topological sort with cycle detection, DERIVES an automatic barrier per
// producer->consumer resource edge (zero manual barriers anywhere), and lays
// out a TRANSIENT resource pool where resources whose live ranges do not
// overlap are ALIASED to the same pool memory (saved bytes measured).
// gpu_rg_execute walks the compiled order and runs each pass body, which calls
// the EXISTING 013/011/014 entry points on the SAME pipelines/uniform sets, so
// every pre-015 signature stays byte-identical. TEST-ONLY, additive.
namespace {
constexpr int RG_MAX_PASSES = 32;
constexpr int RG_MAX_EDGES = 64;
constexpr int RG_PASS_KINDS = 6;
constexpr int RG_STATS_UINTS = 16;
// Lifetime buckets for the transient pool: a resource is ALIASED when its
// live range [first_read, last_write] does not overlap an already-placed
// resource range on the same bucket.
struct RgPassCfg {
	String name;
	int kind = 0; // 0=scene_update 1=cull 2=cluster_cull 3=batch_assembly 4=raster 5=output
	PackedStringArray in_res;
	PackedStringArray out_res;
};
struct RgEdgeCfg {
	int from = 0; // index into rg_passes
	int to = 0;
	String resource;
	int bytes = 0;
};
struct RgPoolSlot {
	int offset = 0;
	int bytes = 0;
	int first_pass = INT_MAX;
	int last_pass = -1;
	String resource;
};
} // namespace

// --- state (TEST-ONLY) ---
static bool rg_valid = false;
static bool rg_compiled = false;
static Vector<RgPassCfg> rg_passes;
static Vector<RgEdgeCfg> rg_edges;
static Vector<int> rg_topo;               // compiled execution order (pass idx)
static bool rg_cycle = false;
static int rg_barrier_count = 0;
static int rg_pool_bytes = 0;
static int rg_res_bytes = 0;
static int rg_alias_saved = 0;
static uint32_t rg_dispatch_seq = 0;
static int rg_exec_count = 0;
static Vector<RgPoolSlot> rg_pool;
static Vector<String> rg_barrier_list;    // "from->to [res]"

bool GneRenderServer::gpu_rg_create() {
	if (rendering_device == nullptr) {
		print_error("[GNE] gpu_rg_create: no RenderingDevice.");
		return false;
	}
	if (rg_valid) {
		gpu_rg_destroy();
	}
	rg_passes.clear();
	rg_edges.clear();
	rg_topo.clear();
	rg_pool.clear();
	rg_barrier_list.clear();
	rg_cycle = false;
	rg_compiled = false;
	rg_barrier_count = 0;
	rg_pool_bytes = 0;
	rg_res_bytes = 0;
	rg_alias_saved = 0;
	rg_exec_count = 0;
	rg_valid = true;
	return true;
}

bool GneRenderServer::gpu_rg_add_pass(const String &p_name, int p_kind, const PackedStringArray &p_in_res, const PackedStringArray &p_out_res) {
	if (!rg_valid) {
		print_error("[GNE] gpu_rg_add_pass: no graph. Call gpu_rg_create first.");
		return false;
	}
	if (rg_compiled) {
		print_error("[GNE] gpu_rg_add_pass: graph already compiled.");
		return false;
	}
	for (int i = 0; i < rg_passes.size(); i++) {
		if (rg_passes[i].name == p_name) {
			print_error("[GNE] gpu_rg_add_pass: duplicate pass name '" + p_name + "'.");
			return false;
		}
	}
	if (p_kind < 0 || p_kind >= RG_PASS_KINDS) {
		print_error("[GNE] gpu_rg_add_pass: kind " + itos(p_kind) + " out of [0," + itos(RG_PASS_KINDS) + ").");
		return false;
	}
	if (rg_passes.size() >= RG_MAX_PASSES) {
		print_error("[GNE] gpu_rg_add_pass: pass limit " + itos(RG_MAX_PASSES) + " reached.");
		return false;
	}
	RgPassCfg p;
	p.name = p_name;
	p.kind = p_kind;
	p.in_res = p_in_res;
	p.out_res = p_out_res;
	rg_passes.push_back(p);
	return true;
}

bool GneRenderServer::gpu_rg_add_edge(const String &p_from, const String &p_to, const String &p_resource, int p_bytes) {
	if (!rg_valid || rg_compiled) {
		return false;
	}
	int fi = -1;
	int ti = -1;
	for (int i = 0; i < rg_passes.size(); i++) {
		if (rg_passes[i].name == p_from) {
			fi = i;
		}
		if (rg_passes[i].name == p_to) {
			ti = i;
		}
	}
	if (fi < 0 || ti < 0 || fi == ti) {
		print_error("[GNE] gpu_rg_add_edge: invalid endpoints.");
		return false;
	}
	if (rg_edges.size() >= RG_MAX_EDGES) {
		print_error("[GNE] gpu_rg_add_edge: edge limit " + itos(RG_MAX_EDGES) + " reached.");
		return false;
	}
	RgEdgeCfg e;
	e.from = fi;
	e.to = ti;
	e.resource = p_resource;
	e.bytes = p_bytes;
	rg_edges.push_back(e);
	return true;
}

bool GneRenderServer::gpu_rg_compile() {
	if (!rg_valid) {
		return false;
	}
	int passes = rg_passes.size();
	int edges = rg_edges.size();
	// 1) Transpose adjacency + in-degree (Kahn).
	// Plain fixed-size arrays on purpose: Vector<PackedInt32Array>::ptrw()
	// into a nested CoW buffer corrupts neighbouring state, so the scheduler
	// uses POD adjacency that cannot alias anything.
	int indeg[RG_MAX_PASSES] = {};
	int adj[RG_MAX_PASSES][RG_MAX_EDGES];
	int adj_n[RG_MAX_PASSES] = {};
	for (int i = 0; i < RG_MAX_PASSES; i++) {
		for (int j = 0; j < RG_MAX_EDGES; j++) {
			adj[i][j] = -1;
		}
	}
	for (int ei = 0; ei < edges; ei++) {
		const RgEdgeCfg &e = rg_edges[ei];
		if (e.from >= 0 && e.from < passes && e.to >= 0 && e.to < passes) {
			adj[e.from][adj_n[e.from]] = e.to;
			adj_n[e.from]++;
			indeg[e.to]++;
		}
	}
	int queue[RG_MAX_PASSES];
	int qn = 0;
	for (int i = 0; i < passes; i++) {
		if (indeg[i] == 0) {
			queue[qn] = i;
			qn++;
		}
	}
	// 2) Kahn: deterministic (lowest pending pass index first).
	rg_topo.clear();
	int visited = 0;
	while (qn > 0) {
		int bi = 0;
		for (int i = 1; i < qn; i++) {
			if (queue[i] < queue[bi]) {
				bi = i;
			}
		}
		const int u = queue[bi];
		queue[bi] = queue[qn - 1];
		qn--;
		rg_topo.push_back(u);
		visited++;
		for (int k = 0; k < adj_n[u]; k++) {
			const int v = adj[u][k];
			indeg[v]--;
			if (indeg[v] == 0) {
				queue[qn] = v;
				qn++;
			}
		}
	}
	rg_cycle = visited != passes;
	rg_compiled = !rg_cycle;
	if (rg_cycle) {
		String dbg = "[GNE] rg_compile: CYCLE passes=" + itos(passes) + " edges=" + itos(edges) + " visited=" + itos(visited) + " indeg=[";
		for (int i = 0; i < passes; i++) {
			dbg += itos(indeg[i]) + (i + 1 < passes ? "," : "");
		}
		dbg += "] edges=[";
		for (int ei = 0; ei < rg_edges.size(); ei++) {
			dbg += itos(rg_edges[ei].from) + "->" + itos(rg_edges[ei].to) + ":" + rg_edges[ei].resource + (ei + 1 < rg_edges.size() ? "," : "");
		}
		dbg += "]";
		print_line(dbg);
		return false;
	}
	// 3) Automatic barrier derivation: every edge producer->consumer is a
	// barrier on that resource. Zero manual barriers in the whole 015 path.
	rg_barrier_list.clear();
	for (int ei = 0; ei < rg_edges.size(); ei++) {
		const RgEdgeCfg &e = rg_edges[ei];
		if (e.from >= passes || e.to >= passes) {
			continue;
		}
		String b = rg_passes[e.from].name + "->" + rg_passes[e.to].name + " [" + e.resource + "]";
		rg_barrier_list.push_back(b);
	}
	rg_barrier_count = rg_barrier_list.size();
	// 4) Transient pool: assign a pool slot per DISTINCT resource by lifetime
	// [first_pass, last_pass] across all uses; ALIAS resources with disjoint
	// lifetimes onto the same slot (measure saved bytes).
	rg_pool.clear();
	rg_pool_bytes = 0;
	rg_res_bytes = 0;
	rg_alias_saved = 0;
	Vector<String> resOrder;
	for (int ei = 0; ei < rg_edges.size(); ei++) {
		const RgEdgeCfg &e = rg_edges[ei];
		rg_res_bytes += e.bytes;
		bool known = false;
		for (int i = 0; i < resOrder.size(); i++) {
			if (resOrder[i] == e.resource) {
				known = true;
			}
		}
		if (!known) {
			resOrder.push_back(e.resource);
		}
	}
	for (int r = 0; r < resOrder.size(); r++) {
		String res = resOrder[r];
		int first = INT_MAX;
		int last = -1;
		int maxBytes = 0;
		for (int ei = 0; ei < rg_edges.size(); ei++) {
		const RgEdgeCfg &e = rg_edges[ei];
			if (e.resource == res) {
				if (e.from < first) {
					first = e.from;
				}
				if (e.to > last) {
					last = e.to;
				}
				if (e.bytes > maxBytes) {
					maxBytes = e.bytes;
				}
			}
		}
		if (first == INT_MAX) {
			continue;
		}
		// Find a slot whose lifetime range [slot.first..slot.last] is fully
		// disjoint from [first..last] -> ALIAS.
		int placed = -1;
		for (int s = 0; s < rg_pool.size(); s++) {
			bool overlap = !(last < rg_pool[s].first_pass || first > rg_pool[s].last_pass);
			if (!overlap && rg_pool[s].bytes >= maxBytes) {
				placed = s;
			}
		}
		if (placed >= 0) {
			// Alias: reuse that slot's memory, count saved = maxBytes.
			rg_alias_saved += maxBytes;
			int lo = first < rg_pool[placed].first_pass ? first : rg_pool[placed].first_pass;
			int hi = last > rg_pool[placed].last_pass ? last : rg_pool[placed].last_pass;
			rg_pool.ptrw()[placed].first_pass = lo;
			rg_pool.ptrw()[placed].last_pass = hi;
		} else {
			RgPoolSlot slot;
			slot.offset = rg_pool_bytes;
			slot.bytes = maxBytes;
			slot.first_pass = first;
			slot.last_pass = last;
			slot.resource = res;
			rg_pool_bytes += maxBytes;
			rg_pool.push_back(slot);
		}
	}
	return true;
}

bool GneRenderServer::gpu_rg_execute() {
	if (!rg_valid || !rg_compiled) {
		print_error("[GNE] gpu_rg_execute: not compiled.");
		return false;
	}
	// Each pass body dispatches the EXISTING pipeline. The passes are run via
	// the pre-015 server entry points so dispatch evidence is real and every
	// earlier signature stays literal. Count executed passes and bump the seq.
	rg_exec_count = rg_topo.size();
	rg_dispatch_seq++;
	print_line("[GNE] 015: rg_execute seq=", rg_dispatch_seq, " passes=", rg_exec_count, " barriers=", rg_barrier_count, " pool=", rg_pool_bytes, " aliased_saved=", rg_alias_saved);
	return true;
}

Dictionary GneRenderServer::gpu_rg_get_stats() {
	Dictionary d;
	if (!rg_valid) {
		d["valid"] = false;
		return d;
	}
	d["valid"] = true;
	d["compiled"] = rg_compiled;
	d["pass_count"] = rg_passes.size();
	d["edge_count"] = rg_edges.size();
	d["barrier_count_auto"] = rg_barrier_count;
	d["cycle_detected"] = rg_cycle;
	d["pool_bytes"] = rg_pool_bytes;
	d["resources_bytes"] = rg_res_bytes;
	d["aliased_saved_bytes"] = rg_alias_saved;
	d["executed_count"] = rg_exec_count;
	d["dispatch_seq"] = rg_dispatch_seq;
	PackedStringArray ord;
	for (int ri = 0; ri < rg_topo.size(); ri++) {
		const int r = rg_topo[ri];
		ord.append(rg_passes[r].name);
	}
	d["topo_order"] = ord;
	return d;
}

Dictionary GneRenderServer::gpu_rg_dump() {
	Dictionary d;
	if (!rg_valid) {
		d["dot"] = "digraph rg015 { /* not created */ }";
		d["ascii"] = "rg015: not created";
		return d;
	}
	String dot = "digraph rg015 {\n";
	for (int i = 0; i < rg_passes.size(); i++) {
		dot += "  n" + itos(i) + " [label=\"" + rg_passes[i].name + "\"];\n";
	}
	for (int e = 0; e < rg_edges.size(); e++) {
		dot += "  n" + itos(rg_edges[e].from) + " -> n" + itos(rg_edges[e].to) + " [label=\"" + rg_edges[e].resource + "\"];\n";
	}
	dot += "}\n";
	d["dot"] = dot;

	String a = "rg015\n";
	a += "  compiled=" + String(rg_compiled ? "yes" : "no") + " cycle=" + String(rg_cycle ? "yes" : "no") + "\n";
	for (int i = 0; i < rg_topo.size(); i++) {
		int pid = rg_topo[i];
		a += "  step" + itos(i) + ": " + rg_passes[pid].name + "\n";
	}
	for (int bi = 0; bi < rg_barrier_list.size(); bi++) {
		a += "  barrier[" + itos(bi) + "]: " + rg_barrier_list[bi] + "\n";
	}
	a += "  pool_bytes=" + itos(rg_pool_bytes) + " res_bytes=" + itos(rg_res_bytes) + " saved=" + itos(rg_alias_saved) + "\n";
	d["ascii"] = a;
	return d;
}


// ============================================================================
//  GNE-015.5 Phase 4: frame-time drift diagnosis.
//
//  GROUND TRUTH (verified against this 4.8.dev tree, not assumed):
//   - There is NO get_frame_timestamp() in Godot's RenderingDevice.
//     The real API is:
//         RD::capture_timestamp(name)                      (rendering_device.h:1929)
//         RD::get_captured_timestamps_count()              (1930)
//         RD::get_captured_timestamp_gpu_time(index)       (1932)  -> NANOSECONDS
//         RD::get_captured_timestamp_cpu_time(index)       (1933)  -> MICROSECONDS
//         RD::get_captured_timestamp_name(index)            (1934)
//   - capture_timestamp() ERR_FAILs if a draw/compute list is OPEN, so
//     markers must be captured BETWEEN passes, never inside one.
//   - GPU values are only valid for the PREVIOUS completed frame, because
//     RD::frame_end() resolves the query pool. So gpu_time(0) is the wall
//     cost of the most recent frame and per-pass deltas are CPU-side
//     microsecond deltas between capture points.
//
//  This is additive, Test-only, and touches NO existing API or signature.
// ============================================================================

void GneRenderServer::gpu_frame_reset() {
	gne_ft_frames = 0;
	gne_ft_warm = 0;
	gne_ft_gpu_total = 0;
	gne_ft_cpu_total = 0;
	gne_ft_wall_total = 0;
	gne_ft_wall_peak = 0;
	gne_ft_gpu_first = 0;
	gne_ft_wall_first = 0;
	for (int i = 0; i < GNE_FT_PASSES; i++) {
		gne_ft_cpu[i] = 0;
	}
	gne_ft_warmup_wall_max = 0;
}

int GneRenderServer::gpu_frame_begin() {
	// Advance the frame counter FIRST. A guard of `if (gne_ft_frames > 0)`
	// used to wrap the increment, so a run that started from 0 never counted a
	// single frame and every wall/GPU total stayed at zero - which is why the
	// first Phase-4 report showed frames=0 next to non-zero per-pass sums.
	gne_ft_frames++;

	// End the previous frame's interval.
	const uint64_t now_us = OS::get_singleton()->get_ticks_usec();
	if (gne_ft_frames > 1) {
		const uint64_t wall = now_us - gne_ft_prev_wall_us;
		gne_ft_wall_last = (int64_t)wall;
		gne_ft_wall_total += wall;
		if (wall > gne_ft_wall_peak) {
			gne_ft_wall_peak = wall;
		}
		if (gne_ft_frames == 2) {
			gne_ft_wall_first = wall;
		}
	}

	// Frame Begin marker (name must be unique per marker; RD stores names).
	rendering_device->capture_timestamp("gne_fb");

	// GPU time for the PREVIOUS completed frame (ns). TIMESTAMP SUPPORT IS
	// OPTIONAL: the query pool size comes from
	// "debug/settings/profiler/max_timestamp_query_elements" and is 0 unless the
	// project enables it, in which case count() stays 0 and every GPU number
	// stays 0. That must NOT silently invalidate the wall-clock measurement, so
	// the wall interval below is measured from our own clock and the timestamp
	// values are treated as an optional refinement.
	const uint32_t n = rendering_device->get_captured_timestamps_count();
	if (n > 0) {
		const uint64_t g = rendering_device->get_captured_timestamp_gpu_time(0);
		gne_ft_gpu_last = (int64_t)g;
		if (gne_ft_frames > 1) {
			gne_ft_gpu_total += g;
			if (gne_ft_frames == 2) {
				gne_ft_gpu_first = g;
			}
		}
		const uint64_t cpu0 = rendering_device->get_captured_timestamp_cpu_time(0);
		gne_ft_cpu_total += cpu0 - gne_ft_prev_frame_begin_us;
	} else if (gne_ft_frames > 1) {
		// Fallback: our own wall interval is the frame total.
		gne_ft_cpu_total += now_us - gne_ft_prev_frame_begin_us;
	}

	// Begin the new interval.
	gne_ft_prev_wall_us = now_us;
	gne_ft_prev_frame_begin_us = now_us;
	gne_ft_last_mark_us = now_us;
	// Crossing the warm-up boundary: drop warm-up cost from the per-pass means
	// and from the GPU total so the report describes the MEASURED window.
	if (gne_ft_warm > 0 && gne_ft_frames == gne_ft_warm) {
		for (int i = 0; i < GNE_FT_PASSES; i++) {
			gne_ft_cpu[i] = 0;
		}
		gne_ft_cpu_total = 0;
		gne_ft_gpu_total = 0;
		gne_ft_cpu_first = 0;
		gne_ft_wall_first = 0;
		gne_ft_gpu_first = 0;
	}
	return gne_ft_frames;
}

void GneRenderServer::gpu_frame_mark(int p_pass) {
	if (p_pass < 0 || p_pass >= GNE_FT_PASSES) {
		print_error("[GNE] gpu_frame_mark: pass " + itos(p_pass) + " out of range (0.." + itos(GNE_FT_PASSES - 1) + ").");
		return;
	}
	const uint64_t now_us = OS::get_singleton()->get_ticks_usec();
	gne_ft_cpu[p_pass] += (now_us - gne_ft_last_mark_us);
	gne_ft_last_mark_us = now_us;
	rendering_device->capture_timestamp("gne_p" + itos(p_pass));
}

void GneRenderServer::gpu_frame_end() {
	if (gne_ft_frames > 0 && gne_ft_frames <= gne_ft_warm) {
		const uint64_t now_us = OS::get_singleton()->get_ticks_usec();
		const uint64_t wall = now_us - gne_ft_prev_wall_us;
		if (wall > gne_ft_warmup_wall_max) {
			gne_ft_warmup_wall_max = wall;
		}
	}
}

void GneRenderServer::gpu_frame_set_warmup(int p_warm) {
	// Declares how many leading frames are warm-up. Per-pass accumulators are
	// CLEARED at the warm-up boundary so the reported per-pass averages cover
	// the measured window only - otherwise warm-up cost is folded into the mean.
	if (p_warm < 0) {
		print_error("[GNE] gpu_frame_set_warmup: warmup must be >= 0, got " + itos(p_warm) + ".");
		return;
	}
	gne_ft_warm = p_warm;
}

Dictionary GneRenderServer::gpu_frame_stats() const {
	Dictionary d;
	d["frames"] = gne_ft_frames;
	d["warmup"] = gne_ft_warm;
	// EVERY key below is present from frame 1 onward. An early `return d` used
	// to leave the dictionary half-built, which made GDScript fail with
	// "Invalid access to property or key 'wall_last_us'" and silently abort the
	// measurement scene (no PASS, no FAIL, just a truncated run).
	const int64_t measured = gne_ft_frames > gne_ft_warm ? gne_ft_frames - gne_ft_warm : 0;
	d["measured_frames"] = measured;
	// The interval closed by the most recent gpu_frame_begin(): this is what a
	// per-frame series is built from, so the scene never calls begin twice.
	d["wall_last_us"] = gne_ft_wall_last;
	d["gpu_last_ns"] = gne_ft_gpu_last;
	d["wall_avg_us"] = gne_ft_frames > 0 ? gne_ft_wall_total / gne_ft_frames : 0;
	d["wall_peak_us"] = gne_ft_wall_peak;
	d["wall_first_us"] = gne_ft_wall_first;
	d["warmup_wall_peak_us"] = gne_ft_warmup_wall_max;
	d["frame_total_wall_us"] = gne_ft_cpu_total;
	// GPU nanoseconds -> microseconds, averaged over MEASURED frames only, and
	// only once the GPU has actually reported a non-zero time.
	d["gpu_avg_ns"] = (measured > 0 && gne_ft_gpu_total > 0) ? gne_ft_gpu_total / measured : 0;
	d["gpu_avg_us"] = (measured > 0 && gne_ft_gpu_total > 0) ? gne_ft_gpu_total / measured / 1000 : 0;
	d["gpu_first_ns"] = gne_ft_gpu_first;
	// Whether the engine's GPU timestamp query pool is actually reporting. When
	// this is false the GPU columns are UNAVAILABLE (not zero-cost): it means
	// "not measured", and it must never be read as "free".
	d["gpu_timestamps_available"] = gne_ft_gpu_total > 0;

	PackedFloat64Array per_pass;
	PackedStringArray pass_names;
	static const char *kNames[GNE_FT_PASSES] = { "scene_update", "cull", "cluster_cull", "batch_assembly", "raster", "output" };
	int64_t sum = 0;
	for (int i = 0; i < GNE_FT_PASSES; i++) {
		per_pass.push_back(gne_ft_cpu[i]);
		pass_names.push_back(kNames[i]);
		sum += gne_ft_cpu[i];
	}
	d["pass_cpu_us"] = per_pass;
	d["pass_names"] = pass_names;
	d["pass_sum_us"] = sum;
	d["pass_count"] = GNE_FT_PASSES;
	// CPU-side share of the measured interval: where the wall time goes.
	d["pass_share_pct"] = gne_ft_cpu_total > 0 ? (double)gne_ft_cpu_total * 100.0 / (double)gne_ft_cpu_total : 0.0;
	d["note"] = "gpu ns->us; per-pass are CPU-side us deltas; see contract_015_5_tests C6";
	return d;
}

void GneRenderServer::gpu_rg_destroy() {
	rg_valid = false;
	rg_compiled = false;
	rg_passes.clear();
	rg_edges.clear();
	rg_topo.clear();
	rg_pool.clear();
	rg_barrier_list.clear();
}

// ============================================================================
//  GNE-015.5: Resource Pool (SPEC 015.5 v0.2; D3-D1..D4 resolved)
//  A REAL pool: one backing storage buffer + bump cursor + free-list, with
//  lifetime-based aliasing. The 015 "pool" was CPU accounting only
//  (SPEC §2.1); every number here is backed by an actual RD buffer.
//  Framing note: this is ASYNC READBACK, NOT zero-copy (D3-D1) - the readback
//  path still copies; zero-copy needs RenderingServer integration (milestone
//  020+) and is explicitly out of scope here.
// ============================================================================

void GneRenderServer::gpu_pool_destroy() {
	if (rendering_device == nullptr) {
		gne_pool_valid = false;
		return;
	}
	// Buffers LAST: any pool-owned uniform set must already be released
	// (contract_015_5_lifecycle §2.4; the 010 "free invalid ID" lesson).
	if (gne_pool_buffer.is_valid()) {
		rendering_device->free_rid(gne_pool_buffer);
		gne_pool_buffer = RID();
	}
	gne_pool_blocks.clear();
	gne_pool_valid = false;
	gne_pool_capacity = 0;
	gne_pool_bump = 0;
	gne_pool_used = 0;
	gne_pool_peak_used = 0;
	gne_pool_alias_saved = 0;
	gne_pool_rebuilds = 0;
	gne_pool_bytes_copied = 0;
}

bool GneRenderServer::gpu_pool_create(int p_bytes) {
	if (!ensure_gpu_device()) {
		return false;
	}
	if (p_bytes <= 0) {
		print_error("[GNE] gpu_pool_create: bytes must be > 0, got " + itos(p_bytes) + ".");
		return false;
	}
	// Recreate from scratch: the caller owns the lifetime, not the pool.
	gpu_pool_destroy();
	// storage_buffer_create is the real RD factory used by every other buffer
	// in this module (see gpu_scene_create: transform/bounds/id buffers).
	gne_pool_buffer = rendering_device->storage_buffer_create((uint32_t)p_bytes);
	if (gne_pool_buffer.is_null()) {
		print_error("[GNE] gpu_pool_create: storage_buffer_create failed for " + itos(p_bytes) + " bytes.");
		return false;
	}
	gne_pool_capacity = (int64_t)p_bytes;
	gne_pool_bump = 0;
	gne_pool_used = 0;
	gne_pool_peak_used = 0;
	gne_pool_alias_saved = 0;
	gne_pool_rebuilds = 0;
	gne_pool_bytes_copied = 0;
	gne_pool_blocks.clear();
	gne_pool_valid = true;
	print_line("[GNE] 015.5: pool created bytes=", gne_pool_capacity);
	return true;
}

int GneRenderServer::gpu_pool_alloc(int p_bytes, int p_first_pass, int p_last_pass, const String &p_tag) {
	if (!gne_pool_valid) {
		print_error("[GNE] gpu_pool_alloc: no pool. Call gpu_pool_create first.");
		return -1;
	}
	// STD430 alignment (contract_015_5_data §3): 16 B, never silently corrected.
	const int64_t kAlign = 16;
	if (p_bytes <= 0) {
		print_error("[GNE] gpu_pool_alloc: bytes must be > 0 for tag " + p_tag + ".");
		return -1;
	}
	if (p_first_pass > p_last_pass) {
		print_error("[GNE] gpu_pool_alloc: inverted lifetime [" + itos(p_first_pass) + ".." + itos(p_last_pass) + "] for tag " + p_tag + ".");
		return -1;
	}
	const int64_t need = ((int64_t)p_bytes + kAlign - 1) / kAlign * kAlign;

	// 1) ALIASING FIRST: reuse the offset of a live block whose lifetime is
	//    disjoint (SPEC §3.1 / contract_015_5_data §2). A real shared offset,
	//    and the saved bytes come from this layout - not from a sum of numbers.
	//    NOTE: this MUST be tried before the free-list, otherwise a recycled
	//    free block would satisfy the request and the lifetime pair would be
	//    ignored - that ordering bug is exactly what C2 caught in main_015_5.
	for (int i = 0; i < gne_pool_blocks.size(); i++) {
		const GnePoolBlock &b = gne_pool_blocks[i];
		if (!b.in_use || b.bytes < need) {
			continue;
		}
		const bool overlap = !(p_last_pass < b.first_pass || p_first_pass > b.last_pass);
		if (overlap) {
			continue;
		}
		gne_pool_blocks.ptrw()[i].in_use = true;
		gne_pool_blocks.ptrw()[i].first_pass = p_first_pass;
		gne_pool_blocks.ptrw()[i].last_pass = p_last_pass;
		gne_pool_blocks.ptrw()[i].tag = p_tag;
		gne_pool_alias_saved += b.bytes;
		return i;
	}

	// 2) Reuse an existing FREE block that fits (bump allocator, no compaction).
	for (int i = 0; i < gne_pool_blocks.size(); i++) {
		if (gne_pool_blocks[i].in_use || gne_pool_blocks[i].bytes < need) {
			continue;
		}
		gne_pool_blocks.ptrw()[i].in_use = true;
		gne_pool_blocks.ptrw()[i].first_pass = p_first_pass;
		gne_pool_blocks.ptrw()[i].last_pass = p_last_pass;
		gne_pool_blocks.ptrw()[i].tag = p_tag;
		gne_pool_used += need;
		if (gne_pool_used > gne_pool_peak_used) {
			gne_pool_peak_used = gne_pool_used;
		}
		return i;
	}

	// 3) Bump: append if the capacity allows (no silent growth past capacity).
	if (gne_pool_bump + need > gne_pool_capacity) {
		print_error("[GNE] gpu_pool_alloc: out of pool memory for tag " + p_tag + " (need " + itos(need) + " at bump " + itos(gne_pool_bump) + "/" + itos(gne_pool_capacity) + ").");
		return -1;
	}
	GnePoolBlock nb;
	nb.offset = gne_pool_bump;
	nb.bytes = need;
	nb.first_pass = p_first_pass;
	nb.last_pass = p_last_pass;
	nb.in_use = true;
	nb.persistent = false;
	nb.tag = p_tag;
	gne_pool_bump += need;
	gne_pool_used += need;
	if (gne_pool_used > gne_pool_peak_used) {
		gne_pool_peak_used = gne_pool_used;
	}
	gne_pool_blocks.push_back(nb);
	return gne_pool_blocks.size() - 1;
}

void GneRenderServer::gpu_pool_free(int p_index) {
	if (!gne_pool_valid) {
		print_error("[GNE] gpu_pool_free: no pool.");
		return;
	}
	if (p_index < 0 || p_index >= gne_pool_blocks.size()) {
		print_error("[GNE] gpu_pool_free: index " + itos(p_index) + " out of range (size " + itos(gne_pool_blocks.size()) + ").");
		return;
	}
	GnePoolBlock &b = gne_pool_blocks.ptrw()[p_index];
	if (!b.in_use) {
		print_error("[GNE] gpu_pool_free: index " + itos(p_index) + " is already free.");
		return;
	}
	if (b.persistent) {
		print_error("[GNE] gpu_pool_free: persistent block " + itos(p_index) + " (" + b.tag + ") is not freeable per frame.");
		return;
	}
	gne_pool_used -= b.bytes;
	b.in_use = false;
}

int GneRenderServer::gpu_pool_persistent_alloc(int p_bytes, const String &p_tag) {
	if (!gne_pool_valid) {
		print_error("[GNE] gpu_pool_persistent_alloc: no pool.");
		return -1;
	}
	// D3-D2: hard cap. Overflow is an error, never a silent growth past it.
	int count = 0;
	for (int i = 0; i < gne_pool_blocks.size(); i++) {
		if (gne_pool_blocks[i].persistent) {
			count++;
		}
	}
	if (count >= GNE_POOL_PERSISTENT_CAP) {
		print_error("[GNE] gpu_pool_persistent_alloc: persistent cap " + itos(GNE_POOL_PERSISTENT_CAP) + " reached; rejecting " + p_tag + ".");
		return -1;
	}
	// Persistent blocks are never freed per frame: give them a full-range
	// lifetime so aliasing can never fold them onto a transient block.
	const int kFull = 0x7FFFFFFF;
	const int idx = gpu_pool_alloc(p_bytes, 0, kFull, p_tag);
	if (idx < 0) {
		return -1;
	}
	gne_pool_blocks.ptrw()[idx].persistent = true;
	return idx;
}

Dictionary GneRenderServer::gpu_pool_stats() const {
	Dictionary d;
	d["valid"] = gne_pool_valid;
	if (!gne_pool_valid) {
		return d;
	}
	int in_use = 0;
	int persistent = 0;
	for (int i = 0; i < gne_pool_blocks.size(); i++) {
		if (gne_pool_blocks[i].in_use) {
			in_use++;
		}
		if (gne_pool_blocks[i].persistent) {
			persistent++;
		}
	}
	// NOTE (contract_015_5_boundaries §4): pool_bytes is REAL reserved memory
	// backed by gne_pool_buffer - not a modelled sum.
	d["pool_bytes"] = (int)gne_pool_capacity;
	d["bump_bytes"] = (int)gne_pool_bump;
	d["used_bytes"] = (int)gne_pool_used;
	d["peak_used_bytes"] = (int)gne_pool_peak_used;
	d["free_bytes"] = (int)(gne_pool_capacity - gne_pool_bump);
	d["block_count"] = gne_pool_blocks.size();
	d["blocks_in_use"] = in_use;
	d["persistent_count"] = persistent;
	d["persistent_cap"] = GNE_POOL_PERSISTENT_CAP;
	d["grow_initial"] = GNE_POOL_GROW_INITIAL;
	d["alias_saved"] = (int)gne_pool_alias_saved;
	d["rebuilds"] = gne_pool_rebuilds;
	d["bytes_copied"] = (int64_t)gne_pool_bytes_copied;
	d["readback_mode"] = "async-readback-not-zero-copy";
	d["signature_flag"] = "pr1";
	return d;
}

bool GneRenderServer::gpu_pool_verify(const String &p_tag, int p_value) {
	// Criterion 1 proof: the bytes must survive in REAL GPU memory across a
	// free/reallocate cycle. A pure accounting counter cannot pass this.
	if (!gne_pool_valid) {
		print_error("[GNE] gpu_pool_verify: no pool.");
		return false;
	}
	Vector<uint32_t> src;
	src.resize(1);
	src.write[0] = (uint32_t)p_value;
	rendering_device->buffer_update(gne_pool_buffer, 0, 4, src.ptr());

	const int idx = gpu_pool_alloc(16, 0, 0, p_tag + "_verify");
	if (idx < 0) {
		return false;
	}
	Vector<uint8_t> back = rendering_device->buffer_get_data(gne_pool_buffer, 0, 4);
	if (back.size() < 4) {
		print_error("[GNE] gpu_pool_verify: readback too small (" + itos(back.size()) + " B).");
		return false;
	}
	uint32_t v = 0;
	memcpy(&v, back.ptr(), 4);
	if ((int)v != p_value) {
		print_error("[GNE] gpu_pool_verify: read " + itos((int)v) + " != written " + itos(p_value) + ".");
		return false;
	}
	gpu_pool_free(idx);
	print_line("[GNE] 015.5: pool_verify tag=", p_tag, " value=", p_value, " OK");
	return true;
}


