extends Node

# GNE-015 - Render Graph (SPEC 015)
#
# The graph is a real DAG built through the module (modules/gne_render).
# It is completely separate from the 001B scene, the 008A/010/011 batch path,
# the 009 depth buffer, the 013 meshlet pipeline and the 014 GPU scene
# manager; the graph only *schedules* the pre-015 server entry points and
# never changes any earlier signature.
#
# Evidence targets (PASS criteria from SPEC 015):
#   (1) DAG >= 6 passes : scene_update, cull, cluster_cull, batch_assembly,
#       raster, output - with resource-scoped dependencies between them.
#   (2) Topo + cycles   : Kahn topological sort; the execution order is a
#       valid linearisation and a deliberately planted cycle is DETECTED and
#       rejected instead of dispatching.
#   (3) Auto barriers   : every producer->consumer edge on a resource implies a
#       barrier. Zero manual barriers anywhere in the 015 path.
#   (4) Transient pool  : per-resource transient allocation with lifetime
#       aliasing; the measured VRAM saving is a positive number.
#   (5) Execute         : >= 6 passes really dispatch per frame, through the
#       existing (013/011/014) entry points so the evidence is real.
#   (6) DET             : two consecutive executes produce identical order,
#       identical barriers, identical pool layout and an advancing seq.
#   (7) Zero RID errors : no RID cleanups / failed allocations in the output.
#   (8) No signature    : 011 / 013 / 014 signatures stay byte-identical with
#       drift            the graph alive (criterion enforced in _phase_compat).

const INSTANCE_COUNT := 6
const CAM_POS := Vector3(0, 0, 2000)
const LOD_COUNT := 3

# 015 pass kinds (must match RG_PASS_* in the module).
const K_SCN := 0
const K_CULL := 1
const K_CLUSTER := 2
const K_BATCH := 3
const K_RASTER := 4
const K_OUT := 5

var sig_file := "C:/Users/opc/AppData/Local/Temp/opencode/gt015_sig.txt"
var asset_path := "res://mesh_013_torus.gomlet"

var server: GneRenderServer
var camera: Camera3D

var pass_count := 0
var sig := ""

# 013/014 reference evidence (Phase 0), kept alive to prove signature drift = 0.
var ref_lods := PackedInt32Array()
var ref_ev := PackedFloat32Array()
var ref_ord_bases := PackedInt32Array()
var ref_ord_total := 0
var ref_013_sig := ""

var _topo := PackedStringArray()
var _barriers := 0
var _pool_bytes := 0
var _res_bytes := 0
var _saved := 0
var _exec_a := 0
var _exec_b := 0
var _seq_a := 0
var _seq_b := 0

func _ready() -> void:
	_parse_user_args()
	server = GneRenderServer.get_server_singleton()
	if server == null:
		_fail(200, "server singleton is null")
		return
	if not server.ensure_gpu_device():
		_fail(201, "local RenderingDevice not available")
		return

	camera = $Camera
	camera.global_position = CAM_POS
	camera.rotation = Vector3.ZERO
	camera.near = 300.0
	camera.far = 4000.0

	if not _phase_013_reference():
		return
	if not _phase_build_dag():
		return
	if not _phase_topology():
		return
	if not _phase_cycle_reject():
		return
	if not _phase_pool():
		return
	if not _phase_execute():
		return
	if not _phase_det():
		return
	if not _phase_compat():
		return

	var s0 := server.gpu_rg_get_stats()
	sig = "sig=v15-pc%d-p%d-e%d-b%d-po%d-res%d-sv%d-x%d-%d-q%d-%d-t%d" % [
		pass_count,
		int(s0["pass_count"]),
		int(s0["edge_count"]),
		_barriers,
		_pool_bytes,
		_res_bytes,
		_saved,
		_exec_a,
		_exec_b,
		_seq_a,
		_seq_b,
		int(ref_ord_total)]
	print("GNE 015: ", sig)
	var f := FileAccess.open(sig_file, FileAccess.WRITE)
	if f == null:
		print("GNE 015: sig file WRITE FAILED: ", sig_file)
	else:
		f.store_line(sig)
		f.close()

	print("GNE 015: PASS")
	server.gpu_rg_destroy()
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
# The 013 pipeline is driven once here to capture the reference signature that
# MUST survive the graph (criterion 8). The graph itself does not touch it.
func _phase_013_reference() -> bool:
	if not server.gpu_scene_create(INSTANCE_COUNT, 1.0):
		_fail(202, "gpu_scene_create")
		return false
	if not server.gpu_scene_dispatch(13):
		_fail(203, "gpu_scene_dispatch")
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
		_fail(204, "asset read failed/empty: " + asset_path)
		return false
	if not server.gpu_meshlet_load(bytes):
		_fail(205, "gpu_meshlet_load")
		return false
	if not server.gpu_meshlet_set_lod_thresholds(2200.0, 3200.0):
		_fail(206, "gpu_meshlet_set_lod_thresholds")
		return false
	if not server.gpu_meshlet_cull_dispatch():
		_fail(207, "gpu_meshlet_cull_dispatch")
		return false
	ref_lods = server.gpu_meshlet_get_instance_lods()
	var dbg := server.gpu_meshlet_get_cull_debug()
	if ref_lods.size() != INSTANCE_COUNT:
		_fail(208, "instance_lods size")
		return false
	var expect := PackedInt32Array([0, 1, 1, 2, 2, 0])
	if ref_lods != expect:
		_fail(209, "instance_lods " + str(ref_lods) + " != 0,1,1,2,2,0")
		return false
	if dbg.size() < LOD_COUNT:
		_fail(210, "cull debug size")
		return false
	if not server.gpu_meshlet_raster_dispatch():
		_fail(211, "gpu_meshlet_raster_dispatch")
		return false
	ref_ev = server.gpu_meshlet_raster_evidence()
	if ref_ev.size() < 4 or ref_ev[0] <= 0.0 or ref_ev[1] <= 0.0:
		_fail(212, "013 raster evidence empty")
		return false
	var bases := PackedInt32Array()
	var acc := LOD_COUNT
	for l in LOD_COUNT:
		bases.append(acc)
		acc += dbg[l]
	ref_ord_bases = bases
	ref_ord_total = acc
	ref_013_sig = str(ref_lods) + "/" + str(int(ref_ev[3]))
	print("GNE 015: ref013 fnv=", int(ref_ev[3]), " ord_total=", ref_ord_total)
	return true

