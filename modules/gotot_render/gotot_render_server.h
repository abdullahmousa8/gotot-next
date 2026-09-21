#ifndef GOTOT_RENDER_SERVER_H
#define GOTOT_RENDER_SERVER_H

#include "core/math/plane.h"
#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/object/class_db.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"
#include "servers/rendering/rendering_device.h"

class GototRenderServer : public Object {
	GDCLASS(GototRenderServer, Object);

	static GototRenderServer *server_singleton;

	RenderingDevice *rendering_device = nullptr;

	// GOTOT-001B: GPU Scene (SoA instance buffers + compute fill).
	int gpu_instance_count = 0;
	float gpu_scene_spread = 200.0f;
	bool gpu_scene_valid = false;
	RID transform_buffer;
	RID bounds_buffer;
	RID instance_id_buffer;
	RID compute_shader;
	RID compute_pipeline;
	RID uniform_set;

	// GOTOT-002: GPU frustum culling.
	bool gpu_cull_valid = false;
	bool frustum_valid = false;
	Plane frustum_planes[6];
	RID visibility_buffer;
	RID visible_count_buffer;
	RID compact_buffer;
	RID cull_shader;
	RID cull_pipeline;
	RID cull_uniform_set;

	// GOTOT-003: GPU indirect draw args.
	bool gpu_drawargs_valid = false;
	RID indirect_args_buffer;
	RID drawargs_shader;
	RID drawargs_pipeline;
	RID drawargs_uniform_set;

	// GOTOT-004: Hierarchical Z occlusion.
	bool gpu_hzb_valid = false;
	bool camera_view_valid = false;
	static constexpr int HZB_TEXEL_COUNT = 512;
	static constexpr int HZB_LEVELS = 10;
	float hzb_viewport_w = 1920.0f;
	float hzb_viewport_h = 1080.0f;
	float far_plane = 2000.0f;
	float tan_half_fov_v = 0.0f;
	int occluder_count = 0;
	RID hzb_array;
	RID occluder_min_buffer;
	RID occluder_max_buffer;
	RID view_ubo;
	RID hzb_clear_shader;
	RID hzb_clear_pipeline;
	RID hzb_clear_uniform_set;
	RID hzb_occ_shader;
	RID hzb_occ_pipeline;
	RID hzb_occ_uniform_set;
	RID hzb_down_shader;
	RID hzb_down_pipeline;
	RID hzb_down_uniform_set;
	float last_vp[16];

	// GOTOT-005: GPU indirect raster draw (VkDrawIndexedIndirect on the compacted list).
	bool gpu_raster_valid = false;
	static constexpr int RASTER_TARGET_W = 1920;
	static constexpr int RASTER_TARGET_H = 1080;
	int64_t raster_framebuffer_format = -1;
	RID raster_color_texture;
	RID raster_framebuffer;
	RID raster_shader;
	RID raster_pipeline;
	RID raster_uniform_set;
	RID quad_index_buffer;
	RID quad_index_array;

	// GOTOT-009: real depth buffer (D32_SFLOAT) attached to the raster
	// framebuffer. The billboard path keeps depth disabled (unchanged
	// behavior); the real-mesh path tests/writes depth.
	bool raster_depth_attached = false;
	int raster_depth_format_value = -1;
	RID raster_depth_texture;
	bool mesh_depth_enabled = false;
	bool raster_depth_enabled = false;

	// GOTOT-008A: independent REAL MESH path (real vertex buffer + real index
	// buffer + real vertex format + indexed indirect draw). Additive only: the
	// GOTOT-005 billboard path above is never replaced or modified.
	bool gpu_mesh_valid = false;
	int mesh_vertex_count = 0;
	int mesh_index_count = 0;
	int64_t mesh_vertex_format = -1;
	RID mesh_vertex_buffer;
	RID mesh_index_buffer;
	RID mesh_vertex_array;
	RID mesh_index_array;
	RID mesh_shader;
	RID mesh_pipeline;
	RID mesh_uniform_set;
	RID mesh_drawargs_shader;
	RID mesh_drawargs_pipeline;
	RID mesh_drawargs_uniform_set;

	void _destroy_gpu_scene();
	void _destroy_mesh();
	bool _create_hzb_passes();
	bool _create_raster_pipeline();
	bool _create_mesh_pipeline();
	void _run_compute_pass(RID p_pipeline, RID p_uniform_set, const void *p_push_data, uint32_t p_push_size, uint32_t p_groups_x, uint32_t p_groups_y, uint32_t p_groups_z);
	static float _projection_tan_half_fov_v(const Projection &p_projection);

protected:
	static void _bind_methods();

public:
	GototRenderServer();
	~GototRenderServer();

