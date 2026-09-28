// stubs.cpp - unreachable-by-construction third-party shims for tex_import.
//
// WHY THEY EXIST: basisu's encoder references optional subsystems this tool
// never exercises. Linking the real ones would drag in zstd (~15 files),
// tinyexr (~40K lines) and miniz plumbing for zero benefit. Each stub below
// aborts loudly with the exact reason, so a future code path that reaches one
// fails OBVIOUSLY instead of silently.
//
// UNREACHABILITY PROOF (all by construction in main.cpp):
// - ZSTD_*: only called from basis_compressor::create_ktx2_file() and the
//   transcoder's KTX2-supercompression path. We set m_create_ktx2_file=false
//   and our payloads carry no supercompression. Never reached.
// - EXR_*: only called from basisu_enc's read_exr/write_exr helpers. We feed
//   raw RGBA via image::init() and never touch EXR. Never reached.
// - pv_png::load_png is NOT stubbed: encoder/pvpngreader.cpp is compiled for
//   real (miniz is header-only inside basisu).
#include <cstdio>
#include <cstdlib>

#include "zstd.h"    // Godot thirdparty: exact C signatures enforced by compiler
#include "tinyexr.h" // Godot thirdparty: exact C++ signatures enforced by compiler

[[noreturn]] static void tex_unreachable(const char *what) {
	fprintf(stderr, "[tex_import] FATAL: reached stub %s (internal bug: this path must never execute)\n", what);
	fflush(stderr);
	abort();
}

extern "C" {

size_t ZSTD_compress(void *dst, size_t dstCapacity, const void *src, size_t srcSize, int compressionLevel) {
	(void)dst; (void)dstCapacity; (void)src; (void)srcSize; (void)compressionLevel;
	tex_unreachable("ZSTD_compress (KTX2 output disabled)");
	return 0;
}

size_t ZSTD_compressBound(size_t srcSize) {
	(void)srcSize;
	tex_unreachable("ZSTD_compressBound (KTX2 output disabled)");
	return 0;
}

size_t ZSTD_decompress(void *dst, size_t dstCapacity, const void *src, size_t compressedSize) {
	(void)dst; (void)dstCapacity; (void)src; (void)compressedSize;
	tex_unreachable("ZSTD_decompress (no supercompressed payloads)");
	return 0;
}

unsigned ZSTD_isError(size_t code) {
	(void)code;
	tex_unreachable("ZSTD_isError (no zstd use)");
	return 0;
}

} // extern "C"

int LoadEXRWithLayer(float **out_rgba, int *width, int *height,
		const char *filename, const char *layer_name, const char **err) {
	(void)out_rgba; (void)width; (void)height;
	(void)filename; (void)layer_name; (void)err;
	tex_unreachable("LoadEXRWithLayer (raw RGBA input only)");
	return -1;
}

void InitEXRHeader(EXRHeader *exr_header) {
	(void)exr_header;
	tex_unreachable("InitEXRHeader (no EXR use)");
}

void InitEXRImage(EXRImage *exr_image) {
	(void)exr_image;
	tex_unreachable("InitEXRImage (no EXR use)");
}

void FreeEXRErrorMessage(const char *msg) {
	(void)msg;
	tex_unreachable("FreeEXRErrorMessage (no EXR use)");
}

int SaveEXRImageToFile(const EXRImage *image,
		const EXRHeader *exr_header, const char *filename, const char **err) {
	(void)image; (void)exr_header; (void)filename; (void)err;
	tex_unreachable("SaveEXRImageToFile (no EXR use)");
	return -1;
}

int LoadEXRFromMemory(float **out_rgba, int *width, int *height,
		const unsigned char *memory, size_t size, const char **err) {
	(void)out_rgba; (void)width; (void)height;
	(void)memory; (void)size; (void)err;
	tex_unreachable("LoadEXRFromMemory (no EXR use)");
	return -1;
}
