extends Node

# GNE-026-pre - Country House scene + objective visual metric (Tier 2).
# SPEC: docs/spec_026_pre_scene_metric.md. AUTHORIZED: 0.26-pre implementation
# (D11-7 procedural default). No visual features, no new APIs, no C++.
#
# Procedural house via gpu_mesh_create_from_arrays (zero external assets).
# Parts mirror the 0.26a fix list so each has a subject: separated roof
# slopes (no shared planes with the body), back wall as real geometry, ground
# path vertically separated (no z-fight by construction). All triangles are
# emitted double-sided (each + its reverse) so visibility never depends on an
# assumed winding convention; the fragment normal comes from screen-space
# derivatives (flat shading), proven by main_010's tetra/octa meshes.
#
# Metric (pre-registered, not post-tuned): contrast = stddev(lum)/mean(lum),
# color variance = mean per-pixel channel variance, edge density = fraction
# of pixels with |d lum/dx| > EDGE_T (EDGE_T = 0.1, fixed here before the
# baseline run). Read twice for determinism; baseline printed + sig.

const INSTANCE_COUNT := 9
const RASTER_W := 1920
const RASTER_H := 1080
const CAM_POS := Vector3(0, 300, 1500)
const LIGHT_HOUSE := Vector3(0.5, 1.0, 0.5) # instrument sun: from above-front.
# LIGHT_CANON (-0.5,-1,-0.5) lights bottom-facing surfaces (measured: an
# outdoor scene under it renders at ambient ~0.07 with only a sliver above
# 0.1); top-facing outdoor geometry needs the sun above. Recorded, not tuned.
const WARMUP := 4
const EDGE_T := 0.1 # pre-registered edge threshold (SPEC 0.26-pre section 9)

var server: GneRenderServer
var camera: Camera3D
var display: TextureRect
var frame := 0
var done := false
var sig_file := ""

func _box(verts: PackedVector3Array, idx: PackedInt32Array, mn: Vector3, mx: Vector3) -> void:
	var c := [mn, Vector3(mx.x, mn.y, mn.z), Vector3(mx.x, mn.y, mx.z), Vector3(mn.x, mn.y, mx.z),
		Vector3(mn.x, mx.y, mn.z), Vector3(mx.x, mx.y, mn.z), mx, Vector3(mn.x, mx.y, mx.z)]
	var faces := [[0, 1, 2, 3], [4, 6, 5, 7], [0, 4, 5, 1], [2, 6, 7, 3], [1, 5, 6, 2], [0, 3, 7, 4]]
	for f in faces:
		_quad(verts, idx, c[f[0]], c[f[1]], c[f[2]], c[f[3]])

func _quad(verts: PackedVector3Array, idx: PackedInt32Array, a: Vector3, b: Vector3, c: Vector3, d: Vector3) -> void:
	_tri(verts, idx, a, b, c)
	_tri(verts, idx, a, c, d)

func _tri(verts: PackedVector3Array, idx: PackedInt32Array, a: Vector3, b: Vector3, c: Vector3) -> void:
	var base := verts.size()
	verts.append(a); verts.append(c); verts.append(b)
	idx.append_array([base, base + 1, base + 2])

