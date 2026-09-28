extends Node

# GNE-016 — Materials closure scene (criteria C1-C8, SPEC 016 FINAL).
#
# Scene: 8 instances (4 cube + 4 octahedron), each on its OWN mesh slot so it
# owns one material (slots 0-3 cube geometry, 4-7 octa geometry). Camera is a
# 3/4 view from the SAME side the fixed light comes from, so visible faces
# span N.L 0.41..0.82 and the histogram has real range.
#
# Canonical light (D4-1, frozen for DET): L=normalize(-0.5,-1.0,-0.5), amb=0.1.
# Probes L2/L3 are temporary rotations, always restored before the signature.
# Negative paths (invalid ids) are NOT executed in-gate: they print_error by
# contract, and the gate fails on any ERROR: line. They are verified by code
# inspection + an off-gate manual run (see _negtest, --negtest flag).

const INSTANCE_COUNT := 8
const RASTER_W := 1920
const RASTER_H := 1080
const CAM_POS := Vector3(-850, -550, 2100)
const LIGHT_CANON := Vector3(-0.5, -1.0, -0.5)
const LIGHT_MIRROR := Vector3(0.5, 1.0, -0.5)
const LIGHT_DARK := Vector3(0.5, 1.0, 0.5)

const PTS: Array[Vector3] = [
	Vector3(0, 0, -700),
	Vector3(-350, 150, -850),
	Vector3(350, -150, -900),
	Vector3(0, 320, -800),
	Vector3(-300, -250, -750),
	Vector3(300, 250, -950),
	Vector3(0, -120, -650),
	Vector3(-150, 120, -1000),
]
const SCALES: Array[float] = [50.0, 40.0, 45.0, 35.0, 40.0, 55.0, 35.0, 40.0]
const ALBEDOS: Array[Color] = [
	Color(1, 0, 0), Color(0, 1, 0), Color(0, 0, 1), Color(1, 1, 0),
	Color(0, 1, 1), Color(1, 0, 1), Color(1, 1, 1), Color(0.5, 0.5, 0.5),
]
const ROUGHS: Array[float] = [0.5, 0.5, 0.5, 0.4, 0.6, 0.5, 0.1, 0.9]
const METALS: Array[float] = [0.0, 0.0, 0.0, 0.2, 0.4, 0.0, 0.0, 0.8]

const OCTA_VERTS: Array[Vector3] = [
	Vector3(0, 0, 1),
	Vector3(1, 0, 0),
	Vector3(0, 1, 0),
	Vector3(-1, 0, 0),
	Vector3(0, -1, 0),
	Vector3(0, 0, -1),
]
const OCTA_INDICES: Array[int] = [
	0, 2, 1, 0, 3, 2, 0, 4, 3, 0, 1, 4,
	5, 1, 2, 5, 2, 3, 5, 3, 4, 5, 4, 1,
]

var server: GneRenderServer
var camera: Camera3D
var display: TextureRect
var image_tex: ImageTexture
var sig_file := ""
var negtest := false

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--sigf="):
			sig_file = a.split("=")[1]
		elif a == "--negtest":
			negtest = true
	_setup()

