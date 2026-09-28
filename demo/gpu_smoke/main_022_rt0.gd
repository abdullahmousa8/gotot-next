extends Node
# GNE-022 R0-RT: ray-tracing isolation test scene (test-only).
# Analytic check: triangle at z=-5; ray0 (-z) hits at t=5.0; ray1 (+z) misses (-1).

func _ready() -> void:
	var server := GneRenderServer.get_server_singleton()
	if server == null:
		_fail(400, "no server")
		return
	if not server.has_method("gpu_rt_selftest"):
		_fail(401, "gpu_rt_selftest not bound")
		return
	var r: PackedFloat32Array = server.gpu_rt_selftest()
	print("RT0: result=", r)
	if r.size() < 5:
		_fail(402, "short result: " + str(r))
		return
	var status := int(r[0])
	var hit_t := r[1]
	var miss_marker := r[2]
	var exp_t := r[3]
	var bytes := r[4]
	if status != 0:
		print("RT0: FAIL stage code=", status)
		get_tree().quit(20)
		return
	var ok := true
	if absf(hit_t - exp_t) > 0.001:
		ok = false
	if absf(miss_marker + 1.0) > 0.000001:
		ok = false
	if bytes < 32.0:
		ok = false
	print("RT0: hit_t=", hit_t, " miss=", miss_marker, " bytes=", bytes)
	if ok:
		print("RT0: PASS (analytic verify: hit t=", hit_t, " == expected ", exp_t, "; miss marker = ", miss_marker, ")")
		get_tree().quit(0)
	else:
		print("RT0: FAIL analytic verify")
		get_tree().quit(21)

func _fail(code: int, msg: String) -> void:
	print("RT0: FAIL code=", code, " ", msg)
	get_tree().quit(code)
