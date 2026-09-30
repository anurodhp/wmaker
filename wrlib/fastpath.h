/* fastpath.h - 32bpp TrueColor / alpha-blend fast paths (DAR-437)
 *
 * Pure C + optional arm_neon.h, no Xlib dependency, so that the pixel-diff
 * test (tools/userland_staging/build_wrlib_fastpath_test.sh) can compile this
 * exact header on the host and compare it against the original loops.
 *
 * WR_NEON is defined only when the target really has NEON (__ARM_NEON,
 * arm64) and -DWR_NO_NEON was not given; the scalar versions are always
 * present and are bit-identical.
 */
#ifndef WR_FASTPATH_H_
#define WR_FASTPATH_H_

#include <string.h>

#if defined(__ARM_NEON) && !defined(WR_NO_NEON)
#include <arm_neon.h>
#define WR_NEON 1
#endif

/*
 * Byte position of each colour channel inside a 32bpp pixel as it lies in
 * memory, or -1 if the layout is not "four distinct byte lanes".
 * libX11 src/ImUtil.c:727-750 (_XPutPixel32) stores pixel value P at
 * data[y*bytes_per_line + 4*x]: LSBFirst => byte i is P>>(8*i); MSBFirst =>
 * byte i is P>>(8*(3-i)).  So a channel shifted by offs (multiple of 8) lives
 * at byte offs/8 (LSBFirst) or 3-offs/8 (MSBFirst).
 */
static inline int wr_lane(int offs, int msb)
{
	if (offs < 0 || offs > 24 || (offs & 7))
		return -1;
	return msb ? 3 - offs / 8 : offs / 8;
}

/* Scalar reference of what convert.c's per-pixel XPutPixel loop does for
 * 0xff masks: pixel = r<<ro | g<<go | b<<bo, stored per _XPutPixel32. */
static inline void
wr_pack32_scalar_row(unsigned char *d, const unsigned char *s, unsigned w, int channels,
		     int ro, int go, int bo, int msb)
{
	unsigned x;

	for (x = 0; x < w; x++, s += channels, d += 4) {
		unsigned long pixel = ((unsigned long)s[0] << ro) | ((unsigned long)s[1] << go)
			| ((unsigned long)s[2] << bo);
		unsigned int p = (unsigned int)pixel;
		if (msb) {
			d[0] = p >> 24; d[1] = p >> 16; d[2] = p >> 8; d[3] = p;
		} else {
			d[3] = p >> 24; d[2] = p >> 16; d[1] = p >> 8; d[0] = p;
		}
	}
}

#ifdef WR_NEON
/* pr/pg/pb are compile-time constants in every caller, so the lane
 * assignment below folds away. The 4th lane is zero, as it is in the
 * scalar pixel value (no bits shifted there). */
static inline void
wr_pack32_neon_row(unsigned char *d, const unsigned char *s, unsigned w, int channels,
		   const int pr, const int pg, const int pb)
{
	unsigned x = 0;
	uint8x16_t zero = vdupq_n_u8(0);

	for (; x + 16 <= w; x += 16) {
		uint8x16_t r, g, b;
		uint8x16x4_t o;
		if (channels == 4) {
			uint8x16x4_t v = vld4q_u8(s);
			r = v.val[0]; g = v.val[1]; b = v.val[2];
			s += 64;
		} else {
			uint8x16x3_t v = vld3q_u8(s);
			r = v.val[0]; g = v.val[1]; b = v.val[2];
			s += 48;
		}
		o.val[0] = o.val[1] = o.val[2] = o.val[3] = zero;
		o.val[pr] = r;
		o.val[pg] = g;
		o.val[pb] = b;
		vst4q_u8(d, o);
		d += 64;
	}
	if (x < w) {
		/* tail: <16 pixels, scalar with the same lanes */
		for (; x < w; x++, s += channels, d += 4) {
			d[0] = d[1] = d[2] = d[3] = 0;
			d[pr] = s[0]; d[pg] = s[1]; d[pb] = s[2];
		}
	}
}
#endif

/*
 * Pack an RImage (3 or 4 bytes/pixel, rows contiguous) into a 32bpp ZPixmap
 * buffer with row stride bpl, for a visual with 8-bit masks at offsets
 * ro/go/bo.  Padding bytes at the end of each row (bpl > 4*w) are untouched.
 */
