// GNE-022 R0-RT: RD ray-tracing isolation test (test-only; additive; no gates).
// Chain: 1-triangle BLAS -> 1-instance TLAS -> RT pipeline (raygen/miss/chit)
// -> SBT -> trace_rays (2 rays: 1 hit / 1 miss) -> readback. The caller compares
// analytically (expected hit t = 5.0; miss marker = -1.0).
// Defined here (separate TU) to keep the main module file untouched by design.

#include "gne_render_server.h"

#include "core/templates/local_vector.h"
#include "core/templates/span.h"

PackedFloat32Array GneRenderServer::gpu_rt_selftest() {
	PackedFloat32Array out;
	if (!ensure_gpu_device()) {
		out.append(-1);
		return out;
	}
	// Geometry: triangle v0(0,0,-5) v1(2,0,-5) v2(0,2,-5), indices 0,1,2.
	const float verts[9] = { 0.0f, 0.0f, -5.0f, 2.0f, 0.0f, -5.0f, 0.0f, 2.0f, -5.0f };
	const uint32_t idx[3] = { 0, 1, 2 };
	Vector<uint8_t> vb;
	vb.resize(36);
	memcpy(vb.ptrw(), verts, 36);
	Vector<uint8_t> ib;
	ib.resize(12);
	memcpy(ib.ptrw(), idx, 12);
	RID vbuf = rendering_device->vertex_buffer_create(36, Span<uint8_t>(vb.ptrw(), 36), RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT);
	RID ibuf = rendering_device->index_buffer_create(3, RD::INDEX_BUFFER_FORMAT_UINT32, Span<uint8_t>(ib.ptrw(), 12), false, RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT);
	if (vbuf.is_null() || ibuf.is_null()) {
		out.append(-2);
		return out;
	}
	RD::AccelerationStructureGeometry geo;
	geo.flags = RD::ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT;
	geo.vertex_buffer = vbuf;
	geo.vertex_offset = 0;
	geo.vertex_stride = 12;
	geo.vertex_count = 3;
	geo.vertex_format = RD::DATA_FORMAT_R32G32B32_SFLOAT;
	geo.index_buffer = ibuf;
	geo.index_offset = 0;
	geo.index_count = 3;
	LocalVector<RD::AccelerationStructureGeometry> geos;
	geos.push_back(geo);
	RID blas = rendering_device->blas_create(geos.span(), RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT);
	if (blas.is_null()) {
		out.append(-3);
		return out;
	}
	if (rendering_device->blas_build(blas) != OK) {
		out.append(-4);
		return out;
	}
	RID tlas = rendering_device->tlas_create(1, RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT);
	if (tlas.is_null()) {
		out.append(-5);
		return out;
	}
	RenderingDevice::AccelerationStructureInstance inst;
	inst.transform = Transform3D();
	inst.id = 0;
	inst.mask = 0xFF;
	inst.hit_sbt_range = 0;
	inst.flags = 0;
	inst.blas = blas;
	LocalVector<RD::AccelerationStructureInstance> insts;
	insts.push_back(inst);	// Shaders (GLSL via the fork's shader compiler; R0-RT is the compile-path proof).
	static const char *rt_raygen = R"(
#version 460
#extension GL_EXT_ray_tracing : require
layout(set = 0, binding = 0) uniform accelerationStructureEXT gne_tlas;
layout(set = 0, binding = 1, std430) buffer GneRtOut { vec4 data[]; } gne_out;
layout(location = 0) rayPayloadEXT vec4 gne_payload;
void main() {
	uint ridx = gl_LaunchIDEXT.y * gl_LaunchSizeEXT.x + gl_LaunchIDEXT.x;
	vec3 origin = vec3(0.2, 0.2, 0.0);
	vec3 dir = vec3(0.0, 0.0, -1.0);
	if (ridx == 1u) {
		dir = vec3(0.0, 0.0, 1.0);
	}
	gne_payload = vec4(-2.0, 0.0, 0.0, 0.0);
	traceRayEXT(gne_tlas, gl_RayFlagsOpaqueEXT, 0xff, 0, 0, 0, origin, 0.0, dir, 1000.0, 0);
	gne_out.data[ridx] = gne_payload;
}
)";
	static const char *rt_miss = R"(
#version 460
#extension GL_EXT_ray_tracing : require
layout(location = 0) rayPayloadInEXT vec4 gne_payload;
void main() {
	gne_payload = vec4(-1.0, 0.0, 0.0, 0.0);
}
)";
	static const char *rt_chit = R"(
