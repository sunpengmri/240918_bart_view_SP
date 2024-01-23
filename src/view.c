/* Copyright 2015-2016. Martin Uecker.
 * All rights reserved. Use of this source code is governed by
 * a BSD-style license which can be found in the LICENSE file.
 */

#define _GNU_SOURCE
#include <unistd.h>
#include <complex.h>
#include <stdbool.h>
#include <math.h>

#include <libgen.h>

#include "gtk_ui.h"

#include "num/multind.h"

#include "misc/misc.h"
#include "misc/png.h"
#include "misc/debug.h"

#include "draw.h"

#include "view.h"

#ifndef DIMS
#define DIMS 16
#endif

struct view_control_s {

	//data
	long dims[DIMS];
	long strs[DIMS];
	const complex float* data;

	// interpolation buffer
	complex float* buf;

	// rgb buffer
	int rgbh;
	int rgbw;
	int rgbstr;
	unsigned char* rgb;

	// geometry
	unsigned long geom_flags;
	const float (*geom)[3][3];
	const float (*geom_current)[3][3];

	// windowing
	int lastx;
	int lasty;

	//misc
	bool status_bar;
	bool transpose;
	double max;
};

#if 0
static void add_text(cairo_surface_t* surface, int x, int y, int size, const char* text)
{
	cairo_t* cr = cairo_create(surface);
	cairo_set_source_rgb(cr, 1., 1., 1.);

	PangoLayout* layout = pango_cairo_create_layout(cr);
	pango_layout_set_text(layout, text, -1);
	PangoFontDescription* desc = pango_font_description_new();
	pango_font_description_set_family(desc, "sans");
	pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
	pango_font_description_set_absolute_size(desc, size * PANGO_SCALE);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);

	int w = 0;
	int h = 0;
	pango_layout_get_pixel_size(layout, &w, &h);

	cairo_move_to(cr, (x >= 0) ? x : -(x + (double)w),
			  (y >= 0) ? y : -(y + (double)h));

	pango_cairo_show_layout(cr, layout);

	g_object_unref(layout);
	cairo_destroy(cr);
}
#endif

extern void update_geom(struct view_s* v)
{
	if (NULL == v->control->geom)
		return;

	long dims[DIMS];
	md_select_dims(DIMS, v->control->geom_flags, dims, v->control->dims);

	long strs[DIMS];
	md_calc_strides(DIMS, strs, dims, 1);

	v->control->geom_current = &v->control->geom[md_calc_offset(DIMS, strs, v->settings.pos)];

	printf("%f %f %f %f %f %f %f %f %f\n",
		(*v->control->geom_current)[0][0], (*v->control->geom_current)[0][1], (*v->control->geom_current)[0][2],
		(*v->control->geom_current)[1][0], (*v->control->geom_current)[1][1], (*v->control->geom_current)[1][2],
		(*v->control->geom_current)[2][0], (*v->control->geom_current)[2][1], (*v->control->geom_current)[2][2]);
}

void view_sync_windowing(struct view_s* v)
{

	for (struct view_s* v2 = v->next; v2 != v; v2 = v2->next) {

		if (v->sync && v2->sync) {

			v2->settings.mode = v->settings.mode;
			v2->settings.winlow = v->settings.winlow;
			v2->settings.winhigh= v->settings.winhigh;
			ui_set_values(v2);
			ui_set_mode(v2);
		}
	}
}

