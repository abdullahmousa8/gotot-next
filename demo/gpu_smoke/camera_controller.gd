extends Camera3D

# FPS-style free camera for the GOTOT demo.
# WASD move, Shift up, Ctrl/space-down, mouse look (click to capture/release).

var move_speed := 900.0
var mouse_sens := 0.0022
var captured := false

func _ready() -> void:
	Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)
	captured = true

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouseButton and event.button_index == MOUSE_BUTTON_LEFT and event.pressed:
		if captured:
			Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)
			captured = false
		else:
			Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)
			captured = true
	if event is InputEventMouseMotion and captured:
		rotate_y(-event.relative.x * mouse_sens)
		rotate_object_local(Vector3.RIGHT, -event.relative.y * mouse_sens)
	if event is InputEventKey and event.pressed and event.keycode == KEY_ESCAPE:
		Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)
		captured = false

func _process(delta: float) -> void:
	var dir := Vector3.ZERO
	if Input.is_key_pressed(KEY_W):
		dir -= global_transform.basis.z
	if Input.is_key_pressed(KEY_S):
		dir += global_transform.basis.z
	if Input.is_key_pressed(KEY_A):
		dir -= global_transform.basis.x
	if Input.is_key_pressed(KEY_D):
		dir += global_transform.basis.x
	if Input.is_key_pressed(KEY_SHIFT):
		dir += Vector3.UP
	if Input.is_key_pressed(KEY_CTRL) or Input.is_key_pressed(KEY_SPACE):
		dir -= Vector3.UP
	dir = dir.normalized()
	if dir != Vector3.ZERO:
		global_position += dir * move_speed * delta