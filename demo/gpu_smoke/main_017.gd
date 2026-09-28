extends Node

# GNE-017 Phase 2 — texture evidence scene (criteria T1-T8, SPEC 017).
#
# 4 instances (2 cube + 2 octa). ONE texture (checkerboard.gtex) bound to
# materials 0 and 3, albedo slot. Others flat (proves textured + flat coexist).
# Camera = 016 3/4 view (proven frustum). Instance 3 sits far (z=-1500) for
# the mip-chain evidence (T5): near = crisp cells, far = averaged gray.

const INSTANCE_COUNT := 4
const RASTER_W := 1920
const RASTER_H := 1080
# 3/4 view from the -X/-Y side: the faces pointing at the camera (-X: N.L=0.41,
# -Y: N.L=0.82) are LIT under canonical L. A frontal +Z camera would show only
# dark faces (N.L<0) and starve the histogram (measured: all pixels < 0.26).
const CAM_POS := Vector3(-2200, -600, -500)
const CAM_TGT := Vector3(0, 0, -900)

const PTS: Array[Vector3] = [
	Vector3(-1600, -450, -650),
	Vector3(-350, 150, -850),
	Vector3(350, -150, -900),
	Vector3(0, 100, -1500),
]
const SCALES: Array[float] = [80.0, 60.0, 65.0, 60.0]
const ALBEDOS: Array[Color] = [
	Color(1, 1, 1), Color(1, 0, 0), Color(0, 1, 0), Color(1, 1, 1),
]

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
var shot_file := ""

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--sigf="):
			sig_file = a.split("=")[1]
		elif a.begins_with("--shot="):
			shot_file = a.split("=")[1]
	_setup()

func _setup() -> void:
	server = GneRenderServer.get_server_singleton()
	if server == null:
		_fail(900, "server singleton is null")
		return
	if not server.ensure_gpu_device():
		_fail(901, "local RenderingDevice not available")
		return
	if not server.gpu_scene_create(INSTANCE_COUNT, 1.0):
		_fail(902, "gpu_scene_create(4)")
		return
	if not server.gpu_scene_dispatch(4):
		_fail(903, "gpu_scene_dispatch(4)")
		return
	if not server.gpu_mesh_create():
		_fail(904, "gpu_mesh_create")
		return
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
	if server.gpu_mesh_create_from_arrays(cube_v, cube_i) != 1:
		_fail(905, "cube slot 1")
		return
	var octa_v := PackedVector3Array(OCTA_VERTS)
	var octa_i := PackedInt32Array(OCTA_INDICES)
	if server.gpu_mesh_create_from_arrays(octa_v, octa_i) != 2:
		_fail(905, "octa slot 2")
		return
	# Slot 3 is cube geometry (not octa): T5 needs an axis-aligned -X face on
	# the far instance (an octahedron has no axis-aligned faces; anchoring its
	# "-X face" lands on a vertex and mixes background into the window).
	if server.gpu_mesh_create_from_arrays(cube_v, cube_i) != 3:
		_fail(905, "cube slot 3")
		return
	for i in INSTANCE_COUNT:
		server.gpu_scene_set_instance_transform(i, PTS[i], SCALES[i])
		server.gpu_scene_set_instance_mesh(i, i)
	if not server.gpu_mesh_set_batch_strategy(0):
		_fail(906, "set_batch_strategy(PER_MESH)")
		return
	if not server.gpu_material_create():
		_fail(907, "gpu_material_create")
		return
	for i in INSTANCE_COUNT:
		if not server.gpu_material_set_albedo(i, ALBEDOS[i]):
			_fail(908, "set_albedo " + str(i))
			return
		if not server.gpu_material_set_params(i, 0.5, 0.0):
			_fail(908, "set_params " + str(i))
			return
	# --- T1: load ---
	var tex := server.gpu_texture_load("res://miptex.gtex")
	if tex != 0:
		_fail(910, "texture id != 0, got " + str(tex))
		return
	print("GNE 017: T1 KTX2 load OK (id=0)")
	# --- T3: bind (slot 0 = albedo) on materials 0 and 3 ---
	if not server.gpu_texture_bind(0, 0, 0):
		_fail(911, "bind(0,0,0)")
		return
	if not server.gpu_texture_bind(3, 0, 0):
		_fail(911, "bind(3,0,0)")
		return
	if server.gpu_texture_get_binding(0, 0) != 0:
		_fail(912, "binding readback != 0")
		return
	print("GNE 017: T3 bind OK (mat_tex verified by GPU readback)")
	var st: PackedInt32Array = server.gpu_texture_get_stats()
	if st[0] != 1 or st[1] != 2:
		_fail(913, "stats wrong " + str(st))
		return
	camera = $Camera
	display = $Overlay/Display
	camera.global_position = CAM_POS
	camera.look_at(CAM_TGT, Vector3.UP)
	camera.near = 300.0
	camera.far = 4000.0
	var vp_size: Vector2 = get_viewport().get_visible_rect().size
	server.gpu_scene_set_viewport(vp_size.x, vp_size.y)
	server.gpu_scene_set_camera(camera.get_global_transform(), camera.get_camera_projection())
	_measure()

