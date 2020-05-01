/*
   This file is part of darktable,
   Copyright (C) 2019-2020 darktable developers.

   darktable is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   darktable is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "bauhaus/bauhaus.h"
#include "common/iop_profile.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/darktable.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop_math.h"
#include "dtgtk/button.h"
#include "dtgtk/drawingarea.h"
#include "dtgtk/expander.h"
#include "dtgtk/paint.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "gui/color_picker_proxy.h"
#include "iop/iop_api.h"
#include "iop/gaussian_elimination.h"


#include "develop/imageop.h"
#include "gui/draw.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>



#define DT_GUI_CURVE_EDITOR_INSET DT_PIXEL_APPLY_DPI(1)


DT_MODULE_INTROSPECTION(2, dt_iop_filmicrgb_params_t)

/**
 * DOCUMENTATION
 *
 * This code ports :
 * 1. Troy Sobotka's filmic curves for Blender (and other softs)
 *      https://github.com/sobotka/OpenAgX/blob/master/lib/agx_colour.py
 * 2. ACES camera logarithmic encoding
 *        https://github.com/ampas/aces-dev/blob/master/transforms/ctl/utilities/ACESutil.Lin_to_Log2_param.ctl
 *
 * The ACES log implementation is taken from the profile_gamma.c IOP
 * where it works in camera RGB space. Here, it works on an arbitrary RGB
 * space. ProPhotoRGB has been chosen for its wide gamut coverage and
 * for conveniency because it's already in darktable's libs. Any other
 * RGB working space could work. This chouice could (should) also be
 * exposed to the user.
 *
 * The filmic curves are tonecurves intended to simulate the luminance
 * transfer function of film with "S" curves. These could be reproduced in
 * the tonecurve.c IOP, however what we offer here is a parametric
 * interface useful to remap accurately and promptly the middle grey
 * to any arbitrary value chosen accordingly to the destination space.
 *
 * The combined use of both define a modern way to deal with large
 * dynamic range photographs by remapping the values with a comprehensive
 * interface avoiding many of the back and forth adjustments darktable
 * is prone to enforce.
 *
 * */


 /** Note :
 * we use finite-math-only and fast-math because divisions by zero are manually avoided in the code
 * fp-contract=fast enables hardware-accelerated Fused Multiply-Add
 * the rest is loop reorganization and vectorization optimization
 **/
#if defined(__GNUC__)
#pragma GCC optimize ("unroll-loops", "tree-loop-if-convert", \
                      "tree-loop-distribution", "no-strict-aliasing", \
                      "loop-interchange", "loop-nest-optimize", "tree-loop-im", \
                      "unswitch-loops", "tree-loop-ivcanon", "ira-loop-pressure", \
                      "split-ivs-in-unroller", "variable-expansion-in-unroller", \
                      "split-loops", "ivopts", "predictive-commoning",\
                      "tree-loop-linear", "loop-block", "loop-strip-mine", \
                      "finite-math-only", "fp-contract=fast", "fast-math")
#endif

typedef enum dt_iop_filmicrgb_pickcolor_type_t
{
  DT_PICKPROFLOG_NONE = 0,
  DT_PICKPROFLOG_GREY_POINT = 1,
  DT_PICKPROFLOG_BLACK_POINT = 2,
  DT_PICKPROFLOG_WHITE_POINT = 3,
  DT_PICKPROFLOG_AUTOTUNE = 4
} dt_iop_filmicrgb_pickcolor_type_t;


typedef enum dt_iop_filmicrgb_methods_type_t
{
  DT_FILMIC_METHOD_NONE = 0,
  DT_FILMIC_METHOD_MAX_RGB = 1,
  DT_FILMIC_METHOD_LUMINANCE = 2,
  DT_FILMIC_METHOD_POWER_NORM = 3
} dt_iop_filmicrgb_methods_type_t;


typedef enum dt_iop_filmicrgb_curve_type_t
{
  DT_FILMIC_CURVE_POLY_4 = 0,
  DT_FILMIC_CURVE_POLY_3 = 1
} dt_iop_filmicrgb_curve_type_t;


typedef enum dt_iop_filmicrgb_colorscience_type_t
{
  DT_FILMIC_COLORSCIENCE_V1 = 0,
  DT_FILMIC_COLORSCIENCE_V2 = 1,
} dt_iop_filmicrgb_colorscience_type_t;


typedef struct dt_iop_filmic_rgb_spline_t
{
  float DT_ALIGNED_PIXEL M1[4], M2[4], M3[4], M4[4], M5[4]; // factors for the interpolation polynom
  float latitude_min, latitude_max;                         // bounds of the latitude == linear part by design
  float y[5];                                               // controls nodes
  float x[5];                                               // controls nodes
} dt_iop_filmic_rgb_spline_t;


typedef struct dt_iop_filmicrgb_params_t
{
  float grey_point_source;
  float black_point_source;
  float white_point_source;
  float reconstruct_threshold;
  float reconstruct_feather;
  float reconstruct_bloom_vs_details;
  float reconstruct_grey_vs_color;
  float reconstruct_structure_vs_texture;
  float security_factor;
  float grey_point_target;
  float black_point_target;
  float white_point_target;
  float output_power;
  float latitude;
  float contrast;
  float saturation;
  float balance;
  int preserve_color;
  int version;
  int auto_hardness;
  int custom_grey;
  int high_quality_reconstruction;
  dt_iop_filmicrgb_curve_type_t shadows;
  dt_iop_filmicrgb_curve_type_t highlights;
} dt_iop_filmicrgb_params_t;

typedef struct dt_iop_filmicrgb_gui_data_t
{
  GtkWidget *white_point_source;
  GtkWidget *grey_point_source;
  GtkWidget *black_point_source;
  GtkWidget *reconstruct_threshold, *reconstruct_bloom_vs_details, *reconstruct_grey_vs_color, *reconstruct_structure_vs_texture, *reconstruct_feather;
  GtkWidget *security_factor;
  GtkWidget *auto_button;
  GtkWidget *grey_point_target;
  GtkWidget *white_point_target;
  GtkWidget *black_point_target;
  GtkWidget *output_power;
  GtkWidget *latitude;
  GtkWidget *contrast;
  GtkWidget *saturation;
  GtkWidget *balance;
  GtkWidget *preserve_color;
  GtkWidget *autoset_display_gamma;
  GtkWidget *shadows, *highlights;
  GtkWidget *version;
  GtkWidget *auto_hardness;
  GtkWidget *custom_grey;
  GtkWidget *high_quality_reconstruction;
  GtkNotebook *notebook;
  dt_iop_color_picker_t color_picker;
  GtkDrawingArea *area;
  struct dt_iop_filmic_rgb_spline_t spline DT_ALIGNED_ARRAY;
  gint show_mask;
} dt_iop_filmicrgb_gui_data_t;

typedef struct dt_iop_filmicrgb_data_t
{
  float max_grad;
  float white_source;
  float grey_source;
  float black_source;
  float reconstruct_threshold;
  float reconstruct_feather;
  float reconstruct_bloom_vs_details;
  float reconstruct_grey_vs_color;
  float reconstruct_structure_vs_texture;
  float dynamic_range;
  float saturation;
  float output_power;
  float contrast;
  float sigma_toe, sigma_shoulder;
  int preserve_color;
  int version;
  int high_quality_reconstruction;
  struct dt_iop_filmic_rgb_spline_t spline DT_ALIGNED_ARRAY;
} dt_iop_filmicrgb_data_t;


typedef struct dt_iop_filmicrgb_global_data_t
{
  int kernel_filmic_rgb_split;
  int kernel_filmic_rgb_chroma;
} dt_iop_filmicrgb_global_data_t;


const char *name()
{
  return _("filmic rgb");
}

