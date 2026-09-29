extends Node

# GNE-016.5 slice-3: isolate the CLUSTERED light loop from the global
# ambient/directional terms, and prove the material channel path feeds it.
#
# WHY A STANDALONE SCENE
# -----------------------
# In main_016_5 the cluster is crowded: 4 spots (lids 16-19) sit in nearly every
# cluster plus white points, so a channel change is mediated by several stacked
# white lights and the isolation would drown in interference. The spots are
# white or near-white (1, 0.9, 0.8), so they carry no usable chroma. Adding a
# saturated light there would also perturb the cluster population that gt_018a
# pins with a literal signature. Hence: ONE instance, ONE saturated point
# light, this scene only. No reference gate is touched.
#
# HOW THE ISOLATION WORKS
# -----------------------
# Shader (gpu_mat_light_frag_glsl):
#   col  = AMB*alb + alb*ndl*light_color + spec(rough) + emi     <- global terms
#   col += (ldiff + lspec) * shf                                   <- cluster loop
#     ldiff = alb * ndl2 * A1.rgb * (A1.a * att * cone_f)
# Both the global and the cluster term scale with `alb`, so a plain difference
# cannot separate them. Two facts make the split exact:
#
#  1. `gpu_material_set_light(dir)` points the global directional light along
#     +Z, perpendicular to inst0's front-face normal (-Z). Then
#     ndl = max(dot(N,L),0) = 0 AND step(0.0, dot(N,L)) = 0, which kills BOTH
#     the global diffuse and the global specular in one move.
#  2. The light is PURE RED. The cluster term is therefore red-only, while the
#     surviving global term AMB*alb (AMB = 0.1, hardcoded) is NEUTRAL - it is
#     multiplied by albedo, not by the light colour.
#
# The albedo payload is NEUTRAL GREY (128,128,128) on purpose. A tinted albedo
# would colour the neutral term too and destroy the split. With grey albedo:
#     delta_G == delta_B            (only the neutral term moves G and B)
#     delta_R  = neutral + cluster  (the cluster term is red-only)
#  => cluster_chroma = delta_R - delta_G   is the cluster contribution alone.
#
# 2x2 DESIGN
#   A = channel bound,  light present
#   B = channel bound,  light absent (--nolight)
#   C = channel unset,  light present
#   D = channel unset,  light absent (--nolight)
#   cluster_term(channel) = A - B ; cluster_term(none) = C - D
#   interaction = (A - B) - (C - D)
# Light presence cannot be toggled at runtime (gpu_light_create is append-only,
# no delete and no intensity setter), so the four cells are four PROCESSES of
# this same scene selected by --nolight. That keeps every other variable fixed.
#
# THRESHOLDS (pre-declared, not tuned to the result)
#   body_px == 81/81                     the window is surface, not background
#   neutral_split  |dG - dB|  <= 2.0     G and B carry only the neutral term
#   cluster_chroma  dR - dG   >= 2.55    >= 1.0% of the 0..255 range
#   chroma_ratio    (dR-dG)/|dG| >= 0.5  the red signature must dominate
#   positive controls: roughness must move max_lsb, normal must move the readback
# The verdict is PRINTED, not gated. It becomes a SLICE condition only after it
# is shown to pass, per the X1 precedent.

const RASTER_W := 1920
const RASTER_H := 1080
const CAM_POS := Vector3(0, 0, 1700)

const PTS: Array[Vector3] = [Vector3(0, 0, -600)]
const SCALES: Array[float] = [120.0]

