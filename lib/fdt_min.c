/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal read-only flattened device tree walker, see include/fdt_min.h.
 *
 * All FDT integers are big-endian, so the header and every value read here goes
 * through an explicit big-endian load; a native load of the magic would see
 * 0xedfe0dd0 on a little-endian machine.
 *
 * Kept free of Zephyr headers so the exact same code can be exercised on the
 * host against a real device tree (tests/host).
 */

#include <string.h>

#ifndef ARG_UNUSED
#define ARG_UNUSED(x) (void)(x)
#endif

#include "fdt_min.h"

#define FDT_BEGIN_NODE 1U
#define FDT_END_NODE   2U
#define FDT_PROP       3U
#define FDT_NOP        4U
#define FDT_END        9U

#define FDT_HEADER_SIZE 40U

struct fdt_hdr {
	uint32_t magic;
	uint32_t totalsize;
	uint32_t off_dt_struct;
	uint32_t off_dt_strings;
	uint32_t off_mem_rsvmap;
	uint32_t version;
	uint32_t last_comp_version;
	uint32_t boot_cpuid_phys;
	uint32_t size_dt_strings;
	uint32_t size_dt_struct;
};

static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
	       (uint32_t)p[3];
}

static uint64_t be64(const uint8_t *p)
{
	return ((uint64_t)be32(p) << 32) | (uint64_t)be32(p + 4);
}

static void hdr_read(uintptr_t dtb, struct fdt_hdr *h)
{
	const uint8_t *p = (const uint8_t *)dtb;
	uint32_t *f = (uint32_t *)h;

	for (unsigned int i = 0U; i < sizeof(*h) / sizeof(uint32_t); i++) {
		f[i] = be32(p + 4U * i);
	}
}

static bool range_ok(const struct fdt_hdr *h, uint32_t off, uint32_t len)
{
	return (off <= h->totalsize) && (len <= h->totalsize - off);
}

bool fdt_min_valid(uintptr_t dtb)
{
	struct fdt_hdr h;

	if (dtb == 0U) {
		return false;
	}

	hdr_read(dtb, &h);

	if (h.magic != FDT_MIN_MAGIC) {
		return false;
	}
	if (h.totalsize < FDT_HEADER_SIZE || h.totalsize > (16U * 1024U * 1024U)) {
		return false;
	}
	if (!range_ok(&h, h.off_dt_struct, 8U) || !range_ok(&h, h.off_dt_strings, 8U)) {
		return false;
	}
	if (h.version < 16U || h.version > 17U) {
		return false;
	}

	return true;
}

struct walk {
	uintptr_t dtb;
	const struct fdt_hdr *h;
	uint32_t addr_cells;
	uint32_t size_cells;

	/* Node under construction */
	bool node_is_fb;
	uint64_t cur_base;
	uint64_t cur_size;
	uint32_t cur_w;
	uint32_t cur_h;
	uint32_t cur_stride;
	uint8_t cur_bpp;

	/* Result */
	struct fdt_fb_info fb;
	bool have_fb;
	bool in_chosen_now;
	const char *bootargs;
};

static void parse_fb_prop(struct walk *w, const char *name, const uint8_t *val, uint32_t len)
{
	if (strcmp(name, "reg") == 0) {
		if (w->addr_cells == 2U && w->size_cells == 2U && len >= 16U) {
			w->cur_base = be64(val);
			w->cur_size = be64(val + 8);
		} else if (w->addr_cells == 1U && w->size_cells == 1U && len >= 8U) {
			w->cur_base = be32(val);
			w->cur_size = be32(val + 4);
		} else if (w->addr_cells == 2U && w->size_cells == 1U && len >= 12U) {
			w->cur_base = be64(val);
			w->cur_size = be32(val + 8);
		}
	} else if (strcmp(name, "width") == 0 && len >= 4U) {
		w->cur_w = be32(val);
	} else if (strcmp(name, "height") == 0 && len >= 4U) {
		w->cur_h = be32(val);
	} else if (strcmp(name, "stride") == 0 && len >= 4U) {
		w->cur_stride = be32(val);
	} else if (strcmp(name, "format") == 0) {
		/* Stored as bytes per pixel, which is what the driver uses. */
		if (len >= 6U && memcmp(val, "r5g6b5", 6) == 0) {
			w->cur_bpp = 2U;
		} else if (len >= 6U && memcmp(val, "r8g8b8", 6) == 0) {
			w->cur_bpp = 3U;
		} else if (len >= 8U && memcmp(val, "x8r8g8b8", 8) == 0) {
			w->cur_bpp = 4U;
		}
	}
}

