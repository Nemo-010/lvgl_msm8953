/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Display driver for the framebuffer the bootloader left behind on MSM8953.
 *
 * This is a port of the "continuous splash" pipe decoding from Linux
 * (drivers/gpu/drm/msm/mdp/mdp5/mdp5_plane.c, mdp5_pipe) as it is also done by
 * lk2nd in lk2nd/display/cont-splash/pipe.c: the MDSS MDP5 scanout pipe still
 * holds the surface address, geometry, stride and pixel format that the
 * primary bootloader programmed. We read them back, and from then on we own
 * the pixels - the MDP keeps fetching them by itself, so writing to that
 * memory updates the panel.
 *
 * What this is *not*: a DSI driver. There is no PLL programming, no DSI PHY,
 * no panel init sequence, no TE handling. Those are all done, once, by the
 * stock aboot before lk2nd ever runs, and nothing in the boot chain between
 * then and us turns them off.
 *
 * Two sources for the surface geometry, in order:
 *
 *  1. the runtime devicetree in x0, if lk2nd injected a "simple-framebuffer"
 *     node (kernel cmdline carries lk2nd.pass-simplefb) - this is exact, as
 *     lk2nd read the same registers;
 *  2. the MDP5 SSPP registers themselves, which also works on a boot chain
 *     that did not pass the node.
 */

#define DT_DRV_COMPAT qcom_mdp5_cont_splash

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/cache.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/sys_io.h>

#if defined(CONFIG_SOC_MSM8953)
#include <soc.h>
#define BOOT_DTB msm8953_boot_dtb
#else
#define BOOT_DTB 0
#endif

#if defined(CONFIG_ARCH_POSIX)
/* native_sim: the register block is provided by tests/sim/mdp5_sim.c */
#include "mdp5_sim.h"
#endif

#include "fdt_min.h"

LOG_MODULE_REGISTER(display_mdp5, CONFIG_DISPLAY_LOG_LEVEL);

/* MDP5 SSPP (source surface processor) register offsets, from mdp5.xml */
#define SSPP_SRC_SIZE		0x000
#define SSPP_SRC_IMG_SIZE	0x004
#define SSPP_SRC_XY		0x008
#define SSPP_SRC_OUT_SIZE	0x00C
#define SSPP_OUT_XY		0x010
#define SSPP_SRC0_ADDR		0x014
#define SSPP_SRC_YSTRIDE	0x024
#define SSPP_SRC_FORMAT		0x030

#define SSPP_FMT_SRC_BPP(fmt)	((((fmt) >> 9) & 0x3U) + 1U)

/* SSPP instance offsets from the MDP base, MSM8953 (mdss_mdp @ 0x1a00000) */
#define MDP5_VIG_0		0x05000
#define MDP5_RGB_0		0x15000
#define MDP5_DMA_0		0x25000

struct mdp5_pipe {
	const char *name;
	uint32_t off;
};

static const struct mdp5_pipe mdp5_pipes[] = {
	{ "VIG_0", MDP5_VIG_0 },
	{ "RGB_0", MDP5_RGB_0 },
	{ "DMA_0", MDP5_DMA_0 },
};

struct mdp5_config {
	mm_reg_t base;
};

struct mdp5_surface {
	bool valid;
	uint8_t *fb;
	uint32_t width;
	uint32_t height;
	uint32_t stride;	/* bytes per line */
	uint8_t bpp;
	enum display_pixel_format fmt;
	const char *pipe;
	const char *source;
};

static struct mdp5_surface surf;

static inline uint32_t mdp_rd(mm_reg_t base, uint32_t off)
{
#if defined(CONFIG_ARCH_POSIX)
	ARG_UNUSED(base);
	return mdp5_sim_read(off);
#else
	return sys_read32(base + off);
#endif
}

static void decode_and_store(const char *source, const char *pipe, uint64_t base, uint32_t width,
			     uint32_t height, uint32_t stride, uint8_t bpp)
{
	surf.valid = false;

	if (base == 0U || width == 0U || height == 0U || stride == 0U) {
		return;
	}
	if (bpp != 2U && bpp != 3U && bpp != 4U) {
		LOG_ERR("unsupported bpp %u", bpp);
		return;
	}

	surf.fb = (uint8_t *)(uintptr_t)base;
	surf.width = width;
	surf.height = height;
	surf.stride = stride;
	surf.bpp = bpp;
	surf.pipe = pipe;
	surf.source = source;

	switch (bpp) {
	case 2:
		surf.fmt = PIXEL_FORMAT_RGB_565;
		break;
	case 3:
		surf.fmt = PIXEL_FORMAT_RGB_888;
		break;
	default:
		surf.fmt = PIXEL_FORMAT_ARGB_8888;
		break;
	}

	surf.valid = true;
}