int default_group()
{
  return IOP_GROUP_TONE;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_rgb;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params,
                  const int new_version)
{
  if(old_version == 1 && new_version == 2)
  {
    typedef struct dt_iop_filmicrgb_params_v1_t
    {
      float grey_point_source;
      float black_point_source;
      float white_point_source;
      float security_factor;
      float grey_point_target;
      float black_point_target;
      float white_point_target;
      float output_power;
      float latitude;
      float contrast;
      float saturation;
      float balance;
      int preserve_color;
    } dt_iop_filmicrgb_params_v1_t;

    dt_iop_filmicrgb_params_v1_t *o = (dt_iop_filmicrgb_params_v1_t *)old_params;
    dt_iop_filmicrgb_params_t *n = (dt_iop_filmicrgb_params_t *)new_params;
    dt_iop_filmicrgb_params_t *d = (dt_iop_filmicrgb_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

    n->grey_point_source = o->grey_point_source;
    n->white_point_source = o->white_point_source;
    n->black_point_source = o->black_point_source;
    n->security_factor = o->security_factor;
    n->grey_point_target = o->grey_point_target;
    n->black_point_target = o->black_point_target;
    n->white_point_target = o->white_point_target;
    n->output_power = o->output_power;
    n->latitude = o->latitude;
    n->contrast = o->contrast;
    n->saturation = o->saturation;
    n->balance = o->balance;
    n->preserve_color = o->preserve_color;
    n->shadows = DT_FILMIC_CURVE_POLY_4;
    n->highlights = DT_FILMIC_CURVE_POLY_3;
    n->reconstruct_threshold = 3.0f; // for old edits, this ensures clipping threshold >> white level, so it's a no-op
    n->reconstruct_bloom_vs_details = d->reconstruct_bloom_vs_details;
    n->reconstruct_grey_vs_color = d->reconstruct_grey_vs_color;
    n->reconstruct_structure_vs_texture = d->reconstruct_structure_vs_texture;
    n->reconstruct_feather = 3.0f;
    n->version = DT_FILMIC_COLORSCIENCE_V1;
    n->auto_hardness = TRUE;
    n->custom_grey = TRUE;
    n->high_quality_reconstruction = FALSE;
    return 0;
  }
  return 1;
}


void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "white exposure"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "black exposure"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "middle grey luminance"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "dynamic range scaling"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "contrast"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "latitude"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "shadows highlights balance"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "extreme luminance saturation"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "target black luminance"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "target middle grey"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "target white luminance"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "target power transfer function"));
  dt_accel_register_combobox_iop(self, FALSE, NC_("accel", "preserve chrominance"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

  dt_accel_connect_slider_iop(self, "white exposure", GTK_WIDGET(g->white_point_source));
  dt_accel_connect_slider_iop(self, "black exposure", GTK_WIDGET(g->black_point_source));
  dt_accel_connect_slider_iop(self, "middle grey luminance", GTK_WIDGET(g->grey_point_source));
  dt_accel_connect_slider_iop(self, "dynamic range scaling", GTK_WIDGET(g->security_factor));
  dt_accel_connect_slider_iop(self, "contrast", GTK_WIDGET(g->contrast));
  dt_accel_connect_slider_iop(self, "latitude", GTK_WIDGET(g->latitude));
  dt_accel_connect_slider_iop(self, "shadows highlights balance", GTK_WIDGET(g->balance));
  dt_accel_connect_slider_iop(self, "extreme luminance saturation", GTK_WIDGET(g->saturation));
  dt_accel_connect_slider_iop(self, "target black luminance", GTK_WIDGET(g->black_point_target));
  dt_accel_connect_slider_iop(self, "target middle grey", GTK_WIDGET(g->grey_point_target));
  dt_accel_connect_slider_iop(self, "target white luminance", GTK_WIDGET(g->white_point_target));
  dt_accel_connect_slider_iop(self, "target power transfer function", GTK_WIDGET(g->output_power));
  dt_accel_connect_combobox_iop(self, "preserve chrominance", GTK_WIDGET(g->preserve_color));
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float clamp_simd(const float x)
{
  return fminf(fmaxf(x, 0.0f), 1.0f);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(pixel:16)
#endif
static inline float pixel_rgb_norm_power(const float pixel[4])
{
  // weird norm sort of perceptual. This is black magic really, but it looks good.
  // the full norm is (R^3 + G^3 + B^3) / (R^2 + G^2 + B^2) and it should be in ]0; +infinity[

  float numerator = 0.0f;
  float denominator = 0.0f;

#ifdef _OPENMP
#pragma omp simd aligned(pixel:16) reduction(+:numerator, denominator)
#endif
  for(int c = 0; c < 3; c++)
  {
    const float value = fabsf(pixel[c]);
    const float RGB_square = value * value;
    const float RGB_cubic = RGB_square * value;
    numerator += RGB_cubic;
    denominator += RGB_square;
  }

  return numerator / fmaxf(denominator, 1e-12f);  // prevent from division-by-0 (note: (1e-6)^2 = 1e-12
}


#ifdef _OPENMP
#pragma omp declare simd aligned(pixel:16)
#endif
static inline float get_pixel_norm(const float pixel[4], const dt_iop_filmicrgb_methods_type_t variant,
                                   const dt_iop_order_iccprofile_info_t *const work_profile)
{
  switch(variant)
  {
    case(DT_FILMIC_METHOD_MAX_RGB):
      return fmaxf(fmaxf(pixel[0], pixel[1]), pixel[2]);

    case(DT_FILMIC_METHOD_LUMINANCE):
      return (work_profile) ? dt_ioppr_get_rgb_matrix_luminance(pixel,
                                                                work_profile->matrix_in,
                                                                work_profile->lut_in,
                                                                work_profile->unbounded_coeffs_in,
                                                                work_profile->lutsize,
                                                                work_profile->nonlinearlut)
                            : dt_camera_rgb_luminance(pixel);

    case(DT_FILMIC_METHOD_POWER_NORM):
      return pixel_rgb_norm_power(pixel);

    default:
      return (work_profile) ? dt_ioppr_get_rgb_matrix_luminance(pixel,
                                                                work_profile->matrix_in,
                                                                work_profile->lut_in,
                                                                work_profile->unbounded_coeffs_in,
                                                                work_profile->lutsize,
                                                                work_profile->nonlinearlut)
                            : dt_camera_rgb_luminance(pixel);
  }
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float log_tonemapping_v1(const float x, const float grey, const float black, const float dynamic_range)
{
  const float temp = (log2f(x / grey) - black) / dynamic_range;
  return fmaxf(fminf(temp, 1.0f), 1.52587890625e-05f);
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float log_tonemapping_v2(const float x, const float grey, const float black, const float dynamic_range)
{
  return clamp_simd((log2f(x / grey) - black) / dynamic_range);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(M1, M2, M3, M4:16)
#endif
static inline float filmic_spline(const float x,
                                  const float M1[4], const float M2[4], const float M3[4], const float M4[4], const float M5[4],
                                  const float latitude_min, const float latitude_max)
{
  return (x < latitude_min) ? M1[0] + x * (M2[0] + x * (M3[0] + x * (M4[0] + x * M5[0]))) : // toe
         (x > latitude_max) ? M1[1] + x * (M2[1] + x * (M3[1] + x * (M4[1] + x * M5[1]))) : // shoulder
                              M1[2] + x * (M2[2] + x * (M3[2] + x * (M4[2] + x * M5[2])));  // latitude
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float filmic_desaturate_v1(const float x, const float sigma_toe, const float sigma_shoulder, const float saturation)
{
  const float radius_toe = x;
  const float radius_shoulder = 1.0f - x;

  const float key_toe = expf(-0.5f * radius_toe * radius_toe / sigma_toe);
  const float key_shoulder = expf(-0.5f * radius_shoulder * radius_shoulder / sigma_shoulder);

  return 1.0f - clamp_simd((key_toe + key_shoulder) / saturation) ;
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float filmic_desaturate_v2(const float x, const float sigma_toe, const float sigma_shoulder, const float saturation)
{
  const float radius_toe = x;
  const float radius_shoulder = 1.0f - x;
  const float sat2 = 0.5f / sqrtf(saturation);
  const float key_toe = expf(-radius_toe * radius_toe / sigma_toe * sat2);
  const float key_shoulder = expf(-radius_shoulder * radius_shoulder / sigma_shoulder * sat2);

  return (saturation - (key_toe + key_shoulder) * (saturation));
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float linear_saturation(const float x, const float luminance, const float saturation)
{
  return luminance + saturation * (x - luminance);
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float fmaxabsf(const float a, const float b)
{
  const float abs_a = fabsf(a);
  const float abs_b = fabsf(b);
  return (abs_a > abs_b) ? a : b;
}

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float fminabsf(const float a, const float b)
{
  const float abs_a = fabsf(a);
  const float abs_b = fabsf(b);
  return (abs_a < abs_b) ? a : b;
}

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float sqf(const float x)
{
  return x*x;
}


#define MAX_NUM_SCALES 12


static inline gint mask_clipped_pixels(const float *const restrict in,
                                       float *const restrict mask,
                                       const float normalize, const float feathering,
                                       const size_t width, const size_t height, const size_t ch)
{
  /* 1. Detect if pixels are clipped and count them,
   * 2. assign them a weight in [0. ; 1.] depending on how close from clipping they are. The weights are defined
   *    by a sigmoid centered in `reconstruct_threshold` so the transition is soft and symmetrical
   */

  int clipped = 0;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(in, mask, normalize, feathering, width, height, ch) \
  schedule(simd:static) aligned(mask, in:64) reduction(+:clipped)
#endif
  for(size_t k = 0; k < height * width * ch; k += ch)
  {
    const float pix_max = sqrtf(sqf(in[k]) + sqf(in[k+ 1]) + sqf(in[k + 2]));
    const float argument = -pix_max * normalize + feathering;
    const float weight = 1.0f / ( 1.0f + exp2f(argument));
    mask[k / ch] = weight;

    // at x = 4, the sigmoid produces opacity = 5.882 %.
    // any x > 4 will produce neglictible changes over the image,
    // especially since we have reduced visual sensitivity in highlights.
    // so we discard pixels for argument > 4. for they are not worth computing.
    clipped += (4.f > argument);
  }

  // If clipped area is < 9 pixels, recovery is not worth the computational cost, so skip it.
  return (clipped > 9);
}


// B spline filter
#define fsize 5
static const float DT_ALIGNED_ARRAY filter[fsize] = { 1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f };


inline static void blur_2D_Bspline_vertical(const float *const restrict in, float *const restrict out,
                                            const size_t width, const size_t height, const size_t ch, const size_t mult,
                                            const int bound_left, const int bound_right)
{
  // À-trous B-spline interpolation/blur shifted by mult
  // Convolve B-spline filter over lines
  #ifdef _OPENMP
  #pragma omp parallel for default(none) \
    dt_omp_firstprivate(in, out, filter, width, height, ch, bound_left, bound_right, mult) \
    schedule(simd:static) collapse(2)
  #endif
  for(size_t i = 0; i < height; i++)
    for(size_t j = 0; j < width; j++)
    {
      const size_t index_out = (i * width + j) * ch;
      float DT_ALIGNED_PIXEL accumulator[4] = { 0.0f };

      // Are we in the boundary zone that needs bound checking ?
      const int check = !((j > 2 * mult) && (j < width - 2 * mult));

      // -funswitch-loops should compile 2 loops, for each check outcome
      if(check)
      {
        #ifdef _OPENMP
        #pragma omp simd aligned(in, filter:64) aligned(accumulator:16) reduction(+:accumulator)
        #endif
        for(size_t jj = 0; jj < fsize; ++jj)
          for(size_t c = 0; c < 3; ++c)
          {
            int index_x = mult * (jj - (fsize - 1) / 2) + j;
            index_x = (index_x < bound_left)  ? bound_left  :
                      (index_x > bound_right) ? bound_right :
                                                index_x     ;


            accumulator[c] += filter[jj] * in[(i * width + index_x) * ch + c];
          }
      }
      else // fast-track
      {
        #ifdef _OPENMP
        #pragma omp simd aligned(in, filter:64) aligned(accumulator:16) reduction(+:accumulator)
        #endif
        for(size_t jj = 0; jj < fsize; ++jj)
          for(size_t c = 0; c < 3; ++c)
          {
            const size_t index_x = mult * (jj - (fsize - 1) / 2) + j;
            accumulator[c] += filter[jj] * in[(i * width + index_x) * ch + c];
          }
      }

      #ifdef _OPENMP
      #pragma omp simd aligned(out:64) aligned(accumulator:16)
      #endif
      for(size_t c = 0; c < 3; ++c)
        out[index_out + c] = accumulator[c];

    }
}


inline static void blur_2D_Bspline_horizontal(const float *const restrict in, float *const restrict out,
                                              const size_t width, const size_t height, const size_t ch, const size_t mult,
                                              const int bound_top, const int bound_bot)
{
  // À-trous B-spline interpolation/blur shifted by mult
  // Convolve B-spline filter over columns
  #ifdef _OPENMP
  #pragma omp parallel for default(none) \
    dt_omp_firstprivate(stdout, out, in, width, height, ch, filter, bound_bot, bound_top, mult) \
    schedule(simd:static) collapse(2)
  #endif
  for(size_t i = 0; i < height; i++)
    for(size_t j = 0; j < width; j++)
    {
      const size_t index_out = (i * width + j) * ch;
      float DT_ALIGNED_PIXEL accumulator[4] = { 0.0f };

      // Are we in the boundary zone that needs bound checking ?
      const int check = !((i > 2 * mult) && (i < height - 2 * mult));

      // -funswitch-loops should compile 2 loops, for each check outcome
      if(check)
      {
        #ifdef _OPENMP
        #pragma omp simd aligned(in, filter:64) aligned(accumulator:16) reduction(+:accumulator)
        #endif
        for(size_t ii = 0; ii < fsize; ++ii)
          for(size_t c = 0; c < 3; ++c)
          {
            int index_y = mult * (ii - (fsize - 1) / 2) + i;
            index_y = (index_y < bound_top) ? bound_top :
                      (index_y > bound_bot) ? bound_bot :
                                              index_y   ;
            accumulator[c] += filter[ii] * in[(index_y * width + j) * ch + c];
          }
      }
      else // fast-track
      {
        for(size_t ii = 0; ii < fsize; ++ii)
        {
          const size_t index_y = mult * (ii - (fsize - 1) / 2) + i;

          #ifdef _OPENMP
          #pragma omp simd aligned(in, filter:64) aligned(accumulator:16) reduction(+:accumulator)
          #endif
          for(size_t c = 0; c < 3; ++c)
            accumulator[c] += filter[ii] * in[(index_y * width + j) * ch + c];
        }
      }

      #ifdef _OPENMP
      #pragma omp simd aligned(out:64) aligned(accumulator:16)
      #endif
      for(size_t c = 0; c < ch; ++c) out[index_out + c] = accumulator[c];
    }
}


inline static void wavelets_reconstruct_RGB(const float *const restrict HF, const float *const restrict LF,
                                            const float *const restrict texture, const float *const restrict mask,
                                            float *const restrict reconstructed,
                                            const size_t width, const size_t height, const size_t ch,
                                            const float gamma, const float gamma_comp, const float beta, const float beta_comp, const float delta,
                                            const size_t s, const size_t scales)
{
  #ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(width, height, ch, HF, LF, texture, mask, reconstructed, gamma, gamma_comp, beta, beta_comp, delta, s, scales) \
  schedule(simd:static) aligned(HF, LF, texture, mask, reconstructed:64)
  #endif
  for(size_t k = 0; k < height * width * ch; k += ch)
  {
    const float alpha = mask[k / ch];

    // cache RGB wavelets scales just to be sure the compiler doesn't reload them
    const float DT_ALIGNED_ARRAY HF_c[4] = { HF[k], HF[k + 1], HF[k + 2], HF[k + 3] };
    const float DT_ALIGNED_ARRAY LF_c[4] = { LF[k], LF[k + 1], LF[k + 2], LF[k + 3] };

    // synthesize the max of all RGB channels texture as a flat texture term for the whole pixel
    // this is useful if only 1 or 2 channels are clipped, so we transfer the valid/sharpest texture on the other channels
    float grey_texture = gamma * texture[k / ch];

    // synthesize the max of all interpolated/inpainted RGB channels as a flat details term for the whole pixel
    // this is smoother than grey_texture and will fill holes smoothly in details layers if grey_texture ~= 0.f
    float grey_details = gamma_comp * fmaxabsf(fmaxabsf(HF_c[0], HF_c[1]), HF_c[2]);

    // synthesize both terms with weighting
    // when beta_comp ~= 1.0, we force the reconstruction to be achromatic, which may help with gamut issues or magenta highlights.
    const float grey_HF = beta_comp * (grey_details + grey_texture);

    // synthesize the min of all low-frequency RGB channels as a flat structure term for the whole pixel
    // when beta_comp ~= 1.0, we force the reconstruction to be achromatic, which may help with gamut issues or magenta highlights.
    float grey_residual = beta_comp * fminf(fminf(LF_c[0], LF_c[1]), LF_c[2]);

    for(size_t c = 0; c < 3; c++)
    {
      // synthesize interpolated/inpainted RGB channels color details residuals and weigh them
      // this brings back some color on top of the grey_residual
      const float color_residual = LF_c[c] * beta;

      // synthesize interpolated/inpainted RGB channels color details and weigh them
      // this brings back some color on top of the grey_details
      const float color_details = HF_c[c] * beta * gamma_comp;

      // reconstruction
      reconstructed[k + c] += alpha * (delta * (grey_HF + color_details) + (grey_residual + color_residual) / (float)scales);
    }
  }
}


inline static void wavelets_reconstruct_ratios(const float *const restrict HF, const float *const restrict LF,
                                               const float *const restrict texture, const float *const restrict mask,
                                               float *const restrict reconstructed,
                                               const size_t width, const size_t height, const size_t ch,
                                               const float gamma, const float gamma_comp, const float beta, const float beta_comp, const float delta,
                                               const size_t s, const size_t scales)
{
  /*
  * This is the adapted version of the RGB reconstruction
  * RGB contain high frequencies that we try to recover, so we favor them in the reconstruction.
  * The ratios represent the chromaticity in image and contain low frequencies in the absence of noise or aberrations,
  * so, here, we favor them instead.
  *
  * Consequences : 
  *  1. use min of interpolated channels details instead of max, to get smoother details
  *  4. use the max of low frequency channels instead of min, to favor achromatic solution.
  *
  * Note : ratios close to 1 mean higher spectral purity (more white). Ratios close to 0 mean lower spectral purity (more colorful)
  */
  #ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(width, height, ch, HF, LF, texture, mask, reconstructed, gamma, gamma_comp, beta, beta_comp, delta, s, scales) \
  schedule(simd:static) aligned(HF, LF, texture, mask, reconstructed:64)
  #endif
  for(size_t k = 0; k < height * width * ch; k += ch)
  {
    const float alpha = mask[k / ch];

    // cache RGB wavelets scales just to be sure the compiler doesn't reload them
    const float DT_ALIGNED_ARRAY HF_c[4] = { HF[k], HF[k + 1], HF[k + 2], HF[k + 3] };
    const float DT_ALIGNED_ARRAY LF_c[4] = { LF[k], LF[k + 1], LF[k + 2], LF[k + 3] };

    // synthesize the max of all RGB channels texture as a flat texture term for the whole pixel
    // this is useful if only 1 or 2 channels are clipped, so we transfer the valid/sharpest texture on the other channels
    float grey_texture = gamma * texture[k / ch];

    // synthesize the max of all interpolated/inpainted RGB channels as a flat details term for the whole pixel
    // this is smoother than grey_texture and will fill holes smoothly in details layers if grey_texture ~= 0.f
    float grey_details = gamma_comp * fmaxabsf(fmaxabsf(HF_c[0], HF_c[1]), HF_c[2]);

    // synthesize both terms with weighting
    // when beta_comp ~= 1.0, we force the reconstruction to be achromatic, which may help with gamut issues or magenta highlights.
    const float grey_HF = beta_comp * (grey_details + grey_texture);

    // synthesize the min of all low-frequency RGB channels as a flat structure term for the whole pixel
    // when beta_comp ~= 1.0, we force the reconstruction to be achromatic, which may help with gamut issues or magenta highlights.
    float grey_residual = beta_comp * fmaxf(fmaxf(LF_c[0], LF_c[1]), LF_c[2]);

    for(size_t c = 0; c < 3; c++)
    {
      // synthesize interpolated/inpainted RGB channels color details residuals and weigh them
      // this brings back some color on top of the grey_residual
      const float color_residual = LF_c[c] * beta;

      // synthesize interpolated/inpainted RGB channels color details and weigh them
      // this brings back some color on top of the grey_details
      const float color_details = HF_c[c] * beta * gamma_comp;

      // reconstruction
      reconstructed[k + c] += alpha * (delta * (grey_HF + color_details) + (grey_residual + color_residual) / (float)scales);
    }
  }
}


static inline void init_reconstruct(const float *const restrict in, const float *const restrict mask, float *const restrict reconstructed,
                                    const size_t width, const size_t height, const size_t ch)
{
  // init the reconstructed buffer with non-clipped and partially clipped pixels
  // Note : it's a simple multiplied alpha blending where mask = alpha weight
  #ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(in, mask, reconstructed, width, height, ch) \
    schedule(simd:static) aligned(in, mask, reconstructed:64)
  #endif
  for(size_t k = 0; k < height * width * ch; k++)
  {
    reconstructed[k] = in[k] * (1.f - mask[k/ch]);
  }
}

static inline void wavelets_detail_level_RGB(const float *const restrict detail, const float *const restrict LF,
                                             float *const restrict HF, float *const restrict texture,
                                             const size_t width, const size_t height, const size_t ch)
{
  #ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(width, height, ch, HF, LF, detail, texture) \
    schedule(simd:static) aligned(HF, LF, detail, texture:64)
  #endif
  for(size_t k = 0; k < height * width * ch; k += ch)
  {
    for(size_t c = 0; c < 3; ++c) HF[k + c] = detail[k + c] - LF[k + c];
    texture[k / ch] = fmaxabsf(fmaxabsf(HF[k], HF[k + 1]), HF[k + 2]);
  }
}


static inline void wavelets_detail_level_ratios(const float *const restrict detail, const float *const restrict LF,
                                                float *const restrict HF, float *const restrict texture,
                                                const size_t width, const size_t height, const size_t ch)
{
  #ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(width, height, ch, HF, LF, detail, texture) \
    schedule(simd:static) aligned(HF, LF, detail, texture:64)
  #endif
  for(size_t k = 0; k < height * width * ch; k += ch)
  {
    for(size_t c = 0; c < 3; ++c) HF[k + c] = detail[k + c] - LF[k + c];
    texture[k / ch] = fminabsf(fminabsf(HF[k], HF[k + 1]), HF[k + 2]);
  }
}


static int get_scales(const dt_iop_roi_t *roi_in, const dt_dev_pixelpipe_iop_t *const piece)
{
  /* How many wavelets scales do we need to compute at current zoom level ?
   * 0. To get the same preview no matter the zoom scale, the relative image coverage ratio of the filter at
   * the coarsest wavelet level should always stay constant.
   * 1. The image coverage of each B spline filter of size `fsize` is `2^(level) * (fsize - 1) / 2 + 1` pixels
   * 2. The coarsest level filter at full resolution should cover `1/fsize` of the largest image dimension.
   * 3. The coarsest level filter at current zoom level should cover `scale/fsize` of the largest image dimension.
   *
   * So we compute the level that solves 1. subject to 3. Of course, integer rounding doesn't make that 1:1 accurate.
   */
  const float scale = roi_in->scale / piece->iscale;
  const size_t size = MAX(piece->buf_in.height * piece->iscale, piece->buf_in.width * piece->iscale);
  const int scales = floorf(log2f((2.0f * size * scale / ((fsize - 1) * fsize)) - 1.0f));
  return CLAMP(scales, 1, MAX_NUM_SCALES);
}

static inline gint reconstruct_highlights(const float *const restrict in, const float *const restrict mask,
                                          float *const restrict reconstructed, const int variant,
                                          const dt_iop_filmicrgb_data_t *const data, dt_dev_pixelpipe_iop_t *piece,
                                          const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  gint success = TRUE;

  // wavelets scales
  const int scales = get_scales(roi_in, piece);

  // wavelets scales buffers
  float *const restrict LF_even = dt_alloc_sse_ps(roi_out->width * roi_out->height * 4); // low-frequencies RGB
  float *const restrict LF_odd = dt_alloc_sse_ps(roi_out->width * roi_out->height * 4); // low-frequencies RGB
  float *const restrict HF_RGB = dt_alloc_sse_ps(roi_out->width * roi_out->height * 4); // high-frequencies RGB
  float *const restrict HF_grey = dt_alloc_sse_ps(roi_out->width * roi_out->height); // max(high-frequencies RGB) grey

  // alloc a permanent reusable buffer for intermediate computations - avoid multiple alloc/free
  float *const restrict temp = dt_alloc_sse_ps(roi_out->width * roi_out->height * 4);

  if(!LF_even || !LF_odd || !HF_RGB || !HF_grey || !temp)
  {
    dt_control_log(_("filmic highlights reconstruction failed to allocate memory, check your RAM settings"));
    success = FALSE;
    goto error;
  }

  // Init reconstructed with valid parts of image
  init_reconstruct(in, mask, reconstructed, roi_out->width, roi_out->height, 4);

  // structure inpainting vs. texture duplicating weight
  const float gamma = (data->reconstruct_structure_vs_texture);
  const float gamma_comp = 1.0f - data->reconstruct_structure_vs_texture;

  // colorful vs. grey weight
  const float beta = data->reconstruct_grey_vs_color;
  const float beta_comp = 1.f - data->reconstruct_grey_vs_color;

  // bloom vs reconstruct weight
  const float delta = data->reconstruct_bloom_vs_details;

  // boundary conditions
  const int bound_left = 0;
  const int bound_right = roi_out->width - 1;
  const int bound_top = 0;
  const int bound_bot = roi_out->height - 1;

  // À trous wavelet decompose
  // there is a paper from a guy we know that explains it : https://jo.dreggn.org/home/2010_atrous.pdf
  // the wavelets decomposition here is the same as the equalizer/atrous module,
  // but simplified because we don't need the edge-aware term, so we can seperate the convolution kernel
  // with a vertical and horizontal blur, wich is 10 multiply-add instead of 25 by pixel.
  for(int s = 0; s < scales; ++s)
  {
    const float *restrict detail;
    float *restrict LF;

    // swap buffers so we only need 2 LF buffers : the LF at scale (s-1) and the one at current scale (s)
    if(s == 0)
    {
      detail = in;
      LF = LF_odd;
    }
    else if(s % 2 != 0)
    {
      detail = LF_odd;
      LF = LF_even;
    }
    else
    {
      detail = LF_even;
      LF = LF_odd;
    }

    const int mult = 1 << s; // fancy-pants C notation for 2^s with integer type, don't be afraid

    // Compute wavelets low-frequency scales
    blur_2D_Bspline_vertical(detail, temp, roi_out->width, roi_out->height, 4, mult,
                             bound_left, bound_right);
    blur_2D_Bspline_horizontal(temp, LF, roi_out->width, roi_out->height, 4, mult,
                               bound_top, bound_bot);

    // Compute wavelets high-frequency scales and save the maximum of texture over the RGB channels
    // Note : HF_RGB = detail - LF, HF_grey = max(HF_RGB)
    if(variant == 0)
      wavelets_detail_level_RGB(detail, LF, HF_RGB, HF_grey, roi_out->width, roi_out->height, 4);
    else if(variant == 1)
      wavelets_detail_level_ratios(detail, LF, HF_RGB, HF_grey, roi_out->width, roi_out->height, 4);

    // interpolate/blur/inpaint (same thing) the RGB high-frequency to fill holes
    blur_2D_Bspline_vertical(HF_RGB, temp, roi_out->width, roi_out->height, 4, mult,
                             bound_left, bound_right);
    blur_2D_Bspline_horizontal(temp, HF_RGB, roi_out->width, roi_out->height, 4, mult,
                               bound_top, bound_bot);

    // Reconstruct clipped parts
    if(variant == 0)
      wavelets_reconstruct_RGB(HF_RGB, LF, HF_grey, mask, reconstructed, roi_out->width, roi_out->height, 4,
                               gamma, gamma_comp, beta, beta_comp, delta, s, scales);
    else if(variant == 1)
      wavelets_reconstruct_ratios(HF_RGB, LF, HF_grey, mask, reconstructed, roi_out->width, roi_out->height, 4,
                               gamma, gamma_comp, beta, beta_comp, delta, s, scales);
  }

error:
  if(temp) dt_free_align(temp);
  if(LF_even) dt_free_align(LF_even);
  if(LF_odd) dt_free_align(LF_odd);
  if(HF_RGB) dt_free_align(HF_RGB);
  if(HF_grey) dt_free_align(HF_grey);
  return success;
}

static inline void filmic_split_v1(const float *const restrict in, float *const restrict out,
                                   const dt_iop_order_iccprofile_info_t *const work_profile,
                                   const dt_iop_filmicrgb_data_t *const data, const dt_iop_filmic_rgb_spline_t spline,
                                   const size_t width, const size_t height, const size_t ch)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(width, height, ch, data, in, out, work_profile, spline) \
  schedule(simd:static) aligned(in, out:64)
#endif
  for(size_t k = 0; k < height * width * ch; k += ch)
  {
    const float *const restrict pix_in = in + k;
    float *const restrict pix_out = out + k;
    float DT_ALIGNED_PIXEL temp[4];

    // Log tone-mapping
    for(int c = 0; c < 3; c++)
      temp[c] = log_tonemapping_v1((pix_in[c] < 1.52587890625e-05f) ? 1.52587890625e-05f : pix_in[c],
                                   data->grey_source, data->black_source, data->dynamic_range);

    // Get the desaturation coeff based on the log value
    const float lum = (work_profile) ? dt_ioppr_get_rgb_matrix_luminance(temp,
                                                                         work_profile->matrix_in,
                                                                         work_profile->lut_in,
                                                                         work_profile->unbounded_coeffs_in,
                                                                         work_profile->lutsize,
                                                                         work_profile->nonlinearlut)
                                      : dt_camera_rgb_luminance(temp);
    const float desaturation = filmic_desaturate_v1(lum, data->sigma_toe, data->sigma_shoulder, data->saturation);

    // Desaturate on the non-linear parts of the curve
    // Filmic S curve on the max RGB
    // Apply the transfer function of the display
    for(int c = 0; c < 3; c++)
      pix_out[c] = powf(clamp_simd(filmic_spline(linear_saturation(temp[c], lum, desaturation), spline.M1, spline.M2, spline.M3, spline.M4, spline.M5, spline.latitude_min, spline.latitude_max)), data->output_power);
  }
}


static inline void filmic_split_v2(const float *const restrict in, float *const restrict out,
                                   const dt_iop_order_iccprofile_info_t *const work_profile,
                                   const dt_iop_filmicrgb_data_t *const data, const dt_iop_filmic_rgb_spline_t spline,
                                   const size_t width, const size_t height, const size_t ch)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(width, height, ch, data, in, out, work_profile, spline) \
  schedule(simd:static) aligned(in, out:64)
#endif
  for(size_t k = 0; k < height * width * ch; k += ch)
  {
    const float *const restrict pix_in = in + k;
    float *const restrict pix_out = out + k;
    float DT_ALIGNED_PIXEL temp[4];

    // Log tone-mapping
    for(int c = 0; c < 3; c++)
      temp[c] = log_tonemapping_v2((pix_in[c] < 1.52587890625e-05f) ? 1.52587890625e-05f : pix_in[c],
                                    data->grey_source, data->black_source, data->dynamic_range);

    // Get the desaturation coeff based on the log value
    const float lum = (work_profile) ? dt_ioppr_get_rgb_matrix_luminance(temp,
                                                                         work_profile->matrix_in,
                                                                         work_profile->lut_in,
                                                                         work_profile->unbounded_coeffs_in,
                                                                         work_profile->lutsize,
                                                                         work_profile->nonlinearlut)
                                      : dt_camera_rgb_luminance(temp);
    const float desaturation = filmic_desaturate_v2(lum, data->sigma_toe, data->sigma_shoulder, data->saturation);

    // Desaturate on the non-linear parts of the curve
    // Filmic S curve on the max RGB
    // Apply the transfer function of the display
    for(int c = 0; c < 3; c++)
      pix_out[c] = powf(clamp_simd(filmic_spline(linear_saturation(temp[c], lum, desaturation), spline.M1, spline.M2, spline.M3, spline.M4, spline.M5, spline.latitude_min, spline.latitude_max)), data->output_power);
  }
}


static inline void filmic_chroma_v1(const float *const restrict in, float *const restrict out,
                                    const dt_iop_order_iccprofile_info_t *const work_profile,
                                    const dt_iop_filmicrgb_data_t *const data, const dt_iop_filmic_rgb_spline_t spline,
                                    const int variant,
                                    const size_t width, const size_t height, const size_t ch)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(width, height, ch, data, in, out, work_profile, variant, spline) \
  schedule(simd:static) aligned(in, out:64)
#endif
  for(size_t k = 0; k < height * width * ch; k += ch)
  {
    const float *const restrict pix_in = in + k;
    float *const restrict pix_out = out + k;

    float DT_ALIGNED_PIXEL ratios[4];
    float norm = get_pixel_norm(pix_in, variant, work_profile);

    norm = (norm < 1.52587890625e-05f) ? 1.52587890625e-05f : norm; // norm can't be < to 2^(-16)

    // Save the ratios
    for(int c = 0; c < 3; c++) ratios[c] = pix_in[c] / norm;

    // Sanitize the ratios
    const float min_ratios = fminf(fminf(ratios[0], ratios[1]), ratios[2]);
    if(min_ratios < 0.0f) for(int c = 0; c < 3; c++) ratios[c] -= min_ratios;

    // Log tone-mapping
    norm = log_tonemapping_v1(norm, data->grey_source, data->black_source, data->dynamic_range);

    // Get the desaturation value based on the log value
    const float desaturation = filmic_desaturate_v1(norm, data->sigma_toe, data->sigma_shoulder, data->saturation);

    for(int c = 0; c < 3; c++) ratios[c] *= norm;

    const float lum = (work_profile) ? dt_ioppr_get_rgb_matrix_luminance(ratios,
                                                                         work_profile->matrix_in,
                                                                         work_profile->lut_in,
                                                                         work_profile->unbounded_coeffs_in,
                                                                         work_profile->lutsize,
                                                                         work_profile->nonlinearlut)
                                      : dt_camera_rgb_luminance(ratios);

    // Desaturate on the non-linear parts of the curve and save ratios
    for(int c = 0; c < 3; c++) ratios[c] = linear_saturation(ratios[c], lum, desaturation) / norm;

    // Filmic S curve on the max RGB
    // Apply the transfer function of the display
    norm = powf(clamp_simd(filmic_spline(norm, spline.M1, spline.M2, spline.M3, spline.M4, spline.M5, spline.latitude_min, spline.latitude_max)), data->output_power);

    // Re-apply ratios
    for(int c = 0; c < 3; c++) pix_out[c] = ratios[c] * norm;
  }
}


static inline void filmic_chroma_v2(const float *const restrict in, float *const restrict out,
                                    const dt_iop_order_iccprofile_info_t *const work_profile,
                                    const dt_iop_filmicrgb_data_t *const data, const dt_iop_filmic_rgb_spline_t spline,
                                    const int variant,
                                    const size_t width, const size_t height, const size_t ch)
{

  #ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(width, height, ch, data, in, out, work_profile, variant, spline) \
    schedule(simd:static) aligned(in, out:64)
  #endif
  for(size_t k = 0; k < height * width * ch; k += ch)
  {
    const float *const restrict pix_in = in + k;
    float *const restrict pix_out = out + k;

    float norm = get_pixel_norm(pix_in, variant, work_profile);
    norm = (norm < 1.52587890625e-05f) ? 1.52587890625e-05f : norm; // norm can't be < to 2^(-16)

    // Save the ratios
    float DT_ALIGNED_PIXEL ratios[4];
    for(int c = 0; c < 3; c++) ratios[c] = pix_in[c] / norm;

    // Sanitize the ratios
    const float min_ratios = fminf(fminf(ratios[0], ratios[1]), ratios[2]);
    const int sanitize = (min_ratios < 0.0f);

    if(sanitize)
      for(int c = 0; c < 3; c++)
        ratios[c] -= min_ratios;

    // Log tone-mapping
    norm = log_tonemapping_v2(norm, data->grey_source, data->black_source, data->dynamic_range);

    // Get the desaturation value based on the log value
    const float desaturation = filmic_desaturate_v2(norm, data->sigma_toe, data->sigma_shoulder, data->saturation);

    // Filmic S curve on the max RGB
    // Apply the transfer function of the display
    norm = powf(clamp_simd(filmic_spline(norm, spline.M1, spline.M2, spline.M3, spline.M4, spline.M5, spline.latitude_min, spline.latitude_max)), data->output_power);

    // Re-apply ratios with saturation change
    for(int c = 0; c < 3; c++)
    {
      ratios[c] = fmaxf(ratios[c] + (1.0f - ratios[c]) * (1.0f - desaturation), 0.0f);
      pix_out[c] = ratios[c] * norm;
    }

    // Gamut mapping
    const float max_pix = fmaxf(fmaxf(pix_out[0], pix_out[1]), pix_out[2]);
    const int penalize = (max_pix > 1.0f);

    // Penalize the ratios by the amount of clipping
    if(penalize)
    {
      for(int c = 0; c < 3; c++)
      {
        ratios[c] = fmaxf(ratios[c] + (1.0f - max_pix), 0.0f);
        pix_out[c] = clamp_simd(ratios[c] * norm);
      }
    }
  }
}

static inline void display_mask(const float *const restrict mask, float *const restrict out,
                                const size_t width, const size_t height, const size_t ch)
{
  #ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(width, height, ch, out, mask) \
    schedule(simd:static) aligned(mask, out:64)
  #endif
  for(size_t k = 0; k < height * width * ch; k++)
    out[k] = mask[k / ch];
}


static inline void compute_ratios(const float *const restrict in, float *const restrict norms, float *const restrict ratios,
                                  const dt_iop_order_iccprofile_info_t *const work_profile, const int variant,
                                  const size_t width, const size_t height, const size_t ch)
{
  #ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(ch, width, height, norms, ratios, in, work_profile, variant) \
    schedule(simd:static) aligned(norms, ratios, in:64)
  #endif
  for(size_t k = 0; k < height * width * ch; k += ch)
  {
    float norm = get_pixel_norm(in + k, variant, work_profile);
    norm = (norm < 1.52587890625e-05f) ? 1.52587890625e-05f : norm; // norm can't be < to 2^(-16)
    norms[k / ch] = norm;

    for(size_t c = 0; c < 3; c++) ratios[k + c] = in[k + c] / norm;
  }
}

static inline void restore_ratios(float *const restrict ratios, const float *const restrict norms,
                                  const size_t width, const size_t height, const size_t ch)
{
  #ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(width, height, ch, norms, ratios) \
    schedule(simd:static) aligned(norms, ratios:64)
  #endif
  for(size_t k = 0; k < height * width * ch; k += ch)
  {
    for(size_t c = 0; c < 3; c++) ratios[k + c] *= norms[k/ch];
  }
}


void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const restrict ivoid, void *const restrict ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_filmicrgb_data_t *const data = (dt_iop_filmicrgb_data_t *)piece->data;
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);

  if(piece->colors != 4)
  {
    dt_control_log(_("filmic works only on RGB input"));
    return;
  }

  const size_t ch = 4;

  /** The log2(x) -> -INF when x -> 0
  * thus very low values (noise) will get even lower, resulting in noise negative amplification,
  * which leads to pepper noise in shadows. To avoid that, we need to clip values that are noise for sure.
  * Using 16 bits RAW data, the black value (known by rawspeed for every manufacturer) could be used as a threshold.
  * However, at this point of the pixelpipe, the RAW levels have already been corrected and everything can happen with black levels
  * in the exposure module. So we define the threshold as the first non-null 16 bit integer
  */

  float *restrict in = (float *)ivoid;
  float *const restrict out = (float *)ovoid;

  const int variant = data->preserve_color;
  const dt_iop_filmic_rgb_spline_t spline = (dt_iop_filmic_rgb_spline_t)data->spline;

  float *const restrict mask =  dt_alloc_sse_ps(roi_out->width * roi_out->height);

  // build a mask of clipped pixels
  const float normalize = data->reconstruct_feather / data->reconstruct_threshold;
  const int recover_highlights = mask_clipped_pixels(in, mask, normalize, data->reconstruct_feather, roi_out->width, roi_out->height, 4);

  // display mask and exit
  if(self->dev->gui_attached && piece->pipe->type == DT_DEV_PIXELPIPE_FULL && mask)
  {
    dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

    if(g->show_mask)
    {
      display_mask(mask, out, roi_out->width, roi_out->height, 4);
      dt_free_align(mask);
      return;
    }
  }

  float *const restrict reconstructed = dt_alloc_sse_ps(roi_out->width * roi_out->height * ch);

  if(recover_highlights && mask && reconstructed)
  {
    const gint success_1 = reconstruct_highlights(in, mask, reconstructed, 0, data, piece, roi_in, roi_out);

    if(data->high_quality_reconstruction && success_1)
    {
      float *const restrict norms = dt_alloc_sse_ps(roi_out->width * roi_out->height);
      float *const restrict ratios = dt_alloc_sse_ps(roi_out->width * roi_out->height * ch);

      // reconstruct highlights PASS 2 on ratios
      if(norms && ratios)
      {
        compute_ratios(reconstructed, norms, ratios, work_profile, variant, roi_out->width, roi_out->height, 4);
        const gint success_2 = reconstruct_highlights(ratios, mask, reconstructed, 1, data, piece, roi_in, roi_out);
        if(success_2) restore_ratios(reconstructed, norms, roi_out->width, roi_out->height, 4);
      }

      if(norms) dt_free_align(norms);
      if(ratios) dt_free_align(ratios);

    }

    if(success_1) in = reconstructed; // use reconstructed buffer as tonemapping input
  }

  if(mask) dt_free_align(mask);

  if(variant == DT_FILMIC_METHOD_NONE)
  {
    // no chroma preservation
    if(data->version == DT_FILMIC_COLORSCIENCE_V1)
      filmic_split_v1(in, out, work_profile, data, spline, roi_out->width, roi_in->height, ch);
    else if(data->version == DT_FILMIC_COLORSCIENCE_V2)
      filmic_split_v2(in, out, work_profile, data, spline, roi_out->width, roi_in->height, ch);
  }
  else
  {
    // chroma preservation
    if(data->version == DT_FILMIC_COLORSCIENCE_V1)
      filmic_chroma_v1(in, out, work_profile, data, spline, variant, roi_out->width, roi_out->height, ch);
    else if(data->version == DT_FILMIC_COLORSCIENCE_V2)
      filmic_chroma_v2(in, out, work_profile, data, spline, variant, roi_out->width, roi_out->height, ch);
  }

  if(reconstructed) dt_free_align(reconstructed);

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
    dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_filmicrgb_data_t *const d = (dt_iop_filmicrgb_data_t *)piece->data;
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);
  dt_iop_filmicrgb_global_data_t *const gd = (dt_iop_filmicrgb_global_data_t *)self->global_data;
  const dt_iop_filmic_rgb_spline_t spline = (dt_iop_filmic_rgb_spline_t)d->spline;

  cl_int err = -999;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;
  const int use_work_profile = (work_profile == NULL) ? 0 : 1;

  cl_mem dev_profile_info = NULL;
  cl_mem dev_profile_lut = NULL;
  dt_colorspaces_iccprofile_info_cl_t *profile_info_cl;
  cl_float *profile_lut_cl = NULL;

  err = dt_ioppr_build_iccprofile_params_cl(work_profile, devid, &profile_info_cl, &profile_lut_cl,
                                            &dev_profile_info, &dev_profile_lut);
  if(err != CL_SUCCESS) goto error;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };

  if(d->preserve_color == DT_FILMIC_METHOD_NONE)
  {
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 4, sizeof(float), (void *)&d->dynamic_range);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 5, sizeof(float), (void *)&d->black_source);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 6, sizeof(float), (void *)&d->grey_source);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 7, sizeof(cl_mem), (void *)&dev_profile_info);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 8, sizeof(cl_mem), (void *)&dev_profile_lut);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 9, sizeof(int), (void *)&use_work_profile);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 10, sizeof(float), (void *)&d->sigma_toe);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 11, sizeof(float), (void *)&d->sigma_shoulder);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 12, sizeof(float), (void *)&d->saturation);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 13, 4 * sizeof(float), (void *)&spline.M1);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 14, 4 * sizeof(float), (void *)&spline.M2);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 15, 4 * sizeof(float), (void *)&spline.M3);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 16, 4 * sizeof(float), (void *)&spline.M4);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 17, 4 * sizeof(float), (void *)&spline.M5);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 18, sizeof(float), (void *)&spline.latitude_min);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 19, sizeof(float), (void *)&spline.latitude_max);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 20, sizeof(float), (void *)&d->output_power);

    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic_rgb_split, sizes);
    dt_ioppr_free_iccprofile_params_cl(&profile_info_cl, &profile_lut_cl, &dev_profile_info, &dev_profile_lut);
    if(err != CL_SUCCESS) goto error;
    return TRUE;
  }
  else
  {
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 4, sizeof(float), (void *)&d->dynamic_range);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 5, sizeof(float), (void *)&d->black_source);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 6, sizeof(float), (void *)&d->grey_source);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 7, sizeof(cl_mem), (void *)&dev_profile_info);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 8, sizeof(cl_mem), (void *)&dev_profile_lut);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 9, sizeof(int), (void *)&use_work_profile);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 10, sizeof(float), (void *)&d->sigma_toe);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 11, sizeof(float), (void *)&d->sigma_shoulder);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 12, sizeof(float), (void *)&d->saturation);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 13, 4 * sizeof(float), (void *)&spline.M1);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 14, 4 * sizeof(float), (void *)&spline.M2);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 15, 4 * sizeof(float), (void *)&spline.M3);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 16, 4 * sizeof(float), (void *)&spline.M4);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 17, 4 * sizeof(float), (void *)&spline.M5);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 18, sizeof(float), (void *)&spline.latitude_min);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 19, sizeof(float), (void *)&spline.latitude_max);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 20, sizeof(float), (void *)&d->output_power);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 21, sizeof(int), (void *)&d->preserve_color);

    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic_rgb_chroma, sizes);
    dt_ioppr_free_iccprofile_params_cl(&profile_info_cl, &profile_lut_cl, &dev_profile_info, &dev_profile_lut);
    if(err != CL_SUCCESS) goto error;
    return TRUE;
  }
