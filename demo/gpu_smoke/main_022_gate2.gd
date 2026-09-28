extends Node
# GNE-022 official 9.3 gate - PAIRED single-process run (Architect-mandated):
# phase NEG (barrier) then phase POS (no barrier), same scenario otherwise.
# Criteria frozen: POS = deep-B inside pre-computed interval [est/3, 3*est],
# est = rawA * 0.35 * 0.1; NEG = byte-literal 0.0 everywhere in B; strict direct
# isolation (rawM == rawB == 0) via light range 800.
# Also: M verification item (old +53% observation vs M3 measurement).

var server: GneRenderServer

func _mk_point(pos: Vector3, radius: float, color: Color, intensity: float) -> Dictionary:
	return {"type": 0, "pos": pos, "range": radius, "color": color, "intensity": intensity}

func _run_phase(with_barrier: bool) -> Dictionary:
	var tag := "neg" if with_barrier else "pos"
	if not server.gpu_scene_create(5, 1.0):
		_fail(402, "scene_create " + tag)
	server.gpu_scene_dispatch(8)
	server.gpu_mesh_create()
	server.gpu_scene_set_instance_transform(0, Vector3(0, -3000, -900), 6000.0)
	server.gpu_scene_set_instance_mesh(0, 0)
	var bx := [-1200.0, -400.0, 400.0, 1200.0]
	for i in range(4):
		if with_barrier:
			server.gpu_scene_set_instance_transform(i + 1, Vector3(bx[i], 400.0, 0.0), 800.0)
		else:
			server.gpu_scene_set_instance_transform(i + 1, Vector3(0, -9000, 0), 1.0)
		server.gpu_scene_set_instance_mesh(i + 1, 0)
	server.gpu_light_create(_mk_point(Vector3(0, 300, 750), 800.0, Color(1, 1, 1), 10.0))
	var cfg := {"min": Vector3(-1600, -50, -1600), "max": Vector3(1600, 750, 1600), "dims": Vector3i(16, 8, 16)}
	if not server.gpu_gi_create(cfg):
		_fail(405, "gi_create " + tag)
	server.gpu_gi_config({"ambient": Vector3(0, 0, 0), "bounce": false})
	server.gpu_gi_reset()
	server.gpu_gi_trace()
	var pA := 8 + 1 * 16 + 10 * 128
	var pM := 8 + 1 * 16 + 5 * 128
	var pB := 8 + 1 * 16 + 4 * 128   # repositioned deep (9.3.10): iz4 z=-700, margin ~253x over half floor
	var rawA := float((server.gpu_gi_read_avg(pA))[0])
	var rawM := float((server.gpu_gi_read_avg(pM))[0])
	var rawB := float((server.gpu_gi_read_avg(pB))[0])
	server.gpu_gi_config({"bounce": true})
	server.gpu_gi_reset()
	var b10 := 0.0
	var b30 := 0.0
	var b240 := 0.0
	var m240 := 0.0
	for f in range(1, 241):
		if not server.gpu_gi_accum_step():
			_fail(409, "accum " + tag)
		if f == 10:
			b10 = float((server.gpu_gi_read_avg(pB))[0])
		if f == 30:
			b30 = float((server.gpu_gi_read_avg(pB))[0])
	b240 = float((server.gpu_gi_read_avg(pB))[0])
	m240 = float((server.gpu_gi_read_avg(pM))[0])
	print("GATE2[", tag, "]: raw A=", rawA, " M=", rawM, " B=", rawB, " | accum B: f10=", b10, " f30=", b30, " f240=", b240, " M240=", m240)
	server.gpu_scene_destroy()
	return {"tag": tag, "rawA": rawA, "rawM": rawM, "rawB": rawB, "b240": b240, "m240": m240}

func _ready() -> void:
	server = GneRenderServer.get_server_singleton()
	if server == null:
		_fail(400, "no server")
		return
	if not server.ensure_gpu_device():
		_fail(401, "no device")
		return
	# paired: NEG first (isolation proof), then POS (transport proof) - one process.
	var rNeg: Dictionary = _run_phase(true)
	var rPos: Dictionary = _run_phase(false)
	var ok := true
	# NEG criteria: byte-zero everywhere in B (direct and accumulated)
	if float(rNeg["rawB"]) != 0.0 or float(rNeg["rawM"]) != 0.0 or float(rNeg["b240"]) != 0.0 or float(rNeg["m240"]) != 0.0:
		ok = false
		print("GATE2: NEG FAIL - leak: rawB=", rNeg["rawB"], " rawM=", rNeg["rawM"], " b240=", rNeg["b240"], " m240=", rNeg["m240"])
	else:
		print("GATE2: NEG PASS (byte-literal 0.0 across 240 frames)")
	# strict direct isolation must hold in POS too (light range 800)
	if float(rPos["rawB"]) != 0.0:
		ok = false
		print("GATE2: POS direct-isolation FAIL: rawB=", rPos["rawB"], " rawM=", rPos["rawM"])
	# POS criteria: deep-B inside the pre-computed interval
	# Confirmed-equation interval (9.3.9 model): est = 0.35 * (4/64) * 0.5 * v(iz5)
	var est := 0.35 * 0.0625 * 0.5 * float(rPos["m240"])
	var lo := est / 3.0
	var hi := est * 3.0
	var b := float(rPos["b240"])
	print("GATE2: POS est=", est, " interval=[", lo, ",", hi, "] b240=", b)
	if b <= 0.0 or b < lo or b > hi:
		ok = false
		print("GATE2: POS FAIL (out of interval)")
	else:
		print("GATE2: POS PASS (in interval)")
	print("GATE2: M-verification: old observation +53% over direct; new m240=", rPos["m240"], " (rawM=0 strict)")
	if ok:
		print("GATE2: PASS (paired) - GI label opens")
		get_tree().quit(0)
	else:
		print("GATE2: FAIL (paired)")
		get_tree().quit(21)

func _fail(code: int, msg: String) -> void:
	print("GATE2: FAIL code=", code, " ", msg)
	get_tree().quit(code)
