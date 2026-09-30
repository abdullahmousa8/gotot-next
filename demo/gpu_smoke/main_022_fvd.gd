extends "res://main_022_m1.gd"

# GNE-022 - FIELD vs DISPLAYED RADIANCE (Convergence Delta). Owner-directed
# 2026-10-01. This unit links the two instruments the project already trusts:
#   - the float FIELD, read with gpu_gi_read_avg() (float32 since 11-R1), on the
#     section-11 near probe, every frame (inherited from the M1 instrument);
#   - the DISPLAYED radiance, read with gpu_raster_read_hdr(x, y) (pre-clamp,
#     KI-017), at the projection of the SAME witness face.
# Purpose: catch a future drift between the GI field and what the raster pass
# actually shows, by putting both convergence rates on one table.
#
# Read cost, measured rather than assumed: gpu_raster_read_hdr() fetches the WHOLE
# 1920x1080 RGBA32F attachment (~33 MB) per call, so the displayed series is
# sampled on the section-11 cadence (every 10 frames = 16 samples per pass), while
# the field series stays per-frame. Sampling it per frame would move ~10 GB.
#
# Units, stated so nothing is over-read:
#   - ratio_f and ratio_L are DIMENSIONLESS interval-decay rates; comparing them
#     is the point of this unit, and their difference is the Convergence Delta.
#   - the gain L/f is dimensionless but INSTRUMENT-specific (the field is
#     irradiance-like, the pixel is radiance after the shading pass), so it is
#     reported as an instrument ratio, never as a physical equivalence.
# No threshold is introduced here: this unit reports, exactly like M1 did. Turning
# it into a gate would be a separate registered decision.

const RASTER_W_FVD := 1920
const RASTER_H_FVD := 1080

var fvd_passes: Array = []
var fvd_pass: Dictionary = {}
var fvd_near_px := Vector2(-1, -1)
var fvd_ok := false

func _pre_sequence() -> void:
	if fvd_near_px.x < 0.0:
		fvd_near_px = _project_witness(NEAR_C + Vector3(0, 0, 5.0))
	# M1 blacks every albedo (field-only instrument), so the raster would be
	# identically zero. Mirror main_022b.gd lines 55-60: neutral 0.7 grey for
	# all instances so the displayed series observes a lit city. The GI field
	# uses its own albedo uniform (0.35), so this does not move the field
	# series - verified byte-equal against bare M1 in the FVD runs.
	for mi in range(56):
		server.gpu_material_set_albedo(mi, Color(0.7, 0.7, 0.7))
	fvd_pass = {"k": [], "Lr": [], "Llum": []}
	fvd_passes.append(fvd_pass)

func _on_sample(p_k: int) -> void:
	if fvd_near_px.x < 0.0:
		return
	# The M1 instrument never rasterizes (GI trace/accum only), so the raster
	# framebuffer is still clear at sample time. Redraw the static scene through
	# the standard _redraw-equivalent sequence before reading the pixel.
	if not server.gpu_cull_dispatch():
		return
	if not server.gpu_visibility_dispatch():
		return
	if not server.gpu_mesh_batch_dispatch():
		return
	if not server.gpu_material_draw_lights():
		return
	var h: PackedFloat32Array = server.gpu_raster_read_hdr(int(fvd_near_px.x), int(fvd_near_px.y))
	if h.size() < 3:
		return
	fvd_pass["k"].append(p_k)
	fvd_pass["Lr"].append(float(h[0]))
	fvd_pass["Llum"].append((float(h[0]) + float(h[1]) + float(h[2])) / 3.0)

func _project_witness(p: Vector3) -> Vector2:
	var vp: PackedFloat32Array = server.gpu_scene_get_vp()
	if vp.size() != 16:
		return Vector2(-1, -1)
	var cx: float = vp[0] * p.x + vp[4] * p.y + vp[8] * p.z + vp[12]
	var cy: float = vp[1] * p.x + vp[5] * p.y + vp[9] * p.z + vp[13]
	var cw: float = vp[3] * p.x + vp[7] * p.y + vp[11] * p.z + vp[15]
	if cw <= 0.0:
		return Vector2(-1, -1)
	var nx := cx / cw
	var ny := cy / cw
	var px := (nx * 0.5 + 0.5) * float(RASTER_W_FVD)
	# Same dual-convention selection as the frozen section-11 scene
	# (main_022_loop._proj/_first_in_bounds): the readback row order is an
	# empirical property, so both candidates are tested, never assumed.
	for py in [(ny * 0.5 + 0.5) * float(RASTER_H_FVD), (0.5 - ny * 0.5) * float(RASTER_H_FVD)]:
		if px >= 4.0 and py >= 4.0 and px < float(RASTER_W_FVD - 4) and py < float(RASTER_H_FVD - 4):
			return Vector2(floor(px), floor(py))
	return Vector2(-1, -1)

func _ratios_of(series: Array) -> Array:
	var r: Array = []
	var d: Array = []
	for i in range(1, series.size()):
		d.append(float(series[i]) - float(series[i - 1]))
	for i in range(1, d.size()):
		r.append(float(d[i]) / maxf(float(d[i - 1]), 1e-12))
	return r

