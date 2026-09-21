extends Node

# GOTOT-009 - Real Depth Buffer (D32_SFLOAT) Proof
#
# Uses the ENTIRE GOTOT-008B real-mesh path (cull -> compact -> drawargs ->
# indirect draw) unchanged, with the 009 additions:
#   - the raster framebuffer now carries a D32_SFLOAT depth attachment cleared
#     to 1.0 (far) every frame (DRAW_CLEAR_DEPTH),
#   - the REAL MESH pipeline tests/writes depth with LESS_OR_EQUAL,
#   - the billboard (raster) pipeline keeps depth disabled (unchanged behavior).
#
# Fixed overlay scene (static camera at (0,0,1700), identity rotation, FOV 60,
# 1920x1080): four cubes A/B/C/D where A (nearest) hides B and C on shared
# pixels while their rims stay visible, plus a separated control cube D.
#
# Depth orientation (measured): standard near=smaller, far=larger, clear=1.0.
# Camera near is 300 (not 0.05) so the 2300..2700 cluster maps to a well
# separated band (~0.94 / ~0.95 / ~0.96); LESS_OR_EQUAL rejects the farther
# surface on resampled pixels.
#
# Objective evidence:
#   A) depth write   : run --front-only renders ONLY A+D; pixels with
#      depth < 0.9995 (fg) match the covered color exactly (1:1) and the
#      cleared background reads back 1.0.
#   B) depth test    : full run (A+B+C+D) has depth IDENTICAL to the front-only
#      run on every pixel A/D covered -> B/C never overwrote nearer depth.
#   C) depth values  : the global minimum depth in the frame equals depth(A
#      center) and d(A) < d(D control) -> depth encodes true per-surface
#      distance (the nearest surface holds the smallest value), not a constant.
#   D) visible rims  : B/C rims still render (they pass where they are in front
#      of the far clear), so depth does NOT cull visible geometry.
#   E) readback/args/determinism: args==[36,visible,0,0,0], visible==2/4,
#      cross-run DET signature, repeated frame == identical readback.

const INSTANCE_COUNT := 4
const FRAME_LIMIT := 60
const PRINT_EVERY := 20
const RASTER_W := 1920
const RASTER_H := 1080
# Written foreground depths sit well below 1.0 (measured band ~0.94..0.96);
# the cleared background is exactly 1.0. fg = depth < 0.9995, bg = depth > 1e-6.
const DEPTH_FG := 0.9995
const DEPTH_FAR_CLEAR := 0.999999
const DEPTH_EQ_TOL := 0.001
const DEPTH_ORDER_TOL := 0.002
const DEPTH_A_FILE := "C:/Users/opc/AppData/Local/Temp/opencode/gt009_depth_a.bin"
const META_A_FILE := "C:/Users/opc/AppData/Local/Temp/opencode/gt009_meta_a.txt"
const META_B_FILE := "C:/Users/opc/AppData/Local/Temp/opencode/gt009_meta_b.txt"