func _setup() -> void:
	server = GneRenderServer.get_server_singleton()
	if server == null:
		_fail(800, "server singleton is null")
		return
	if not server.ensure_gpu_device():
		_fail(801, "local RenderingDevice not available")
		return
	if negtest:
		_negtest()
		return
	if not server.gpu_scene_create(INSTANCE_COUNT, 1.0):
		_fail(802, "gpu_scene_create(8)")
		return
	if not server.gpu_scene_dispatch(8):
		_fail(803, "gpu_scene_dispatch(8)")
		return
	if not server.gpu_mesh_create():
		_fail(804, "gpu_mesh_create")
		return
	# Slots 1-3: cube geometry again; slots 4-7: octahedron. Slot 0 = cube.
	var cube_v := PackedVector3Array([
		Vector3(-0.5, -0.5, -0.5), Vector3(0.5, -0.5, -0.5),
		Vector3(0.5, 0.5, -0.5), Vector3(-0.5, 0.5, -0.5),
		Vector3(-0.5, -0.5, 0.5), Vector3(0.5, -0.5, 0.5),
		Vector3(0.5, 0.5, 0.5), Vector3(-0.5, 0.5, 0.5),
	])
	var cube_i := PackedInt32Array([
		4, 5, 6, 4, 6, 7, 1, 0, 3, 1, 3, 2, 0, 4, 7, 0, 7, 3,
		5, 1, 2, 5, 2, 6, 3, 7, 6, 3, 6, 2, 0, 1, 5, 0, 5, 4,
	])
	for m in 3:
		var mid := server.gpu_mesh_create_from_arrays(cube_v, cube_i)
		if mid != m + 1:
			_fail(805, "cube slot " + str(m + 1) + " got id " + str(mid))
			return
	var octa_v := PackedVector3Array(OCTA_VERTS)
	var octa_i := PackedInt32Array(OCTA_INDICES)
	for m in 4:
		var mid := server.gpu_mesh_create_from_arrays(octa_v, octa_i)
		if mid != m + 4:
			_fail(805, "octa slot " + str(m + 4) + " got id " + str(mid))
			return
	for i in INSTANCE_COUNT:
		server.gpu_scene_set_instance_transform(i, PTS[i], SCALES[i])
		server.gpu_scene_set_instance_mesh(i, i)
	if not server.gpu_mesh_set_batch_strategy(0):
		_fail(806, "set_batch_strategy(PER_MESH)")
		return
	if not server.gpu_material_create():
		_fail(807, "gpu_material_create")
		return
	# C1 setup: 8 distinct materials.
	for i in INSTANCE_COUNT:
		if not server.gpu_material_set_albedo(i, ALBEDOS[i]):
			_fail(808, "set_albedo " + str(i))
			return
		if not server.gpu_material_set_params(i, ROUGHS[i], METALS[i]):
			_fail(808, "set_params " + str(i))
			return
		if not server.gpu_material_set_specular(i, Color(1, 1, 1), 32.0):
			_fail(808, "set_specular " + str(i))
			return
	var em_on := true
	if not server.gpu_material_set_emissive(5, Color(1, 0, 0), 1.0, em_on, false):
		_fail(808, "set_emissive 5")
		return
	if not server.gpu_material_set_light(LIGHT_CANON):
		_fail(809, "set_light canonical")
		return
	camera = $Camera
	display = $Overlay/Display
	camera.global_position = CAM_POS
	camera.look_at(Vector3.ZERO, Vector3.UP)
	camera.near = 300.0
	camera.far = 4000.0
	var vp_size: Vector2 = get_viewport().get_visible_rect().size
	server.gpu_scene_set_viewport(vp_size.x, vp_size.y)
	server.gpu_scene_set_camera(camera.get_global_transform(), camera.get_camera_projection())
	_measure()

