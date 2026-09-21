extends Node

# GOTOT-007A - CPU Readback Viewport Integration Bridge
# TEMPORARY CPU READBACK BRIDGE - NOT FINAL RENDERING PATH
#
# Proves continuous integration:
#   Godot Camera3D -> existing GOTOT GPU pipeline -> GOTOT framebuffer
#   -> CPU readback -> Godot Image/ImageTexture -> TextureRect -> window
#
# Uses ONLY existing GOTOT public APIs. No C++ changes.

const INSTANCE_COUNT := 100000
const SPREAD := 12000.0
const FRAME_LIMIT := 240
const PRINT_EVERY := 30
const RASTER_W := 1920  # Fixed GOTOT raster target width (documented limitation).
const RASTER_H := 1080  # Fixed GOTOT raster target height (documented limitation).
const ORBIT_RADIUS := 800.0
const ORBIT_HEIGHT := 400.0

var server: GototRenderServer
var camera: Camera3D
var display: TextureRect
var image_tex: ImageTexture

var frame := 0
var angle := 0.0
var want_shot := false
var shot_done := false
var probe_done := false
var orientation_flip := false

# Running accumulators for the debug summary.
var tot_cull := 0.0
var tot_vis := 0.0
var tot_final := 0.0
var tot_raster := 0.0
var tot_readback := 0.0
var tot_frame := 0.0
var measured := 0

var visible_min := -1
var visible_max := -1
var prev_pixels := PackedByteArray()
var changed_frames := 0
var compared_frames := 0


func _ready() -> void:
	server = GototRenderServer.get_server_singleton()
	if server == null:
		_fail(40, "server singleton is null")
		return

	if not server.ensure_gpu_device():
		_fail(41, "local RenderingDevice not available")
		return

	camera = $Camera
	display = $Overlay/Display

	if not server.gpu_scene_create(INSTANCE_COUNT, SPREAD):
		_fail(42, "gpu_scene_create")
		return

	if not server.gpu_scene_dispatch(7):
		_fail(43, "gpu_scene_dispatch")
		return

	RenderingServer.frame_post_draw.connect(_on_frame_post_draw)
	print("GOTOT-NEXT 007A: scene ready instances=", INSTANCE_COUNT)


func _process(delta: float) -> void:
	frame += 1
	if frame > FRAME_LIMIT:
		return
	if shot_done:
		return

	# ---- D. Real moving Camera3D (deterministic orbit) ----
	angle += delta * (TAU / 30.0)
	camera.global_position = Vector3(cos(angle) * ORBIT_RADIUS, ORBIT_HEIGHT, sin(angle) * ORBIT_RADIUS)
	camera.look_at(Vector3.ZERO, Vector3.UP)

	# ---- B/C. Consume real viewport dimensions + real camera ----
	var vp_size: Vector2 = get_viewport().get_visible_rect().size
	server.gpu_scene_set_viewport(vp_size.x, vp_size.y)
	server.gpu_scene_set_camera(camera.get_global_transform(), camera.get_camera_projection())

	# ---- GPU/renderer work, measured separately ----
	var t_cull0 := Time.get_ticks_usec()
	if not server.gpu_cull_dispatch():
		_fail(44, "gpu_cull_dispatch")
		return
	var t_cull1 := Time.get_ticks_usec()
	var visible := server.gpu_cull_get_visible_count()

	if not server.gpu_visibility_dispatch():
		_fail(45, "gpu_visibility_dispatch")
		return
	var t_vis1 := Time.get_ticks_usec()

	if not server.gpu_drawargs_finalize():
		_fail(46, "gpu_drawargs_finalize")
		return
	var t_final1 := Time.get_ticks_usec()

	if not server.gpu_raster_indirect_draw():
		_fail(47, "gpu_raster_indirect_draw")
		return
	var t_raster1 := Time.get_ticks_usec()

	# ---- Bridge: CPU readback ----
	var pixels := server.gpu_raster_read_pixels()
	if pixels.size() != RASTER_W * RASTER_H * 4:
		_fail(48, "raster readback size " + str(pixels.size()))
		return
	var t_readback1 := Time.get_ticks_usec()

	var ms_cull := (t_cull1 - t_cull0) / 1000.0
	var ms_vis := (t_vis1 - t_cull1) / 1000.0
	var ms_final := (t_final1 - t_vis1) / 1000.0
	var ms_raster := (t_raster1 - t_final1) / 1000.0
	var ms_readback := (t_readback1 - t_raster1) / 1000.0

	# ---- Display: pixels -> Godot Image -> ImageTexture -> TextureRect ----
	if not probe_done and frame >= 2:
		probe_done = true
		var probe := _probe_orientation(pixels)
		orientation_flip = probe == 1
		print("GOTOT-NEXT 007A: orientation probe=", probe, " flip=", orientation_flip)

	var img := Image.create_from_data(RASTER_W, RASTER_H, false, Image.FORMAT_RGBA8, pixels)
	if orientation_flip:
		img.flip_y()
	if image_tex == null:
		image_tex = ImageTexture.create_from_image(img)
	else:
		image_tex.update(img)
	display.texture = image_tex

	var ms_frame := (Time.get_ticks_usec() - t_cull0) / 1000.0

	# ---- Dynamic visibility tracking ----
	if visible_min == -1 or visible < visible_min:
		visible_min = visible
	if visible_max == -1 or visible > visible_max:
		visible_max = visible

	# ---- Dynamic image tracking (sampled) ----
	if prev_pixels.size() == pixels.size():
		var diff := 0
		var i := 0
		while i < pixels.size():
			if pixels[i] != prev_pixels[i]:
				diff += 1
			i += 4 * 64  # sample every 64th pixel
		compared_frames += 1
		if diff > 0:
			changed_frames += 1
	prev_pixels = pixels

	tot_cull += ms_cull
	tot_vis += ms_vis
	tot_final += ms_final
	tot_raster += ms_raster
	tot_readback += ms_readback
	tot_frame += ms_frame
	measured += 1

	# ---- Debug output (periodic, actual measured averages) ----
	if frame % PRINT_EVERY == 0:
		print("GOTOT-NEXT 007A")
		print("Instances: ", INSTANCE_COUNT)
		print("Visible: ", visible)
		print("Viewport: ", int(vp_size.x), " x ", int(vp_size.y))
		print("GPU/Renderer:")
		print("Cull: ", (tot_cull / measured), " ms")
		print("HZB/Visibility: ", (tot_vis / measured), " ms")
		print("Finalize: ", (tot_final / measured), " ms")
		print("Raster: ", (tot_raster / measured), " ms")
		print("Bridge:")
		print("Readback: ", (tot_readback / measured), " ms")
		print("Frame: ", (tot_frame / measured), " ms")

	if frame == FRAME_LIMIT:
		print("GOTOT-NEXT 007A: visible range ", visible_min, "..", visible_max)
		print("GOTOT-NEXT 007A: changed_frames=", changed_frames, " compared_frames=", compared_frames)
		want_shot = true


