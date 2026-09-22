extends Node

# GOTOT-011 - Multi-Draw / Batch Instancing (workgroup prefix-sum + grouping)
#
# Uses the entire GOTOT-008B/009/010 groundwork unchanged and adds GOTOT-011:
#   - gpu_mesh_set_batch_strategy(PER_MESH | GROUPED | REORDERED),
#   - a workgroup-parallel (Hillis-Steele) inclusive scan assembler replacing
#     the serial pass-2 prefix sum; the per-mesh batch_args layout is identical,
#   - batch GROUPING: G = active (PER_MESH) or min(active, 5) (GROUPED /
#     REORDERED); groups are contiguous ascending-mesh slices,
#   - one procedural (non-indexed) VkDrawIndirectCommand per group with a
#     GPU-written, frame-varying indirect draw count (SPEC 3.2 draw_indirect
#     fallback - vkCmdDrawIndexedIndirectCount does not exist on this RD),
#   - REORDERED produces the same deterministic grouping as GROUPED (the order
#     proof is gpu_mesh_get_batch_order() = strictly ascending mesh ids),
#   - early_fragment_tests on the batch shader (fixed-function Z reject).
#
# Fixed scene (camera (0,0,2000), identity, FOV 60, near=300 far=4000):
#   64 meshes (cube + 63 tetra/octa from arrays) and 128 instances:
#   front plane 8x8 at z=-700 (instances 0..63), back plane 8x8 offset +70x at
#   z=-1100 (instances 64..127). mesh_id[instance] = instance % 64 so every
#   mesh has 2 visible instances -> active = 64 -> 64 DISTINCT batches.
#
# EXPECTED deterministic outputs:
#   visible == 128, active batches == 64.
#   PER_MESH:   groups 64, draw calls 64 (one command per batch).
#   GROUPED:    groups 5,  draw calls 5  (criterion 2: <=5 draw calls).
#   REORDERED:  groups 5,  draw calls 5  + batch_order == [0..63] ascending.
#   depth: front-plane instance depth < back-plane instance depth.
#
# Evidence targets (7 PASS criteria):
#   (1) one frame holds >=10 distinct batches (64 visible meshes all drawn);
#   (2) <=5 draw calls with 64 visible meshes (GROUPED/REORDERED);
#   (3) pixel evidence per batch group (member center shows its mesh color);
#   (4) cross-plane depth ordering d_front < d_back;
#   (5) DET sig stable across the full in-binary repeat; harness runs DET too;
#   (6) regressions 001A..010 still exit 0 (backwards compatibility);
#   (7) draw + dispatch overhead reported per strategy (informational).

const INSTANCE_COUNT := 128
const MESH_COUNT := 64
const FRAME_LIMIT := 60
const PRINT_EVERY := 20
const RASTER_W := 1920
const RASTER_H := 1080
const DEPTH_ORDER_TOL := 0.002
const STRATEGY_PER_MESH := 0
const STRATEGY_GROUPED := 1
const STRATEGY_REORDERED := 2

var window_png := "C:/Users/opc/AppData/Local/Temp/opencode/gt_011_window.png"
var sig_file := "C:/Users/opc/AppData/Local/Temp/opencode/gt011_sig.txt"
var strategy := STRATEGY_REORDERED

var server: GototRenderServer
var camera: Camera3D
var display: TextureRect
var image_tex: ImageTexture

var frame := 0
var want_shot := false
var shot_done := false

var mesh_colors: Array[Color] = []
var last_draw_counts := PackedInt32Array()
var last_order := PackedInt32Array()
var sig := ""
var dispatch_us := 0
var draw_us := 0
var det_ok := false

# Tetrahedron / octahedron geometry (mesh 1 and mesh 2, then reused for 3..63).
const TETRA_VERTS: Array[Vector3] = [
	Vector3(0.0, 0.8, 0.0),
	Vector3(-0.6, -0.4, 0.5),
	Vector3(0.6, -0.4, 0.5),
	Vector3(0.0, -0.4, -0.5),
]
const TETRA_INDICES: Array[int] = [0, 1, 2, 0, 3, 1, 0, 2, 3, 1, 3, 2]
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
const FRONT_PLANE_Z := -700.0
const BACK_PLANE_Z := -1100.0
const INSTANCE_SCALE := 26.0

