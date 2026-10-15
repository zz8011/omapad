#include "surface_bounds.h"

#include <stddef.h>
#include <stdint.h>

#define SURFACE_U32_END (UINT64_C(1) << 32)
#define SURFACE_PRIMARY_STRIDE_ALIGN 64U
#define SURFACE_STRIDE_MAX 0xffffU

static enum surface_result plane_span(
	const struct output_plane_candidate *plane, uint64_t *start,
	uint64_t *end, int primary)
{
	uint64_t active_bytes;
	uint64_t span;

	if (!plane->width || !plane->height || !plane->bytes_per_element ||
	    !plane->bytes_per_row)
		return SURFACE_BAD_PLANE;
	active_bytes = (uint64_t)plane->width * plane->bytes_per_element;
	if (active_bytes > plane->bytes_per_row ||
	    plane->bytes_per_row > SURFACE_STRIDE_MAX)
		return primary ? SURFACE_BAD_PRIMARY_STRIDE : SURFACE_BAD_PLANE;
	if (primary && plane->bytes_per_row % SURFACE_PRIMARY_STRIDE_ALIGN)
		return SURFACE_BAD_PRIMARY_STRIDE;

	span = (uint64_t)plane->bytes_per_row * plane->height;
	*start = plane->offset;
	if (span > UINT64_MAX - *start)
		return SURFACE_ADDRESS_OVERFLOW;
	*end = *start + span;
	return SURFACE_OK;
}

enum surface_result surface_candidate_validate(
	const struct output_surface_candidate *surface)
{
	uint64_t starts[SURFACE_CANDIDATE_PLANES];
	uint64_t ends[SURFACE_CANDIDATE_PLANES];
	uint64_t mapping_end;
	enum surface_result result;
	uint32_t i, j;

	if (!surface)
		return SURFACE_BAD_ARGUMENT;
	if (surface->plane_count != SURFACE_CANDIDATE_PLANES)
		return SURFACE_BAD_PLANE_COUNT;
	if (!surface->allocation_bytes)
		return SURFACE_BAD_ARGUMENT;
	if (surface->allocation_bytes >
	    UINT64_MAX - (uint64_t)surface->codec_base_iova)
		return SURFACE_ADDRESS_OVERFLOW;
	mapping_end = (uint64_t)surface->codec_base_iova +
		surface->allocation_bytes;
	if (mapping_end > SURFACE_U32_END)
		return SURFACE_ADDRESS_OVERFLOW;

	for (i = 0; i < SURFACE_CANDIDATE_PLANES; ++i) {
		result = plane_span(&surface->planes[i], &starts[i], &ends[i],
				    i == 0);
		if (result != SURFACE_OK)
			return result;
		if (ends[i] > surface->allocation_bytes)
			return SURFACE_OUT_OF_BOUNDS;
	}
	for (i = 0; i < SURFACE_CANDIDATE_PLANES; ++i) {
		for (j = i + 1; j < SURFACE_CANDIDATE_PLANES; ++j) {
			if (starts[i] < ends[j] && starts[j] < ends[i])
				return SURFACE_PLANES_OVERLAP;
		}
	}
	return SURFACE_OK;
}