	static void set_server_singleton(GototRenderServer *p_server);
	static GototRenderServer *get_server_singleton();

	void initialize();
	void shutdown();

	bool is_initialized() const;
	bool ensure_gpu_device();
	bool is_gpu_ready() const;

	// GOTOT-001B: GPU Scene API.
	bool gpu_scene_create(int p_instance_count, float p_spread);
	bool gpu_scene_dispatch(int p_seed);
	PackedVector3Array gpu_scene_readback_positions(int p_index, int p_count);
	PackedFloat32Array gpu_scene_readback_scales(int p_index, int p_count);
	Dictionary gpu_scene_stats();
	int gpu_scene_get_instance_count() const;
	void gpu_scene_destroy();

	// GOTOT-002: GPU frustum culling API.
	void gpu_scene_set_camera(const Transform3D &p_camera_transform, const Projection &p_projection);
	bool gpu_cull_dispatch();
	int gpu_cull_get_visible_count();
	PackedInt32Array gpu_cull_get_visibility();
	PackedVector4Array gpu_scene_get_frustum_planes();

	// GOTOT-003: GPU indirect draw args API.
	bool gpu_drawargs_finalize();
	PackedInt32Array gpu_drawargs_read();
	PackedInt32Array gpu_compact_read();

	// GOTOT-004: HZB occlusion API.
	void gpu_scene_set_viewport(float p_viewport_w, float p_viewport_h);
	void gpu_scene_set_occluders(const Vector<Vector4> &p_occluders);
	bool gpu_visibility_dispatch();

	// GOTOT-005: GPU indirect raster draw API.
	bool gpu_raster_indirect_draw();
	PackedByteArray gpu_raster_read_pixels();
	PackedFloat32Array gpu_scene_get_vp();

	// GOTOT-009: real depth buffer API.
	// The raster framebuffer carries a 1920x1080 D32_SFLOAT depth attachment
	// cleared to 1.0 (far) every frame; the REAL MESH pipeline (008A) is the
	// only depth consumer (depth test + write, COMPARE_OP_LESS_OR_EQUAL); the
	// billboard (raster) pipeline keeps depth disabled (unchanged).
	//
	// --- TEST-ONLY --- (overlay proof scenes only; not part of the runtime
	// scene-generation pipeline - the authoritative instance transform source
	// remains the GPU scene dispatch from 001B):
	// Overrides a single instance's position/scale in the transform buffer for
	// fixed-layout repro scenes (e.g. main_009's A/B/C/D overlay). Writes
	// 16 bytes = vec4(position, scale) at byte offset p_index * 16.
	void gpu_scene_set_instance_transform(int p_index, const Vector3 &p_position, float p_scale);
	//
	// --- TEST-ONLY / DIAGNOSTIC --- (verification readback bridge, same class
	// as gpu_raster_read_pixels - NOT a shipping frame-data path):
	// Returns the FULL depth image as PackedFloat32Array of size W*H
	// (1920*1080 = 2,073,600 floats, row-major, value 1.0 == far/clear,
	// smaller == nearer) via a blocking GPU readback. Range [-1,1]-safe: 0..1.
	PackedFloat32Array gpu_raster_read_depth();
	//
	// --- TEST-ONLY GETTERS --- (probe the 009 wiring from GDScript).
	// Returns the depth attachment format enum value (125 == D32_SFLOAT) or -1
	// when no depth attachment is attached.
	int gpu_raster_get_depth_format() const;
	// True when the mesh pipeline has depth test+write enabled (009 wire check).
	bool gpu_mesh_get_depth_enabled() const;
	// True when the billboard (raster) pipeline has depth enabled. Must stay
	// FALSE for 009 (the raster path intentionally keeps depth disabled).
	bool gpu_raster_get_depth_enabled() const;
	// NOTE (architect): gpu_*_get_depth_* are consolidation candidates - they
	// may merge into a single capabilities/introspection query in a later
	// milestone (no change in 009, recorded only).

	// GOTOT-008A: independent real-mesh (indexed indirect) API.
	bool gpu_mesh_create();
	bool gpu_mesh_drawargs_finalize();
	bool gpu_mesh_indirect_draw();
	int gpu_mesh_get_index_count() const;
	int gpu_mesh_get_vertex_count() const;
};

#endif