# Pure red, placed close to inst0 so its range AABB certainly intersects the
# cluster that covers the target pixels. Intensity is deliberately LOW: the
# first run at 1.1 drove every channel to 255 (clipped), which destroys the
# chroma split because a clipped channel cannot report a delta. The red term
# must land mid-scale so R stays below 255 and the R-vs-G difference survives.
const LIGHT_POS := Vector3(0, 0, -300)
const LIGHT_COLOR := Color(1, 0, 0)
const LIGHT_RANGE := 1400.0
const LIGHT_INTENSITY := 0.35
# Base albedo is mid-grey, not white. AMB is hardcoded 0.1, so a white base
# plus the specular term saturates the frame before the cluster term is visible.
const BASE_ALBEDO := Color(0.25, 0.25, 0.25)
# Baseline specular/roughness parameters, restored after the SPEC sweep. Also the
# reference point the SPEC steps are compared against.
const BASE_SPEC := Color(0.25, 0.25, 0.25)
const BASE_SHINE := 32.0
const ROUGHNESS := 0.5
const CH_SCALE := Vector2(0.0125, 0.0125)
# grey albedo payload id, needed by the SPEC sweep as well as the gate cells
var tid_grey := -1

var server: GneRenderServer
var camera: Camera3D
var no_light := false
var slot := -1
var sig_file := ""

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--sigf="):
			sig_file = a.split("=")[1]
		elif a == "--nolight":
			no_light = true
		elif a.begins_with("--slot="):
			slot = int(a.split("=")[1])
	_setup()

func _fail(code: int, msg: String) -> void:
	print("GNE 016.cclus: FAIL code=", code, " ", msg)
	get_tree().quit(code)

func _setup() -> void:
	server = GneRenderServer.get_server_singleton()
	if server == null:
		_fail(6000, "server"); return
	if not server.ensure_gpu_device():
		_fail(6001, "device"); return
	if not server.gpu_scene_create(1, 1.0):
		_fail(6002, "scene_create"); return
	if not server.gpu_scene_dispatch(1):
		_fail(6003, "dispatch"); return
	if not server.gpu_mesh_create():
		_fail(6004, "mesh_create"); return
	server.gpu_scene_set_instance_transform(0, PTS[0], SCALES[0])
	server.gpu_scene_set_instance_mesh(0, 0)
	if not server.gpu_mesh_set_batch_strategy(0):
		_fail(6005, "batch"); return
	if not server.gpu_material_create():
		_fail(6006, "material_create"); return
	if not server.gpu_material_set_albedo(0, BASE_ALBEDO):
		_fail(6007, "albedo"); return
	if not server.gpu_material_set_params(0, 0.5, 0.0):
		_fail(6007, "params"); return
	if not server.gpu_material_set_specular(0, Color(0.25, 0.25, 0.25), 32.0):
		_fail(6007, "specular"); return
	# ISOLATION LEVER 1: kill the global directional term. inst0's camera-facing
	# normal is -Z, so a +Z light is perpendicular: ndl = 0 and the NdotL gate
	# zeroes the global specular. Only AMB*alb (neutral) plus the cluster loop
	# can remain.
	if not server.gpu_material_set_light(Vector3(0, 0, 1)):
		_fail(6008, "set_light"); return
	if not no_light:
		var p := {"type": 0, "pos": LIGHT_POS, "range": LIGHT_RANGE,
			"color": LIGHT_COLOR, "intensity": LIGHT_INTENSITY}
		if server.gpu_light_create(p) != 0:
			_fail(6009, "light id"); return
	camera = $Camera
	camera.global_position = CAM_POS
	camera.rotation = Vector3.ZERO
	var vp_size: Vector2 = get_viewport().get_visible_rect().size
	server.gpu_scene_set_viewport(vp_size.x, vp_size.y)
	server.gpu_scene_set_camera(camera.get_global_transform(), camera.get_camera_projection())
	_measure()

func _redraw() -> bool:
	if not server.gpu_cull_dispatch():
		_fail(6010, "cull"); return false
	if not server.gpu_visibility_dispatch():
		_fail(6011, "vis"); return false
	if not server.gpu_mesh_batch_dispatch():
		_fail(6012, "batch"); return false
	if not server.gpu_material_draw_lights():
		_fail(6013, "draw"); return false
	return true

