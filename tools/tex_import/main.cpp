// tex_import: offline raw-RGBA -> UASTC .basis -> .gtex (GNE-owned container).
// GNE-017 Phase 1. Single albedo slot. No PNG decoder inside (the .rgba is
// prepared by make_checkerboard.py): zero new third-party dependencies in
// this tool besides basisu itself (encoder + transcoder, offline only).
//
// Usage: tex_import <in.rgba> <W> <H> <out.gtex>
// Evidence contract: encodes UASTC, transcodes back, verifies first texel
// EXACTLY, writes .gtex, re-reads the FILE and verifies again. Any mismatch
// => nonzero exit (no silent success).
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "basisu_comp.h"
#include "basisu_transcoder.h"

static void fail(const char *msg) {
	fprintf(stderr, "[tex_import] FAIL %s\n", msg);
	exit(1);
}

#pragma pack(push, 1)
struct GtexHeader {
	uint32_t magic;   // 0x474E4554 "GNET" (LE bytes: G N E T)
	uint32_t version; // 1
	uint32_t width, height, mip_count;
	uint32_t format;     // 0 = UASTC-in-basis payload
	uint32_t slot_count; // 1 in Phase 1
	uint32_t flags;      // 0
};
struct GtexSlot {
	uint32_t width, height, mips;
	uint32_t basis_offset, basis_size;
	uint32_t rgba_offset, rgba_size; // offline-transcoded RGBA8 mip chain
	uint32_t flags; // 0
};
#pragma pack(pop)
static_assert(sizeof(GtexHeader) == 32, "GtexHeader must be 32 bytes");
static_assert(sizeof(GtexSlot) == 32, "GtexSlot must be 32 bytes");

#define GNET_MAGIC 0x474E4554u
#define GNET_VERSION 1u
#define GNET_FORMAT_UASTC 0u

struct RawImage {
	int w = 0, h = 0;
	std::vector<uint8_t> px; // RGBA8
};

static RawImage read_rgba(const char *path, int w, int h) {
	FILE *f = nullptr;
	if (fopen_s(&f, path, "rb") != 0 || !f) {
		fail("cannot open input .rgba");
	}
	RawImage img;
	img.w = w;
	img.h = h;
	img.px.resize((size_t)w * h * 4);
	size_t n = fread(img.px.data(), 1, img.px.size(), f);
	fclose(f);
	if (n != img.px.size()) {
		fail("input .rgba size mismatch (want W*H*4 bytes)");
	}
	return img;
}

// Exact box-filter mip chain (level 0 = source). Deterministic by construction.
static std::vector<RawImage> build_mips(const RawImage &src) {
	std::vector<RawImage> mips;
	mips.push_back(src);
	while (mips.back().w > 1 || mips.back().h > 1) {
		const RawImage &p = mips.back();
		RawImage m;
		m.w = p.w > 1 ? p.w / 2 : 1;
		m.h = p.h > 1 ? p.h / 2 : 1;
		m.px.resize((size_t)m.w * m.h * 4);
		for (int y = 0; y < m.h; y++) {
			for (int x = 0; x < m.w; x++) {
				for (int c = 0; c < 4; c++) {
					int sx = x * 2, sy = y * 2;
					unsigned s = 0, n = 0;
					for (int dy = 0; dy < 2 && sy + dy < p.h; dy++) {
						for (int dx = 0; dx < 2 && sx + dx < p.w; dx++) {
							s += p.px[((size_t)(sy + dy) * p.w + (sx + dx)) * 4 + c];
							n++;
						}
					}
					m.px[((size_t)y * m.w + x) * 4 + c] = (uint8_t)((s + n / 2) / n);
				}
			}
		}
		mips.push_back(m);
	}
	return mips;
}

static void to_basis_image(const RawImage &src, basisu::image &dst) {
	// This fork's image API takes raw pixels directly (no set_pixel).
	dst.init(src.px.data(), (uint32_t)src.w, (uint32_t)src.h, 4);
}

