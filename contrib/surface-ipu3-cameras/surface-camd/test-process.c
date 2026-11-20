// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Tests for surface-camd's image processing on synthetic sensor frames:
 * Bayer order handling, white balance convergence, auto exposure software
 * gain, 180 degree rotation, and the IR (Y10) path. Run: make test
 */
#define SURFACE_CAMD_NO_MAIN
#include "surface-camd.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

/* Build a packed frame where each 2x2 block has the given R,G,B (10-bit, incl. black level). */
static uint8_t *make_bayer(struct cam *c, int (*rgb)(unsigned x, unsigned y, int ch))
{
	uint8_t *raw = calloc((size_t)c->bpl * c->h, 1);
	uint16_t *line = calloc(c->w, 2);

	for (unsigned y = 0; y < c->h; y++) {
		for (unsigned x = 0; x < c->w; x++) {
			int idx = (y & 1) * 2 + (x & 1);
			int ch = idx == c->r_idx ? 0 : idx == c->b_idx ? 2 : 1;
			line[x] = rgb(x / 2, y / 2, ch);
		}
		ipu3_pack_line(line, raw + (size_t)y * c->bpl, c->w);
	}
	free(line);
	return raw;
}

static int cast_grey(unsigned x, unsigned y, int ch) { (void)x; (void)y; return 64 + (ch == 0 ? 120 : ch == 1 ? 200 : 90); }
static int red_left(unsigned x, unsigned y, int ch) { (void)y; return 64 + (x < 8 ? (ch == 0 ? 600 : 40) : 200); }
static int dark(unsigned x, unsigned y, int ch) { (void)x; (void)y; (void)ch; return 64 + 10; }

static void init_out(struct cam *c, uint32_t code, unsigned w, unsigned h, unsigned ow, unsigned oh)
{
	memset(c, 0, sizeof(*c));
	c->black_level = 64; c->target = 0.18; c->saturation = 1.0;
	c->soft_gain = 1.0; c->wb_r = c->wb_b = 1.0; c->wb_bias_r = c->wb_bias_b = 1.0;
	c->sensor_fd = -1;
	c->mbus_code = code; c->w = w; c->h = h;
	c->bpl = ipu3_bytesperline(w);
	if (code == MEDIA_BUS_FMT_Y10_1X10) c->kind = KIND_MONO;
	else { c->kind = KIND_BAYER; bayer_layout(code, &c->r_idx, &c->b_idx); }
	c->out_w = ow; c->out_h = oh;
	setup_maps(c);
	c->out_size = c->kind == KIND_BAYER ? c->ow * c->oh * 2 : c->ow * c->oh;
	c->out = calloc(c->out_size, 1); c->rgb_row = calloc(c->ow, 3);
	c->line0 = calloc(w + 32, 2); c->line1 = calloc(w + 32, 2);
	build_lut(c);
}

static void init(struct cam *c, uint32_t code, unsigned w, unsigned h)
{
	init_out(c, code, w, h, 0, 0);	/* native: half size, no crop */
}

static void run(struct cam *c, const uint8_t *raw, int frames)
{
	for (int i = 0; i < frames; i++) {
		struct stats st = { 0 };
		if (c->kind == KIND_BAYER) process_bayer(c, raw, &st); else process_mono(c, raw, &st);
		update_3a(c, &st);
	}
}

/* --- regression tests for what a Surface Pro 5 user saw ------------------ */

/* Mean U and V (128 = neutral) of the output rows [y0, y1). */
static void mean_uv(const struct cam *c, unsigned y0, unsigned y1, double *u, double *v)
{
	double su = 0, sv = 0;
	unsigned n = 0;

	for (unsigned y = y0; y < y1; y++)
		for (unsigned x = 0; x + 3 < c->ow * 2; x += 4) {
			su += c->out[(size_t)y * c->ow * 2 + x + 1];
			sv += c->out[(size_t)y * c->ow * 2 + x + 3];
			n++;
		}
	*u = su / n;
	*v = sv / n;
}

/* Warm-lit (R 1.5, G 1, B 0.55) grey scene getting brighter from top to
 * bottom, on a sensor whose real black level is 16. */
static int g_true_black = 16;
static int warm_gradient(unsigned x, unsigned y, int ch)
{
	(void)x;
	double lum = y * 9.0;	/* 0 .. ~280 over 32 block rows */
	double k = ch == 0 ? 1.5 : ch == 1 ? 1.0 : 0.55;

	return g_true_black + (int)(lum * k);
}

