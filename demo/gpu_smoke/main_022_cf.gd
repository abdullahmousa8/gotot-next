extends Node
# GNE-022 S1b.2 (a): closed-form case. One ground plane + one point light.
# Independent closed-form (reimplemented here from scratch, NOT reusing module
# code) predicts the radiance for exact (probe, texel) pairs; the GPU field must
# match. Also (b): a coherent-direction falloff profile row.

var server: GneRenderServer

func _octa_decode(fx: float, fy: float) -> Vector3:
	var nx := fx
	var ny := fy
	var nz := 1.0 - absf(fx) - absf(fy)
	var t := maxf(-nz, 0.0)
	nx += (-t) if nx >= 0.0 else t
	ny += (-t) if ny >= 0.0 else t
	var v := Vector3(nx, ny, nz)
	return v.normalized()

func _expected(probe_pos: Vector3, texel: int) -> float:
	# ground plane top at y=0; single light L=(0,500,0) range 1500 intensity 2 white
	var lx := texel % 8
	var ly := texel / 8
	var u := (float(lx) + 0.5) / 8.0
	var v := (float(ly) + 0.5) / 8.0
	var f := Vector2(u * 2.0 - 1.0, v * 2.0 - 1.0)
	var d := _octa_decode(f.x, f.y)
	if d.y >= -0.0001:
		return 0.03 # up / parallel: miss -> ambient
	var th := -probe_pos.y / d.y
	if th <= 0.0:
		return 0.03
	var hx := probe_pos.x + d.x * th
	var hz := probe_pos.z + d.z * th
	var ddx := 0.0 - hx
	var ddy := 500.0 - 0.0
	var ddz := 0.0 - hz
	var dist := sqrt(ddx * ddx + ddy * ddy + ddz * ddz)
	if dist >= 1500.0:
		return 0.0 # hit but out of range -> exactly 0 in the model
	var att := 1.0 - dist / 1500.0
	return 2.0 * att * att

func _ready() -> void:
	server = GneRenderServer.get_server_singleton()
	if server == null:
		_fail(400, "no server")
		return
	if not server.ensure_gpu_device():
		_fail(401, "no device")
		return
	if not server.gpu_scene_create(1, 1.0):
		_fail(402, "scene_create")
		return
	if not server.gpu_scene_dispatch(8):
		_fail(403, "dispatch")
		return
	if not server.gpu_mesh_create():
		_fail(404, "mesh_create")
		return
	# ground cube: top face at y=0
	server.gpu_scene_set_instance_transform(0, Vector3(0, -3000, 0), 6000.0)
	server.gpu_scene_set_instance_mesh(0, 0)
	var lid: int = server.gpu_light_create({"type": 0, "pos": Vector3(0, 500, 0), "range": 1500.0, "color": Color(1, 1, 1), "intensity": 2.0})
	print("S1CF: light=", lid)
	var cfg := {"min": Vector3(-1600, -50, -1600), "max": Vector3(1600, 750, 1600), "dims": Vector3i(16, 8, 16)}
	if not server.gpu_gi_create(cfg):
		_fail(405, "gi_create")
		return
	if not server.gpu_gi_trace():
		_fail(406, "trace")
		return
	# probe (7,3,7) -> p=951, pos (-100,300,-100); probe (0,3,7) -> p=944 pos (-1500,300,-100)
	var p_mid := 7 + 3 * 16 + 7 * 128
	var p_far := 0 + 3 * 16 + 7 * 128
	var pos_mid := Vector3(-100.0, 300.0, -100.0)
	var pos_far := Vector3(-1500.0, 300.0, -100.0)
	var checks := [
		{"p": p_mid, "t": 3, "pos": pos_mid, "tol": 0.02, "kind": "rel"},
		{"p": p_mid, "t": 59, "pos": pos_mid, "tol": 0.002, "kind": "abs"},
		{"p": p_far, "t": 0, "pos": pos_far, "tol": 0.0005, "kind": "abs"}
	]
	var ok := true
	for c in checks:
		var got: PackedFloat32Array = server.gpu_gi_read_texel(int(c["p"]), int(c["t"]))
		if got.size() < 3:
			_fail(407, "read_texel short")
			return
		var exp := _expected(c["pos"], int(c["t"]))
		var derr := absf(got[0] - exp)
		var pass_line := false
		if c["kind"] == "rel":
			pass_line = derr <= c["tol"] * maxf(exp, 0.01) + 0.001
		else:
			pass_line = derr <= float(c["tol"])
		print("S1CF: probe=", c["p"], " texel=", c["t"], " got=", got[0], " expected=", exp, " err=", derr, " ok=", pass_line)
		if not pass_line:
			ok = false
	# (b) falloff row: probes (ix,1,7), ix=0..15
	var row: Array = []
	for ix in range(16):
		var pr: PackedFloat32Array = server.gpu_gi_read_avg(ix + 1 * 16 + 7 * 128)
		row.append(pr[0])
	print("S1CF: falloff row (ix=0..15)=", row)
	if row[7] <= row[0] or row[7] <= row[15]:
		ok = false
	server.gpu_scene_destroy()
	if ok:
		print("S1CF: PASS")
		get_tree().quit(0)
	else:
		print("S1CF: FAIL")
		get_tree().quit(21)

func _fail(code: int, msg: String) -> void:
	print("S1CF: FAIL code=", code, " ", msg)
	get_tree().quit(code)
