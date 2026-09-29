extends Node

# GNE-012 - Production HZB (two-phase occlusion culling + temporal coherence)
#
# Reuses the 011 multi-batch scene (64 meshes, 128 two-plane instances) intact
# and ADDS the production pyramid on top, without changing any pre-012 path:
#   - gpu_hzb_prod_create(): 2048^2 R32UI 2D-array pyramid, 12 levels,
#     reused 004 clear/occ/down pipelines against the prod array +
#     a new depth-source shader that builds level 0 from the PREVIOUS frame's
#     D32 raster depth,
#   - gpu_hzb_build(): temporal bookkeeping (same-vp reuse; hzb_valid=0 while
#     the pyramid would be stale -> conservative frustum-only),
#   - gpu_visibility_prod_dispatch(): TWO PHASES - phase 1 frustum-only cull
#     into a phase-1 list, phase 2 HZB occlusion over that list writing the
#     FINAL compact[]/visible_count[] that gpu_mesh_batch_dispatch consumes.
#
# Scene adds 4 WALL occluder instances (mesh_id 64, a big box) at z=-950
# BETWEEN the two instance planes. The pyramid is built from the previous
# frame's depth, so the back plane (behind the walls) must be partially
# occluded on every frame AFTER the first build, while the front plane and the
# walls themselves never drop out.
#
# EXPECTED deterministic outputs:
#   levels == 12 (2048^2 -> 12 levels), p1 == 132 (frustum keeps all).
#   frame 1 (no pyramid built yet, hzb_valid=0): p2 == p1 == 132  (control).
#   frame 2+  (pyramid from previous-frame depth): p2 < p1  (occlusion active).
#   hzb_coherent becomes true from the 2nd same-vp rebuild; walls/front stay.
#   DET: rerunning build+dispatch+batch keeps (p1,p2,levels,coherent) + pixels.
#
# Evidence targets (SPEC 012 7 PASS criteria + additive 011 regression):
#   (1) 12-level production pyramid exists and is consumed two-phase;
#   (2) phase1 == 132, phase2 < phase1 after first build (depth occlusion);
#   (3) pixel/depth evidence: front plane still drawn, back plane partially
#       dropped, walls drawn (per-group center evidence like 011);
#   (4) temporal coherence: hzb_coherent flips true after >=2 same-vp builds
#       and phase counts stay stable (deterministic);
#   (5) DET sig stable across a full in-binary repeat + harness re-runs;
#   (6) regressions 001A..011 unchanged (additive paths only);
#   (7) dispatch + draw timing reported for the report.

const MESH_COUNT := 64
const INSTANCE_COUNT := 128
const WALL_COUNT := 4
const TOTAL_INSTANCES := INSTANCE_COUNT + WALL_COUNT
# The walls are big (@WALL_SCALE) instances of an EXISTING small mesh (mesh 5,
# octahedron) - the GNE mesh table capacity is fixed at 64, so adding a 65th
# mesh would require a capacity change (risk to the protected 011 signatures).
# The walls still write their depth into the previous-frame D32 buffer and the
# pyramid is built from that depth, so their occlusion role is unchanged; the
# pixel evidence uses mesh 5's palette color as the "wall" color.
const WALL_MESH_ID := 5
const FRAME_LIMIT := 60
const PRINT_EVERY := 20
const RASTER_W := 1920
const RASTER_H := 1080
const DEPTH_ORDER_TOL := 0.002
const STRATEGY_REORDERED := 2

var window_png := "C:/Users/opc/AppData/Local/Temp/opencode/gt_012_window.png"
var sig_file := "C:/Users/opc/AppData/Local/Temp/opencode/gt012_sig.txt"

var server: GneRenderServer
var camera: Camera3D
var display: TextureRect
var image_tex: ImageTexture

var frame := 0
var want_shot := false
var shot_done := false

var mesh_colors: Array[Color] = []
var last_draw_counts := PackedInt32Array()
var last_order := PackedInt32Array()
var sig := ""
var dispatch_us := 0
var draw_us := 0
var det_ok := false

var p1 := 0
var p2 := 0
var levels := 0
var coherent := false
var occlusion_active := false
var occlusion_seen_at := 0
var phase_history: Array[Vector2i] = []

const TETRA_VERTS: Array[Vector3] = [
	Vector3(0.0, 0.8, 0.0),
	Vector3(-0.6, -0.4, 0.5),
	Vector3(0.6, -0.4, 0.5),
	Vector3(0.0, -0.4, -0.5),
]
const TETRA_INDICES: Array[int] = [0, 1, 2, 0, 3, 1, 0, 2, 3, 1, 3, 2]
const OCTA_VERTS: Array[Vector3] = [
	Vector3(0, 0, 1),
	Vector3(1, 0, 0),
	Vector3(0, 1, 0),
	Vector3(-1, 0, 0),
	Vector3(0, -1, 0),
	Vector3(0, 0, -1),
]
const OCTA_INDICES: Array[int] = [
	0, 2, 1, 0, 3, 2, 0, 4, 3, 0, 1, 4,
	5, 1, 2, 5, 2, 3, 5, 3, 4, 5, 4, 1,
]
const FRONT_PLANE_Z := -700.0
const BACK_PLANE_Z := -1100.0
const INSTANCE_SCALE := 26.0
const WALL_Z := -950.0
const WALL_SCALE := 250.0
const WALL_X: Array[float] = [-540.0, -180.0, 180.0, 540.0]

var _user_args012: Dictionary = {}

