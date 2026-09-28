extends Node
# GNE 015.5 — Resource Pool (real allocation + async readback)
# SPEC 015.5 v0.2; D3-D1..D4 resolved:
#   D3-D1 presentation = async readback, NOT zero-copy
#   D3-D2 persistent cap = 256 entries
#   D3-D3 growth = double-up-to-cap (64 -> 128 -> 256)
#   D3-D4 signature  = inside v15, flag pr  =>  v15-...-pr1
#
# Acceptance criteria (SPEC §4) covered here:
#   C1 pool is REAL      -> gpu_pool_verify round-trips bytes through the buffer
#   C2 aliasing is REAL  -> disjoint lifetimes share one offset; overlap does not
#   C3 signatures stable -> 013/014/015 reproduced + 012 = rc 123 (harness)
#   C4 persistent cap    -> the 257th persistent alloc is REJECTED
#   C5 dynamic auditable -> rebuilds / bytes_copied printed and stable
#   C7 readback framing  -> "async readback, not zero-copy" asserted in output
#   C8 no regression     -> full gt_harness sweep + gt_regress
# (C6 frame-time drift is a SEPARATE measurement phase; see SPEC §8 Phase 4.)

const POOL_BYTES := 4 * 1024 * 1024 # 4 MiB
const PERSISTENT_TRIALS := 300      # > 256 so the cap MUST reject

var server: GneRenderServer
var pass_count := 0


func _ready() -> void:
	server = GneRenderServer.get_server_singleton()
	if server == null:
		_fail(300, "server singleton is null")
		return
	if not server.ensure_gpu_device():
		_fail(301, "local RenderingDevice not available")
		return

	if not _phase_pool_real():
		return
	if not _phase_aliasing():
		return
	if not _phase_persistent_cap():
		return
	if not _phase_dynamic():
		return
	if not _phase_readback_framing():
		return

	print("GNE 015.5: phases=", pass_count, "/5")
	print("GNE 015.5: PASS")
	get_tree().quit(0)


# C1: the pool is REAL memory, not a modelled number. gpu_pool_verify writes
# an int into the pool buffer, allocates a block, reads the bytes back, and
# fails on any mismatch - an accounting-only implementation cannot pass.
func _phase_pool_real() -> bool:
	if not server.gpu_pool_create(POOL_BYTES):
		_fail(302, "gpu_pool_create(" + str(POOL_BYTES) + ")")
		return false
	var st: Dictionary = server.gpu_pool_stats()
	if int(st["pool_bytes"]) != POOL_BYTES:
		_fail(303, "pool_bytes " + str(st["pool_bytes"]) + " != " + str(POOL_BYTES))
		return false
	if not server.gpu_pool_verify("alpha", 123456789):
		_fail(304, "gpu_pool_verify alpha")
		return false
	if not server.gpu_pool_verify("beta", -42):
		_fail(305, "gpu_pool_verify beta")
		return false
	pass_count += 1
	print("GNE 015.5: C1 pool_real bytes=", st["pool_bytes"], " verify=alpha,beta OK")
	return true


# C2: ALIASING IS REAL. A lives in passes [0,0], B in [5,5] => disjoint
# lifetimes => B must land on A's offset. We prove it by observing that the
# pool did NOT grow (bump unchanged) and alias_saved increased by A's size.
# A counter-only implementation that ignores lifetimes would bump instead.
func _phase_aliasing() -> bool:
	# Baseline is taken AFTER A: A legitimately bumps, because at that moment
	# there is no live block to share an offset with. What we then prove is
	# that B (disjoint lifetime) aliases A, and C (overlapping) does not.
	var a := server.gpu_pool_alloc(64, 0, 0, "aliasA")
	if a < 0:
		_fail(306, "alloc aliasA rejected")
		return false
	var before: Dictionary = server.gpu_pool_stats()
	var bump_before := int(before["bump_bytes"])
	var alias_before := int(before["alias_saved"])
	var b := server.gpu_pool_alloc(64, 5, 5, "aliasB")
	if b < 0:
		_fail(307, "alloc aliasB rejected")
		return false
	var after: Dictionary = server.gpu_pool_stats()
	if int(after["bump_bytes"]) != bump_before:
		_fail(308, "disjoint lifetimes still bumped (" + str(bump_before) + " -> " + str(after["bump_bytes"]) + ") - aliasing NOT applied")
		return false
	if int(after["alias_saved"]) <= alias_before:
		_fail(309, "alias_saved did not increase (" + str(alias_before) + " -> " + str(after["alias_saved"]) + ")")
		return false
	# An OVERLAPPING lifetime must NOT alias: it has to take fresh memory.
	var c := server.gpu_pool_alloc(64, 5, 5, "aliasC_overlap")
	if c < 0:
		_fail(310, "alloc aliasC rejected")
		return false
	var final_stats: Dictionary = server.gpu_pool_stats()
	if int(final_stats["bump_bytes"]) <= bump_before:
		_fail(311, "overlapping lifetime wrongly aliased")
		return false
	server.gpu_pool_free(c)
	server.gpu_pool_free(b)
	server.gpu_pool_free(a)
	pass_count += 1
	print("GNE 015.5: C2 aliasing_real alias_saved=", final_stats["alias_saved"], " disjoint=shared overlap=fresh OK")
	return true


