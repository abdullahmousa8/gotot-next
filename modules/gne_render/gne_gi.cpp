// GNE-022: probe-based radiance field. S1: direct + occlusion (NOT "GI").
// S2: temporal accumulation + probe-feedback bounce (feedback loop - see
// spec_022 section 9 for the frozen safety decisions: convergence gate,
// cross-frame determinism, GI naming gate).

#include "gne_render_server.h"

static float gi_half_to_float(uint16_t p_half) {
	uint32_t sign = (p_half >> 15) & 0x1;
	uint32_t expo = (p_half >> 10) & 0x1F;
	uint32_t mant = p_half & 0x3FF;
	uint32_t f;
	if (expo == 0) {
		if (mant == 0) {
			f = sign << 31;
		} else {
			expo = 127 - 15 + 1;
			while ((mant & 0x400) == 0) {
				mant <<= 1;
				expo--;
			}
			mant &= 0x3FF;
			f = (sign << 31) | (expo << 23) | (mant << 13);
		}
	} else if (expo == 31) {
		f = (sign << 31) | 0x7F800000 | (mant << 13);
	} else {
		f = (sign << 31) | ((expo + 127 - 15) << 23) | (mant << 13);
	}
	float out;
	memcpy(&out, &f, 4);
	return out;
}

static const char *gne_gi_trace_glsl = R"(
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 0, rgba16f) uniform image2D gi_atlas;
layout(set = 0, binding = 1, std430) buffer TrBuf { vec4 t[]; } trs;
layout(set = 0, binding = 2, std430) buffer LtBuf { vec4 l[]; } lights;
layout(set = 0, binding = 3) uniform sampler2D gi_prev;
layout(push_constant, std430) uniform P {
	vec4 gmin;
	vec4 gmax;
	vec4 dims;    // (gx, gy, gz, instance_count)
	vec4 ambient; // miss ambient rgb
	vec4 counts;  // (light_count, 0, 0, 0)
	vec4 params;  // (mode: 0 accum / 1 raw / 2 zero, alpha, albedo, bounce_on)
} pc;
vec3 octa_decode(vec2 f) {
	vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
	float t = max(-n.z, 0.0);
	n.x += (n.x >= 0.0) ? -t : t;
	n.y += (n.y >= 0.0) ? -t : t;
	return normalize(n);
}
vec2 octa_encode(vec3 n) {
	n = normalize(n);
	n /= (abs(n.x) + abs(n.y) + abs(n.z));
	vec2 f = n.xy;
	if (n.z < 0.0) {
		f = (1.0 - abs(n.yx)) * vec2((n.x >= 0.0) ? 1.0 : -1.0, (n.y >= 0.0) ? 1.0 : -1.0);
	}
	return f;
}
// Single-source probe-field sampler (spec_022 section 8.1 requirement 3):
// octahedral lookup + trilinear probe blend. Used by the S2 hit-point
// feedback; later also by the material shading integration.
vec3 gne_gi_sample_field(vec3 pos) {
	vec3 gsz = pc.dims.xyz;
	vec3 g = (pos - pc.gmin.xyz) / (pc.gmax.xyz - pc.gmin.xyz) * gsz - 0.5;
	// Diagnostic round (spec_022 9.3.2): sub-cell offset removes the lattice
	// degeneracy where hit points land exactly on node positions (w=0 -> self
	// sampling). Irrational-ish per-axis constants; no exact-node sampling remains.
	g += vec3(0.31, 0.17, 0.0);
	vec3 g0 = floor(g);
	vec3 w = g - g0;
	vec3 acc = vec3(0.0);
	for (int dz = 0; dz < 2; dz++) {
		for (int dy = 0; dy < 2; dy++) {
			for (int dx = 0; dx < 2; dx++) {
				vec3 idx = g0 + vec3(dx, dy, dz);
				idx = clamp(idx, vec3(0.0), gsz - 1.0);
				float wx = (dx == 0) ? (1.0 - w.x) : w.x;
				float wy = (dy == 0) ? (1.0 - w.y) : w.y;
				float wz = (dz == 0) ? (1.0 - w.z) : w.z;
				vec3 center = pc.gmin.xyz + (idx + 0.5) * (pc.gmax.xyz - pc.gmin.xyz) / gsz;
				vec3 d = pos - center;
				if (dot(d, d) < 1e-8) {
					d = vec3(0.0, 1.0, 0.0);
				}
				vec2 f = octa_encode(d);
				ivec2 lxy = ivec2(clamp((f * 0.5 + 0.5) * 8.0, vec2(0.0), vec2(7.999)));
				ivec2 tile = ivec2(int(idx.x) * 8, (int(idx.y) + int(idx.z) * int(gsz.y)) * 8);
				acc += wx * wy * wz * texelFetch(gi_prev, tile + lxy, 0).rgb;
			}
		}
	}
	return acc;
}
void main() {
	ivec3 wid = ivec3(gl_WorkGroupID);
	ivec2 lp = ivec2(gl_LocalInvocationID.xy);
	vec3 cell = (pc.gmax.xyz - pc.gmin.xyz) / pc.dims.xyz;
	vec3 ppos = pc.gmin.xyz + (vec3(wid) + 0.5) * cell;
	vec2 f = ((vec2(lp) + 0.5) / 8.0) * 2.0 - 1.0;
	vec3 dir = octa_decode(f);
	float best = 1e30;
	uint ninst = uint(pc.dims.w + 0.5);
	for (uint i = 0u; i < ninst; i++) {
		vec4 trv = trs.t[i];
		vec3 c = trv.xyz;
		float s = trv.w;
		vec3 lo = c - s * 0.5;
		vec3 hi = c + s * 0.5;
		vec3 inv = 1.0 / dir;
		vec3 t0 = (lo - ppos) * inv;
		vec3 t1 = (hi - ppos) * inv;
		vec3 tsm = min(t0, t1);
		vec3 tbg = max(t0, t1);
		float tn = max(max(tsm.x, tsm.y), tsm.z);
		float tf = min(min(tbg.x, tbg.y), tbg.z);
		tn = max(tn, 0.0);
		if (tf >= tn && tn < best) {
			best = tn;
		}
	}
	vec3 rad;
	if (best < 1e29) {
		vec3 hp = ppos + dir * best;
		rad = vec3(0.0);
		uint nl = uint(pc.counts.x + 0.5);
		for (uint li = 0u; li < nl; li++) {
			vec4 L0 = lights.l[li * 4u + 0u];
			vec4 L1 = lights.l[li * 4u + 1u];
			vec3 d = L0.xyz - hp;
			float dist = length(d);
			float range = L0.w;
			if (dist < range) {
				float att = max(0.0, 1.0 - dist / range);
				rad += L1.rgb * L1.w * att * att;
			}
		}
		if (pc.params.w > 0.5) {
			// S2 bounce: surface receives the previous frame's probe field as an
			// additional incoming source (constant albedo approximation).
			rad += pc.params.z * gne_gi_sample_field(hp);
		}
	} else {
		rad = pc.ambient.rgb;
	}
	ivec2 tile = ivec2(wid.x * 8 + lp.x, (wid.y + wid.z * int(pc.dims.y + 0.5)) * 8 + lp.y);
	vec4 outc;
	float mode = pc.params.x;
	if (mode > 1.5) {
		outc = vec4(0.0);
	} else if (mode > 0.5) {
		outc = vec4(rad, 1.0);
	} else {
		vec4 prev = texelFetch(gi_prev, tile, 0);
		outc = mix(prev, vec4(rad, 1.0), pc.params.y);
	}
	imageStore(gi_atlas, tile, outc);
}
)";

