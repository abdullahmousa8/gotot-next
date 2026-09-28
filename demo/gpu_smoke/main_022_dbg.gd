extends Node
# GNE-022 diagnostic: actual segment tn values at the coupling gather for the
# deep probe (and contrasts), via debug mode 3. POS scene (no barrier).

var server: GneRenderServer

func _ready() -> void:
	server = GneRenderServer.get_server_singleton()
	if server == null:
		_fail(400, "no server")
		return
	if not server.ensure_gpu_device():
		_fail(401, "no device")
		return
	server.gpu_scene_create(5, 1.0)
	server.gpu_scene_dispatch(8)
	server.gpu_mesh_create()
	server.gpu_scene_set_instance_transform(0, Vector3(0, -3000, -900), 6000.0)
	server.gpu_scene_set_instance_mesh(0, 0)
	for i in range(4):
		server.gpu_scene_set_instance_transform(i + 1, Vector3(0, -9000, 0), 1.0)
		server.gpu_scene_set_instance_mesh(i + 1, 0)
	server.gpu_light_create({"type": 0, "pos": Vector3(0, 300, 750), "range": 800.0, "color": Color(1, 1, 1), "intensity": 10.0})
	var cfg := {"min": Vector3(-1600, -50, -1600), "max": Vector3(1600, 750, 1600), "dims": Vector3i(16, 8, 16)}
	if not server.gpu_gi_create(cfg):
		_fail(405, "gi_create")
		return
	server.gpu_gi_config({"debug": true, "ambient": Vector3(0, 0, 0), "bounce": false})
	server.gpu_gi_reset()
	if not server.gpu_gi_trace():
		_fail(406, "debug dispatch")
		return
	for pp in [{"n": "B", "p": 8 + 16 + 3 * 128}, {"n": "M", "p": 8 + 16 + 5 * 128}, {"n": "A", "p": 8 + 16 + 10 * 128}]:
		var p := int(pp["p"])
		var min_tn := 1e9
		var blk_total := 0.0
		var miss := 0
		var samples := PackedFloat32Array()
		for t in range(0, 16):
			var v: PackedFloat32Array = server.gpu_gi_read_texel(p, t)
			if v.size() < 3:
				continue
			if v[0] >= -2.5 and v[0] <= -1.5:
				miss += 1
				continue
			if v[0] >= 0.0 and v[0] < min_tn:
				min_tn = v[0]
			blk_total += v[1]
			if samples.size() < 8:
				samples.append(v[0])
		print("DBG[", pp["n"], "]: min_tn=", min_tn, " blocked_total(16 texels)=", blk_total, " miss_texels=", miss, " first_tns=", samples)
	server.gpu_scene_destroy()
	print("DBG: DONE")
	get_tree().quit(0)

func _fail(code: int, msg: String) -> void:
	print("DBG: FAIL code=", code, " ", msg)
	get_tree().quit(code)
