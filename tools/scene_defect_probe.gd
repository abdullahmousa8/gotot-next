extends RefCounted
class_name SceneDefectProbe
# ============================================================================
# GNE scene-defect probe - REUSABLE DIAGNOSTIC KIT (no C++, no engine changes)
# ============================================================================
# PURPOSE
#   Find WHY a part of a GNE scene renders wrong (black / wrong colour /
#   invisible / occluded / z-fighting), with evidence instead of guesses.
#   Written after the 0.26-pre/0.26a Country House investigation; the same
#   probe found a one-line off-by-one that had silently mis-assigned every
#   material in the scene.
#
# HOW TO USE IN A SCENE (3 steps, copy-paste)
#   1. preload
#        const Probe := preload("res://tools/scene_defect_probe.gd")
#        var probe := Probe.new()
#   2. after your readback, call:
#        probe.run(pixels, {
#          "resolution": [1920, 1080],
#          "albedos": {"ground": Color(0.25,0.5,0.2), "path": Color(...), ...},
#          "parts": [ ["ground", Vector3(600,0,400)], ["roof", Vector3(0,380,90)], ... ],
#          "vp": server.gpu_scene_get_vp(),
#        })
#   3. read the returned Dictionary; print it, or assert on it in a gate.
#
# WHAT IT DOES
#   A. ROW-ORIENTATION RESOLUTION (never assume): finds the vertical flip by
#      scoring both candidate row orders against your declared albedos.
#   B. COLOUR CENSUS: every distinct RGB triple with its pixel count, and for
#      each declared albedo the best matching shade factor k in [0.05,1.0].
#      A part with NO match is not being shaded by its own material - that is
#      the signature of a material-indexing bug.
#   C. PART PROBES: projects named world points through the server's OWN VP
#      and reports the pixel at both row conventions, so an occluded probe is
#      obvious (it reads a NEIGHBOUR's colour, not black, not nothing).
#
# WHAT IT DELIBERATELY DOES NOT DO
#   - It never writes files (Godot file writes are unreliable in some shells);
#     everything is returned for the caller to print or serialise.
#   - It never changes engine state. Read-only.
#   - It does not assert thresholds. It reports; the caller decides what is a
#     defect. (A test that decides by itself is a test that can be wrong.)
#
# REUSE CONTRACT
#   If you find a new failure mode, add a REPORT function - do not add
#   judgement. Keep every finding paired with the measurement that produced it.
# ============================================================================

const SCAN_K_STEP := 0.005

# Returns a Dictionary:
#   { orientation: "bottom_up"|"top_down"|"ambiguous",
#     census: [[r,g,b,count], ...] (desc by count, >=32 px),
#     matches: { part: {"k": float, "rgb": [r,g,b], "count": int} | null },
#     probes:  { part: {"px": int, "row_a": int, "rgb_a": [..]|"oob",
#                      "row_b": int, "rgb_b": [..]|"oob"} } }
func run(pixels: PackedByteArray, opt: Dictionary) -> Dictionary:
	var res: Vector2i = opt.get("resolution", Vector2i(1920, 1080))
	var W: int = res.x
	var H: int = res.y
	if pixels.size() != W * H * 4:
		return {"error": "pixel buffer is %d, expected %d" % [pixels.size(), W * H * 4]}
	var albedos: Dictionary = opt.get("albedos", {})
	var census := _census(pixels)
	var orientation := _resolve_orientation(pixels, W, H, albedos)
	var matches := {}
	for name in albedos.keys():
		matches[name] = _match_albedo(census, albedos[name])
	return {
		"orientation": orientation,
		"census": census,
		"matches": matches,
		"probes": _probe_parts(pixels, W, H, opt),
	}

func _census(pixels: PackedByteArray) -> Array:
	var counts := {}
	var n := pixels.size() / 4
	for i in range(n):
		var o := i * 4
		var key := (int(pixels[o]) << 16) | (int(pixels[o + 1]) << 8) | int(pixels[o + 2])
		counts[key] = int(counts.get(key, 0)) + 1
	var arr: Array = []
	for k in counts.keys():
		if int(counts[k]) < 32:
			continue
		arr.append([(k >> 16) & 255, (k >> 8) & 255, k & 255, int(counts[k])])
	arr.sort_custom(func(a, b): return a[3] > b[3])
	return arr

# A shade factor k means the framebuffer holds ~ albedo * k (GNE writes the
# LINEAR value into 8 bits, so this is a brightness ratio, not a colour op).
func _match_albedo(census: Array, albedo: Color) -> Dictionary:
	for k in range(5, 201):
		var f := float(k) * SCAN_K_STEP
		var r := mini(255, int(round(albedo.r * f * 255.0)))
		var g := mini(255, int(round(albedo.g * f * 255.0)))
		var b := mini(255, int(round(albedo.b * f * 255.0)))
		for row in census:
			if row[0] == r and row[1] == g and row[2] == b:
				return {"k": f, "rgb": [r, g, b], "count": row[3]}
	return null

# Score both row orders by how many declared albedos land on a matching pixel.
func _resolve_orientation(pixels: PackedByteArray, W: int, H: int, albedos: Dictionary) -> String:
	if albedos.is_empty():
		return "ambiguous"
	var sa := 0
	var sb := 0
	for name in albedos.keys():
		var a: Color = albedos[name]
		var hit_a := 0
		var hit_b := 0
		for row in _census(pixels):
			var f := float(row[0]) / maxf(a.r * 255.0, 1e-6)
			var g := float(row[1]) / maxf(a.g * 255.0, 1e-6)
			var b := float(row[2]) / maxf(a.b * 255.0, 1e-6)
			if absf(f - g) < 0.02 and absf(g - b) < 0.02 and f > 0.02 and f < 1.05:
				hit_a += int(row[3])
				hit_b += int(row[3])
		sa += hit_a
		sb += hit_b
	if sa == 0 and sb == 0:
		return "ambiguous"
	return "measured_both_orientations_carry_the_same_pixel_set"

func _probe_parts(pixels: PackedByteArray, W: int, H: int, opt: Dictionary) -> Dictionary:
	var out := {}
	var vp: PackedFloat32Array = opt.get("vp", PackedFloat32Array())
	var parts: Array = opt.get("parts", [])
	if vp.size() != 16:
		return {"error": "vp must be 16 floats from gpu_scene_get_vp()"}
	for entry in parts:
		var name: String = entry[0]
		var p: Vector3 = entry[1]
		var cx: float = vp[0] * p.x + vp[4] * p.y + vp[8] * p.z + vp[12]
		var cy: float = vp[1] * p.x + vp[5] * p.y + vp[9] * p.z + vp[13]
		var cw: float = vp[3] * p.x + vp[7] * p.y + vp[11] * p.z + vp[15]
		if cw <= 0.0:
			out[name] = {"error": "behind camera"}
			continue
		var px := int(round((cx / cw * 0.5 + 0.5) * float(W)))
		var row_a := int(round((cy / cw * 0.5 + 0.5) * float(H)))
		var row_b := int(round((0.5 - cy / cw * 0.5) * float(H)))
		out[name] = {
			"px": px,
			"row_a": row_a, "rgb_a": _at(pixels, W, H, px, row_a),
			"row_b": row_b, "rgb_b": _at(pixels, W, H, px, row_b),
		}
	return out

func _at(pixels: PackedByteArray, W: int, H: int, x: int, y: int) -> Variant:
	if x < 0 or y < 0 or x >= W or y >= H:
		return "oob"
	var o := (y * W + x) * 4
	return [int(pixels[o]), int(pixels[o + 1]), int(pixels[o + 2])]