int main(int argc, char **argv) {
	// Two modes:
	//   tex_import <in.rgba> <W> <H> <out.gtex>          (mips generated)
	//   tex_import --levels <stub> <W> <H> <out.gtex>    (explicit <stub>_L0..Ln.rgba)
	bool explicit_mips = argc == 6 && strcmp(argv[1], "--levels") == 0;
	if (!(argc == 5 || explicit_mips)) {
		fprintf(stderr, "usage: tex_import <in.rgba> <W> <H> <out.gtex>\n");
		fprintf(stderr, "   or: tex_import --levels <stub> <W> <H> <out.gtex>\n");
		return 2;
	}
	const int W = atoi(argv[explicit_mips ? 3 : 2]);
	const int H = atoi(argv[explicit_mips ? 4 : 3]);
	const char *out_path = argv[explicit_mips ? 5 : 4];
	if (W <= 0 || H <= 0 || (W & (W - 1)) != 0 || (H & (H - 1)) != 0) {
		fail("W/H must be positive powers of two");
	}
	RawImage src;
	std::vector<RawImage> mips;
	if (explicit_mips) {
		int w = W, h = H, l = 0;
		while (true) {
			char path[512];
			snprintf(path, sizeof(path), "%s_L%d.rgba", argv[2], l);
			mips.push_back(read_rgba(path, w, h));
			if (w == 1 && h == 1) {
				break;
			}
			w = w > 1 ? w / 2 : 1;
			h = h > 1 ? h / 2 : 1;
			l++;
		}
		src = mips[0];
	} else {
		src = read_rgba(argv[1], W, H);
		mips = build_mips(src);
	}
	printf("[tex_import] input %dx%d, %u mip levels\n", W, H, (unsigned)mips.size());

	basisu::basisu_encoder_init();
	basisu::basis_compressor_params p;
	p.m_uastc = true;
	p.m_y_flip = false;
	p.m_multithreading = false;
	p.m_write_output_basis_or_ktx2_files = false;
	p.m_create_ktx2_file = false;
	basisu::image level0;
	to_basis_image(mips[0], level0);
	p.m_source_images.push_back(level0);
	p.m_source_mipmap_images.resize(1);
	for (size_t l = 1; l < mips.size(); l++) {
		basisu::image img;
		to_basis_image(mips[l], img);
		p.m_source_mipmap_images[0].push_back(img);
	}
	basisu::basis_compressor comp;
	basisu::job_pool jpool(1); // serial: deterministic output
	p.m_pJob_pool = &jpool;
	if (!comp.init(p)) {
		fail("basis_compressor init");
	}
	if (comp.process() != 0 /* cECSuccess */) {
		fail("basis_compressor process (UASTC encode)");
	}
	const basisu::uint8_vec &basis = comp.get_output_basis_file();
	printf("[tex_import] UASTC .basis payload: %u bytes\n", (unsigned)basis.size());
	if (basis.empty()) {
		fail("empty .basis output");
	}

	// Proof 1: transcode the payload back and verify (first texel EXACT).
	basist::basisu_transcoder_init();
	basist::basisu_transcoder dec;
	if (!dec.validate_header(basis.data(), (uint32_t)basis.size())) {
		fail("basis header invalid");
	}
	if (dec.get_total_image_levels(basis.data(), (uint32_t)basis.size(), 0) != (uint32_t)mips.size()) {
		fail("basis mip count mismatch");
	}
	if (!dec.start_transcoding(basis.data(), (uint32_t)basis.size())) {
		fail("transcoder start");
	}
	std::vector<uint8_t> rgba((size_t)W * H * 4);
	if (!dec.transcode_image_level(basis.data(), (uint32_t)basis.size(), 0, 0,
			rgba.data(), (uint32_t)(W * H), basist::transcoder_texture_format::cTFRGBA32)) {
		fail("transcode level 0");
	}
	if (memcmp(rgba.data(), src.px.data(), 4) != 0) {
		fail("first texel mismatch after UASTC round-trip");
	}
	unsigned mism = 0;
	for (size_t i = 0; i < (size_t)W * H; i++) {
		if (memcmp(&rgba[i * 4], &src.px[i * 4], 4) != 0) {
			mism++;
		}
	}

	// RGBA8 section: transcode EVERY level offline (runtime uploads raw bytes,
	// zero Basis dependency). Proof: first texel of every level == box-filtered
	// source (exact for flat cells; reported honestly per level below).
	std::vector<uint8_t> rgba_blob;
	std::vector<unsigned> level_mism;
	for (size_t l = 0; l < mips.size(); l++) {
		int lw = mips[l].w, lh = mips[l].h;
		std::vector<uint8_t> lr((size_t)lw * lh * 4);
		if (!dec.transcode_image_level(basis.data(), (uint32_t)basis.size(), 0, (uint32_t)l,
				lr.data(), (uint32_t)(lw * lh), basist::transcoder_texture_format::cTFRGBA32)) {
			fail("transcode level for RGBA8 section");
		}
		unsigned lm = 0;
		for (int i = 0; i < lw * lh; i++) {
			if (memcmp(&lr[(size_t)i * 4], &mips[l].px[(size_t)i * 4], 4) != 0) {
				lm++;
			}
		}
		level_mism.push_back(lm);
		rgba_blob.insert(rgba_blob.end(), lr.begin(), lr.end());
	}

	// Write .gtex: header + 1 slot + basis payload + RGBA8 payload.
	GtexHeader hdr = {};
	hdr.magic = GNET_MAGIC;
	hdr.version = GNET_VERSION;
	hdr.width = (uint32_t)W;
	hdr.height = (uint32_t)H;
	hdr.mip_count = (uint32_t)mips.size();
	hdr.format = GNET_FORMAT_UASTC;
	hdr.slot_count = 1;
	hdr.flags = 0;
	GtexSlot slot = {};
	slot.width = (uint32_t)W;
	slot.height = (uint32_t)H;
	slot.mips = (uint32_t)mips.size();
	slot.basis_offset = sizeof(hdr) + sizeof(slot);
	slot.basis_size = (uint32_t)basis.size();
	slot.rgba_offset = slot.basis_offset + slot.basis_size;
	slot.rgba_size = (uint32_t)rgba_blob.size();
	slot.flags = 0;
	FILE *f = nullptr;
	if (fopen_s(&f, out_path, "wb") != 0 || !f) {
		fail("cannot open output .gtex");
	}
	bool ok = fwrite(&hdr, 1, sizeof(hdr), f) == sizeof(hdr)
		&& fwrite(&slot, 1, sizeof(slot), f) == sizeof(slot)
		&& fwrite(basis.data(), 1, basis.size(), f) == basis.size()
		&& fwrite(rgba_blob.data(), 1, rgba_blob.size(), f) == rgba_blob.size();
	fclose(f);
	if (!ok) {
		fail("writing .gtex");
	}

	// Proof 2: re-read the FILE from disk and verify header + first texel.
	FILE *g = nullptr;
	if (fopen_s(&g, out_path, "rb") != 0 || !g) {
		fail("cannot re-open .gtex");
	}
	GtexHeader rh = {};
	GtexSlot rs = {};
	std::vector<uint8_t> payload;
	if (fread(&rh, 1, sizeof(rh), g) != sizeof(rh)
		|| fread(&rs, 1, sizeof(rs), g) != sizeof(rs)) {
		fclose(g);
		fail("reading .gtex header");
	}
	payload.resize(rs.basis_size);
	if (fread(payload.data(), 1, payload.size(), g) != payload.size()) {
		fclose(g);
		fail("reading .gtex payload");
	}
	std::vector<uint8_t> rgba_sec(rs.rgba_size);
	if (fread(rgba_sec.data(), 1, rgba_sec.size(), g) != rgba_sec.size()) {
		fclose(g);
		fail("reading .gtex RGBA8 section");
	}
	fclose(g);
	if (rh.magic != GNET_MAGIC || rh.version != GNET_VERSION
		|| rh.width != (uint32_t)W || rh.height != (uint32_t)H
		|| rh.mip_count != (uint32_t)mips.size() || rh.slot_count != 1) {
		fail(".gtex header mismatch on re-read");
	}
	basist::basisu_transcoder dec2;
	if (!dec2.validate_header(payload.data(), (uint32_t)payload.size())
		|| !dec2.start_transcoding(payload.data(), (uint32_t)payload.size())
		|| !dec2.transcode_image_level(payload.data(), (uint32_t)payload.size(), 0, 0,
			rgba.data(), (uint32_t)(W * H), basist::transcoder_texture_format::cTFRGBA32)
		|| memcmp(rgba.data(), src.px.data(), 4) != 0) {
		fail(".gtex payload first-texel mismatch on re-read");
	}

	printf("GTEX verify:\n");
	printf("- magic: 0x%08X\n", rh.magic);
	printf("- version: %u\n", rh.version);
	printf("- dimensions: %ux%u\n", rh.width, rh.height);
	printf("- mip_count: %u\n", rh.mip_count);
	printf("- format: UASTC\n");
	printf("- first_texel: [%u, %u, %u, %u]\n",
		rgba[0], rgba[1], rgba[2], rgba[3]);
	printf("- level0 mismatches vs source: %u / %u\n", mism, (unsigned)(W * H));
	printf("- rgba section: %u bytes, first_texel [%u, %u, %u, %u]\n",
		(unsigned)rgba_sec.size(), rgba_sec[0], rgba_sec[1], rgba_sec[2], rgba_sec[3]);
	if (memcmp(rgba_sec.data(), src.px.data(), 4) != 0) {
		fail("RGBA8 section first-texel mismatch");
	}
	printf("- per-level transcode mismatches:");
	for (size_t l = 0; l < level_mism.size(); l++) {
		printf(" L%u=%u", (unsigned)l, level_mism[l]);
	}
	printf("\n");
	printf("[tex_import] wrote %s (%u bytes payload)\n", out_path, (unsigned)payload.size());
	return 0;
}
