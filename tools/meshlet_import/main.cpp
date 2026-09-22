// GOTOT-NEXT Meshlet Import Tool (offline).
// Generates a procedural high-poly torus, builds 3 LOD proxies with
// meshoptimizer (v1.2), meshletizes each LOD, and writes the GOTOT
// Meshlet binary format (".gomlet") consumed by the 013 runtime.
// MIT-licensed third-party code: meshoptimizer (in third_party/meshoptimizer).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <cmath>

#include "third_party/meshoptimizer/src/meshoptimizer.h"

// GOMLET constants (mirrored by the 013 runtime).
static const unsigned char GOMLET_MAGIC[8] = {'G', 'O', 'T', 'O', 'M', 'L', '1', '1'};
static const uint32_t GOMLET_VERSION = 1;

// Meshlet micro-buffer limits (must match GPU shader decode limits).
static const size_t ML_MAX_VERTICES = 64;
static const size_t ML_MAX_TRIANGLES = 124;

static const size_t SEG_MAJOR = 1024;
static const size_t SEG_MINOR = 512;

static void write_u32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void write_vec4(FILE *f, float x, float y, float z) {
	float v[4] = {x, y, z, 0.0f};
	fwrite(v, 4, 4, f);
}

// Torus in the XY plane (axis = Z), so a camera at +Z sees flat rings.
struct Torus {
	std::vector<float> positions; // Vec3 (stride 12)
	std::vector<uint32_t> indices;
	size_t vertex_count;
	size_t tri_count;

	explicit Torus(float major_R, float minor_r, size_t seg_u, size_t seg_v) {
		vertex_count = seg_u * seg_v;
		positions.resize(vertex_count * 3);
		for (size_t i = 0; i < seg_u; i++) {
			float u = float(i) / float(seg_u) * 2.0f * 3.14159265358979f;
			for (size_t j = 0; j < seg_v; j++) {
				float v = float(j) / float(seg_v) * 2.0f * 3.14159265358979f;
				float r = major_R + minor_r * std::cos(v);
				size_t id = i * seg_v + j;
				positions[id * 3 + 0] = r * std::cos(u);
				positions[id * 3 + 1] = r * std::sin(u);
				positions[id * 3 + 2] = minor_r * std::sin(v);
			}
		}
		tri_count = seg_u * seg_v * 2;
		indices.resize(tri_count * 3);
		size_t t = 0;
		for (size_t i = 0; i < seg_u; i++) {
			size_t i0 = i == seg_u - 1 ? 0 : i + 1;
			for (size_t j = 0; j < seg_v; j++) {
				size_t j0 = j == seg_v - 1 ? 0 : j + 1;
				uint32_t a0 = uint32_t(i * seg_v + j);
				uint32_t b0 = uint32_t(i0 * seg_v + j);
				uint32_t a1 = uint32_t(i * seg_v + j0);
				uint32_t b1 = uint32_t(i0 * seg_v + j0);
				indices[t++] = a0;
				indices[t++] = b0;
				indices[t++] = a1;
				indices[t++] = b0;
				indices[t++] = b1;
				indices[t++] = a1;
			}
		}
	}
};

// One simplified LOD mesh ready for meshletization.
struct LODMesh {
	std::vector<float> positions;    // Vec3 (subset of the LOD0 source)
	std::vector<uint32_t> indices;
	size_t vertex_count;
	size_t tri_count;
};

static size_t simplify_lod(const std::vector<float> &src_pos, const std::vector<uint32_t> &src_idx,
		float target_ratio, LODMesh &out) {
	out.positions = src_pos;
	size_t target_index_count = size_t(src_idx.size() * target_ratio);
	std::vector<uint32_t> si(src_idx.size());
	float result_error = 0.0f;
	size_t result = meshopt_simplify(si.data(), src_idx.data(), src_idx.size(), src_pos.data(),
			src_pos.size() / 3, sizeof(float) * 3, target_index_count, 1e-4f, 0, &result_error);
	out.indices.assign(si.begin(), si.begin() + result);
	out.tri_count = result / 3;
	out.vertex_count = src_pos.size() / 3;
	printf("  simplify: target %zu tris, produced %zu tris, error %.6f\n",
			target_index_count / 3, out.tri_count, result_error);
	return result;
}

