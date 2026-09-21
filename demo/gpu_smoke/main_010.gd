extends Node

# GOTOT-010 - Batch Instance Rendering (multi-mesh, per-batch indirect draw) Proof
#
# Uses the ENTIRE GOTOT-008B real-mesh groundwork (cull -> compact) unchanged,
# with the 010 additions:
#   - a per-instance mesh_id buffer (gpu_scene_set_instance_mesh),
#   - a 64-slot GPU mesh table (GototMeshDesc) built from gpu_mesh_create (cube,
#     mesh 0) + gpu_mesh_create_from_arrays (tetrahedron mesh 1, octahedron mesh 2),
#   - a prefix-sum build_batch_args compute path (per-mesh count + assemble),
#   - a single multi-draw indirect draw whose draw_count == number of DISTINCT
#     visible meshes (batch_count), NOT the instance count,
#   - the depth attachment/test from 009 unchanged (D32_SFLOAT, LESS_OR_EQUAL,
#     clear=1.0 far).
#
# Fixed scene (camera (0,0,1700), identity rotation, FOV 60, near=300 far=4000,
# 1920x1080): 6 instances of 3 different meshes.
#   i0 cube  (0,0,-600)   s=29   mesh0 green   <- nearest, on axis (blocks i1/i2)
#   i1 tetra (0,0,-800)   s=45   mesh1 blue    <- occluded center
#   i2 octa  (0,0,-1000)  s=86   mesh2 orange  <- occluded center
#   i3 cube  (320,0,-700) s=36   mesh0 green   <- off-axis, fully visible
#   i4 tetra (-380,0,-840) s=46  mesh1 blue    <- off-axis, fully visible
#   i5 octa  (0,-420,-940) s=64  mesh2 orange  <- off-axis, fully visible
#
# Expected deterministic outputs (see body):
#   visible == 6, mesh_id_count == 3, batch_count == 3,
#   draw_counts == [2,2,2], batch args per mesh with first_instance prefix sums,
#   center pixel (960,540) == cube green (nearest mesh owns shared pixels),
#   d(cube) < d(tetra front) < d(octa front), DET stable across runs.
#
# Evidence targets (8 PASS criteria):
#   (1) >=3 visible meshes in one camera/draw; (2) per-mesh pixel color evidence;
#   (3) cross-mesh depth ordering; (4) batch_count == distinct visible meshes
#   (no per-instance billboard loop); (5) DET sig stable across two runs;
#   (6) sweep regressions exit 0; (7) GPU==CPU culling; (8) billboard path
#   untouched (depth still disabled, never invoked here).

const INSTANCE_COUNT := 6
const FRAME_LIMIT := 60
const PRINT_EVERY := 20
const RASTER_W := 1920
const RASTER_H := 1080
const DEPTH_FG := 0.9995
const DEPTH_ORDER_TOL := 0.002
const WINDOW_PNG := "C:/Users/opc/AppData/Local/Temp/opencode/gt_010_window.png"
const SIG_FILE := "C:/Users/opc/AppData/Local/Temp/opencode/gt010_sig.txt"

const MESH_ASSIGN: Array[int] = [0, 1, 2, 0, 1, 2]
const CUBES: Array[Vector3] = [
	Vector3(0, 0, -600),
	Vector3(0, 0, -800),
	Vector3(0, 0, -1000),
	Vector3(320, 0, -700),
	Vector3(-380, 0, -840),
	Vector3(0, -420, -940),
]
const SCALES: Array[float] = [29.0, 45.0, 86.0, 36.0, 46.0, 64.0]
const CAM_POS := Vector3(0, 0, 1700)

# Meshes as meshes: tetrahedron (mesh 1) and octahedron (mesh 2).
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
# Indices whose projected CENTER must show their mesh color (fully visible).
const CENTER_EVIDENCE: Array[int] = [0, 3, 4, 5]
# Indices whose centers are occluded by the nearest cube (must show GREEN).
const OCCLUDED_CENTER: Array[int] = [1, 2]
# Control center index for the depth ordering (cube front face).
const CUBE_FRONT_INDEX := 0
const TETRA_FRONT_INDEX := 4
const OCTA_FRONT_INDEX := 5

var server: GototRenderServer
var camera: Camera3D
var display: TextureRect
var image_tex: ImageTexture

var frame := 0
var want_shot := false
var shot_done := false

var mesh_colors: Array[Color] = []
var last_args := PackedInt32Array()
var compact_sorted := PackedInt32Array()
var last_draw_counts := PackedInt32Array()
var sig := ""