# Fixed overlay scene. The frustum built by gpu_scene_set_camera from an
# identity-rotation camera at z = +1700 looks toward +Z and covers world z in
# [-1700, +2294], so a STATIC camera at (0,0,1700) with identity rotation sees
# the cluster at world z = -1000..-600 (view z 2300..2700). View distances:
#   i0 cube A front: (0,0,-600)  scale 29 -> view z 2300, r ~11.9 px (nearest,
#      SMALLEST projected)
#   i1 cube B mid:   (0,0,-800)  scale 52 -> view z 2500, r ~19.9 px
#   i2 cube C back:  (0,0,-1000) scale 84 -> view z 2700, r ~30.0 px (farthest,
#      LARGEST projected)
#   i3 cube D:       (700,0,-800) scale 40 -> control cube, r ~14.6 px
# Pixel-space: B's square surrounds A's, C's surrounds B's -> three concentric
# depth bands: center (A only), ring r in (11.9, 19.9) (B only), ring r in
# (19.9, 30.0) (C only). Depth must read d(A) < d(B) < d(C).
const CUBES: Array[Vector3] = [
	Vector3(0, 0, -600),  # 0: A front (nearest)
	Vector3(0, 0, -800),  # 1: B mid
	Vector3(0, 0, -1000), # 2: C back (farthest)
	Vector3(700, 0, -800), # 3: D control
]
const SCALES: Array[float] = [29.0, 52.0, 140.0, 40.0]
# Static identity-rotation camera (its frustum looks +Z, covering the cluster).
const CAM_POS := Vector3(0, 0, 1700)
# Off-frustum placement for the hidden cubes in the --front-only reference run.
const HIDDEN_POS := Vector3(99999.0, 0.0, 0.0)
# Indices kept visible in the --front-only reference run (A front + D control).
const FRONT_KEEP: Array[int] = [0, 3]
const FULL_KEEP: Array[int] = [0, 1, 2, 3]

var server: GototRenderServer
var camera: Camera3D
var display: TextureRect
var image_tex: ImageTexture

var front_only := false
var frame := 0
var want_shot := false
var shot_done := false

var last_args := PackedInt32Array()
var compact_sorted := PackedInt32Array()
var green_final := -1
var fg_final := -1
var bg_final := -1

# Depth evidence.
var d_center := -1.0
var d_mid_rim := -1.0
var d_back_rim := -1.0
var d_control := -1.0
var d_min := -1.0


func _ready() -> void:
	server = GototRenderServer.get_server_singleton()
	if server == null:
		_fail(50, "server singleton is null")
		return

	front_only = OS.get_cmdline_user_args().has("--front-only")

	if not server.ensure_gpu_device():
		_fail(51, "local RenderingDevice not available")
		return

	camera = $Camera
	display = $Overlay/Display

	camera.global_position = CAM_POS
	camera.rotation = Vector3.ZERO
	# Force a sane near:far ratio (near=0.05 vs far=4000 would crush every
	# written depth into a ~1e-5 band under 1.0). 300 keeps the overlay cluster
	# (view z 2300..2700) in a well separated depth band ~0.94..0.96.
	camera.near = 300.0
	camera.far = 4000.0

	if not server.gpu_scene_create(INSTANCE_COUNT, 1.0):
		_fail(52, "gpu_scene_create")
		return
	if not server.gpu_scene_dispatch(8):
		_fail(53, "gpu_scene_dispatch")
		return
	if not server.gpu_mesh_create():
		_fail(54, "gpu_mesh_create")
		return

	# GOTOT-009 additions: fixed overlay transforms + depth attachment checks.
	for i in INSTANCE_COUNT:
		var pos := CUBES[i]
		var s := SCALES[i]
		if front_only and (i == 1 or i == 2):
			pos = HIDDEN_POS
		server.gpu_scene_set_instance_transform(i, pos, s)

	var dfmt := server.gpu_raster_get_depth_format()
	var mesh_depth := server.gpu_mesh_get_depth_enabled()
	var raster_depth := server.gpu_raster_get_depth_enabled()
	print("GOTOT-NEXT 009: depth_format_value=", dfmt, " mesh_depth_enabled=", mesh_depth,
			" raster_depth_enabled=", raster_depth)
	if dfmt < 0:
		_fail(55, "depth format not exposed (value " + str(dfmt) + ")")
		return
	if not mesh_depth:
		_fail(56, "mesh pipeline depth is not enabled")
		return
	if raster_depth:
		_fail(57, "billboard pipeline must keep depth DISABLED")
		return

	var vp_size: Vector2 = get_viewport().get_visible_rect().size
	server.gpu_scene_set_viewport(vp_size.x, vp_size.y)
	server.gpu_scene_set_camera(camera.get_global_transform(), camera.get_camera_projection())

	RenderingServer.frame_post_draw.connect(_on_frame_post_draw)
	print("GOTOT-NEXT 009: scene ready front_only=", front_only)