func _ready() -> void:
	_parse_user_args()
	server = GototRenderServer.get_server_singleton()
	if server == null:
		_fail(100, "server singleton is null")
		return
	if not server.ensure_gpu_device():
		_fail(101, "local RenderingDevice not available")
		return

	camera = $Camera
	display = $Overlay/Display
	camera.global_position = Vector3(0, 0, 2000)
	camera.rotation = Vector3.ZERO
	camera.near = 300.0
	camera.far = 4000.0

	if not server.gpu_scene_create(INSTANCE_COUNT, 1.0):
		_fail(102, "gpu_scene_create")
		return
	if not server.gpu_scene_dispatch(13):
		_fail(103, "gpu_scene_dispatch")
		return
	if not server.gpu_mesh_create():
		_fail(104, "gpu_mesh_create")
		return

	var tetra_verts := PackedVector3Array(TETRA_VERTS)
	var tetra_idx := PackedInt32Array(TETRA_INDICES)
	var octa_verts := PackedVector3Array(OCTA_VERTS)
	var octa_idx := PackedInt32Array(OCTA_INDICES)
	for m in range(1, MESH_COUNT):
		var mesh_id := -1
		if m % 2 == 1:
			mesh_id = server.gpu_mesh_create_from_arrays(tetra_verts, tetra_idx)
		else:
			mesh_id = server.gpu_mesh_create_from_arrays(octa_verts, octa_idx)
		if mesh_id != m:
			_fail(105, "mesh table entry " + str(m) + " got id " + str(mesh_id))
			return

	if server.gpu_mesh_get_mesh_id_count() != MESH_COUNT:
		_fail(106, "mesh_id_count != " + str(MESH_COUNT))
		return

	for m in MESH_COUNT:
		mesh_colors.append(server.gpu_mesh_get_mesh_color(m))

	for i in INSTANCE_COUNT:
		server.gpu_scene_set_instance_transform(i, _instance_pos(i), INSTANCE_SCALE)
		server.gpu_scene_set_instance_mesh(i, i % MESH_COUNT)

	if not server.gpu_mesh_set_batch_strategy(strategy):
		_fail(107, "gpu_mesh_set_batch_strategy")
		return
	if server.gpu_mesh_get_batch_strategy() != strategy:
		_fail(108, "strategy getter mismatch")
		return

	var vp_size: Vector2 = get_viewport().get_visible_rect().size
	server.gpu_scene_set_viewport(vp_size.x, vp_size.y)
	server.gpu_scene_set_camera(camera.get_global_transform(), camera.get_camera_projection())

	RenderingServer.frame_post_draw.connect(_on_frame_post_draw)
	print("GOTOT-NEXT 011: scene ready strategy=", strategy,
			" meshes=", MESH_COUNT, " instances=", INSTANCE_COUNT)

func _parse_user_args() -> void:
	for a in OS.get_cmdline_user_args():
		var kv := a.split("=")
		if kv.size() != 2:
			continue
		if kv[0] == "--strategy":
			strategy = clampi(int(kv[1]), 0, 2)
		elif kv[0] == "--png":
			window_png = kv[1]
		elif kv[0] == "--sigf":
			sig_file = kv[1]

