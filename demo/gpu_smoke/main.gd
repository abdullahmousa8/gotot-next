extends Node

const INSTANCE_COUNT := 100000
const SPREAD := 12000.0


func _camera_looking_at(target: Vector3, pos: Vector3) -> Transform3D:
	var basis: Basis = Basis.looking_at(target - pos, Vector3.UP)
	return Transform3D(basis, pos)


func _ready() -> void:
	var server: GototRenderServer = GototRenderServer.get_server_singleton()
	if server == null:
		print("GOTOT-SMOKE: FAIL server singleton is null")
		get_tree().quit(1)
		return

	if not server.ensure_gpu_device():
		print("GOTOT-SMOKE: FAIL local RenderingDevice not available")
		get_tree().quit(2)
		return

	print("GOTOT-SMOKE: OK is_gpu_ready = ", server.is_gpu_ready())

	var t0 := Time.get_ticks_usec()
	if not server.gpu_scene_create(INSTANCE_COUNT, SPREAD):
		print("GOTOT-SMOKE: FAIL gpu_scene_create")
		get_tree().quit(3)
		return
	var t1 := Time.get_ticks_usec()

	if not server.gpu_scene_dispatch(42):
		print("GOTOT-SMOKE: FAIL gpu_scene_dispatch")
		get_tree().quit(4)
		return
	var t2 := Time.get_ticks_usec()

	print("GOTOT-SMOKE-001B: instances = ", server.gpu_scene_get_instance_count())
	print("GOTOT-SMOKE-001B: create_ms = ", (t1 - t0) / 1000.0, " fill_ms = ", (t2 - t1) / 1000.0)

	var sample := server.gpu_scene_readback_positions(0, INSTANCE_COUNT)
	if sample.size() != INSTANCE_COUNT:
		print("GOTOT-SMOKE: FAIL readback size = ", sample.size())
		get_tree().quit(5)
		return

	# ---- GOTOT-002: GPU frustum culling ----
	var projection := Projection.create_perspective(60.0, 16.0 / 9.0, 0.1, 2000.0)
	var look_at_origin := _camera_looking_at(Vector3.ZERO, Vector3(800.0, 400.0, 600.0))
	server.gpu_scene_set_camera(look_at_origin, projection)

	var planes := server.gpu_scene_get_frustum_planes()
	if planes.size() != 6:
		print("GOTOT-SMOKE: FAIL frustum planes = ", planes.size())
		get_tree().quit(6)
		return

	var t3 := Time.get_ticks_usec()
	if not server.gpu_cull_dispatch():
		print("GOTOT-SMOKE: FAIL gpu_cull_dispatch")
		get_tree().quit(7)
		return
	var t4 := Time.get_ticks_usec()
	var gpu_count_a := server.gpu_cull_get_visible_count()

	server.gpu_cull_dispatch()
	var gpu_count_b := server.gpu_cull_get_visible_count()

	print("GOTOT-SMOKE-002: cull_ms = ", (t4 - t3) / 1000.0,
			" gpu_visible_a = ", gpu_count_a, " gpu_visible_b = ", gpu_count_b)

	if gpu_count_a != gpu_count_b:
		print("GOTOT-SMOKE: FAIL culling not deterministic: ", gpu_count_a, " vs ", gpu_count_b)
		get_tree().quit(8)
		return

	# Golden sanity: center camera culls most of a 12K-wide scene (not all, not zero).
	if gpu_count_a <= 100 or gpu_count_a >= 90000:
		print("GOTOT-SMOKE: FAIL suspicious visible count for centered camera: ", gpu_count_a)
		get_tree().quit(9)
		return

	# Golden sanity: camera looking away must cull (almost) everything.
	var look_away := _camera_looking_at(look_at_origin.origin * 2.0, look_at_origin.origin)
	server.gpu_scene_set_camera(look_away, projection)
	server.gpu_cull_dispatch()
	var gpu_count_away := server.gpu_cull_get_visible_count()
	if gpu_count_away > 500:
		print("GOTOT-SMOKE: FAIL look-away camera kept ", gpu_count_away, " instances")
		get_tree().quit(10)
		return
	print("GOTOT-SMOKE-002: look-away gpu_visible = ", gpu_count_away)

	# GPU == CPU reference over the full set, using the server's own planes.
	server.gpu_scene_set_camera(look_at_origin, projection)
	server.gpu_cull_dispatch()
	var t5 := Time.get_ticks_usec()
	var visibility := server.gpu_cull_get_visibility()
	var scales := server.gpu_scene_readback_scales(0, INSTANCE_COUNT)
	var cpu_count := 0
	var cpu_visible: PackedInt32Array = []
	for i in INSTANCE_COUNT:
		var p: Vector3 = sample[i]
		var inside := true
		for pl in planes:
			if pl.x * p.x + pl.y * p.y + pl.z * p.z + pl.w < -scales[i]:
				inside = false
				break
		if inside:
			cpu_count += 1
			cpu_visible.append(i)
			if visibility[i] != 1:
				print("GOTOT-SMOKE: FAIL visibility[", i, "] = ", visibility[i], " but CPU says visible")
				get_tree().quit(11)
				return
		elif visibility[i] != 0:
			print("GOTOT-SMOKE: FAIL visibility[", i, "] = ", visibility[i], " but CPU says hidden")
			get_tree().quit(12)
			return
	if cpu_count != gpu_count_a:
		print("GOTOT-SMOKE: FAIL GPU count ", gpu_count_a, " != CPU count ", cpu_count)
		get_tree().quit(13)
		return
	print("GOTOT-SMOKE-002: gpu == cpu == ", cpu_count, " (ref_ms = ", (Time.get_ticks_usec() - t5) / 1000.0, ")")

	# ---- GOTOT-003: GPU indirect draw args + compacted visible list ----
	var t6 := Time.get_ticks_usec()
	if not server.gpu_drawargs_finalize():
		print("GOTOT-SMOKE: FAIL gpu_drawargs_finalize")
		get_tree().quit(14)
		return
	var args_a := server.gpu_drawargs_read()
	server.gpu_drawargs_finalize()
	var args_b := server.gpu_drawargs_read()
	var t7 := Time.get_ticks_usec()

	print("GOTOT-SMOKE-003: finalize_ms = ", (t7 - t6) / 1000.0,
			" args = ", args_a, " args_deterministic = ", args_a == args_b)

	if args_a.size() != 5:
		print("GOTOT-SMOKE: FAIL drawargs size = ", args_a.size())
		get_tree().quit(15)
		return
	if args_a[0] != 6 or args_a[1] != cpu_count or args_a[2] != 0 or args_a[3] != 0 or args_a[4] != 0:
		print("GOTOT-SMOKE: FAIL invalid indirect args: ", args_a)
		get_tree().quit(16)
		return

	var compact := server.gpu_compact_read()
	compact.sort()
	cpu_visible.sort()
	print("GOTOT-SMOKE-003: compact_ids = ", compact.size())
	if compact != cpu_visible:
		print("GOTOT-SMOKE: FAIL compact list does not match CPU visible set (", compact.size(), " vs ", cpu_visible.size(), ")")
		get_tree().quit(17)
		return

	# ---- GOTOT-004: HZB occlusion ----
	server.gpu_scene_set_viewport(1920.0, 1080.0)

	# Occluder wall between camera and origin, covering the central view.
	var wall: PackedVector4Array = [
		Vector4(-350.0, -350.0, -350.0, 0.0),
		Vector4(350.0, 350.0, -80.0, 0.0),
	]
	server.gpu_scene_set_occluders(wall)

	var t8 := Time.get_ticks_usec()
	if not server.gpu_visibility_dispatch():
		print("GOTOT-SMOKE: FAIL gpu_visibility_dispatch")
		get_tree().quit(18)
		return
	var occ_visible_a := server.gpu_cull_get_visible_count()
	var t9 := Time.get_ticks_usec()

	server.gpu_visibility_dispatch()
	var occ_visible_b := server.gpu_cull_get_visible_count()

	print("GOTOT-SMOKE-004: visibility_ms = ", (t9 - t8) / 1000.0,
			" frustum_only = ", gpu_count_a,
			" hzb_visible_a = ", occ_visible_a, " hzb_visible_b = ", occ_visible_b)

	if occ_visible_a != occ_visible_b:
		print("GOTOT-SMOKE: FAIL HZB culling not deterministic: ", occ_visible_a, " vs ", occ_visible_b)
		get_tree().quit(19)
		return
	if occ_visible_a <= 0 or occ_visible_a >= gpu_count_a:
		print("GOTOT-SMOKE: FAIL HZB should reduce visibility strictly (", occ_visible_a, " of ", gpu_count_a, ")")
		get_tree().quit(20)
		return

	if not server.gpu_drawargs_finalize():
		print("GOTOT-SMOKE: FAIL gpu_drawargs_finalize (post-HZB)")
		get_tree().quit(21)
		return
	var occ_args := server.gpu_drawargs_read()
	if occ_args.size() != 5 or occ_args[1] != occ_visible_a:
		print("GOTOT-SMOKE: FAIL HZB drawargs mismatch: ", occ_args)
		get_tree().quit(22)
		return
	print("GOTOT-SMOKE-004: post-HZB args = ", occ_args)

	# Control: without occluders the count must return to frustum-only.
	server.gpu_scene_set_occluders(PackedVector4Array())
	server.gpu_visibility_dispatch()
	var occ_visible_control := server.gpu_cull_get_visible_count()
	if occ_visible_control != gpu_count_a:
		print("GOTOT-SMOKE: FAIL no-occluder control ", occ_visible_control, " != frustum-only ", gpu_count_a)
		get_tree().quit(23)
		return
	print("GOTOT-SMOKE-004: no-occluder control = ", occ_visible_control)

	# ---- GOTOT-005: GPU indirect raster draw ----
	if not server.gpu_drawargs_finalize():
		print("GOTOT-SMOKE: FAIL gpu_drawargs_finalize (raster)")
		get_tree().quit(24)
		return
	var t10 := Time.get_ticks_usec()
	if not server.gpu_raster_indirect_draw():
		print("GOTOT-SMOKE: FAIL gpu_raster_indirect_draw")
		get_tree().quit(25)
		return
	var t11 := Time.get_ticks_usec()
	var pixels_a := server.gpu_raster_read_pixels()
	if pixels_a.size() != 1920 * 1080 * 4:
		print("GOTOT-SMOKE: FAIL raster readback size = ", pixels_a.size())
		get_tree().quit(26)
		return
	var colored_a := _count_magenta(pixels_a)

	server.gpu_raster_indirect_draw()
	var colored_b := _count_magenta(server.gpu_raster_read_pixels())

	print("GOTOT-SMOKE-005: draw_ms = ", (t11 - t10) / 1000.0,
			" colored = ", colored_a, " deterministic = ", colored_a == colored_b)

	if colored_a <= 0:
		print("GOTOT-SMOKE: FAIL raster drew nothing: ", colored_a)
		get_tree().quit(27)
		return
	if colored_a != colored_b:
		print("GOTOT-SMOKE: FAIL raster not deterministic: ", colored_a, " vs ", colored_b)
		get_tree().quit(28)
		return

	# Every frustum-visible instance must have a magenta pixel at (or near) its projected center.
	# clip = VP * world using the exact matrix the shaders used (column-major floats).
	var magenta_pixels: PackedVector2Array = []
	for idx in pixels_a.size() / 4:
		if pixels_a[idx * 4] > 200 and pixels_a[idx * 4 + 2] > 200:
			magenta_pixels.append(Vector2(idx % 1920, idx / 1920))

	if magenta_pixels.size() != colored_a:
		print("GOTOT-SMOKE: FAIL magenta pixel list mismatch")
		get_tree().quit(29)
		return

	var vp := server.gpu_scene_get_vp()
	if vp.size() != 16:
		print("GOTOT-SMOKE: FAIL gpu_scene_get_vp size = ", vp.size())
		get_tree().quit(29)
		return
	var best_convx := 0
	var best_convy := 0
	var best_checked := 0
	var best_miss := 0
	for convx: int in [1, -1]:
		for convy: int in [1, -1]:
			var checked := 0
			var small := 0
			var miss := 0
			for i in cpu_visible:
				var p: Vector3 = sample[i]
				var cx: float = vp[0] * p.x + vp[4] * p.y + vp[8] * p.z + vp[12]
				var cy: float = vp[1] * p.x + vp[5] * p.y + vp[9] * p.z + vp[13]
				var cz: float = vp[2] * p.x + vp[6] * p.y + vp[10] * p.z + vp[14]
				var cw: float = vp[3] * p.x + vp[7] * p.y + vp[11] * p.z + vp[15]
				if cw <= 0.0:
					continue
				var ndcx := cx / cw
				var ndcy := cy / cw
				if ndcx < -1.0 or ndcx > 1.0 or ndcy < -1.0 or ndcy > 1.0:
					continue
				var px: float = (ndcx * 0.5 + 0.5) * 1920.0
				var py: float = (0.5 + convy * 0.5 * ndcy) * 1080.0
				# Projected half-size in pixels: s_px = scale * (H/2) / (dist * tanHalfFovV), tanHalfFovV = 1/vp[5].
				var dist := look_at_origin.origin.distance_to(sample[i])
				if dist <= 0.0:
					continue
				var s_px: float = scales[i] * 540.0 / (vp[5] * dist)
				checked += 1
				if s_px < 1.0:
					small += 1
				var hit := false
				for m in magenta_pixels:
					if abs(m.x - px) <= 2.0 and abs(m.y - py) <= 2.0:
						hit = true
						break
				if not hit:
					miss += 1
			# Subpixel quads (< 1 projected px) may legitimately rasterize zero pixels.
			var eff_miss := maxi(miss - small, 0)
			if checked > best_checked or (checked == best_checked and eff_miss < best_miss):
				best_convx = convx
				best_convy = convy
				best_checked = checked
				best_miss = eff_miss
	if best_checked == 0 or best_miss != 0:
		print("GOTOT-SMOKE: FAIL projected centers missing color: checked = ", best_checked,
				", miss = ", best_miss, " (convx ", best_convx, ", convy ", best_convy, ")")
		get_tree().quit(30)
		return
	print("GOTOT-SMOKE-005: projected centers = ", best_checked, " all matched (convx ", best_convx, ", convy ", best_convy, ")")

	# ---- GOTOT-006: scaling benchmark 100K -> 1M -> 10M ----
	var look_origin := _camera_looking_at(Vector3.ZERO, Vector3(800.0, 400.0, 600.0))
	print("GOTOT-SMOKE-006: instance_count | create_ms fill_ms cull_ms finalize_ms visibility_ms raster_ms | visible")
	for count in [100000, 1000000, 10000000]:
		var bench := _bench_scale(server, count, look_origin, projection)
		if bench.is_empty():
			print("GOTOT-SMOKE: FAIL benchmark at count ", count)
			get_tree().quit(31)
			return
		print("GOTOT-SMOKE-006: ", count,
				" | ", bench["create_ms"], " ", bench["fill_ms"], " ", bench["cull_ms"],
				" ", bench["finalize_ms"], " ", bench["visibility_ms"], " ", bench["raster_ms"],
				" | ", bench["visible"])

	server.gpu_scene_destroy()
	print("GOTOT-SMOKE: OK (001B fill + 002 culling + 003 indirect args + 004 HZB occlusion + 005 indirect raster + 006 scaling verified)")
	get_tree().quit(0)