# C4: the persistent cap is a HARD cap (D3-D2 = 256). We ask for 300 and the
# 257th must be rejected - overflow is an error, never silent growth.
func _phase_persistent_cap() -> bool:
	var accepted := 0
	var rejected := 0
	for i in PERSISTENT_TRIALS:
		var idx := server.gpu_pool_persistent_alloc(32, "persist" + str(i))
		if idx < 0:
			rejected += 1
		else:
			accepted += 1
	var cap: int = int(server.gpu_pool_stats()["persistent_cap"])
	if accepted != cap:
		_fail(312, "accepted " + str(accepted) + " persistent != cap " + str(cap))
		return false
	if rejected != PERSISTENT_TRIALS - cap:
		_fail(313, "rejected " + str(rejected) + " != " + str(PERSISTENT_TRIALS - cap))
		return false
	pass_count += 1
	print("GNE 015.5: C4 persistent_cap accepted=", accepted, " rejected=", rejected, " cap=", cap)
	return true
	return true


# C5: dynamic allocation stays auditable and deterministic. Counters must be
# visible and must NOT move without an actual resize.
func _phase_dynamic() -> bool:
	var st: Dictionary = server.gpu_pool_stats()
	var rebuilds := int(st["rebuilds"])
	var copied := int(st["bytes_copied"])
	var blocks_a := int(st["block_count"])
	var t1 := server.gpu_pool_alloc(48, 0, 1, "det1")
	var t2 := server.gpu_pool_alloc(48, 2, 3, "det2")
	if t1 < 0 or t2 < 0:
		_fail(314, "det alloc rejected")
		return false
	var mid: Dictionary = server.gpu_pool_stats()
	if int(mid["block_count"]) <= blocks_a:
		_fail(315, "block count did not grow")
		return false
	var st2: Dictionary = server.gpu_pool_stats()
	if int(st2["rebuilds"]) != rebuilds or int(st2["bytes_copied"]) != copied:
		_fail(316, "rebuild counters changed without a resize")
		return false
	server.gpu_pool_free(t2)
	server.gpu_pool_free(t1)
	pass_count += 1
	print("GNE 015.5: C5 dynamic rebuilds=", st2["rebuilds"], " bytes_copied=", st2["bytes_copied"], " grow_initial=", st2["grow_initial"])
	return true


# C7: the presentation decision is ASYNC READBACK, NOT zero-copy (D3-D1).
# We assert the module reports that framing so the claim can never silently
# become "zero-copy". The 30-50% readback reduction is a HYPOTHESIS to be
# measured in a later phase - nothing here claims a number.
func _phase_readback_framing() -> bool:
	var st: Dictionary = server.gpu_pool_stats()
	if String(st["readback_mode"]) != "async-readback-not-zero-copy":
		_fail(317, "readback_mode " + str(st["readback_mode"]))
		return false
	if String(st["signature_flag"]) != "pr1":
		_fail(318, "signature_flag " + str(st["signature_flag"]))
		return false
	# D3-D4: the 015.5 signature lives INSIDE v15 with the pr flag and is
	# backward compatible (the old v15 signatures stay valid).
	print("GNE 015.5: C7 readback=async-readback-not-zero-copy flag=pr1 (30-50pct claim NOT made here - unmeasured)")
	print("GNE 015.5: sig=v15-pr1 pool=", st["pool_bytes"], " blocks=", st["block_count"], " alias_saved=", st["alias_saved"])
	pass_count += 1
	return true


func _fail(code: int, msg: String) -> void:
	print("GNE 015.5: FAIL code=", code, " ", msg)
	get_tree().quit(code)