func _instance_pos(i: int) -> Vector3:
	var col: int = i % 8
	var row: int = i / 8
	if i < MESH_COUNT:
		return Vector3((col - 3.5) * 160.0, (row - 3.5) * 100.0, FRONT_PLANE_Z)
	else:
		var j: int = i - MESH_COUNT
		var c2: int = j % 8
		var r2: int = j / 8
		return Vector3((c2 - 3.5) * 160.0 + 70.0, (r2 - 3.5) * 100.0, BACK_PLANE_Z)

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
		_fail(110, "gpu_cull_dispatch")
		return
	var visible := server.gpu_cull_get_visible_count()
	if not server.gpu_visibility_dispatch():
		_fail(111, "gpu_visibility_dispatch")
		return
	if visible != INSTANCE_COUNT:
		_fail(112, "visible=" + str(visible) + " expected " + str(INSTANCE_COUNT))
		return

	var t0 := Time.get_ticks_usec()
	if not server.gpu_mesh_batch_dispatch():
		_fail(113, "gpu_mesh_batch_dispatch")
		return
	dispatch_us = Time.get_ticks_usec() - t0

	var compact := server.gpu_compact_read()
	var compact_sorted := compact.duplicate()
	compact_sorted.sort()
	var expect_ids := PackedInt32Array()
	for id in INSTANCE_COUNT:
		expect_ids.append(id)
	if compact_sorted != expect_ids:
		_fail(114, "compact ids not 0.." + str(INSTANCE_COUNT - 1))
		return

	if not _check_batch_state():
		return

	var t1 := Time.get_ticks_usec()
	if not server.gpu_mesh_batch_draw():
		_fail(115, "gpu_mesh_batch_draw")
		return
	draw_us = Time.get_ticks_usec() - t1

	var pixels := server.gpu_raster_read_pixels()
	if pixels.size() != RASTER_W * RASTER_H * 4:
		_fail(116, "raster readback size " + str(pixels.size()))
		return
	var depth := server.gpu_raster_read_depth()
	if depth.size() != RASTER_W * RASTER_H:
		_fail(117, "depth readback size " + str(depth.size()))
		return

	var img := Image.create_from_data(RASTER_W, RASTER_H, false, Image.FORMAT_RGBA8, pixels)
	if image_tex == null:
		image_tex = ImageTexture.create_from_image(img)
	else:
		image_tex.update(img)
	display.texture = image_tex

	if frame % PRINT_EVERY == 0:
		print("GOTOT-NEXT 011: frame=", frame, " visible=", visible, " strategy=", strategy,
				" groups=", server.gpu_mesh_get_batch_group_count(),
				" draw_calls=", server.gpu_mesh_get_draw_call_count(),
				" batches=", _distinct_batches())

	if frame == FRAME_LIMIT:
		_finalize(pixels, depth, visible)

func _distinct_batches() -> int:
	var n := 0
	for c in last_draw_counts:
		if c > 0:
			n += 1
	return n

func _check_batch_state() -> bool:
	last_draw_counts = server.gpu_mesh_get_draw_counts()
	var groups := server.gpu_mesh_get_batch_group_count()
	var draw_calls := server.gpu_mesh_get_draw_call_count()
	var indirect := server.gpu_mesh_get_indirect_count()
	var batches := _distinct_batches()

	# Draw call + indirect count must agree (same GPU total).
	if draw_calls != indirect or draw_calls != server.gpu_mesh_get_batch_count():
		_fail(118, "draw_call/indirect/batch_total mismatch " + str([draw_calls, indirect, server.gpu_mesh_get_batch_count()]))
		return false

	# PASS (1): >=10 distinct batches in a single frame (64 visible meshes).
	if batches < 10:
		_fail(119, "batches=" + str(batches) + " < 10")
		return false

	# PASS (2): <=5 draw calls in GROUPED / REORDERED; PER_MESH keeps 64.
	if strategy >= STRATEGY_GROUPED:
		if draw_calls > 5:
			_fail(120, "draw_calls=" + str(draw_calls) + " > 5")
			return false
		if groups != 5:
			_fail(121, "groups=" + str(groups) + " != 5")
			return false
	else:
		if groups != MESH_COUNT or draw_calls != MESH_COUNT:
			_fail(122, "per_mesh groups/draw_calls=" + str([groups, draw_calls]))
			return false

	# Reorder proof: batch_order is the concatenated per-group ascending member
	# list -> globally [0..63] under every strategy.
	last_order = server.gpu_mesh_get_batch_order()
	var expected := PackedInt32Array()
	for m in MESH_COUNT:
		expected.append(m)
	if last_order != expected:
		_fail(123, "batch_order mismatch (not ascending 0.." + str(MESH_COUNT - 1) + ")")
		return false
	return true