func _on_frame_post_draw() -> void:
	if not want_shot or shot_done:
		return
	shot_done = true
	want_shot = false

	# Evidence: capture the real game window (containing the TextureRect display).
	var shot := get_viewport().get_texture().get_image()
	if shot.is_empty():
		print("GOTOT-NEXT 007A: window screenshot NOT EXECUTED (empty image)")
	else:
		var err := shot.save_png("C:/Users/opc/AppData/Local/Temp/opencode/gt_007_window.png")
		print("GOTOT-NEXT 007A: window screenshot saved=", err == OK)

	server.gpu_scene_destroy()
	print("GOTOT-NEXT 007A: PASS")
	get_tree().quit(0)


# Experimental orientation check (contract 14): project visible instance centers
# with the exact VP the shaders used, then look for the magenta pixel at the
# computed position, in both the natural (NDC +y = down) and flipped convention.
# Returns 0 = natural orientation correct, 1 = image is vertically flipped,
# -1 = inconclusive (no testable instance).
func _probe_orientation(pixels: PackedByteArray) -> int:
	var vp := server.gpu_scene_get_vp()
	if vp.size() != 16:
		return -1

	var magenta := PackedVector2Array()
	var total := RASTER_W * RASTER_H
	for i in total:
		if pixels[i * 4] > 200 and pixels[i * 4 + 2] > 200:
			magenta.append(Vector2(i % RASTER_W, i / RASTER_W))
	if magenta.is_empty():
		return -1

	var positions := server.gpu_scene_readback_positions(0, INSTANCE_COUNT)
	var compact := server.gpu_compact_read()
	if positions.size() != INSTANCE_COUNT or compact.is_empty():
		return -1

	var hits_up := 0
	var hits_flip := 0
	var tested := 0
	for k in mini(compact.size(), 16):
		var idx: int = compact[k]
		var p: Vector3 = positions[idx]
		var cx: float = vp[0] * p.x + vp[4] * p.y + vp[8] * p.z + vp[12]
		var cy: float = vp[1] * p.x + vp[5] * p.y + vp[9] * p.z + vp[13]
		var cw: float = vp[3] * p.x + vp[7] * p.y + vp[11] * p.z + vp[15]
		if cw <= 0.0:
			continue
		var ndcx := cx / cw
		var ndcy := cy / cw
		if ndcx < -1.0 or ndcx > 1.0 or ndcy < -1.0 or ndcy > 1.0:
			continue
		var px: float = (ndcx * 0.5 + 0.5) * RASTER_W
		var py_up: float = (ndcy * 0.5 + 0.5) * RASTER_H
		var py_flip: float = (RASTER_H - 1) - py_up
		tested += 1
		if _has_magenta_near(magenta, px, py_up):
			hits_up += 1
		if _has_magenta_near(magenta, px, py_flip):
			hits_flip += 1

	if tested == 0:
		return -1
	if hits_flip > hits_up:
		return 1
	return 0


func _has_magenta_near(magenta: PackedVector2Array, px: float, py: float) -> bool:
	for m in magenta:
		if abs(m.x - px) <= 4.0 and abs(m.y - py) <= 4.0:
			return true
	return false


func _fail(code: int, msg: String) -> void:
	print("GOTOT-NEXT 007A: FAIL code=", code, " ", msg)
	if server != null:
		server.gpu_scene_destroy()
	get_tree().quit(code)