func _measure() -> void:
	# One process evaluates the WHOLE gate. The conditions span different
	# channel configurations, so a per-slot invocation could never test them
	# all and could never own a single exit code.
	# Payloads: NEUTRAL grey for albedo (keeps the chroma split valid),
	# CHECKER for roughness/normal (a flat texture would leave them inert).
	# ids are 0 and 1, and gpu_texture_bind publishes them into tex_array at
	# those positions, which is exactly what the m2 path indexes.
	tid_grey = server.gpu_texture_load("res://assets/neutral_grey.gtex")
	var tid_chk := server.gpu_texture_load("res://checkerboard.gtex")
	if tid_grey < 0 or tid_chk < 0:
		_fail(6020, "payload load"); return
	if not server.gpu_texture_bind(0, 1, tid_grey):
		_fail(6021, "bind grey"); return
	if not server.gpu_texture_bind(0, 2, tid_chk):
		_fail(6021, "bind checker"); return

	# --- BASELINE: every channel slot unset ---
	if not server.gpu_material_set_maps(0, -1, -1, -1, Vector2(0.0125, 0.0125)):
		_fail(6022, "set_maps baseline"); return
	if not _redraw():
		return
	var base_px := server.gpu_raster_read_pixels()
	var vp := server.gpu_scene_get_vp()
	var fc := PTS[0] + Vector3(0, 0, SCALES[0] * 0.5)
	var fpx := _proj_px(fc, vp)
	var base_nrm := server.gpu_raster_read_normal(int(fpx.x), int(fpx.y))

	# --- CELL A: albedo channel, light at full intensity ---
	# --- CELL B: SAME channel, SAME light, intensity forced to 0 via the new
	# gpu_light_set_intensity (a narrow single-field write, so the light keeps
	# its position, range and cluster membership). B is what the 016.cclus B/D
	# cells needed and what no public API could express before: the cluster
	# term is proportional to intensity, so at 0 it must vanish while the
	# neutral AMB*alb term is untouched. chroma(B) == 0 therefore attributes
	# the whole red excess in A to the cluster loop, with no appeal to a
	# missing light or a rebuilt store.
	if not server.gpu_material_set_maps(0, tid_grey, -1, -1, Vector2(0.0125, 0.0125)):
		_fail(6022, "set_maps albedo"); return
	if not _redraw():
		return
	var a := _win(server.gpu_raster_read_pixels(), base_px, fpx)
	# SECOND BASELINE at zero intensity. Comparing cell B against the FIRST
	# baseline would confound the channel effect with the light removal (that
	# mix is what produced the bogus chroma_B=38 on the first attempt). With a
	# baseline that shares B's intensity, B measures the channel alone.
	if not server.gpu_light_set_intensity(0, 0.0):
		_fail(6025, "set_intensity 0"); return
	if not server.gpu_material_set_maps(0, -1, -1, -1, Vector2(0.0125, 0.0125)):
		_fail(6022, "set_maps base0"); return
	if not _redraw():
		return
	var base0_px := server.gpu_raster_read_pixels()
	if not server.gpu_material_set_maps(0, tid_grey, -1, -1, Vector2(0.0125, 0.0125)):
		_fail(6022, "set_maps albedo B"); return
	if not _redraw():
		return
	var b := _win(server.gpu_raster_read_pixels(), base0_px, fpx)
	# Absolute cluster term: the A-minus-B difference at the SAME channel state
	# isolates the cluster emission exactly, with the neutral term cancelling.
	var ct_r: float = a.mr - b.mr
	var ct_g: float = a.mg - b.mg
	var ct_b: float = a.mb - b.mb
	print("GNE 016.cclus: A-B cluster term  dR=%+.3f dG=%+.3f dB=%+.3f" % [ct_r, ct_g, ct_b])
	# restore emission for the controls below
	if not server.gpu_light_set_intensity(0, LIGHT_INTENSITY):
		_fail(6025, "set_intensity restore"); return

	# --- CELL R: roughness channel -> positive control (spatial + pixel change)
	if not server.gpu_material_set_maps(0, -1, tid_chk, -1, Vector2(0.0125, 0.0125)):
		_fail(6022, "set_maps rough"); return
	if not _redraw():
		return
	var r := _win(server.gpu_raster_read_pixels(), base_px, fpx)

	# --- CELL N: normal channel -> positive control (readback perturbation)
	if not server.gpu_material_set_maps(0, -1, -1, tid_chk, Vector2(0.0125, 0.0125)):
		_fail(6022, "set_maps normal"); return
	if not _redraw():
		return
	var nrm := server.gpu_raster_read_normal(int(fpx.x), int(fpx.y))
	var n_delta := _nrm_delta(base_nrm, nrm)
	var n_px := server.gpu_raster_read_pixels()
	var n := _win(n_px, base_px, fpx)

	# --- GNE-017 follow-up: is the non-scaling cluster share SPECULAR? ---
	# At metal=0, sc = spec_col, so the cluster term splits as
	#   C_off = S + D        (no channel:  alb full)
	#   C_on  = S + D/2      (grey channel halves alb, so only diffuse scales)
	#   S     = 2*C_on - C_off
	# S is therefore the part of the cluster term that does NOT track alb. The
	# question is whether S is the specular term. Four steps, each independent.
	# gpu_material_set_specular and gpu_material_set_params already exist, so
	# no engine change is required.
	var step_ids: Array = []
	# step 1 (decisive): black spec_col must annihilate S if S is specular
	step_ids.append(["spec_black", 0.0, Color(0, 0, 0), BASE_SHINE])
	# step 2: S must scale linearly with spec_col
	step_ids.append(["spec_0125", 0.0, Color(0.125, 0.125, 0.125), BASE_SHINE])
	step_ids.append(["spec_0500", 0.0, Color(0.5, 0.5, 0.5), BASE_SHINE])
	# step 3: identity control - S must MOVE with shininess (isolates s2)
	step_ids.append(["shine_004", 0.0, BASE_SPEC, 4.0])
	step_ids.append(["shine_128", 0.0, BASE_SPEC, 128.0])
	# step 4: metal=1 makes sc = alb, so everything must scale and S -> 0
	step_ids.append(["metal_100", 1.0, BASE_SPEC, BASE_SHINE])
	for st in step_ids:
		var res: Dictionary = _cluster_pair(float(st[1]), st[2], float(st[3]), fpx)
		if res.is_empty():
			return
		var c_off: Array = res["C_off"]
		var c_on: Array = res["C_on"]
		var t_off: Array = res["T_off"]
		var b_off: Array = res["B_off"]
		var s_r: float = 2.0 * float(c_on[0]) - float(c_off[0])
		var s_g: float = 2.0 * float(c_on[1]) - float(c_off[1])
		var s_b: float = 2.0 * float(c_on[2]) - float(c_off[2])
		print("GNE 016.cclus: SPEC %s T_off=(%.1f,%.1f,%.1f) B_off=(%.1f,%.1f,%.1f) C_off=(%+.1f,%+.1f,%+.1f) C_on=(%+.1f,%+.1f,%+.1f) S=(%+.3f,%+.3f,%+.3f)" % [st[0], t_off[0], t_off[1], t_off[2], b_off[0], b_off[1], b_off[2], c_off[0], c_off[1], c_off[2], c_on[0], c_on[1], c_on[2], s_r, s_g, s_b])
	# restore the gate configuration
	if not server.gpu_material_set_params(0, ROUGHNESS, 0.0):
		_fail(6026, "restore params"); return
	if not server.gpu_material_set_specular(0, BASE_SPEC, BASE_SHINE):
		_fail(6026, "restore specular"); return
	if not server.gpu_light_set_intensity(0, LIGHT_INTENSITY):
		_fail(6025, "restore intensity"); return

	# --- EVALUATE. Thresholds pre-declared by owner 2026-09-29. ---
	var dR: float = a.mr - a.base_mr
	var dG: float = a.mg - a.base_mg
	var dB: float = a.mb - a.base_mb
	var bR: float = b.mr - b.base_mr
	var bG: float = b.mg - b.base_mg
	# ct_r / ct_g / ct_b were already computed at the A-B measurement above; do
	# not redeclare them here.
	var neutral_split: float = absf(dG - dB)
	var chroma: float = absf(dR - dG)
	# Silence control, now correctly formed: with NO cluster light the red
	# excess must be gone, so the channel's own response must be neutral.
	var chroma_b: float = absf(bR - bG)
	var c_body: bool = (a.body == 81)
	var c_split: bool = (neutral_split <= 2.0)
	var c_chroma: bool = (chroma >= 2.55)
	var c_silence: bool = (chroma_b <= 0.5)
	# The cluster term itself must be red-only and above the same 1% floor.
	var c_term: bool = (ct_r >= 2.55 and absf(ct_g) <= 0.5 and absf(ct_b) <= 0.5)
	var c_rough: bool = (r.sd_r > 1.0 and r.max_lsb > 0)
	var c_norm: bool = (n_delta > 0.1)
	print("GNE 016.cclus: A dR=%.3f dG=%.3f dB=%.3f neutral_split=%.3f chroma=%.3f body=%d" % [dR, dG, dB, neutral_split, chroma, a.body])
	print("GNE 016.cclus: A-B cluster term dR=%+.3f dG=%+.3f dB=%+.3f | chroma_B=%.3f" % [ct_r, ct_g, ct_b, chroma_b])
	print("GNE 016.cclus: R sd_r=%.3f max_lsb=%d | N nrm_delta=%.6f max_lsb=%d" % [r.sd_r, r.max_lsb, n_delta, n.max_lsb])
	print("GNE 016.cclus: cond body=%s neutral_split=%s chroma=%s silence=%s term=%s rough_ctl=%s normal_ctl=%s" % [str(c_body), str(c_split), str(c_chroma), str(c_silence), str(c_term), str(c_rough), str(c_norm)])
	var sig := "v166cclus|body=%d|split=%.3f|chroma=%.3f|chromaB=%.3f|ctR=%.3f|ctG=%.3f|rsd=%.3f|rl=%d|nd=%.6f|%d%d%d%d%d%d%d" % [a.body, neutral_split, chroma, chroma_b, ct_r, ct_g, r.sd_r, r.max_lsb, n_delta, int(c_body), int(c_split), int(c_chroma), int(c_silence), int(c_term), int(c_rough), int(c_norm)]
	print("GNE 016.cclus: sig=", sig)
	if sig_file != "":
		var f := FileAccess.open(sig_file, FileAccess.WRITE)
		if f == null:
			_fail(6024, "sig file"); return
		f.store_string(sig + "\n")
	server.gpu_scene_destroy()
	if c_body and c_split and c_chroma and c_silence and c_term and c_rough and c_norm:
		print("GNE 016.cclus: GATE PASS")
		get_tree().quit(0)
	else:
		print("GNE 016.cclus: GATE FAIL body=", c_body, " split=", c_split,
				" chroma=", c_chroma, " silence=", c_silence, " term=", c_term,
				" rough=", c_rough, " normal=", c_norm)
		get_tree().quit(61)

