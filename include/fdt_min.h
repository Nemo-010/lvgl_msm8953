/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal read-only flattened device tree walker.
 *
 * Zephyr builds its devicetree at compile time, but lk2nd hands us a runtime
 * tree in x0 that carries things we cannot know statically - most notably the
 * "simple-framebuffer" node it injects when the kernel command line asks for
 * lk2nd.pass-simplefb. This is just enough of libfdt to find that node.
 */

#ifndef MIDO_FDT_MIN_H
#define MIDO_FDT_MIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FDT_MIN_MAGIC 0xd00dfeedU

struct fdt_fb_info {
	bool valid;
	uint64_t base;
	uint64_t size;
	uint32_t width;
	uint32_t height;
	uint32_t stride;	/* bytes per line */
	uint8_t bpp;
};

/* Returns true if @dtb looks like a flattened device tree. */
bool fdt_min_valid(uintptr_t dtb);

/* Find the "simple-framebuffer" node and decode its geometry. */
bool fdt_min_get_simplefb(uintptr_t dtb, struct fdt_fb_info *out);

/* Pointer to /chosen/bootargs, or NULL. Points into @dtb, do not free. */
const char *fdt_min_get_bootargs(uintptr_t dtb);

#endif /* MIDO_FDT_MIN_H */
