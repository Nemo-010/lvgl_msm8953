/* SPDX-License-Identifier: Apache-2.0
 *
 * Host harness: run the on-target FDT walker against a real device tree and
 * print what the display driver would discover.
 */
#include <stdio.h>
#include <stdint.h>

#include "fdt_min.h"

int main(int argc, char **argv)
{
	struct fdt_fb_info fb;
	const char *args;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <image.dtb>\n", argv[0]);
		return 2;
	}

	uintptr_t dtb = (uintptr_t)argv[1]; /* only used as an opaque value here */
	(void)dtb;

	FILE *f = fopen(argv[1], "rb");
	if (!f) { perror("open"); return 1; }
	static unsigned char buf[4 * 1024 * 1024];
	size_t n = fread(buf, 1, sizeof(buf), f);
	fclose(f);
	(void)n;

	if (!fdt_min_valid((uintptr_t)buf)) {
		printf("not a valid FDT\n");
		return 1;
	}
	printf("valid FDT\n");

	args = fdt_min_get_bootargs((uintptr_t)buf);
	printf("bootargs: %s\n", args ? args : "<none>");

	if (fdt_min_get_simplefb((uintptr_t)buf, &fb)) {
		printf("simple-framebuffer: base 0x%llx size 0x%llx %ux%u stride %u bpp %u\n",
		       (unsigned long long)fb.base, (unsigned long long)fb.size,
		       fb.width, fb.height, fb.stride, fb.bpp);
		return 0;
	}
	printf("no simple-framebuffer\n");
	return 1;
}