static bool compat_is_fb(const uint8_t *val, uint32_t len)
{
	uint32_t off = 0U;

	while (off < len) {
		const char *c = (const char *)val + off;
		size_t n = strlen(c);

		if (strcmp(c, "simple-framebuffer") == 0) {
			return true;
		}
		off += (uint32_t)n + 1U;
	}

	return false;
}

static void reset_node(struct walk *w)
{
	w->node_is_fb = false;
	w->cur_base = 0U;
	w->cur_size = 0U;
	w->cur_w = 0U;
	w->cur_h = 0U;
	w->cur_stride = 0U;
	w->cur_bpp = 0U;
}

static void finish_node(struct walk *w)
{
	if (!w->node_is_fb || w->have_fb) {
		w->node_is_fb = false;
		return;
	}

	w->fb.base = w->cur_base;
	w->fb.size = w->cur_size;
	w->fb.width = w->cur_w;
	w->fb.height = w->cur_h;
	w->fb.stride = w->cur_stride;
	w->fb.bpp = w->cur_bpp;
	w->fb.valid = (w->cur_base != 0U) && (w->cur_w != 0U) && (w->cur_h != 0U) &&
		      (w->cur_stride != 0U) && (w->cur_bpp != 0U);

	if (w->fb.valid) {
		w->have_fb = true;
	}

	w->node_is_fb = false;
}

static void walk_tree(uintptr_t dtb, struct walk *w)
{
	struct fdt_hdr h;
	const uint8_t *p;
	const uint8_t *end;
	unsigned int depth = 0U;

	memset(w, 0, sizeof(*w));
	w->dtb = dtb;
	hdr_read(dtb, &h);
	w->h = &h;

	/* Root defaults, as used when the DT does not say otherwise. */
	w->addr_cells = 2U;
	w->size_cells = 2U;

	p = (const uint8_t *)dtb + h.off_dt_struct;
	end = p + h.size_dt_struct;

	reset_node(w);

	while ((p + 4U) <= end) {
		uint32_t tok = be32(p);

		p += 4U;

		if (tok == FDT_BEGIN_NODE) {
			const char *name = (const char *)p;
			size_t n = strlen(name);
			bool is_chosen;

			/* Opening a node ends the previous one. */
			finish_node(w);
			reset_node(w);

			depth++;
			is_chosen = (depth == 2U) && (strcmp(name, "chosen") == 0);

			/* Remember whether the node we just opened is /chosen by
			 * encoding it in a sentinel: the bootargs property only
			 * exists there, and it is a direct child property.
			 */
			w->in_chosen_now = is_chosen;

			p += (n + 4U) & ~3U;
			if (p > end) {
				break;
			}
		} else if (tok == FDT_END_NODE) {
			finish_node(w);
			if (depth == 0U) {
				break;
			}
			depth--;
			w->in_chosen_now = false;
		} else if (tok == FDT_PROP) {
			uint32_t len, nameoff;
			const uint8_t *val;
			const char *name;

			if ((p + 8U) > end) {
				break;
			}
			len = be32(p);
			nameoff = be32(p + 4);
			p += 8U;
			if ((p + len) > end) {
				break;
			}
			val = p;

			if (nameoff >= h.size_dt_strings) {
				break;
			}
			name = (const char *)dtb + h.off_dt_strings + nameoff;

			if (depth == 1U && strcmp(name, "#address-cells") == 0 && len >= 4U) {
				w->addr_cells = be32(val);
			} else if (depth == 1U && strcmp(name, "#size-cells") == 0 && len >= 4U) {
				w->size_cells = be32(val);
			} else if (strcmp(name, "compatible") == 0) {
				if (compat_is_fb(val, len)) {
					w->node_is_fb = true;
				}
			} else if (w->node_is_fb) {
				parse_fb_prop(w, name, val, len);
			} else if (w->in_chosen_now && strcmp(name, "bootargs") == 0) {
				w->bootargs = (const char *)val;
			}

			p += (len + 3U) & ~3U;
			if (p > end) {
				break;
			}
		} else if (tok == FDT_END) {
			break;
		} else if (tok == FDT_NOP) {
			continue;
		} else {
			break;
		}
	}
}

bool fdt_min_get_simplefb(uintptr_t dtb, struct fdt_fb_info *out)
{
	struct walk w;

	if (!fdt_min_valid(dtb)) {
		return false;
	}

	walk_tree(dtb, &w);

	if (out != NULL) {
		*out = w.fb;
	}

	return w.have_fb;
}

const char *fdt_min_get_bootargs(uintptr_t dtb)
{
	struct walk w;

	if (!fdt_min_valid(dtb)) {
		return NULL;
	}

	walk_tree(dtb, &w);

	return w.bootargs;
}