# Color evidence.
var count_green := 0
var count_blue := 0
var count_orange := 0
var d_cube := -1.0
var d_tetra := -1.0
var d_octa := -1.0
var center_is_cube := false
var center_depth := -1.0

func _ready() -> void:
	server = GototRenderServer.get_server_singleton()
	if server == null:
		_fail(50, "server singleton is null")
		return
	if not server.ensure_gpu_device():
		_fail(51, "local RenderingDevice not available")
		return

	camera = $Camera
	display = $Overlay/Display
	camera.global_position = CAM_POS
	camera.rotation = Vector3.ZERO
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

	var tetra_verts := PackedVector3Array()
	for v in TETRA_VERTS:
		tetra_verts.append(v)
	var tetra_idx := PackedInt32Array()
	for i in TETRA_INDICES:
		tetra_idx.append(i)
	var mesh1 := server.gpu_mesh_create_from_arrays(tetra_verts, tetra_idx)
	if mesh1 != 1:
		_fail(55, "gpu_mesh_create_from_arrays (tetra) -> " + str(mesh1))
		return

	var octa_verts := PackedVector3Array()
	for v in OCTA_VERTS:
		octa_verts.append(v)
	var octa_idx := PackedInt32Array()
	for i in OCTA_INDICES:
		octa_idx.append(i)
	var mesh2 := server.gpu_mesh_create_from_arrays(octa_verts, octa_idx)
	if mesh2 != 2:
		_fail(56, "gpu_mesh_create_from_arrays (octa) -> " + str(mesh2))
		return

	if server.gpu_mesh_get_mesh_id_count() != 3:
		_fail(90, "mesh_id_count != 3")
		return

	for m in 3:
		mesh_colors.append(server.gpu_mesh_get_mesh_color(m))

	for i in INSTANCE_COUNT:
		server.gpu_scene_set_instance_transform(i, CUBES[i], SCALES[i])
		server.gpu_scene_set_instance_mesh(i, MESH_ASSIGN[i])

	# 008/009: the billboard (raster) pipeline must keep depth DISABLED.
	if server.gpu_raster_get_depth_enabled():
		_fail(57, "billboard path must keep depth disabled")
		return

	var vp_size: Vector2 = get_viewport().get_visible_rect().size
	server.gpu_scene_set_viewport(vp_size.x, vp_size.y)
	server.gpu_scene_set_camera(camera.get_global_transform(), camera.get_camera_projection())

	RenderingServer.frame_post_draw.connect(_on_frame_post_draw)
	print("GOTOT-NEXT 010: scene ready meshes=", mesh_colors)
	print("GOTOT-NEXT 010: colors cube=", mesh_colors[0], " tetra=", mesh_colors[1], " octa=", mesh_colors[2])

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
	if not server.gpu_visibility_dispatch():
		_fail(60, "gpu_visibility_dispatch")
		return
	if not server.gpu_mesh_batch_dispatch():
		_fail(61, "gpu_mesh_batch_dispatch")
		return

	if visible != INSTANCE_COUNT:
		_fail(62, "visible=" + str(visible) + " expected " + str(INSTANCE_COUNT))
		return

	var culled := _cpu_frustum_visible()
	if culled != INSTANCE_COUNT:
		_fail(70, "GPU==CPU cull mismatch gpu=" + str(visible) + " cpu=" + str(culled))
		return

	var compact := server.gpu_compact_read()
	compact_sorted = compact.duplicate()
	compact_sorted.sort()
	var expect_ids := PackedInt32Array()
	for id in INSTANCE_COUNT:
		expect_ids.append(id)
	if compact_sorted != expect_ids:
		_fail(63, "compact ids " + str(compact_sorted) + " != " + str(expect_ids))
		return

	if not _check_batch_state():
		return

	if not server.gpu_mesh_batch_draw():
		_fail(64, "gpu_mesh_batch_draw")
		return

	var pixels := server.gpu_raster_read_pixels()
	if pixels.size() != RASTER_W * RASTER_H * 4:
		_fail(65, "raster readback size " + str(pixels.size()))
		return
	var depth := server.gpu_raster_read_depth()
	if depth.size() != RASTER_W * RASTER_H:
		_fail(66, "depth readback size " + str(depth.size()))
		return

	var img := Image.create_from_data(RASTER_W, RASTER_H, false, Image.FORMAT_RGBA8, pixels)
	if image_tex == null:
		image_tex = ImageTexture.create_from_image(img)
	else:
		image_tex.update(img)
	display.texture = image_tex

	if frame % PRINT_EVERY == 0:
		var argsline := ""
		for b in server.gpu_mesh_get_batch_count():
			argsline += " " + str(server.gpu_mesh_get_batch_args(b))
		print("GOTOT-NEXT 010: frame=", frame, " visible=", visible, " batch_count=",
				server.gpu_mesh_get_batch_count(), " draw_counts=", server.gpu_mesh_get_draw_counts(),
				" args=", argsline)

	if frame == FRAME_LIMIT:
		_finalize(pixels, depth, visible)