RID GneRenderServer::_gi_front_atlas() const {
	return (gi_front == 0) ? gi_atlas : gi_atlas2;
}

bool GneRenderServer::_gi_dispatch(float p_mode) {
	if (gi_trace_pipeline.is_null()) {
		print_error("[GNE] _gi_dispatch: no probe field. Call gpu_gi_create first.");
		return false;
	}
	if (!gpu_scene_valid || !gpu_light_valid) {
		print_error("[GNE] _gi_dispatch: scene/lights not ready.");
		return false;
	}
	struct GiPush {
		float gmin[4];
		float gmax[4];
		float dims[4];
		float ambient[4];
		float counts[4];
		float params[4];
	};
	GiPush pc;
	pc.gmin[0] = gi_min.x;
	pc.gmin[1] = gi_min.y;
	pc.gmin[2] = gi_min.z;
	pc.gmin[3] = 0.0f;
	pc.gmax[0] = gi_max.x;
	pc.gmax[1] = gi_max.y;
	pc.gmax[2] = gi_max.z;
	pc.gmax[3] = 0.0f;
	pc.dims[0] = (float)gi_gx;
	pc.dims[1] = (float)gi_gy;
	pc.dims[2] = (float)gi_gz;
	pc.dims[3] = (float)gpu_instance_count;
	pc.ambient[0] = gi_ambient_r;
	pc.ambient[1] = gi_ambient_g;
	pc.ambient[2] = gi_ambient_b;
	pc.ambient[3] = 0.0f;
	pc.counts[0] = (float)light_count;
	pc.counts[1] = 0.0f;
	pc.counts[2] = 0.0f;
	pc.counts[3] = 0.0f;
	pc.params[0] = p_mode;
	pc.params[1] = gi_alpha;
	pc.params[2] = gi_albedo;
	pc.params[3] = gi_bounce ? 1.0f : 0.0f;
	// Target set: write to the NON-front atlas, read the front one.
	RID use_set = (gi_front == 0) ? gi_trace_set_b : gi_trace_set_a;
	_run_compute_pass(gi_trace_pipeline, use_set, &pc, sizeof(pc), gi_gx, gi_gy, gi_gz);
	gi_front = 1 - gi_front;
	return true;
}