func _part_mesh(def: Array) -> int:
	var verts := PackedVector3Array()
	var idx := PackedInt32Array()
	for prim in def:
		if prim[0] == "box":
			_box(verts, idx, prim[1], prim[2])
		elif prim[0] == "quad":
			_quad(verts, idx, prim[1], prim[2], prim[3], prim[4])
		elif prim[0] == "tri":
			_tri(verts, idx, prim[1], prim[2], prim[3])
	return server.gpu_mesh_create_from_arrays(verts, idx)

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
		_fail(2600, "no server"); return
	if not server.ensure_gpu_device():
		_fail(2601, "no device"); return
	if not server.gpu_scene_create(INSTANCE_COUNT, 1.0):
		_fail(2602, "scene_create"); return
	if not server.gpu_scene_dispatch(8):
		_fail(2603, "dispatch"); return
	if not server.gpu_mesh_create():
		_fail(2604, "mesh_create"); return
	if not server.gpu_mesh_set_batch_strategy(0):
		_fail(2609, "set_batch_strategy(PER_MESH)"); return
	# Parts: [mesh-def, albedo, rough]. Baked local coords, centered.
	var parts := [
		[[["quad", Vector3(-1000, 0, -800), Vector3(1000, 0, -800), Vector3(1000, 0, 800), Vector3(-1000, 0, 800)]], Color(0.25, 0.5, 0.2), 0.9],
		[[["quad", Vector3(-80, 0.6, 200), Vector3(80, 0.6, 200), Vector3(80, 0.6, 900), Vector3(-80, 0.6, 900)]], Color(0.75, 0.65, 0.45), 0.9],
		[[["box", Vector3(-210, 0, -270), Vector3(210, 260, 70)]], Color(0.85, 0.75, 0.6), 0.8],
		[[["quad", Vector3(0, 380, -290), Vector3(0, 380, 90), Vector3(-260, 250, 90), Vector3(-260, 250, -290)]], Color(0.55, 0.12, 0.1), 0.7],
		[[["quad", Vector3(0, 380, -290), Vector3(260, 250, -290), Vector3(260, 250, 90), Vector3(0, 380, 90)]], Color(0.55, 0.12, 0.1), 0.7],
		[[["tri", Vector3(-210, 260, 70), Vector3(210, 260, 70), Vector3(0, 380, 70)]], Color(0.8, 0.7, 0.55), 0.8],
		[[["tri", Vector3(210, 260, -270), Vector3(-210, 260, -270), Vector3(0, 380, -270)]], Color(0.8, 0.7, 0.55), 0.8],
		[[["box", Vector3(-250, 0, -460), Vector3(250, 300, -420)]], Color(0.5, 0.5, 0.52), 0.9],
		[[["box", Vector3(95, 300, -185), Vector3(145, 440, -135)]], Color(0.5, 0.2, 0.15), 0.8],
	]
	if not server.gpu_material_create():
		_fail(2605, "material_create"); return
	for i in parts.size():
		var mid := _part_mesh(parts[i][0])
		if mid != i + 1:
			_fail(2606, "mesh id, expected " + str(i + 1) + " got " + str(mid)); return
		server.gpu_scene_set_instance_transform(i, Vector3.ZERO, 1.0)
		server.gpu_scene_set_instance_mesh(i, mid)
		if not server.gpu_material_set_albedo(i, parts[i][1]):
			_fail(2607, "albedo " + str(i)); return
		if not server.gpu_material_set_params(i, parts[i][2], 0.0):
			_fail(2607, "params " + str(i)); return
		if not server.gpu_material_set_specular(i, Color(0, 0, 0), 32.0):
			_fail(2607, "specular " + str(i)); return
	if not server.gpu_material_set_light(LIGHT_HOUSE):
		_fail(2608, "set_light"); return
	if OS.get_cmdline_user_args().has("--unlitlight"):
		if not server.gpu_material_set_light(Vector3(0, -1, 0)):
			_fail(2619, "set_light unlit"); return
		print("GNE 026-pre: unlit protocol (sun straight down, ambient floor)")
	if OS.get_cmdline_user_args().has("--direct"):
		# Step 2 protocol: GNE has no ambient off-switch (0.1xALBEDO is
		# hardcoded in cpp:7151), so direct-only is measured as a DIFFERENCE
		# against the --unlitlight frame (identical geometry/camera/materials,
		# only the sun direction differs), which cancels the ambient term
		# exactly. The twin side can literally zero its ambient.
		if not server.gpu_material_set_light(Vector3(0.5, 1.0, 0.5)):
			_fail(2619, "set_light direct"); return
		print("GNE 026-pre: direct protocol (above-front sun + ambient floor)")
	# A/B isolation protocol flags (dormant unless passed; documented):
	# --dump : print the readback as hex rows (GNEDUMP/GNEROW) for diffing.
	# --unlitlight : point the instrument sun straight down so every
	#   camera-facing normal reads ndl=0 (with black specular the frame is
	#   exactly 0.1xALBEDO - the GNE ambient floor for unlit comparison).
	camera = $Camera
	display = $Overlay/Display
	camera.global_position = CAM_POS
	camera.rotation = Vector3.ZERO
	camera.near = 300.0
	camera.far = 6200.0
	var vp_size: Vector2 = get_viewport().get_visible_rect().size
	print("GNE 026-pre: viewport=", vp_size)
	server.gpu_scene_set_viewport(vp_size.x, vp_size.y)
	server.gpu_scene_set_camera(camera.get_global_transform(), camera.get_camera_projection())
	print("GNE 026-pre: house ready parts=", parts.size())