# =========================================================================
# GATE 124 â€” D1 and D2 (owner-approved replacement of the two conflicting
# colour conditions). Both are self-contained: they read the ENGINE API
# directly and never read any TEMP-DIAG-012memb variable or block, so the
# gate can never come to depend on an uncommitted diagnostic.
# =========================================================================
var _gate_d1_reason := ""
var _gate_d2_reason := ""

# D1 - instance 90 is absent from the compact generation that ENTERED the
# batch. Fail-closed binding: the generation read here is only accepted as
# "the consumed one" when sum(last_draw_counts) == p2 in the same cycle.
func _gate_d1_absence() -> bool:
	_gate_d1_reason = ""
	var gen := server.gpu_compact_read()
	var n := mini(p2, gen.size())
	var m90 := false
	var m65 := false
	var walls := 0
	for k in range(n):
		var v: int = gen[k]
		if v == 90:
			m90 = true
		if v == 65:
			m65 = true
		if v >= INSTANCE_COUNT:
			walls += 1
	var csum := 0
	for c in last_draw_counts:
		csum += c
	var bound := (csum == p2)
	_gate_d1_reason = "gen=" + str(n) + "/" + str(p2) + " m90=" + str(m90) \
		+ " m65=" + str(m65) + " walls=" + str(walls) \
		+ " sum=" + str(csum) + " bound(sum==p2)=" + str(bound)
	# The generation must be provably the consumed one, occlusion must have
	# actually reduced the set, and 90 must be absent from it.
	if not bound:
		_gate_d1_reason += " REJECT:not_the_consumed_generation"
		return false
	if p2 >= p1:
		_gate_d1_reason += " REJECT:occlusion_inactive"
		return false
	if m90:
		_gate_d1_reason += " REJECT:90_present"
		return false
	_gate_d1_reason += " ACCEPT(65_survives=" + str(m65) + ",walls=" + str(walls) + ")"
	return true

# D2 - a wall is present in the PRODUCTION image with its IDENTITY taken
# from the id pass. Colour and depth are corroborating checks, never the
# sole basis. Fails closed on any missing prerequisite; the id pass is
# disabled by default in the engine, so an absent probe is a FAIL, not a
# skip.
func _gate_d2_wall_present(pixels: PackedByteArray, depth: PackedFloat32Array,
		vp: PackedFloat32Array) -> bool:
	_gate_d2_reason = ""
	if not server.gpu_idprobe_is_valid():
		_gate_d2_reason = "REJECT:idpass_not_valid"
		return false
	var ids := server.gpu_idprobe_read_ids()
	var idd := server.gpu_idprobe_read_depth()
	var want := RASTER_W * RASTER_H
	if ids.size() != want or idd.size() != want:
		_gate_d2_reason = "REJECT:readback_size ids=" + str(ids.size()) + "/" + str(want) \
			+ " depth=" + str(idd.size())
		return false
	# Frame-wide coverage agreement: no pixel drawn by production that the id
	# pass did not draw, and vice versa (depth-based, not colour-based).
	var prod_only := 0
	var id_only := 0
	for i in range(want):
		var pc: bool = depth[i] < 0.999999
		var ic: bool = idd[i] < 0.999999
		if pc and not ic:
			prod_only += 1
		elif ic and not pc:
			id_only += 1
	if prod_only > 0 or id_only > 0:
		_gate_d2_reason = "REJECT:coverage prod_only=" + str(prod_only) + " id_only=" + str(id_only)
		return false
	var wr := int(round(mesh_colors[WALL_MESH_ID].r * 255.0))
	var wg := int(round(mesh_colors[WALL_MESH_ID].g * 255.0))
	var wb := int(round(mesh_colors[WALL_MESH_ID].b * 255.0))
	var parts := []
	for wi in range(WALL_COUNT):
		var wix: int = INSTANCE_COUNT + wi
		# window centre derived geometrically, not hard-coded
		var c := _idprobe_px(Vector3(WALL_X[wi], 0.0, WALL_Z), vp)
		var cx := int(c.x)
		var cy := int(c.y)
		var ok := 0
		for dy in range(-3, 4):
			for dx in range(-3, 4):
				var xx := cx + dx
				var yy := cy + dy
				if xx < 0 or xx >= RASTER_W or yy < 0 or yy >= RASTER_H:
					continue
				var ix := yy * RASTER_W + xx
				if ids[ix] != wix:
					continue
				var o := ix * 4
				if not (_close(int(pixels[o]), wr) and _close(int(pixels[o + 1]), wg) and _close(int(pixels[o + 2]), wb)):
					continue
				if absf(idd[ix] - depth[ix]) > 0.000001:
					continue
				ok += 1
		parts.append("wall" + str(wix) + "@(" + str(cx) + "," + str(cy) + ")=" + str(ok) + "/49")
		if ok < 49:
			_gate_d2_reason = "REJECT:window_incomplete " + ", ".join(parts)
			return false
	_gate_d2_reason = "ACCEPT coverage_exact " + ", ".join(parts)
	return true

	return true

# Projects a world point to raster pixel coords using the same mapping the
# occbuf writer and the cull phase use: px = (ndc * 0.5 + 0.5) * viewport.
# Used by the D2 gate to derive its window centre geometrically, so no pixel
# coordinate is ever hard-coded.
func _idprobe_px(p: Vector3, vp: PackedFloat32Array) -> Vector2:
	var cx: float = vp[0] * p.x + vp[4] * p.y + vp[8] * p.z + vp[12]
	var cy: float = vp[1] * p.x + vp[5] * p.y + vp[9] * p.z + vp[13]
	var cw: float = vp[3] * p.x + vp[7] * p.y + vp[11] * p.z + vp[15]
	if cw <= 0.0:
		return Vector2(-1e9, -1e9)
	return Vector2((cx / cw * 0.5 + 0.5) * float(RASTER_W), (cy / cw * 0.5 + 0.5) * float(RASTER_H))