bool GneRenderServer::gpu_gi_create(const Dictionary &p_cfg) {
	if (!ensure_gpu_device()) {
		return false;
	}
	if (gi_atlas.is_valid()) {
		print_error("[GNE] gpu_gi_create: atlas already exists (non-destructive guard).");
		return false;
	}
	gi_min = p_cfg.get("min", Vector3(-10, -10, -10));
	gi_max = p_cfg.get("max", Vector3(10, 10, 10));
	Vector3i dims = p_cfg.get("dims", Vector3i(16, 8, 16));
	gi_gx = MAX(1, dims.x);
	gi_gy = MAX(1, dims.y);
	gi_gz = MAX(1, dims.z);
	int aw = gi_gx * 8;
	int ah = gi_gy * gi_gz * 8;
	RD::TextureFormat tf;
	tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	tf.width = aw;
	tf.height = ah;
	tf.depth = 1;
	tf.texture_type = RD::TEXTURE_TYPE_2D;
	// S1/S2 forward-compat (spec_022 section 8.1): SAMPLING from day one;
	// double-buffered for the S2 feedback loop.
	tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	gi_atlas = rendering_device->texture_create(tf, RD::TextureView());
	gi_atlas2 = rendering_device->texture_create(tf, RD::TextureView());
	if (gi_atlas.is_null() || gi_atlas2.is_null()) {
		print_error("[GNE] gpu_gi_create: atlas texture_create failed.");
		return false;
	}
	String error;
	Vector<uint8_t> spirv = rendering_device->shader_compile_spirv_from_source(
			RD::SHADER_STAGE_COMPUTE, String(gne_gi_trace_glsl), RD::SHADER_LANGUAGE_GLSL, &error);
	if (spirv.is_empty()) {
		print_error("[GNE] gi trace shader compile failed:");
		print_error(error);
		return false;
	}
	Vector<RD::ShaderStageSPIRVData> stages;
	RD::ShaderStageSPIRVData cs;
	cs.shader_stage = RD::SHADER_STAGE_COMPUTE;
	cs.spirv = spirv;
	stages.push_back(cs);
	gi_trace_shader = rendering_device->shader_create_from_spirv(stages, "gne_gi_trace");
	gi_trace_pipeline = rendering_device->compute_pipeline_create(gi_trace_shader);
	if (gi_trace_shader.is_null() || gi_trace_pipeline.is_null()) {
		print_error("[GNE] gpu_gi_create: pipeline create failed.");
		return false;
	}
	RD::SamplerState ss;
	ss.min_filter = RD::SAMPLER_FILTER_NEAREST;
	ss.mag_filter = RD::SAMPLER_FILTER_NEAREST;
	ss.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	ss.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	ss.repeat_w = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	gi_sampler = rendering_device->sampler_create(ss);
	if (gi_sampler.is_null()) {
		print_error("[GNE] gpu_gi_create: sampler_create failed.");
		return false;
	}
	Vector<RD::Uniform> cu;
	RD::Uniform u0;
	u0.uniform_type = RD::UNIFORM_TYPE_IMAGE;
	u0.binding = 0;
	u0.append_id(gi_atlas);
	cu.push_back(u0);
	RD::Uniform u1;
	u1.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	u1.binding = 1;
	u1.append_id(transform_buffer);
	cu.push_back(u1);
	RD::Uniform u2;
	u2.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	u2.binding = 2;
	u2.append_id(light_buffer);
	cu.push_back(u2);
	RD::Uniform u3;
	u3.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
	u3.binding = 3;
	u3.append_id(gi_sampler);
	u3.append_id(gi_atlas2);
	cu.push_back(u3);
	gi_trace_set_a = rendering_device->uniform_set_create(cu, gi_trace_shader, 0);
	Vector<RD::Uniform> cu2;
	RD::Uniform v0;
	v0.uniform_type = RD::UNIFORM_TYPE_IMAGE;
	v0.binding = 0;
	v0.append_id(gi_atlas2);
	cu2.push_back(v0);
	RD::Uniform v1;
	v1.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	v1.binding = 1;
	v1.append_id(transform_buffer);
	cu2.push_back(v1);
	RD::Uniform v2;
	v2.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	v2.binding = 2;
	v2.append_id(light_buffer);
	cu2.push_back(v2);
	RD::Uniform v3;
	v3.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
	v3.binding = 3;
	v3.append_id(gi_sampler);
	v3.append_id(gi_atlas);
	cu2.push_back(v3);
	gi_trace_set_b = rendering_device->uniform_set_create(cu2, gi_trace_shader, 0);
	if (gi_trace_set_a.is_null() || gi_trace_set_b.is_null()) {
		print_error("[GNE] gpu_gi_create: uniform_set_create failed.");
		return false;
	}
	print_line("[GNE] 022-S1: probe field created dims=", gi_gx, "x", gi_gy, "x", gi_gz, " atlas=", aw, "x", ah);
	return true;
}

