# GOTOT-NEXT Dependency Register

The register is the authoritative record (see `docs/open_source_system_strategy_v1.md` Section 21.5).

**Status:** One offline tooling dependency integrated (see entry below). Runtime remains dependency-free.

For each future dependency, record:
- Name + version + hash
- Category (runtime/offline)
- License + obligations
- Wrapper location
- Removal plan
- Approval date + approver
- SBOM entry

---

## meshoptimizer

| Field | Value |
|-------|-------|
| Name | meshoptimizer |
| Version | v1.2 (`9d9890c73011d75920af614485296d1e03e95448`) |
| License | MIT |
| Category | offline tooling (build-time meshlet generation) |
| Runtime dependency | **None** — the GOTOT-NEXT runtime module is meshoptimizer-free |
| Wrapper | `tools/meshlet_import` (vendored source at `tools/meshlet_import/third_party/meshoptimizer`) |
| Removal plan | Replace wrapper with a GOTOT-owned meshlet builder once hosted/hand-rolled tooling is feasible |
| Integration date | 2026-09-22 |
| Approver | Architect (GOTOT-013 commit authorization) |
| SBOM entry | Add when SBOM artifacts are next refreshed (strategy §17) |