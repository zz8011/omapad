#ifndef A1599_SURFACE_BOUNDS_H
#define A1599_SURFACE_BOUNDS_H

#include <stdint.h>

#define SURFACE_CANDIDATE_PLANES 2U

enum surface_result {
	SURFACE_OK = 0,
	SURFACE_BAD_ARGUMENT,
	SURFACE_BAD_PLANE_COUNT,
	SURFACE_BAD_PLANE,
	SURFACE_BAD_PRIMARY_STRIDE,
	SURFACE_ADDRESS_OVERFLOW,
	SURFACE_OUT_OF_BOUNDS,
	SURFACE_PLANES_OVERLAP,
};

/* Format-neutral values imported from an IOSurface-like plane description. */
struct output_plane_candidate {
	uint32_t offset;
	uint32_t width;
	uint32_t height;
	uint32_t bytes_per_element;
	uint32_t bytes_per_row;
};

struct output_surface_candidate {
	uint32_t codec_base_iova;
	uint64_t allocation_bytes;
	uint32_t plane_count;
	struct output_plane_candidate planes[SURFACE_CANDIDATE_PLANES];
};

/*
 * Bounds-only validator for a provisional two-plane 32x32 4:2:0 experiment.
 * It does not identify pixel format, chroma ordering, VXD mapping, or whether
 * the selected allocation layout is accepted by the decoder.
 */
enum surface_result surface_candidate_validate(
	const struct output_surface_candidate *surface);

#endif