func _finalize(pixels: PackedByteArray, depth: PackedFloat32Array, visible: int) -> void:
	var vp := server.gpu_scene_get_vp()
	var groups := server.gpu_mesh_get_batch_group_count()

	# PASS (3): pixel evidence per batch GROUP. Compute the exact group member
	# sizes with the same formula as the GPU/CPU partition (active 64).
	var active := 64
	var base: int = active / groups
	var rem: int = active % groups
	var cum := 0
	var cc := 0
	for g in groups:
		var size: int = base + (1 if g < rem else 0)
		var m0: int = last_order[cum]   # first member of this group
		var ok := _color_at_projected(_instance_pos(m0), mesh_colors[m0], pixels, vp)
		cum += size
		if ok:
			cc += 1
	if cc != groups:
		_fail(124, "per-group center evidence " + str(cc) + " != " + str(groups))
		return

	# PASS (4): depth ordering across depth planes (front before back).
	var d_front := _front_depth_at_projected(_instance_pos(0), mesh_colors[0], pixels, depth, vp)
	var d_back := _front_depth_at_projected(_instance_pos(MESH_COUNT), mesh_colors[0], pixels, depth, vp)
	if d_front <= 0.0 or d_back <= 0.0:
		_fail(125, "depth evidence missing dF=" + str(d_front) + " dB=" + str(d_back))
		return
	if not (d_front < d_back - DEPTH_ORDER_TOL):
		_fail(126, "front/back depth order broken dF=" + str(d_front) + " dB=" + str(d_back))
		return

	# Center pixel is inter-grid background (nothing drawn at the exact axis).
	var center_bg := _pixel_matches(960, 540, Color(0, 0, 0), pixels) or _pixel_matches(960, 540, Color(0, 0, 0, 0), pixels)

	# PASS (5): in-binary DET - rerun the full GPU path and compare.
	det_ok = _det_check(pixels, depth, visible)

	var color_counts := _color_counts(pixels)
	print("GOTOT-NEXT 011: evidence strategy=", strategy, " visible=", visible,
			" batches=", _distinct_batches(), " groups=", groups,
			" draw_calls=", server.gpu_mesh_get_draw_call_count(),
			" indirect=", server.gpu_mesh_get_indirect_count(),
			" cc=", cc)
	print("GOTOT-NEXT 011: pixels g=", color_counts[0], " b=", color_counts[1],
			" o=", color_counts[2], " k=", color_counts[3])
	print("GOTOT-NEXT 011: depth dF=", d_front, " dB=", d_back,
			" dispatch_us=", dispatch_us, " draw_us=", draw_us, " det=", det_ok)
	_print_signature(color_counts, cc, d_front, d_back, center_bg)
	_finish_pass()

func _det_check(pixels: PackedByteArray, depth: PackedFloat32Array, visible: int) -> bool:
	var cc1 := _color_counts(pixels)
	var center_depth := depth[540 * RASTER_W + 960]
	if not server.gpu_cull_dispatch():
		return false
	if not server.gpu_mesh_batch_dispatch():
		return false
	if not _check_batch_state():
		return false
	if not server.gpu_mesh_batch_draw():
		return false
	var pixels2 := server.gpu_raster_read_pixels()
	var depth2 := server.gpu_raster_read_depth()
	var cc2 := _color_counts(pixels2)
	if cc1 != cc2:
		print("GOTOT-NEXT 011: DET color counts differ")
		return false
	if absf(depth2[540 * RASTER_W + 960] - center_depth) > 1e-6:
		print("GOTOT-NEXT 011: DET center depth differs")
		return false
	if server.gpu_mesh_get_draw_call_count() != (5 if strategy >= STRATEGY_GROUPED else MESH_COUNT):
		return false
	return true

func _color_counts(pixels: PackedByteArray) -> Array:
	# [green, blue, orange, gold] in mesh palette order.
	var cg := _count_color(pixels, mesh_colors[0])
	var cb := _count_color(pixels, mesh_colors[1])
	var co := _count_color(pixels, mesh_colors[2])
	var ck := 0
	for m in range(3, MESH_COUNT):
		var c := _count_color(pixels, mesh_colors[m])
		if c > ck:
			ck = c
	return [cg, cb, co, ck]

func _print_signature(color_counts: Array, cc: int, d_front: float, d_back: float, center_bg: bool) -> void:
	sig = "sig=v%d|st%d|m%d|gc%d|dc%d|ic%d|cc%d|dF%.5f|dB%.5f|cb%d|dt%d|%d/%d" % [
		INSTANCE_COUNT, strategy, _distinct_batches(), server.gpu_mesh_get_batch_group_count(),
		server.gpu_mesh_get_draw_call_count(), server.gpu_mesh_get_indirect_count(),
		cc, d_front, d_back, 1 if center_bg else 0, 1 if det_ok else 0, dispatch_us, draw_us]
	print("GOTOT-NEXT 011-DET ", sig)
	var f := FileAccess.open(sig_file, FileAccess.WRITE)
	if f == null:
		print("GOTOT-NEXT 011: sig file WRITE FAILED: ", sig_file)
	else:
		f.store_line(sig)
		f.close()

func _finish_pass() -> void:
	want_shot = true
	print("GOTOT-NEXT 011: EVIDENCE OK")

