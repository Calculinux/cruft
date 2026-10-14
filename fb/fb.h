/* See LICENSE for licence details. */
/*
 * Framebuffer type shapes: struct fb_info_t, struct framebuffer_t,
 * cmap_t and the two small inlines. Contains no non-inline function
 * definitions, so it is includable from multiple TUs: the fb backend
 * (fb/common.h, fb/linux.h — the backend's functions are instantiated
 * in exactly one TU, cruft.c) and consumers that only need the struct
 * layout (status.c).
 *
 * Consumers must include cruft.h first: color2pixel uses bit_mask
 * (cruft.h) and the structs use COLORS/BITS_PER_RGB (color.h) — the
 * same includer-provided-dependency pattern fb/common.h always had.
 */
#ifndef CRUFT_FB_H
#define CRUFT_FB_H

#include <linux/fb.h>

typedef struct fb_cmap cmap_t;

enum fb_type_t {
	YAFT_FB_TYPE_PACKED_PIXELS = 0,
	YAFT_FB_TYPE_PLANES,
	YAFT_FB_TYPE_UNKNOWN,
};

enum fb_visual_t {
	YAFT_FB_VISUAL_TRUECOLOR = 0,
	YAFT_FB_VISUAL_DIRECTCOLOR,
	YAFT_FB_VISUAL_PSEUDOCOLOR,
	YAFT_FB_VISUAL_UNKNOWN,
};

struct fb_info_t {
	struct bitfield_t {
		int length;
		int offset;
	} red, green, blue;
	int width, height;       /* display resolution */
	long screen_size;        /* screen data size (byte) */
	int line_length;         /* line length (byte) */
	int bytes_per_pixel;
	int bits_per_pixel;
	enum fb_type_t type;
	enum fb_visual_t visual;
	int reserved;            /* os specific data */
};

struct framebuffer_t {
	int fd;                        /* file descriptor of framebuffer */
	uint8_t *fp;                   /* pointer of framebuffer */
	uint8_t *buf;                  /* copy of framebuffer (NULL unless wallpaper) */
	uint8_t *wall;                 /* buffer for wallpaper */
	uint32_t real_palette[COLORS]; /* hardware specific color palette */
	struct fb_info_t info;
	cmap_t *cmap, *cmap_orig;
};

static inline uint8_t *fb_draw_dest(struct framebuffer_t *fb)
{
	return fb->buf ? fb->buf : fb->fp;
}

static inline uint32_t color2pixel(struct fb_info_t *info, uint32_t color)
{
	uint32_t r, g, b;

	r = bit_mask[BITS_PER_RGB] & (color >> (BITS_PER_RGB * 2));
	g = bit_mask[BITS_PER_RGB] & (color >>  BITS_PER_RGB);
	b = bit_mask[BITS_PER_RGB] & (color >>  0);

	r = r >> (BITS_PER_RGB - info->red.length);
	g = g >> (BITS_PER_RGB - info->green.length);
	b = b >> (BITS_PER_RGB - info->blue.length);

	return (r << info->red.offset)
			+ (g << info->green.offset)
			+ (b << info->blue.offset);
}

#endif /* CRUFT_FB_H */
