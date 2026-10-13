/* dxmod-core-1 revision 1 contract fixture, not a runtime host or guest SDK. */
#ifndef DXMOD_CORE_CODEC_H
#define DXMOD_CORE_CODEC_H
#include <stddef.h>
#include <stdint.h>

static uint32_t dx_read(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void dx_write(uint8_t *p, uint32_t v)
{
	unsigned i;
	for (i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));
}
/* The caller supplies the accessible slice, never an unchecked guest pointer. */
static int dx_validate(const uint8_t *p, size_t n)
{
	uint32_t k;
	if (n < 16 || n > UINT32_MAX || dx_read(p) != n) return 1;
	if (dx_read(p + 4) != 1) return 2;
	if (dx_read(p + 12) != 0) return 1;
	k = dx_read(p + 8);
	if (k > 6) return 1;
	if (k == 6) {
		if (n < 24 || dx_read(p + 20) != n - 24) return 1;
		return dx_read(p + 16) == 0 ? 8 : 0;
	}
	if (n != (k == 0 ? 16 : k == 1 ? 24 : 40)) return 1;
	if ((k == 2 || k == 3) && (dx_read(p + 32) != 1 || dx_read(p + 36) != 0)) return 1;
	if (k == 5 && (dx_read(p + 28) > 255 || dx_read(p + 32) > 255 || dx_read(p + 36) > 255)) return 1;
	return 0;
}
static int dx_range(uint32_t memory, uint32_t base, uint32_t capacity, uint32_t length)
{
	return base <= memory && capacity <= memory - base && length >= 16 && length <= capacity;
}
#endif