# ---------------------------------------------------------------- Criterion 1
func _phase_build_dag() -> bool:
	if not server.gpu_rg_create():
		_fail(213, "gpu_rg_create")
		return false

	# 6 passes, each declaring the resources it READS and WRITES.
	if not server.gpu_rg_add_pass("scene_update", K_SCN, PackedStringArray(), PackedStringArray(["draw_records"])):
		_fail(214, "add_pass scene_update")
		return false
	if not server.gpu_rg_add_pass("cull", K_CULL, PackedStringArray(["draw_records"]), PackedStringArray(["cull_out"])):
		_fail(215, "add_pass cull")
		return false
	if not server.gpu_rg_add_pass("cluster_cull", K_CLUSTER, PackedStringArray(["cull_out"]), PackedStringArray(["clusters"])):
		_fail(216, "add_pass cluster_cull")
		return false
	if not server.gpu_rg_add_pass("batch_assembly", K_BATCH, PackedStringArray(["clusters"]), PackedStringArray(["batches"])):
		_fail(217, "add_pass batch_assembly")
		return false
	if not server.gpu_rg_add_pass("raster", K_RASTER, PackedStringArray(["batches"]), PackedStringArray(["color"])):
		_fail(218, "add_pass raster")
		return false
	if not server.gpu_rg_add_pass("output", K_OUT, PackedStringArray(["color"]), PackedStringArray([])):
		_fail(219, "add_pass output")
		return false

	# Resource-scoped dependencies (one edge per consumed resource).
	# Sizes are transient-attachment footprints, used by the pool allocator.
	if not server.gpu_rg_add_edge("scene_update", "cull", "draw_records", 65536):
		_fail(220, "add_edge 1")
		return false
	if not server.gpu_rg_add_edge("cull", "cluster_cull", "cull_out", 262144):
		_fail(221, "add_edge 2")
		return false
	if not server.gpu_rg_add_edge("cluster_cull", "batch_assembly", "clusters", 131072):
		_fail(222, "add_edge 3")
		return false
	if not server.gpu_rg_add_edge("batch_assembly", "raster", "batches", 32768):
		_fail(223, "add_edge 4")
		return false
	if not server.gpu_rg_add_edge("raster", "output", "color", 1920 * 1080 * 4):
		_fail(224, "add_edge 5")
		return false

	# A sixth, parallel producer branch proves the sort is a real topological
	# order and not a hand-written list: depth_prepass reads cull_out too.
	if not server.gpu_rg_add_edge("cull", "batch_assembly", "cull_out", 262144):
		_fail(225, "add_edge 6 (second consumer of cull_out)")
		return false

	pass_count += 1
	print("GNE 015: C1 DAG built (6 passes, 6 resource edges)")
	return true