func _report(r1: Array, r2: Array, det: bool) -> void:
	super._report(r1, r2, det)
	var fvals: Array = r1[3]
	var n := fvals.size()
	var fs: Array = []
	for j in range(SAMPLE_EVERY, n + 1, SAMPLE_EVERY):
		fs.append(float(fvals[j - 1]))
	var rf := _ratios_of(fs)
	var p1: Dictionary = fvd_passes[0] if fvd_passes.size() > 0 else {}
	var Lr: Array = p1.get("Lr", [])
	var Ll: Array = p1.get("Llum", [])
	var rL := _ratios_of(Lr)
	print("FVD: --- field vs displayed radiance at the same witness ---")
	print("FVD: witness_px=", int(fvd_near_px.x), ",", int(fvd_near_px.y), " field_samples=", fs.size(), " displayed_samples=", Lr.size())
	var finite := true
	for v in Lr:
		if is_nan(float(v)) or is_inf(float(v)):
			finite = false
	# table on the section-11 cadence
	print("FVD: k  field_f        displayed_L    ratio_f   ratio_L   delta")
	var cmp_count := 0
	var dmax := 0.0
	var dsum := 0.0
	var dabs: Array = []
	for i in range(fs.size()):
		var kk := (i + 1) * SAMPLE_EVERY
		var rfv := 0.0
		var rlv := 0.0
		var dv := 0.0
		if i >= 2 and (i - 2) < rf.size() and (i - 2) < rL.size():
			rfv = float(rf[i - 2])
			rlv = float(rL[i - 2])
			dv = rlv - rfv
			cmp_count += 1
			dsum += absf(dv)
			dabs.append(absf(dv))
			if absf(dv) > dmax:
				dmax = absf(dv)
		var Lv := 0.0
		if i < Lr.size():
			Lv = float(Lr[i])
		print("FVD: %d  %.9f  %.9f  %.6f  %.6f  %+.6f" % [kk, float(fs[i]), Lv, rfv, rlv, dv])
	var dmean := dsum / maxf(float(cmp_count), 1.0)
	print("FVD: convergence_delta mean_abs=%.6f max_abs=%.6f over %d intervals" % [dmean, dmax, cmp_count])
	print("FVD: field_ratio  min=%.6f max=%.6f" % [_min_of(rf), _max_of(rf)])
	print("FVD: display_ratio min=%.6f max=%.6f" % [_min_of(rL), _max_of(rL)])
	if fs.size() > 0 and Lr.size() > 0:
		var f_last := float(fs[fs.size() - 1])
		var l_last := float(Lr[Lr.size() - 1])
		print("FVD: last field=%.9f last displayed=%.9f instrument_ratio_L_over_f=%.6f (dimensionless, instrument-specific)" % [
			f_last, l_last, l_last / maxf(f_last, 1e-12)])
	print("FVD: displayed_finite=", finite, " lum_first=%.9f lum_last=%.9f" % [
		float(Ll[0]) if Ll.size() > 0 else 0.0, float(Ll[Ll.size() - 1]) if Ll.size() > 0 else 0.0])
	# determinism of the DISPLAYED series across the two in-process passes
	var fvd_det := fvd_passes.size() == 2 and _series_key(fvd_passes[0]) == _series_key(fvd_passes[1])
	print("FVD: displayed_determinism_byte_equal=", fvd_det)
	print("FVD: field_determinism_byte_equal=", det)
	print("FVD: note=field is float irradiance-like, displayed is pre-clamp radiance; ratios are the comparable quantity")
	fvd_ok = finite and Lr.size() == FRAMES / SAMPLE_EVERY and fvd_det and det
	print("FVD: instrument_integrity=", "PASS" if fvd_ok else "FAIL")
	var sig := "v022fvd|samples=%d|rLmin=%.6f|rLmax=%.6f|rfmin=%.6f|rfmax=%.6f|dmean=%.6f|dmax=%.6f|det=%d|d1" % [
		Lr.size(), _min_of(rL), _max_of(rL), _min_of(rf), _max_of(rf), dmean, dmax,
		(1 if (fvd_det and det) else 0)]
	print("FVD: sig=", sig)
	if sig_file != "":
		var fl := FileAccess.open(sig_file, FileAccess.WRITE)
		if fl != null:
			fl.store_string(sig + "\n")

func _series_key(p: Dictionary) -> String:
	var s := ""
	var ks: Array = p.get("k", [])
	var ls: Array = p.get("Lr", [])
	var ms: Array = p.get("Llum", [])
	for i in range(ls.size()):
		s += "%d:%.9f:%.9f;" % [int(ks[i]), float(ls[i]), float(ms[i])]
	return s

func _min_of(a: Array) -> float:
	if a.is_empty():
		return 0.0
	var m := float(a[0])
	for v in a:
		if float(v) < m:
			m = float(v)
	return m

func _max_of(a: Array) -> float:
	if a.is_empty():
		return 0.0
	var m := float(a[0])
	for v in a:
		if float(v) > m:
			m = float(v)
	return m

func _final_ok(p_r1: Array, p_det: bool) -> bool:
	return super._final_ok(p_r1, p_det) and fvd_ok