func _process(_delta: float) -> void:
	if done:
		return
	frame += 1
	if frame > WARMUP:
		_measure()
		return
	if not _redraw():
		return

func _redraw() -> bool:
	if not server.gpu_cull_dispatch():
		_fail(2610, "cull"); return false
	var visible := server.gpu_cull_get_visible_count()
	if visible != INSTANCE_COUNT:
		_fail(2611, "frustum must keep all 9, got " + str(visible)); return false
	if not server.gpu_visibility_dispatch():
		_fail(2612, "visibility"); return false
	if not server.gpu_mesh_batch_dispatch():
		_fail(2613, "batch"); return false
	if not server.gpu_material_draw():
		_fail(2614, "draw"); return false
	return true

func _metric(pixels: PackedByteArray) -> Array:
	var n := RASTER_W * RASTER_H
	var sum := 0.0
	var lums := PackedFloat32Array()
	lums.resize(n)
	for i in range(n):
		var o := i * 4
		var r := float(pixels[o]) / 255.0
		var g := float(pixels[o + 1]) / 255.0
		var b := float(pixels[o + 2]) / 255.0
		var l := (r + g + b) / 3.0
		lums[i] = l
		sum += l
	var mean := sum / float(n)
	var vsum := 0.0
	var csum := 0.0
	for i in range(n):
		var o := i * 4
		var r := float(pixels[o]) / 255.0
		var g := float(pixels[o + 1]) / 255.0
		var b := float(pixels[o + 2]) / 255.0
		var m := (r + g + b) / 3.0
		vsum += (lums[i] - mean) * (lums[i] - mean)
		csum += ((r - m) * (r - m) + (g - m) * (g - m) + (b - m) * (b - m)) / 3.0
	var contrast := sqrt(vsum / float(n)) / maxf(mean, 1e-9)
	var variance := csum / float(n)
	var edge := 0
	for y in range(RASTER_H):
		var row := y * RASTER_W
		for x in range(RASTER_W - 1):
			if absf(lums[row + x + 1] - lums[row + x]) > EDGE_T:
				edge += 1
	var edge_density := float(edge) / float(n)
	return [contrast, variance, edge_density]