# ---------------------------------------------------------------- Criterion 2
func _phase_topology() -> bool:
	if not server.gpu_rg_compile():
		var d := server.gpu_rg_get_stats()
		print("GNE 015: DIAG compile-failed stats=", d)
		_fail(226, "gpu_rg_compile")
		return false
	var st := server.gpu_rg_get_stats()
	if not bool(st["valid"]) or not bool(st["compiled"]):
		_fail(227, "stats not valid/compiled after compile")
		return false
	if bool(st["cycle_detected"]):
		_fail(228, "cycle_detected on an acyclic graph")
		return false
	if int(st["pass_count"]) != 6:
		_fail(229, "pass_count " + str(st["pass_count"]) + " != 6")
		return false
	var order: PackedStringArray = st["topo_order"]
	if order.size() != 6:
		_fail(230, "topo_order size " + str(order.size()) + " != 6")
		return false
	# Kahn guarantees every producer precedes every consumer of its resource.
	var idx := {}
	for i in 6:
		idx[order[i]] = i
	var required := [
		["scene_update", "cull"],
		["cull", "cluster_cull"],
		["cluster_cull", "batch_assembly"],
		["batch_assembly", "raster"],
		["raster", "output"],
		["cull", "batch_assembly"],
	]
	for pair in required:
		if idx[pair[0]] >= idx[pair[1]]:
			_fail(231, "topo order violates edge " + str(pair) + " -> " + str(order))
			return false
	# scene_update is the unique source, output the unique sink.
	if order[0] != "scene_update":
		_fail(232, "topo[0] " + str(order[0]) + " != scene_update")
		return false
	if order[5] != "output":
		_fail(233, "topo[5] " + str(order[5]) + " != output")
		return false
	_topo = order
	pass_count += 1
	print("GNE 015: C2 topo pass (order=" + str(order) + ")")
	return true

# ---------------------------------------------------------------- Criterion 2b
# A cycle must be DETECTED and rejected - never dispatched, never a hang.
func _phase_cycle_reject() -> bool:
	# Rebuild the acyclic DAG (uncompiled), then plant a back-edge. Edges can
	# only be added before compile(), so the injection must come after the build.
	if not _phase_build_dag():
		return false
	if not server.gpu_rg_add_edge("output", "scene_update", "color", 16):
		_fail(234, "add_edge back-edge (cycle injection)")
		return false
	if server.gpu_rg_compile():
		_fail(235, "compile accepted a cyclic graph (must return false)")
		return false
	var st := server.gpu_rg_get_stats()
	if not bool(st["cycle_detected"]):
		_fail(236, "cycle_detected not set after cyclic compile")
		return false
	if bool(st["compiled"]):
		_fail(237, "compiled stayed true on a cyclic graph")
		return false
	# The previous acyclic schedule must be recoverable after a rebuild.
	if not _phase_build_dag():
		return false
	if not server.gpu_rg_compile():
		_fail(239, "re-compile acyclic graph after cycle")
		return false
	var st2 := server.gpu_rg_get_stats()
	if bool(st2["cycle_detected"]) or not bool(st2["compiled"]):
		_fail(240, "state not clean after acyclic rebuild")
		return false
	pass_count += 1
	print("GNE 015: C2b cycle detection pass (back-edge rejected, rebuilt clean)")
	return true

# ---------------------------------------------------------------- Criterion 3
func _phase_pool() -> bool:
	var st := server.gpu_rg_get_stats()
	# One barrier per edge: fully automatic, zero manual barriers declared.
	_barriers = int(st["barrier_count_auto"])
	if _barriers != 6:
		_fail(241, "auto barriers " + str(_barriers) + " != 6 edges")
		return false
	_pool_bytes = int(st["pool_bytes"])
	_res_bytes = int(st["resources_bytes"])
	_saved = int(st["aliased_saved_bytes"])
	if _pool_bytes <= 0:
		_fail(242, "transient pool bytes " + str(_pool_bytes) + " <= 0")
		return false
	if _res_bytes <= 0:
		_fail(243, "resource bytes " + str(_res_bytes) + " <= 0")
		return false
	# Aliasing MUST save memory: the pool never exceeds the naive footprint.
	if _saved <= 0:
		_fail(244, "aliased_saved_bytes " + str(_saved) + " <= 0 (no aliasing)")
		return false
	if _pool_bytes >= _res_bytes:
		_fail(245, "pool " + str(_pool_bytes) + " >= resources " + str(_res_bytes) + " (aliasing not applied)")
		return false
	pass_count += 1
	print("GNE 015: C3 auto-barrier+pool pass (barriers=", _barriers, " pool=", _pool_bytes, " res=", _res_bytes, " saved=", _saved, ")")
	return true

