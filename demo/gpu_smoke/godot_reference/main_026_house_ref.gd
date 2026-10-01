extends Node

# GNE A/B Isolation - vanilla Godot twin of the 0.26-pre Country House.
# Same 9 parts (baked numbers duplicated from main_026_house.gd - the unlit
# MAE itself validates the transcription), StandardMaterial3D albedo/rough/
# metal matched, NO lights (sun energy 0), ambient COLOR white x 0.1, no sky,
# tonemap LINEAR, glow off, AA off. Camera identical to GNE.
# Capture: full-res viewport hex dump to STDOUT (this shell cannot rely on
# Godot file writes) - one line per row, 6 hex chars per pixel (RRGGBB).

# Phase-2 protocol flag --direct: sun ON at the canon outdoor direction
# (normalized(-0.5,-1,-0.5) rotated to the above-front used by the GNE house,
# i.e. (0.5,1,0.5)) and ambient ZERO (Step-2 direct-only twin capture).
const DIRECT_LIGHT := Vector3(0.5, 1.0, 0.5)

const PARTS := [
	[[["quad", [-1000, 0, -800], [1000, 0, -800], [1000, 0, 800], [-1000, 0, 800]]], [0.25, 0.5, 0.2], 0.9],
	[[["quad", [-80, 0.6, 200], [80, 0.6, 200], [80, 0.6, 900], [-80, 0.6, 900]]], [0.75, 0.65, 0.45], 0.9],
	[[["box", [-210, 0, -270], [210, 260, 70]]], [0.85, 0.75, 0.6], 0.8],
	[[["quad", [0, 380, -290], [0, 380, 90], [-260, 250, 90], [-260, 250, -290]]], [0.55, 0.12, 0.1], 0.7],
	[[["quad", [0, 380, -290], [260, 250, -290], [260, 250, 90], [0, 380, 90]]], [0.55, 0.12, 0.1], 0.7],
	[[["tri", [-210, 260, 70], [210, 260, 70], [0, 380, 70]]], [0.8, 0.7, 0.55], 0.8],
	[[["tri", [210, 260, -270], [-210, 260, -270], [0, 380, -270]]], [0.8, 0.7, 0.55], 0.8],
	[[["box", [-250, 0, -460], [250, 300, -420]]], [0.5, 0.5, 0.52], 0.9],
	[[["box", [95, 300, -185], [145, 440, -135]]], [0.5, 0.2, 0.15], 0.8],
]

var frame := 0

func _v(a: Array) -> Vector3:
	return Vector3(a[0], a[1], a[2])

func _tri(st: SurfaceTool, a: Vector3, b: Vector3, c: Vector3) -> void:
	st.add_vertex(a); st.add_vertex(b); st.add_vertex(c)

func _quad(st: SurfaceTool, a: Vector3, b: Vector3, c: Vector3, d: Vector3) -> void:
	_tri(st, a, b, c); _tri(st, a, c, d)

func _box(st: SurfaceTool, mn: Vector3, mx: Vector3) -> void:
	var c := [mn, Vector3(mx.x, mn.y, mn.z), Vector3(mx.x, mn.y, mx.z), Vector3(mn.x, mn.y, mx.z),
		Vector3(mn.x, mx.y, mn.z), Vector3(mx.x, mx.y, mn.z), mx, Vector3(mn.x, mx.y, mx.z)]
	var faces := [[0, 1, 2, 3], [4, 6, 5, 7], [0, 4, 5, 1], [2, 6, 7, 3], [1, 5, 6, 2], [0, 3, 7, 4]]
	for f in faces:
		_quad(st, c[f[0]], c[f[1]], c[f[2]], c[f[3]])

func _ready() -> void:
	$Camera.global_position = Vector3(0, 300, 1500)
	$Camera.rotation = Vector3.ZERO
	for part in PARTS:
		var st := SurfaceTool.new()
		st.begin(Mesh.PRIMITIVE_TRIANGLES)
		for prim in part[0]:
			if prim[0] == "box":
				_box(st, _v(prim[1]), _v(prim[2]))
			elif prim[0] == "quad":
				_quad(st, _v(prim[1]), _v(prim[2]), _v(prim[3]), _v(prim[4]))
			elif prim[0] == "tri":
				_tri(st, _v(prim[1]), _v(prim[2]), _v(prim[3]))
		st.generate_normals()
		var mi := MeshInstance3D.new()
		mi.mesh = st.commit()
		var mat := StandardMaterial3D.new()
		var col: Array = part[1]
		mat.albedo_color = Color(col[0], col[1], col[2])
		mat.roughness = part[2]
		mat.metallic = 0.0
		mi.material_override = mat
		add_child(mi)
	var direct := OS.get_cmdline_user_args().has("--direct")
	if direct:
		var e := ($WorldEnvironment.environment as Environment).duplicate() as Environment
		e.ambient_light_energy = 0.0
		$WorldEnvironment.environment = e
		var sun := $Sun as DirectionalLight3D
		sun.light_energy = 1.0
		sun.light_color = Color(1, 1, 1)
		# Godot DirectionalLight3D shines along -Z of its own basis; rotate so
		# the light vector points FROM the sun TO the scene = -LIGHT_DIR.
		var l := DIRECT_LIGHT.normalized()
		# Godot's DirectionalLight3D emits along its local -Z. Build the basis
		# so local -Z == l exactly: pick an up vector not parallel to l, then
		# x = up.cross(l).normalized(), y = l.cross(x), z = l.
		var basis := Basis()
		var zaxis := -l
		var up := Vector3.UP if absf(l.dot(Vector3.UP)) < 0.99 else Vector3.RIGHT
		var xaxis := up.cross(zaxis).normalized()
		var yaxis := zaxis.cross(xaxis).normalized()
		basis.x = xaxis; basis.y = yaxis; basis.z = zaxis
		sun.global_transform = Transform3D(basis, Vector3.ZERO)
		print("REF: direct protocol (light_dir=", zaxis, " ambient=0)")
	print("REF: twin ready parts=", PARTS.size())

func _process(_delta: float) -> void:
	frame += 1
	if frame < 5:
		return
	var img := get_viewport().get_texture().get_image()
	print("REF: capture size=", img.get_size())
	img.convert(Image.FORMAT_RGB8)
	var w := img.get_width()
	var h := img.get_height()
	print("REFDUMP: begin ", w, "x", h)
	var data := img.get_data()
	var per := 160
	var chunks := int(ceil(float(w) / float(per)))
	for y in range(h):
		var base := y * w * 3
		for k in range(chunks):
			var sb := ""
			for x in range(per):
				var xx := k * per + x
				if xx >= w:
					break
				var o := base + xx * 3
				sb += "%02x%02x%02x" % [data[o], data[o + 1], data[o + 2]]
			print("REFC:%d:%d:%s" % [y, k, sb])
	print("REFDUMP: end")
	get_tree().quit(0)