static void test_yellow_shadows(void)
{
	struct cam c;
	double u_old, v_old, u, v;

	/* the old fixed black level of 64 on a black-16 sensor: yellow shadows */
	init(&c, MEDIA_BUS_FMT_SBGGR10_1X10, 64, 64);
	uint8_t *raw = make_bayer(&c, warm_gradient);

	run(&c, raw, 60);
	mean_uv(&c, 4, 12, &u_old, &v_old);	/* the darker rows */
	CHECK(u_old < 120, "reproduces the yellow cast with black level 64 (U %.1f)", u_old);

	/* auto black level (rear/IR default) finds 16 and the shadows are neutral */
	init(&c, MEDIA_BUS_FMT_SBGGR10_1X10, 64, 64);
	c.black_auto = true;
	run(&c, raw, 60);
	CHECK(c.black_level >= 14 && c.black_level <= 20, "auto black level %d (true 16)", c.black_level);
	mean_uv(&c, 4, 12, &u, &v);
	CHECK(fabs(u - 128) <= 4 && fabs(v - 128) <= 4, "shadows neutral with auto black: U %.1f V %.1f", u, v);
	mean_uv(&c, 20, 30, &u, &v);
	CHECK(fabs(u - 128) <= 4 && fabs(v - 128) <= 4, "bright rows neutral: U %.1f V %.1f", u, v);

	/* the front camera's fixed 16 (libcamera's value) does the same */
	init(&c, MEDIA_BUS_FMT_SBGGR10_1X10, 64, 64);
	c.black_level = 16;
	run(&c, raw, 60);
	mean_uv(&c, 4, 12, &u, &v);
	CHECK(fabs(u - 128) <= 4 && fabs(v - 128) <= 4, "shadows neutral with black 16: U %.1f V %.1f", u, v);

	/* auto never raises the black level above the truth */
	g_true_black = 64;
	free(raw);
	init(&c, MEDIA_BUS_FMT_SBGGR10_1X10, 64, 64);
	raw = make_bayer(&c, warm_gradient);
	c.black_auto = true;
	run(&c, raw, 30);
	CHECK(c.black_level == 64, "auto keeps a correct 64 (got %d)", c.black_level);
	g_true_black = 16;
	free(raw);
}

/* Left 70%: a red wall; right 30%: a grey card. Warm light on both. */
static int wall_and_card(unsigned x, unsigned y, int ch)
{
	(void)y;
	double k_light = ch == 0 ? 1.5 : ch == 1 ? 1.0 : 0.55;
	double refl = x < 22 ? (ch == 0 ? 2.0 : ch == 1 ? 1.0 : 1.1) : 1.0;	/* wall: red */

	return 16 + (int)(250 * k_light * refl);
}

static void test_awb_ignores_coloured_walls(void)
{
	struct cam c;
	double u, v;

	init(&c, MEDIA_BUS_FMT_SBGGR10_1X10, 64, 32);
	c.black_level = 16;
	uint8_t *raw = make_bayer(&c, wall_and_card);

	run(&c, raw, 150);
	/* the grey card is output columns 22..31 */
	double su = 0, sv = 0;
	int n = 0;

	for (unsigned y = 0; y < c.oh; y++)
		for (unsigned x = 24; x < 32; x += 2) {
			su += c.out[(size_t)y * c.ow * 2 + 2 * x + 1];
			sv += c.out[(size_t)y * c.ow * 2 + 2 * x + 3];
			n++;
		}
	u = su / n;
	v = sv / n;
	CHECK(fabs(u - 128) <= 5 && fabs(v - 128) <= 5, "grey card stays grey next to a red wall: U %.1f V %.1f (wb %.2f/%.2f)",
	      u, v, c.wb_r, c.wb_b);

	/* --wb-bias shifts the result the way it says */
	c.wb_bias_b = 1.2;
	run(&c, raw, 150);
	su = 0;
	for (unsigned y = 0; y < c.oh; y++)
		for (unsigned x = 24; x < 32; x += 2)
			su += c.out[(size_t)y * c.ow * 2 + 2 * x + 1];
	CHECK(su / n > u + 2, "wb-bias 1.2 on blue makes it bluer (U %.1f -> %.1f)", u, su / n);
	free(raw);
}

/* Left half of the binned image red, right half green; the top 1/8 blue. */
static int quadrants(unsigned x, unsigned y, int ch)
{
	if (y < 972 / 8)
		return 64 + (ch == 2 ? 600 : 20);
	return 64 + (x < 648 ? (ch == 0 ? 600 : 20) : (ch == 1 ? 600 : 20));
}