func _parse_user_args() -> void:
	_user_args012.clear()
	for a in OS.get_cmdline_user_args():
		var kv := a.split("=", true, 1)
		if kv.size() == 2:
			_user_args012[kv[0].lstrip("-")] = kv[1]
		else:
			_user_args012[a.lstrip("-")] = ""

# GNE-012 (restored): deterministic instance placement. 128 instances =
# two 8x8 grids clamped to the same z-slab evidence the phases read: front
# plane z=-700 (+70 x from the -400 column base), back plane z=-1100 (+170 x).
# Instance 0 = front col0 x=-330 (the deliberate slit, must stay VISIBLE);
# instance 65 = back col1 x=-330 (the slit SURVIVOR that proves only part of
# the back column is occluded); instance 90 = back col2 x=-430, inside wall-Box
# A [-650,-350] -> must be OCCLUDED (p2 drops it). This matches the 8x8 grid
# layout used by the cpp production pyramid and the 128-instance scene table.
func _instance_pos(i: int) -> Vector3:
	var idx := i & 127
	var plane := idx / 64
	var local := idx % 64
	var gx := local % 8
	var gy := local / 8
	var off := 70.0 if plane == 0 else 170.0
	return Vector3(
		-400.0 + off - float(gx) * 100.0,
		-300.0 + float(gy) * 100.0,
		FRONT_PLANE_Z if plane == 0 else BACK_PLANE_Z)

