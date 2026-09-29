extends RefCounted

# GNE-024 boot probe: referenced by `--check-only --script` as the module
# presence check for the boot gate.
#
# It names GneRenderServer EXPLICITLY on purpose. If the engine is built
# without the gne_render module (e.g. custom_modules was forgotten), this
# script fails to parse, so the boot gate gets TWO independent signals -
# a non-zero exit code AND the absence of the registration marker - instead
# of relying on the marker alone.
#
# --check-only parses this file and does NOT execute it; the body exists so
# the reference to the type is real and survives any future linter.
static func probe() -> bool:
	var s: GneRenderServer = GneRenderServer.get_server_singleton()
	return s != null
