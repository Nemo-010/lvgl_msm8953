/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * MMU regions for MSM8953.
 *
 * Zephyr only maps what it is told about. Besides the kernel image itself we
 * need:
 *
 *  - the DDR around the image, because the bootloader's continuous-splash
 *    framebuffer lives somewhere in DDR and we do not know where until we read
 *    the MDP5 pipe registers, and because the device tree lk2nd hands over in
 *    x0 sits above the image (0x83400000 by default);
 *  - the BLSP1 UART0 register window, for the console;
 *  - the MDSS/MDP5 register window, to read the splash pipe configuration.
 *
 * The DDR window must be split around the kernel image: Zephyr maps the image
 * itself first and then adds these regions with MT_NO_OVERWRITE, and a region
 * that overlaps an existing mapping is rejected as a whole. A single
 * 0x80000000..0xc0000000 entry therefore maps *nothing* - which is how a
 * runtime DTB read ended up faulting.
 *
 * Device mappings are non-secure, since we run in the TrustZone normal world.
 */

#include <zephyr/kernel.h>
#include <zephyr/arch/arm64/arm_mmu.h>
#include <zephyr/linker/linker-defs.h>

#include "soc.h"

#define MT_DEV	(MT_DEVICE_nGnRnE | MT_P_RW_U_NA | MT_NS)
#define MT_MEM	(MT_NORMAL | MT_P_RW_U_NA | MT_NS)

static const struct arm_mmu_region mmu_regions[] = {
	/*
	 * The device tree lk2nd copies before jumping: ABOOT_FORCE_TAGS_ADDR is
	 * DDR_START + 0x3400000 on msm8953, a fixed address, and the tree we
	 * append is ~65 KiB. Mapping it is what makes the fwargs hook's x0
	 * readable.
	 *
	 * The framebuffer is *not* mapped here: its address is only known at
	 * runtime, so display_qcom_mdp5.c maps exactly that range with
	 * arch_mem_map() once it has read it out of the MDP5 registers.
	 */
	MMU_REGION_FLAT_ENTRY("BOOT_DTB", MSM8953_BOOT_DTB_ADDR, MSM8953_BOOT_DTB_SIZE, MT_MEM),

	MMU_REGION_FLAT_ENTRY("BLSP1_UART0", MSM8953_BLSP1_UART0_BASE, 0x1000, MT_DEV),
	MMU_REGION_FLAT_ENTRY("MDSS_MDP", MSM8953_MDP_BASE, 0x10000, MT_DEV),
	MMU_REGION_FLAT_ENTRY("GCC", MSM8953_GCC_BASE, 0x1000, MT_DEV),
};

const struct arm_mmu_config mmu_config = {
	.num_regions = ARRAY_SIZE(mmu_regions),
	.mmu_regions = mmu_regions,
};