func _measure() -> void:
	done = true
	var pix_a := server.gpu_raster_read_pixels()
	if pix_a.size() != RASTER_W * RASTER_H * 4:
		_fail(2615, "pixels size a"); return
	if not _redraw():
		return
	var pix_b := server.gpu_raster_read_pixels()
	if pix_b.size() != RASTER_W * RASTER_H * 4:
		_fail(2616, "pixels size b"); return
	var det: bool = (pix_a == pix_b)
	print("GNE 026-pre: readback_det=", det)
	if not det:
		_fail(2617, "readback determinism"); return
	if shot_file != "":
		var img := Image.create_from_data(RASTER_W, RASTER_H, false, Image.FORMAT_RGBA8, pix_a)
		img.save_png(shot_file)
		print("GNE 026-pre: shot saved")
	var m := _metric(pix_a)
	print("GNE 026-pre: BASELINE contrast=%.6f variance=%.9f edge=%.6f" % [m[0], m[1], m[2]])
	if OS.get_cmdline_user_args().has("--probe"):
		_probe(pix_a)
	if OS.get_cmdline_user_args().has("--dump"):
		print("GNEDUMP: begin ", RASTER_W, "x", RASTER_H)
		for y in range(RASTER_H):
			var sb := ""
			var base := y * RASTER_W * 4
			for x in range(RASTER_W):
				var o := base + x * 4
				sb += "%02x%02x%02x" % [pix_a[o], pix_a[o + 1], pix_a[o + 2]]
			print("GNEROW:" + sb)
		print("GNEDUMP: end")
	var sig := "v26pre|contrast=%.6f|variance=%.9f|edge=%.6f|det=1|d1" % [m[0], m[1], m[2]]
	print("GNE 026-pre: sig=", sig)
	print("sig d1: \"" + sig + "\"")
	print("GNE 026-pre: PASS")
	if sig_file != "":
		var fl := FileAccess.open(sig_file, FileAccess.WRITE)
		if fl != null:
			fl.store_string(sig + "\n")
	server.gpu_scene_destroy()
	get_tree().quit(0)

const PART_PROBES := [
	["ground", Vector3(600, 0, 400)],
	["path", Vector3(0, 0.6, 600)],
	["body_front", Vector3(0, 150, 70)],
	["roof_ridge", Vector3(0, 380, 90)],
	["roof_slope_L", Vector3(-130, 315, 90)],
	["roof_slope_R", Vector3(130, 315, 90)],
	["gable_front", Vector3(0, 320, 70)],
	["backwall", Vector3(0, 150, -420)],
	["backwall_rimL", Vector3(-230, 150, -420)],
	["backwall_rimR", Vector3(230, 150, -420)],
	["backwall_top", Vector3(0, 290, -420)],
	["chimney", Vector3(120, 370, -160)],
]

# Diagnostic probe (0.26a D11-9): project each named part point with the
# server's OWN VP (never a hand-rolled projection) and print the pixel there.
# y convention is resolved by trying both and reporting the pair, so the
# readback orientation is measured, not assumed (KI-021).
func _probe(pixels: PackedByteArray) -> void:
	var vp := server.gpu_scene_get_vp()
	print("GNE 026-pre: PROBE vp_len=", vp.size())
	for pr in PART_PROBES:
		var name: String = pr[0]
		var p: Vector3 = pr[1]
		var cx: float = vp[0] * p.x + vp[4] * p.y + vp[8] * p.z + vp[12]
		var cy: float = vp[1] * p.x + vp[5] * p.y + vp[9] * p.z + vp[13]
		var cw: float = vp[3] * p.x + vp[7] * p.y + vp[11] * p.z + vp[15]
		if cw <= 0.0:
			print("GNE 026-pre: PROBE ", name, " behind camera")
			continue
		var px := (cx / cw * 0.5 + 0.5) * float(RASTER_W)
		var yA := int((cy / cw * 0.5 + 0.5) * float(RASTER_H))
		var yB := int((0.5 - cy / cw * 0.5) * float(RASTER_H))
		var xi := int(px)
		var out := ""
		for yy in [yA, yB]:
			if xi < 0 or xi >= RASTER_W or yy < 0 or yy >= RASTER_H:
				out += " oob"
			else:
				var o: int = (yy * RASTER_W + xi) * 4
				out += " (%d,%d,%d)" % [pixels[o], pixels[o + 1], pixels[o + 2]]
		print("GNE 026-pre: PROBE ", name, " px=", xi, " yA=", yA, " yB=", yB, out)

func _fail(code: int, msg: String) -> void:
	print("GNE 026-pre: FAIL code=", code, " ", msg)
	get_tree().quit(code)
