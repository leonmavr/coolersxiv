/* Copyright 2011-2013 Bert Muennich
 *
 * This file is part of sxiv.
 *
 * sxiv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * sxiv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with sxiv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "sxiv.h"
#define _WINDOW_CONFIG
#include "config.h"
#include "icon/data.h"
#include "utf8.h"

#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <X11/cursorfont.h>
#include <X11/Xatom.h>
#include <X11/Xresource.h>

#define RES_CLASS "Sxiv"

enum {
	H_TEXT_PAD = 5,
	V_TEXT_PAD = 3
};

static struct {
	int name;
	Cursor icon;
} cursors[CURSOR_COUNT] = {
	{ XC_left_ptr }, { XC_dotbox }, { XC_watch },
	{ XC_sb_left_arrow }, { XC_sb_right_arrow }
};

static GC gc;

static XftFont *font;
static int fontheight;
static double fontsize;
static int barheight;

/* The configured font is often much larger than a full key map needs, so the
 * overlay uses its own font, shrunk until the whole map fits on screen. */
static const FcChar8 *fontfamily;
static XftFont *keysfont;
static int keysfontheight;
static double keysfontsize;
static double keyssize;          /* font size currently chosen for the overlay */
static double keyssize_base;     /* configured font size it was chosen for */
static int keyssize_w, keyssize_h; /* geometry it was chosen for */

Atom atoms[ATOM_COUNT];

static unsigned int bar_total(win_t *win)
{
	return win->bar.h + win->bar.h2;
}

void win_init_font(const win_env_t *e, const char *fontstr)
{
	FcChar8 *fam = NULL;

	if ((font = XftFontOpenName(e->dpy, e->scr, fontstr)) == NULL)
		error(EXIT_FAILURE, 0, "Error loading font '%s'", fontstr);
	fontheight = font->ascent + font->descent;
	FcPatternGetDouble(font->pattern, FC_SIZE, 0, &fontsize);
	if (FcPatternGetString(font->pattern, FC_FAMILY, 0, &fam) != FcResultMatch)
		fam = NULL;
	fontfamily = fam;
	barheight = fontheight + 2 * V_TEXT_PAD;
}

/* (Re)open the overlay font, unless it is already at the requested size. */
static void keys_init_font(win_env_t *e, double size)
{
	XftFont *f;

	if (keysfont != NULL && keysfontsize == size)
		return;
	f = XftFontOpen(e->dpy, e->scr, FC_FAMILY, FcTypeString,
	                fontfamily != NULL ? (const char*) fontfamily : "monospace",
	                FC_SIZE, FcTypeDouble, size, NULL);
	if (f == NULL)
		return;
	if (keysfont != NULL)
		XftFontClose(e->dpy, keysfont);
	keysfont = f;
	keysfontheight = f->ascent + f->descent;
	keysfontsize = size;
}

/* Width of a string in the overlay font. */
#define TEXTWIDTH(win, text, len) \
	win_draw_text(win, NULL, NULL, 0, 0, text, len, 0)

int win_draw_text(win_t *win, XftDraw *d, const XftColor *color, int x, int y,
                  char *text, int len, int w);

static int keys_textw(win_t *win, const char *s)
{
	XftFont *of = font;
	int ofh = fontheight, w;

	font = keysfont;
	fontheight = keysfontheight;
	w = TEXTWIDTH(win, (char*) s, strlen(s));
	font = of;
	fontheight = ofh;
	return w;
}

/* Largest overlay font size at which the key map still fits on screen. */
static double keys_fit(win_t *win, int totalh)
{
	int i, half, avail, wmax;
	double size;

	half = (win->keys_cnt + 1) / 2;
	avail = totalh - 4 * V_TEXT_PAD;
	wmax = (int) win->w_image / 2 - 3 * H_TEXT_PAD;

	for (size = fontsize; size > 5.0; size *= 0.85) {
		int wl = 0, wr = 0, rows;

		keys_init_font(&win->env, size);
		if (keysfont == NULL)
			return 0.0;
		rows = half * (keysfontheight + 2);
		for (i = 0; i < half; i++)
			wl = MAX(wl, keys_textw(win, win->keys_lines[i]));
		for (i = half; i < win->keys_cnt; i++)
			wr = MAX(wr, keys_textw(win, win->keys_lines[i]));
		if (rows <= avail && MAX(wl, wr) <= wmax)
			return size;
	}
	keys_init_font(&win->env, 5.0);
	return keysfont != NULL ? 5.0 : fontsize;
}