extern void view_refresh(struct view_s* v)
{
	if (v->settings.absolute_windowing) {

		long idims[DIMS];
		md_select_dims(DIMS, MD_BIT(v->settings.xdim) | MD_BIT(v->settings.ydim), idims, v->control->dims);
	
		complex float* tmp = md_alloc(DIMS, idims, sizeof(complex float));
		
		long pos[DIMS];
		md_copy_dims(DIMS, pos, v->settings.pos);
		pos[v->settings.xdim] = 0;
		pos[v->settings.ydim] = 0;

		md_slice(DIMS, ~(MD_BIT(v->settings.xdim) | MD_BIT(v->settings.ydim)), pos, v->control->dims, tmp, v->control->data, sizeof(complex float));

		long size = md_calc_size(DIMS, idims);

		double max = 0.;
		for (long j = 0; j < size; j++)
			if (max < cabsf(tmp[j]))
				max = cabsf(tmp[j]);

		max = MIN(1.e10, max);

		md_free(tmp);

		if (0 == v->control->max) {

			v->win_high_max = max;
			v->win_low_max = max;
			v->settings.winhigh = max;
			v->control->max = max;

			ui_set_limits(v);
			ui_set_values(v);
		}

		if (v->control->max < max) {

			v->win_high_max = max;
			v->win_low_max = max;
			v->control->max = max;

			ui_set_limits(v);
		}
	} else {

		long size = md_calc_size(DIMS, v->control->dims);
		double max = 0.;

		for (long j = 0; j < size; j++)
			if (max < cabsf(v->control->data[j]))
				max = cabsf(v->control->data[j]);
		
		max = MIN(1.e10, max);

		if (0. == max)
			max = 1.;

		v->control->max = max;
	}

	ui_trigger_redraw(v);
}


extern void view_add_geometry(struct view_s* v, unsigned long flags, const float (*geom)[3][3])
{
	v->control->geom_flags = flags;
	v->control->geom = geom;
	v->control->geom_current = NULL;

	update_geom(v);
}


void view_set_geom(struct view_s* v, struct view_ui_geom_params_s gp)
{
	for (int j = 0; j < DIMS; j++) {
		if (!gp.selected[j])
			continue;

		if (1 == v->control->dims[j]) {

			gp.selected[j] = false;
		} else if ((j != v->settings.xdim) && (j != v->settings.ydim)) {

			for (int i = 0; i < DIMS; i++) {

				if (v->settings.xdim == (DIMS + j - i) % DIMS) {

					gp.selected[v->settings.xdim] = false;
					v->settings.xdim = j;
					break;
				}

				if (v->settings.ydim == (DIMS + j - i) % DIMS) {

					gp.selected[v->settings.ydim] = false;
					v->settings.ydim = j;
					break;
				}

			}
		}
	}

	gp.selected[v->settings.xdim] = true;
	gp.selected[v->settings.ydim] = true;

	ui_set_selected_dims(v, gp.selected);

	v->settings.xzoom = gp.zoom * gp.aniso;
	v->settings.yzoom = gp.zoom;

	if (gp.transpose) {

		if (v->settings.xdim < v->settings.ydim) {

			int swp = v->settings.xdim;
			v->settings.xdim = v->settings.ydim;
			v->settings.ydim = swp;
		}
	} else {

		if (v->settings.xdim > v->settings.ydim) {

			int swp = v->settings.xdim;
			v->settings.xdim = v->settings.ydim;
			v->settings.ydim = swp;
		}
	}

	v->control->lastx = -1;
	v->control->lasty = -1;

	v->invalid = true;

	update_geom(v);
	ui_trigger_redraw(v);
}

void view_touch_rgb_settings(struct view_s* v)
{
	view_sync_windowing(v);
	v->rgb_invalid = true;

	ui_trigger_redraw(v);
}

static void update_buf_view(struct view_s* v)
{
	update_buf(v->settings.xdim, v->settings.ydim, DIMS, v->control->dims, v->control->strs, v->settings.pos,
		v->settings.flip, v->settings.interpolation, v->settings.xzoom, v->settings.yzoom, v->settings.plot,
		v->control->rgbw, v->control->rgbh, v->control->data, v->control->buf);
}


char *construct_filename_view2(struct view_s* v)
{
	long loopdims[DIMS];
	md_select_dims(DIMS, ~(MD_BIT(v->settings.xdim) | MD_BIT(v->settings.ydim)), loopdims, v->control->dims);

	return construct_filename_view(DIMS, loopdims, v->settings.pos, v->name, "png");
}

bool view_save_png(struct view_s* v, const char *filename)
{
	return gtk_ui_save_png(v, filename);
}