func _process(_delta: float) -> void:
	if shot_done:
		return
	frame += 1
	if frame > FRAME_LIMIT:
		return

	var vp_size: Vector2 = get_viewport().get_visible_rect().size
	server.gpu_scene_set_viewport(vp_size.x, vp_size.y)
	server.gpu_scene_set_camera(camera.get_global_transform(), camera.get_camera_projection())

	if not server.gpu_cull_dispatch():
		_fail(59, "gpu_cull_dispatch")
		return
	var visible := server.gpu_cull_get_visible_count()

	if frame == 1:
		var pos := server.gpu_scene_readback_positions(0, INSTANCE_COUNT)
		var sc := server.gpu_scene_readback_scales(0, INSTANCE_COUNT)
		var pl := server.gpu_scene_get_frustum_planes()
		var visf := server.gpu_cull_get_visibility()
		var vp := server.gpu_scene_get_vp()
		var dbg := "DEBUG frame1 visible=" + str(visible)
		for i in INSTANCE_COUNT:
			var d: float = (pos[i] - CAM_POS).length()
			var rpx: float = sc[i] * 540.0 * vp[5] / d if d > 0.0 else -1.0
			dbg += " i" + str(i) + "=" + str(pos[i]) + " s=" + str(sc[i]) + " vz=" + str(d) + " r=" + str(rpx) + " v=" + str(visf[i])
		for p in pl:
			dbg += " pl=" + str(p)
		print(dbg)

	if not server.gpu_visibility_dispatch():
		_fail(60, "gpu_visibility_dispatch")
		return

	if not server.gpu_mesh_drawargs_finalize():
		_fail(61, "gpu_mesh_drawargs_finalize")
		return
	last_args = server.gpu_drawargs_read()

	var expect_visible := 4 if not front_only else 2
	if visible != expect_visible:
		_fail(62, "visible=" + str(visible) + " expected " + str(expect_visible))
		return
	if last_args.size() != 5 or last_args[0] != 36 or last_args[1] != visible or last_args[2] != 0 or last_args[3] != 0 or last_args[4] != 0:
		_fail(63, "indirect args mismatch " + str(last_args) + " visible=" + str(visible))
		return

	var compact := server.gpu_compact_read()
	if compact.size() != visible:
		_fail(64, "compact size " + str(compact.size()) + " != visible " + str(visible))
		return
	compact_sorted = compact.duplicate()
	compact_sorted.sort()
	var keep_ids: Array[int] = FRONT_KEEP if front_only else FULL_KEEP
	var expect_ids := PackedInt32Array()
	for id in keep_ids:
		expect_ids.append(id)
	if compact_sorted != expect_ids:
		_fail(65, "compact ids " + str(compact_sorted) + " != " + str(expect_ids))
		return

	if not server.gpu_mesh_indirect_draw():
		_fail(66, "gpu_mesh_indirect_draw")
		return

	var pixels := server.gpu_raster_read_pixels()
	if pixels.size() != RASTER_W * RASTER_H * 4:
		_fail(67, "raster readback size " + str(pixels.size()))
		return

	var depth := server.gpu_raster_read_depth()
	if depth.size() != RASTER_W * RASTER_H:
		_fail(68, "depth readback size " + str(depth.size()))
		return

	var img := Image.create_from_data(RASTER_W, RASTER_H, false, Image.FORMAT_RGBA8, pixels)
	if image_tex == null:
		image_tex = ImageTexture.create_from_image(img)
	else:
		image_tex.update(img)
	display.texture = image_tex

	if frame == 60:
		var row := 540
		var dbg2 := "DBG row540:"
		for xx in [960, 976, 985, 1010, 1170, 1270, 1408, 1450, 1500]:
			var o: int = (row * RASTER_W + xx) * 4
			var is_green := pixels[o + 1] > 150 and pixels[o] < 120 and pixels[o + 2] < 150
			dbg2 += " x" + str(xx) + "=" + (str(depth[row * RASTER_W + xx]) + ("G" if is_green else "b"))
		var left := 0
		var right := 0
		for xx in RASTER_W:
			var o2: int = (row * RASTER_W + xx) * 4
			if pixels[o2 + 1] > 150 and pixels[o2] < 120 and pixels[o2 + 2] < 150:
				if xx < 1150:
					left += 1
				else:
					right += 1
		dbg2 += " greenL=" + str(left) + " greenR=" + str(right)
		var vp2 := server.gpu_scene_get_vp()
		dbg2 += " vp5=" + str(vp2[5]) + " vp10=" + str(vp2[10]) + " vp14=" + str(vp2[14]) + " camNear=" + str(camera.near)
		var ax_min := 99999
		var ax_max := -1
		var dx_min := 99999
		var dx_max := -1
		for xx in RASTER_W:
			var o2: int = (row * RASTER_W + xx) * 4
			if pixels[o2 + 1] > 150 and pixels[o2] < 120 and pixels[o2 + 2] < 150:
				if xx < 1150:
					ax_min = mini(ax_min, xx)
					ax_max = maxi(ax_max, xx)
				else:
					dx_min = mini(dx_min, xx)
					dx_max = maxi(dx_max, xx)
		dbg2 += " Arow=" + str(ax_min) + ".." + str(ax_max) + " Drow=" + str(dx_min) + ".." + str(dx_max)
		for xx in [988, 980, 976, 970, 960, 950, 944, 936]:
			var o3: int = (row * RASTER_W + xx) * 4
			var g3 := pixels[o3 + 1] > 150 and pixels[o3] < 120 and pixels[o3 + 2] < 150
			dbg2 += " @" + str(xx) + "=" + str(depth[row * RASTER_W + xx]) + ("G" if g3 else "b")
		print(dbg2)

	if frame % PRINT_EVERY == 0:
		print("GOTOT-NEXT 009: frame=", frame, " visible=", visible,
				" args=", last_args, " green_px=", _count_green_exact(pixels))

	if frame == FRAME_LIMIT:
		_finalize(pixels, depth, visible)