func _bench_scale(server: GototRenderServer, count: int, camera: Transform3D, projection: Projection) -> Dictionary:
	var t0 := Time.get_ticks_usec()
	if not server.gpu_scene_create(count, 12000.0):
		return {}
	var t1 := Time.get_ticks_usec()
	if not server.gpu_scene_dispatch(0):
		return {}
	var t2 := Time.get_ticks_usec()
	server.gpu_scene_set_camera(camera, projection)
	if not server.gpu_cull_dispatch():
		return {}
	var t3 := Time.get_ticks_usec()
	var visible := server.gpu_cull_get_visible_count()
	if not server.gpu_drawargs_finalize():
		return {}
	var t4 := Time.get_ticks_usec()
	if not server.gpu_visibility_dispatch():
		return {}
	var t5 := Time.get_ticks_usec()
	if not server.gpu_drawargs_finalize():
		return {}
	if not server.gpu_raster_indirect_draw():
		return {}
	var t6 := Time.get_ticks_usec()
	server.gpu_scene_destroy()
	return {
		"create_ms": (t1 - t0) / 1000.0,
		"fill_ms": (t2 - t1) / 1000.0,
		"cull_ms": (t3 - t2) / 1000.0,
		"finalize_ms": (t4 - t3) / 1000.0,
		"visibility_ms": (t5 - t4) / 1000.0,
		"raster_ms": (t6 - t5) / 1000.0,
		"visible": visible,
	}


func _count_magenta(pixels: PackedByteArray) -> int:
	var c := 0
	var n := pixels.size() / 4
	for i in n:
		if pixels[i * 4] > 200 and pixels[i * 4 + 2] > 200:
			c += 1
	return c