void win_alloc_color(const win_env_t *e, const char *name, XftColor *col)
{
	if (!XftColorAllocName(e->dpy, DefaultVisual(e->dpy, e->scr),
	                       DefaultColormap(e->dpy, e->scr), name, col))
	{
		error(EXIT_FAILURE, 0, "Error allocating color '%s'", name);
	}
}

const char* win_res(XrmDatabase db, const char *name, const char *def)
{
	char *type;
	XrmValue ret;

	if (db != None &&
	    XrmGetResource(db, name, name, &type, &ret) &&
	    STREQ(type, "String"))
	{
		return ret.addr;
	} else {
		return def;
	}
}

#define INIT_ATOM_(atom) \
	atoms[ATOM_##atom] = XInternAtom(e->dpy, #atom, False);

void win_init(win_t *win)
{
	win_env_t *e;
	const char *bg, *fg, *f, *bar_height_str;
	/* background checkerboard colors */
	const char *cb1, *cb2;
	char *res_man, *end;
	XrmDatabase db;
	long bar_height;

	memset(win, 0, sizeof(win_t));

	e = &win->env;
	if ((e->dpy = XOpenDisplay(NULL)) == NULL)
		error(EXIT_FAILURE, 0, "Error opening X display");

	e->scr = DefaultScreen(e->dpy);
	e->scrw = DisplayWidth(e->dpy, e->scr);
	e->scrh = DisplayHeight(e->dpy, e->scr);
	e->vis = DefaultVisual(e->dpy, e->scr);
	e->cmap = DefaultColormap(e->dpy, e->scr);
	e->depth = DefaultDepth(e->dpy, e->scr);

	if (setlocale(LC_CTYPE, "") == NULL || XSupportsLocale() == 0)
		error(0, 0, "No locale support");

	XrmInitialize();
	res_man = XResourceManagerString(e->dpy);
	db = res_man != NULL ? XrmGetStringDatabase(res_man) : None;

	f = win_res(db, RES_CLASS ".font", "monospace-12");
	win_init_font(e, f);

	bar_height_str = win_res(db, RES_CLASS ".barHeight", "");
	bar_height = strtol(bar_height_str, &end, 10);
	if (bar_height_str[0] != '\0' && *end == '\0' && bar_height > 0)
		barheight = (int)bar_height;

	bg = win_res(db, RES_CLASS ".background", "white");
	fg = win_res(db, RES_CLASS ".foreground", "black");
	/* Checker colors: default to background and white */
	cb1 = win_res(db, RES_CLASS ".checkerPrimary", bg);
	cb2 = win_res(db, RES_CLASS ".checkerSecondary", "white");
	win_alloc_color(e, bg, &win->bg);
	win_alloc_color(e, fg, &win->fg);
	win_alloc_color(e, cb1, &win->cb1);
	win_alloc_color(e, cb2, &win->cb2);

	win->bar.l.size = BAR_L_LEN;
	win->bar.r.size = BAR_R_LEN;
	win->bar.l2.size = BAR_L_LEN;
	/* 3 padding bytes needed by utf8_decode */
	win->bar.l.buf = emalloc(win->bar.l.size + 3);
	win->bar.l.buf[0] = '\0';
	win->bar.r.buf = emalloc(win->bar.r.size + 3);
	win->bar.r.buf[0] = '\0';
	win->bar.l2.buf = emalloc(win->bar.l2.size + 3);
	win->bar.l2.buf[0] = '\0';
	win->bar.h = options->hide_bar ? 0 : barheight;
	win->bar.h2 = 0;

	INIT_ATOM_(WM_DELETE_WINDOW);
	INIT_ATOM_(_NET_WM_NAME);
	INIT_ATOM_(_NET_WM_ICON_NAME);
	INIT_ATOM_(_NET_WM_ICON);
	INIT_ATOM_(_NET_WM_STATE);
	INIT_ATOM_(_NET_WM_STATE_FULLSCREEN);

	if (db != None)
		XrmDestroyDatabase(db);
}

void win_open(win_t *win)
{
	int c, i, j, n;
	long parent;
	win_env_t *e;
	XClassHint classhint;
	unsigned long *icon_data;
	XColor col;
	Cursor *cnone = &cursors[CURSOR_NONE].icon;
	char none_data[] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	Pixmap none;
	int gmask;
	XSizeHints sizehints;

	e = &win->env;
	parent = options->embed != 0 ? options->embed : RootWindow(e->dpy, e->scr);

	sizehints.flags = PWinGravity;
	sizehints.win_gravity = NorthWestGravity;

	/* determine window offsets, width & height */
	if (options->geometry == NULL)
		gmask = 0;
	else
		gmask = XParseGeometry(options->geometry, &win->x, &win->y,
		                       &win->w_image, &win->h_image);
	if ((gmask & WidthValue) != 0)
		sizehints.flags |= USSize;
	else
		win->w_image = WIN_WIDTH;
	if ((gmask & HeightValue) != 0)
		sizehints.flags |= USSize;
	else
		win->h_image = WIN_HEIGHT;
	if ((gmask & XValue) != 0) {
		if ((gmask & XNegative) != 0) {
			win->x += e->scrw - win->w_image;
			sizehints.win_gravity = NorthEastGravity;
		}
		sizehints.flags |= USPosition;
	} else {
		win->x = 0;
	}
	if ((gmask & YValue) != 0) {
		if ((gmask & YNegative) != 0) {
			win->y += e->scrh - win->h_image;
			sizehints.win_gravity = sizehints.win_gravity == NorthEastGravity
			                      ? SouthEastGravity : SouthWestGravity;
		}
		sizehints.flags |= USPosition;
	} else {
		win->y = 0;
	}

	win->xwin = XCreateWindow(e->dpy, parent,
	                          win->x, win->y, win->w_image, win->h_image, 0,
	                          e->depth, InputOutput, e->vis, 0, NULL);
	if (win->xwin == None)
		error(EXIT_FAILURE, 0, "Error creating X window");

	XSelectInput(e->dpy, win->xwin,
	             ButtonReleaseMask | ButtonPressMask | KeyPressMask |
	             PointerMotionMask | StructureNotifyMask);

	for (i = 0; i < ARRLEN(cursors); i++) {
		if (i != CURSOR_NONE)
			cursors[i].icon = XCreateFontCursor(e->dpy, cursors[i].name);
	}
	if (XAllocNamedColor(e->dpy, DefaultColormap(e->dpy, e->scr), "black",
	                     &col, &col) == 0)
	{
		error(EXIT_FAILURE, 0, "Error allocating color 'black'");
	}
	none = XCreateBitmapFromData(e->dpy, win->xwin, none_data, 8, 8);
	*cnone = XCreatePixmapCursor(e->dpy, none, none, &col, &col, 0, 0);

	gc = XCreateGC(e->dpy, win->xwin, 0, None);

	n = icons[ARRLEN(icons)-1].size;
	icon_data = emalloc((n * n + 2) * sizeof(*icon_data));

	for (i = 0; i < ARRLEN(icons); i++) {
		n = 0;
		icon_data[n++] = icons[i].size;
		icon_data[n++] = icons[i].size;

		for (j = 0; j < icons[i].cnt; j++) {
			for (c = icons[i].data[j] >> 4; c >= 0; c--)
				icon_data[n++] = icon_colors[icons[i].data[j] & 0x0F];
		}
		XChangeProperty(e->dpy, win->xwin,
		                atoms[ATOM__NET_WM_ICON], XA_CARDINAL, 32,
		                i == 0 ? PropModeReplace : PropModeAppend,
		                (unsigned char *) icon_data, n);
	}
	free(icon_data);

	win_set_title(win, "sxiv");

	classhint.res_class = RES_CLASS;
	classhint.res_name = options->res_name != NULL ? options->res_name : "sxiv";
	XSetClassHint(e->dpy, win->xwin, &classhint);

	XSetWMProtocols(e->dpy, win->xwin, &atoms[ATOM_WM_DELETE_WINDOW], 1);

	sizehints.width = win->w_image;
	sizehints.height = win->h_image;
	sizehints.x = win->x;
	sizehints.y = win->y;
	XSetWMNormalHints(win->env.dpy, win->xwin, &sizehints);

	win->h_image -= bar_total(win);

	win->buf.w = e->scrw;
	win->buf.h = e->scrh;
	win->buf.pm = XCreatePixmap(e->dpy, win->xwin,
	                            win->buf.w, win->buf.h, e->depth);
	XSetForeground(e->dpy, gc, win->bg.pixel);
	XFillRectangle(e->dpy, win->buf.pm, gc, 0, 0, win->buf.w, win->buf.h);
	XSetWindowBackgroundPixmap(e->dpy, win->xwin, win->buf.pm);

	XMapWindow(e->dpy, win->xwin);
	XFlush(e->dpy);

	if (options->fullscreen)
		win_toggle_fullscreen(win);
}

CLEANUP void win_close(win_t *win)
{
	int i;

	for (i = 0; i < ARRLEN(cursors); i++)
		XFreeCursor(win->env.dpy, cursors[i].icon);

	XFreeGC(win->env.dpy, gc);

	if (font != NULL)
		XftFontClose(win->env.dpy, font);
	if (keysfont != NULL)
		XftFontClose(win->env.dpy, keysfont);

	XDestroyWindow(win->env.dpy, win->xwin);
	XCloseDisplay(win->env.dpy);
}

bool win_configure(win_t *win, XConfigureEvent *c)
{
	bool changed;

	changed = win->w_image != c->width || win->h_image + bar_total(win) != c->height;

	win->x = c->x;
	win->y = c->y;
	win->w_image = c->width;
	win->h_image = c->height - bar_total(win);
	win->bw = c->border_width;

	return changed;
}

void win_toggle_fullscreen(win_t *win)
{
	XEvent ev;
	XClientMessageEvent *cm;

	memset(&ev, 0, sizeof(ev));
	ev.type = ClientMessage;

	cm = &ev.xclient;
	cm->window = win->xwin;
	cm->message_type = atoms[ATOM__NET_WM_STATE];
	cm->format = 32;
	cm->data.l[0] = 2; // toggle
	cm->data.l[1] = atoms[ATOM__NET_WM_STATE_FULLSCREEN];

	XSendEvent(win->env.dpy, DefaultRootWindow(win->env.dpy), False,
	           SubstructureNotifyMask | SubstructureRedirectMask, &ev);
}

void win_toggle_bar(win_t *win)
{
	if (win->bar.h != 0) {
		win->h_image += win->bar.h;
		win->bar.h = 0;
	} else {
		win->bar.h = barheight;
		win->h_image -= win->bar.h;
	}
}

void win_toggle_bar2(win_t *win)
{
	if (win->bar.h2 != 0) {
		win->h_image += win->bar.h2;
		win->bar.h2 = 0;
	} else {
		win->bar.h2 = barheight;
		win->h_image -= win->bar.h2;
	}
}

void win_clear(win_t *win)
{
	win_env_t *e;

	e = &win->env;

	if (win->w_image > win->buf.w || win->h_image + bar_total(win) > win->buf.h) {
		XFreePixmap(e->dpy, win->buf.pm);
		win->buf.w = MAX(win->buf.w, win->w_image);
		win->buf.h = MAX(win->buf.h, win->h_image + bar_total(win));
		win->buf.pm = XCreatePixmap(e->dpy, win->xwin,
		                            win->buf.w, win->buf.h, e->depth);
	}
	/* Draw checkerboard background instead of solid fill */
	{
		const int tile = 20; /* 20px squares */
		unsigned long c0 = win->cb1.pixel; /* primary checker color */
		unsigned long c1 = win->cb2.pixel; /* secondary checker color */
		/* Ensure contrast if both colors end up identical */
		if (c0 == c1) {
			XColor exact, screen;
			if (XAllocNamedColor(e->dpy, DefaultColormap(e->dpy, e->scr),
								  "gray70", &screen, &exact))
			{
				c0 = screen.pixel;
			}
		}
		int x, y, wrem, hrem;
		for (y = 0; y < (int)win->buf.h; y += tile) {
			hrem = ((y + tile) <= (int)win->buf.h) ? tile : (int)win->buf.h - y;
			for (x = 0; x < (int)win->buf.w; x += tile) {
				wrem = ((x + tile) <= (int)win->buf.w) ? tile : (int)win->buf.w - x;
				XSetForeground(e->dpy, gc, (((x / tile) + (y / tile)) & 1) ? c1 : c0);
				XFillRectangle(e->dpy, win->buf.pm, gc, x, y, wrem, hrem);
			}
		}
	}
}

int win_draw_text(win_t *win, XftDraw *d, const XftColor *color, int x, int y,
                  char *text, int len, int w)
{
	int err, tw = 0;
	char *t, *next;
	uint32_t rune;
	XftFont *f;
	FcCharSet *fccharset;
	XGlyphInfo ext;

	for (t = text; t - text < len; t = next) {
		next = utf8_decode(t, &rune, &err);
		if (XftCharExists(win->env.dpy, font, rune)) {
			f = font;
		} else { /* fallback font */
			fccharset = FcCharSetCreate();
			FcCharSetAddChar(fccharset, rune);
			f = XftFontOpen(win->env.dpy, win->env.scr, FC_CHARSET, FcTypeCharSet,
			                fccharset, FC_SCALABLE, FcTypeBool, FcTrue,
			                FC_SIZE, FcTypeDouble, fontsize, NULL);
			FcCharSetDestroy(fccharset);
		}
		XftTextExtentsUtf8(win->env.dpy, f, (XftChar8*)t, next - t, &ext);
		tw += ext.xOff;
		if (tw <= w) {
			XftDrawStringUtf8(d, color, f, x, y, (XftChar8*)t, next - t);
			x += ext.xOff;
		}
		if (f != font)
			XftFontClose(win->env.dpy, f);
	}
	return tw;
}

static void win_draw_bar_content(win_t *win, XftDraw *d, win_bar_t *l,
                                 win_bar_t *r, int y)
{
	int len, x, w, tw;

	w = win->w_image - 2*H_TEXT_PAD;

	if (r != NULL && (len = strlen(r->buf)) > 0) {
		if ((tw = TEXTWIDTH(win, r->buf, len)) > w)
			return; /* right-hand part too wide, skip this row */
		x = win->w_image - tw - H_TEXT_PAD;
		w -= tw;
		win_draw_text(win, d, &win->bg, x, y, r->buf, len, tw);
	}
	if (l != NULL && (len = strlen(l->buf)) > 0) {
		x = H_TEXT_PAD;
		w -= 2 * H_TEXT_PAD; /* gap between left and right parts */
		win_draw_text(win, d, &win->bg, x, y, l->buf, len, w);
	}
}

void win_draw_bar(win_t *win)
{
	int y;
	win_env_t *e;
	XftDraw *d;

	if (win->bar.l.buf == NULL || win->bar.r.buf == NULL || bar_total(win) == 0)
		return;

	e = &win->env;
	d = XftDrawCreate(e->dpy, win->buf.pm, DefaultVisual(e->dpy, e->scr),
	                  DefaultColormap(e->dpy, e->scr));

	/* Draw an opaque bar background over all bar rows. */
	XSetForeground(e->dpy, gc, win->fg.pixel);
	XFillRectangle(e->dpy, win->buf.pm, gc, 0, win->h_image, win->w_image,
	               bar_total(win));

	XSetForeground(e->dpy, gc, win->bg.pixel);
	XSetBackground(e->dpy, gc, win->fg.pixel);

	y = win->h_image + font->ascent + V_TEXT_PAD;
	win_draw_bar_content(win, d, &win->bar.l, &win->bar.r, y);

	if (win->bar.h2 > 0) {
		y += win->bar.h; /* second bar sits right below the first */
		win_draw_bar_content(win, d, &win->bar.l2, NULL, y);
	}

	XftDrawDestroy(d);
}

static void win_draw_keys(win_t *win)
{
	win_env_t *e;
	XftDraw *d;
	XftFont *of;
	int ofh, i, half, lineh, x1, x2, y, len, wmax;
	unsigned int totalh;

	if (!win->keys_on || win->keys_cnt <= 0 || win->keys_lines == NULL)
		return;

	e = &win->env;
	totalh = win->h_image + bar_total(win);

	/* Pick the largest font size at which the whole map fits on screen.
	 * The result only depends on the configured font and the window size,
	 * so it is cached across redraws. */
	if (keyssize <= 0 || keyssize_base != fontsize ||
	    keyssize_w != (int) win->w_image || keyssize_h != (int) totalh)
	{
		keyssize = keys_fit(win, totalh);
		keyssize_base = fontsize;
		keyssize_w = win->w_image;
		keyssize_h = totalh;
	}
	keys_init_font(e, keyssize);
	if (keysfont == NULL)
		return;

	of = font;
	ofh = fontheight;
	font = keysfont;
	fontheight = keysfontheight;

	d = XftDrawCreate(e->dpy, win->buf.pm, DefaultVisual(e->dpy, e->scr),
	                  DefaultColormap(e->dpy, e->scr));

	/* Dim the whole window; text is drawn on top of this. */
	XSetForeground(e->dpy, gc, win->fg.pixel);
	XFillRectangle(e->dpy, win->buf.pm, gc, 0, 0, win->w_image, totalh);
	XSetForeground(e->dpy, gc, win->bg.pixel);
	XSetBackground(e->dpy, gc, win->fg.pixel);

	/* First half in the left column, second half in the right one. */
	half = (win->keys_cnt + 1) / 2;
	lineh = fontheight + 2;
	x1 = 2 * H_TEXT_PAD;
	x2 = (int) win->w_image / 2;

	for (i = 0; i < win->keys_cnt; i++) {
		int x = i < half ? x1 : x2;

		y = font->ascent + 2 * V_TEXT_PAD + (i < half ? i : i - half) * lineh;
		if (y > (int) totalh)
			break;
		len = strlen(win->keys_lines[i]);
		wmax = win->w_image - x - 2 * H_TEXT_PAD;
		win_draw_text(win, d, &win->bg, x, y, (char*) win->keys_lines[i],
		              len, wmax);
	}

	XftDrawDestroy(d);

	font = of;
	fontheight = ofh;
}

void win_draw(win_t *win)
{
	if (bar_total(win) > 0)
		win_draw_bar(win);

	if (win->keys_on)
		win_draw_keys(win);

	XSetWindowBackgroundPixmap(win->env.dpy, win->xwin, win->buf.pm);
	XClearWindow(win->env.dpy, win->xwin);
	XFlush(win->env.dpy);
}

void win_draw_rect(win_t *win, int x, int y, int w, int h, bool fill, int lw,
                   unsigned long col)
{
	XGCValues gcval;

	gcval.line_width = lw;
	gcval.foreground = col;
	XChangeGC(win->env.dpy, gc, GCForeground | GCLineWidth, &gcval);

	if (fill)
		XFillRectangle(win->env.dpy, win->buf.pm, gc, x, y, w, h);
	else
		XDrawRectangle(win->env.dpy, win->buf.pm, gc, x, y, w, h);
}

void win_set_title(win_t *win, const char *title)
{
	XStoreName(win->env.dpy, win->xwin, title);
	XSetIconName(win->env.dpy, win->xwin, title);

	XChangeProperty(win->env.dpy, win->xwin, atoms[ATOM__NET_WM_NAME],
	                XInternAtom(win->env.dpy, "UTF8_STRING", False), 8,
	                PropModeReplace, (unsigned char *) title, strlen(title));
	XChangeProperty(win->env.dpy, win->xwin, atoms[ATOM__NET_WM_ICON_NAME],
	                XInternAtom(win->env.dpy, "UTF8_STRING", False), 8,
	                PropModeReplace, (unsigned char *) title, strlen(title));
}

void win_set_cursor(win_t *win, cursor_t cursor)
{
	if (cursor >= 0 && cursor < ARRLEN(cursors)) {
		XDefineCursor(win->env.dpy, win->xwin, cursors[cursor].icon);
		XFlush(win->env.dpy);
	}
}

void win_cursor_pos(win_t *win, int *x, int *y)
{
	int i;
	unsigned int ui;
	Window w;

	if (!XQueryPointer(win->env.dpy, win->xwin, &w, &w, &i, &i, x, y, &ui))
		*x = *y = 0;
}