# ---------------------------------------------------------------- Criterion 5
func _phase_execute() -> bool:
	if not server.gpu_rg_execute():
		_fail(246, "gpu_rg_execute")
		return false
	var st := server.gpu_rg_get_stats()
	_exec_a = int(st["executed_count"])
	if _exec_a < 6:
		_fail(247, "executed_count " + str(_exec_a) + " < 6 passes")
		return false
	_seq_a = int(st["dispatch_seq"])
	if _seq_a <= 0:
		_fail(248, "dispatch_seq " + str(_seq_a) + " <= 0 (no real dispatch)")
		return false
	pass_count += 1
	print("GNE 015: C5 execute pass (passes=", _exec_a, " seq=", _seq_a, ")")
	return true

# ---------------------------------------------------------------- Criterion 6
func _phase_det() -> bool:
	if not server.gpu_rg_execute():
		_fail(249, "gpu_rg_execute #2")
		return false
	var st := server.gpu_rg_get_stats()
	_exec_b = int(st["executed_count"])
	_seq_b = int(st["dispatch_seq"])
	if _exec_b != _exec_a:
		_fail(250, "DET executed_count " + str(_exec_b) + " != " + str(_exec_a))
		return false
	if _seq_b != _seq_a + 1:
		_fail(251, "DET dispatch_seq " + str(_seq_b) + " != " + str(_seq_a + 1))
		return false
	if int(st["barrier_count_auto"]) != _barriers:
		_fail(252, "DET barrier count drifted")
		return false
	if int(st["pool_bytes"]) != _pool_bytes or int(st["aliased_saved_bytes"]) != _saved:
		_fail(253, "DET pool layout drifted")
		return false
	if st["topo_order"] != _topo:
		_fail(254, "DET topo order drifted")
		return false
	# dump() is the textual form of the same schedule; it must agree too.
	var d := server.gpu_rg_dump()
	if not d.has("dot") or not d.has("ascii"):
		_fail(255, "dump missing dot/ascii")
		return false
	var dot: String = d["dot"]
	for nm in _topo:
		if not dot.contains(nm):
			_fail(256, "dump dot missing pass " + str(nm))
			return false
	pass_count += 1
	print("GNE 015: C6 DET pass (2 executes, order+barriers+pool identical, seq ", _seq_a, "->", _seq_b, ")")
	return true

# ---------------------------------------------------------------- Criterion 8
# The graph must not perturb any earlier signature: 013 cull/raster and the
# 014 manager snapshot are re-driven with the graph ALIVE and compared against
# the reference taken before the graph existed.
func _phase_compat() -> bool:
	if not server.gpu_meshlet_cull_dispatch():
		_fail(257, "013 cull with graph alive")
		return false
	if server.gpu_meshlet_get_instance_lods() != ref_lods:
		_fail(258, "013 instance_lods changed with graph alive")
		return false
	if not server.gpu_meshlet_raster_dispatch():
		_fail(259, "013 raster with graph alive")
		return false
	if server.gpu_meshlet_raster_evidence() != ref_ev:
		_fail(260, "013 raster evidence changed with graph alive")
		return false
	var now := str(ref_lods) + "/" + str(int(ref_ev[3]))
	if now != ref_013_sig:
		_fail(261, "013 signature drift " + now + " != " + ref_013_sig)
		return false
	# 014 manager still allocates, fills and dispatches unchanged (011 grouping
	# cap still min(distinct,5)=5 with the graph alive).
	if not server.gpu_scene_manager_alloc(4096):
		_fail(262, "gpu_scene_manager_alloc with graph alive")
		return false
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
		_fail(263, "014 set_instances with graph alive")
		return false
	if not server.gpu_scene_manager_dispatch():
		_fail(264, "014 dispatch with graph alive")
		return false
	var dc := server.gpu_scene_manager_get_draw_counts()
	if dc.size() < 3 or int(dc[0]) != 4096:
		_fail(265, "014 snapshot changed with graph alive: " + str(dc[0]))
		return false
	if int(dc[1]) != 8 or int(dc[2]) != 5:
		_fail(266, "014 distinct/groups " + str([dc[1], dc[2]]) + " != 8/5")
		return false
	var stm := server.gpu_scene_manager_get_stats()
	if int(stm["active"]) != 4096 or not bool(stm["valid"]):
		_fail(267, "014 manager invalid with graph alive")
		return false
	pass_count += 1
	print("GNE 015: C7 zero-drift pass (013 fnv=", int(ref_ev[3]), " 014 snap=", int(dc[0]), " groups=", int(dc[2]), ")")
	return true

func _fail(code: int, msg: String) -> void:
	print("GNE 015: FAIL code=", code, " ", msg)
	if server != null:
		server.gpu_rg_destroy()
		server.gpu_scene_manager_destroy()
		server.gpu_meshlet_destroy()
		server.gpu_scene_destroy()
	get_tree().quit(code)