func _check_batch_state() -> bool:
	last_draw_counts = server.gpu_mesh_get_draw_counts()
	# PASS (4): batch_count == number of distinct visible meshes (not instances).
	# Every one of the 3 meshes has visible instances -> expect 3 batches.
	var bc := server.gpu_mesh_get_batch_count()
	if bc != 3:
		_fail(80, "batch_count=" + str(bc) + " expected 3")
		return false
	if last_draw_counts.size() < 3 or last_draw_counts[0] != 2 or last_draw_counts[1] != 2 or last_draw_counts[2] != 2:
		_fail(81, "draw_counts mismatch " + str(last_draw_counts))
		return false

	# batch args: dense, ordered by mesh id, first_instance = prefix sum.
	var args: Array[PackedInt32Array] = []
	for b in bc:
		args.append(server.gpu_mesh_get_batch_args(b))
	# cube mesh0: index_count 36, vertex_offset 0, first_index 0
	# tetra mesh1: 12 indices at first_index 36, vertex offset 8
	# octa mesh2: 24 indices at first_index 48, vertex offset 12
	if args.size() != 3:
		_fail(82, "args size != 3")
		return false
	var expected := [
		PackedInt32Array([36, 2, 0, 0, 0]),
		PackedInt32Array([12, 2, 36, 8, 2]),
		PackedInt32Array([24, 2, 48, 12, 4]),
	]
	for b in bc:
		if args[b] != expected[b]:
			_fail(83, "batch args[" + str(b) + "]=" + str(args[b]) + " expected " + str(expected[b]))
			return false
	last_args = args[bc - 1]
	return true

func _finalize(pixels: PackedByteArray, depth: PackedFloat32Array, visible: int) -> void:
	count_green = _count_color(pixels, mesh_colors[0])
	count_blue = _count_color(pixels, mesh_colors[1])
	count_orange = _count_color(pixels, mesh_colors[2])

	var vp := server.gpu_scene_get_vp()

	# Per-mesh color evidence on fully-visible centers.
	for i in CENTER_EVIDENCE:
		var found := _color_at_projected(CUBES[i], mesh_colors[MESH_ASSIGN[i]], pixels, vp)
		if not found:
			_fail(84, "center evidence missing for instance " + str(i))
			return

	# Occluded centers must show the nearest mesh (cube green) - cross-mesh depth.
	for i in OCCLUDED_CENTER:
		var found := _color_at_projected(CUBES[i], mesh_colors[0], pixels, vp)
		if not found:
			_fail(85, "occluded center not covered by cube for instance " + str(i))
			return

	# Per-mesh pixel counts > 0 (each mesh draws its own geometry).
	if count_green <= 0 or count_blue <= 0 or count_orange <= 0:
		_fail(86, "a mesh color count is 0 g/b/o=" + str(count_green) + "/" + str(count_blue) + "/" + str(count_orange))
		return

	# Center pixel depth/sample.
	center_depth = depth[540 * RASTER_W + 960]
	center_is_cube = _pixel_matches(960, 540, mesh_colors[0], pixels)
	if not center_is_cube:
		_fail(87, "center pixel is not cube green")
		return

	# Depth ordering across meshes (front = nearest).
	d_cube = _front_depth_at_projected(CUBES[CUBE_FRONT_INDEX], mesh_colors[0], pixels, depth, vp)
	d_tetra = _front_depth_at_projected(CUBES[TETRA_FRONT_INDEX], mesh_colors[1], pixels, depth, vp)
	d_octa = _front_depth_at_projected(CUBES[OCTA_FRONT_INDEX], mesh_colors[2], pixels, depth, vp)
	if d_cube <= 0.0 or d_tetra <= 0.0 or d_octa <= 0.0:
		_fail(88, "depth evidence missing dC=" + str(d_cube) + " dT=" + str(d_tetra) + " dO=" + str(d_octa))
		return
	if not (d_cube < d_tetra - DEPTH_ORDER_TOL and d_tetra < d_octa - DEPTH_ORDER_TOL):
		_fail(89, "cross-mesh depth order broken dC=" + str(d_cube) + " dT=" + str(d_tetra) + " dO=" + str(d_octa))
		return
	if d_cube > DEPTH_FG:
		_fail(91, "cube depth not foreground dC=" + str(d_cube))
		return

	# Determinism: repeat the full GPU path once and compare.
	if not server.gpu_cull_dispatch():
		_fail(71, "gpu_cull_dispatch (det)")
		return
	if not server.gpu_mesh_batch_dispatch():
		_fail(72, "gpu_mesh_batch_dispatch (det)")
		return
	if not _check_batch_state():
		return
	if not server.gpu_mesh_batch_draw():
		_fail(73, "gpu_mesh_batch_draw (det)")
		return
	var pixels2 := server.gpu_raster_read_pixels()
	var depth2 := server.gpu_raster_read_depth()
	if _count_color(pixels2, mesh_colors[0]) != count_green or _count_color(pixels2, mesh_colors[1]) != count_blue or _count_color(pixels2, mesh_colors[2]) != count_orange:
		_fail(74, "same-frame color counts differ (det)")
		return
	if absf(depth2[540 * RASTER_W + 960] - center_depth) > 1e-6:
		_fail(75, "same-frame center depth differs (det)")
		return

	print("GOTOT-NEXT 010: evidence batch_count=", server.gpu_mesh_get_batch_count(),
			" draw_counts=", last_draw_counts)
	print("GOTOT-NEXT 010: pixels g=", count_green, " b=", count_blue, " o=", count_orange)
	print("GOTOT-NEXT 010: depth dC=", d_cube, " dT=", d_tetra, " dO=", d_octa, " center=", center_depth)
	_print_signature()
	_finish_pass()