void GneRenderServer::gpu_gi_enabled_set(bool p_enabled) {
	gne_gi_enabled = p_enabled;
}

void GneRenderServer::gpu_gi_config(const Dictionary &p_cfg) {
	gi_alpha = p_cfg.get("alpha", gi_alpha);
	gi_albedo = p_cfg.get("albedo", gi_albedo);
	gi_bounce = p_cfg.get("bounce", gi_bounce);
	Vector3 amb = p_cfg.get("ambient", Vector3(gi_ambient_r, gi_ambient_g, gi_ambient_b));
	gi_ambient_r = amb.x;
	gi_ambient_g = amb.y;
	gi_ambient_b = amb.z;
}

Dictionary GneRenderServer::gpu_gi_info() const {
	Dictionary d;
	d["valid"] = gi_atlas.is_valid();
	d["enabled"] = gne_gi_enabled;
	d["gx"] = gi_gx;
	d["gy"] = gi_gy;
	d["gz"] = gi_gz;
	d["probes"] = gi_gx * gi_gy * gi_gz;
	d["atlas_w"] = gi_gx * 8;
	d["atlas_h"] = gi_gy * gi_gz * 8;
	d["min"] = gi_min;
	d["max"] = gi_max;
	d["alpha"] = gi_alpha;
	d["albedo"] = gi_albedo;
	d["bounce"] = gi_bounce;
	d["front"] = gi_front;
	d["mode"] = "probe-direct+occlusion"; // NOT "GI" until the naming gate (9.3)
	return d;
}