#version 460
#extension GL_EXT_ray_tracing : require
layout(location = 0) rayPayloadInEXT vec4 gne_payload;
void main() {
	gne_payload = vec4(gl_HitTEXT, float(gl_PrimitiveID), float(gl_InstanceCustomIndexEXT), 1.0);
}
)";
	String error;
	Vector<uint8_t> rg_sp = rendering_device->shader_compile_spirv_from_source(RD::SHADER_STAGE_RAYGEN, String(rt_raygen), RD::SHADER_LANGUAGE_GLSL, &error);
	if (rg_sp.is_empty()) {
		print_error("[GNE] R0-RT raygen compile failed:");
		print_error(error);
		out.append(-7);
		return out;
	}
	Vector<uint8_t> ms_sp = rendering_device->shader_compile_spirv_from_source(RD::SHADER_STAGE_MISS, String(rt_miss), RD::SHADER_LANGUAGE_GLSL, &error);
	if (ms_sp.is_empty()) {
		print_error("[GNE] R0-RT miss compile failed:");
		print_error(error);
		out.append(-8);
		return out;
	}
	Vector<uint8_t> ch_sp = rendering_device->shader_compile_spirv_from_source(RD::SHADER_STAGE_CLOSEST_HIT, String(rt_chit), RD::SHADER_LANGUAGE_GLSL, &error);
	if (ch_sp.is_empty()) {
		print_error("[GNE] R0-RT closest-hit compile failed:");
		print_error(error);
		out.append(-9);
		return out;
	}
	Vector<RD::ShaderStageSPIRVData> rts;
	RD::ShaderStageSPIRVData st0;
	st0.shader_stage = RD::SHADER_STAGE_RAYGEN;
	st0.spirv = rg_sp;
	rts.push_back(st0);
	RD::ShaderStageSPIRVData st1;
	st1.shader_stage = RD::SHADER_STAGE_MISS;
	st1.spirv = ms_sp;
	rts.push_back(st1);
	RD::ShaderStageSPIRVData st2;
	st2.shader_stage = RD::SHADER_STAGE_CLOSEST_HIT;
	st2.spirv = ch_sp;
	rts.push_back(st2);
	RID rt_shader = rendering_device->shader_create_from_spirv(rts, "gne_rt_selftest");
	if (rt_shader.is_null()) {
		out.append(-10);
		return out;
	}
	RD::PipelineShader ps_rg;
	ps_rg.shader = rt_shader;
	RD::PipelineShader ps_ms;
	ps_ms.shader = rt_shader;
	RD::PipelineShader ps_ch;
	ps_ch.shader = rt_shader;
	LocalVector<RD::PipelineShader> rgs;
	rgs.push_back(ps_rg);
	LocalVector<RD::PipelineShader> mss;
	mss.push_back(ps_ms);
	RD::HitGroup hg;
	hg.closest_hit_shader = ps_ch;
	LocalVector<RD::HitGroup> hgs;
	hgs.push_back(hg);
	RID rt_pipe = rendering_device->raytracing_pipeline_create(rgs.span(), mss.span(), hgs.span(), 1);
	if (rt_pipe.is_null()) {
		out.append(-11);
		return out;
	}
	RID sbt = rendering_device->hit_sbt_create(rt_pipe, 1);
	if (sbt.is_null()) {
		out.append(-12);
		return out;
	}
	if (rendering_device->hit_sbt_set_pipeline(sbt, rt_pipe) != OK) {
		out.append(-13);
		return out;
	}
	RD::HitShaderBindingTableRange sbt_range = rendering_device->hit_sbt_range_alloc(sbt, 1);
	uint32_t group_idx[1] = { 0 };
	rendering_device->hit_sbt_range_update(sbt, sbt_range, 0, Span<uint32_t>(group_idx, 1));
	inst.hit_sbt_range = sbt_range;
	insts[0] = inst;
	if (rendering_device->tlas_build(tlas, insts.span()) != OK) {
		out.append(-14);
		return out;
	}
	RID res = rendering_device->storage_buffer_create(32);
	Vector<RD::Uniform> us;
	RD::Uniform u0;
	u0.uniform_type = RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE;
	u0.binding = 0;
	u0.append_id(tlas);
	us.push_back(u0);
	RD::Uniform u1;
	u1.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	u1.binding = 1;
	u1.append_id(res);
	us.push_back(u1);
	RID uset = rendering_device->uniform_set_create(us, rt_shader, 0);
	if (uset.is_null()) {
		out.append(-15);
		return out;
	}
	RD::RaytracingListID list = rendering_device->raytracing_list_begin();
	rendering_device->raytracing_list_bind_raytracing_pipeline(list, rt_pipe);
	rendering_device->raytracing_list_bind_uniform_set(list, uset, 0);
	rendering_device->raytracing_list_trace_rays(list, 0, sbt, 2, 1, 1);
	rendering_device->raytracing_list_end();
	rendering_device->submit();
	rendering_device->sync();
	Vector<uint8_t> data = rendering_device->buffer_get_data(res, 0, 32);
	float hit_t = -99.0f;
	float miss_marker = -99.0f;
	if (data.size() >= 32) {
		memcpy(&hit_t, data.ptr(), 4);
		memcpy(&miss_marker, data.ptr() + 16, 4);
	}
	out.append(0.0);
	out.append(hit_t);
	out.append(miss_marker);
	out.append(5.0);
	out.append((float)data.size());
	rendering_device->free_rid(uset);
	rendering_device->free_rid(res);
	rendering_device->free_rid(sbt);
	rendering_device->free_rid(rt_pipe);
	rendering_device->free_rid(rt_shader);
	rendering_device->free_rid(tlas);
	rendering_device->free_rid(blas);
	rendering_device->free_rid(ibuf);
	rendering_device->free_rid(vbuf);
	return out;
}