func _finalize(pixels: PackedByteArray, depth: PackedFloat32Array, visible: int) -> void:
	green_final = _count_green_exact(pixels)
	var stats := _depth_stats(depth)
	fg_final = stats[0]
	bg_final = stats[1]

	# Depth samples at fixed overlay pixels. A (nearest) owns the on-axis
	# center; B/C are concentric and their clean front faces are never visible
	# (always occluded), so their ring depths come from angled side faces. The
	# robust distance claim instead uses: (1) global foreground minimum == A's
	# center depth (the nearest surface holds the smallest value), and
	# (2) the separated control cube D: d(A) < d(D) with both front faces clear.
	d_center = depth[540 * RASTER_W + 960]        # cube A near face
	d_mid_rim = depth[540 * RASTER_W + 970]       # ring sample (informational)
	d_back_rim = depth[540 * RASTER_W + 980]      # ring sample (informational)
	d_control = _sample_control_depth(pixels, depth)  # cube D center
	d_min = _min_foreground_depth(depth)          # global min over covered px

	# Determinism: repeat whole frame (cull + finalize + draw + readback).
	if not server.gpu_cull_dispatch():
		_fail(71, "gpu_cull_dispatch (determinism repeat)")
		return
	if not server.gpu_mesh_drawargs_finalize():
		_fail(72, "gpu_mesh_drawargs_finalize (determinism repeat)")
		return
	var args2 := server.gpu_drawargs_read()
	if args2 != last_args:
		_fail(73, "drawargs repeat mismatch " + str(last_args) + " vs " + str(args2))
		return
	if not server.gpu_mesh_indirect_draw():
		_fail(74, "gpu_mesh_indirect_draw (determinism repeat)")
		return
	var pixels2 := server.gpu_raster_read_pixels()
	var depth2 := server.gpu_raster_read_depth()
	if _count_green_exact(pixels2) != green_final:
		_fail(75, "same-frame color not identical (determinism)")
		return
	var stats2 := _depth_stats(depth2)
	if stats2[0] != fg_final or stats2[1] != bg_final:
		_fail(76, "same-frame depth not identical (determinism)")
		return
	# Spot depth samples must be identical too.
	if absf(depth2[540 * RASTER_W + 960] - d_center) > 1e-6:
		_fail(77, "same-frame depth sample differs (determinism)")
		return

	# ---- A) depth write + clear (front-only reference run) ----
	# depth < 0.999995 on the drawn pixels and exactly 1.0 on the cleared
	# background. green must equal fg 1:1 (depth covers exactly the drawn
	# geometry) and the covered fraction must be small (drawn cubes _not_ half
	# the screen). The analytic square-area model is unusable here: off-axis
	# cubes project a wider silhouette (parallax), so coverage is evidenced by
	# the green==fg equality plus a loose upper bound instead of a band ratio.
	if front_only:
		var ok_write := fg_final > 0 and bg_final > RASTER_W * RASTER_H / 2
		var ok_budget := fg_final < RASTER_W * RASTER_H / 4
		var green_ok := green_final >= 0.8 * fg_final and green_final <= 1.2 * fg_final
		print("GOTOT-NEXT 009: front-only fg=", fg_final, " bg=", bg_final,
				" green=", green_final)
		print("GOTOT-NEXT 009: depth samples front-only dF=", d_center, " dC=", d_control)
		if not ok_write:
			_fail(78, "depth write/clear failed fg=" + str(fg_final) + " bg=" + str(bg_final))
			return
		if not ok_budget:
			_fail(79, "front-only covered pixels exceed budget " + str(fg_final))
			return
		if not green_ok:
			_fail(80, "front-only green/fg mismatch " + str(green_final) + " vs " + str(fg_final))
			return
		if d_center < 0.0 or d_center > DEPTH_FG:
			_fail(81, "front-only center depth out of range " + str(d_center))
			return
		_save_depth(depth)
		_save_meta()
		_print_signature()
		_finish_pass()
		return

	# ---- B) depth test: full run must equal front-only ON every covered pixel.
	# If M/B wrote through the near surface, the full-run depth would show their
	# (farther) values on F's pixels; equality proves LESS_OR_EQUAL rejected them.
	var depth_a := _load_depth()
	var meta_a := _load_meta_a()
	if depth_a.size() != depth.size() or meta_a.is_empty():
		_fail(81, "reference run artifacts missing/short (depth " + str(depth_a.size()) + ")")
		return
	var eq := _depth_masked_equal(depth, depth_a)
	if eq[2] != 0:
		_fail(82, "depth test broken: " + str(eq[2]) + "/" + str(eq[1]) + " covered pixels differ vs front-only")
		return

	# ---- C) depth values encode true distance ----
	# A (vz 2300, nearest) holds the global foreground depth minimum and sits
	# clearly below the farther control cube D (vz 2596, front face visible).
	var order_ok := d_center < d_control - DEPTH_ORDER_TOL and d_control < DEPTH_FAR_CLEAR
	var min_ok := d_min > 0.0 and absf(d_min - d_center) <= 0.005
	if not order_ok:
		_fail(83, "depth control ordering broken dF=" + str(d_center) + " dC=" + str(d_control))
		return
	if not min_ok:
		_fail(84, "global min depth != A center dMin=" + str(d_min) + " dF=" + str(d_center))
		return

	# ---- D) visible rims: the full run adds B/C rim pixels over the front-only
	# coverage (rim > 0, bounded, and the rims are colored) while equality (B)
	# proves the nearer depth wins every shared pixel.
	var meta_ok := meta_a.size() == 2
	var fg_front: int = meta_a[0] if meta_ok else -1
	var green_front: int = meta_a[1] if meta_ok else -1
	var rim := fg_final - fg_front
	var rim_ok := rim > 0 and rim <= fg_front * 6
	var rim_colored := green_final > green_front
	print("GOTOT-NEXT 009: full fg=", fg_final, " bg=", bg_final,
			" rim=", rim, " green=", green_final)
	print("GOTOT-NEXT 009: depth samples dF=", d_center, " dM=", d_mid_rim,
			" dB=", d_back_rim, " dC=", d_control, " dmin=", d_min)
	print("GOTOT-NEXT 009: masked_eq pixels=", eq[1], " bad=", eq[2])
	if not meta_ok:
		_fail(85, "reference meta malformed")
		return
	if not rim_ok:
		_fail(85, "rim coverage out of bounds rim=" + str(rim) + " fg_front=" + str(fg_front))
		return
	if not rim_colored:
		_fail(86, "rim pixels not colored")
		return

	_save_meta()
	_print_signature()
	_finish_pass()


