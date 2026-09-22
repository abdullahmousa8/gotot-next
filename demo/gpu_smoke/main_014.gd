extends Node

# GOTOT-014 - GPU Scene Manager (SPEC 014)
#
# The manager is a GPU-resident scene database (additive TEST-ONLY evidence
# bridge in modules/gotot_render). Everything here is separate from the 001B
# scene, the 008A/010/011 mesh batch path, the 009 depth buffer and the 013
# meshlet pipeline; the manager mirrors the 013 meshlet ordinal space + LOD
# config into its 32-byte draw-record snapshot (015 render-graph hand-off).
#
# Evidence targets (8 PASS criteria from SPEC 014):
#   (1) GPU Scene DB   : allocate 1,048,576 unified ids (64-B AoS records).
#   (2) GPU-driven upd : 16 MB ring + compute apply (add/remove/move), zero
#       readbacks in the critical path (only tiny verify counters).
#   (3) 013 hand-off   : draw-record ordinals fall within the 013 meshlet DB
#       LOD ordinal ranges + LOD config; the 013 signature (cull-raster fnv)
#       is reproduced byte-identically with the manager alive.
#   (4) 011 compat     : the scene-managed set groups into min(distinct,5) <= 5
#       draw commands (011 REORDERED grouping on the managed list).
#   (5) DET            : repeat dispatches produce identical snapshot ordinals
#       + stats; in-binary double-dispatch signature.
#   (6) Regressions    : 001A..013 exit 0 (enforced by the regression harness).
#   (7) Benchmark      : capacity/active/applied counts, ring bytes, dispatch
#       seq, SSBO footprint recorded as measured numbers.
#   (8) Zero RID errors: no RID cleanups / failed allocs in the run output
#       (harness greps for ERROR; every shader/pipe/set here is guarded).

const CAPACITY := 1048576
const INSTANCE_COUNT := 6
const MIN_ONE_M := 1048576
const LOD_COUNT := 3
const CAM_POS := Vector3(0, 0, 2000)
const DELTA_BYTES := 80

var sig_file := "C:/Users/opc/AppData/Local/Temp/opencode/gt014_sig.txt"
var asset_path := "res://mesh_013_torus.gomlet"

var server: GototRenderServer
var camera: Camera3D

var pass_count := 0
var sig := ""

# 013 reference evidence (Phase 0).
var ref_lods := PackedInt32Array()
var ref_cc := PackedInt32Array()
var ref_ev := PackedFloat32Array()
var ref_dbg := PackedInt32Array()
var ref_ord_bases := PackedInt32Array()
var ref_ord_total := 0

func _ready() -> void:
	_parse_user_args()
	server = GototRenderServer.get_server_singleton()
	if server == null:
		_fail(160, "server singleton is null")
		return
	if not server.ensure_gpu_device():
		_fail(161, "local RenderingDevice not available")
		return

	camera = $Camera
	camera.global_position = CAM_POS
	camera.rotation = Vector3.ZERO
	camera.near = 300.0
	camera.far = 4000.0

	if not _run_013_pipeline():
		return

	if not _phase_capacity():
		return
	if not _phase_gpu_driven_update():
		return
	if not _phase_011_compat():
		return
	if not _phase_013_handoff():
		return
	if not _phase_det():
		return
	if not _phase_guard():
		return

	sig = "sig=v14|c%d|a%d|u%d/%d/%d|sn%d|dm%d|g%d|s%d|dd%d|d%d" % [
		ref_ord_total,
		_phase2_active,
		_phase3_adds, _phase3_removes, _phase3_moves,
		_phase4_snap, _phase4_distinct, _phase4_groups,
		_phase6_ssbo,
		_pass_m5_snap_hash,
		1 if pass_count >= 5 else 0]
	print("GOTOT-NEXT 014: ", sig)
	var f := FileAccess.open(sig_file, FileAccess.WRITE)
	if f == null:
		print("GOTOT-NEXT 014: sig file WRITE FAILED: ", sig_file)
	else:
		f.store_line(sig)
		f.close()

	print("GOTOT-NEXT 014: PASS")
	server.gpu_scene_manager_destroy()
	server.gpu_meshlet_destroy()
	server.gpu_scene_destroy()
	get_tree().quit(0)

func _parse_user_args() -> void:
	for a in OS.get_cmdline_user_args():
		var kv := a.split("=")
		if kv.size() != 2:
			continue
		if kv[0] == "--sigf":
			sig_file = kv[1]
		elif kv[0] == "--asset":
			asset_path = kv[1]

