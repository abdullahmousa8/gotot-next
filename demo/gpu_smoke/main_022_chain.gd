extends Node
# GNE-022 diagnostic (Architect-ordered): measure c empirically at EVERY real cell
# along the z-chain between A and the deep probe - no single-point extrapolation.

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
	server.gpu_gi_config({"ambient": Vector3(0, 0, 0), "bounce": false})
	server.gpu_gi_reset()
	server.gpu_gi_trace()
	var raw_row := PackedFloat32Array()
	for iz in range(3, 13):
		raw_row.append(float((server.gpu_gi_read_avg(8 + 16 + iz * 128))[0]))
	print("CHAIN: raw_row iz=3..12 (z=-900..+900)=", raw_row)
	server.gpu_gi_config({"bounce": true})
	server.gpu_gi_reset()
	for f in range(1, 241):
		if not server.gpu_gi_accum_step():
			_fail(409, "accum")
			return
	var fin_row := PackedFloat32Array()
	for iz in range(3, 13):
		fin_row.append(float((server.gpu_gi_read_avg(8 + 16 + iz * 128))[0]))
	print("CHAIN: fin_row iz=3..12=", fin_row)
	var parts := PackedStringArray()
	for iz in range(3, 12):
		var a := fin_row[iz - 3]
		var b := fin_row[iz - 2]
		var c := (a / b) if b > 0.0 else -1.0
		parts.append("c(iz%d->%d)=%.3f" % [iz + 1, iz, c])
	print("CHAIN: hop ratios toward B: ", " ".join(parts))
	server.gpu_scene_destroy()
	print("CHAIN: DONE")
	get_tree().quit(0)

func _fail(code: int, msg: String) -> void:
	print("CHAIN: FAIL code=", code, " ", msg)
	get_tree().quit(code)