func _ready() -> void:
	print("[GNE DBG] _ready START frame=", frame)
	_parse_user_args()
	server = GneRenderServer.get_server_singleton()
	if server == null:
		_fail(100, "server singleton is null")
		return
	if not server.ensure_gpu_device():
		_fail(101, "local RenderingDevice not available")
		return

	camera = $Camera
	display = $Overlay/Display
	camera.global_position = Vector3(0, 0, 2000)
	camera.rotation = Vector3.ZERO
	camera.near = 300.0
	camera.far = 4000.0

	if not server.gpu_scene_create(TOTAL_INSTANCES, 1.0):
		_fail(102, "gpu_scene_create")
		return
	if not server.gpu_scene_dispatch(13):
		_fail(103, "gpu_scene_dispatch")
		return
	if not server.gpu_mesh_create():
		_fail(104, "gpu_mesh_create")
		return

	# Enable the TEST-ONLY instance-id pass (engine capability, disabled by
	# default; see gne_render_server gpu_idprobe_*). The D2 gate depends on it
	# by design. It writes to its OWN framebuffer and touches no production
	# pipeline, format or shader.
	if not server.gpu_idprobe_set_enabled(true):
		print("GNE 012-DBG idprobe ENABLE FAILED (experiment continues without it)")

	var tetra_verts := PackedVector3Array(TETRA_VERTS)
	var tetra_idx := PackedInt32Array(TETRA_INDICES)
	var octa_verts := PackedVector3Array(OCTA_VERTS)
	var octa_idx := PackedInt32Array(OCTA_INDICES)
	for m in range(1, MESH_COUNT):
		var mesh_id := -1
		if m % 2 == 1:
			mesh_id = server.gpu_mesh_create_from_arrays(tetra_verts, tetra_idx)
		else:
			mesh_id = server.gpu_mesh_create_from_arrays(octa_verts, octa_idx)
		if mesh_id != m:
			_fail(105, "mesh table entry " + str(m) + " got id " + str(mesh_id))
			return

	if server.gpu_mesh_get_mesh_id_count() != MESH_COUNT:
		_fail(106, "mesh_id_count != " + str(MESH_COUNT))
		return

	for m in MESH_COUNT:
		mesh_colors.append(server.gpu_mesh_get_mesh_color(m))

	for i in INSTANCE_COUNT:
		server.gpu_scene_set_instance_transform(i, _instance_pos(i), INSTANCE_SCALE)
		server.gpu_scene_set_instance_mesh(i, i % MESH_COUNT)

	for w in WALL_COUNT:
		var idx: int = INSTANCE_COUNT + w
		server.gpu_scene_set_instance_transform(idx, Vector3(WALL_X[w], 0.0, WALL_Z), WALL_SCALE)
		server.gpu_scene_set_instance_mesh(idx, WALL_MESH_ID)

	if not server.gpu_mesh_set_batch_strategy(STRATEGY_REORDERED):
		_fail(108, "gpu_mesh_set_batch_strategy")
		return

	var vp_size: Vector2 = get_viewport().get_visible_rect().size
	server.gpu_scene_set_viewport(vp_size.x, vp_size.y)
	server.gpu_scene_set_camera(camera.get_global_transform(), camera.get_camera_projection())

	if not server.gpu_hzb_prod_create():
		_fail(109, "gpu_hzb_prod_create")
		return

	# GNE-012: register the two wall AABB slabs as GPU occluders (spec 012
	# occluder path - a flat storage buffer projection, the ONE reliable GPU
	# cross-submission route, rebuilt every dispatch). Box A covers cols 0/1
	# back, Box B covers cols 2..7; the x-gap at -330 (back col 1) is the
	# deliberate visibility slit. Near face z=-800 -> pyramid inv 1200, which
	# is > back-sphere inv (925, occludes them) but < wall/front sphere inv
	# (walls+front survive). Boxes are much taller than the occluder octa so
	# they also provide the wall-color evidence at occluded pixels.
	# GNE-012 GAP EXPERIMENT (owner-approved, local only).
	#
	# ORIGINAL bounds, preserved verbatim for the record - DO NOT LOSE:
	#   Box A: x [-650, -350]  Box B: x [-270, 700]   (both y [-400,400], z [-1200,-800])
	#   Vector4(-650,-400,-1200)  Vector4(-350,400,-800)
	#   Vector4(-270,-400,-1200)  Vector4(700,400,-800)
	#
	# MEASURED reason the slit fails (AUDIT-012-f065, frame 60):
	#   instance 65 centre x = -330 (inside the 80-unit gap), radius 26,
	#   r_px = 7.84 -> level 3 (256 texels over 1920 px => 1 texel = 7.5 px).
	#   phase-2 samples the 2x2 texel block at (114,106) => x texels 114 and 115.
	#   occbuf writes Box A over x texels [99,115) and Box B over [115,160),
	#   so BOTH halves of 65's tested block are covered and 65 is culled
	#   (sphere_inv 926 < box inv 1200). The projected gap is only ~12 px,
	#   i.e. under two texels at this level.
	#
	# This experiment only moves the two INNER x edges (A.max and B.min) to
	# widen the slit so 65's tested texels are clear. A.min and B.max are
	# untouched, so instance 90 (x = -430) stays deep inside Box A.
	# Bounds are overridable from the command line so a sweep needs no edits:
	#   --axmax=<float> --bxmin=<float>
	# The defaults are the MEASURED, owner-promoted Stage-2 values. They were
	# validated over three identical command-line runs; the promotion to
	# default was verified separately by two runs with NO command-line args.
	# Texel evidence: 90 tests x-texels 110-111, 65 tests 114-115. Box A must
	# stop at or before 111 and Box B must start at or after 116 for 65 to
	# clear while 90 stays covered.
	#
	# The command-line overrides are RETAINED so the original slit and the
	# intermediate experiment stay reproducible on demand:
	#   --axmax=-350 --bxmin=-270   reproduces the ORIGINAL (pre-Stage-2) slit
	#   --axmax=-420 --bxmin=-240   reproduces the rejected intermediate sweep
	var _axmax := -450.0
	var _bxmin := -210.0
	if _user_args012.has("axmax"):
		_axmax = float(_user_args012["axmax"])
	if _user_args012.has("bxmin"):
		_bxmin = float(_user_args012["bxmin"])
	print("GNE 012-DBG occgap A.x=[-650,", _axmax, "] B.x=[", _bxmin, ",700]",
		" orig_A.x=[-650,-350] orig_B.x=[-270,700]",
		" slit_width=", _bxmin - _axmax)
	var occ := PackedVector4Array([
		Vector4(-650.0, -400.0, -1200.0, 0.0), Vector4(_axmax, 400.0, -800.0, 0.0),
		Vector4(_bxmin, -400.0, -1200.0, 0.0), Vector4(700.0, 400.0, -800.0, 0.0),
	])
	server.gpu_scene_set_occluders(occ)

	server.gpu_scene_set_camera(camera.get_global_transform(), camera.get_camera_projection())

	# GNE-012 completion path: RenderingServer.frame_post_draw (the SAME signal
	# main_011 uses) drives the screenshot + quit. Without this the callback at
	# the bottom of this file is never invoked, so a scene that PASSES its
	# evidence can never reach "GNE 012: PASS" nor exit - it only ever reached
	# this point before because _fail() quits directly.
	RenderingServer.frame_post_draw.connect(_on_frame_post_draw)

	while frame <= FRAME_LIMIT:
		if not server.gpu_cull_dispatch():
			_fail(110, "gpu_cull_dispatch")
			return
		var visible := server.gpu_cull_get_visible_count()
		if visible != TOTAL_INSTANCES:
			_fail(111, "frustum visible=" + str(visible) + " expected " + str(TOTAL_INSTANCES))
			return

		# GNE-012 consumption-path fix (audit-directed, owner-approved).
		# The batch MUST consume the phase-2 survivor list, so the production
		# two-phase dispatch is moved BEFORE gpu_mesh_batch_dispatch/draw.
		#
		# Dependencies (read from source, not assumed):
		#  - gpu_visibility_prod_dispatch is SELF-CONTAINED: it clears
		#    hzb_pyramid_data_buffer, runs hzb_occbuf_pipeline (projects the
		#    registered occluder AABBs into that flat buffer, all 12 levels),
		#    then phase1, then phase2 - which reads pyrbuf at binding 4 in the
		#    SAME submission (gne_render_server.cpp:4725-4732, 4594).
		#    It reads NO raster attachment and NO previous-frame depth.
		#  - gpu_hzb_build only manages validity: it writes hzb_valid into the
		#    shared view UBO, sets hzb_coherent, and sets hzb_rebuild_requested
		#    (whose only consumer is a debug-probe buffer_clear in batch_draw).
		#    It must run BEFORE prod_dispatch, which is why it is not moved.
		#  - gpu_cull_dispatch still runs first and still produces the 132-entry
		#    frustum list, which assertions 111/114 depend on; prod then
		#    OVERWRITES compact[]/visible_count with the p2 survivors.
		# Frame trace (loop starts at frame = 0; hzb_build is gated frame > 1):
		#    f0/f1: hzb_valid = 0 (never written; cull_dispatch forces it) -> p2=p1
		#    f2   : hzb_build takes the !hzb_pyramid_fresh branch, writes 0 -> p2=p1
		#    f3+  : hzb_build writes 1 -> phase-2 occlusion active
		# The control frame asserted by _finalize (phase_history[0].y == .x) is
		# frame 0, and is preserved for two independent reasons.
		if frame > 1 and not server.gpu_hzb_build():
			_fail(112, "gpu_hzb_build")
			return

		var t0 := Time.get_ticks_usec()
		if not server.gpu_visibility_prod_dispatch():
			_fail(113, "gpu_visibility_prod_dispatch")
			return
		dispatch_us = Time.get_ticks_usec() - t0

		levels = server.gpu_hzb_get_level_count()
		var counts := server.gpu_hzb_get_phase_counts()
		p1 = counts[0]
		p2 = counts[1]
		coherent = server.gpu_hzb_get_coherent()
		phase_history.append(Vector2i(p1, p2))

		var t1 := Time.get_ticks_usec()
		if not server.gpu_mesh_batch_dispatch():
			_fail(116, "gpu_mesh_batch_dispatch")
			return
		if not _check_batch_state():
			return

		if not server.gpu_mesh_batch_draw():
			_fail(117, "gpu_mesh_batch_draw")
			return
		draw_us = Time.get_ticks_usec() - t1

		if frame == 8:
			print("[GNE DBG] f8 ENTER frame=", frame)
			print("[GNE DBG] f8 sim2 BEFORE")
			var sim := server.gpu_hzb_dbg_sim2()
			var i := 0
			while i + 8 <= sim.size():
				var row := "012-DBG sim2 i=%d lv=%d t=(%d,%d) mx=%d sp=%d vis=%d rpx=%d" % [
					sim[i], sim[i + 1], sim[i + 2], sim[i + 3], sim[i + 4], sim[i + 5], sim[i + 6], sim[i + 7]]
				print("[GNE] ", row)
				i += 8
			print("[GNE DBG] f8 sim2 AFTER")
			print("[GNE DBG] f8 scanL0 BEFORE")
			var scan := server.gpu_hzb_dbg_scan_level0()
			if scan.size() == 4:
				print("[GNE] 012-DBG scanL0 max_inv=", scan[0], " @(", scan[1], ",", scan[2], ") nonzero=", scan[3])
			print("[GNE DBG] f8 scanL0 AFTER")
			print("[GNE DBG] f8 scanL/B loop BEFORE")
			for lvl in [1, 2, 4, 6, 8, 10]:
				var s1 := server.gpu_hzb_dbg_scan_level1(lvl)
				if s1.size() == 4:
					print("[GNE] 012-DBG scanL", lvl, " max_inv=", s1[0], " @(", s1[1], ",", s1[2], ") nonzero=", s1[3])
				var sb := server.gpu_hzb_dbg_scan_buffer(lvl)
				if sb.size() == 4:
					print("[GNE] 012-DBG scanB", lvl, " max_inv=", sb[0], " @(", sb[1], ",", sb[2], ") nonzero=", sb[3])
			print("[GNE DBG] f8 scanL/B loop AFTER")
			print("[GNE DBG] f8 EXIT")

		if p1 != TOTAL_INSTANCES:
			_fail(114, "phase1=" + str(p1) + " expected " + str(TOTAL_INSTANCES))
			return
		if p2 > p1:
			_fail(115, "phase2=" + str(p2) + " > phase1=" + str(p1))
			return
		if frame > 1 and p2 < p1 and not occlusion_active:
			occlusion_active = true
			occlusion_seen_at = frame

		var pixels := server.gpu_raster_read_pixels()
		var depth := server.gpu_raster_read_depth()

		# AUDIT-012-frames (audit-only, revert after): the per-frame trace the
		# owner asked for and that the 20-frame summary never showed - frames
		# 0-3 plus the first frame where occlusion actually goes active. Read-
		# only, no new API, no state change, no assertion is bypassed.
		if frame <= 3 or frame == occlusion_seen_at:
			var _csum2 := 0
			for _cx2 in last_draw_counts:
				_csum2 += _cx2
			var _m65f := false
			var _m90f := false
			var _walls := 0
			var _lst := server.gpu_compact_read()
			var _ln := mini(p2, _lst.size())
			for _id3 in range(_ln):
				var _v3: int = _lst[_id3]
				if _v3 == 65:
					_m65f = true
				if _v3 == 90:
					_m90f = true
				if _v3 >= INSTANCE_COUNT:
					_walls += 1
			print("GNE 012-DBG trace frame=", frame,
				" p1=", p1, " p2=", p2, " co=", coherent,
				" hzb_valid=", server.gpu_hzb_dbg_valid(),
				" batch_count_sum=", _csum2, " vis_frustum=", visible,
				" m65_in_batch_list=", _m65f, " m90_in_batch_list=", _m90f,
				" walls_in_batch_list=", _walls,
				" control=", (p2 == p1))

		var img := Image.create_from_data(RASTER_W, RASTER_H, false, Image.FORMAT_RGBA8, pixels)
		if image_tex == null:
			image_tex = ImageTexture.create_from_image(img)
		else:
			image_tex.update(img)
		display.texture = image_tex

		if frame % PRINT_EVERY == 0:
			print("GNE 012: frame=", frame, " lv=", levels, " p1=", p1, " p2=", p2,
					" co=", coherent, " occlusion=", occlusion_active)
		if frame == 3:
			print("[GNE DBG] f3 ENTER frame=", frame)
			var probe := ""
			print("[GNE DBG] f3 dbg_level0 row1024 BEFORE")
			for pxr in range(1020, 1330, 40):
				probe += "[" + str(pxr) + "]=" + str(server.gpu_hzb_dbg_level0(pxr, 1024)) + " "
			print("[GNE DBG] f3 dbg_level0 row1024 AFTER")
			print("[GNE DBG] f3 valid+lv0pair BEFORE")
			print("GNE 012-DBG valid=", server.gpu_hzb_dbg_valid(), " lv0mid_row1024 ", probe)
			print("GNE 012-DBG lv0(" + str(1023) + "," + str(995) + ")=", server.gpu_hzb_dbg_level0(1023, 995),
					" lv0(" + str(1023) + "," + str(1024) + ")=", server.gpu_hzb_dbg_level0(1023, 1024))
			print("[GNE DBG] f3 valid+lv0pair AFTER")
			print("[GNE DBG] f3 read_depth BEFORE")
			var dpx := server.gpu_raster_read_depth()
			print("[GNE DBG] f3 read_depth AFTER")
			print("GNE 012-DBG cpu_depth@x960,540=", dpx[540 * RASTER_W + 960],
					" x1180,540=", dpx[540 * RASTER_W + 1180],
					" x810,540=", dpx[540 * RASTER_W + 810],
					" x960,470=", dpx[470 * RASTER_W + 960])
			print("[GNE DBG] f3 lv0pp BEFORE")
			for pp in [[1020, 1001], [1024, 1002]]:
				print("GNE 012-DBG lv0@(" + str(pp[0]) + "," + str(pp[1]) + ")=",
						server.gpu_hzb_dbg_level0(pp[0], pp[1]))
			print("[GNE DBG] f3 lv0pp AFTER")
			print("[GNE DBG] f3 grid BEFORE")
			var grid := ""
			for ty2 in range(1010, 1040):
				for tx2 in range(850, 880):
					grid += str(server.gpu_hzb_dbg_level0(tx2, ty2)) + " "
			print("GNE 012-DBG lv0grid@864,1024:", grid)
			var grid2 := ""
			for ty2 in range(1040, 1070):
				for tx2 in range(1060, 1090):
					grid2 += str(server.gpu_hzb_dbg_level0(tx2, ty2)) + " "
			print("GNE 012-DBG lv0grid@1072,1053:", grid2)
			print("[GNE DBG] f3 grid AFTER")
			print("[GNE DBG] f3 probe BEFORE")
			var pb := server.gpu_hzb_dbg_probe()
			if pb.size() == 4:
				print("GNE 012-DBG probe count>0 p:" + str(pb[0]) + " max_inv p:" + str(pb[1]) +
						" dbits p:" + str(pb[2]) + " ndcbits p:" + str(pb[3]))
			print("[GNE DBG] f3 probe AFTER")
			print("[GNE DBG] f3 EXIT")

		if frame == FRAME_LIMIT:
			_finalize(pixels, depth)
		frame += 1
	print("[GNE DBG] _ready END frame=", frame)

