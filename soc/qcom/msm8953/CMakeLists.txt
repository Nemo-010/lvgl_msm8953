# SPDX-License-Identifier: Apache-2.0

zephyr_include_directories(.)

set(SOC_LINKER_SCRIPT ${ZEPHYR_BASE}/include/zephyr/arch/arm64/scripts/linker.ld CACHE INTERNAL "")

zephyr_sources(
  mmu_regions.c
  fwargs.c
)
