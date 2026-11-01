/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr + LVGL on a Xiaomi Redmi Note 4 (MSM8953), booted by lk2nd.
 *
 * The interesting part is not the UI: it is that this image is loaded the same
 * way Linux is loaded - as an ARM64 boot image, handed over from TrustZone
 * with x0 pointing at a device tree - and that it draws by writing straight
 * into the framebuffer the bootloader is still scanning out. See
 * drivers/display/display_qcom_mdp5.c and soc/qcom/msm8953/.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>

#include <lvgl.h>

#include <soc.h>

#if defined(CONFIG_ARCH_POSIX)
#include <stdio.h>
#include <stdlib.h>
#include "mdp5_sim.h"

/* Host-side render check: give LVGL a few frames to draw, then write the
 * simulated panel out as a PPM so the result can actually be looked at.
 */
static void dump_fb_ppm(const char *path)
{
	FILE *f = fopen(path, "wb");
	unsigned char *fb = mdp5_sim_fb;

	if (!f) {
		perror(path);
		exit(1);
	}

	fprintf(f, "P6\n%u %u\n255\n", mdp5_sim_width, mdp5_sim_height);
	for (uint32_t y = 0; y < mdp5_sim_height; y++) {
		const unsigned char *row = fb + y * mdp5_sim_stride;

		for (uint32_t x = 0; x < mdp5_sim_width; x++) {
			/* MDP5 x8r8g8b8 is B,G,R,X in memory. */
			unsigned char rgb[3] = {row[x * 4 + 2], row[x * 4 + 1], row[x * 4 + 0]};

			fwrite(rgb, 1, 3, f);
		}
	}
	fclose(f);
	printf("wrote %s\n", path);
}
#endif

#define SURFACE_COLOR_HEX 0x00101828U
#define ACCENT_HEX        0x00F0A020U
#define ACCENT2_HEX       0x0020C0F0U

static const struct device *const disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

/* ---------------------------------------------------------------- splash */

static void build_ui(void)
{
	lv_obj_t *scr = lv_screen_active();
	lv_obj_t *label;

	lv_obj_set_style_bg_color(scr, lv_color_hex(SURFACE_COLOR_HEX), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

	/* Title */
	label = lv_label_create(scr);
	lv_label_set_text(label, "Hello from Zephyr");
	lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
	lv_obj_set_style_text_font(label, &lv_font_montserrat_48, LV_PART_MAIN);
	lv_obj_align(label, LV_ALIGN_CENTER, 0, -120);

	/* Subtitle */
	label = lv_label_create(scr);
	lv_label_set_text(label, "Qualcomm MSM8953  -  Redmi Note 4 (mido)\nbooted by lk2nd, drawing into the bootloader framebuffer");
	lv_obj_set_style_text_color(label, lv_color_hex(0xB0C4D8), LV_PART_MAIN);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
	lv_obj_set_style_text_font(label, &lv_font_montserrat_20, LV_PART_MAIN);
	lv_obj_align(label, LV_ALIGN_CENTER, 0, -30);

	/* Spinning arc: proves the frame really is being re-rendered. */
	lv_obj_t *arc = lv_arc_create(scr);
	lv_obj_set_size(arc, 220, 220);
	lv_obj_align(arc, LV_ALIGN_CENTER, 0, 150);
	lv_arc_set_rotation(arc, 0);
	lv_arc_set_bg_angles(arc, 0, 360);
	lv_arc_set_range(arc, 0, 100);
	lv_arc_set_value(arc, 25);
	lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
	lv_obj_set_style_arc_width(arc, 14, LV_PART_MAIN);
	lv_obj_set_style_arc_width(arc, 14, LV_PART_INDICATOR);
	lv_obj_set_style_arc_color(arc, lv_color_hex(0x2A3A4A), LV_PART_MAIN);
	lv_obj_set_style_arc_color(arc, lv_color_hex(ACCENT_HEX), LV_PART_INDICATOR);

	/* Footer */
	label = lv_label_create(scr);
	lv_label_set_text_fmt(label, "Neucom Inc. - to provide the software that shapes the world of tomorrow");
	lv_obj_set_style_text_color(label, lv_color_hex(ACCENT2_HEX), LV_PART_MAIN);
	lv_obj_set_style_text_font(label, &lv_font_montserrat_14, LV_PART_MAIN);
	lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -70);
}

