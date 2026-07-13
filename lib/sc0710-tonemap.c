/*
 * Host-side HDR PQ → SDR tonemap (MK.2 preview path).
 *
 * BGR24: luminance-preserving map. Defaults tm_bgr_*=50/400/150/150 (live).
 *        Baked gold path (T=0.58 / paper 203) still used when knobs match those.
 * YUYV:  live Reinhard on limited Y'. Defaults tm_yuyv_*=80/225/100/300.
 *
 * Curve LUTs: scripts/gen-tonemap-luts.py (BGR24 baked at paper 203 / T=0.58).
 */

#include <linux/types.h>
#include <linux/kernel.h>
#include "sc0710.h"
#include "sc0710-tonemap-luts.h"

static inline int sc0710_tm_clamp_int(int v, int lo, int hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

static inline u8 sc0710_tm_lin_q16_to_srgb(u32 lin_q16)
{
	u32 idx = lin_q16 >> 6; /* 16-bit → 10-bit index */

	if (idx > 1023)
		idx = 1023;
	return sc0710_tm_lin_q10_to_srgb[idx];
}

static inline u8 sc0710_tm_apply_u8_gain(u8 v, int gain)
{
	int out;

	if (gain == 100)
		return v;
	out = (v * gain) / 100;
	if (out > 255)
		out = 255;
	return (u8)out;
}

/* Post-tonemap saturation: out = Y + sat% · (C − Y). Rec.709 luma on SDR codes. */
static inline void sc0710_tm_apply_sat_u8(u8 *b, u8 *g, u8 *r, int sat)
{
	int y, bo, go, ro;

	if (sat == 100)
		return;
	/* Y ≈ (54·R + 183·G + 19·B) / 256 */
	y = (19 * (*b) + 183 * (*g) + 54 * (*r)) >> 8;
	bo = y + (((*b) - y) * sat) / 100;
	go = y + (((*g) - y) * sat) / 100;
	ro = y + (((*r) - y) * sat) / 100;
	if (bo < 0)
		bo = 0;
	else if (bo > 255)
		bo = 255;
	if (go < 0)
		go = 0;
	else if (go > 255)
		go = 255;
	if (ro < 0)
		ro = 0;
	else if (ro > 255)
		ro = 255;
	*b = (u8)bo;
	*g = (u8)go;
	*r = (u8)ro;
}

/*
 * Gold-standard BGR24 path (identical to sc0710-software-hdr):
 * baked Reinhard LUT + fixed paper 203. Optional post RGB gain only.
 */
static void sc0710_tm_rgb_pq_pixel_baked(u8 *b, u8 *g, u8 *r, int gain)
{
	u32 bn = sc0710_tm_pq_to_nits_q12[*b]; /* nits × 4096 */
	u32 gn = sc0710_tm_pq_to_nits_q12[*g];
	u32 rn = sc0710_tm_pq_to_nits_q12[*r];
	/* BT.2020 luma on linear nits (Q12): 0.2627/0.6780/0.0593 */
	u64 y_q12 = (689ull * rn + 1778ull * gn + 155ull * bn) >> 12;
	u32 y_nits = (u32)(y_q12 >> 12);
	u32 y_out_q16;
	u32 y_rel_q16;
	u32 scale_q16;
	u64 bo, go, ro;
	u32 b_nits = bn >> 12;
	u32 g_nits = gn >> 12;
	u32 r_nits = rn >> 12;
	u32 b_rel, g_rel, r_rel;

	if (y_nits == 0) {
		*b = *g = *r = 0;
		return;
	}

	if (y_nits > 4095)
		y_nits = 4095;
	y_out_q16 = sc0710_tm_nits_to_lin_q16[y_nits];

	/* Relative linear vs paper white (203 nits), Q16. */
	y_rel_q16 = (y_nits << 16) / SC0710_TM_PAPER_NITS;
	if (y_rel_q16 == 0)
		y_rel_q16 = 1;
	scale_q16 = (u32)(((u64)y_out_q16 << 16) / y_rel_q16);
	if (scale_q16 > 0x30000) /* cap 3× */
		scale_q16 = 0x30000;

	/* Channel relative × scale → SDR linear Q16. */
	b_rel = (b_nits << 16) / SC0710_TM_PAPER_NITS;
	g_rel = (g_nits << 16) / SC0710_TM_PAPER_NITS;
	r_rel = (r_nits << 16) / SC0710_TM_PAPER_NITS;
	bo = ((u64)b_rel * scale_q16) >> 16;
	go = ((u64)g_rel * scale_q16) >> 16;
	ro = ((u64)r_rel * scale_q16) >> 16;
	if (bo > 65535)
		bo = 65535;
	if (go > 65535)
		go = 65535;
	if (ro > 65535)
		ro = 65535;

	*b = sc0710_tm_apply_u8_gain(sc0710_tm_lin_q16_to_srgb((u32)bo), gain);
	*g = sc0710_tm_apply_u8_gain(sc0710_tm_lin_q16_to_srgb((u32)go), gain);
	*r = sc0710_tm_apply_u8_gain(sc0710_tm_lin_q16_to_srgb((u32)ro), gain);
}

/* Live Reinhard when the user leaves T=0.58 / paper=203 via tm_bgr_* knobs. */
static void sc0710_tm_rgb_pq_pixel_live(u8 *b, u8 *g, u8 *r,
					int target, int paper, int gain)
{
	u32 bn = sc0710_tm_pq_to_nits_q12[*b];
	u32 gn = sc0710_tm_pq_to_nits_q12[*g];
	u32 rn = sc0710_tm_pq_to_nits_q12[*r];
	u64 y_q12 = (689ull * rn + 1778ull * gn + 155ull * bn) >> 12;
	u32 y_nits = (u32)(y_q12 >> 12);
	u32 y_out_q16;
	u32 y_rel_q16;
	u32 scale_q16;
	u64 bo, go, ro;
	u32 b_nits = bn >> 12;
	u32 g_nits = gn >> 12;
	u32 r_nits = rn >> 12;
	u32 b_rel, g_rel, r_rel;
	int den;

	if (y_nits == 0) {
		*b = *g = *r = 0;
		return;
	}

	if (y_nits > 10000)
		y_nits = 10000;

	den = (int)y_nits + paper;
	if (den < 1)
		den = 1;
	y_out_q16 = (u32)(((u64)2 * target * y_nits * 65535ull) /
			  ((u64)100 * den));
	if (y_out_q16 > 65535)
		y_out_q16 = 65535;

	y_rel_q16 = ((u32)y_nits << 16) / (u32)paper;
	if (y_rel_q16 == 0)
		y_rel_q16 = 1;
	scale_q16 = (u32)(((u64)y_out_q16 << 16) / y_rel_q16);
	if (scale_q16 > 0x30000)
		scale_q16 = 0x30000;

	b_rel = (b_nits << 16) / (u32)paper;
	g_rel = (g_nits << 16) / (u32)paper;
	r_rel = (r_nits << 16) / (u32)paper;
	bo = ((u64)b_rel * scale_q16) >> 16;
	go = ((u64)g_rel * scale_q16) >> 16;
	ro = ((u64)r_rel * scale_q16) >> 16;
	if (bo > 65535)
		bo = 65535;
	if (go > 65535)
		go = 65535;
	if (ro > 65535)
		ro = 65535;

	*b = sc0710_tm_apply_u8_gain(sc0710_tm_lin_q16_to_srgb((u32)bo), gain);
	*g = sc0710_tm_apply_u8_gain(sc0710_tm_lin_q16_to_srgb((u32)go), gain);
	*r = sc0710_tm_apply_u8_gain(sc0710_tm_lin_q16_to_srgb((u32)ro), gain);
}

void sc0710_bgr24_sw_tonemap(u8 *bgr, u32 w, u32 h)
{
	size_t i, n;
	int target = sc0710_tm_clamp_int(tm_bgr_target, 1, 100);
	int paper = sc0710_tm_clamp_int(tm_bgr_paper, 1, 10000);
	int gain = sc0710_tm_clamp_int(tm_bgr_gain, 1, 200);
	int chroma = sc0710_tm_clamp_int(tm_bgr_chroma, 0, 300);
	/* Gold defaults from gen-tonemap-luts.py / sc0710-software-hdr. */
	bool use_baked = (target == 58 && paper == SC0710_TM_PAPER_NITS);

	if (!bgr || !w || !h)
		return;

	n = (size_t)w * h * 3;
	for (i = 0; i + 2 < n; i += 3) {
		if (use_baked)
			sc0710_tm_rgb_pq_pixel_baked(&bgr[i], &bgr[i + 1],
						     &bgr[i + 2], gain);
		else
			sc0710_tm_rgb_pq_pixel_live(&bgr[i], &bgr[i + 1],
						    &bgr[i + 2], target, paper,
						    gain);
		sc0710_tm_apply_sat_u8(&bgr[i], &bgr[i + 1], &bgr[i + 2],
				       chroma);
	}
}

/*
 * Map one limited-range PQ Y' sample through Reinhard + chosen OETF.
 * black/white are absolute limited codes (typically 16+lift and white clip).
 */
static u8 sc0710_tm_yuyv_map_y(int y, int target, int paper, int gain,
			       int black, int white, int pq_bias, int oetf)
{
	int pq, nits, out, den;
	u32 l_q16;

	if (y < 16)
		y = 16;
	else if (y > 235)
		y = 235;

	pq = ((y - 16) * 255) / 219 + pq_bias;
	if (pq < 0)
		pq = 0;
	else if (pq > 255)
		pq = 255;

	nits = (int)(sc0710_tm_pq_to_nits_q12[pq] >> 12);
	den = nits + paper;
	if (den < 1)
		den = 1;
	/* L = 2·T·Y/(Y+paper); T = target/100 → Q16 */
	l_q16 = (u32)(((u64)2 * target * nits * 65535ull) /
		      ((u64)100 * den));
	if (l_q16 > 65535)
		l_q16 = 65535;

	if (oetf == 1) {
		/* sRGB full-range code → limited */
		out = sc0710_tm_lin_q10_to_srgb[l_q16 >> 6];
		out = 16 + (out * 219) / 255;
	} else if (oetf == 2) {
		/* linear → limited */
		out = 16 + (int)((l_q16 * 219ull) / 65535ull);
	} else {
		/* Rec.709 limited (default) */
		out = sc0710_tm_lin_q10_to_709lim[l_q16 >> 6];
	}

	out = 16 + (((out - 16) * gain) / 100);
	if (out < black)
		out = black;
	else if (out > white)
		out = white;
	return (u8)out;
}

/*
 * YUYV is limited-range PQ Y'CbCr from the card.
 * Knobs are live via /sys/module/sc0710/parameters/tm_* or sc0710-hdr-config.
 */
void sc0710_yuyv8_sw_tonemap(u8 *yuyv, u32 w, u32 h)
{
	size_t i, n;
	int target = sc0710_tm_clamp_int(tm_yuyv_target, 1, 100);
	int paper = sc0710_tm_clamp_int(tm_paper_nits, 1, 10000);
	int gain = sc0710_tm_clamp_int(tm_yuyv_gain, 1, 200);
	int chroma = sc0710_tm_clamp_int(tm_yuyv_chroma, 0, 300);
	int u_pct = sc0710_tm_clamp_int(tm_yuyv_u, 0, 200);
	int v_pct = sc0710_tm_clamp_int(tm_yuyv_v, 0, 200);
	int pq_bias = sc0710_tm_clamp_int(tm_yuyv_pq_bias, -64, 64);
	int oetf = sc0710_tm_clamp_int(tm_yuyv_oetf, 0, 2);
	int black = 16 + sc0710_tm_clamp_int(tm_yuyv_black, 0, 40);
	int white = sc0710_tm_clamp_int(tm_yuyv_white, 180, 235);
	int uscale = (chroma * u_pct * 256) / 10000;
	int vscale = (chroma * v_pct * 256) / 10000;

	if (!yuyv || w < 2 || !h)
		return;
	if (black > white)
		black = white;

	n = (size_t)w * h * 2;
	for (i = 0; i + 3 < n; i += 4) {
		int u = yuyv[i + 1];
		int v = yuyv[i + 3];
		int uo, vo;

		yuyv[i] = sc0710_tm_yuyv_map_y(yuyv[i], target, paper, gain,
					       black, white, pq_bias, oetf);
		yuyv[i + 2] = sc0710_tm_yuyv_map_y(yuyv[i + 2], target, paper,
						   gain, black, white, pq_bias,
						   oetf);
		uo = 128 + (((u - 128) * uscale) >> 8);
		vo = 128 + (((v - 128) * vscale) >> 8);
		if (uo < 0)
			uo = 0;
		else if (uo > 255)
			uo = 255;
		if (vo < 0)
			vo = 0;
		else if (vo > 255)
			vo = 255;
		yuyv[i + 1] = (u8)uo;
		yuyv[i + 3] = (u8)vo;
	}
}
