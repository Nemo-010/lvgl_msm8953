/* SPDX-License-Identifier: Apache-2.0 */
#ifndef _SOC_QCOM_MSM8953_SOC_H_
#define _SOC_QCOM_MSM8953_SOC_H_

/* Physical memory map, from the downstream msm-3.18 / msm-4.9 iomap.h */
#define MSM8953_DDR_BASE		0x80000000ULL
#define MSM8953_DDR_END			0xC0000000ULL	/* first GiB, always populated */
#define MSM8953_PERIPH_BASE		0x07800000ULL
#define MSM8953_BLSP1_UART0_BASE	0x078AF000ULL	/* debug UART, lk2nd console */
#define MSM8953_MDP_BASE		0x01A00000ULL	/* MDSS MDP5 */
#define MSM8953_GCC_BASE		0x01800000ULL
/* lk2nd's ABOOT_FORCE_TAGS_ADDR: DDR_START + 0x3400000 */
#define MSM8953_BOOT_DTB_ADDR		0x83400000ULL
#define MSM8953_BOOT_DTB_SIZE		0x40000ULL	/* 256 KiB, our DTB is 65 KiB */

#define MSM8953_GIC_DIST_BASE		0x0B000000ULL
#define MSM8953_GIC_CPU_BASE		0x0B002000ULL

/* Boot hand-off: lk2nd jumps to the image with x0 = physical DTB address. */
extern uintptr_t msm8953_boot_dtb;

#endif /* _SOC_QCOM_MSM8953_SOC_H_ */