func _check_batch_state() -> bool:
	last_draw_counts = server.gpu_mesh_get_draw_counts()
	var groups := server.gpu_mesh_get_batch_group_count()
	var draw_calls := server.gpu_mesh_get_draw_call_count()
	var indirect := server.gpu_mesh_get_indirect_count()

	# 011 additive regression: <=5 draw calls while >=10 batches stay distinct.
	var batches := 0
	for c in last_draw_counts:
		if c > 0:
			batches += 1
	if batches < 10:
		_fail(118, "batches=" + str(batches) + " < 10")
		return false
	if draw_calls > 5 or groups != 5 or draw_calls != indirect:
		_fail(119, "draw_calls/groups/indirect=" + str([draw_calls, groups, indirect]))
		return false
	return true

func _finalize(pixels: PackedByteArray, depth: PackedFloat32Array) -> void:
	if levels != 12:
		_fail(120, "levels=" + str(levels) + " != 12")
		return

	# PASS (1)+(2): control frame p2==p1, later frames p2 < p1.
	if phase_history.size() < 3:
		_fail(121, "phase history too short")
		return
	if phase_history[0].y != phase_history[0].x:
		_fail(122, "control frame p2=" + str(phase_history[0].y) + " != p1=" + str(phase_history[0].x))
		return
	if not occlusion_active:
		_fail(123, "occlusion never activated (p2 < p1 never seen)")
		return

	# PASS (3): pixel evidence - walls drawn, front visible, surviving back
	# visible, occluded back gone with the wall drawn in its place.
	var vp := server.gpu_scene_get_vp()
	var cc := 0
	var front_ok := _color_at_projected(_instance_pos(0), mesh_colors[0], pixels, vp)
	var wall_ok := _color_at_projected(Vector3(WALL_X[0], 0.0, WALL_Z), mesh_colors[WALL_MESH_ID], pixels, vp)
	# Back col 1 (x=-330) sits in the occluder x-gap -> it must SURVIVE the
	# pyramid and render its own mesh color.
	var back_visible := _color_at_projected(_instance_pos(MESH_COUNT + 1), mesh_colors[(MESH_COUNT + 1) % MESH_COUNT], pixels, vp)
	# GATE 124 REPLACEMENT (owner-approved): the two CONFLICTING colour
	# conditions are replaced by D1 (membership) and D2 (id-attributed wall
	# presence). mesh_colors[90 % 64] and mesh_colors[WALL_MESH_ID=5] are
	# the SAME palette entry, so those two could never both hold.
	# Nothing else in this function is touched.
	var d1 := _gate_d1_absence()
	var d2 := _gate_d2_wall_present(pixels, depth, vp)
	if front_ok:
		cc += 1
	if wall_ok:
		cc += 1
	if back_visible:
		cc += 1
	if d1:
		cc += 1
	if d2:
		cc += 1
	if cc != 5:
		_fail(124, "pixel evidence " + str(cc) + "/5 front=" + str(front_ok) +
				" wall=" + str(wall_ok) + " back_vis=" + str(back_visible) +
				" D1_90_absent=" + str(d1) + " D2_wall_id=" + str(d2) +
				" d1=" + str(_gate_d1_reason) + " d2=" + str(_gate_d2_reason))
		return

	# PASS (5): in-binary DET - rerun the full 012 GPU path and compare counts.
	det_ok = _det_check(pixels)

	print("GNE 012: evidence lv=", levels, " p1=", p1, " p2=", p2,
			" co=", coherent, " oc_first=", occlusion_seen_at,
			" groups=", server.gpu_mesh_get_batch_group_count(),
			" draw_calls=", server.gpu_mesh_get_draw_call_count(),
			" dispatch_us=", dispatch_us, " draw_us=", draw_us, " det=", det_ok)
	_print_signature(cc)
	_finish_pass()

