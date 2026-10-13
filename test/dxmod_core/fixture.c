#include "codec.h"
#include <stdio.h>

int main(void)
{
	/* query, entity, tick event, move, light, versioned state */
	const uint32_t payload[6][6] = {
		{ 1, 2 }, { 1, 2, 10, 20, 1, 0 }, { 7, 0, 1, 2, 1, 0 },
		{ 7, 0, 1, 2, 11, 20 }, { 1, 2, 3, 255, 128, 0 }, { 1, 4, 42 }
	};
	unsigned k, i;
	for (k = 1; k <= 6; ++k) {
		uint8_t b[40] = { 0 };
		unsigned n = k == 1 ? 24 : k == 6 ? 28 : 40;
		dx_write(b, n); dx_write(b + 4, 1); dx_write(b + 8, k);
		for (i = 16; i < n; i += 4) dx_write(b + i, payload[k - 1][(i - 16) / 4]);
		if (dx_validate(b, n) != 0) return 1;
		for (i = 0; i < n; ++i) if (dx_validate(b, i) == 0) return 2;
		b[12] = 1;
		if (dx_validate(b, n) != 1) return 3;
		b[12] = 0;
		dx_write(b, UINT32_MAX);
		if (dx_validate(b, n) != 1) return 4;
		dx_write(b, n);
		if (!dx_range(65536, 1024, 40, n) || dx_range(65536, UINT32_MAX, 40, n)
		    || dx_range(65536, 65520, 40, n) || dx_range(65536, 1024, 16, n)) return 5;
		for (i = 0; i < n; ++i) printf("%02x", b[i]);
		puts("");
	}
	return 0;
}
