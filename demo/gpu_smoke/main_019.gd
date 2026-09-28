extends Node

# GNE-019 — Shadows + Real Depth HZB closure scene (criteria S1-S8, SPEC 019 FINAL/D7).
#
# 4 boxes (019 layout: inst3 floats mid-air on the spot0->inst1 ray as the S3
# caster; inst0 casts S1-dir + S2-green shadows onto inst2's big face).
# 20 lights (018 photometry, recalibrated albedos) + 5 shadow binds:
# dir->CSM, green->cube0, red->cube1, spot0->spot0, spot3->spot1 (sm=5).
# Casters = all 4 instances (cs=4). rd=1 via gpu_hzb_depth_feed proof.
# S8 (regressions incl. 012-rev green) runs at harness level (gt_019a + gt_regress).

const INSTANCE_COUNT := 4
const RASTER_W := 1920
const RASTER_H := 1080
const CAM_POS := Vector3(0, 0, 1700)
const DIR_PSEUDO := 2147483646 # GNE_SHADOW_DIR_LIGHT

const PTS: Array[Vector3] = [
	Vector3(0, 0, -600),
	Vector3(-700, 0, -800),
	Vector3(0, 0, -1040),
	Vector3(-350, 400, -137),
]
const SCALES: Array[float] = [72.0, 84.0, 220.0, 36.0] # inst0/1/2 sized so the S1/S2/S3 umbras pass their >=500 px gates
const ALBEDOS: Array[Color] = [
	Color(0.35, 0.35, 0.35),
	Color(0.35, 0.35, 0.35),
	Color(0.35, 0.35, 0.35),
	Color(0.6, 0.6, 0.6),
]

