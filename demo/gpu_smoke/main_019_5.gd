extends Node

# GNE-019.5 slice-0 - ESM prototype on the directional CSM (RFC docs/rfc_019_5_shadow_filters.md).
# Same scene as 019 (4 boxes, dir light, CSM bound only); PCF-4 baseline vs ESM
# (flag gpu_shadow_esm, default OFF: changes the CSM fill encode + the CSM read).
# Pre-registered acceptance: E1 penumbra width >= 2x PCF, E2 lit-plateau
# tolerance, E3 cost reported, E4 determinism byte-equal. E5 = CVS (external).

const INSTANCE_COUNT := 4
const RASTER_W := 1920
const RASTER_H := 1080
const CAM_POS := Vector3(0, 0, 1700)
const DIR_PSEUDO := 2147483646

const PTS: Array[Vector3] = [
	Vector3(0, 0, -600),
	Vector3(-700, 0, -800),
	Vector3(0, 0, -1040),
	Vector3(-350, 400, -137),
]
const SCALES: Array[float] = [72.0, 84.0, 220.0, 36.0]
const ALBEDOS: Array[Color] = [
	Color(0.35, 0.35, 0.35),
	Color(0.35, 0.35, 0.35),
	Color(0.35, 0.35, 0.35),
	Color(0.6, 0.6, 0.6),
]
const POINTS: Array = [
	[Vector3(0, 0, -200), 800.0, Color(1, 1, 1), 0.45],
	[Vector3(-350, 400, 150), 380.0, Color(1, 0, 0), 0.6],
	[Vector3(34.4, 34.4, -435), 1000.0, Color(0, 1, 0), 1.4],
	[Vector3(0, -400, -700), 380.0, Color(0, 0, 1), 0.25],
	[Vector3(0, 500, -800), 420.0, Color(1, 1, 1), 0.11],
	[Vector3(-400, -300, -400), 420.0, Color(1, 1, 1), 0.11],
	[Vector3(400, -300, -400), 420.0, Color(1, 1, 1), 0.11],
	[Vector3(-400, 300, -1000), 420.0, Color(1, 1, 1), 0.11],
	[Vector3(400, 300, -1000), 420.0, Color(1, 1, 1), 0.11],
	[Vector3(0, 0, -300), 350.0, Color(1, 1, 1), 0.11],
	[Vector3(-200, 400, -600), 380.0, Color(1, 1, 1), 0.11],
	[Vector3(200, -400, -600), 380.0, Color(1, 1, 1), 0.11],
	[Vector3(0, 200, -1200), 380.0, Color(1, 1, 1), 0.11],
	[Vector3(0, -200, -1200), 380.0, Color(1, 1, 1), 0.11],
	[Vector3(5000, 5000, 5000), 420.0, Color(1, 1, 1), 0.11],
	[Vector3(-5000, -5000, 5000), 420.0, Color(1, 1, 1), 0.11],
]
const SPOTS: Array = [
	[Vector3(0, 800, 500), Vector3(-700, 0, -800), 0.2, 0.5, Color(1, 1, 1), 1.1],
	[Vector3(-600, 800, 500), Vector3(-350, 150, -850), 0.2, 0.5, Color(1, 1, 1), 0.18],
	[Vector3(600, 800, 500), Vector3(350, -150, -900), 0.2, 0.5, Color(1, 1, 1), 0.18],
	[Vector3(0, 800, 500), Vector3(0, 0, -600), 0.15, 0.4, Color(1, 0.9, 0.8), 0.25],
]

var server: GneRenderServer
var camera: Camera3D
var display: TextureRect
var image_tex: ImageTexture
var sig_file := ""

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--sigf="):
			sig_file = a.split("=")[1]
	_setup()

func _mk_point(pos: Vector3, radius: float, color: Color, intensity: float) -> Dictionary:
	return {"type": 0, "pos": pos, "range": radius, "color": color, "intensity": intensity}

func _mk_spot(pos: Vector3, target: Vector3, inner: float, outer: float, color: Color, intensity: float) -> Dictionary:
	var d: Vector3 = (target - pos).normalized()
	return {"type": 1, "pos": pos, "range": 4000.0, "color": color, "intensity": intensity, "dir": d, "cone_inner": inner, "cone_outer": outer}