func _measure() -> void:
	if not server.gpu_cull_dispatch():
		_fail(810, "gpu_cull_dispatch")
		return
	var visible := server.gpu_cull_get_visible_count()
	if visible != INSTANCE_COUNT:
		_fail(811, "frustum must keep all 8, got " + str(visible))
		return
	if not server.gpu_visibility_dispatch():
		_fail(812, "gpu_visibility_dispatch")
		return
	if not server.gpu_mesh_batch_dispatch():
		_fail(813, "gpu_mesh_batch_dispatch")
		return
	if not server.gpu_material_draw():
		_fail(814, "gpu_material_draw")
		return
	var pixels := server.gpu_raster_read_pixels()
	if pixels.size() != RASTER_W * RASTER_H * 4:
		_fail(815, "pixels size")
		return
	_show(pixels)
	# --- C1: readback matches every written record ---
	for i in INSTANCE_COUNT:
		var rb: Dictionary = server.gpu_material_readback(i)
		if rb.is_empty():
			_fail(820, "readback empty " + str(i))
			return
		var a: Vector3 = rb["albedo"]
		if (a - Vector3(ALBEDOS[i].r, ALBEDOS[i].g, ALBEDOS[i].b)).length() > 0.001:
			_fail(821, "albedo mismatch " + str(i))
			return
		if absf(float(rb["roughness"]) - ROUGHS[i]) > 0.0001:
			_fail(821, "roughness mismatch " + str(i))
			return
		if absf(float(rb["metallic"]) - METALS[i]) > 0.0001:
			_fail(821, "metallic mismatch " + str(i))
			return
	print("GNE 016: C1 material store OK (8/8 readback identical)")
	# --- anchor: brightest candidate of instance-0 face center (y-flip free) ---
	var vp := server.gpu_scene_get_vp()
	if vp.size() != 16:
		_fail(822, "vp size")
		return
	var anchor := _anchor_px(pixels, vp, PTS[3] + Vector3(-SCALES[3] * 0.5, 0, 0), 0.2)
	if anchor.x < 0:
		_fail(823, "anchor not found")
		return
	# --- C3: face uniformity around anchor ---
	if not _window_uniform(pixels, anchor, 1, 3.0):
		_fail(824, "face not uniform at " + str(anchor))
		return
	print("GNE 016: C3 face uniformity OK")
	# --- C4: rotate light -> predicted change ---
	var b1 := _brightness(pixels, anchor)
	if not server.gpu_material_set_light(LIGHT_MIRROR):
		_fail(825, "set_light mirror")
		return
	if not _redraw():
		return
	var pixels_m := server.gpu_raster_read_pixels()
	var b2 := _brightness(pixels_m, anchor)
	var colored := _colored_count(pixels, 0.05)
	var changed := _changed_count(pixels, pixels_m, 0.02)
	print("GNE 016: C4 probe ", b1, " -> ", b2, " colored=", colored, " changed=", changed)
	if b1 - b2 < 0.1:
		_fail(826, "light rotation did not dim probe")
		return
	if colored < 500:
		_fail(826, "scene too sparse, colored=" + str(colored))
		return
	if changed < colored / 4:
		_fail(826, "too few changed pixels")
		return
	if not server.gpu_material_set_light(LIGHT_CANON):
		_fail(825, "restore light")
		return
	print("GNE 016: C4 N.L response OK")
	# --- C5: same material, roughness toggle on a diffuse face ---
	# Anchor: instance-0 -X face (lit, N.L=0.41). Same pixels measured twice,
	# so only roughness can explain a change. Temp shininess 8 widens the span.
	var anchor5 := _anchor_px(pixels, vp, PTS[0] + Vector3(-SCALES[0] * 0.5, 0, 0), 0.1)
	if anchor5.x < 0:
		_fail(827, "c5 anchor not found")
		return
	if not server.gpu_material_set_specular(0, Color(1, 1, 1), 8.0):
		_fail(827, "temp shininess")
		return
	if not server.gpu_material_set_params(0, 0.1, 0.0):
		_fail(827, "rough 0.1")
		return
	if not _redraw():
		return
	var m_a := _window_mean(server.gpu_raster_read_pixels(), anchor5, 1)
	if not server.gpu_material_set_params(0, 0.9, 0.0):
		_fail(827, "rough 0.9")
		return
	if not _redraw():
		return
	var m_b := _window_mean(server.gpu_raster_read_pixels(), anchor5, 1)
	print("GNE 016: C5 rough means ", m_a, " vs ", m_b)
	if absf(m_a - m_b) < 0.05:
		_fail(828, "roughness had no measurable effect")
		return
	if not server.gpu_material_set_params(0, ROUGHS[0], METALS[0]):
		_fail(827, "restore rough")
		return
	if not server.gpu_material_set_specular(0, Color(1, 1, 1), 32.0):
		_fail(827, "restore shininess")
		return
	print("GNE 016: C5 roughness measurable OK")
	# --- C6: emissive in a dark field ---
	if not server.gpu_material_set_light(LIGHT_DARK):
		_fail(829, "set_light dark")
		return
	if not _redraw():
		return
	var e_on := _emissive_count(server.gpu_raster_read_pixels())
	if not server.gpu_material_set_emissive(5, Color(1, 0, 0), 1.0, false, false):
		_fail(829, "emissive off")
		return
	if not _redraw():
		return
	var e_off := _emissive_count(server.gpu_raster_read_pixels())
	print("GNE 016: C6 emissive on=", e_on, " off=", e_off)
	if e_on < 50 or e_off != 0:
		_fail(830, "emissive evidence bad")
		return
	if not server.gpu_material_set_emissive(5, Color(1, 0, 0), 1.0, true, false):
		_fail(829, "emissive restore")
		return
	if not server.gpu_material_set_light(LIGHT_CANON):
		_fail(829, "light restore")
		return
	print("GNE 016: C6 emissive OK")
	# --- final canonical pass + signature ---
	if not _redraw():
		return
	var fin := server.gpu_raster_read_pixels()
	_show(fin)
	var h := _histogram(fin)
	print("GNE 016: C5 hist hr=", h[1])
	if h[1] < 0.3:
		_fail(831, "histogram range too narrow")
		return
	var st: Dictionary = server.gpu_material_stats()
	if int(st["bytes"]) != 4096 or int(st["slots"]) != 64 or int(st["dispatches"]) <= 0:
		_fail(832, "stats wrong " + str(st))
		return
	print("GNE 016: C8 stats OK ", st)
	var ln := LIGHT_CANON.normalized()
	var sig := "v16|mc=8|L%.2f|%.2f|%.2f|amb=0.10|hp=%d|hr=%.2f|d1" % [ln.x, ln.y, ln.z, h[2], h[1]]
	print("GNE 016: sig=" + sig)
	if sig_file != "":
		var f := FileAccess.open(sig_file, FileAccess.WRITE)
		if f == null:
			_fail(833, "sig file write failed")
			return
		f.store_string(sig + "\n")
	print("GNE 016: PASS")
	get_tree().quit(0)