static void test_zoom_output_size(void)
{
	struct cam c;

	/* front camera mode 2592x1944 -> 1280x720 for video calls */
	init_out(&c, MEDIA_BUS_FMT_SBGGR10_1X10, 2592, 1944, 1280, 720);
	CHECK(c.ow == 1280 && c.oh == 720, "output %ux%u", c.ow, c.oh);
	/* 1296x972 binned, 16:9 crop = 1296x729, centred: 121 rows off the top */
	CHECK(c.xmap[0] == 0 && c.xmap[1279] == 1295, "x covers the full width: %u..%u", c.xmap[0], c.xmap[1279]);
	CHECK(c.ymap[0] >= 121 && c.ymap[0] <= 122 && c.ymap[719] >= 848 && c.ymap[719] <= 850,
	      "y is the centred 16:9 band: %u..%u", c.ymap[0], c.ymap[719]);

	uint8_t *raw = make_bayer(&c, quadrants);
	run(&c, raw, 1);
	/* top rows: the blue band is cropped away */
	CHECK(c.out[1] < 140, "no blue band at the top after cropping (U %d)", c.out[1]);
	/* left: red (V high), right: green (V low) */
	CHECK(c.out[(size_t)360 * 2560 + 3] > 150, "left is red");
	CHECK(c.out[(size_t)360 * 2560 + 2556 + 3] < 110, "right is green");
	free(raw);

	/* rear 3264x2448 -> 1280x720 (scaling down), and 4:3 output keeps everything */
	init_out(&c, MEDIA_BUS_FMT_SBGGR10_1X10, 3264, 2448, 1280, 720);
	CHECK(c.xmap[1279] < 1632 && c.ymap[719] < 1224 && c.ymap[0] > 0, "rear maps inside the image");
	init_out(&c, MEDIA_BUS_FMT_SBGGR10_1X10, 2592, 1944, 640, 480);
	CHECK(c.ymap[0] <= 1 && c.ymap[479] >= 969, "4:3 output: no crop (%u..%u)", c.ymap[0], c.ymap[479]);

	/* IR stays native */
	init(&c, MEDIA_BUS_FMT_Y10_1X10, 640, 480);
	CHECK(c.ow == 640 && c.oh == 480 && c.xmap[639] == 639 && c.ymap[479] == 479, "IR native");
}

int main(void)
{
	test_yellow_shadows();
	test_awb_ignores_coloured_walls();
	test_zoom_output_size();

	uint32_t codes[] = { MEDIA_BUS_FMT_SBGGR10_1X10, MEDIA_BUS_FMT_SGBRG10_1X10,
			     MEDIA_BUS_FMT_SGRBG10_1X10, MEDIA_BUS_FMT_SRGGB10_1X10 };
	struct cam c;

	for (unsigned k = 0; k < 4; k++) {
		/* White balance: a grey scene with a green cast should end up neutral. */
		init(&c, codes[k], 64, 32);
		uint8_t *raw = make_bayer(&c, cast_grey);
		run(&c, raw, 60);
		int u = c.out[1], v = c.out[3];
		CHECK(abs(u - 128) <= 3 && abs(v - 128) <= 3, "code %x: grey not neutral u=%d v=%d (wb %.2f/%.2f)", codes[k], u, v, c.wb_r, c.wb_b);
		free(raw);

		/* Bayer order: a red area must come out red (V >> 128), the rest not. */
		init(&c, codes[k], 64, 32);
		raw = make_bayer(&c, red_left);
		run(&c, raw, 1);
		CHECK(c.out[3] > 160, "code %x: red area V=%d", codes[k], c.out[3]);
		CHECK(c.out[(c.ow - 2) * 2 + 3] < 140, "code %x: right side V=%d", codes[k], c.out[(c.ow - 2) * 2 + 3]);

		/* 180 degree rotation moves the red area to the right edge. */
		c.rot180 = true;
		run(&c, raw, 1);
		CHECK(c.out[(c.ow - 2) * 2 + 3] > 160 && c.out[3] < 140, "code %x: rotation", codes[k]);
		free(raw);
	}

	/* Without sensor controls, auto exposure falls back to software gain on dark scenes. */
	init(&c, MEDIA_BUS_FMT_SBGGR10_1X10, 64, 32);
	uint8_t *raw = make_bayer(&c, dark);
	run(&c, raw, 30);
	CHECK(c.soft_gain > 3.5, "dark scene soft gain %.2f", c.soft_gain);
	free(raw);

	/* IR / Y10 path: brightness maps monotonically, rotation reverses. */
	init(&c, MEDIA_BUS_FMT_Y10_1X10, 50, 4);
	{
		uint8_t *r = calloc((size_t)c.bpl * c.h, 1);
		uint16_t line[50];
		for (unsigned y = 0; y < c.h; y++) {
			for (unsigned x = 0; x < 50; x++) line[x] = 64 + x * 19;
			ipu3_pack_line(line, r + (size_t)y * c.bpl, 50);
		}
		run(&c, r, 1);
		CHECK(c.out[0] == 0 && c.out[49] > 240 && c.out[10] < c.out[20], "IR ramp %d..%d", c.out[0], c.out[49]);
		c.rot180 = true;
		run(&c, r, 1);
		CHECK(c.out[49] == 0 && c.out[0] > 240, "IR rotation");
		free(r);
	}

	printf(fails ? "%d test(s) failed\n" : "all processing tests passed\n", fails);
	return fails != 0;
}
