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
	RID raster_viewz_texture; // R32_SFLOAT view-space depth (GOTOT-012 pyramid source).
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

	// GOTOT-012: production HZB (SPEC 012, additive over 004/010/011).
	// A 2048x2048 R32UI 2D-array pyramid (12 levels = log2(2048)+1, the SPEC
	// minimum) is built EVERY frame from the PREVIOUS frame's actual D32_SFLOAT
	// raster depth (the production occluder source) plus the optional 004 box
	// occluder supplement; occlusion is then applied as a SEPARATE second phase
	// over the phase-1 frustum survivors, and the surviving list feeds the SAME
	// compact[]/visible_count[] that the 011 batch assembler consumes. All build
	// and test work happens on the GPU (the only CPU reads are tiny 4-byte count
	// readbacks on the verification/evidence bridge, same class as
	// gpu_cull_get_visible_count). Temporal coherence: the pyramid may only be
	// used with the SAME camera vp that wrote the previous depth; when the vp
	// changed the pyramid is considered stale -> hzb_valid=0 (frustum-only,
	// conservative, no false dropout) and once the camera rests the pyramid is
	// rebuilt from the fresh depth. hzb_coherent == true after >=2 consecutive
	// same-vp builds (static camera + static geometry). The non-occluded control
	// (no prior depth) yields phase2 == phase1 exactly.
	static constexpr int HZB_PROD_TEXEL_COUNT = 2048;
	static constexpr int HZB_PROD_LEVELS = 12;
	// Flat storage-buffer mirror of the pyramid (all levels concatenated in
	// level order): 2048^2 * (1 + 1/4 + ... + 1/4^11) = 5,592,405 uints. The
	// occlusion passes read THIS (storage buffers are reliable in this RDG
	// fork, while same-submission GPU image reads are not); the image array is
	// kept for CPU readback evidence only.
	static constexpr int HZB_PYRAMID_DATA_UINTS = 5592405;
	bool gpu_hzb_prod_valid = false;
	// Set by gpu_hzb_build when pyramid rebuild work is queued; the actual GPU
	// passes run inside the NEXT gpu_mesh_batch_draw submission (same submission
	// as the draw - sampling an attachment written by an EARLIER submission
	// returns the pre-draw version in this RDG fork, so the build must share the
	// draw's command stream).
	bool hzb_rebuild_requested = false;
	bool hzb_temporal_enabled = true;
	bool hzb_pyramid_fresh = false;
	bool hzb_coherent = false;
	int hzb_stable_frames = 0;
	float hzb_inflate_factor = 2.0f;
	int hzb_phase1_count = 0;
	int hzb_phase2_count = 0;
	float hzb_proj_a = 0.0f;
	float hzb_proj_b = 0.0f;
	float hzb_build_vp[16];
	RID hzb_prod_array;
	RID hzb_prod_clear_set;
	RID hzb_prod_occ_set;
	RID hzb_prod_down_set;
	RID hzb_depth_sampler;
	RID hzb_dbg_probe_buffer; // GOTOT-012 diagnostics (count_gt0 / max_inv / probe d / probe ndc).
	RID hzb_depth_source_shader;
	RID hzb_depth_source_pipeline;
	RID hzb_depth_source_uniform_set;
	RID hzb_occbuf_shader;
	RID hzb_occbuf_pipeline;
	RID hzb_occbuf_uniform_set;
	RID hzb_phase1_shader;
	RID hzb_phase1_pipeline;
	RID hzb_phase1_uniform_set;
	RID hzb_phase2_shader;
	RID hzb_phase2_pipeline;
	RID hzb_phase2_uniform_set;
	RID phase1_compact_buffer;
	RID phase1_count_buffer;
	RID hzb_pyramid_data_buffer;

	// GOTOT-013: Meshlets / LOD / cluster culling (SPEC 013). meshoptimizer is
	// used OFFLINE ONLY (tools/meshlet_import builds the .gomlet asset); the
	// runtime NEVER links meshoptimizer. Outcome is fully additive: loading a
	// .gomlet mesh uploads storage buffers, a cluster-cull compute pass picks
	// the LOD per instance by distance and frustum/backface-cone culls every
	// meshlet (dense id mapping, deterministic), and a 3-pass software
	// rasterizer (select -> commit bary + covered count -> per-LOD pixel cover)
	// proves the culling is a real render front end (sub-pixel triangles are
	// made visible - SPEC 013 criterion 6).
	enum {
		ML_MAX_LODS = 4,
		ML_STATE_HEADER = 10, // total, visible, subpixel, covered, vis_lod0..2, px_lod0..2
	};
	float meshlet_camera_position[3] = { 0.0f, 0.0f, 2000.0f };
	bool gpu_meshlet_valid = false;
	int ml_lod_count = 0;
	int ml0_max = 0; // LOD0 meshlet count == dense grid stride (instance_count x ml0_max)
	int ml_instance_count = 0;
	int ml_total_vertices = 0;
	int ml_total_tris = 0;
	int ml_total_meshlets = 0;
	int ml_lod0_tri_count = 0;
	int ml_lod0_meshlet_count = 0;
	int ml_state_uint_count = 0;
	float ml_lod_t0 = 2200.0f;
	float ml_lod_t1 = 3200.0f;
	int ml_vis_w = 1920;
	int ml_vis_h = 1080;
	RID ml_ubo;
	RID ml_vert_buffer;
	RID ml_tri_buffer;
	RID ml_desc_buffer;
	RID ml_state_buffer;
	RID ml_debug_buffer;
	RID ml_vis_buffer;
	RID ml_cull_shader;
	RID ml_cull_pipeline;
	RID ml_cull_uniform_set;
	RID ml_raster_shader;
	RID ml_raster_pipeline;
	RID ml_raster_uniform_set;

	// GOTOT-014: GPU Scene Manager (SPEC 014, additive TEST-ONLY evidence
	// bridge). A GPU-resident scene database over a unified id space:
	//   64-byte AoS record per id (transform, bounds, meshlet_ordinal +
	//   mesh_ref + flags, per-instance LOD config);
	//   a 16 MB CPU write-back update ring (deltas applied on GPU);
	//   a compute apply pass (consumes the ring - add/remove/move),
	//   a compact pass (dense ascending-id active list) and
	//   a snapshot pass (32-byte draw records = 015 render-graph hand-off).
	// Nothing here touches the 001B scene, the 008A/010/011 mesh batch path,
	// the 009 depth buffer or the 013 meshlet pipeline: allocations are fully
	// separate buffers and the manager only mirrors the 013 meshlet ordinal
	// space + LOD config in its draw records (hand-off evidence).
	enum {
		GMS_RING_BYTES = 16 * 1024 * 1024,
		GMS_DELTA_BYTES = 80,
		GMS_RECORD_BYTES = 64,
		GMS_DRAW_RECORD_BYTES = 32,
		GMS_MAX_CAPACITY = 4000000,
		GMS_STATS_UINTS = 16,
		GMS_MESH_SLOTS = 64,
	};
	bool gpu_scene_mgr_valid = false;
	int gms_capacity = 0;
	int gms_active_cpu = 0;     // CPU mirror (set_instances / update accounting)
	uint32_t gms_ring_tail = 0; // CPU write cursor (bytes); head is always 0
	uint32_t gms_dispatch_seq = 0;
	RID gms_record_buffer;
	RID gms_active_buffer;       // uint[capacity] dense ascending-id list
	RID gms_active_count_buffer; // uint[1]
	RID gms_ring_buffer;         // GMS_RING_BYTES update ring
	RID gms_snapshot_buffer;     // 32-byte draw records
	RID gms_stats_buffer;        // uint[16]
	RID gms_mesh_count_buffer;   // uint[64] per mesh_ref draw-record counts
	RID gms_shader;
	RID gms_pipeline;
	RID gms_uniform_set;

	void _destroy_meshlet();
	void _destroy_scene_manager();
	bool _upload_meshlet_view();
	void _destroy_hzb_prod();
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
	float gpu_raster_read_viewz(int p_x, int p_y);
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

	// GOTOT-012: production HZB API (TEST-ONLY evidence bridge, additive over
	// the 004/010/011 paths - when not created the pre-012 dispatch behavior is
	// byte-identical, which keeps every 001A..011 signature unchanged).
	// Creates the 2048^2 x 12-level R32UI pyramid, the phase-1/phase-2 shaders
	// and the two-phase buffers. Requires a created GPU scene (with the raster
	// D32 depth attachment). Idempotent.
	bool gpu_hzb_prod_create();
	// Builds the production pyramid from the PREVIOUS frame's D32 depth (+ the
	// optional 004 box occluders) and performs the temporal-coherence
	// bookkeeping. Safe (no-op returning true) on the first frame when the depth
	// has not been written yet (hzb_valid stays 0 = frustum-only).
	bool gpu_hzb_build();
	// TWO-PHASE production dispatch: phase 1 = frustum-only cull into a phase-1
	// list, phase 2 = HZB occlusion over that list writing the FINAL
	// compact[]/visible_count[] consumed by gpu_mesh_batch_dispatch (GOTOT-011).
	// Returns false when the production resources are not available.
	bool gpu_visibility_prod_dispatch();
	// Enables/disables the temporal-coherence reuse of the previous-frame pyramid
	// (TEST toggle for evidence comparisons). When disabled hzb_coherent is false
	// and the conservative inflate factor is applied to the occlusion test.
	void gpu_hzb_enable_temporal(bool p_enabled);
	// Alias of gpu_scene_set_occluders (SPEC 012 API name; box supplements to the
	// production pyramid, additive to the depth source).
	void gpu_hzb_set_occluders(const Vector<Vector4> &p_occluders);
	// Number of pyramid levels: 12 when the production HZB is active, else the
	// 004 legacy 10, else 0.
	int gpu_hzb_get_level_count();
	// [phase1_count, phase2_count] from the last production dispatch (verify-bridge).
	PackedInt32Array gpu_hzb_get_phase_counts();
	// True when the camera vp has been stable for >=2 consecutive builds
	// (static camera + static geometry => exact pyramid reuse).
	bool gpu_hzb_get_coherent() const;

	// Verify-bridge probes (GOTOT-012 debug): read the UBO hzb_valid the phase-2
	// test sees, and one pyramid level-0 texel (inverted depth, far - z_view).
	int gpu_hzb_dbg_valid();
	int gpu_hzb_dbg_level0(int p_x, int p_y);
	PackedInt32Array gpu_hzb_dbg_probe();
	PackedInt32Array gpu_hzb_dbg_scan_level0();
	PackedInt32Array gpu_hzb_dbg_scan_level1(int p_level);
	PackedInt32Array gpu_hzb_dbg_scan_buffer(int p_level);
	PackedInt32Array gpu_hzb_dbg_sim2();

	// GOTOT-013: meshlet API (TEST-ONLY evidence bridge; additive - until
	// gpu_meshlet_load runs every earlier signature is byte-identical).
	// Loads a GOTOML11 ".gomlet" mesh (cf. tools/meshlet_import/main.cpp),
	// builds the GPU buffers/shader/uniform sets and bakes in the current GPU
	// scene's instance count. Requires gpu_scene_create first. Idempotent
	// (reload replaces the previous meshlet state).
	bool gpu_meshlet_load(const PackedByteArray &p_data);
	bool gpu_meshlet_load_path(const String &p_path);
	bool gpu_meshlet_set_lod_thresholds(float p_t0, float p_t1);
	// Cluster cull: LOD by distance + frustum + backface cone. Writes
	// instance_lod[], the dense visible_flag set and the total/visible counters.
	bool gpu_meshlet_cull_dispatch();
	// Software rasterize the SURVIVING clusters into ml_vis (3 barrier-separated
	// passes). Reads the visible_flag set written by the last cull dispatch.
	bool gpu_meshlet_raster_dispatch();
	// Introspection (counts baked at load / last dispatch).
	int gpu_meshlet_get_total_meshlets() const;
	int gpu_meshlet_get_lod0_tri_count() const;
	int gpu_meshlet_get_lod0_meshlet_count() const;
	PackedInt32Array gpu_meshlet_stats();
	// Evidience from the last cull: [total_slots, visible_slots, vis_lod0,
	// vis_lod1, vis_lod2, px_lod0, px_lod1, px_lod2].
	PackedInt32Array gpu_meshlet_get_cluster_counts();
	PackedInt32Array gpu_meshlet_get_instance_lods();
	PackedInt32Array gpu_meshlet_get_cull_debug();
	// Raster evidence from the last raster dispatch (proves criterion 6):
	// [covered_px, subpixel_tris, winner_px, fnv1a_over_winner_ids].
	PackedFloat32Array gpu_meshlet_raster_evidence();
	void gpu_meshlet_destroy();

	// GOTOT-014: GPU Scene Manager API (TEST-ONLY evidence bridge; additive -
	// every pre-014 signature is byte-identical until a manager alloc runs).
	// Allocates a GPU scene database of p_max_instances unified ids (record
	// buffer + active list + snapshot + 16 MB update ring). Returns false when
	// the device/capacity is unavailable. Re-alloc is destructive.
	bool gpu_scene_manager_alloc(int p_max_instances);
	// Bulk-writes p_instances (16 floats per 64-byte record: transform,
	// bounds, refs orb/mesh/flags, lodcfg t0/t1) as ids 0..N-1, all active;
	// resets the ring and active state. N must be <= capacity.
	bool gpu_scene_manager_set_instances(const PackedFloat32Array &p_instances);
	// Queues deltas into the 16 MB ring for the next dispatch. Each delta is
	// 20 floats (80 bytes): [op 1=add 2=remove 3=move, id, seq, flags,
	// transform, bounds, refs, lodcfg]. Add/move write the record fields;
	// remove clears the active bit. Returns false when the ring overflows.
	bool gpu_scene_manager_update(const Array &p_deltas);
	// GPU apply (consumes the ring) + compact (dense active ids) + snapshot
	// (draw records + per-mesh counts). Zero readbacks in the critical path.
	bool gpu_scene_manager_dispatch();
	// Evidence dict: capacity/active/applied add-remove-move counts, ring
	// used/free bytes, snapshot record count, distinct meshes, 011 group
	// count (min(distinct,5)), SSBO footprint, dispatch seq.
	Dictionary gpu_scene_manager_get_stats();
	// [snap_records, distinct_meshes, group_count_011] + 64 per-mesh counts.
	PackedInt32Array gpu_scene_manager_get_draw_counts();
	// First u32 (meshlet_ordinal) of the first p_count draw records.
	PackedInt32Array gpu_scene_manager_get_snapshot(int p_count);
	// First p_count ids of the compacted dense active list.
	PackedInt32Array gpu_scene_manager_get_active_ids(int p_count);
	void gpu_scene_manager_destroy();
};

#endif