static inline void
wr_pack32(unsigned char *dst, int bpl, const unsigned char *src, unsigned w, unsigned h,
	  int channels, int ro, int go, int bo, int msb)
{
	unsigned y;
	int pr = wr_lane(ro, msb), pg = wr_lane(go, msb), pb = wr_lane(bo, msb);
#ifdef WR_NEON
	int mode = 0;	/* 0 scalar, 1 BGRx (2,1,0), 2 RGBx (0,1,2), 3 xRGB (1,2,3), 4 xBGR (3,2,1) */

	if (pr >= 0 && pg >= 0 && pb >= 0 && pr != pg && pg != pb && pr != pb) {
		if (pr == 2 && pg == 1 && pb == 0) mode = 1;
		else if (pr == 0 && pg == 1 && pb == 2) mode = 2;
		else if (pr == 1 && pg == 2 && pb == 3) mode = 3;
		else if (pr == 3 && pg == 2 && pb == 1) mode = 4;
	}
#endif
	for (y = 0; y < h; y++, src += (size_t)w * channels, dst += bpl) {
#ifdef WR_NEON
		switch (mode) {
		case 1: wr_pack32_neon_row(dst, src, w, channels, 2, 1, 0); continue;
		case 2: wr_pack32_neon_row(dst, src, w, channels, 0, 1, 2); continue;
		case 3: wr_pack32_neon_row(dst, src, w, channels, 1, 2, 3); continue;
		case 4: wr_pack32_neon_row(dst, src, w, channels, 3, 2, 1); continue;
		}
#endif
		wr_pack32_scalar_row(dst, src, w, channels, ro, go, bo, msb);
	}
}

/*
 * Alpha blend used by raster.c (RCombineArea, RCombineImages*, RCombineAlpha
 * callers): d = (d*(255-a) + s*a) / 256, all in int, per channel.  The
 * numerator is <= 255*255 = 65025, so it fits u16 and /256 is a >>8:
 * bit-exact to the C expression.
 */

/* dst RGB (3 bytes/px), src RGBA (4 bytes/px), n pixels */
static inline void wr_blend_rgb_rgba(unsigned char *d, const unsigned char *s, unsigned n)
{
	unsigned i = 0;
#ifdef WR_NEON
	for (; i + 16 <= n; i += 16, d += 48, s += 64) {
		uint8x16x3_t dv = vld3q_u8(d);
		uint8x16x4_t sv = vld4q_u8(s);
		uint8x8_t al = vget_low_u8(sv.val[3]), ah = vget_high_u8(sv.val[3]);
		uint8x8_t cl = vmvn_u8(al), ch = vmvn_u8(ah);	/* 255 - a */
		int c;
		for (c = 0; c < 3; c++) {
			uint16x8_t lo = vmlal_u8(vmull_u8(vget_low_u8(dv.val[c]), cl), vget_low_u8(sv.val[c]), al);
			uint16x8_t hi = vmlal_u8(vmull_u8(vget_high_u8(dv.val[c]), ch), vget_high_u8(sv.val[c]), ah);
			dv.val[c] = vcombine_u8(vshrn_n_u16(lo, 8), vshrn_n_u16(hi, 8));
		}
		vst3q_u8(d, dv);
	}
#endif
	for (; i < n; i++, d += 3, s += 4) {
		int alpha = s[3], calpha = 255 - alpha;
		d[0] = (((int)d[0] * calpha) + ((int)s[0] * alpha)) / 256;
		d[1] = (((int)d[1] * calpha) + ((int)s[1] * alpha)) / 256;
		d[2] = (((int)d[2] * calpha) + ((int)s[2] * alpha)) / 256;
	}
}

/* dst and src both RGB: d = (d*(255-op) + s*op)/256 over n bytes (n = 3*pixels) */
static inline void wr_blend_bytes_op(unsigned char *d, const unsigned char *s, unsigned n, int op)
{
	unsigned i = 0;
	int cop = 255 - op;
#ifdef WR_NEON
	if (op < 0 || op > 255) {
		for (; i < n; i++)
			d[i] = (((int)d[i] * cop) + ((int)s[i] * op)) / 256;
		return;
	}
	{
	uint8x8_t vop = vdup_n_u8((unsigned char)op), vcop = vdup_n_u8((unsigned char)cop);
	for (; i + 16 <= n; i += 16) {
		uint8x16_t dv = vld1q_u8(d + i), sv = vld1q_u8(s + i);
		uint16x8_t lo = vmlal_u8(vmull_u8(vget_low_u8(dv), vcop), vget_low_u8(sv), vop);
		uint16x8_t hi = vmlal_u8(vmull_u8(vget_high_u8(dv), vcop), vget_high_u8(sv), vop);
		vst1q_u8(d + i, vcombine_u8(vshrn_n_u16(lo, 8), vshrn_n_u16(hi, 8)));
	}
	}
#endif
	for (; i < n; i++)
		d[i] = (((int)d[i] * cop) + ((int)s[i] * op)) / 256;
}

/* Fill n RGB pixels with a constant colour (gradient.c renderGradientWidth) */
static inline unsigned char *wr_fill_rgb(unsigned char *p, unsigned n, unsigned char r, unsigned char g, unsigned char b)
{
	unsigned i = 0;
#ifdef WR_NEON
	uint8x16x3_t v;
	v.val[0] = vdupq_n_u8(r); v.val[1] = vdupq_n_u8(g); v.val[2] = vdupq_n_u8(b);
	for (; i + 16 <= n; i += 16, p += 48)
		vst3q_u8(p, v);
#endif
	for (; i < n; i++) {
		*p++ = r; *p++ = g; *p++ = b;
	}
	return p;
}

#endif /* WR_FASTPATH_H_ */