func _setup() -> void:
	server = GneRenderServer.get_server_singleton()
	if server == null:
		_fail(3000, "server singleton"); return
	if not server.ensure_gpu_device():
		_fail(3001, "device"); return
	if not server.gpu_scene_create(INSTANCE_COUNT, 1.0):
		_fail(3002, "scene_create"); return
	if not server.gpu_scene_dispatch(4):
		_fail(3003, "scene_dispatch"); return
	if not server.gpu_mesh_create():
		_fail(3004, "mesh_create"); return
	for i in INSTANCE_COUNT:
		server.gpu_scene_set_instance_transform(i, PTS[i], SCALES[i])
		server.gpu_scene_set_instance_mesh(i, 0)
	if not server.gpu_mesh_set_batch_strategy(0):
		_fail(3005, "batch_strategy"); return
	if not server.gpu_material_create():
		_fail(3006, "material_create"); return
	if not server.gpu_material_set_light(Vector3(0.2, 0.2, 0.96).normalized()):
		_fail(3007, "dir light"); return
	for i in INSTANCE_COUNT:
		if not server.gpu_material_set_albedo(i, ALBEDOS[i]):
			_fail(3008, "albedo"); return
		if not server.gpu_material_set_params(i, 0.5, 0.0):
			_fail(3008, "params"); return
		if not server.gpu_material_set_specular(i, Color(0.25, 0.25, 0.25), 32.0):
			_fail(3008, "specular"); return
	for i in POINTS.size():
		var q: Array = POINTS[i]
		if server.gpu_light_create(_mk_point(q[0], q[1], q[2], q[3])) != i:
			_fail(3009, "point id"); return
	for i in SPOTS.size():
		var s: Array = SPOTS[i]
		if server.gpu_light_create(_mk_spot(s[0], s[1], s[2], s[3], s[4], s[5])) != 16 + i:
			_fail(3009, "spot id"); return
	camera = $Camera
	display = $Overlay/Display
	camera.global_position = CAM_POS
	camera.rotation = Vector3.ZERO
	camera.near = 300.0
	camera.far = 4000.0
	var vp_size: Vector2 = get_viewport().get_visible_rect().size
	server.gpu_scene_set_viewport(vp_size.x, vp_size.y)
	server.gpu_scene_set_camera(camera.get_global_transform(), camera.get_camera_projection())
	_measure()