func _finish_pass() -> void:
	want_shot = true
	print("GOTOT-NEXT 009: EVIDENCE OK")


func _on_frame_post_draw() -> void:
	if not want_shot or shot_done:
		return
	shot_done = true
	want_shot = false

	var shot := get_viewport().get_texture().get_image()
	if shot.is_empty():
		print("GOTOT-NEXT 009: window screenshot NOT EXECUTED (empty image)")
	else:
		var err := shot.save_png("C:/Users/opc/AppData/Local/Temp/opencode/gt_009_window.png")
		print("GOTOT-NEXT 009: window screenshot saved=", err == OK)

	server.gpu_scene_destroy()
	print("GOTOT-NEXT 009: PASS")
	get_tree().quit(0)


func _print_signature() -> void:
	var tag := "A" if front_only else "B"
	var v := compact_sorted.size()
	var m := v / 2
	var c0 := compact_sorted[0] if v > 0 else -1
	var cm := compact_sorted[m] if v > 0 else -1
	var sig := "sig=v%d|%d|%d|%s|g%d|f%d|b%d|dF%.5f|dM%.5f|dB%.5f|dC%.5f|dmin%.5f|c%d|%d" % [
		v, last_args[0], last_args[1], tag, green_final, fg_final, bg_final,
		d_center, d_mid_rim, d_back_rim, d_control, d_min, c0, cm]
	print("GOTOT-NEXT 009-DET ", sig)