error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_filmicrgb] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif


static void apply_auto_grey(dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

  const dt_iop_order_iccprofile_info_t *const work_profile
        = dt_ioppr_get_iop_work_profile_info(self, self->dev->iop);
  const float grey = get_pixel_norm(self->picked_color, p->preserve_color, work_profile) / 2.0f;

  const float prev_grey = p->grey_point_source;
  p->grey_point_source = CLAMP(100.f * grey, 0.001f, 100.0f);
  const float grey_var = log2f(prev_grey / p->grey_point_source);
  p->black_point_source = p->black_point_source - grey_var;
  p->white_point_source = p->white_point_source + grey_var;
  p->output_power =  logf(p->grey_point_target / 100.0f) / logf(-p->black_point_source / (p->white_point_source - p->black_point_source));

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  dt_bauhaus_slider_set_soft(g->grey_point_source, p->grey_point_source);
  dt_bauhaus_slider_set_soft(g->black_point_source, p->black_point_source);
  dt_bauhaus_slider_set_soft(g->white_point_source, p->white_point_source);
  dt_bauhaus_slider_set_soft(g->output_power, p->output_power);
  darktable.gui->reset = reset;

  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void apply_auto_black(dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

  // Black
  const dt_iop_order_iccprofile_info_t *const work_profile
        = dt_ioppr_get_iop_work_profile_info(self, self->dev->iop);
  const float black = get_pixel_norm(self->picked_color_min, p->preserve_color, work_profile);

  float EVmin = CLAMP(log2f(black / (p->grey_point_source / 100.0f)), -16.0f, -1.0f);
  EVmin *= (1.0f + p->security_factor / 100.0f);

  p->black_point_source = fmaxf(EVmin, -16.0f);
  p->output_power =  logf(p->grey_point_target / 100.0f) / logf(-p->black_point_source / (p->white_point_source - p->black_point_source));

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  dt_bauhaus_slider_set_soft(g->black_point_source, p->black_point_source);
  dt_bauhaus_slider_set_soft(g->output_power, p->output_power);
  darktable.gui->reset = reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(self->widget);
}


static void apply_auto_white_point_source(dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

  // White
  const dt_iop_order_iccprofile_info_t *const work_profile
        = dt_ioppr_get_iop_work_profile_info(self, self->dev->iop);
  const float white = get_pixel_norm(self->picked_color_max, p->preserve_color, work_profile);

  float EVmax = CLAMP(log2f(white / (p->grey_point_source / 100.0f)), 1.0f, 16.0f);
  EVmax *= (1.0f + p->security_factor / 100.0f);

  p->white_point_source = EVmax;
  p->output_power =  logf(p->grey_point_target / 100.0f) / logf(-p->black_point_source / (p->white_point_source - p->black_point_source));

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  dt_bauhaus_slider_set_soft(g->white_point_source, p->white_point_source);
  dt_bauhaus_slider_set_soft(g->output_power, p->output_power);
  darktable.gui->reset = reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(self->widget);
}

static void apply_autotune(dt_iop_module_t *self)
{
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  const dt_iop_order_iccprofile_info_t *const work_profile
        = dt_ioppr_get_iop_work_profile_info(self, self->dev->iop);

  // Grey
  const float grey = get_pixel_norm(self->picked_color, p->preserve_color, work_profile) / 2.0f;
  p->grey_point_source = CLAMP(100.f * grey, 0.001f, 100.0f);

  // White
  const float white = get_pixel_norm(self->picked_color_max, p->preserve_color, work_profile);
  float EVmax = CLAMP(log2f(white / (p->grey_point_source / 100.0f)), 1.0f, 16.0f);
  EVmax *= (1.0f + p->security_factor / 100.0f);

  // Black
  const float black = get_pixel_norm(self->picked_color_min, p->preserve_color, work_profile);
  float EVmin = CLAMP(log2f(black / (p->grey_point_source / 100.0f)), -16.0f, -1.0f);
  EVmin *= (1.0f + p->security_factor / 100.0f);

  p->black_point_source = fmaxf(EVmin, -16.0f);
  p->white_point_source = EVmax;
  p->output_power =  logf(p->grey_point_target / 100.0f) / logf(-p->black_point_source / (p->white_point_source - p->black_point_source));

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  dt_bauhaus_slider_set_soft(g->grey_point_source, p->grey_point_source);
  dt_bauhaus_slider_set_soft(g->black_point_source, p->black_point_source);
  dt_bauhaus_slider_set_soft(g->white_point_source, p->white_point_source);
  dt_bauhaus_slider_set_soft(g->output_power, p->output_power);
  darktable.gui->reset = reset;

  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static int _iop_color_picker_get_set(dt_iop_module_t *self, GtkWidget *button)
{
  dt_iop_filmicrgb_gui_data_t *g =  (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

  const int current_picker = g->color_picker.current_picker;

  g->color_picker.current_picker = DT_PICKPROFLOG_NONE;

  if(button == g->grey_point_source)
    g->color_picker.current_picker = DT_PICKPROFLOG_GREY_POINT;
  else if(button == g->black_point_source)
    g->color_picker.current_picker = DT_PICKPROFLOG_BLACK_POINT;
  else if(button == g->white_point_source)
    g->color_picker.current_picker = DT_PICKPROFLOG_WHITE_POINT;
  else if(button == g->auto_button)
    g->color_picker.current_picker = DT_PICKPROFLOG_AUTOTUNE;

  if (current_picker == g->color_picker.current_picker)
    return DT_COLOR_PICKER_ALREADY_SELECTED;
  else
    return g->color_picker.current_picker;
}

static void _iop_color_picker_apply(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;
  switch(g->color_picker.current_picker)
  {
     case DT_PICKPROFLOG_GREY_POINT:
       apply_auto_grey(self);
       break;
     case DT_PICKPROFLOG_BLACK_POINT:
       apply_auto_black(self);
       break;
     case DT_PICKPROFLOG_WHITE_POINT:
       apply_auto_white_point_source(self);
       break;
     case DT_PICKPROFLOG_AUTOTUNE:
       apply_autotune(self);
       break;
     default:
       break;
  }
}

static void _iop_color_picker_update(dt_iop_module_t *self)
{
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;
  const int which_colorpicker = g->color_picker.current_picker;
  dt_bauhaus_widget_set_quad_active(g->grey_point_source, which_colorpicker == DT_PICKPROFLOG_GREY_POINT);
  dt_bauhaus_widget_set_quad_active(g->black_point_source, which_colorpicker == DT_PICKPROFLOG_BLACK_POINT);
  dt_bauhaus_widget_set_quad_active(g->white_point_source, which_colorpicker == DT_PICKPROFLOG_WHITE_POINT);
  dt_bauhaus_widget_set_quad_active(g->auto_button, which_colorpicker == DT_PICKPROFLOG_AUTOTUNE);
}


static void grey_point_source_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  float prev_grey = p->grey_point_source;
  p->grey_point_source = dt_bauhaus_slider_get(slider);

  float grey_var = log2f(prev_grey / p->grey_point_source);
  p->black_point_source = p->black_point_source - grey_var;
  p->white_point_source = p->white_point_source + grey_var;

  if(p->auto_hardness)
    p->output_power =  logf(p->grey_point_target / 100.0f) / logf(-p->black_point_source / (p->white_point_source - p->black_point_source));

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  dt_bauhaus_slider_set_soft(g->white_point_source, p->white_point_source);
  dt_bauhaus_slider_set_soft(g->black_point_source, p->black_point_source);
  dt_bauhaus_slider_set_soft(g->output_power, p->output_power);
  darktable.gui->reset = reset;

  dt_iop_color_picker_reset(self, TRUE);

  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void white_point_source_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

  p->white_point_source = dt_bauhaus_slider_get(slider);

  if(p->auto_hardness)
  {
    p->output_power =  logf(p->grey_point_target / 100.0f) / logf(-p->black_point_source / (p->white_point_source - p->black_point_source));

    const int reset = darktable.gui->reset;
    darktable.gui->reset = 1;
    dt_bauhaus_slider_set_soft(g->output_power, p->output_power);
    darktable.gui->reset = reset;
  }

  dt_iop_color_picker_reset(self, TRUE);
  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void black_point_source_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

  p->black_point_source = dt_bauhaus_slider_get(slider);

  if(p->auto_hardness)
  {
    p->output_power =  logf(p->grey_point_target / 100.0f) / logf(-p->black_point_source / (p->white_point_source - p->black_point_source));

    const int reset = darktable.gui->reset;
    darktable.gui->reset = 1;
    dt_bauhaus_slider_set_soft(g->output_power, p->output_power);
    darktable.gui->reset = reset;
  }

  dt_iop_color_picker_reset(self, TRUE);
  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void security_threshold_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

  float previous = p->security_factor;
  p->security_factor = dt_bauhaus_slider_get(slider);
  float ratio = (p->security_factor - previous) / (previous + 100.0f);

  float EVmin = p->black_point_source;
  EVmin = EVmin + ratio * EVmin;

  float EVmax = p->white_point_source;
  EVmax = EVmax + ratio * EVmax;

  p->white_point_source = EVmax;
  p->black_point_source = EVmin;

  if(p->auto_hardness)
    p->output_power =  logf(p->grey_point_target / 100.0f) / logf(-p->black_point_source / (p->white_point_source - p->black_point_source));

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  dt_bauhaus_slider_set_soft(g->white_point_source, p->white_point_source);
  dt_bauhaus_slider_set_soft(g->black_point_source, p->black_point_source);
  dt_bauhaus_slider_set_soft(g->output_power, p->output_power);
  darktable.gui->reset = reset;

  dt_iop_color_picker_reset(self, TRUE);
  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void reconstruct_threshold_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  p->reconstruct_threshold = dt_bauhaus_slider_get(slider);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void reconstruct_feather_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  p->reconstruct_feather = dt_bauhaus_slider_get(slider);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void show_mask_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;
  g->show_mask = !(g->show_mask);
  dt_bauhaus_widget_set_quad_active(g->reconstruct_feather, g->show_mask);
  dt_dev_reprocess_center(self->dev);
}

static void reconstruct_bloom_vs_details_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;
  p->reconstruct_bloom_vs_details = dt_bauhaus_slider_get(slider);

  if(p->reconstruct_bloom_vs_details == -100.f)
  {
    // user disabled the reconstruction in favor of full blooming
    // so the structure vs. texture setting doesn't make any difference
    // make it insensitive to not confuse users
    gtk_widget_set_sensitive(g->reconstruct_structure_vs_texture, FALSE);
  }
  else
  {
    gtk_widget_set_sensitive(g->reconstruct_structure_vs_texture, TRUE);
  }

  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void reconstruct_grey_vs_color_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  p->reconstruct_grey_vs_color = dt_bauhaus_slider_get(slider);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void reconstruct_structure_vs_texture_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  p->reconstruct_structure_vs_texture = dt_bauhaus_slider_get(slider);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void grey_point_target_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  p->grey_point_target = dt_bauhaus_slider_get(slider);
  dt_iop_color_picker_reset(self, TRUE);
  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void latitude_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  p->latitude = dt_bauhaus_slider_get(slider);
  dt_iop_color_picker_reset(self, TRUE);
  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void contrast_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  p->contrast = dt_bauhaus_slider_get(slider);
  dt_iop_color_picker_reset(self, TRUE);
  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void saturation_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  p->saturation = dt_bauhaus_slider_get(slider);
  dt_iop_color_picker_reset(self, TRUE);
  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void white_point_target_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  p->white_point_target = dt_bauhaus_slider_get(slider);
  dt_iop_color_picker_reset(self, TRUE);
  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void black_point_target_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  p->black_point_target = dt_bauhaus_slider_get(slider);
  dt_iop_color_picker_reset(self, TRUE);
  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void output_power_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  p->output_power = dt_bauhaus_slider_get(slider);
  dt_iop_color_picker_reset(self, TRUE);
  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void balance_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  p->balance = dt_bauhaus_slider_get(slider);
  dt_iop_color_picker_reset(self, TRUE);
  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void version_callback(GtkWidget *combo, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  p->version = dt_bauhaus_combobox_get(combo);

  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

  if(p->version == DT_FILMIC_COLORSCIENCE_V1)
    dt_bauhaus_widget_set_label(g->saturation, NULL, _("extreme luminance saturation"));
  else if(p->version == DT_FILMIC_COLORSCIENCE_V2)
    dt_bauhaus_widget_set_label(g->saturation, NULL, _("middle tones saturation"));

  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void preserve_color_callback(GtkWidget *combo, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  p->preserve_color = dt_bauhaus_combobox_get(combo);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void shadows_callback(GtkWidget *combo, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  p->shadows = dt_bauhaus_combobox_get(combo);
  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void highlights_callback(GtkWidget *combo, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  p->highlights = dt_bauhaus_combobox_get(combo);
  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void custom_grey_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

  p->custom_grey = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  gtk_widget_set_visible(g->grey_point_source, p->custom_grey);
  gtk_widget_set_visible(g->grey_point_target, p->custom_grey);
  darktable.gui->reset = reset;

  gtk_widget_queue_draw(self->widget);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void auto_hardness_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  p->auto_hardness = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));

  if(p->auto_hardness)
  {
    dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

    p->output_power =  logf(p->grey_point_target / 100.0f) / logf(-p->black_point_source / (p->white_point_source - p->black_point_source));

    const int reset = darktable.gui->reset;
    darktable.gui->reset = 1;
    dt_bauhaus_slider_set_soft(g->output_power, p->output_power);
    darktable.gui->reset = reset;

    gtk_widget_queue_draw(self->widget);
  }

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void high_quality_reconstruction_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  p->high_quality_reconstruction = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


void gui_focus(struct dt_iop_module_t *self, gboolean in)
{
  if(!in) dt_iop_color_picker_reset(self, TRUE);
}


#define ORDER_4 5
#define ORDER_3 4


inline static void dt_iop_filmic_rgb_compute_spline(const dt_iop_filmicrgb_params_t *const p, struct dt_iop_filmic_rgb_spline_t *const spline)
{
  float grey_display;

  if(p->custom_grey)
  {
    // user set a custom value
    grey_display = powf(CLAMP(p->grey_point_target, p->black_point_target, p->white_point_target) / 100.0f, 1.0f / (p->output_power));
  }
  else
  {
    // use 18.45% grey and don't bother
    grey_display = powf(0.1845f, 1.0f / (p->output_power));
  }

  const float white_source = p->white_point_source;
  const float black_source = p->black_point_source;
  const float dynamic_range = white_source - black_source;

  // luminance after log encoding
  const float black_log = 0.0f; // assumes user set log as in the autotuner
  const float grey_log = fabsf(p->black_point_source) / dynamic_range;
  const float white_log = 1.0f; // assumes user set log as in the autotuner

  // target luminance desired after filmic curve
  const float black_display = CLAMP(p->black_point_target, 0.0f, p->grey_point_target) / 100.0f; // in %
  const float white_display = CLAMP(p->white_point_target, p->grey_point_target, 100.0f)  / 100.0f; // in %

  const float latitude = CLAMP(p->latitude, 0.0f, 100.0f) / 100.0f * dynamic_range; // in % of dynamic range
  const float balance = CLAMP(p->balance, -50.0f, 50.0f) / 100.0f; // in %
  const float contrast = CLAMP(p->contrast, 0.1f, 2.0f);

  // nodes for mapping from log encoding to desired target luminance
  // X coordinates
  float toe_log = grey_log - latitude/dynamic_range * fabsf(black_source/dynamic_range);
  float shoulder_log = grey_log + latitude/dynamic_range * fabsf(white_source/dynamic_range);

  // interception
  float linear_intercept = grey_display - (contrast * grey_log);

  // y coordinates
  float toe_display = (toe_log * contrast + linear_intercept);
  float shoulder_display = (shoulder_log * contrast + linear_intercept);

  // Apply the highlights/shadows balance as a shift along the contrast slope
  const float norm = sqrtf(contrast * contrast + 1.0f);

  // negative values drag to the left and compress the shadows, on the UI negative is the inverse
  const float coeff = -((2.0f * latitude) / dynamic_range) * balance;

  toe_display += coeff * contrast / norm;
  shoulder_display += coeff * contrast / norm;
  toe_log += coeff / norm;
  shoulder_log += coeff / norm;

  /**
   * Now we have 3 segments :
   *  - x = [0.0 ; toe_log], curved part
   *  - x = [toe_log ; grey_log ; shoulder_log], linear part
   *  - x = [shoulder_log ; 1.0] curved part
   *
   * BUT : in case some nodes overlap, we need to remove them to avoid
   * degenerating of the curve
  **/

  // Build the curve from the nodes
  spline->x[0] = black_log;
  spline->x[1] = toe_log;
  spline->x[2] = grey_log;
  spline->x[3] = shoulder_log;
  spline->x[4] = white_log;

  spline->y[0] = black_display;
  spline->y[1] = toe_display;
  spline->y[2] = grey_display;
  spline->y[3] = shoulder_display;
  spline->y[4] = white_display;

  spline->latitude_min = spline->x[1];
  spline->latitude_max = spline->x[3];

  /**
   * For background and details, see :
   * https://eng.aurelienpierre.com/2018/11/30/filmic-darktable-and-the-quest-of-the-hdr-tone-mapping/#filmic_s_curve
   *
   **/
  const double Tl = spline->x[1];
  const double Tl2 = Tl * Tl;
  const double Tl3 = Tl2 * Tl;
  const double Tl4 = Tl3 * Tl;

  const double Sl = spline->x[3];
  const double Sl2 = Sl * Sl;
  const double Sl3 = Sl2 * Sl;
  const double Sl4 = Sl3 * Sl;

  // solve the linear central part - affine function
  spline->M2[2] = contrast;                                    // * x¹ (slope)
  spline->M1[2] = spline->y[1] - spline->M2[2] * spline->x[1]; // * x⁰ (offset)
  spline->M3[2] = 0.f;                                         // * x²
  spline->M4[2] = 0.f;                                         // * x³
  spline->M5[2] = 0.f;                                         // * x⁴

  // solve the toe part
  if(p->shadows == DT_FILMIC_CURVE_POLY_4)
  {
    // fourth order polynom - only mode in darktable 3.0.0
    double A0[ORDER_4 * ORDER_4] = {0.,         0.,       0.,      0., 1.,   // position in 0
                                    0.,         0.,       0.,      1., 0.,   // first derivative in 0
                                    Tl4,        Tl3,      Tl2,     Tl, 1.,   // position at toe node
                                    4. * Tl3,   3. * Tl2, 2. * Tl, 1., 0.,   // first derivative at toe node
                                    12. * Tl2,  6. * Tl,  2.,      0., 0. }; // second derivative at toe node

    double b0[ORDER_4] = { spline->y[0], 0., spline->y[1], spline->M2[2], 0. };

    gauss_solve(A0, b0, ORDER_4);

    spline->M5[0] = b0[0]; // * x⁴
    spline->M4[0] = b0[1]; // * x³
    spline->M3[0] = b0[2]; // * x²
    spline->M2[0] = b0[3]; // * x¹
    spline->M1[0] = b0[4]; // * x⁰
  }
  else
  {
    // third order polynom
    double A0[ORDER_3 * ORDER_3] = {0.,        0.,       0.,     1.,   // position in 0
                                    Tl3,       Tl2,      Tl,     1.,   // position at toe node
                                    3. * Tl2,  2. * Tl,  1.,     0.,   // first derivative at toe node
                                    6. * Tl,   2.,       0.,     0. }; // second derivative at toe node

    double b0[ORDER_3] = { spline->y[0], spline->y[1], spline->M2[2], 0. };

    gauss_solve(A0, b0, ORDER_3);

    spline->M5[0] = 0.0f;  // * x⁴
    spline->M4[0] = b0[0]; // * x³
    spline->M3[0] = b0[1]; // * x²
    spline->M2[0] = b0[2]; // * x¹
    spline->M1[0] = b0[3]; // * x⁰
  }

  // solve the shoulder part
  if(p->highlights == DT_FILMIC_CURVE_POLY_3)
  {
    // 3rd order polynom - only mode in darktable 3.0.0
    double A1[ORDER_3 * ORDER_3] = { 1.,        1.,        1.,      1.,   // position in 1
                                     Sl3,       Sl2,       Sl,      1.,   // position at shoulder node
                                     3. * Sl2,  2. * Sl,   1.,      0.,   // first derivative at shoulder node
                                     6. * Sl,   2.,        0.,      0. }; // second derivative at shoulder node

    double b1[ORDER_3] = { spline->y[4], spline->y[3], spline->M2[2], 0. };

    gauss_solve(A1, b1, ORDER_3);

    spline->M5[1] = 0.0f;  // * x⁴
    spline->M4[1] = b1[0]; // * x³
    spline->M3[1] = b1[1]; // * x²
    spline->M2[1] = b1[2]; // * x¹
    spline->M1[1] = b1[3]; // * x⁰
  }
  else
  {
    // 4th order polynom
    double A1[ORDER_4 * ORDER_4] = { 1.,        1.,        1.,      1.,      1.,   // position in 1
                                     4.,        3.,        2.,      1.,      0.,   // first derivative in 1
                                     Sl4,       Sl3,       Sl2,     Sl,      1.,   // position at shoulder node
                                     4. * Sl3,  3. * Sl2,  2. * Sl, 1.,      0.,   // first derivative at shoulder node
                                     12. * Sl2, 6. * Sl,   2.     , 0.,      0. }; // second derivative at shoulder node

    double b1[ORDER_4] = { spline->y[4], 0., spline->y[3], spline->M2[2], 0. };

    gauss_solve(A1, b1, ORDER_4);

    spline->M5[1] = b1[0]; // * x⁴
    spline->M4[1] = b1[1]; // * x³
    spline->M3[1] = b1[2]; // * x²
    spline->M2[1] = b1[3]; // * x¹
    spline->M1[1] = b1[4]; // * x⁰
  }
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)p1;
  dt_iop_filmicrgb_data_t *d = (dt_iop_filmicrgb_data_t *)piece->data;

  // source and display greys
  float grey_source, grey_display;
  if(p->custom_grey)
  {
    // user set a custom value
    grey_source = p->grey_point_source / 100.0f; // in %
    grey_display = powf(p->grey_point_target / 100.0f, 1.0f / (p->output_power));
  }
  else
  {
    // use 18.45% grey and don't bother
    grey_source = 0.1845f; // in %
    grey_display = powf(0.1845f, 1.0f / (p->output_power));
  }

  // source luminance - Used only in the log encoding
  const float white_source = p->white_point_source;
  const float black_source = p->black_point_source;
  const float dynamic_range = white_source - black_source;

  // luminance after log encoding
  const float grey_log = fabsf(p->black_point_source) / dynamic_range;


  float contrast = p->contrast;
  if (contrast < grey_display / grey_log)
  {
    // We need grey_display - (contrast * grey_log) <= 0.0
    contrast = 1.0001f * grey_display / grey_log;
  }

  // commit
  d->dynamic_range = dynamic_range;
  d->black_source = black_source;
  d->grey_source = grey_source;
  d->output_power = p->output_power;
  d->contrast = contrast;
  d->version = p->version;
  d->preserve_color = p->preserve_color;
  d->high_quality_reconstruction = p->high_quality_reconstruction;

  // TODO: write OpenCL for v2
  if(p->version == DT_FILMIC_COLORSCIENCE_V2)
    piece->process_cl_ready = FALSE;


  // compute the curves and their LUT
  dt_iop_filmic_rgb_compute_spline(p, &d->spline);

  d->saturation = (2.0f * p->saturation / 100.0f + 1.0f);
  d->sigma_toe = powf(d->spline.latitude_min / 3.0f, 2.0f);
  d->sigma_shoulder = powf((1.0f - d->spline.latitude_max) / 3.0f, 2.0f);

  d->reconstruct_threshold = powf(2.0f, white_source + p->reconstruct_threshold) * grey_source;
  d->reconstruct_feather = exp2f(12.f / p->reconstruct_feather);

  // offset and rescale user param to alpha blending so 0 -> 50% and 1 -> 100%
  d->reconstruct_structure_vs_texture = (p->reconstruct_structure_vs_texture / 100.0f + 1.f) / 2.f;
  d->reconstruct_bloom_vs_details = (p->reconstruct_bloom_vs_details / 100.0f + 1.f) / 2.f;
  d->reconstruct_grey_vs_color = (p->reconstruct_grey_vs_color / 100.0f + 1.f) / 2.f;

}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_filmicrgb_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)module->params;

  dt_iop_color_picker_reset(self, TRUE);

  g->show_mask = FALSE;

  self->color_picker_box[0] = self->color_picker_box[1] = .25f;
  self->color_picker_box[2] = self->color_picker_box[3] = .50f;
  self->color_picker_point[0] = self->color_picker_point[1] = 0.5f;

  dt_bauhaus_slider_set_soft(g->white_point_source, p->white_point_source);
  dt_bauhaus_slider_set_soft(g->grey_point_source, p->grey_point_source);
  dt_bauhaus_slider_set_soft(g->black_point_source, p->black_point_source);
  dt_bauhaus_slider_set_soft(g->security_factor, p->security_factor);
  dt_bauhaus_slider_set_soft(g->reconstruct_threshold, p->reconstruct_threshold);
  dt_bauhaus_slider_set_soft(g->reconstruct_feather, p->reconstruct_feather);
  dt_bauhaus_slider_set_soft(g->reconstruct_bloom_vs_details, p->reconstruct_bloom_vs_details);
  dt_bauhaus_slider_set_soft(g->reconstruct_grey_vs_color, p->reconstruct_grey_vs_color);
  dt_bauhaus_slider_set_soft(g->reconstruct_structure_vs_texture, p->reconstruct_structure_vs_texture);
  dt_bauhaus_slider_set_soft(g->white_point_target, p->white_point_target);
  dt_bauhaus_slider_set_soft(g->grey_point_target, p->grey_point_target);
  dt_bauhaus_slider_set_soft(g->black_point_target, p->black_point_target);
  dt_bauhaus_slider_set_soft(g->output_power, p->output_power);
  dt_bauhaus_slider_set_soft(g->latitude, p->latitude);
  dt_bauhaus_slider_set_soft(g->contrast, p->contrast);
  dt_bauhaus_slider_set_soft(g->saturation, p->saturation);
  dt_bauhaus_slider_set_soft(g->balance, p->balance);

  dt_bauhaus_combobox_set(g->version, p->version);
  dt_bauhaus_combobox_set(g->preserve_color, p->preserve_color);
  dt_bauhaus_combobox_set(g->shadows, p->shadows);
  dt_bauhaus_combobox_set(g->highlights, p->highlights);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->auto_hardness), p->auto_hardness);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->custom_grey), p->custom_grey);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->high_quality_reconstruction), p->high_quality_reconstruction);

  gtk_widget_set_visible(g->grey_point_source, p->custom_grey);
  gtk_widget_set_visible(g->grey_point_target, p->custom_grey);

  if(p->reconstruct_bloom_vs_details == -100.f)
  {
    // user disabled the reconstruction in favor of full blooming
    // so the structure vs. texture setting doesn't make any difference
    // make it insensitive to not confuse users
    gtk_widget_set_sensitive(g->reconstruct_structure_vs_texture, FALSE);
  }
  else
  {
    gtk_widget_set_sensitive(g->reconstruct_structure_vs_texture, TRUE);
  }

  if(p->version == DT_FILMIC_COLORSCIENCE_V1)
    dt_bauhaus_widget_set_label(g->saturation, NULL, _("extreme luminance saturation"));
  else if(p->version == DT_FILMIC_COLORSCIENCE_V2)
    dt_bauhaus_widget_set_label(g->saturation, NULL, _("middle tones saturation"));

  gtk_widget_queue_draw(self->widget);

}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_filmicrgb_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_filmicrgb_params_t));
  module->default_enabled = 0;
  module->params_size = sizeof(dt_iop_filmicrgb_params_t);
  module->gui_data = NULL;

  dt_iop_filmicrgb_params_t tmp
    = (dt_iop_filmicrgb_params_t){
                                 .grey_point_source   = 18.45,  // source grey
                                 .black_point_source  = -10.55f,// source black
                                 .white_point_source  = 3.45f,  // source white
                                 .reconstruct_threshold = 0.0f,
                                 .reconstruct_feather   = 3.0f,
                                 .reconstruct_bloom_vs_details = 100.0f,
                                 .reconstruct_grey_vs_color    = 0.0f,
                                 .reconstruct_structure_vs_texture = 50.0f,
                                 .security_factor     = 0.0f,
                                 .grey_point_target   = 18.45f, // target grey
                                 .black_point_target  = 0.0,    // target black
                                 .white_point_target  = 100.0,  // target white
                                 .output_power        = 5.98f,  // target power (~ gamma)
                                 .latitude            = 40.0f,  // intent latitude
                                 .contrast            = 1.30f,  // intent contrast
                                 .saturation          = 0.0f,   // intent saturation
                                 .balance             = 12.0f,  // balance shadows/highlights
                                 .preserve_color      = DT_FILMIC_METHOD_POWER_NORM, // run the saturated variant
                                 .shadows             = DT_FILMIC_CURVE_POLY_4,
                                 .highlights          = DT_FILMIC_CURVE_POLY_4,
                                 .version             = DT_FILMIC_COLORSCIENCE_V2,
                                 .auto_hardness     = TRUE,
                                 .custom_grey         = FALSE,
                                 .high_quality_reconstruction = FALSE
                              };
  memcpy(module->params, &tmp, sizeof(dt_iop_filmicrgb_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_filmicrgb_params_t));
}