extern bool view_save_pngmovie(struct view_s* v, const char *folder)
{
	int frame_dim = 10;

	for (unsigned int f = 0; f < v->control->dims[frame_dim]; f++) {

		v->settings.pos[frame_dim] = f;
		update_buf_view(v);

		draw(v->control->rgbw, v->control->rgbh, v->control->rgbstr, (unsigned char(*)[v->control->rgbw][v->control->rgbstr / 4][4])v->control->rgb,
			v->settings.mode, 1. / v->control->max, v->settings.winlow, v->settings.winhigh, v->settings.phrot,
			v->control->rgbw, v->control->buf);

		char output_name[256];
		int len = snprintf(output_name, 256, "%s/mov-%04d.png", folder, f);

		if (len + 1 >= sizeof(output_name)) {

			ui_set_msg(v, "Error: writing image file.\n");
			goto fail;
		}

		if (gtk_ui_save_png(v, output_name)) {

			ui_set_msg(v, "Error: writing image file.\n");
			goto fail;
		}
	}

	return true;

fail:
	return false;
}


struct xy_s { float x; float y; };

static struct xy_s pos2screen(const struct view_s* v, const float (*pos)[DIMS])
{
	float x = (*pos)[v->settings.xdim];
	float y = (*pos)[v->settings.ydim];

	if ((XY == v->settings.flip) || (XO == v->settings.flip))
		x = v->control->dims[v->settings.xdim] - 1 - x;

	if ((XY == v->settings.flip) || (OY == v->settings.flip))
		y = v->control->dims[v->settings.ydim] - 1 - y;

	// shift to the center of pixels
	x += 0.5;
	y += 0.5;

	x *= v->settings.xzoom;
	y *= v->settings.yzoom;

	if (v->settings.plot)
		y = v->control->rgbh / 2;

	return (struct xy_s){ x, y };
}

static void screen2pos(const struct view_s* v, float (*pos)[DIMS], struct xy_s xy)
{
	for (unsigned int i = 0; i < DIMS; i++)
		(*pos)[i] = v->settings.pos[i];

	float x = xy.x / v->settings.xzoom - 0.5;
	float y = xy.y / v->settings.yzoom - 0.5;

	if ((XY == v->settings.flip) || (XO == v->settings.flip))
		x = v->control->dims[v->settings.xdim] - 1 - x;

	if ((XY == v->settings.flip) || (OY == v->settings.flip))
		y = v->control->dims[v->settings.ydim] - 1 - y;

	(*pos)[v->settings.xdim] = roundf(x);

	if (!v->settings.plot)
		(*pos)[v->settings.ydim] = roundf(y);
}



static void clear_status_bar(struct view_s* v)
{
	char buf = '\0';
	ui_set_msg(v, &buf);
}


static void update_status_bar(struct view_s* v, const float (*pos)[DIMS])
{
	int x2 = (*pos)[v->settings.xdim];
	int y2 = (*pos)[v->settings.ydim];

	complex float val = sample(DIMS, *pos, v->control->dims, v->control->strs, v->settings.interpolation, v->control->data);

	// FIXME: make sure this matches exactly the pixel
	char buf[100];
	snprintf(buf, 100, "Pos: %03d %03d Magn: %.3e Val: %+.3e%+.3ei Arg: %+.2f", x2, y2,
			cabsf(val), crealf(val), cimagf(val), cargf(val));

	ui_set_msg(v, buf);
}


