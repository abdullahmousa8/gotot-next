extends Node
# GNE-022 S1a smoke: probe field sanity. Ground + one lit wall + one point light.
# Expected: the probe near the light reads a clearly brighter average radiance
# than a far-field probe (both above the miss-ambient floor 0.03).

func _ready() -> void:
	var server := GneRenderServer.get_server_singleton()
	if server == null:
		_fail(400, "no server")
		return
	if not server.ensure_gpu_device():
		_fail(401, "no device")
		return
	if not server.gpu_scene_create(2, 1.0):
		_fail(402, "scene_create")
		return
	if not server.gpu_scene_dispatch(8):
		_fail(403, "dispatch")
		return
	if not server.gpu_mesh_create():
		_fail(404, "mesh_create")
		return
	# ground: big cube, top face at y=0
	server.gpu_scene_set_instance_transform(0, Vector3(0, -2000, 0), 4000.0)
	server.gpu_scene_set_instance_mesh(0, 0)
	# wall cube near the light
	server.gpu_scene_set_instance_transform(1, Vector3(320, 300, 0), 600.0)
	server.gpu_scene_set_instance_mesh(1, 0)
	# one point light
	var lid: int = server.gpu_light_create({"type": 0, "pos": Vector3(0, 150, 0), "range": 1200.0, "color": Color(1.0, 0.95, 0.9), "intensity": 3.0})
	print("S1A: light id=", lid)
	# probe field (8 x 4 x 8 = 256 probes)
	var cfg := {"min": Vector3(-760, -100, -760), "max": Vector3(760, 900, 760), "dims": Vector3i(8, 4, 8)}
	if not server.gpu_gi_create(cfg):
		_fail(405, "gi_create")
		return
	var info: Dictionary = server.gpu_gi_info()
	print("S1A: info=", info)
	if not server.gpu_gi_trace():
		_fail(406, "gi_trace")
		return
	# near-light probe index (3,1,3) -> p = 3 + 1*8 + 3*64 = 203 ; far probe (0,1,0) -> 8
	var near: PackedFloat32Array = server.gpu_gi_read_avg(203)
	var far: PackedFloat32Array = server.gpu_gi_read_avg(8)
	print("S1A: near(203)=", near)
	print("S1A: far(8)=", far)
	if near.size() < 3 or far.size() < 3:
		_fail(407, "readback short")
		return
	var ok := true
	if near[0] <= far[0]:
		ok = false
	if near[0] <= 0.031:
		ok = false
	if far[0] <= 0.031:
		ok = false
	if ok:
		server.gpu_scene_destroy()
		print("S1A: PASS (near r=", near[0], " > far r=", far[0], " > ambient 0.03)")
		get_tree().quit(0)
	else:
		server.gpu_scene_destroy()
		print("S1A: FAIL sanity (near=", near[0], " far=", far[0], ")")
		get_tree().quit(21)

func _fail(code: int, msg: String) -> void:
	print("S1A: FAIL code=", code, " ", msg)
	get_tree().quit(code)