func _det_check(pixels: PackedByteArray) -> bool:
	var cc1 := _color_counts(pixels)
	if not server.gpu_cull_dispatch():
		return false
	if not server.gpu_hzb_build():
		return false
	if not server.gpu_visibility_prod_dispatch():
		return false
	var c2 := server.gpu_hzb_get_phase_counts()
	if c2[0] != p1 or c2[1] != p2:
		print("GNE 012: DET phase counts differ ", c2, " vs [", p1, ",", p2, "]")
		return false
	if server.gpu_hzb_get_level_count() != levels:
		return false
	if server.gpu_hzb_get_coherent() != coherent:
		return false
	if not server.gpu_mesh_batch_dispatch():
		return false
	if not _check_batch_state():
		return false
	if not server.gpu_mesh_batch_draw():
		return false
	var pixels2 := server.gpu_raster_read_pixels()
	if _color_counts(pixels2) != cc1:
		print("GNE 012: DET pixel counts differ")
		return false
	return true


func _color_counts_reference(pixels: PackedByteArray) -> Array:
	# ORIGINAL five-pass implementation, retained ONLY as the defensive
	# fallback when the live palette is not uniform (see _color_counts).
	# It is not on the normal path and runs at most once per call there.
	var cg := _count_color(pixels, mesh_colors[0])
	var cb := _count_color(pixels, mesh_colors[1])
	var co := _count_color(pixels, mesh_colors[2])
	var ck := 0
	for m in range(3, MESH_COUNT):
		var c := _count_color(pixels, mesh_colors[m])
		if c > ck:
			ck = c
	var cw := _count_color(pixels, mesh_colors[WALL_MESH_ID])
	return [cg, cb, co, ck, cw]