func _read_asset(path: String) -> PackedByteArray:
	var f := FileAccess.open(path, FileAccess.READ)
	if f == null or not f.is_open():
		return PackedByteArray()
	var v := f.get_buffer(f.get_length())
	f.close()
	var out := PackedByteArray()
	out.resize(v.size())
	for i in v.size():
		out[i] = v[i]
	return out

# ---------------------------------------------------------------- Phase 0
# Reference 013 pipeline (cull + raster), capturing lods / cluster counts /
# raster evidence / ordinal bases. This is the signature that MUST survive
# the manager (criterion 3).
func _run_013_pipeline() -> bool:
	if not server.gpu_scene_create(INSTANCE_COUNT, 1.0):
		_fail(162, "gpu_scene_create")
		return false
	if not server.gpu_scene_dispatch(13):
		_fail(163, "gpu_scene_dispatch")
		return false

	var positions := [
		Vector3(0, 0, 0), Vector3(0, 0, -400), Vector3(0, 0, -800),
		Vector3(0, 0, -1400), Vector3(3000, 0, 0), Vector3(0, 0, 2500),
	]
	for i in INSTANCE_COUNT:
		server.gpu_scene_set_instance_transform(i, positions[i], 1.0)

	server.gpu_scene_set_viewport(1920.0, 1080.0)
	server.gpu_scene_set_camera(camera.get_global_transform(), camera.get_camera_projection())

	var bytes := _read_asset(asset_path)
	if bytes.size() < 20:
		_fail(164, "asset read failed/empty: " + asset_path)
		return false
	if not server.gpu_meshlet_load(bytes):
		_fail(165, "gpu_meshlet_load")
		return false
	if not server.gpu_meshlet_set_lod_thresholds(2200.0, 3200.0):
		_fail(166, "gpu_meshlet_set_lod_thresholds")
		return false
	if not server.gpu_meshlet_cull_dispatch():
		_fail(167, "gpu_meshlet_cull_dispatch")
		return false
	ref_lods = server.gpu_meshlet_get_instance_lods()
	ref_cc = server.gpu_meshlet_get_cluster_counts()
	ref_dbg = server.gpu_meshlet_get_cull_debug()
	if ref_lods.size() != INSTANCE_COUNT:
		_fail(168, "instance_lods size")
		return false
	var expect := PackedInt32Array([0, 1, 1, 2, 2, 0])
	if ref_lods != expect:
		_fail(169, "instance_lods " + str(ref_lods) + " != 0,1,1,2,2,0")
		return false
	if ref_cc.size() < 8 or ref_cc[0] <= 0 or ref_cc[1] <= 0:
		_fail(170, "013 cluster counts sanity")
		return false
	if not server.gpu_meshlet_raster_dispatch():
		_fail(171, "gpu_meshlet_raster_dispatch")
		return false
	ref_ev = server.gpu_meshlet_raster_evidence()
	if ref_ev.size() < 4:
		_fail(172, "raster_evidence size")
		return false
	if ref_ev[0] <= 0.0 or ref_ev[1] <= 0.0:
		_fail(173, "013 covered/subpixel empty")
		return false
	# Ordinal bases for each LOD (desc buffer: LOD headers at 0..2, meshlet
	# descs start at LOD_COUNT). dbg[0..2] = per-LOD meshlet counts.
	if ref_dbg.size() < 3:
		_fail(174, "cull debug size")
		return false
	var bases := PackedInt32Array()
	var acc := LOD_COUNT
	for l in LOD_COUNT:
		bases.append(acc)
		acc += ref_dbg[l]
	ref_ord_bases = bases
	ref_ord_total = acc # == LOD_COUNT + total_meshlets (incl. LOD header slots)
	var expected_total := LOD_COUNT + server.gpu_meshlet_get_total_meshlets()
	if ref_ord_total != expected_total:
		_fail(175, "ordinal total " + str(ref_ord_total) + " != " + str(expected_total))
		return false
	print("GOTOT-NEXT 014: ref013 ev=[covered=" + str(int(ref_ev[0])) + ", sub=" + str(int(ref_ev[1])) + ", winner=" + str(int(ref_ev[2])) + ", fnv=" + str(int(ref_ev[3])) + "] ord_bases=" + str(ref_ord_bases) + " ord_total=" + str(ref_ord_total))
	return true