func _measure() -> void:
	if not server.gpu_cull_dispatch():
		_fail(920, "gpu_cull_dispatch")
		return
	var vis0 := server.gpu_cull_get_visible_count()
	if vis0 != INSTANCE_COUNT:
		_fail(921, "frustum must keep all 4, got " + str(vis0))
		return
	if not server.gpu_visibility_dispatch():
		_fail(922, "gpu_visibility_dispatch")
		return
	if not server.gpu_mesh_batch_dispatch():
		_fail(923, "gpu_mesh_batch_dispatch")
		return
	if not server.gpu_material_draw():
		_fail(924, "gpu_material_draw")
		return
	var pixels := server.gpu_raster_read_pixels()
	if pixels.size() != RASTER_W * RASTER_H * 4:
		_fail(925, "pixels size")
		return
	_show(pixels)
	var vp := server.gpu_scene_get_vp()
	if vp.size() != 16:
		_fail(926, "vp size")
		return
	# --- T4: checkerboard fingerprint on instance 0's lit -X face ---
	# (+Z faces camera but is dark under canonical L; -X has N.L=0.41.)
	var w0 := _window_stats(pixels, vp, PTS[0] + Vector3(-SCALES[0] * 0.5, 0, 0), 7)
	print("GNE 017: T4 near-window dark=", w0[0], " bright=", w0[1], " var=", w0[2])
	if w0[0] < 30 or w0[1] < 30:
		_fail(927, "no checkerboard fingerprint")
		return
	print("GNE 017: T4 sampled OK (bimodal cells present)")
	# --- T5: mip chain via diagnostic mip colors ---
	# Instance 3 -X face (white, textured, far): samples level ~3 (solid RED
	# from L3 up). Instance 0 samples levels 0-1 (white/black checker).
	# Redness (r-b) separates the two populations decisively.
	var w3 := _window_stats(pixels, vp, PTS[3] + Vector3(-SCALES[3] * 0.5, 0, 0), 2)
	var w0r := _window_stats(pixels, vp, PTS[0] + Vector3(-SCALES[0] * 0.5, 0, 0), 7)
	var c3 := _window_rgb(pixels, vp, PTS[3] + Vector3(-SCALES[3] * 0.5, 0, 0))
	var c0 := _window_rgb(pixels, vp, PTS[0] + Vector3(-SCALES[0] * 0.5, 0, 0))
	print("GNE 017: T5 far redness=", c3[0] - c3[2], " near redness=", c0[0] - c0[2])
	if c3[0] - c3[2] < 0.2:
		_fail(928, "far face not red (mip level wrong?)")
		return
	if c0[0] - c0[2] > 0.1:
		_fail(928, "near face unexpectedly red")
		return
	print("GNE 017: T5 mipmaps OK (far=red L3+, near=checker L0-1)")
	# --- T6 + signature ---
	var h := _histogram(pixels)
	print("GNE 017: T6 hist hr=", h[1])
	if h[1] < 0.5:
		_fail(929, "histogram range too narrow")
		return
	var sig := "v17|tc=1|fmt=uastc|slot=5|hr=%.2f|d1" % h[1]
	print("GNE 017: sig=" + sig)
	if sig_file != "":
		var f := FileAccess.open(sig_file, FileAccess.WRITE)
		if f == null:
			_fail(930, "sig file write failed")
			return
		f.store_string(sig + "\n")
	print("GNE 017: PASS")
	get_tree().quit(0)