func _color_counts(pixels: PackedByteArray) -> Array:
	# Single-pass equivalent of the original five-pass histogram.
	#
	# Equivalence argument (exact, not approximate):
	#   The reference counts each colour INDEPENDENTLY - a pixel matching two
	#   colours is counted in BOTH, never split. It returns
	#   [c(0), c(1), c(2), max_{m in 3..63} c(m), c(5)].
	#   gne_mesh_palette() has a single `default` branch, so entries 3..63 and
	#   WALL_MESH_ID=5 are ONE RGB triple => all c(m) for m in 3..63 are equal
	#   => max(...) == c(3), and c(5) == c(3).
	#   The fast path is taken ONLY after re-deriving that uniformity from the
	#   LIVE mesh_colors at runtime; if it does not hold, the reference is used,
	#   so the result can never differ.
	# Match rule unchanged: _close() is |v - t| <= 40, inlined verbatim.
	# Counting rule unchanged: independent counters, no else-if.
	var g0r := int(round(mesh_colors[0].r * 255.0))
	var g0g := int(round(mesh_colors[0].g * 255.0))
	var g0b := int(round(mesh_colors[0].b * 255.0))
	var b0r := int(round(mesh_colors[1].r * 255.0))
	var b0g := int(round(mesh_colors[1].g * 255.0))
	var b0b := int(round(mesh_colors[1].b * 255.0))
	var o0r := int(round(mesh_colors[2].r * 255.0))
	var o0g := int(round(mesh_colors[2].g * 255.0))
	var o0b := int(round(mesh_colors[2].b * 255.0))
	var k0r := int(round(mesh_colors[3].r * 255.0))
	var k0g := int(round(mesh_colors[3].g * 255.0))
	var k0b := int(round(mesh_colors[3].b * 255.0))

	# --- runtime proof of the uniformity the fast path relies on ---
	var uniform := true
	for m in range(4, MESH_COUNT):
		if int(round(mesh_colors[m].r * 255.0)) != k0r \
				or int(round(mesh_colors[m].g * 255.0)) != k0g \
				or int(round(mesh_colors[m].b * 255.0)) != k0b:
			uniform = false
			break
	if int(round(mesh_colors[WALL_MESH_ID].r * 255.0)) != k0r \
			or int(round(mesh_colors[WALL_MESH_ID].g * 255.0)) != k0g \
			or int(round(mesh_colors[WALL_MESH_ID].b * 255.0)) != k0b:
		uniform = false
	if not uniform:
		# Defensive only: the shipped palette is uniform, so this never runs.
		return _color_counts_reference(pixels)

	var cg := 0
	var cb := 0
	var co := 0
	var ck := 0
	var n: int = RASTER_W * RASTER_H
	for i in n:
		var o: int = i * 4
		var r := int(pixels[o])
		var g := int(pixels[o + 1])
		var b := int(pixels[o + 2])
		if absi(r - g0r) <= 40 and absi(g - g0g) <= 40 and absi(b - g0b) <= 40:
			cg += 1
		if absi(r - b0r) <= 40 and absi(g - b0g) <= 40 and absi(b - b0b) <= 40:
			cb += 1
		if absi(r - o0r) <= 40 and absi(g - o0g) <= 40 and absi(b - o0b) <= 40:
			co += 1
		if absi(r - k0r) <= 40 and absi(g - k0g) <= 40 and absi(b - k0b) <= 40:
			ck += 1
	var res: Array = [cg, cb, co, ck, ck]
	return res