# ---------------------------------------------------------------- Criterion 1
func _phase_capacity() -> bool:
	if not server.gpu_scene_manager_alloc(CAPACITY):
		_fail(176, "gpu_scene_manager_alloc(1M)")
		return false
	var st := server.gpu_scene_manager_get_stats()
	if int(st["capacity"]) != CAPACITY:
		_fail(177, "capacity " + str(st["capacity"]) + " != 1048576")
		return false

	# 1,048,576 deterministic records (16 floats each).
	var arr := PackedFloat32Array()
	arr.resize(CAPACITY * 16)
	for i in CAPACITY:
		var px := float(i % 128) * 200.0
		var py := float((i / 128) % 128) * 200.0
		var pz := float(i / 16384) * 200.0
		var o := i * 16
		arr[o + 0] = px
		arr[o + 1] = py
		arr[o + 2] = pz
		arr[o + 3] = 1.0
		arr[o + 4] = px
		arr[o + 5] = py
		arr[o + 6] = pz
		arr[o + 7] = 300.0
		arr[o + 8] = 0.0
		arr[o + 9] = 0.0
		arr[o + 10] = 1.0
		arr[o + 11] = 0.0
		arr[o + 12] = 2200.0
		arr[o + 13] = 3200.0
		arr[o + 14] = 0.0
		arr[o + 15] = 0.0
	if not server.gpu_scene_manager_set_instances(arr):
		_fail(178, "gpu_scene_manager_set_instances(1M)")
		return false
	if not server.gpu_scene_manager_dispatch():
		_fail(179, "dispatch #1")
		return false
	st = server.gpu_scene_manager_get_stats()
	if int(st["active"]) != MIN_ONE_M:
		_fail(180, "active != 1048576: " + str(st["active"]))
		return false
	if int(st["snap_records"]) != MIN_ONE_M:
		_fail(181, "snap_records != 1048576: " + str(st["snap_records"]))
		return false
	if int(st["ssbo_bytes"]) < CAPACITY * 64:
		_fail(182, "ssbo_bytes < capacity*64")
		return false
	if int(st["distinct_meshes"]) != 1:
		_fail(183, "distinct meshes != 1 on single-mesh fill")
		return false
	pass_count += 1
	print("GOTOT-NEXT 014: C1 GPU Scene DB pass (active=", st["active"], " snap=", st["snap_records"], " ssbo=", st["ssbo_bytes"], ")")
	return true

# ---------------------------------------------------------------- Criterion 2
var _phase2_active := 0
var _phase3_adds := 0
var _phase3_removes := 0
var _phase3_moves := 0

func _phase_gpu_driven_update() -> bool:
	var deltas := Array()
	# 5,000 removes (ids 0..4999).
	for i in 5000:
		deltas.append(_delta_remove(i, i))
	# 5,000 re-adds (same ids) - records rewritten through the ring.
	for i in 5000:
		deltas.append(_delta_add(i, i, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0))
	# 10,000 moves (ids 5000..14999).
	for i in 10000:
		var id := 5000 + i
		deltas.append(_delta_move(id, id, float(id % 256) * 10.0, 40.0, 40.0, 1.0, float(id % 256) * 10.0, 40.0, 40.0, 300.0))
	if not server.gpu_scene_manager_update(deltas):
		_fail(184, "gpu_scene_manager_update(20k deltas)")
		return false
	var st := server.gpu_scene_manager_get_stats()
	var used := int(st["ring_used_bytes"])
	if used != 20000 * DELTA_BYTES:
		_fail(185, "ring used " + str(used) + " != 20000*80")
		return false
	if not server.gpu_scene_manager_dispatch():
		_fail(186, "dispatch #2")
		return false
	st = server.gpu_scene_manager_get_stats()
	if int(st["adds"]) != 5000 or int(st["removes"]) != 5000 or int(st["moves"]) != 10000:
		_fail(187, "applied ops " + str([st["adds"], st["removes"], st["moves"]]) + " != 5000/5000/10000")
		return false
	if int(st["active"]) != MIN_ONE_M:
		_fail(188, "active after net-zero updates " + str(st["active"]) + " != 1048576")
		return false
	if int(st["ring_consumed_bytes"]) != 20000 * DELTA_BYTES:
		_fail(189, "ring consumed " + str(st["ring_consumed_bytes"]) + " != 1600000")
		return false
	if int(st["ring_used_bytes"]) != 0:
		_fail(190, "ring not reset after dispatch")
		return false
	_phase2_active = 0
	_phase3_adds = 5000
	_phase3_removes = 5000
	_phase3_moves = 10000
	pass_count += 1
	print("GOTOT-NEXT 014: C2 GPU-driven update pass (applied ", st["adds"], "/", st["removes"], "/", st["moves"], " ring=16MiB)")
	return true