void view_redraw(struct view_s* v)
{
	v->control->rgbw = v->control->dims[v->settings.xdim] * v->settings.xzoom;
	v->control->rgbh = v->control->dims[v->settings.ydim] * v->settings.yzoom;
	v->control->rgbstr = 4 * v->control->rgbw;

	if (v->invalid) {

		v->control->buf = realloc(v->control->buf, v->control->rgbh * v->control->rgbw * sizeof(complex float));

		update_buf_view(v);

		v->invalid = false;
		v->rgb_invalid = true;
	}

	if (v->rgb_invalid) {

		ui_rgbbuffer_disconnect(v);
		v->control->rgb = realloc(v->control->rgb, v->control->rgbh * v->control->rgbstr);
		ui_rgbbuffer_connect(v, v->control->rgbw, v->control->rgbh, v->control->rgbstr, v->control->rgb);

		(v->settings.plot ? draw_plot : draw)(v->control->rgbw, v->control->rgbh, v->control->rgbstr,
			(unsigned char(*)[v->control->rgbw][v->control->rgbstr / 4][4])v->control->rgb,
			v->settings.mode, v->settings.absolute_windowing ? 1. : 1. / v->control->max, v->settings.winlow, v->settings.winhigh, v->settings.phrot,
			v->control->rgbw, v->control->buf);

		v->rgb_invalid = false;
	}


	// add_text(v->ui->source, 3, 3, 10, v->name);

	if (v->settings.cross_hair) {

		float posi[DIMS];
		for (unsigned int i = 0; i < DIMS; i++)
			posi[i] = v->settings.pos[i];

		struct xy_s xy = pos2screen(v, &posi);

		draw_line(v->control->rgbw, v->control->rgbh, v->control->rgbstr, (unsigned char (*)[v->control->rgbw][v->control->rgbstr / 4][4])v->control->rgb,
				0, (int)xy.y, v->control->rgbw - 1, (int)xy.y, (v->settings.xdim > v->settings.ydim) ? &color_red : &color_blue);

		draw_line(v->control->rgbw, v->control->rgbh, v->control->rgbstr, (unsigned char (*)[v->control->rgbw][v->control->rgbstr / 4][4])v->control->rgb,
				(int)xy.x, 0, (int)xy.x, v->control->rgbh - 1, (v->settings.xdim < v->settings.ydim) ? &color_red : &color_blue);

//		float coords[4][2] = { { 0, 0 }, { 100, 0 }, { 0, 100 }, { 100, 100 } };
//		draw_grid(v->control->rgbw, v->control->rgbh, v->control->rgbstr, (unsigned char (*)[v->control->rgbw][v->control->rgbstr / 4][4])v->control->rgb, &coords, 4, &color_white);
	}

	if (v->control->status_bar) {

		float posi[DIMS];
		for (unsigned int i = 0; i < DIMS; i++)
			posi[i] = v->settings.pos[i];

		update_status_bar(v, &posi);

		for (struct view_s* v2 = v->next; v2 != v; v2 = v2->next)
			if (v->sync && v2->sync)
				update_status_bar(v2, &posi);
	}
}




struct view_s* create_view(const char* name, const long pos[DIMS], const long dims[DIMS], const complex float* data)
{
	long sq_dims[2] = { 0 };

	int l = 0;

	for (int i = 0; (i < DIMS) && (l < 2); i++)
		if (1 != dims[i])
			sq_dims[l++] = i;

	assert(2 == l);

	struct view_s* v = xmalloc(sizeof(struct view_s));
	v->control = xmalloc(sizeof(struct view_control_s));
	v->ui = xmalloc(sizeof(struct view_gtk_ui_s));

	v->next = v->prev = v;
	v->sync = true;

	v->settings.cross_hair = false;
	v->control->status_bar = false;

	v->name = name;
	v->control->max = 0.;

	v->settings.pos = xmalloc(DIMS * sizeof(long));

	for (int i = 0; i < DIMS; i++)
		v->settings.pos[i] = (NULL != pos) ? pos[i] : 0;

	v->settings.xdim = sq_dims[0];
	v->settings.ydim = sq_dims[1];

	v->settings.plot = false;

	v->settings.xzoom = 2.;
	v->settings.yzoom = 2.;

	v->ui->source = NULL;
	v->control->rgb = NULL;
	v->control->buf = NULL;


	md_copy_dims(DIMS, v->control->dims, dims);
	md_calc_strides(DIMS, v->control->strs, dims, sizeof(complex float));
	v->control->data = data;

	v->control->geom_flags = 0ul;
	v->control->geom = NULL;
	v->control->geom_current = NULL;

	v->settings.winlow = 0.;
	v->settings.winhigh = 1.;
	v->settings.phrot = 0.;

	v->control->lastx = -1;
	v->control->lasty = -1;

	v->invalid = true;

	return v;
}