/* Animation: rotate the arc and update a counter, so the screen is clearly
 * live and not a leftover bootloader splash.
 */
static void anim_cb(lv_timer_t *timer)
{
	static uint32_t ticks;
	lv_obj_t *arc = (lv_obj_t *)lv_timer_get_user_data(timer);

	ticks++;
	lv_arc_set_value(arc, (int32_t)(ticks % 101U));
	lv_arc_set_rotation(arc, (int32_t)((ticks * 3U) % 360U));
}

/*
 * First-boot sanity check for the interrupt path.
 *
 * Everything above this point (console, display probe, LVGL init) needs no
 * interrupts. The kernel tick does: if the GIC or the ARM generic timer were
 * not handed over the way we expect, k_msleep() would simply never return and
 * the phone would sit there with a rendered-but-frozen screen and no clue why.
 * So: spin on the counter for a while, and if uptime did not move, say so and
 * keep rendering with a polled delay instead.
 */
static bool tick_is_alive(void)
{
	int64_t t0 = k_uptime_get();

	k_busy_wait(250000); /* 250 ms, no interrupts involved */

	return k_uptime_get() > t0;
}

static uint32_t polled_tick;

static uint32_t polled_tick_cb(void)
{
	return polled_tick;
}

static void polled_delay_ms(uint32_t ms)
{
	k_busy_wait(ms * 1000U);
	polled_tick += ms;
}

int main(void)
{
	struct display_capabilities caps;

	printk("\n*** Zephyr on mido (MSM8953) ***\n");
#if defined(CONFIG_SOC_MSM8953)
	printk("runtime DTB from bootloader: 0x%lx\n", (unsigned long)msm8953_boot_dtb);
#endif

	if (!device_is_ready(disp)) {
		printk("display device is not ready - is the bootloader splash alive?\n");
		return 0;
	}

	display_get_capabilities(disp, &caps);
	printk("display: %ux%u, %u bpp, format %d\n", caps.x_resolution, caps.y_resolution,
	       (caps.current_pixel_format == PIXEL_FORMAT_RGB_565)   ? 16
	       : (caps.current_pixel_format == PIXEL_FORMAT_RGB_888) ? 24
								     : 32,
	       caps.current_pixel_format);

	build_ui();

	lv_obj_t *arc = lv_obj_get_child(lv_screen_active(), 2);
	lv_timer_create(anim_cb, 40, arc);

	if (!tick_is_alive()) {
		printk("*** timer interrupt never fired: GIC/timer handoff is off.\n");
		printk("*** falling back to polled rendering (screen stays live,\n");
		printk("*** animations run at whatever rate we can spin at).\n");
		lv_tick_set_cb(polled_tick_cb);

		for (;;) {
			(void)lv_timer_handler();
			polled_delay_ms(16);
		}
	}

	printk("LVGL up, entering handler loop\n");

#if defined(CONFIG_ARCH_POSIX)
	for (int i = 0; i < 400; i++) {
		uint32_t next = lv_timer_handler();

		k_msleep(next > 10U ? 10U : (next == 0U ? 1U : next));
	}
	dump_fb_ppm(getenv("MIDO_FB_PPM") ? getenv("MIDO_FB_PPM") : "mido_fb.ppm");
	exit(0);
#endif

	for (;;) {
		uint32_t next = lv_timer_handler();

		k_msleep(next > 10U ? 10U : (next == 0U ? 1U : next));
	}

	return 0;
}