# ---------------------------------------------------------------- Criterion 4
var _phase4_snap := 0
var _phase4_distinct := 0
var _phase4_groups := 0

func _phase_011_compat() -> bool:
	# 4,096 instances spread over 8 mesh refs -> REORDERED grouping cap = 5.
	var arr := PackedFloat32Array()
	arr.resize(4096 * 16)
	for i in 4096:
		var px := float(i % 64) * 20.0
		var o := i * 16
		arr[o + 0] = px
		arr[o + 1] = 0.0
		arr[o + 2] = 0.0
		arr[o + 3] = 1.0
		arr[o + 4] = px
		arr[o + 5] = 0.0
		arr[o + 6] = 0.0
		arr[o + 7] = 300.0
		arr[o + 8] = 0.0
		arr[o + 9] = float(i % 8)
		arr[o + 10] = 1.0
		arr[o + 11] = 0.0
		arr[o + 12] = 2200.0
		arr[o + 13] = 3200.0
		arr[o + 14] = 0.0
		arr[o + 15] = 0.0
	if not server.gpu_scene_manager_set_instances(arr):
		_fail(191, "set_instances(4096)")
		return false
	if not server.gpu_scene_manager_dispatch():
		_fail(192, "dispatch #3")
		return false
	var dc := server.gpu_scene_manager_get_draw_counts()
	if dc.size() < 3:
		_fail(193, "draw_counts size")
		return false
	var snap := dc[0]
	var distinct := dc[1]
	var groups := dc[2]
	if snap != 4096:
		_fail(194, "snap " + str(snap) + " != 4096")
		return false
	if distinct != 8:
		_fail(195, "distinct meshes " + str(distinct) + " != 8")
		return false
	if groups != 5:
		_fail(196, "011 groups " + str(groups) + " != min(8,5)=5")
		return false
	_phase4_snap = snap
	_phase4_distinct = distinct
	_phase4_groups = groups
	pass_count += 1
	print("GOTOT-NEXT 014: C4 011-compat grouping pass (snap=", snap, " distinct=", distinct, " groups=", groups, ")")
	return true

# ---------------------------------------------------------------- Criterion 3
var _pass_m5_snap_hash := 0

func _phase_013_handoff() -> bool:
	# Rebuild the 013 6-instance scene through the manager: each record carries
	# the per-instance LOD ordinal base + LOD config from the meshlet DB.
	var arr := PackedFloat32Array()
	arr.resize(INSTANCE_COUNT * 16)
	for i in INSTANCE_COUNT:
		var lod := ref_lods[i]
		var ord := float(ref_ord_bases[lod])
		var o := i * 16
		arr[o + 0] = 0.0
		arr[o + 1] = 0.0
		arr[o + 2] = 0.0
		arr[o + 3] = 1.0
		arr[o + 4] = 0.0
		arr[o + 5] = 0.0
		arr[o + 6] = 0.0
		arr[o + 7] = 300.0
		arr[o + 8] = ord
		arr[o + 9] = 0.0
		arr[o + 10] = 1.0
		arr[o + 11] = 0.0
		arr[o + 12] = 2200.0
		arr[o + 13] = 3200.0
		arr[o + 14] = 0.0
		arr[o + 15] = 0.0
	if not server.gpu_scene_manager_set_instances(arr):
		_fail(197, "set_instances(013 hand-off)")
		return false
	if not server.gpu_scene_manager_dispatch():
		_fail(198, "dispatch #4")
		return false
	var ords := server.gpu_scene_manager_get_snapshot(INSTANCE_COUNT)
	if ords.size() != INSTANCE_COUNT:
		_fail(199, "hand-off snapshot size")
		return false
	# Each draw-record ordinal must fall inside its instance's LOD meshlet range.
	for i in INSTANCE_COUNT:
		var lod := ref_lods[i]
		var ord := ords[i]
		if ord < ref_ord_bases[lod] or ord >= ref_ord_bases[lod] + ref_dbg[lod]:
			_fail(200, "hand-off ordinal " + str(ord) + " outside LOD" + str(lod) + " range [" + str(ref_ord_bases[lod]) + ", " + str(ref_ord_bases[lod] + ref_dbg[lod]) + ")")
			return false
	var snap := server.gpu_scene_manager_get_stats()
	var fs := PackedInt32Array(ords)
	var h := 2166136261
	for o in fs:
		h = (h ^ o) * 16777619
	_pass_m5_snap_hash = h & 0x7fffffff
	_pass_m5_stats = snap

	# Signature preservation: the FULL 013 cull+raster pipeline still reproduces
	# the reference evidence while the manager is alive.
	if not server.gpu_meshlet_cull_dispatch():
		_fail(201, "013 cull after manager")
		return false
	if server.gpu_meshlet_get_instance_lods() != ref_lods:
		_fail(202, "013 instance_lods changed with manager alive")
		return false
	if not server.gpu_meshlet_raster_dispatch():
		_fail(203, "013 raster after manager")
		return false
	var ev2 := server.gpu_meshlet_raster_evidence()
	if ev2 != ref_ev:
		_fail(204, "013 raster evidence changed with manager alive (ev1=" + str(ref_ev) + " ev2=" + str(ev2) + ")")
		return false
	pass_count += 1
	print("GOTOT-NEXT 014: C3 013 hand-off pass (ord hash=", _pass_m5_snap_hash, " fnv preserved=", int(ref_ev[3]), ")")
	return true