# Per-window per-channel means over surface pixels only.
func _means(px: PackedByteArray, fpx: Vector2) -> Array:
	var sum := [0.0, 0.0, 0.0]
	var n := 0
	for dy in range(-4, 5):
		for dx in range(-4, 5):
			var o: int = ((int(fpx.y) + dy) * RASTER_W + (int(fpx.x) + dx)) * 4
			if px[o + 3] > 0:
				n += 1
				sum[0] += px[o]
				sum[1] += px[o + 1]
				sum[2] += px[o + 2]
	if n == 0:
		return [0.0, 0.0, 0.0]
	return [sum[0] / float(n), sum[1] / float(n), sum[2] / float(n)]

# Measures the ABSOLUTE cluster term for one material configuration.
# C_off / C_on differ only in the light intensity (LIGHT_INTENSITY vs 0), so
# the neutral AMB*alb term cancels exactly and what remains is the cluster
# emission. T_* is the lit frame, B_* the same frame with the light silenced.
func _cluster_pair(p_metal: float, p_spec: Color, p_shine: float, p_fpx: Vector2) -> Dictionary:
	if not server.gpu_material_set_params(0, ROUGHNESS, p_metal):
		_fail(6026, "spec params"); return {}
	if not server.gpu_material_set_specular(0, p_spec, p_shine):
		_fail(6026, "spec color/shine"); return {}
	var t_off: Array = []
	var b_off: Array = []
	var t_on: Array = []
	var b_on: Array = []
	# --- channel OFF ---
	if not server.gpu_material_set_maps(0, -1, -1, -1, CH_SCALE):
		_fail(6022, "spec maps off"); return {}
	if not server.gpu_light_set_intensity(0, LIGHT_INTENSITY):
		_fail(6025, "spec int on"); return {}
	if not _redraw():
		return {}
	t_off = _means(server.gpu_raster_read_pixels(), p_fpx)
	if not server.gpu_light_set_intensity(0, 0.0):
		_fail(6025, "spec int off"); return {}
	if not _redraw():
		return {}
	b_off = _means(server.gpu_raster_read_pixels(), p_fpx)
	# --- channel ON (neutral grey) ---
	if not server.gpu_material_set_maps(0, tid_grey, -1, -1, CH_SCALE):
		_fail(6022, "spec maps on"); return {}
	if not server.gpu_light_set_intensity(0, LIGHT_INTENSITY):
		_fail(6025, "spec int on2"); return {}
	if not _redraw():
		return {}
	t_on = _means(server.gpu_raster_read_pixels(), p_fpx)
	if not server.gpu_light_set_intensity(0, 0.0):
		_fail(6025, "spec int off2"); return {}
	if not _redraw():
		return {}
	b_on = _means(server.gpu_raster_read_pixels(), p_fpx)
	var c_off := [t_off[0] - b_off[0], t_off[1] - b_off[1], t_off[2] - b_off[2]]
	var c_on := [t_on[0] - b_on[0], t_on[1] - b_on[1], t_on[2] - b_on[2]]
	return {"T_off": t_off, "B_off": b_off, "T_on": t_on, "B_on": b_on, "C_off": c_off, "C_on": c_on}