// Optimizes vertex fetch: reorders positions/indices so active vertices are packed.
static void fetch_optimize(LODMesh &m) {
	size_t vcount = m.positions.size() / 3;
	std::vector<float> new_pos(vcount * 3);
	size_t new_count = meshopt_optimizeVertexFetch(new_pos.data(), m.indices.data(), m.indices.size(),
			m.positions.data(), vcount, sizeof(float) * 3);
	new_pos.resize(new_count * 3);
	m.positions.swap(new_pos);
	m.vertex_count = new_count;
}

int main(int argc, char **argv) {
	setvbuf(stdout, nullptr, _IONBF, 0);
	const char *out_path = argc > 1 ? argv[1] : "mesh_013_torus.gomlet";

	printf("[GOTOT] meshlet import v1 (meshoptimizer 1.2)\n");

	Torus torus(300.0f, 60.0f, SEG_MAJOR, SEG_MINOR);
	printf("[GOTOT] torus: %zu verts, %zu tris (LOD0)\n", torus.vertex_count, torus.tri_count);

	// LOD0: cache-optimize the full mesh before meshletizing.
	{
		std::vector<uint32_t> vcache(torus.indices.size());
		meshopt_optimizeVertexCache(vcache.data(), torus.indices.data(), torus.indices.size(), torus.vertex_count);
		torus.indices.swap(vcache);
	}

	const size_t LOD_COUNT = 3;
	std::vector<LODMesh> lods(LOD_COUNT);
	lods[0].positions = torus.positions;
	lods[0].indices = torus.indices;
	lods[0].vertex_count = torus.vertex_count;
	lods[0].tri_count = torus.tri_count;

	for (size_t l = 1; l < LOD_COUNT; l++) {
		float target = l == 1 ? 0.5f : 0.2f;
		printf("[GOTOT] building LOD%zu (target %.0f%% of LOD0)\n", l, target * 100.0f);
		LODMesh lm;
		simplify_lod(torus.positions, torus.indices, target, lm);
		lods[l] = std::move(lm);
	}

	// Meshletization records per LOD (positions padded to Vec4, refs, tri bytes, descs).
	struct MeshletRec {
		std::vector<unsigned int> vertices; // concatenated vertex refs
		std::vector<unsigned char> triangles; // concatenated local tri indices (u8)
		std::vector<meshopt_Meshlet> meshlets;
		std::vector<meshopt_Bounds> bounds;
	};
	std::vector<MeshletRec> recs(LOD_COUNT);

	for (size_t l = 0; l < LOD_COUNT; l++) {
		LODMesh &m = lods[l];
		fetch_optimize(m);
		printf("[GOTOT] meshletizing LOD%zu: %zu verts, %zu tris...\n", l, m.vertex_count, m.tri_count);

		size_t bound = meshopt_buildMeshletsBound(m.indices.size(), ML_MAX_VERTICES, ML_MAX_TRIANGLES);
		MeshletRec &rec = recs[l];
		// buildMeshlets writes worst case index_count refs/bytes (per meshoptimizer.h docs).
		rec.vertices.resize(m.indices.size());
		rec.triangles.resize(m.indices.size());
		rec.meshlets.resize(bound);

		size_t meshlet_count = meshopt_buildMeshlets(rec.meshlets.data(), rec.vertices.data(),
				rec.triangles.data(), m.indices.data(), m.indices.size(),
				m.positions.data(), m.vertex_count, sizeof(float) * 3,
				ML_MAX_VERTICES, ML_MAX_TRIANGLES, 1.0f);

		rec.meshlets.resize(meshlet_count);
		rec.vertices.resize(rec.meshlets.back().vertex_offset + rec.meshlets.back().vertex_count);
		rec.triangles.resize(rec.meshlets.back().triangle_offset + rec.meshlets.back().triangle_count * 3);
		rec.bounds.resize(meshlet_count);

		for (size_t i = 0; i < meshlet_count; i++) {
			meshopt_optimizeMeshlet(&rec.vertices[rec.meshlets[i].vertex_offset],
					&rec.triangles[rec.meshlets[i].triangle_offset],
					rec.meshlets[i].triangle_count, rec.meshlets[i].vertex_count);
			rec.bounds[i] = meshopt_computeMeshletBounds(&rec.vertices[rec.meshlets[i].vertex_offset],
					&rec.triangles[rec.meshlets[i].triangle_offset],
					rec.meshlets[i].triangle_count, m.positions.data(), m.vertex_count, sizeof(float) * 3);
		}

		size_t total_refs = 0, total_tris = 0;
		for (size_t i = 0; i < meshlet_count; i++) {
			total_refs += rec.meshlets[i].vertex_count;
			total_tris += rec.meshlets[i].triangle_count;
		}
		printf("[GOTOT] LOD%zu: %zu meshlets, %zu refs, %zu micro-tris\n",
				l, meshlet_count, total_refs, total_tris);
	}

	// Write GOMLET file.
	FILE *f = fopen(out_path, "wb");
	if (!f) {
		fprintf(stderr, "error: cannot open %s for writing\n", out_path);
		return 2;
	}
	fwrite(GOMLET_MAGIC, 1, 8, f);
	write_u32(f, GOMLET_VERSION);
	write_u32(f, uint32_t(LOD_COUNT));
	write_u32(f, 0); // reserved

	for (size_t l = 0; l < LOD_COUNT; l++) {
		MeshletRec &rec = recs[l];
		write_u32(f, uint32_t(lods[l].vertex_count));
		write_u32(f, uint32_t(lods[l].tri_count));
		write_u32(f, uint32_t(rec.meshlets.size()));
		// ref_count makes the concatenated ref array length self-describing so
		// the 013 runtime can locate positions/refs/micro-tris/descs without
		// ambiguity (per-LOD header = vertex_count, tri_count, meshlet_count,
		// ref_count; micro-tri bytes follow as 3 * sum(meshlet triangle_counts)).
		write_u32(f, uint32_t(rec.vertices.size()));
		// Vertices (Vec4).
		for (size_t v = 0; v < lods[l].vertex_count; v++)
			write_vec4(f, lods[l].positions[v * 3 + 0], lods[l].positions[v * 3 + 1], lods[l].positions[v * 3 + 2]);
		// Vertex refs.
		for (unsigned int r : rec.vertices)
			write_u32(f, r);
		// Micro triangles (u8).
		fwrite(rec.triangles.data(), 1, rec.triangles.size(), f);
		// Meshlet descriptors.
		for (size_t i = 0; i < rec.meshlets.size(); i++) {
			const meshopt_Meshlet &ml = rec.meshlets[i];
			const meshopt_Bounds &b = rec.bounds[i];
			write_u32(f, ml.vertex_offset);
			write_u32(f, ml.vertex_count);
			write_u32(f, ml.triangle_offset);
			write_u32(f, ml.triangle_count);
			fwrite(&b.center[0], 4, 3, f);
			fwrite(&b.radius, 4, 1, f);
			fwrite(&b.cone_axis[0], 4, 3, f);
			fwrite(&b.cone_cutoff, 4, 1, f);
		}
	}
	fclose(f);

	uint64_t lod0_tris = lods[0].tri_count;
	size_t lod0_meshlets = recs[0].meshlets.size();
	printf("[GOTOT] wrote %s\n", out_path);
	printf("[GOTOT] STATS lod0_tri_count=%llu lod0_meshlet_count=%zu"
			" total_meshlets=%zu\n",
			(unsigned long long)lod0_tris, lod0_meshlets, recs[0].meshlets.size() + recs[1].meshlets.size() + recs[2].meshlets.size());
	return (lod0_tris >= 1000000 && lod0_meshlets >= 10000) ? 0 : 3;
}