# ---------------------------------------------------------------- Criterion 5
var _pass_m5_stats := {}
var _phase6_ssbo := 0

func _phase_det() -> bool:
	if not server.gpu_scene_manager_dispatch():
		_fail(205, "dispatch #5 (DET A)")
		return false
	var ords_a := server.gpu_scene_manager_get_snapshot(INSTANCE_COUNT)
	var stats_a := server.gpu_scene_manager_get_stats()
	if not server.gpu_scene_manager_dispatch():
		_fail(206, "dispatch #6 (DET B)")
		return false
	var ords_b := server.gpu_scene_manager_get_snapshot(INSTANCE_COUNT)
	var stats_b := server.gpu_scene_manager_get_stats()
	if ords_a != ords_b:
		_fail(207, "DET snapshot ordinals differ between dispatches")
		return false
	for k in ["active", "snap_records", "distinct_meshes", "group_count_011", "adds", "removes", "moves"]:
		if stats_a[k] != stats_b[k]:
			_fail(208, "DET stat '" + k + "' differs")
			return false
	_phase6_ssbo = int(stats_a["ssbo_bytes"])
	pass_count += 1
	print("GOTOT-NEXT 014: C5 DET pass (d1, ordinals+stats identical across runs)")
	return true

# ---------------------------------------------------------------- Guard / errors
func _phase_guard() -> bool:
	# Capacity guard: exceeding GMS_MAX_CAPACITY must fail WITHOUT tearing the
	# live manager down.
	if server.gpu_scene_manager_alloc(4000001):
		_fail(209, "capacity guard accepted 4,000,001 (must reject)")
		return false
	var st := server.gpu_scene_manager_get_stats()
	if not bool(st["valid"]):
		_fail(210, "manager torn down by failed alloc (guard must be non-destructive)")
		return false
	if int(st["active"]) != INSTANCE_COUNT:
		_fail(211, "active after guard != 6")
		return false
	pass_count += 1
	return true

# ---------------------------------------------------------------- delta builders
func _delta_remove(id: int, seq: int) -> PackedFloat32Array:
	var d := PackedFloat32Array()
	d.resize(20)
	d[0] = 2.0
	d[1] = float(id)
	d[2] = float(seq)
	return d

func _delta_add(id: int, seq: int, px: float, py: float, pz: float, scale: float, mesh: float, flags: float) -> PackedFloat32Array:
	var d := PackedFloat32Array()
	d.resize(20)
	d[0] = 1.0
	d[1] = float(id)
	d[2] = float(seq)
	d[3] = flags
	d[4] = px
	d[5] = py
	d[6] = pz
	d[7] = scale
	d[8] = px
	d[9] = py
	d[10] = pz
	d[11] = 300.0
	d[12] = 0.0
	d[13] = mesh
	d[14] = flags
	d[15] = 0.0
	d[16] = 2200.0
	d[17] = 3200.0
	return d

func _delta_move(id: int, seq: int, px: float, py: float, pz: float, scale: float, bx: float, by: float, bz: float, br: float) -> PackedFloat32Array:
	var d := PackedFloat32Array()
	d.resize(20)
	d[0] = 3.0
	d[1] = float(id)
	d[2] = float(seq)
	d[4] = px
	d[5] = py
	d[6] = pz
	d[7] = scale
	d[8] = bx
	d[9] = by
	d[10] = bz
	d[11] = br
	return d

func _fail(code: int, msg: String) -> void:
	print("GOTOT-NEXT 014: FAIL code=", code, " ", msg)
	if server != null:
		server.gpu_scene_manager_destroy()
		server.gpu_meshlet_destroy()
		server.gpu_scene_destroy()
	get_tree().quit(code)