void init_global(dt_iop_module_so_t *module)
{
  const int program = 22; // filmic.cl, from programs.conf
  dt_iop_filmicrgb_global_data_t *gd
      = (dt_iop_filmicrgb_global_data_t *)malloc(sizeof(dt_iop_filmicrgb_global_data_t));

  module->data = gd;
  gd->kernel_filmic_rgb_split = dt_opencl_create_kernel(program, "filmicrgb_split");
  gd->kernel_filmic_rgb_chroma = dt_opencl_create_kernel(program, "filmicrgb_chroma");
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
  free(module->default_params);
  module->default_params = NULL;
}


void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_filmicrgb_global_data_t *gd = (dt_iop_filmicrgb_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_filmic_rgb_split);
  dt_opencl_free_kernel(gd->kernel_filmic_rgb_chroma);
  free(module->data);
  module->data = NULL;
}


void gui_reset(dt_iop_module_t *self)
{
  dt_iop_color_picker_reset(self, TRUE);
}

static gboolean dt_iop_tonecurve_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;
  dt_iop_filmic_rgb_compute_spline(p, &g->spline);

  const int inset = DT_GUI_CURVE_EDITOR_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  // clear bg
  cairo_set_source_rgb(cr, .2, .2, .2);
  cairo_paint(cr);

  cairo_translate(cr, inset, inset);
  width -= 2 * inset;
  height -= 2 * inset;

  cairo_set_source_rgb(cr, .3, .3, .3);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  // draw grid
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(.4));
  cairo_set_source_rgb(cr, .1, .1, .1);
  dt_draw_grid(cr, 4, 0, 0, width, height);

  // draw identity line
  cairo_move_to(cr, 0, height);
  cairo_line_to(cr, width, 0);
  cairo_stroke(cr);

  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));
  cairo_set_line_cap  (cr, CAIRO_LINE_CAP_ROUND);

  // Draw the saturation curve
  const float saturation = (2.0f * p->saturation / 100.0f + 1.0f);
  const float sigma_toe = powf(g->spline.latitude_min / 3.0f, 2.0f);
  const float sigma_shoulder = powf((1.0f - g->spline.latitude_max) / 3.0f, 2.0f);

  cairo_set_source_rgb(cr, .5, .5, .5);

  if(p->version == DT_FILMIC_COLORSCIENCE_V1)
  {
    cairo_move_to(cr, 0, height * (1.0 - filmic_desaturate_v1(0.0f, sigma_toe, sigma_shoulder, saturation)));
    for(int k = 1; k < 256; k++)
    {
      const float x = k / 255.0;
      float y = filmic_desaturate_v1(x, sigma_toe, sigma_shoulder, saturation);
      cairo_line_to(cr, x * width, height * (1.0 - y));
    }
  }
  else if(p->version == DT_FILMIC_COLORSCIENCE_V2)
  {
    cairo_move_to(cr, 0, height * (1.0 - filmic_desaturate_v2(0.0f, sigma_toe, sigma_shoulder, saturation)));
    for(int k = 1; k < 256; k++)
    {
      const float x = k / 255.0;
      float y = filmic_desaturate_v2(x, sigma_toe, sigma_shoulder, saturation);
      cairo_line_to(cr, x * width, height * (1.0 - y));
    }
  }
  cairo_stroke(cr);

  // draw the tone curve
  cairo_move_to(cr, 0, height * (1.0 - filmic_spline(0.0f, g->spline.M1, g->spline.M2, g->spline.M3, g->spline.M4, g->spline.M5, g->spline.latitude_min, g->spline.latitude_max)));

  for(int k = 1; k < 256; k++)
  {
    const float x = k / 255.0;
    float y = filmic_spline(x, g->spline.M1, g->spline.M2, g->spline.M3, g->spline.M4, g->spline.M5, g->spline.latitude_min, g->spline.latitude_max);

    if(y > 1.0f)
    {
      y = 1.0f;
      cairo_set_source_rgb(cr, 0.75, .5, 0.);
    }
    else if(y < 0.0f)
    {
      y = 0.0f;
      cairo_set_source_rgb(cr, 0.75, .5, 0.);
    }
    else
    {
      cairo_set_source_rgb(cr, .9, .9, .9);
    }

    cairo_line_to(cr, x * width, height * (1.0 - y));
    cairo_stroke(cr);
    cairo_move_to(cr, x * width, height * (1.0 - y));
  }

  // draw nodes

  // special case for the grey node
  cairo_set_source_rgb(cr, 0.75, 0.5, 0.0);
  cairo_arc(cr, g->spline.x[2] * width, (1.0 - g->spline.y[2]) * height, DT_PIXEL_APPLY_DPI(6), 0, 2. * M_PI);
  cairo_fill(cr);
  cairo_stroke(cr);

  // latitude nodes
  cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
  for(int k = 0; k < 5; k++)
  {
    if(k != 2)
    {
      const float x = g->spline.x[k];
      const float y = g->spline.y[k];
      cairo_arc(cr, x * width, (1.0 - y) * height, DT_PIXEL_APPLY_DPI(4), 0, 2. * M_PI);
      cairo_fill(cr);
      cairo_stroke(cr);
    }
  }

  cairo_stroke(cr);
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}