func _redraw() -> bool:
	if not server.gpu_cull_dispatch():
		_fail(840, "recull")
		return false
	if not server.gpu_visibility_dispatch():
		_fail(841, "revisibility")
		return false
	if not server.gpu_mesh_batch_dispatch():
		_fail(842, "rebatch")
		return false
	if not server.gpu_material_draw():
		_fail(843, "redraw")
		return false
	return true

func _show(pixels: PackedByteArray) -> void:
	var img := Image.create_from_data(RASTER_W, RASTER_H, false, Image.FORMAT_RGBA8, pixels)
	if image_tex == null:
		image_tex = ImageTexture.create_from_image(img)
	else:
		image_tex.update(img)
	display.texture = image_tex

func _brightness(pixels: PackedByteArray, p: Vector2) -> float:
	var o := (int(p.y) * RASTER_W + int(p.x)) * 4
	return (float(pixels[o]) + float(pixels[o + 1]) + float(pixels[o + 2])) / 765.0

func _proj(p: Vector3, vp: PackedFloat32Array) -> Array:
	var cx: float = vp[0] * p.x + vp[4] * p.y + vp[8] * p.z + vp[12]
	var cy: float = vp[1] * p.x + vp[5] * p.y + vp[9] * p.z + vp[13]
	var cw: float = vp[3] * p.x + vp[7] * p.y + vp[11] * p.z + vp[15]
	if cw <= 0.0:
		return []
	var nx := cx / cw
	var ny := cy / cw
	var px := (nx * 0.5 + 0.5) * RASTER_W
	var py_a := (ny * 0.5 + 0.5) * RASTER_H
	var py_b := (0.5 - ny * 0.5) * RASTER_H
	return [Vector2(px, py_a), Vector2(px, py_b)]

func _anchor_px(pixels: PackedByteArray, vp: PackedFloat32Array, center: Vector3, thresh: float) -> Vector2:
	var cands := _proj(center, vp)
	if cands.is_empty():
		return Vector2(-1, -1)
	var best := Vector2(-1, -1)
	var best_b := thresh
	for cand in cands:
		var p: Vector2 = cand
		if p.x < 4 or p.y < 4 or p.x >= RASTER_W - 4 or p.y >= RASTER_H - 4:
			continue
		var b := _brightness(pixels, p)
		if b > best_b:
			best_b = b
			best = p
	return best

func _window_uniform(pixels: PackedByteArray, c: Vector2, r: int, tol: float) -> bool:
	var ref := _brightness(pixels, c) * 255.0
	for dy in range(-r, r + 1):
		for dx in range(-r, r + 1):
			var b := _brightness(pixels, c + Vector2(dx, dy)) * 255.0
			if absf(b - ref) > tol:
				return false
	return true