const POINTS: Array = [
	[Vector3(0, 0, -200), 800.0, Color(1, 1, 1), 0.45],
	[Vector3(-350, 400, 150), 380.0, Color(1, 0, 0), 0.6],
	[Vector3(34.4, 34.4, -435), 1000.0, Color(0, 1, 0), 1.4], # green: repositioned so its umbra of inst0 lands at the dir umbra center (S2 probe dark base)
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
# S1/S2/S3 umbra probe world points (computed in §36).
const S1_PROBE := Vector3(-69, -69, -930)
const S2_PROBE := Vector3(-60, -80, -930)

var server: GneRenderServer
var camera: Camera3D
var display: TextureRect
var image_tex: ImageTexture
var sig_file := ""
var shot_file := ""
var h_csm := -1
var h_cube0 := -1
var h_cube1 := -1
var h_spot0 := -1
var h_spot1 := -1

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--sigf="):
			sig_file = a.split("=")[1]
		elif a.begins_with("--shot="):
			shot_file = a.split("=")[1]
	_setup()

func _mk_point(pos: Vector3, radius: float, color: Color, intensity: float) -> Dictionary:
	return {"type": 0, "pos": pos, "range": radius, "color": color, "intensity": intensity}

func _mk_spot(pos: Vector3, target: Vector3, inner: float, outer: float, color: Color, intensity: float) -> Dictionary:
	var d: Vector3 = (target - pos).normalized()
	return {"type": 1, "pos": pos, "range": 4000.0, "color": color, "intensity": intensity, "dir": d, "cone_inner": inner, "cone_outer": outer}

func _setup() -> void:
	server = GneRenderServer.get_server_singleton()
	if server == null:
		_fail(2000, "server singleton is null")
		return
	if not server.ensure_gpu_device():
		_fail(2001, "local RenderingDevice not available")
		return
	if not server.gpu_scene_create(INSTANCE_COUNT, 1.0):
		_fail(2003, "gpu_scene_create(4)")
		return
	if not server.gpu_scene_dispatch(4):
		_fail(2004, "gpu_scene_dispatch(4)")
		return
	if not server.gpu_mesh_create():
		_fail(2005, "gpu_mesh_create")
		return
	for i in INSTANCE_COUNT:
		server.gpu_scene_set_instance_transform(i, PTS[i], SCALES[i])
		server.gpu_scene_set_instance_mesh(i, 0)
	if not server.gpu_mesh_set_batch_strategy(0):
		_fail(2006, "set_batch_strategy(PER_MESH)")
		return
	if not server.gpu_material_create():
		_fail(2007, "gpu_material_create")
		return
	if not server.gpu_material_set_light(Vector3(0.2, 0.2, 0.96).normalized()):
		_fail(2002, "set 019 dir")
		return
	for i in INSTANCE_COUNT:
		if not server.gpu_material_set_albedo(i, ALBEDOS[i]):
			_fail(2008, "set_albedo " + str(i))
			return
		if not server.gpu_material_set_params(i, 0.5, 0.0):
			_fail(2008, "set_params " + str(i))
			return
		if not server.gpu_material_set_specular(i, Color(0.25, 0.25, 0.25), 32.0):
			_fail(2008, "set_specular " + str(i))
			return
	for i in POINTS.size():
		var q: Array = POINTS[i]
		if server.gpu_light_create(_mk_point(q[0], q[1], q[2], q[3])) != i:
			_fail(2009, "point id != " + str(i))
			return
	for i in SPOTS.size():
		var s: Array = SPOTS[i]
		if server.gpu_light_create(_mk_spot(s[0], s[1], s[2], s[3], s[4], s[5])) != 16 + i:
			_fail(2009, "spot id != " + str(16 + i))
			return
	camera = $Camera
	print("GNE 019: green light params range=", POINTS[2][1], " intensity=", POINTS[2][3], " pos=", POINTS[2][0])
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
		_fail(2010, "gpu_cull_dispatch")
		return
	if server.gpu_cull_get_visible_count() != INSTANCE_COUNT:
		_fail(2011, "frustum must keep all 4")
		return
	if not server.gpu_visibility_dispatch():
		_fail(2012, "gpu_visibility_dispatch")
		return
	if not server.gpu_mesh_batch_dispatch():
		_fail(2013, "gpu_mesh_batch_dispatch")
		return
	# --- Shadow store: 1 CSM + 2 cube + 2 spot, then 5 binds (sm=5) ---
	h_csm = server.gpu_shadow_map_create(0, 2048)
	h_cube0 = server.gpu_shadow_map_create(1, 1024)
	h_cube1 = server.gpu_shadow_map_create(1, 1024)
	h_spot0 = server.gpu_shadow_map_create(2, 2048)
	h_spot1 = server.gpu_shadow_map_create(2, 2048)
	if h_csm < 0 or h_cube0 < 0 or h_cube1 < 0 or h_spot0 < 0 or h_spot1 < 0:
		_fail(2014, "shadow map create")
		return
	if not server.gpu_shadow_light_bind(DIR_PSEUDO, h_csm):
		_fail(2015, "bind dir")
		return
	if not server.gpu_shadow_light_bind(2, h_cube0):
		_fail(2015, "bind green")
		return
	if not server.gpu_shadow_light_bind(1, h_cube1):
		_fail(2015, "bind red")
		return
	if not server.gpu_shadow_light_bind(16, h_spot0):
		_fail(2015, "bind spot0")
		return
	if not server.gpu_shadow_light_bind(19, h_spot1):
		_fail(2015, "bind spot3")
		return
	if not server.gpu_shadow_cull_dispatch():
		_fail(2016, "shadow cull")
		return
	var sst: Dictionary = server.gpu_shadow_get_stats()
	print("GNE 019: shadow stats ", sst)
	if int(sst["count"]) != 5:
		_fail(2017, "sm != 5, got " + str(sst))
		return
	if int(sst["casters"]) != 4:
		_fail(2017, "cs != 4, got " + str(sst))
		return
	if not server.gpu_shadow_render_maps():
		_fail(2018, "shadow render")
		return
	var dbg0: PackedInt32Array = server.gpu_shadow_dbg_map(0, 0, 0)
	var dbg1: PackedInt32Array = server.gpu_shadow_dbg_map(0, 0, 1)
	var dbg2: PackedInt32Array = server.gpu_shadow_dbg_map(0, 0, 2)
	var dbg3: PackedInt32Array = server.gpu_shadow_dbg_map(0, 0, 3)
	print("GNE 019: CSM maps ", dbg0, " ", dbg1, " ", dbg2, " ", dbg3)
	for lk in 4:
		print("GNE 019: dump layer ", lk, " ok=", server.gpu_shadow_dbg_dump(lk, "C:/Users/opc/AppData/Local/Temp/opencode/csm_layer_%d.bin" % lk))
	for cf in 6:
		print("GNE 019: cube0 f", cf, " ", server.gpu_shadow_dbg_map(1, 0, cf))
	var vp3: PackedFloat32Array = server.gpu_shadow_dbg_vp(0, 3)
	var vp2: PackedFloat32Array = server.gpu_shadow_dbg_vp(0, 2)
	print("GNE 019: VP2 ", vp2)
	print("GNE 019: VP3 ", vp3)
	if not server.gpu_material_draw_lights():
		_fail(2019, "gpu_material_draw_lights")
		return
	var pixels := server.gpu_raster_read_pixels()
	if pixels.size() != RASTER_W * RASTER_H * 4:
		_fail(2020, "pixels size")
		return
	_show(pixels)
	var vp := server.gpu_scene_get_vp()
	if vp.size() != 16:
		_fail(2021, "vp size")
		return
	# --- S1: directional (CSM) shadow ON vs OFF at the inst0 umbra on inst2 ---
	var p1 := _proj_px(S1_PROBE, vp)
	var b1 := _brightness(pixels, p1)
	if not server.gpu_shadow_light_bind(DIR_PSEUDO, -1):
		_fail(2022, "dir unbind")
		return
	if not _redraw():
		return
	var q1 := server.gpu_raster_read_pixels()
	if shot_file != "":
		var im_off := Image.create_from_data(RASTER_W, RASTER_H, false, Image.FORMAT_RGBA8, q1)
		var off_path := shot_file.get_basename() + "_off.png"
		im_off.save_png(off_path)
		print("GNE 019: off shot saved ", off_path)
	var b1off := _brightness(q1, p1)
	var ch1 := _changed_count(pixels, q1, 0.02)
	print("GNE 019: S1 dir probe ", b1, " -> ", b1off, " changed=", ch1)
	# DIAG-019: changed-pixel bbox + probe pixel (S1 geometry evidence).
	var dmin := Vector2(999999.0, 999999.0)
	var dmax := Vector2(-1.0, -1.0)
	var dcount := 0
	for yy in RASTER_H:
		for xx in RASTER_W:
			var oo := (yy * RASTER_W + xx) * 4
			var va := (float(pixels[oo]) + float(pixels[oo + 1]) + float(pixels[oo + 2])) / 765.0
			var vb := (float(q1[oo]) + float(q1[oo + 1]) + float(q1[oo + 2])) / 765.0
			if absf(va - vb) > 0.02:
				dcount += 1
				dmin.x = minf(dmin.x, float(xx))
				dmin.y = minf(dmin.y, float(yy))
				dmax.x = maxf(dmax.x, float(xx))
				dmax.y = maxf(dmax.y, float(yy))
	print("GNE 019: S1 changed bbox ", dmin, " .. ", dmax, " count=", dcount, " probe_px=", p1)
	if b1off - b1 < 0.15:
		_fail(2023, "dir shadow not visible")
		return
	if ch1 < 500:
		_fail(2023, "dir shadow too small")
		return
	if not server.gpu_shadow_light_bind(DIR_PSEUDO, h_csm):
		_fail(2022, "dir rebind")
		return
	print("GNE 019: S1 dir shadow OK")
	# --- S2: point (green cube) shadow ON vs OFF ---
	if not _redraw():
		return
	var pon := server.gpu_raster_read_pixels()
	var p2 := _proj_px(S2_PROBE, vp)
	var b2 := _brightness(pon, p2)
	if not server.gpu_shadow_light_bind(2, -1):
		_fail(2024, "green unbind")
		return
	if not _redraw():
		return
	var poff := server.gpu_raster_read_pixels()
	var b2off := _brightness(poff, p2)
	var ch2 := _changed_count(pon, poff, 0.02)
	print("GNE 019: S2 point probe ", b2, " -> ", b2off, " changed=", ch2)
	var s2min := Vector2(999999.0, 999999.0)
	var s2max := Vector2(-1.0, -1.0)
	var s2cnt := 0
	for yy2 in RASTER_H:
		for xx2 in RASTER_W:
			var oo2 := (yy2 * RASTER_W + xx2) * 4
			var va2 := (float(pon[oo2]) + float(pon[oo2 + 1]) + float(pon[oo2 + 2])) / 765.0
			var vb2 := (float(poff[oo2]) + float(poff[oo2 + 1]) + float(poff[oo2 + 2])) / 765.0
			if absf(va2 - vb2) > 0.02:
				s2cnt += 1
				s2min.x = minf(s2min.x, float(xx2))
				s2min.y = minf(s2min.y, float(yy2))
				s2max.x = maxf(s2max.x, float(xx2))
				s2max.y = maxf(s2max.y, float(yy2))
	print("GNE 019: S2 changed bbox ", s2min, " .. ", s2max, " count=", s2cnt, " probe_px=", p2)
	var o2 := (int(p2.y) * RASTER_W + int(p2.x)) * 4
	print("GNE 019: S2 probe rgb on=(", pon[o2], ",", pon[o2 + 1], ",", pon[o2 + 2], ") off=(", poff[o2], ",", poff[o2 + 1], ",", poff[o2 + 2], ")")
	print("GNE 019: S2 probe first_draw=", _brightness(pixels, p2), " s2_on=", b2, " s2_off=", b2off)
	print("GNE 019: S1 ok? b1 was ", b1, " at ", p1)
	if b2off - b2 < 0.15:
		_fail(2025, "point shadow not visible")
		return
	if ch2 < 500:
		_fail(2025, "point shadow too small")
		return
	if not server.gpu_shadow_light_bind(2, h_cube0):
		_fail(2024, "green rebind")
		return
	print("GNE 019: S2 point shadow OK")
	# --- S3: spot (spot0) shadow ON vs OFF at inst1 face center ---
	if not _redraw():
		return
	var son := server.gpu_raster_read_pixels()
	var p3 := _anchor_inst(vp, 1)
	var b3 := _window_mean(son, p3, 2)
	if not server.gpu_shadow_light_bind(16, -1):
		_fail(2026, "spot0 unbind")
		return
	if not _redraw():
		return
	var soff := server.gpu_raster_read_pixels()
	var b3off := _window_mean(soff, p3, 2)
	var ch3 := _changed_count(son, soff, 0.02)
	print("GNE 019: S3 spot probe ", b3, " -> ", b3off, " changed=", ch3)
	if b3off - b3 < 0.15:
		_fail(2027, "spot shadow not visible")
		return
	if ch3 < 500:
		_fail(2027, "spot shadow too small")
		return
	if not server.gpu_shadow_light_bind(16, h_spot0):
		_fail(2026, "spot0 rebind")
		return
	print("GNE 019: S3 spot shadow OK")
	# Re-run the shadow cull: rebinds reset the caster tally; the sig cs field
	# must read the scene total (4).
	if not server.gpu_shadow_cull_dispatch():
		_fail(2028, "S4 re-cull")
		return
	# --- S4: no cascade-seam steps on lit geometry + analytic coverage ---
	if not _redraw():
		return
	var fin := server.gpu_raster_read_pixels()
	_show(fin)
	var step := _lit_max_step(fin)
	var cov := _cascade_coverage()
	print("GNE 019: S4 lit max step=", step, " coverage=", cov)
	if step > 0.10:
		_fail(2028, "csm seam step too big")
		return
	print("GNE 019: S4 seam OK")
	# --- S5: KI-007 real-depth proof (rd=1) ---
	if not server.gpu_hzb_prod_create():
		_fail(2029, "hzb prod create")
		return
	if not server.gpu_hzb_depth_feed():
		_fail(2030, "depth feed")
		return
	var probe: PackedInt32Array = server.gpu_hzb_dbg_probe()
	print("GNE 019: S5 probe=", probe)
	if probe.size() != 4 or probe[0] <= 0:
		_fail(2031, "no real-depth proof (pcount=0)")
		return
	var st3: Dictionary = server.gpu_shadow_get_stats()
	if int(st3["rd"]) != 1:
		_fail(2031, "rd != 1, got " + str(st3))
		return
	print("GNE 019: S5 KI-007 real depth OK")
	# --- S6 + signature (canonical full state, all shadows bound) ---
	if not _redraw():
		return
	var last := server.gpu_raster_read_pixels()
	_show(last)
	var h := _histogram(last)
	print("GNE 019: S6 hr=", h[1])
	if h[1] < 0.3:
		_fail(2032, "histogram range too narrow")
		return
	var st4: Dictionary = server.gpu_shadow_get_stats()
	var sig := "v19|lc=%d|sm=%d|cs=%d|rd=%d|hr=%.2f|d1" % [20, int(st4["count"]), int(st4["casters"]), int(st4["rd"]), h[1]]
	print("GNE 019: sig=" + sig)
	if sig_file != "":
		var f := FileAccess.open(sig_file, FileAccess.WRITE)
		if f == null:
			_fail(2033, "sig file write failed")
			return
		f.store_string(sig + "\n")
	print("GNE 019: PASS")
	get_tree().quit(0)

func _redraw() -> bool:
	if not server.gpu_cull_dispatch():
		_fail(2040, "recull")
		return false
	if not server.gpu_visibility_dispatch():
		_fail(2041, "revisibility")
		return false
	if not server.gpu_mesh_batch_dispatch():
		_fail(2042, "rebatch")
		return false
	if not server.gpu_material_draw_lights():
		_fail(2043, "redraw_lights")
		return false
	return true

func _show(pixels: PackedByteArray) -> void:
	var img := Image.create_from_data(RASTER_W, RASTER_H, false, Image.FORMAT_RGBA8, pixels)
	if image_tex == null:
		image_tex = ImageTexture.create_from_image(img)
	else:
		image_tex.update(img)
	display.texture = image_tex
	if shot_file != "":
		img.save_png(shot_file)
		print("GNE 019: shot saved to ", shot_file)

func _brightness(pixels: PackedByteArray, p: Vector2) -> float:
	var o := (int(p.y) * RASTER_W + int(p.x)) * 4
	return (float(pixels[o]) + float(pixels[o + 1]) + float(pixels[o + 2])) / 765.0

func _proj_px(p: Vector3, vp: PackedFloat32Array) -> Vector2:
	var cx: float = vp[0] * p.x + vp[4] * p.y + vp[8] * p.z + vp[12]
	var cy: float = vp[1] * p.x + vp[5] * p.y + vp[9] * p.z + vp[13]
	var cw: float = vp[3] * p.x + vp[7] * p.y + vp[11] * p.z + vp[15]
	var nx := cx / cw
	var ny := cy / cw
	var px := (nx * 0.5 + 0.5) * RASTER_W
	# The raster is y-down for this custom pipeline: world +y maps to LARGER
	# rows (verified via the floating inst3 marker and an in-shader trace).
	# Prefer the in-bounds c0 candidate; c1 stays as a fallback.
	var c0 := Vector2(px, (ny * 0.5 + 0.5) * RASTER_H)
	var c1 := Vector2(px, (0.5 - ny * 0.5) * RASTER_H)
	if c0.x >= 4 and c0.y >= 4 and c0.x < RASTER_W - 4 and c0.y < RASTER_H - 4:
		return c0
	return c1

func _anchor_inst(vp: PackedFloat32Array, inst: int) -> Vector2:
	var c := PTS[inst] + Vector3(0, 0, SCALES[inst] * 0.5)
	return _proj_px(c, vp)

func _cascade_coverage() -> Array:
	# Analytic cascade per instance (hybrid splits, near=300 far=4000):
	# s = 0.5*near*(far/near)^(i/4) + 0.5*(near + i*(far-near)/4).
	var out := []
	var fwd := Vector3(0, 0, -1) # identity camera
	for i in INSTANCE_COUNT:
		var c := PTS[i] + Vector3(0, 0, SCALES[i] * 0.5)
		var planar: float = (c - CAM_POS).dot(fwd)
		var cas := 3
		if planar <= 899.0:
			cas = 0
		elif planar <= 1623.0:
			cas = 1
		elif planar <= 2584.0:
			cas = 2
		out.append([i, planar, cas])
	return out

func _lit_max_step(pixels: PackedByteArray) -> float:
	var mx := 0.0
	var mx_at := Vector2(-1.0, -1.0)
	var mx_vals := Vector2(0.0, 0.0)
	for y in range(0, RASTER_H - 1):
		for x in range(0, RASTER_W - 1):
			var o := (y * RASTER_W + x) * 4
			var b := (float(pixels[o]) + float(pixels[o + 1]) + float(pixels[o + 2])) / 765.0
			# D7-8 #4: measure cascade-seam steps only on fully-lit geometry -
			# object silhouettes, shadow umbra edges and backgrounds are excluded.
			if b < 0.90:
				continue
			var o2 := (y * RASTER_W + x + 1) * 4
			var b2 := (float(pixels[o2]) + float(pixels[o2 + 1]) + float(pixels[o2 + 2])) / 765.0
			var o3 := ((y + 1) * RASTER_W + x) * 4
			var b3 := (float(pixels[o3]) + float(pixels[o3 + 1]) + float(pixels[o3 + 2])) / 765.0
			if b2 < 0.90 or b3 < 0.90:
				continue
			if absf(b - b2) > mx:
				mx = absf(b - b2)
				mx_at = Vector2(x, y)
				mx_vals = Vector2(b, b2)
			if absf(b - b3) > mx:
				mx = absf(b - b3)
				mx_at = Vector2(x, y)
				mx_vals = Vector2(b, b3)
	print("GNE 019: S4 maxstep at ", mx_at, " vals=", mx_vals)
	return mx

func _window_mean(pixels: PackedByteArray, c: Vector2, r: int) -> float:
	var s := 0.0
	var n := 0
	for dy in range(-r, r + 1):
		for dx in range(-r, r + 1):
			s += _brightness(pixels, c + Vector2(dx, dy))
			n += 1
	return s / float(n)

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

func _histogram(pixels: PackedByteArray) -> Array:
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
	return [0, mx - mn, mx]

func _fail(code: int, msg: String) -> void:
	print("GNE 019: FAIL code=", code, " ", msg)
	get_tree().quit(code)