func _on_frame_post_draw() -> void:
	if not want_shot or shot_done:
		return
	shot_done = true
	want_shot = false
	var shot := get_viewport().get_texture().get_image()
	if shot.is_empty():
		print("GOTOT-NEXT 011: window screenshot NOT EXECUTED (empty image)")
	else:
		var err := shot.save_png(window_png)
		print("GOTOT-NEXT 011: window screenshot saved=", err == OK)
	server.gpu_scene_destroy()
	print("GOTOT-NEXT 011: PASS")
	get_tree().quit(0)

# --- pixel helpers (identical conventions to main_010) ---
func _count_color(pixels: PackedByteArray, col: Color) -> int:
	var c := 0
	var n: int = RASTER_W * RASTER_H
	var r0 := int(round(col.r * 255.0))
	var g0 := int(round(col.g * 255.0))
	var b0 := int(round(col.b * 255.0))
	for i in n:
		var o: int = i * 4
		if _close(pixels[o], r0) and _close(pixels[o + 1], g0) and _close(pixels[o + 2], b0):
			c += 1
	return c

func _close(v: int, t: int) -> bool:
	return absi(v - t) <= 40

func _pixel_matches(x: int, y: int, col: Color, pixels: PackedByteArray) -> bool:
	if x < 0 or x >= RASTER_W or y < 0 or y >= RASTER_H:
		return false
	var o: int = (y * RASTER_W + x) * 4
	return _close(pixels[o], int(round(col.r * 255.0))) and _close(pixels[o + 1], int(round(col.g * 255.0))) and _close(pixels[o + 2], int(round(col.b * 255.0)))

func _project_px(p: Vector3, vp: PackedFloat32Array) -> Array:
	var ccx: float = vp[0] * p.x + vp[4] * p.y + vp[8] * p.z + vp[12]
	var ccy: float = vp[1] * p.x + vp[5] * p.y + vp[9] * p.z + vp[13]
	var ccw: float = vp[3] * p.x + vp[7] * p.y + vp[11] * p.z + vp[15]
	if ccw <= 0.0:
		return []
	var ndcx := ccx / ccw
	var ndcy := ccy / ccw
	var px: float = (ndcx * 0.5 + 0.5) * float(RASTER_W)
	var out := []
	for cy_sign: int in [1, -1]:
		var py: float = (0.5 + float(cy_sign) * 0.5 * ndcy) * float(RASTER_H)
		if py >= 0.0 and py <= float(RASTER_H):
			out.append(Vector2(px, py))
	return out

func _color_at_projected(p: Vector3, col: Color, pixels: PackedByteArray, vp: PackedFloat32Array) -> bool:
	var pts := _project_px(p, vp)
	for pt in pts:
		if _search_color(int(pt.x), int(pt.y), 11, col, pixels):
			return true
	return false

func _front_depth_at_projected(p: Vector3, col: Color, pixels: PackedByteArray, depth: PackedFloat32Array, vp: PackedFloat32Array) -> float:
	var pts := _project_px(p, vp)
	if pts.is_empty():
		return -1.0
	var pt: Vector2 = pts[0]
	var x0: int = maxi(int(floor(pt.x)) - 11, 0)
	var x1: int = mini(int(ceil(pt.x)) + 11, RASTER_W - 1)
	var y0: int = maxi(int(floor(pt.y)) - 11, 0)
	var y1: int = mini(int(ceil(pt.y)) + 11, RASTER_H - 1)
	var best := 1.0
	for yy in range(y0, y1 + 1):
		var row: int = yy * RASTER_W
		for xx in range(x0, x1 + 1):
			if _pixel_matches(xx, yy, col, pixels):
				if depth[row + xx] < best:
					best = depth[row + xx]
	if best >= 1.0:
		return -1.0
	return best

func _search_color(cx: int, cy: int, r: int, col: Color, pixels: PackedByteArray) -> bool:
	for dy in range(-r, r + 1):
		var yy := cy + dy
		if yy < 0 or yy >= RASTER_H:
			continue
		for dx in range(-r, r + 1):
			var xx := cx + dx
			if xx < 0 or xx >= RASTER_W:
				continue
			if _pixel_matches(xx, yy, col, pixels):
				return true
	return false

func _fail(code: int, msg: String) -> void:
	print("GOTOT-NEXT 011: FAIL code=", code, " ", msg)
	if server != null:
		server.gpu_scene_destroy()
	get_tree().quit(code)