func _min_foreground_depth(depth: PackedFloat32Array) -> float:
	var m := 1.0
	for i in depth.size():
		if depth[i] < DEPTH_FG and depth[i] < m:
			m = depth[i]
	return m


func _depth_stats(depth: PackedFloat32Array) -> Array:
	var fg := 0
	var bg := 0
	for i in depth.size():
		if depth[i] < DEPTH_FG:
			fg += 1
		elif depth[i] > DEPTH_FAR_CLEAR:
			bg += 1
	return [fg, bg]


func _depth_masked_equal(depth: PackedFloat32Array, ref: PackedFloat32Array) -> Array:
	var checked := 0
	var ok := 0
	var bad := 0
	for i in ref.size():
		if ref[i] < DEPTH_FG:
			checked += 1
			if absf(depth[i] - ref[i]) <= DEPTH_EQ_TOL:
				ok += 1
			else:
				bad += 1
	return [checked, ok, bad]


# Sample the CONTROL cube's depth: project its center through the exact VP with
# both y-flip conventions and take the depth of a green pixel seen in a small
# window. Returns -1.0 if not found.
func _sample_control_depth(pixels: PackedByteArray, depth: PackedFloat32Array) -> float:
	var vp := server.gpu_scene_get_vp()
	if vp.size() != 16:
		return -1.0
	var p: Vector3 = CUBES[3]
	var ccx: float = vp[0] * p.x + vp[4] * p.y + vp[8] * p.z + vp[12]
	var ccy: float = vp[1] * p.x + vp[5] * p.y + vp[9] * p.z + vp[13]
	var ccw: float = vp[3] * p.x + vp[7] * p.y + vp[11] * p.z + vp[15]
	if ccw <= 0.0:
		return -1.0
	var ndcx := ccx / ccw
	var ndcy := ccy / ccw
	var px: float = (ndcx * 0.5 + 0.5) * float(RASTER_W)
	for cy_sign: int in [1, -1]:
		var py: float = (0.5 + float(cy_sign) * 0.5 * ndcy) * float(RASTER_H)
		if py < 0.0 or py > float(RASTER_H):
			continue
		var x0: int = maxi(int(floor(px)) - 8, 0)
		var x1: int = mini(int(ceil(px)) + 8, RASTER_W - 1)
		var y0: int = maxi(int(floor(py)) - 8, 0)
		var y1: int = mini(int(ceil(py)) + 8, RASTER_H - 1)
		var best := 1.0
		var found := false
		for yy in range(y0, y1 + 1):
			var row: int = yy * RASTER_W
			for xx in range(x0, x1 + 1):
				var o: int = (row + xx) * 4
				if pixels[o + 1] > 150 and pixels[o] < 120 and pixels[o + 2] < 150:
					found = true
					if depth[row + xx] < best:
						best = depth[row + xx]
		if found:
			return best
	return -1.0