void gui_init(dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_filmicrgb_gui_data_t));
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;

  g->show_mask = FALSE;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->op));

  // don't make the area square to safe some vertical space -- it's not interactive anyway
  g->area = GTK_DRAWING_AREA(dtgtk_drawing_area_new_with_aspect_ratio(0.618));
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->area), _("read-only graph, use the parameters below to set the nodes\n"
                                                     "the bright curve is the filmic tone mapping curve\n"
                                                     "the dark curve is the desaturation curve\n"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->area), TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->area), "draw", G_CALLBACK(dt_iop_tonecurve_draw), self);

  // Init GTK notebook
  g->notebook = GTK_NOTEBOOK(gtk_notebook_new());
  GtkWidget *page1 = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  GtkWidget *page2 = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  GtkWidget *page3 = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  GtkWidget *page4 = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  GtkWidget *page5 = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));


  gtk_notebook_append_page(GTK_NOTEBOOK(g->notebook), page1, gtk_label_new(_("scene")));
  gtk_notebook_append_page(GTK_NOTEBOOK(g->notebook), page5, gtk_label_new(_("reconstruct")));
  gtk_notebook_append_page(GTK_NOTEBOOK(g->notebook), page2, gtk_label_new(_("look")));
  gtk_notebook_append_page(GTK_NOTEBOOK(g->notebook), page3, gtk_label_new(_("display")));
  gtk_notebook_append_page(GTK_NOTEBOOK(g->notebook), page4, gtk_label_new(_("options")));
  gtk_widget_show_all(GTK_WIDGET(gtk_notebook_get_nth_page(g->notebook, 0)));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->notebook), FALSE, FALSE, 0);

  dtgtk_justify_notebook_tabs(g->notebook);

  // grey_point_source slider
  g->grey_point_source = dt_bauhaus_slider_new_with_range(self, 0.0, 100.0, 0.1, p->grey_point_source, 2);
  dt_bauhaus_slider_set_soft_range(g->grey_point_source, 0.1, 36.0);
  dt_bauhaus_widget_set_label(g->grey_point_source, NULL, _("middle grey luminance"));
  gtk_box_pack_start(GTK_BOX(page1), g->grey_point_source, FALSE, FALSE, 0);
  dt_bauhaus_slider_set_format(g->grey_point_source, "%.2f %%");
  gtk_widget_set_tooltip_text(g->grey_point_source, _("adjust to match the average luminance of the image's subject.\n"
                                                      "the value entered here will then be remapped to 18.45%.\n"
                                                      "decrease the value to increase the overall brightness."));
  g_signal_connect(G_OBJECT(g->grey_point_source), "value-changed", G_CALLBACK(grey_point_source_callback), self);
  dt_bauhaus_widget_set_quad_paint(g->grey_point_source, dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->grey_point_source, TRUE);
  g_signal_connect(G_OBJECT(g->grey_point_source), "quad-pressed", G_CALLBACK(dt_iop_color_picker_callback), &g->color_picker);

  // White slider
  g->white_point_source = dt_bauhaus_slider_new_with_range(self, 0.0, 16.0, 0.1, p->white_point_source, 2);
  dt_bauhaus_slider_set_soft_range(g->white_point_source, 2.0, 8.0);
  dt_bauhaus_widget_set_label(g->white_point_source, NULL, _("white relative exposure"));
  gtk_box_pack_start(GTK_BOX(page1), g->white_point_source, FALSE, FALSE, 0);
  dt_bauhaus_slider_set_format(g->white_point_source, _("%+.2f EV"));
  gtk_widget_set_tooltip_text(g->white_point_source, _("number of stops between middle grey and pure white.\n"
                                                       "this is a reading a lightmeter would give you on the scene.\n"
                                                       "adjust so highlights clipping is avoided"));
  g_signal_connect(G_OBJECT(g->white_point_source), "value-changed", G_CALLBACK(white_point_source_callback), self);
  dt_bauhaus_widget_set_quad_paint(g->white_point_source, dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->white_point_source, TRUE);
  g_signal_connect(G_OBJECT(g->white_point_source), "quad-pressed", G_CALLBACK(dt_iop_color_picker_callback), &g->color_picker);

  // Black slider
  g->black_point_source = dt_bauhaus_slider_new_with_range(self, -16.0, -0.1, 0.1, p->black_point_source, 2);
  dt_bauhaus_slider_set_soft_range(g->black_point_source, -14.0, -3.0);
  dt_bauhaus_widget_set_label(g->black_point_source, NULL, _("black relative exposure"));
  gtk_box_pack_start(GTK_BOX(page1), g->black_point_source, FALSE, FALSE, 0);
  dt_bauhaus_slider_set_format(g->black_point_source, _("%+.2f EV"));
  gtk_widget_set_tooltip_text(g->black_point_source, _("number of stops between middle grey and pure black.\n"
                                                       "this is a reading a lightmeter would give you on the scene.\n"
                                                       "increase to get more contrast.\ndecrease to recover more details in low-lights."));
  g_signal_connect(G_OBJECT(g->black_point_source), "value-changed", G_CALLBACK(black_point_source_callback), self);
  dt_bauhaus_widget_set_quad_paint(g->black_point_source, dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->black_point_source, TRUE);
  g_signal_connect(G_OBJECT(g->black_point_source), "quad-pressed", G_CALLBACK(dt_iop_color_picker_callback), &g->color_picker);

  // Security factor
  g->security_factor = dt_bauhaus_slider_new_with_range(self, -50., 200., 1.0, p->security_factor, 2);
  dt_bauhaus_slider_set_soft_max(g->security_factor, 50.0);
  dt_bauhaus_widget_set_label(g->security_factor, NULL, _("dynamic range scaling"));
  gtk_box_pack_start(GTK_BOX(page1), g->security_factor, FALSE, FALSE, 0);
  dt_bauhaus_slider_set_format(g->security_factor, "%+.2f %%");
  gtk_widget_set_tooltip_text(g->security_factor, _("symmetrically enlarge or shrink the computed dynamic range.\n"
                                                    "useful to give a safety margin to extreme luminances."));
  g_signal_connect(G_OBJECT(g->security_factor), "value-changed", G_CALLBACK(security_threshold_callback), self);

  // Auto tune slider
  g->auto_button = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->auto_button, NULL, _("auto tune levels"));
  dt_bauhaus_widget_set_quad_paint(g->auto_button, dtgtk_cairo_paint_colorpicker,
                                   CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->auto_button, TRUE);
  g_signal_connect(G_OBJECT(g->auto_button), "quad-pressed", G_CALLBACK(dt_iop_color_picker_callback), &g->color_picker);
  gtk_widget_set_tooltip_text(g->auto_button, _("try to optimize the settings with some statistical assumptions.\n"
                                                "this will fit the luminance range inside the histogram bounds.\n"
                                                "works better for landscapes and evenly-lit pictures\n"
                                                "but fails for high-keys, low-keys and high-ISO pictures.\n"
                                                "this is not an artificial intelligence, but a simple guess.\n"
                                                "ensure you understand its assumptions before using it."));
  gtk_box_pack_start(GTK_BOX(page1), g->auto_button, FALSE, FALSE, 0);

  // Reconstruction threshold
  g->reconstruct_threshold = dt_bauhaus_slider_new_with_range(self, -6., 6., 0.1, p->reconstruct_threshold, 2);
  dt_bauhaus_slider_set_format(g->reconstruct_threshold, _("%+.2f EV"));
  dt_bauhaus_widget_set_label(g->reconstruct_threshold, NULL, _("highlights clipping threshold"));
  gtk_box_pack_start(GTK_BOX(page5), g->reconstruct_threshold, FALSE, FALSE, 0);
  gtk_widget_set_tooltip_text(g->reconstruct_threshold, _("set the exposure threshold upon which\n"
                                                          "clipped highlights get reconstructed.\n"
                                                          "values are relative to the scene white point.\n"
                                                          "0 EV means the threshold is the same as the scene white point.\n"
                                                          "decrease to include more areas,\n"
                                                          "increase to exclude more areas."));
  g_signal_connect(G_OBJECT(g->reconstruct_threshold), "value-changed", G_CALLBACK(reconstruct_threshold_callback), self);

  // Reconstruction feather
  g->reconstruct_feather = dt_bauhaus_slider_new_with_range(self, 0.25, 6., 0.1, p->reconstruct_feather, 2);
  dt_bauhaus_slider_set_format(g->reconstruct_feather, _("%+.2f EV"));
  dt_bauhaus_widget_set_label(g->reconstruct_feather, NULL, _("highlights clipping transition"));
  gtk_box_pack_start(GTK_BOX(page5), g->reconstruct_feather, FALSE, FALSE, 0);
  gtk_widget_set_tooltip_text(g->reconstruct_feather, _("soften the transition between clipped highlights and valid pixels.\n"
                                                        "decrease to make the transition harder and sharper,\n"
                                                        "increase to make the transition softer and blurrier."));
  g_signal_connect(G_OBJECT(g->reconstruct_feather), "value-changed", G_CALLBACK(reconstruct_feather_callback), self);
  dt_bauhaus_widget_set_quad_paint(g->reconstruct_feather, dtgtk_cairo_paint_showmask, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->reconstruct_feather, TRUE);
  g_signal_connect(G_OBJECT(g->reconstruct_feather), "quad-pressed", G_CALLBACK(show_mask_callback), self);


  // Reconstruction structure/texture
  g->reconstruct_structure_vs_texture = dt_bauhaus_slider_new_with_range(self, -100., 100., 0.1, p->reconstruct_structure_vs_texture, 2);
  dt_bauhaus_widget_set_label(g->reconstruct_structure_vs_texture, NULL, _("balance structure/texture"));
  dt_bauhaus_slider_set_format(g->reconstruct_structure_vs_texture, "%.2f %%");
  gtk_box_pack_start(GTK_BOX(page5), g->reconstruct_structure_vs_texture, FALSE, FALSE, 0);
  gtk_widget_set_tooltip_text(g->reconstruct_structure_vs_texture, _("decide which reconstruction strategy to favor,\n"
                                                                     "between inpainting a smooth color gradient,\n"
                                                                     "or trying to recover the textured details.\n"
                                                                     "0% is an equal mix of both.\n"
                                                                     "increase if at least one RGB channel is not clipped.\n"
                                                                     "decrease if all RGB channels are clipped over large areas."));
  g_signal_connect(G_OBJECT(g->reconstruct_structure_vs_texture), "value-changed", G_CALLBACK(reconstruct_structure_vs_texture_callback), self);

  // Bloom vs. reconstruct
  g->reconstruct_bloom_vs_details = dt_bauhaus_slider_new_with_range(self, -100., 100., 0.1, p->reconstruct_grey_vs_color, 2);
  dt_bauhaus_widget_set_label(g->reconstruct_bloom_vs_details, NULL, _("balance bloom/reconstruct"));
  dt_bauhaus_slider_set_format(g->reconstruct_bloom_vs_details, "%.2f %%");
  gtk_box_pack_start(GTK_BOX(page5), g->reconstruct_bloom_vs_details, FALSE, FALSE, 0);
  gtk_widget_set_tooltip_text(g->reconstruct_bloom_vs_details, _("decide which reconstruction strategy to favor,\n"
                                                                 "between blooming highlights like film does,\n"
                                                                 "or trying to recover sharp details.\n"
                                                                 "0% is an equal mix of both.\n"
                                                                 "increase if you want more details.\n"
                                                                 "decrease if you want more blur."));
  g_signal_connect(G_OBJECT(g->reconstruct_bloom_vs_details), "value-changed", G_CALLBACK(reconstruct_bloom_vs_details_callback), self);

  // Bloom threshold
  g->reconstruct_grey_vs_color = dt_bauhaus_slider_new_with_range(self, -100., 100., 0.1, p->reconstruct_grey_vs_color, 2);
  dt_bauhaus_widget_set_label(g->reconstruct_grey_vs_color, NULL, _("balance grey/colorful details"));
  dt_bauhaus_slider_set_format(g->reconstruct_grey_vs_color, "%.2f %%");
  gtk_box_pack_start(GTK_BOX(page5), g->reconstruct_grey_vs_color, FALSE, FALSE, 0);
  gtk_widget_set_tooltip_text(g->reconstruct_grey_vs_color, _("decide which reconstruction strategy to favor,\n"
                                                              "between recovering monochromatic highlights,\n"
                                                              "or trying to recover colorful highlights.\n"
                                                              "0% is an equal mix of both.\n"
                                                              "increase if you want more color.\n"
                                                              "decrease if you see magenta or out-of-gamut highlights."));
  g_signal_connect(G_OBJECT(g->reconstruct_grey_vs_color), "value-changed", G_CALLBACK(reconstruct_grey_vs_color_callback), self);


  // contrast slider
  g->contrast = dt_bauhaus_slider_new_with_range(self, 0.0, 5.0, 0.01, p->contrast, 3);
  dt_bauhaus_slider_set_soft_range(g->contrast, 1.0, 2.0);
  dt_bauhaus_widget_set_label(g->contrast, NULL, _("contrast"));
  gtk_box_pack_start(GTK_BOX(page2), g->contrast, FALSE, FALSE, 0);
  gtk_widget_set_tooltip_text(g->contrast, _("slope of the linear part of the curve\n"
                                             "affects mostly the mid-tones"));
  g_signal_connect(G_OBJECT(g->contrast), "value-changed", G_CALLBACK(contrast_callback), self);


  // brightness slider
  g->output_power = dt_bauhaus_slider_new_with_range(self, 1.0, 10.0, 0.1, p->output_power, 2);
  dt_bauhaus_widget_set_label(g->output_power, NULL, _("hardness"));
  gtk_box_pack_start(GTK_BOX(page2), g->output_power, FALSE, FALSE, 0);
  gtk_widget_set_tooltip_text(g->output_power, _("equivalent to paper grade in analog.\n"
                                                 "increase to make highlights brighter and less compressed.\n"
                                                 "decrease to mute highlights."));
  g_signal_connect(G_OBJECT(g->output_power), "value-changed", G_CALLBACK(output_power_callback), self);


  // latitude slider
  g->latitude = dt_bauhaus_slider_new_with_range(self, 0.01, 100.0, 1.0, p->latitude, 2);
  dt_bauhaus_slider_set_soft_range(g->latitude, 5.0, 50.0);
  dt_bauhaus_widget_set_label(g->latitude, NULL, _("latitude"));
  dt_bauhaus_slider_set_format(g->latitude, "%.2f %%");
  gtk_box_pack_start(GTK_BOX(page2), g->latitude, FALSE, FALSE, 0);
  gtk_widget_set_tooltip_text(g->latitude, _("width of the linear domain in the middle of the curve,\n"
                                             "in percent of the dynamic range (white exposure - black exposure).\n"
                                             "increase to get more contrast and less desaturation at extreme luminances,\n"
                                             "decrease otherwise. no desaturation happens in the latitude range.\n"
                                             "this has no effect on mid-tones."));
  g_signal_connect(G_OBJECT(g->latitude), "value-changed", G_CALLBACK(latitude_callback), self);

  // balance slider
  g->balance = dt_bauhaus_slider_new_with_range(self, -50., 50., 1.0, p->balance, 2);
  dt_bauhaus_widget_set_label(g->balance, NULL, _("shadows/highlights balance"));
  gtk_box_pack_start(GTK_BOX(page2), g->balance, FALSE, FALSE, 0);
  dt_bauhaus_slider_set_format(g->balance, "%.2f %%");
  gtk_widget_set_tooltip_text(g->balance, _("slides the latitude along the slope\n"
                                            "to give more room to shadows or highlights.\n"
                                            "use it if you need to protect the details\n"
                                            "at one extremity of the histogram."));
  g_signal_connect(G_OBJECT(g->balance), "value-changed", G_CALLBACK(balance_callback), self);

  // saturation slider
  g->saturation = dt_bauhaus_slider_new_with_range(self, -50., 50., 0.5, p->saturation, 2);

  if(p->version == DT_FILMIC_COLORSCIENCE_V1)
    dt_bauhaus_widget_set_label(g->saturation, NULL, _("extreme luminance saturation"));
  else if(p->version == DT_FILMIC_COLORSCIENCE_V2)
    dt_bauhaus_widget_set_label(g->saturation, NULL, _("middle tones saturation"));

  dt_bauhaus_slider_set_soft_max(g->saturation, 50.0);
  dt_bauhaus_slider_set_format(g->saturation, "%.2f %%");
  gtk_box_pack_start(GTK_BOX(page2), g->saturation, FALSE, FALSE, 0);
  gtk_widget_set_tooltip_text(g->saturation, _("desaturates the output of the module\n"
                                               "specifically at extreme luminances.\n"
                                               "increase if shadows and/or highlights are under-saturated."));
  g_signal_connect(G_OBJECT(g->saturation), "value-changed", G_CALLBACK(saturation_callback), self);


  // Black slider
  g->black_point_target = dt_bauhaus_slider_new_with_range(self, 0.0, 100.0, 1, p->black_point_target, 2);
  dt_bauhaus_widget_set_label(g->black_point_target, NULL, _("target black luminance"));
  gtk_box_pack_start(GTK_BOX(page3), g->black_point_target, FALSE, FALSE, 0);
  dt_bauhaus_slider_set_format(g->black_point_target, "%.2f %%");
  gtk_widget_set_tooltip_text(g->black_point_target, _("luminance of output pure black, "
                                                        "this should be 0%\nexcept if you want a faded look"));
  g_signal_connect(G_OBJECT(g->black_point_target), "value-changed", G_CALLBACK(black_point_target_callback), self);

  // grey_point_source slider
  g->grey_point_target = dt_bauhaus_slider_new_with_range(self, 0.1, 50., 0.5, p->grey_point_target, 2);
  dt_bauhaus_widget_set_label(g->grey_point_target, NULL, _("target middle grey"));
  gtk_box_pack_start(GTK_BOX(page3), g->grey_point_target, FALSE, FALSE, 0);
  dt_bauhaus_slider_set_format(g->grey_point_target, "%.2f %%");
  gtk_widget_set_tooltip_text(g->grey_point_target, _("midde grey value of the target display or color space.\n"
                                                      "you should never touch that unless you know what you are doing."));
  g_signal_connect(G_OBJECT(g->grey_point_target), "value-changed", G_CALLBACK(grey_point_target_callback), self);

  // White slider
  g->white_point_target = dt_bauhaus_slider_new_with_range(self, 0.0, 100.0, 1., p->white_point_target, 2);
  dt_bauhaus_widget_set_label(g->white_point_target, NULL, _("target white luminance"));
  gtk_box_pack_start(GTK_BOX(page3), g->white_point_target, FALSE, FALSE, 0);
  dt_bauhaus_slider_set_format(g->white_point_target, "%.2f %%");
  gtk_widget_set_tooltip_text(g->white_point_target, _("luminance of output pure white, "
                                                        "this should be 100%\nexcept if you want a faded look"));
  g_signal_connect(G_OBJECT(g->white_point_target), "value-changed", G_CALLBACK(white_point_target_callback), self);


  // Color science
  g->version = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->version, NULL, _("color science"));
  dt_bauhaus_combobox_add(g->version, _("v3 (2019)"));
  dt_bauhaus_combobox_add(g->version, _("v4 (2020)"));
  gtk_widget_set_tooltip_text(g->version, _("v3 is darktable 3.0 desaturation method, same as color balance.\n"
                                            "v4 is a newer desaturation method, based on spectral purity of light."));
  gtk_box_pack_start(GTK_BOX(page4), g->version, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->version), "value-changed", G_CALLBACK(version_callback), self);

  // Preserve color
  g->preserve_color = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->preserve_color, NULL, _("preserve chrominance"));
  dt_bauhaus_combobox_add(g->preserve_color, _("no"));
  dt_bauhaus_combobox_add(g->preserve_color, _("max RGB"));
  dt_bauhaus_combobox_add(g->preserve_color, _("luminance Y"));
  dt_bauhaus_combobox_add(g->preserve_color, _("RGB power norm"));
  gtk_widget_set_tooltip_text(g->preserve_color, _("ensure the original color are preserved.\n"
                                                   "may reinforce chromatic aberrations and chroma noise,\n"
                                                   "so ensure they are properly corrected elsewhere.\n"));
  gtk_box_pack_start(GTK_BOX(page4), g->preserve_color , FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->preserve_color), "value-changed", G_CALLBACK(preserve_color_callback), self);


  // Curve type
  g->highlights = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->highlights, NULL, _("contrast in highlights"));
  dt_bauhaus_combobox_add(g->highlights, _("hard"));
  dt_bauhaus_combobox_add(g->highlights, _("soft"));
  gtk_widget_set_tooltip_text(g->highlights, _("choose the desired curvature of the filmic spline in highlights.\n"
                                               "hard uses a high curvature resulting in more tonal compression.\n"
                                               "soft uses a low curvature resulting in less tonal compression."));
  gtk_box_pack_start(GTK_BOX(page4), g->highlights, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->highlights), "value-changed", G_CALLBACK(highlights_callback), self);

  g->shadows = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->shadows, NULL, _("contrast in shadows"));
  dt_bauhaus_combobox_add(g->shadows, _("hard"));
  dt_bauhaus_combobox_add(g->shadows, _("soft"));
  gtk_widget_set_tooltip_text(g->shadows, _("choose the desired curvature of the filmic spline in shadows.\n"
                                            "hard uses a high curvature resulting in more tonal compression.\n"
                                            "soft uses a low curvature resulting in less tonal compression."));
  gtk_box_pack_start(GTK_BOX(page4), g->shadows , FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->shadows), "value-changed", G_CALLBACK(shadows_callback), self);


  // Custom grey
  g->custom_grey = gtk_check_button_new_with_label(_("use custom middle-grey values"));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON(g->custom_grey), p->custom_grey);
  gtk_widget_set_tooltip_text(g->custom_grey, _("enable to input custom middle-grey values\n."
                                                "this is not recommended in general.\n"
                                                "fix the global exposure in the exposure module instead.\n"
                                                "disable to use standard 18.45 %% middle grey."));
  gtk_box_pack_start(GTK_BOX(page4), g->custom_grey, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->custom_grey), "toggled", G_CALLBACK(custom_grey_callback), self);

  // Auto-hardness
  g->auto_hardness = gtk_check_button_new_with_label(_("auto adjust hardness"));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON(g->auto_hardness), p->auto_hardness);
  gtk_widget_set_tooltip_text(g->auto_hardness, _("enable to auto-set the look hardness depending on the scene white and black points.\n"
                                                    "this keeps the middle grey on the identity line and improves fast tuning.\n"
                                                    "disable if you want a manual control."));
  gtk_box_pack_start(GTK_BOX(page4), g->auto_hardness , FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->auto_hardness), "toggled", G_CALLBACK(auto_hardness_callback), self);


  // High quality reconstruction
  g->high_quality_reconstruction = gtk_check_button_new_with_label(_("use high-quality reconstruction"));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON(g->high_quality_reconstruction), p->high_quality_reconstruction);
  gtk_widget_set_tooltip_text(g->high_quality_reconstruction, _("enable to run an extra pass of chromaticity reconstructione\n."
                                                                "this will be slower but will yield more neutral highlights.\n"
                                                                "it also helps with difficult cases of magenta highlights."));
  gtk_box_pack_start(GTK_BOX(page4), g->high_quality_reconstruction , FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->high_quality_reconstruction), "toggled", G_CALLBACK(high_quality_reconstruction_callback), self);

  dt_iop_init_picker(&g->color_picker,
              self,
              DT_COLOR_PICKER_AREA,
              _iop_color_picker_get_set,
              _iop_color_picker_apply,
              _iop_color_picker_update);

  dt_gui_add_help_link(self->widget, dt_get_help_url(self->op));
}


void gui_cleanup(dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