static bool probe_dtb(uintptr_t dtb)
{
	struct fdt_fb_info fb;
	const char *args;

#if !IS_ENABLED(CONFIG_DISPLAY_QCOM_MDP5_CONT_SPLASH_DTB_OVERRIDE)
	return false;
#endif
	if (dtb == 0U) {
		LOG_WRN("no runtime devicetree pointer from bootloader");
		return false;
	}
	if (!fdt_min_valid(dtb)) {
		LOG_WRN("runtime devicetree at 0x%lx is not a valid FDT", (unsigned long)dtb);
		return false;
	}

	args = fdt_min_get_bootargs(dtb);
	LOG_INF("runtime DTB at 0x%lx, bootargs: \"%s\"", (unsigned long)dtb,
		args != NULL ? args : "<none>");

	if (!fdt_min_get_simplefb(dtb, &fb)) {
		LOG_WRN("no simple-framebuffer node (cmdline needs lk2nd.pass-simplefb)");
		return false;
	}

	decode_and_store("dtb", "simplefb", fb.base, fb.width, fb.height, fb.stride, fb.bpp);
	return surf.valid;
}

static bool probe_mdp5(const struct device *dev)
{
	const struct mdp5_config *cfg = dev->config;

	for (size_t i = 0U; i < ARRAY_SIZE(mdp5_pipes); i++) {
		const struct mdp5_pipe *pipe = &mdp5_pipes[i];
		uint32_t addr = mdp_rd(cfg->base, pipe->off + SSPP_SRC0_ADDR);
		uint32_t img, stride, fmt;
		uint8_t bpp;

		if (addr == 0U) {
			continue;
		}

		img = mdp_rd(cfg->base, pipe->off + SSPP_SRC_IMG_SIZE);
		stride = mdp_rd(cfg->base, pipe->off + SSPP_SRC_YSTRIDE);
		fmt = mdp_rd(cfg->base, pipe->off + SSPP_SRC_FORMAT);
		bpp = (uint8_t)SSPP_FMT_SRC_BPP(fmt);

		LOG_INF("%s: src0=0x%08x img=%ux%u stride=%u fmt=0x%08x bpp=%u", pipe->name, addr,
			img & 0xFFFFU, img >> 16, stride, fmt, bpp);

		decode_and_store("mdp5", pipe->name, addr, img & 0xFFFFU, img >> 16, stride, bpp);
		if (surf.valid) {
			return true;
		}
	}

	return false;
}

static void mdp5_get_capabilities(const struct device *dev, struct display_capabilities *caps)
{
	ARG_UNUSED(dev);

	memset(caps, 0, sizeof(*caps));
	caps->x_resolution = surf.width;
	caps->y_resolution = surf.height;
	caps->supported_pixel_formats = (enum display_pixel_format)BIT(surf.fmt);
	caps->current_pixel_format = surf.fmt;
	caps->current_orientation = DISPLAY_ORIENTATION_NORMAL;
}

static int mdp5_write(const struct device *dev, const uint16_t x, const uint16_t y,
		      const struct display_buffer_descriptor *desc, const void *buf)
{
	uint32_t row_bytes;
	const uint8_t *src = buf;

	ARG_UNUSED(dev);

	if (!surf.valid) {
		return -ENODEV;
	}
	if (buf == NULL) {
		return -EINVAL;
	}

	/* The block must fit on the surface. */
	if ((uint32_t)x + desc->width > surf.width || (uint32_t)y + desc->height > surf.height) {
		return -EINVAL;
	}
	if (desc->pitch < desc->width) {
		return -EINVAL;
	}

	row_bytes = desc->width * surf.bpp;

	for (uint32_t row = 0U; row < desc->height; row++) {
		uint8_t *dst = surf.fb + (uint32_t)(y + row) * surf.stride + (uint32_t)x * surf.bpp;

		memcpy(dst, src, row_bytes);

		/* The MDP is a DMA master reading this line from DRAM. */
		sys_cache_data_flush_range(dst, row_bytes);

		src += desc->pitch * surf.bpp;
	}

	return 0;
}

static int mdp5_blanking_on(const struct device *dev)
{
	ARG_UNUSED(dev);

	return 0;
}

static int mdp5_blanking_off(const struct device *dev)
{
	ARG_UNUSED(dev);

	return 0;
}

static DEVICE_API(display, mdp5_api) = {
	.blanking_on = mdp5_blanking_on,
	.blanking_off = mdp5_blanking_off,
	.write = mdp5_write,
	.get_capabilities = mdp5_get_capabilities,
};

static int mdp5_init(const struct device *dev)
{
	if (probe_dtb(BOOT_DTB)) {
		/* nothing more to do */
	} else if (probe_mdp5(dev)) {
		/* nothing more to do */
	} else {
		LOG_ERR("no continuous-splash surface found");
		return -ENODEV;
	}

	LOG_INF("framebuffer 0x%lx %ux%u stride %u bpp %u via %s (%s)", (unsigned long)surf.fb,
		surf.width, surf.height, surf.stride, surf.bpp, surf.source, surf.pipe);

	return 0;
}

#define MDP5_DEVICE(n)                                                                             \
	static const struct mdp5_config mdp5_cfg_##n = {                                           \
		.base = DT_INST_REG_ADDR(n),                                                       \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(n, mdp5_init, NULL, NULL, &mdp5_cfg_##n, POST_KERNEL,                \
			      CONFIG_DISPLAY_INIT_PRIORITY, &mdp5_api);

DT_INST_FOREACH_STATUS_OKAY(MDP5_DEVICE)
