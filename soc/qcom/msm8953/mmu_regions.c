/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * MMU regions for MSM8953.
 *
 * Zephyr only maps what it is told about. Besides the kernel image itself we
 * need:
 *
 *  - the low 1 GiB of DRAM, because the bootloader's continuous-splash
 *    framebuffer lives somewhere in DDR and we do not know where until we
 *    read the MDP5 pipe registers (and because the DTB handed to us in x0
 *    sits just above 0x80000000);
 *  - the BLSP1 UART0 register window, for the console;
 *  - the MDSS/MDP5 register window, to read the splash pipe configuration.
 *
 * Device mappings use MT_DEVICE_nGnRnE and are marked non-secure, since we are
 * running in the TrustZone normal world.
 */

#include <zephyr/kernel.h>
#include <zephyr/arch/arm64/arm_mmu.h>

#include "soc.h"

#define MT_DEV	(MT_DEVICE_nGnRnE | MT_P_RW_U_NA | MT_NS)
#define MT_MEM	(MT_NORMAL | MT_P_RW_U_NA | MT_NS)

static const struct arm_mmu_region mmu_regions[] = {
	/* Framebuffer + runtime DTB live somewhere in the first GiB of DDR. */
	MMU_REGION_FLAT_ENTRY("DDR_LOW", MSM8953_DDR_BASE,
			      MSM8953_DDR_END - MSM8953_DDR_BASE, MT_MEM),

	MMU_REGION_FLAT_ENTRY("BLSP1_UART0", MSM8953_BLSP1_UART0_BASE, 0x1000, MT_DEV),
	MMU_REGION_FLAT_ENTRY("MDSS_MDP", MSM8953_MDP_BASE, 0x10000, MT_DEV),
	MMU_REGION_FLAT_ENTRY("GCC", MSM8953_GCC_BASE, 0x1000, MT_DEV),
};

const struct arm_mmu_config mmu_config = {
	.num_regions = ARRAY_SIZE(mmu_regions),
	.mmu_regions = mmu_regions,
};