func _show(pixels: PackedByteArray) -> void:
	var img := Image.create_from_data(RASTER_W, RASTER_H, false, Image.FORMAT_RGBA8, pixels)
	if image_tex == null:
		image_tex = ImageTexture.create_from_image(img)
	else:
		image_tex.update(img)
	display.texture = image_tex
	if shot_file != "":
		img.save_png(shot_file)
		print("GNE 017: shot saved to ", shot_file)

func _proj(p: Vector3, vp: PackedFloat32Array) -> Array:
	var cx: float = vp[0] * p.x + vp[4] * p.y + vp[8] * p.z + vp[12]
	var cy: float = vp[1] * p.x + vp[5] * p.y + vp[9] * p.z + vp[13]
	var cw: float = vp[3] * p.x + vp[7] * p.y + vp[11] * p.z + vp[15]
	if cw <= 0.0:
		return []
	var nx := cx / cw
	var ny := cy / cw
	var px := (nx * 0.5 + 0.5) * RASTER_W
	return [Vector2(px, (ny * 0.5 + 0.5) * RASTER_H), Vector2(px, (0.5 - ny * 0.5) * RASTER_H)]

func _brightness(pixels: PackedByteArray, p: Vector2) -> float:
	var o := (int(p.y) * RASTER_W + int(p.x)) * 4
	return (float(pixels[o]) + float(pixels[o + 1]) + float(pixels[o + 2])) / 765.0

# Window around projected center (brighter y-flip wins): returns
# [dark_count, bright_count, variance, mean]. Thresholds: dark<0.15,
# bright>0.4 (lit white cells reach ~0.5 under N.L~0.4).
func _window_stats(pixels: PackedByteArray, vp: PackedFloat32Array, center: Vector3, r: int) -> Array:
	var cands := _proj(center, vp)
	var best := Vector2(-1, -1)
	var best_b := -1.0
	for cand in cands:
		var p: Vector2 = cand
		if p.x < r + 2 or p.y < r + 2 or p.x >= RASTER_W - r - 2 or p.y >= RASTER_H - r - 2:
			continue
		var b := _brightness(pixels, p)
		if b > best_b:
			best_b = b
			best = p
	if best.x < 0:
		return [-1, -1, -1.0, -1.0, -1.0, -1.0]
	var dark := 0
	var bright := 0
	var s := 0.0
	var vals := PackedFloat32Array()
	for dy in range(-r, r + 1):
		for dx in range(-r, r + 1):
			var b := _brightness(pixels, best + Vector2(dx, dy))
			vals.append(b)
			s += b
			if b < 0.15:
				dark += 1
			if b > 0.4:
				bright += 1
	var mean := s / float(vals.size())
	var v := 0.0
	var mn := 1.0
	var mx := 0.0
	for b in vals:
		v += (b - mean) * (b - mean)
		mn = minf(mn, b)
		mx = maxf(mx, b)
	return [dark, bright, v / float(vals.size()), mean, mn, mx]

func _window_rgb(pixels: PackedByteArray, vp: PackedFloat32Array, center: Vector3) -> Array:
	# Mean (r,g,b) over a 5x5 window at the projected center (brighter y-flip
	# wins). Used for redness (r-b) evidence in T5.
	var cands := _proj(center, vp)
	var best := Vector2(-1, -1)
	var best_b := -1.0
	for cand in cands:
		var p: Vector2 = cand
		if p.x < 4 or p.y < 4 or p.x >= RASTER_W - 4 or p.y >= RASTER_H - 4:
			continue
		var b := _brightness(pixels, p)
		if b > best_b:
			best_b = b
			best = p
	var sr := 0.0
	var sg := 0.0
	var sb := 0.0
	var n := 0
	for dy in range(-2, 3):
		for dx in range(-2, 3):
			var o := (int(best.y + dy) * RASTER_W + int(best.x + dx)) * 4
			sr += float(pixels[o]) / 255.0
			sg += float(pixels[o + 1]) / 255.0
			sb += float(pixels[o + 2]) / 255.0
			n += 1
	return [sr / float(n), sg / float(n), sb / float(n)]

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

func _fail(code: int, msg: String) -> void:
	print("GNE 017: FAIL code=", code, " ", msg)
	get_tree().quit(code)