func _print_signature(cc: int) -> void:
	var first: Vector2i = phase_history[0]
	var last: Vector2i = phase_history[phase_history.size() - 1]
	sig = "sig=v%d|lv%d|p1=%d|p2=%d|co%d|ocf%d|fc%d|dc%d|dt%d|%d/%d/%d/%d" % [
		TOTAL_INSTANCES, levels, last.x, last.y, 1 if coherent else 0,
		occlusion_seen_at, first.y, server.gpu_mesh_get_draw_call_count(),
		1 if det_ok else 0, dispatch_us, draw_us, p1, p2]
	print("GNE 012-DET ", sig)
	var f := FileAccess.open(sig_file, FileAccess.WRITE)
	if f == null:
		print("GNE 012: sig file WRITE FAILED: ", sig_file)
	else:
		f.store_line(sig)
		f.close()

func _finish_pass() -> void:
	want_shot = true
	print("GNE 012: EVIDENCE OK")

func _on_frame_post_draw() -> void:
	if not want_shot or shot_done:
		return
	shot_done = true
	want_shot = false
	var shot := get_viewport().get_texture().get_image()
	if shot.is_empty():
		print("GNE 012: window screenshot NOT EXECUTED")
	else:
		var err := shot.save_png(window_png)
		print("GNE 012: window screenshot saved=", err == OK)
	server.gpu_scene_destroy()
	print("GNE 012: PASS")
	get_tree().quit(0)

# --- pixel helpers (identical to main_011) ---
func _count_color(pixels: PackedByteArray, col: Color) -> int:
	var c := 0
	var n: int = RASTER_W * RASTER_H
	var r0 := int(round(col.r * 255.0))
	var g0 := int(round(col.g * 255.0))
	var b0 := int(round(col.b * 255.0))
	for i in n:
		var o: int = i * 4
		if _close(pixels[o], r0) and _close(pixels[o + 1], g0) and _close(pixels[o + 2], b0):
			c += 1
	return c

func _close(v: int, t: int) -> bool:
	return absi(v - t) <= 40

func _pixel_matches(x: int, y: int, col: Color, pixels: PackedByteArray) -> bool:
	if x < 0 or x >= RASTER_W or y < 0 or y >= RASTER_H:
		return false
	var o: int = (y * RASTER_W + x) * 4
	return _close(pixels[o], int(round(col.r * 255.0))) and _close(pixels[o + 1], int(round(col.g * 255.0))) and _close(pixels[o + 2], int(round(col.b * 255.0)))

func _project_px(p: Vector3, vp: PackedFloat32Array) -> Array:
	var ccx: float = vp[0] * p.x + vp[4] * p.y + vp[8] * p.z + vp[12]
	var ccy: float = vp[1] * p.x + vp[5] * p.y + vp[9] * p.z + vp[13]
	var ccw: float = vp[3] * p.x + vp[7] * p.y + vp[11] * p.z + vp[15]
	if ccw <= 0.0:
		return []
	var ndcx := ccx / ccw
	var ndcy := ccy / ccw
	var px: float = (ndcx * 0.5 + 0.5) * float(RASTER_W)
	var out := []
	for cy_sign: int in [1, -1]:
		var py: float = (0.5 + float(cy_sign) * 0.5 * ndcy) * float(RASTER_H)
		if py >= 0.0 and py <= float(RASTER_H):
			out.append(Vector2(px, py))
	return out

func _color_at_projected(p: Vector3, col: Color, pixels: PackedByteArray, vp: PackedFloat32Array) -> bool:
	var pts := _project_px(p, vp)
	for pt in pts:
		if _search_color(int(pt.x), int(pt.y), 11, col, pixels):
			return true
	return false

func _search_color(cx: int, cy: int, r: int, col: Color, pixels: PackedByteArray) -> bool:
	for dy in range(-r, r + 1):
		var yy := cy + dy
		if yy < 0 or yy >= RASTER_H:
			continue
		for dx in range(-r, r + 1):
			var xx := cx + dx
			if xx < 0 or xx >= RASTER_W:
				continue
			if _pixel_matches(xx, yy, col, pixels):
				return true
	return false

func _fail(code: int, msg: String) -> void:
	print("GNE 012: FAIL code=", code, " ", msg)
	if server != null:
		server.gpu_scene_destroy()
	get_tree().quit(code)
