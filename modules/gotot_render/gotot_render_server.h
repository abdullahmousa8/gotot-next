#ifndef GOTOT_RENDER_SERVER_H
#define GOTOT_RENDER_SERVER_H

#include "core/math/color.h"
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

	// GOTOT-010: multi-mesh batch instance rendering. Adds (ADDITIVE ONLY, never
	// replaces) a per-instance mesh_id buffer, a 64-slot mesh table (GototMeshDesc),
	// a prefix-sum batch assembly compute path and a multi-draw indirect path.
	// The 008A/008B/009 mesh path, the 005 billboard path and the 004 culling/HZB/
	// compaction structure are all untouched.
	static constexpr int GOTOT_MESH_TABLE_SIZE = 64;
	static constexpr int GOTOT_MAX_MESH_VERTS = 32768;
	static constexpr int GOTOT_MAX_MESH_INDICES = 65536;
	bool gpu_mesh_batch_valid = false;
	bool gpu_mesh_table_valid = false;
	int mesh_table_count = 0;
	int mesh_next_vertex_offset = 0;
	int mesh_next_index_offset = 0;
	int last_batch_count = 0;
	Color mesh_colors[GOTOT_MESH_TABLE_SIZE];
	RID mesh_id_buffer;              // uint per instance (per-instance mesh_id)
	RID mesh_table_buffer;           // GototMeshDesc[64] (32 bytes each)
	RID mesh_color_buffer;           // vec4[64] per-mesh flat color
	RID batch_count_buffer;          // uint[64] per-mesh visible instance counts
	RID batch_offset_buffer;         // uint[64] prefix-sum batch offsets
	RID mesh_scratch_buffer;         // uint[64 * instance_count] per-mesh staging
	RID batch_instances_buffer;      // uint[instance_count] concatenated orig ids
	RID batch_args_buffer;           // VkDrawIndexedIndirectCommand[64] (INDIRECT usage)
	RID batch_total_buffer;          // uint[1] total number of batches drawn
	RID batch_count_shader;
	RID batch_count_pipeline;
	RID batch_count_uniform_set;
	RID batch_assemble_shader;
	RID batch_assemble_pipeline;
	RID batch_assemble_uniform_set;
	RID mesh_batch_shader;
	RID mesh_batch_pipeline;
	RID mesh_batch_uniform_set;
	RID mesh_010_vertex_array;
	RID mesh_010_index_array;

	// GOTOT-011: multi-batch grouping + dynamic indirect count. ADDS (over 010,
	// additive only) a batch strategy toggle (PER_MESH / GROUPED / REORDERED), a
	// workgroup-parallel prefix-sum batch assembly pass, <=5 batched draw
	// commands via a procedural (non-indexed) indirect path, and a dynamic
	// (GPU-written) draw count used through the draw_list_draw_indirect fallback
	// (SPEC 011 section 3.2: this RD has no indirect-count variant, so the count
	// is read back from batch_total and passed as draw_count).
	//
	// Strategies (default REORDERED):
	//   PER_MESH  - one indirect command per distinct visible mesh (baseline,
	//               byte-identical to 010: 64 meshes -> 64 draw commands).
	//   GROUPED   - active meshes partitioned into <=5 contiguous groups; one
	//               procedural non-indexed command per group draws every member
	//               mesh's instances (64 meshes -> 5 draw commands).
	//   REORDERED - same grouping, draw order explicitly sorted ascending by
	//               mesh_id (proven via gpu_mesh_get_batch_order()).
	// When grouping cannot reduce the command count (<=5 distinct visible meshes,
	// i.e. group_count == active) the assembler falls back to the 010 indexed
	// per-mesh multi-draw so every pre-011 scene (incl. main_010) renders
	// byte-identically regardless of the configured strategy.
	enum GototBatchStrategy {
		GOTOT_BATCH_STRATEGY_PER_MESH = 0,
		GOTOT_BATCH_STRATEGY_GROUPED = 1,
		GOTOT_BATCH_STRATEGY_REORDERED = 2,
	};
	int mesh_batch_strategy = GOTOT_BATCH_STRATEGY_REORDERED;
	bool last_used_group_draw = false;
	int last_group_count = 0;
	RID mesh_vertex_storage_buffer; // storage mirror of the shared vertex buffer
	RID mesh_index_storage_buffer;  // storage mirror of the shared index buffer
	RID group_args_buffer;          // VkDrawIndirectCommand[64] non-indexed (16B)
	RID group_member_count_buffer;  // uint[64] members per group
	RID group_member_list_buffer;   // uint[64*64] member mesh ids per group
	int64_t group_vertex_format = -1;
	RID group_vertex_array;
	RID group_batch_shader;
	RID group_batch_pipeline;
	RID group_batch_uniform_set;

	void _destroy_gpu_scene();
	void _destroy_mesh();
	void _destroy_mesh_batch();
	bool _create_hzb_passes();
	bool _create_raster_pipeline();
	bool _create_mesh_pipeline();
	bool _init_mesh_table_gpu();
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

	// GOTOT-010: batch instance rendering API (TEST-ONLY evidence bridge; not a
	// shipping frame-data path - the authoritative instance transform source
	// remains the GPU scene dispatch from 001B).
	//
	// --- SETUP / AUTHORING ---
	// Overrides a single instance's mesh_id for a later gpu_mesh_batch_dispatch.
	// Writes 4 bytes (uint) at byte offset p_index * 4 in the mesh_id buffer.
	// Mesh ids must be < the current registered mesh count (mesh 0 = the 008A cube
	// when gpu_mesh_create ran; extra meshes come from create_from_arrays).
	void gpu_scene_set_instance_mesh(int p_index, int p_mesh_id);
	// Reads back the mesh_id assigned to p_index (-1 when unset/invalid).
	int gpu_scene_get_instance_mesh(int p_index);
	// Registers a new mesh (positions + indices) into the mesh table as an
	// appended sub-range of the SHARED mesh vertex/index buffers. Returns the new
	// mesh id (table slot) or -1 on failure.
	int gpu_mesh_create_from_arrays(const PackedVector3Array &p_verts, const PackedInt32Array &p_indices);
	// Runs the two compute passes (per-mesh counting + prefix-sum batch assembly
	// with batch_args / batch_instances build). Reads the GPU batch total into
	// last_batch_count. Call AFTER gpu_cull_dispatch/gpu_visibility_dispatch.
	bool gpu_mesh_batch_dispatch();
	// Multi-draw: one draw_list_draw_indirect over the batch_args range
	// [0, batch_count) - draw count == number of DISTINCT meshes with visible
	// instances, not instance count. Depth test/write identical to 009.
	bool gpu_mesh_batch_draw();
	//
	// --- TEST-ONLY GETTERS / EVIDENCE ---
	// Registered mesh count in the table (mesh 0 = cube after gpu_mesh_create).
	int gpu_mesh_get_mesh_id_count() const;
	// Total number of batches (distinct visible meshes) from the last dispatch.
	int gpu_mesh_get_batch_count();
	// Per-mesh visible instance counts (64 entries, indexed by mesh_id).
	PackedInt32Array gpu_mesh_get_draw_counts();
	// The 5 uint VkDrawIndexedIndirectCommand fields at batch index p_batch_index.
	PackedInt32Array gpu_mesh_get_batch_args(int p_batch_index);
	// The flat color the batch fragment shader uses for the given mesh.
	Color gpu_mesh_get_mesh_color(int p_mesh_id) const;

	// GOTOT-011: batch strategy + multi-batch evidence API (TEST-ONLY). All of
	// the 011 extra getters below are pure readback bridges of the GPU state
	// produced by the previous gpu_mesh_batch_dispatch. See GototBatchStrategy.
	// Sets the batch strategy for the NEXT dispatch. Returns false when invalid.
	bool gpu_mesh_set_batch_strategy(int p_strategy);
	// The strategy currently configured (default REORDERED).
	int gpu_mesh_get_batch_strategy() const;
	// Number of BATCH GROUPS produced by the last dispatch (== number of draw
	// commands when grouping is active; >=1 whenever any mesh is visible).
	int gpu_mesh_get_batch_group_count() const;
	// Draw order of the visible meshes, flattened per group (group g's members
	// are listed first). Under REORDERED this is ascending mesh_id order.
	PackedInt32Array gpu_mesh_get_batch_order();
	// The draw count consumed by draw_list_draw_indirect. It originates on the
	// GPU (batch_total readback) and is frame-varying - the SPEC 011 section 3.2
	// fallback for the missing vkCmdDrawIndexedIndirectCount API.
	int gpu_mesh_get_indirect_count() const;
	// Number of indirect draw commands executed by the last gpu_mesh_batch_draw
	// (64 under PER_MESH with 64 visible meshes; <=5 under GROUPED/REORDERED).
	int gpu_mesh_get_draw_call_count() const;
};

#endif