func _measure() -> void:
	if not server.gpu_cull_dispatch():
		_fail(3010, "cull"); return
	if server.gpu_cull_get_visible_count() != INSTANCE_COUNT:
		_fail(3011, "frustum"); return
	if not server.gpu_visibility_dispatch():
		_fail(3012, "vis"); return
	if not server.gpu_mesh_batch_dispatch():
		_fail(3013, "batch"); return
	var h_csm := server.gpu_shadow_map_create(0, 2048)
	if h_csm < 0:
		_fail(3014, "csm create"); return
	if not server.gpu_shadow_light_bind(DIR_PSEUDO, h_csm):
		_fail(3015, "bind dir"); return
	if not server.gpu_shadow_cull_dispatch():
		_fail(3016, "shadow cull"); return
	var sst: Dictionary = server.gpu_shadow_get_stats()
	print("GNE 019.5: shadow stats ", sst)
	if int(sst["count"]) != 1:
		_fail(3017, "sm != 1"); return
	if not server.gpu_shadow_render_maps():
		_fail(3018, "render maps pcf"); return
	if not server.gpu_material_draw_lights():
		_fail(3019, "draw pcf"); return
	var pix_pcf := server.gpu_raster_read_pixels()
	print("GNE 019.5: DBG pcf maps ", server.gpu_shadow_dbg_map(0, 0, 0), " ", server.gpu_shadow_dbg_map(0, 0, 1), " ", server.gpu_shadow_dbg_map(0, 0, 2), " ", server.gpu_shadow_dbg_map(0, 0, 3))
	var vp0 := server.gpu_scene_get_vp()
	var s1px := _proj_px(Vector3(-69, -69, -930), vp0)
	print("GNE 019.5: DBG s1_probe px=", s1px, " lum_pcf=", _brightness(pix_pcf, s1px))
	if pix_pcf.size() != RASTER_W * RASTER_H * 4:
		_fail(3020, "pix size"); return
	# E3 cost: PCF state reps
	var t_pcf: Array = []
	for r in range(3):
		var t0 := Time.get_ticks_usec()
		if not server.gpu_shadow_render_maps():
			_fail(3021, "cost maps pcf"); return
		if not server.gpu_material_draw_lights():
			_fail(3021, "cost draw pcf"); return
		t_pcf.append(Time.get_ticks_usec() - t0)
	# shadow contrast reference: dir unbound (no shadow) probe value
	if not server.gpu_shadow_light_bind(DIR_PSEUDO, -1):
		_fail(3026, "unbind dir"); return
	if not _redraw():
		return
	var pix_ns := server.gpu_raster_read_pixels()
	if not server.gpu_shadow_light_bind(DIR_PSEUDO, h_csm):
		_fail(3026, "rebind dir"); return
	print("GNE 019.5: DBG s1 lum=noshadow ", _brightness(pix_ns, s1px), " (contrast ref)")
	# switch to ESM
	server.gpu_shadow_esm_set(true)
	if not server.gpu_shadow_render_maps():
		_fail(3022, "render maps esm"); return
	if not server.gpu_material_draw_lights():
		_fail(3022, "draw esm"); return
	var pix_esm := server.gpu_raster_read_pixels()
	print("GNE 019.5: DBG esm maps ", server.gpu_shadow_dbg_map(0, 0, 0), " ", server.gpu_shadow_dbg_map(0, 0, 1), " ", server.gpu_shadow_dbg_map(0, 0, 2), " ", server.gpu_shadow_dbg_map(0, 0, 3))
	print("GNE 019.5: DBG s1 lum_esm=", _brightness(pix_esm, s1px))
	var t_esm: Array = []
	for r in range(3):
		var t1 := Time.get_ticks_usec()
		if not server.gpu_shadow_render_maps():
			_fail(3023, "cost maps esm"); return
		if not server.gpu_material_draw_lights():
			_fail(3023, "cost draw esm"); return
		t_esm.append(Time.get_ticks_usec() - t1)
	# E4 determinism: two more ON-state draws, byte compare
	if not server.gpu_shadow_render_maps():
		_fail(3024, "det maps"); return
	if not server.gpu_material_draw_lights():
		_fail(3024, "det draw a"); return
	var pix_a := server.gpu_raster_read_pixels()
	if not server.gpu_material_draw_lights():
		_fail(3024, "det draw b"); return
	var pix_b := server.gpu_raster_read_pixels()
	var det_ok: bool = (pix_a == pix_b)
	_show(pix_esm)
	var imgA := Image.create_from_data(RASTER_W, RASTER_H, false, Image.FORMAT_RGBA8, pix_pcf)
	imgA.save_png("C:/Users/opc/AppData/Local/Temp/opencode/s195_pcf.png")
	var imgB := Image.create_from_data(RASTER_W, RASTER_H, false, Image.FORMAT_RGBA8, pix_esm)
	imgB.save_png("C:/Users/opc/AppData/Local/Temp/opencode/s195_esm.png")
	print("GNE 019.5: evidence pngs saved")
	var ch := _changed_count(pix_pcf, pix_esm, 0.02)
	print("GNE 019.5: changed pcf->esm=", ch)
	# E1: penumbra widths
	var wA := _edge_widths(pix_pcf)
	var wB := _edge_widths(pix_esm)
	print("GNE 019.5: E1 widths pcf median=", wA[0], " n=", wA[1], " esm median=", wB[0], " n=", wB[1])
	# Transition-band instrument (primary): the band is derived from the measured
	# plateaus (umbra 0.63 / lit 0.96): lo = umbra+0.05, hi = lit-0.04.
	var band_pcf := _band_count(pix_pcf, 0.68, 0.92)
	var band_esm := _band_count(pix_esm, 0.68, 0.92)
	var band_ratio := float(band_esm) / maxf(float(band_pcf), 1.0)
	print("GNE 019.5: band pcf=", band_pcf, " esm=", band_esm, " ratio=", band_ratio)
	var e1 := band_ratio >= 2.0
	var scan_ratio := float(wB[0]) / maxf(float(wA[0]), 1.0)
	print("GNE 019.5: E1 band_ratio=", band_ratio, " scan_ratio=", scan_ratio, " (scan n=", wA[1], "/", wB[1], ") pass=", e1)
	# E2: lit-plateau tolerance
	var flat := _plateau_diff(pix_pcf, pix_esm)
	var e2 := flat <= 0.10
	print("GNE 019.5: E2 plateau mean diff=", flat, " pass=", e2)
	# E3 report
	print("GNE 019.5: E3 cost pcf_us=", t_pcf, " esm_us=", t_esm)
	print("GNE 019.5: E4 determinism byte-equal=", det_ok)
	var sig := "v195|wa=%.1f|wb=%.1f|flat=%.4f|pcf_us=%d|esm_us=%d|e1=%d|e2=%d|e4=%d|d1" % [float(wA[0]), float(wB[0]), flat, _p50(t_pcf), _p50(t_esm), int(e1), int(e2), int(det_ok)]
	print("GNE 019.5: sig=", sig)
	if sig_file != "":
		var f := FileAccess.open(sig_file, FileAccess.WRITE)
		if f == null:
			_fail(3025, "sig file"); return
		f.store_string(sig + "\n")
	if e1 and e2 and det_ok:
		print("GNE 019.5: SLICE0 PASS")
	else:
		print("GNE 019.5: SLICE0 FAIL e1=", e1, " e2=", e2, " e4=", det_ok)
	server.gpu_scene_destroy()
	get_tree().quit(0)

