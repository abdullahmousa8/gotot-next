extends "res://main_022_m1.gd"

# GNE-022 section 11-M2 - CRITERION (b-field) GATE SCENE.
# Spec: docs/spec_022_gi_scoping.md section 11-M2 (owner-directed 2026-09-30).
#
# This scene IS the M1 instrument plus the registered (b-field) criterion. It
# inherits main_022_m1.gd and does not duplicate a line of its setup: the whole
# sequence, the readbacks and the report are the parent's. What this file adds is
# the evaluation, printed as explicit M2: lines, and one extra integrity
# condition so the exit code can gate.
#
# (b-field), as registered in the spec:
#   for f(k), k = 1..160, sampled every 10 frames on the section-11 near probe,
#   every successive interval-delta ratio r_j = d_j / d_(j-1), j = 2..15,
#   satisfies r_j <= 0.6; the series is finite at every sample; the in-process
#   double run is byte-equal; the run emits no ERROR or RID-leak lines.
# The 0.6 is INHERITED from the frozen section-11 criterion (b) - M2 changes only
# the observable (float field instead of the tonemapped 8-bit image).
#
# Section 11 itself is NOT touched: its criterion text, gain, window and its
# official state (FAIL, criterion b) are preserved verbatim by the R1 closure
# wording, and main_022_loop.gd stays frozen. Whether (b-field) retires criterion
# (b) as a gate and whether section 11 may then be closed are separate owner
# decisions, explicitly NOT taken here.

const EPS_FINE_M2 := 1e-5
const RULE_M2 := 0.6

var m2_crit_ok := false

func _report(r1: Array, r2: Array, det: bool) -> void:
	# Parent report first: it prints the instrument table, the ratio series, the
	# endpoints, the probe set and the cost - identical to a plain M1 run.
	super._report(r1, r2, det)
	# Recompute the ratio series from the raw float series so the criterion is
	# evaluated on the same numbers the table shows (not on parsed strings).
	var fvals: Array = r1[3]
	var n := fvals.size()
	var d10: Array = []
	for j in range(1, n / SAMPLE_EVERY):
		d10.append(float(fvals[(j + 1) * SAMPLE_EVERY - 1]) - float(fvals[j * SAMPLE_EVERY - 1]))
	var ratios: Array = []
	for j in range(1, d10.size()):
		ratios.append(float(d10[j]) / maxf(float(d10[j - 1]), 1e-12))
	# ---- (b-field) verdict ----
	var rmin := 1e30
	var rmax := -1e30
	var gm := 1.0
	var below := 0
	var all_le := true
	for r in ratios:
		var x := float(r)
		if x < rmin:
			rmin = x
		if x > rmax:
			rmax = x
		gm *= maxf(x, 1e-12)
		if x <= RULE_M2:
			below += 1
		else:
			all_le = false
	gm = pow(gm, 1.0 / float(ratios.size()))
	var finite_ok := bool(r1[5])
	var count_ok := int(r1[4]) == FRAMES
	var e5 := -1
	for i in range(1, n):
		var ad: float = absf(float(fvals[i]) - float(fvals[i - 1]))
		if e5 < 0 and ad < EPS_FINE_M2:
			e5 = i + 1
	print("M2: --- criterion (b-field), registered spec section 11-M2 ---")
	print("M2: rule=every interval ratio <= 0.6 (inherited from frozen section-11 criterion b)")
	print("M2: ratio10_min=%.6f ratio10_max=%.6f ratio10_geomean=%.6f" % [rmin, rmax, gm])
	print("M2: intervals_below_rule=%d/%d" % [below, ratios.size()])
	print("M2: finite=%s count_ok=%s determinism_byte_equal=%s" % [finite_ok, count_ok, det])
	print("M2: endpoint_delta_lt_1e-5_frame=%d (reported, not gated)" % e5)
	m2_crit_ok = all_le and finite_ok and count_ok and det and ratios.size() == FRAMES / SAMPLE_EVERY - 2
	print("M2: criterion_b_field=", "PASS" if m2_crit_ok else "FAIL")
	print("M2: section11_unchanged: official state FAIL (criterion b) under the R1 closure wording; main_022_loop.gd frozen and unedited; M1 stays measurement-only")
	var sig := "v022m2|frames=%d|r10min=%.6f|r10max=%.6f|r10gm=%.6f|below=%d/%d|e5=%d|det=%d|d1" % [
		n, rmin, rmax, gm, below, ratios.size(), e5, 1 if det else 0]
	print("M2: sig=", sig)
	if sig_file != "":
		# Overwrites the parent's M1 sig on purpose: this scene's deliverable is
		# the (b-field) signature, and the M1 numbers are already in this log.
		var fl := FileAccess.open(sig_file, FileAccess.WRITE)
		if fl != null:
			fl.store_string(sig + "\n")

func _final_ok(p_r1: Array, p_det: bool) -> bool:
	# Instrument integrity AND the registered criterion - so the process exit code
	# is the gate, and gt_022m2.bat can require rc=0.
	return super._final_ok(p_r1, p_det) and m2_crit_ok