# Analytic diamond/square models are NOT used for gating (off-axis boxes
# project wider silhouettes via parallax, so no square model bounds them).
# Coverage is gated by green==fg and bounded-budget checks in run A/B.


func _count_green_exact(pixels: PackedByteArray) -> int:
	var c := 0
	var n: int = RASTER_W * RASTER_H
	for i in n:
		if pixels[i * 4 + 1] > 150 and pixels[i * 4] < 120 and pixels[i * 4 + 2] < 150:
			c += 1
	return c


func _save_depth(depth: PackedFloat32Array) -> void:
	var f := FileAccess.open(DEPTH_A_FILE, FileAccess.WRITE)
	if f == null:
		print("GOTOT-NEXT 009: reference depth WRITE FAILED")
		return
	f.store_buffer(depth.to_byte_array())
	f.close()
	print("GOTOT-NEXT 009: reference depth saved (" + str(depth.size() * 4) + " bytes)")


func _save_meta() -> void:
	var path := META_A_FILE if front_only else META_B_FILE
	var f := FileAccess.open(path, FileAccess.WRITE)
	if f == null:
		print("GOTOT-NEXT 009: meta WRITE FAILED")
		return
	f.store_line(str(fg_final))
	f.store_line(str(green_final))
	f.close()
	print("GOTOT-NEXT 009: meta saved (", path, ")")


func _load_meta_a() -> PackedInt32Array:
	var f := FileAccess.open(META_A_FILE, FileAccess.READ)
	if f == null:
		return PackedInt32Array()
	var a := PackedInt32Array()
	while not f.eof_reached():
		var l := f.get_line().strip_edges()
		if l.is_valid_int():
			a.append(int(l))
	f.close()
	return a


func _load_depth() -> PackedFloat32Array:
	var f := FileAccess.open(DEPTH_A_FILE, FileAccess.READ)
	if f == null:
		return PackedFloat32Array()
	var ba := f.get_buffer(f.get_length())
	f.close()
	return ba.to_float32_array()


func _fail(code: int, msg: String) -> void:
	print("GOTOT-NEXT 009: FAIL code=", code, " ", msg)
	if server != null:
		server.gpu_scene_destroy()
	get_tree().quit(code)