func _window_mean(pixels: PackedByteArray, c: Vector2, r: int) -> float:
	var s := 0.0
	var n := 0
	for dy in range(-r, r + 1):
		for dx in range(-r, r + 1):
			s += _brightness(pixels, c + Vector2(dx, dy))
			n += 1
	return s / float(n)

func _colored_count(pixels: PackedByteArray, thresh: float) -> int:
	var n := 0
	var count := RASTER_W * RASTER_H
	for i in count:
		var o := i * 4
		var b := (float(pixels[o]) + float(pixels[o + 1]) + float(pixels[o + 2])) / 765.0
		if b > thresh:
			n += 1
	return n

func _changed_count(a: PackedByteArray, b: PackedByteArray, tol: float) -> int:
	var n := 0
	var count := RASTER_W * RASTER_H
	for i in count:
		var o := i * 4
		var ba := (float(a[o]) + float(a[o + 1]) + float(a[o + 2])) / 765.0
		var bb := (float(b[o]) + float(b[o + 1]) + float(b[o + 2])) / 765.0
		if absf(ba - bb) > tol:
			n += 1
	return n

func _emissive_count(pixels: PackedByteArray) -> int:
	var n := 0
	var count := RASTER_W * RASTER_H
	for i in count:
		var o := i * 4
		var r := float(pixels[o])
		var g := float(pixels[o + 1])
		var bl := float(pixels[o + 2])
		if r > 200.0 and r > g + 50.0 and r > bl + 50.0:
			n += 1
	return n

func _histogram(pixels: PackedByteArray) -> Array:
	var bins := PackedInt32Array()
	bins.resize(16)
	var mn := 1.0
	var mx := 0.0
	var count := RASTER_W * RASTER_H
	for i in count:
		var o := i * 4
		var b := (float(pixels[o]) + float(pixels[o + 1]) + float(pixels[o + 2])) / 765.0
		if b < mn:
			mn = b
		if b > mx:
			mx = b
		var bin := mini(15, int(b * 16.0))
		bins[bin] += 1
	var peak := 0
	for v in bins:
		peak = maxi(peak, v)
	return [0, mx - mn, peak]

func _negtest() -> void:
	# Off-gate manual run (--negtest): exercises rejection paths. Expected:
	# every call returns false/empty AND prints ERROR:. Never part of the gate.
	print("GNE 016 negtest: begin (ERROR: lines below are EXPECTED)")
	if server.gpu_material_create():
		print("GNE 016 negtest: UNEXPECTED create ok without mesh")
		get_tree().quit(91)
		return
	print("GNE 016 negtest: pre-mesh create rejected OK")
	if not server.gpu_scene_create(1, 1.0):
		print("GNE 016 negtest: scene create failed")
		get_tree().quit(92)
		return
	if not server.gpu_mesh_create():
		print("GNE 016 negtest: mesh create failed")
		get_tree().quit(92)
		return
	if not server.gpu_material_create():
		print("GNE 016 negtest: material create failed")
		get_tree().quit(92)
		return
	if server.gpu_material_create():
		print("GNE 016 negtest: UNEXPECTED double create ok")
		get_tree().quit(93)
		return
	print("GNE 016 negtest: double create rejected OK")
	var bad := 0
	if server.gpu_material_set_albedo(99, Color.RED):
		bad += 1
	if server.gpu_material_set_params(0, 2.0, 0.0):
		bad += 1
	if server.gpu_material_set_specular(0, Color.WHITE, 0.0):
		bad += 1
	if not server.gpu_material_readback(99).is_empty():
		bad += 1
	if server.gpu_material_set_light(Vector3.ZERO):
		bad += 1
	if server.gpu_material_draw():
		bad += 1
	if bad != 0:
		print("GNE 016 negtest: FAIL rejections=" + str(bad) + "/6 passed")
		get_tree().quit(94)
		return
	print("GNE 016 negtest: all 6 rejections OK")
	print("GNE 016 negtest: PASS")
	get_tree().quit(0)

func _fail(code: int, msg: String) -> void:
	print("GNE 016: FAIL code=", code, " ", msg)
	get_tree().quit(code)