# Per-window statistics against the baseline capture.
func _win(px: PackedByteArray, base_px: PackedByteArray, fpx: Vector2) -> Dictionary:
	var body := 0
	var sum := [0.0, 0.0, 0.0]
	var max_lsb := 0
	for dy in range(-4, 5):
		for dx in range(-4, 5):
			var o: int = ((int(fpx.y) + dy) * RASTER_W + (int(fpx.x) + dx)) * 4
			if px[o + 3] > 0:
				body += 1
				sum[0] += px[o]
				sum[1] += px[o + 1]
				sum[2] += px[o + 2]
			for c in 4:
				var d: int = absi(int(px[o + c]) - int(base_px[o + c]))
				if d > max_lsb:
					max_lsb = d
	if body == 0:
		return {"body": 0, "mr": 0.0, "mg": 0.0, "mb": 0.0, "sd_r": 0.0, "max_lsb": 0, "base_mr": 0.0, "base_mg": 0.0, "base_mb": 0.0}
	var mr: float = sum[0] / float(body)
	var mg: float = sum[1] / float(body)
	var mb: float = sum[2] / float(body)
	var vs: float = 0.0
	var bs := [0.0, 0.0, 0.0]
	for dy in range(-4, 5):
		for dx in range(-4, 5):
			var o: int = ((int(fpx.y) + dy) * RASTER_W + (int(fpx.x) + dx)) * 4
			if base_px[o + 3] > 0:
				bs[0] += base_px[o]
				bs[1] += base_px[o + 1]
				bs[2] += base_px[o + 2]
				var d: float = float(px[o]) - mr
				vs += d * d
	var bmr: float = bs[0] / float(body)
	var bmg: float = bs[1] / float(body)
	var bmb: float = bs[2] / float(body)
	return {"body": body, "mr": mr, "mg": mg, "mb": mb, "sd_r": sqrt(vs / float(body)),
		"max_lsb": max_lsb, "base_mr": bmr, "base_mg": bmg, "base_mb": bmb}

func _nrm_delta(a: PackedFloat32Array, b: PackedFloat32Array) -> float:
	if a.size() < 3 or b.size() < 3:
		return -1.0
	return maxf(absf(b[0] - a[0]), maxf(absf(b[1] - a[1]), absf(b[2] - a[2])))

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
