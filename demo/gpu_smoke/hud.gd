extends Label

# Simple demo HUD. The main scene fills `data` each frame (a compact dictionary)
# and also calls `hint(...)` to show control hints.

var data: Dictionary = {}

func _process(_delta: float) -> void:
	var fps := Engine.get_frames_per_second()
	var text := "GOTOT-NEXT 011 interactive demo\n"
	text += "FPS %d  frame %.1f ms\n" % [fps, 1000.0 / maxf(1.0, float(fps))]
	if data.has("visible"):
		text += "visible %d / %d instances\n" % [data.visible, data.total]
		text += "meshes %d   batches %d   groups %d   draw calls %d\n" % [
			data.meshes, data.batches, data.groups, data.draw_calls]
		text += "strategy %s\n" % data.strategy_name
		text += "dispatch %.2f ms   draw %.2f ms\n" % [data.dispatch_ms, data.draw_ms]
	text += "\nWASD move | Shift up | Ctrl down | Mouse look (click)\n"
	text += "R : cycle strategy   P : pixel evidence   F12 : screenshot\n"
	text += "ESC : release mouse"
	text = text.replace("\n", "\n").replace("  ", " ")
	set_text(text)
	modulate = Color(1, 1, 1, 1)
	position = Vector2(24, 24)