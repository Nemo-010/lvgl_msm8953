/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Simulated MDP5 SSPP register block + framebuffer for native_sim.
 *
 * The point is not to emulate MDP5: it is to let the *unmodified* display
 * driver, blit path and LVGL glue run on the host so the pixel output can be
 * inspected. The register values below are what mido's bootloader leaves in
 * VIG_0 for its 1080x1920 32bpp splash.
 *
 * The surface address has to fit in the 32-bit SRC0_ADDR register, so the
 * buffer is mapped below 4 GiB (MAP_32BIT). That keeps the driver's own
 * `(uint8_t *)(uintptr_t)addr` conversion honest instead of special-casing the
 * host.
 */

#include <string.h>
#include <sys/mman.h>

#include <zephyr/kernel.h>

#include "mdp5_sim.h"

/* SSPP register offsets (see drivers/display/display_qcom_mdp5.c) */
#define SSPP_SRC_IMG_SIZE  0x004
#define SSPP_SRC0_ADDR     0x014
#define SSPP_SRC_YSTRIDE   0x024
#define SSPP_SRC_FORMAT    0x030

#define VIG_0              0x05000
#define REG_BLOCK_WORDS    (0x10000 / 4)

static uint32_t regs[REG_BLOCK_WORDS];

uint8_t *mdp5_sim_fb;
const uint32_t mdp5_sim_width = 1080;
const uint32_t mdp5_sim_height = 1920;
const uint32_t mdp5_sim_stride = 1080 * 4;
const uint32_t mdp5_sim_bpp = 32;

uint32_t mdp5_sim_read(uint32_t off)
{
	if ((off / 4) < REG_BLOCK_WORDS) {
		return regs[off / 4];
	}
	return 0;
}

static int mdp5_sim_init(void)
{
	size_t size = (size_t)mdp5_sim_stride * mdp5_sim_height;
	uint8_t *fb = mmap(NULL, size, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);

	if (fb == MAP_FAILED) {
		return -1;
	}

	mdp5_sim_fb = fb;

	/* Paint something so it is obvious if the UI never overwrites it. */
	for (size_t i = 0; i < size; i += 4) {
		fb[i + 0] = 0x20;
		fb[i + 1] = 0x00;
		fb[i + 2] = 0x20;
		fb[i + 3] = 0x00;
	}

	regs[(VIG_0 + SSPP_SRC_IMG_SIZE) / 4] =
		((uint32_t)mdp5_sim_height << 16) | (uint32_t)mdp5_sim_width;
	regs[(VIG_0 + SSPP_SRC_YSTRIDE) / 4] = (uint32_t)mdp5_sim_stride;
	/* SRC_BPP field = ((fmt >> 9) & 3) + 1 -> 4 bytes per pixel */
	regs[(VIG_0 + SSPP_SRC_FORMAT) / 4] =
		(0x3U << 9) | (0x3U << 6) | (0x3U << 4) | (0x3U << 2) | 0x3U;
	regs[(VIG_0 + SSPP_SRC0_ADDR) / 4] = (uint32_t)(uintptr_t)fb;

	return 0;
}

SYS_INIT(mdp5_sim_init, PRE_KERNEL_1, 0);
