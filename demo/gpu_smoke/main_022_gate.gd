extends Node
# GNE-022 gate 9.3 (raised bar): positive (plausible range) + negative (exact zero).
# Run: res://main_022_gate.tscn -- --gate=pos   (no barrier)
#      res://main_022_gate.tscn -- --gate=neg   (800-tall barrier blocks all B rays)
# Geometry shares one layout; constant: light (0,300,800) R=1000 I=10, ambient=0.

var server: GneRenderServer
var mode := "pos"

func _ready() -> void:
	server = GneRenderServer.get_server_singleton()
	if server == null:
		_fail(400, "no server")
		return
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--gate="):
			mode = a.split("=")[1]
	if not server.ensure_gpu_device():
		_fail(401, "no device")
		return
	if not server.gpu_scene_create(5, 1.0):
		_fail(402, "scene_create")
		return
	if not server.gpu_scene_dispatch(8):
		_fail(403, "dispatch")
		return
	if not server.gpu_mesh_create():
		_fail(404, "mesh_create")
		return
	# 0: ground slab (top y=0)
	server.gpu_scene_set_instance_transform(0, Vector3(0, -3000, -900), 6000.0)
	server.gpu_scene_set_instance_mesh(0, 0)
	# 1..4: barrier (neg) or parked (pos). Scale 800 total height, spans z=[-400,400],
	# x seamless coverage via centered cubes at +-1200/+-400.
	var bx := [-1200.0, -400.0, 400.0, 1200.0]
	for i in range(4):
		if mode == "neg":
			server.gpu_scene_set_instance_transform(i + 1, Vector3(bx[i], 400.0, 0.0), 800.0)
		else:
			server.gpu_scene_set_instance_transform(i + 1, Vector3(0, -9000, 0), 1.0)
		server.gpu_scene_set_instance_mesh(i + 1, 0)
	var lid: int = server.gpu_light_create({"type": 0, "pos": Vector3(0, 300, 800), "range": 900.0, "color": Color(1, 1, 1), "intensity": 10.0})
	var cfg := {"min": Vector3(-1600, -50, -1600), "max": Vector3(1600, 750, 1600), "dims": Vector3i(16, 8, 16)}
	if not server.gpu_gi_create(cfg):
		_fail(405, "gi_create")
		return
	server.gpu_gi_config({"ambient": Vector3(0, 0, 0)})
	# y=100 row (y=0 row is lattice-self-locked; see construction notes)
	var pA := 8 + 1 * 16 + 10 * 128   # (x=100,y=100,z=+500) near-wall lit zone
	var pM := 8 + 1 * 16 + 5 * 128    # (x=100,y=100,z=-500) just behind the boundary
	var pB := 8 + 1 * 16 + 3 * 128    # (x=100,y=100,z=-900)  B, 2-3 cells behind
	# ---- phase 1: direct-only single shot (bounce OFF, ambient 0) ----
	server.gpu_gi_config({"bounce": false})
	if not server.gpu_gi_reset():
		_fail(406, "reset")
		return
	if not server.gpu_gi_trace():
		_fail(407, "raw trace")
		return
	var rawA: PackedFloat32Array = server.gpu_gi_read_avg(pA)
	var rawM: PackedFloat32Array = server.gpu_gi_read_avg(pM)
	var rawB: PackedFloat32Array = server.gpu_gi_read_avg(pB)
	print("GATEG[", mode, "]: direct A=", rawA[0], " M=", rawM[0], " B=", rawB[0])
	# ---- phase 2: 60 accumulation frames (bounce ON) ----
	server.gpu_gi_config({"bounce": true})
	if not server.gpu_gi_reset():
		_fail(408, "reset2")
		return
	var b10 := 0.0
	var b30 := 0.0
	var b60 := 0.0
	var m60 := 0.0
	var m240 := 0.0
	for f in range(1, 241):
		if not server.gpu_gi_accum_step():
			_fail(409, "accum")
			return
		if f == 10:
			b10 = float((server.gpu_gi_read_avg(pB))[0])
		if f == 30:
			b30 = float((server.gpu_gi_read_avg(pB))[0])
		if f == 60:
			b60 = float((server.gpu_gi_read_avg(pB))[0])
		if f == 240:
			m240 = float((server.gpu_gi_read_avg(pM))[0])
	b60 = float((server.gpu_gi_read_avg(pB))[0])
	m60 = m240
	print("GATEG[", mode, "]: accum B: f10=", b10, " f30=", b30, " f60=", b60, " M60=", m60)
	var ok := true
	if rawB[0] != 0.0 or rawM[0] != 0.0:
		ok = false
		print("GATEG: NOTE direct-leak B=", rawB[0], " M=", rawM[0], " (expected literal 0)")
	if rawA[0] <= 0.0:
		ok = false
		print("GATEG: NOTE A zone not lit (scene dead)")
	if mode == "pos":
		# pre-computed plausible interval: est = direct_A * albedo(0.35) * k, k ~ 0.1
		# midpoint (trilinear diffusion attenuation over several cells); interval
		# [est/3, est*3] per the raised-bar tolerance.
		var est := float(rawA[0]) * 0.35 * 0.1
		var lo := est / 3.0
		var hi := est * 3.0
		print("GATEG: POS expected est=", est, " interval=[", lo, ",", hi, "]")
		if b60 <= 0.0 or b60 < lo or b60 > hi:
			ok = false
			print("GATEG: POS out-of-interval b60=", b60)
	else:
		if b60 != 0.0 or m60 != 0.0 or b30 != 0.0 or b10 != 0.0:
			print("GATEG: NEG-LEAK detected: f10=", b10, " f30=", b30, " f60=", b60, " M60=", m60)
	server.gpu_scene_destroy()
	if ok:
		print("GATEG[", mode, "]: PASS")
		get_tree().quit(0)
	else:
		print("GATEG[", mode, "]: FAIL")
		get_tree().quit(21)

func _fail(code: int, msg: String) -> void:
	print("GATEG: FAIL code=", code, " ", msg)
	get_tree().quit(code)