static void delete_view(struct view_s* v)
{
	v->next->prev = v->prev;
	v->prev->next = v->next;

	free(v->control->buf);
	free(v->control->rgb);

#if 0
	free(v->settings.pos);
	//free(v);
#endif
}

static void view_set_windowing(struct view_s* v)
{
	double max = v->settings.absolute_windowing ? v->control->max : 1;
	max = MIN(1.e10, max);
	double inc = exp(log(10) * round(log(max) / log(10.))) * 0.001;
	int digits =  MAX(3, 3 - (int)round(log(max) / log(10.)));
	ui_set_windowing(v, max, inc, digits);
}

void view_toggle_absolute_windowing(struct view_s* v)
{
	if (v->settings.absolute_windowing) {

		long size = md_calc_size(DIMS, v->control->dims);
		double max = 0.;
		
		for (long j = 0; j < size; j++)
			if (max < cabsf(v->control->data[j]))
				max = cabsf(v->control->data[j]);

		max = MIN(1.e10, max);

		if (0. == max)
			max = 1.;

		v->control->max = max;
		v->settings.winhigh = MIN(v->settings.winhigh / v->control->max, 1);
		v->settings.winlow = MIN(v->settings.winlow / v->control->max, 1);

		v->settings.absolute_windowing = false;
	} else {

		v->settings.winhigh *= v->control->max;
		v->settings.winlow *= v->control->max;
		v->settings.absolute_windowing = true;
	}

	view_set_windowing(v);
}

void view_click(struct view_s* v, int x, int y, int button)
{
	struct xy_s xy = { x, y };

	float pos[DIMS];
	screen2pos(v, &pos, xy);

	if (1 == button) {

		v->settings.cross_hair = false;
		v->control->status_bar = false;
		clear_status_bar(v);

		v->rgb_invalid = true;
		ui_trigger_redraw(v);
	}

	if (2 == button) {

		v->settings.cross_hair = true;
		v->control->status_bar = true;

		ui_set_position(v, v->settings.xdim, pos[v->settings.xdim]);
		ui_set_position(v, v->settings.ydim, pos[v->settings.ydim]);
	}
}

void view_windowing_move(struct view_s* v, int x, int y, double inc_low, double inc_high)
{
	if (-1 != v->control->lastx) {

		double low = v->settings.winlow;
		double high = v->settings.winhigh;

		low -= (x - v->control->lastx) * inc_low * 5;
		high -= (y - v->control->lasty) * inc_high * 5;

		if (high > low) {

			v->settings.winlow = low;
			v->settings.winhigh = high;
			ui_set_values(v);
			view_sync_windowing(v);
		}
	}

	v->control->lastx = x;
	v->control->lasty = y;
}

void view_windowing_release(struct view_s* v)
{
	v->control->lastx = -1;
	v->control->lasty = -1;
}



static int nr_windows = 0;

extern void view_window_close(struct view_s* v)
{
	delete_view(v);

	if (0 == --nr_windows)
		ui_loop_quit();
}




extern struct view_s* window_new(const char* name, const long pos[DIMS], const long dims[DIMS], const complex float* x, bool absolute_windowing)
{
	struct view_s* v = create_view(name, pos, dims, x);
	v->settings.absolute_windowing = absolute_windowing;

	ui_window_new(v, DIMS, dims);
	nr_windows++;

//	fit_callback(NULL, v);
	view_refresh(v);
	ui_pull_geom(v);
	ui_pull_window(v);
	view_set_windowing(v);

	return v;
}

void window_connect_sync(struct view_s* v, struct view_s* v2)
{
	// add to linked list for sync
	v2->next = v->next;
	v->next->prev = v2;
	v2->prev = v;
	v->next = v2;

	view_touch_rgb_settings(v);
}

struct view_s* view_clone(struct view_s* v, const long pos[DIMS])
{
	struct view_s* v2 = window_new(v->name, pos, v->control->dims, v->control->data, v->settings.absolute_windowing);

	window_connect_sync(v, v2);

	return v2;
}

const long *view_get_dims(struct view_s* v)
{
	return v->control->dims;
}