bool GneRenderServer::gpu_gi_trace() {
	return _gi_dispatch(1.0f); // raw direct write (S1 semantics)
}

bool GneRenderServer::gpu_gi_accum_step() {
	return _gi_dispatch(0.0f); // S2: bounce + EMA into the back buffer
}

bool GneRenderServer::gpu_gi_reset() {
	// Zero both buffers (deterministic start state), end with front = atlas.
	if (!_gi_dispatch(2.0f)) {
		return false;
	}
	if (!_gi_dispatch(2.0f)) {
		return false;
	}
	gi_front = 0;
	return true;
}

PackedFloat32Array GneRenderServer::gpu_gi_read_avg(int p_probe) {
	PackedFloat32Array out;
	RID src = _gi_front_atlas();
	if (src.is_null()) {
		return out;
	}
	int np = gi_gx * gi_gy * gi_gz;
	if (p_probe < 0 || p_probe >= np) {
		return out;
	}
	int aw = gi_gx * 8;
	int px = (p_probe % gi_gx) * 8;
	int py = (p_probe / gi_gx) * 8;
	Vector<uint8_t> data = rendering_device->texture_get_data(src, 0);
	if (data.size() < (size_t)(aw * (gi_gy * gi_gz * 8) * 8)) {
		return out;
	}
	double sr = 0.0, sg = 0.0, sb = 0.0;
	for (int ly = 0; ly < 8; ly++) {
		for (int lx = 0; lx < 8; lx++) {
			int ofs = ((py + ly) * aw + (px + lx)) * 8;
			uint16_t hr, hg, hb;
			memcpy(&hr, data.ptr() + ofs + 0, 2);
			memcpy(&hg, data.ptr() + ofs + 2, 2);
			memcpy(&hb, data.ptr() + ofs + 4, 2);
			sr += gi_half_to_float(hr);
			sg += gi_half_to_float(hg);
			sb += gi_half_to_float(hb);
		}
	}
	out.append((float)(sr / 64.0));
	out.append((float)(sg / 64.0));
	out.append((float)(sb / 64.0));
	out.append(64.0f);
	return out;
}

PackedFloat32Array GneRenderServer::gpu_gi_read_texel(int p_probe, int p_texel) {
	PackedFloat32Array out;
	RID src = _gi_front_atlas();
	if (src.is_null()) {
		return out;
	}
	int np = gi_gx * gi_gy * gi_gz;
	if (p_probe < 0 || p_probe >= np || p_texel < 0 || p_texel >= 64) {
		return out;
	}
	int aw = gi_gx * 8;
	int px = (p_probe % gi_gx) * 8 + (p_texel % 8);
	int py = (p_probe / gi_gx) * 8 + (p_texel / 8);
	Vector<uint8_t> data = rendering_device->texture_get_data(src, 0);
	if (data.size() < (size_t)(aw * (gi_gy * gi_gz * 8) * 8)) {
		return out;
	}
	int ofs = (py * aw + px) * 8;
	uint16_t hr, hg, hb;
	memcpy(&hr, data.ptr() + ofs + 0, 2);
	memcpy(&hg, data.ptr() + ofs + 2, 2);
	memcpy(&hb, data.ptr() + ofs + 4, 2);
	out.append(gi_half_to_float(hr));
	out.append(gi_half_to_float(hg));
	out.append(gi_half_to_float(hb));
	return out;
}