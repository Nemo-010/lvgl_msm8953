/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-side stand-in for the MSM8953 MDP5 register block, used to run the real
 * display driver on native_sim. It answers register reads the way a bootloader
 * that left a 1080x1920 x8r8g8b8 surface scanning out would, and owns the
 * framebuffer memory the driver then draws into.
 */
#ifndef MIDO_MDP5_SIM_H
#define MIDO_MDP5_SIM_H

#include <stdint.h>
#include <stddef.h>

/* Register read, offset from the MDP base. */
uint32_t mdp5_sim_read(uint32_t off);

/* Framebuffer the simulated pipe points at. */
extern uint8_t *mdp5_sim_fb;
extern const uint32_t mdp5_sim_width;
extern const uint32_t mdp5_sim_height;
extern const uint32_t mdp5_sim_stride;
extern const uint32_t mdp5_sim_bpp;

#endif /* MIDO_MDP5_SIM_H */
