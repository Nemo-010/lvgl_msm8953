/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Capture the boot firmware arguments.
 *
 * lk2nd boots ARM64 payloads the same way it boots Linux: it fills in the
 * device tree, then issues the TrustZone "elexec" SCM call, which drops the
 * payload into non-secure EL1 with x0 pointing at the flattened device tree.
 * Zephyr builds its own devicetree at compile time, but the runtime tree is
 * the only place that describes things the bootloader configured for us -
 * most importantly the "simple-framebuffer" node that lk2nd injects when the
 * kernel command line asks for lk2nd.pass-simplefb.
 *
 * This hook runs after .bss has been zeroed and before the MMU is enabled, so
 * the pointer is simply saved and dereferenced later, once DRAM is mapped.
 */

#include <zephyr/kernel.h>
#include <zephyr/platform/fwargs.h>

uintptr_t msm8953_boot_dtb;

void fwargs_handler_hook(const uintptr_t *args, size_t argc)
{
	if (argc > 0U) {
		msm8953_boot_dtb = args[0];
	}
}