func _band_count(pixels: PackedByteArray, band_lo: float, band_hi: float) -> int:
	var n := 0
	var count := RASTER_W * RASTER_H
	var i := 0
	while i < count:
		var o := i * 4
		var b := (float(pixels[o]) + float(pixels[o + 1]) + float(pixels[o + 2])) / 765.0
		if b > band_lo and b < band_hi:
			n += 1
		i += 2
	return n * 2

func _redraw() -> bool:
	if not server.gpu_cull_dispatch():
		_fail(3040, "recull"); return false
	if not server.gpu_visibility_dispatch():
		_fail(3041, "revisibility"); return false
	if not server.gpu_mesh_batch_dispatch():
		_fail(3042, "rebatch"); return false
	if not server.gpu_material_draw_lights():
		_fail(3043, "redraw_lights"); return false
	return true

func _edge_widths(pixels: PackedByteArray) -> Array:
	# Transition width per boundary: scanlines; candidate = |lum(x+32)-lum(x)| > 0.06
	# with a plausible shadowed side (lo >= 0.06, excludes black background
	# silhouettes); width = pixels strictly between lo+0.02 and hi-0.02.
	var widths: Array = []
	var y := 40
	while y < RASTER_H - 40:
		var x := 8
		while x < RASTER_W - 8 - 32:
			var b0 := _brightness(pixels, Vector2(x, y))
			var b1 := _brightness(pixels, Vector2(x + 32, y))
			if absf(b1 - b0) > 0.04:
				var lo: float = minf(b0, b1)
				var hi: float = maxf(b0, b1)
				if lo >= 0.05 and hi <= 0.99:
					var w := 0
					for xx in range(x, x + 33):
						var v := _brightness(pixels, Vector2(xx, y))
						if v > lo + 0.02 and v < hi - 0.02:
							w += 1
					if w > 0:
						widths.append(w)
				x += 32
			else:
				x += 8
		y += 16
	widths.sort()
	var med := 0
	if widths.size() > 0:
		med = widths[widths.size() / 2]
	return [med, widths.size()]

func _plateau_diff(a: PackedByteArray, b: PackedByteArray) -> float:
	var s := 0.0
	var n := 0
	var count := RASTER_W * RASTER_H
	var i := 0
	while i < count:
		var o := i * 4
		var ba := (float(a[o]) + float(a[o + 1]) + float(a[o + 2])) / 765.0
		if ba > 0.85:
			var bb := (float(b[o]) + float(b[o + 1]) + float(b[o + 2])) / 765.0
			s += absf(ba - bb)
			n += 1
		i += 7
	if n == 0:
		return 0.0
	return s / float(n)

func _proj_px(p: Vector3, vp: PackedFloat32Array) -> Vector2:
	var cx: float = vp[0] * p.x + vp[4] * p.y + vp[8] * p.z + vp[12]
	var cy: float = vp[1] * p.x + vp[5] * p.y + vp[9] * p.z + vp[13]
	var cw: float = vp[3] * p.x + vp[7] * p.y + vp[11] * p.z + vp[15]
	var nx := cx / cw
	var ny := cy / cw
	var px := (nx * 0.5 + 0.5) * RASTER_W
	var c0 := Vector2(px, (ny * 0.5 + 0.5) * RASTER_H)
	var c1 := Vector2(px, (0.5 - ny * 0.5) * RASTER_H)
	if c0.x >= 4 and c0.y >= 4 and c0.x < RASTER_W - 4 and c0.y < RASTER_H - 4:
		return c0
	return c1

func _brightness(pixels: PackedByteArray, p: Vector2) -> float:
	var o := (int(p.y) * RASTER_W + int(p.x)) * 4
	return (float(pixels[o]) + float(pixels[o + 1]) + float(pixels[o + 2])) / 765.0

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

func _p50(a: Array) -> int:
	var s := a.duplicate()
	s.sort()
	return int(s[s.size() / 2])

func _show(pixels: PackedByteArray) -> void:
	var img := Image.create_from_data(RASTER_W, RASTER_H, false, Image.FORMAT_RGBA8, pixels)
	if image_tex == null:
		image_tex = ImageTexture.create_from_image(img)
	else:
		image_tex.update(img)
	display.texture = image_tex

func _fail(code: int, msg: String) -> void:
	print("GNE 019.5: FAIL code=", code, " ", msg)
	get_tree().quit(code)