func _print_signature() -> void:
	sig = "sig=v%d|mc%d|bc%d|dc%d/%d/%d|g%d|b%d|o%d|dC%.5f|dT%.5f|dO%.5f|cc%d|c%d|%d" % [
		INSTANCE_COUNT, server.gpu_mesh_get_mesh_id_count(), server.gpu_mesh_get_batch_count(),
		last_draw_counts[0], last_draw_counts[1], last_draw_counts[2],
		count_green, count_blue, count_orange, d_cube, d_tetra, d_octa,
		1 if center_is_cube else 0, compact_sorted[0], compact_sorted[INSTANCE_COUNT - 1]]
	print("GOTOT-NEXT 010-DET ", sig)
	var f := FileAccess.open(SIG_FILE, FileAccess.WRITE)
	if f == null:
		print("GOTOT-NEXT 010: sig file WRITE FAILED")
	else:
		f.store_line(sig)
		f.close()

func _finish_pass() -> void:
	want_shot = true
	print("GOTOT-NEXT 010: EVIDENCE OK")

func _on_frame_post_draw() -> void:
	if not want_shot or shot_done:
		return
	shot_done = true
	want_shot = false
	var shot := get_viewport().get_texture().get_image()
	if shot.is_empty():
		print("GOTOT-NEXT 010: window screenshot NOT EXECUTED (empty image)")
	else:
		var err := shot.save_png(WINDOW_PNG)
		print("GOTOT-NEXT 010: window screenshot saved=", err == OK)
	server.gpu_scene_destroy()
	print("GOTOT-NEXT 010: PASS")
	get_tree().quit(0)

# --- GPU == CPU culling ---
func _cpu_frustum_visible() -> int:
	var pos := server.gpu_scene_readback_positions(0, INSTANCE_COUNT)
	var sc := server.gpu_scene_readback_scales(0, INSTANCE_COUNT)
	var planes := server.gpu_scene_get_frustum_planes()
	if pos.size() != INSTANCE_COUNT or sc.size() != INSTANCE_COUNT or planes.size() != 6:
		return -1
	var n := 0
	for i in INSTANCE_COUNT:
		var inside := true
		for p in planes:
			var d: float = p.x * pos[i].x + p.y * pos[i].y + p.z * pos[i].z + p.w
			if d < -sc[i]:
				inside = false
				break
		if inside:
			n += 1
	return n

# --- pixel helpers ---
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
	return absf(v - t) <= 40

func _pixel_matches(x: int, y: int, col: Color, pixels: PackedByteArray) -> bool:
	if x < 0 or x >= RASTER_W or y < 0 or y >= RASTER_H:
		return false
	var o: int = (y * RASTER_W + x) * 4
	return _close(pixels[o], int(round(col.r * 255.0))) and _close(pixels[o + 1], int(round(col.g * 255.0))) and _close(pixels[o + 2], int(round(col.b * 255.0)))

# Project p through the exact VP; returns a world-pixel list for both y-flip conventions.
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

# Returns the minimum depth among pixels of col inside the window around the
# projected center -> the front-face depth of that mesh instance.
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
	print("GOTOT-NEXT 010: FAIL code=", code, " ", msg)
	if server != null:
		server.gpu_scene_destroy()
	get_tree().quit(code)