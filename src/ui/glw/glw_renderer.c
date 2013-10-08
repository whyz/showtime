/*
 *  GLW rendering
 *  Copyright (C) 2010, 2011 Andreas Öman
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "glw.h"
#include "glw_renderer.h"

static const glw_rgb_t white = {.r = 1,.g = 1,.b = 1};


/**
 *
 */
void
glw_vtmp_resize(glw_root_t *gr, int num_float)
{
  if(gr->gr_vtmp_capacity >= num_float)
    return;
  gr->gr_vtmp_capacity = num_float * 10;

  gr->gr_vtmp_buffer = realloc(gr->gr_vtmp_buffer,
			       gr->gr_vtmp_capacity * sizeof(float));
}

/**
 * 
 */
void
glw_renderer_init(glw_renderer_t *gr, int num_vertices, int num_triangles,
		  uint16_t *indices)
{
  int i;

  gr->gr_vertices = calloc(1, sizeof(float) * VERTEX_SIZE * num_vertices);
  gr->gr_num_vertices = num_vertices;

  if((gr->gr_static_indices = (indices != NULL))) {
    gr->gr_indices = indices;
  } else {
    gr->gr_indices = malloc(sizeof(uint16_t) * num_triangles * 3);
  }

  gr->gr_num_triangles = num_triangles;

  for(i = 0; i < num_vertices; i++) {
    gr->gr_vertices[i * VERTEX_SIZE + 3] = 1;

    gr->gr_vertices[i * VERTEX_SIZE + 4] = 1;
    gr->gr_vertices[i * VERTEX_SIZE + 5] = 1;
    gr->gr_vertices[i * VERTEX_SIZE + 6] = 1;
    gr->gr_vertices[i * VERTEX_SIZE + 7] = 1;
  }
  gr->gr_dirty = 1;
  gr->gr_color_attributes = 0;
}


/**
 *
 */
void
glw_renderer_triangle(glw_renderer_t *gr, int element, 
		      uint16_t a, uint16_t b, uint16_t c)
{
  gr->gr_indices[element * 3 + 0] = a;
  gr->gr_indices[element * 3 + 1] = b;
  gr->gr_indices[element * 3 + 2] = c;
  gr->gr_dirty = 1;
}


/**
 *
 */
void
glw_renderer_init_quad(glw_renderer_t *gr)
{
  static uint16_t quad[6] = {
    0, 1, 2,
    0, 2, 3,
  };

  glw_renderer_init(gr, 4, 2, quad);
}


/**
 * 
 */
void
glw_renderer_free(glw_renderer_t *gr)
{
  int i;
  free(gr->gr_vertices);
  gr->gr_vertices = NULL;

  if(!gr->gr_static_indices) {
    free(gr->gr_indices);
    gr->gr_indices = NULL;
  }
  for(i = 0; i < GLW_RENDERER_CACHES; i++) {
    if(gr->gr_cache[i] != NULL) {
      free(gr->gr_cache[i]->grc_vertices);
      free(gr->gr_cache[i]);
      gr->gr_cache[i] = NULL;
    }
  }
}


/**
 *
 */
int
glw_renderer_initialized(glw_renderer_t *gr)
{
  return !!gr->gr_vertices;
}

/**
 * 
 */
void
glw_renderer_vtx_pos(glw_renderer_t *gr, int vertex,
		     float x, float y, float z)
{
  gr->gr_vertices[vertex * VERTEX_SIZE + 0] = x;
  gr->gr_vertices[vertex * VERTEX_SIZE + 1] = y;
  gr->gr_vertices[vertex * VERTEX_SIZE + 2] = z;
  gr->gr_dirty = 1;
}

/**
 * 
 */
void
glw_renderer_vtx_st(glw_renderer_t *gr, int vertex,
		    float s, float t)
{
  gr->gr_vertices[vertex * VERTEX_SIZE + 8] = s;
  gr->gr_vertices[vertex * VERTEX_SIZE + 9] = t;
  gr->gr_dirty = 1;
}


/**
 * 
 */
void
glw_renderer_vtx_st2(glw_renderer_t *gr, int vertex,
		     float s, float t)
{
  gr->gr_vertices[vertex * VERTEX_SIZE + 10] = s;
  gr->gr_vertices[vertex * VERTEX_SIZE + 11] = t;
  gr->gr_dirty = 1;
}


/**
 * 
 */
void
glw_renderer_vtx_col(glw_renderer_t *gr, int vertex,
		     float r, float g, float b, float a)
{
  gr->gr_vertices[vertex * VERTEX_SIZE + 4] = r;
  gr->gr_vertices[vertex * VERTEX_SIZE + 5] = g;
  gr->gr_vertices[vertex * VERTEX_SIZE + 6] = b;
  gr->gr_vertices[vertex * VERTEX_SIZE + 7] = a;
  gr->gr_dirty = 1;
  gr->gr_color_attributes = 1;
}


/**
 * 
 */
void
glw_renderer_vtx_col_reset(glw_renderer_t *gr)
{
  if(!gr->gr_color_attributes)
    return;

  for(int i = 0; i < gr->gr_num_vertices; i++) {
    gr->gr_vertices[i * VERTEX_SIZE + 3] = 1;

    gr->gr_vertices[i * VERTEX_SIZE + 4] = 1;
    gr->gr_vertices[i * VERTEX_SIZE + 5] = 1;
    gr->gr_vertices[i * VERTEX_SIZE + 6] = 1;
    gr->gr_vertices[i * VERTEX_SIZE + 7] = 1;
  }  

  gr->gr_dirty = 1;
  gr->gr_color_attributes = 0;
}


/**
 *
 */
static void
emit_triangle(glw_root_t *gr,
	      const Vec4 V1, const Vec4 V2, const Vec4 V3,
	      const Vec4 C1, const Vec4 C2, const Vec4 C3,
	      const Vec4 T1, const Vec4 T2, const Vec4 T3)
{
  glw_vtmp_resize(gr, (gr->gr_vtmp_cur + 3) * VERTEX_SIZE + 4);

  float *f = gr->gr_vtmp_buffer + gr->gr_vtmp_cur * VERTEX_SIZE;

  glw_vec4_store(f,   V1);
  glw_vec4_store(f+4, C1);
  glw_vec4_store(f+8, T1);

  glw_vec4_store(f+VERTEX_SIZE,   V2);
  glw_vec4_store(f+VERTEX_SIZE+4, C2);
  glw_vec4_store(f+VERTEX_SIZE+8, T2);

  glw_vec4_store(f+VERTEX_SIZE*2,   V3);
  glw_vec4_store(f+VERTEX_SIZE*2+4, C3);
  glw_vec4_store(f+VERTEX_SIZE*2+8, T3);

  gr->gr_vtmp_cur += 3;
}


/**
 * Fade a triangle in eye space
 * This could just as well be done in the vertex shader
 */
static void
fader(glw_root_t *gr, glw_renderer_cache_t *grc,
      const Vec4 V1, const Vec4 V2, const Vec4 V3,
      const Vec4 C1, const Vec4 C2, const Vec4 C3,
      const Vec4 T1, const Vec4 T2, const Vec4 T3,
      int plane)
{
  int i;

  Vec4 c1, c2, c3;
  Vec4 v1, v2, v3;
  
  glw_vec4_copy(c1, C1);
  glw_vec4_copy(c2, C2);
  glw_vec4_copy(c3, C3);
    

  glw_vec4_copy(v1, V1);
  glw_vec4_copy(v2, V2);
  glw_vec4_copy(v3, V3);

  for(i = 0; i < NUM_FADERS; i++) {
    if(!(grc->grc_active_faders & (1 << plane)))
      continue;

    const float D1 = glw_vec34_dot(V1, grc->grc_fader[i]);
    const float D2 = glw_vec34_dot(V2, grc->grc_fader[i]);
    const float D3 = glw_vec34_dot(V3, grc->grc_fader[i]);

    float br = grc->grc_fader_blur[i];
    float ar = grc->grc_fader_alpha[i];


    if(ar > 0) {
      grc->grc_colored = 1;
      glw_vec4_mul_c3(c1, 1 + D1 / ar);
      glw_vec4_mul_c3(c2, 1 + D2 / ar);
      glw_vec4_mul_c3(c3, 1 + D3 / ar);
    }

    if(br > 0) {

      float b1 = 1 + D1 / br;
      float b2 = 1 + D2 / br;
      float b3 = 1 + D3 / br;

      if(b1 < 1 || b2 < 1 || b3 < 1) {
	grc->grc_blurred = 1;
	glw_vec4_mul_c3(v1, b1);
	glw_vec4_mul_c3(v2, b2);
	glw_vec4_mul_c3(v3, b3);
      }
    }
  }
  emit_triangle(gr, v1, v2, v3, c1, c2, c3, T1, T2, T3);
}

#include "misc/sha.h"

//#define docoloring

#ifdef docoloring
static float
random_color(const glw_root_t *gr, int v)
{
  
  sha1_decl(shactx);
  uint8_t d[20];
  int w;

  sha1_init(shactx);
  sha1_update(shactx, (void *)&gr->gr_vtmp_cur, sizeof(int));
  sha1_update(shactx, (void *)&v, sizeof(int));
  sha1_final(shactx, d);

  w = d[0] << 8 | d[1];
  
  return w / 65536.0;
}
#endif

/**
 * Clip a triangle in eye space
 */
static void
clipper(glw_root_t *gr, glw_renderer_cache_t *grc,
	const Vec4 V1, const Vec4 V2, const Vec4 V3,
#ifdef docoloring
	const Vec4 c1, const Vec4 c2, const Vec4 c3,
#else
	const Vec4 C1, const Vec4 C2, const Vec4 C3,
#endif
	const Vec4 T1, const Vec4 T2, const Vec4 T3,
	int plane)
{
#ifdef docoloring
  Vec4 C1, C2, C3;
  
  glw_vec4_copy(C1, glw_vec4_make(
				  random_color(gr, 0),
				  random_color(gr, 1),
				  random_color(gr, 2),
				  1));

  glw_vec4_copy(C2, glw_vec4_make(
				  random_color(gr, 3),
				  random_color(gr, 4),
				  random_color(gr, 5),
				  1));

  glw_vec4_copy(C3, glw_vec4_make(
				  random_color(gr, 6),
				  random_color(gr, 7),
				  random_color(gr, 8),
				  1));
#endif

  while(1) {
    if(plane == NUM_CLIPPLANES) {
      fader(gr, grc, V1, V2, V3, C1, C2, C3, T1, T2, T3, 0);
      return;
    }
    if(grc->grc_active_clippers & (1 << plane))
      break;
    plane++;
  }

  const float D1 = glw_vec34_dot(V1, grc->grc_clip[plane]);
  const float D2 = glw_vec34_dot(V2, grc->grc_clip[plane]);
  const float D3 = glw_vec34_dot(V3, grc->grc_clip[plane]);

  plane++;

  float s12;
  float s13;
  float s23;

  Vec4 V12, V13, V23;
  Vec4 C12, C13, C23;
  Vec4 T12, T13, T23;

  if(D1 >= 0) {
    if(D2 >= 0) {
      if(D3 >= 0) {
	clipper(gr, grc, V1, V2, V3, C1, C2, C3, T1, T2, T3, plane);
      } else {
	s13 = D1 / (D1 - D3);
	s23 = D2 / (D2 - D3);
	
	glw_vec4_lerp(V13, s13, V1, V3);
	glw_vec4_lerp(V23, s23, V2, V3);

	glw_vec4_lerp(C13, s13, C1, C3);
	glw_vec4_lerp(C23, s23, C2, C3);

	glw_vec4_lerp(T13, s13, T1, T3);
	glw_vec4_lerp(T23, s23, T2, T3);
	
	clipper(gr, grc, V1,  V2, V23, C1,  C2, C23, T1, T2, T23, plane);
	clipper(gr, grc, V1, V23, V13, C1, C23, C13, T1, T23, T13, plane);
      }

    } else {
      s12 = D1 / (D1 - D2);
      glw_vec4_lerp(V12, s12, V1, V2);
      glw_vec4_lerp(C12, s12, C1, C2);
      glw_vec4_lerp(T12, s12, T1, T2);

      if(D3 >= 0) {
	s23 = D2 / (D2 - D3);
	glw_vec4_lerp(V23, s23, V2, V3);
	glw_vec4_lerp(C23, s23, C2, C3);
	glw_vec4_lerp(T23, s23, T2, T3);

	clipper(gr, grc, V1, V12, V23, C1, C12, C23, T1, T12, T23, plane);
	clipper(gr, grc, V1, V23, V3,  C1, C23, C3,  T1, T23, T3, plane);

      } else {
	s13 = D1 / (D1 - D3);
	glw_vec4_lerp(V13, s13, V1, V3);
	glw_vec4_lerp(C13, s13, C1, C3);
	glw_vec4_lerp(T13, s13, T1, T3);

	clipper(gr, grc, V1, V12, V13, C1, C12, C13, T1, T12, T13, plane);
      }

    }
  } else {
    if(D2 >= 0) {
      s12 = D1 / (D1 - D2);
      glw_vec4_lerp(V12, s12, V1, V2);
      glw_vec4_lerp(C12, s12, C1, C2);
      glw_vec4_lerp(T12, s12, T1, T2);
      
      if(D3 >= 0) {
	s13 = D1 / (D1 - D3);
	glw_vec4_lerp(V13, s13, V1, V3);
	glw_vec4_lerp(C13, s13, C1, C3);
	glw_vec4_lerp(T13, s13, T1, T3);

	clipper(gr, grc, V12, V2, V3,  C12, C2, C3,  T12, T2, T3, plane);
	clipper(gr, grc, V12, V3, V13, C12, C3, C13, T12, T3, T13, plane);

      } else {
	s23 = D2 / (D2 - D3);
	glw_vec4_lerp(V23, s23, V2, V3);
	glw_vec4_lerp(C23, s23, C2, C3);
	glw_vec4_lerp(T23, s23, T2, T3);

	clipper(gr, grc, V12, V2, V23, C12, C2, C23, T12, T2, T23, plane);

      }
    } else {
      if(D3 >= 0) {
	s13 = D1 / (D1 - D3);
	s23 = D2 / (D2 - D3);
	
	glw_vec4_lerp(V13, s13, V1, V3);
	glw_vec4_lerp(V23, s23, V2, V3);

	glw_vec4_lerp(C13, s13, C1, C3);
	glw_vec4_lerp(C23, s23, C2, C3);

	glw_vec4_lerp(T13, s13, T1, T3);
	glw_vec4_lerp(T23, s23, T2, T3);

	clipper(gr, grc, V13, V23, V3, C13, C23, C3, T13, T23, T3, plane);
      }
    }
  }
}


/**
 * Stenciling
 */
static void
stenciler(glw_root_t *gr, glw_renderer_cache_t *grc,
	  const Vec4 V1, const Vec4 V2, const Vec4 V3,
	  const Vec4 C1, const Vec4 C2, const Vec4 C3,
	  const Vec4 t1, const Vec4 t2, const Vec4 t3,
	  int plane)
{
  float D1, D2, D3;

  if(grc->grc_stencil_width == 0 || plane == 4) {
    clipper(gr, grc, V1, V2, V3, C1, C2, C3, t1, t2, t3, 0);
    return;
  }


  Vec4 T1, T2, T3;
  
  glw_vec4_copy(T1, t1);
  glw_vec4_copy(T2, t2);
  glw_vec4_copy(T3, t3);

#if 0
  if(plane == 0 && 0) {
    D1 = glw_vec34_dot(V1, grc->grc_stencil[0]) * 0.5 + 0.5;
    D2 = glw_vec34_dot(V2, grc->grc_stencil[0]) * 0.5 + 0.5;
    D3 = glw_vec34_dot(V3, grc->grc_stencil[0]) * 0.5 + 0.5;

    glw_vec4_set(T1, 2, D1);
    glw_vec4_set(T2, 2, D2);
    glw_vec4_set(T3, 2, D3);

    D1 = glw_vec34_dot(V1, grc->grc_stencil[1]) * 0.5 + 0.5;
    D2 = glw_vec34_dot(V2, grc->grc_stencil[1]) * 0.5 + 0.5;
    D3 = glw_vec34_dot(V3, grc->grc_stencil[1]) * 0.5 + 0.5;

    glw_vec4_set(T1, 3, D1);
    glw_vec4_set(T2, 3, D2);
    glw_vec4_set(T3, 3, D3);
  }


  printf("T1: %f,%f,%f,%f\n", T1[0], T1[1], T1[2], T1[3]);
  printf("T2: %f,%f,%f,%f\n", T2[0], T2[1], T2[2], T2[3]);
  printf("T3: %f,%f,%f,%f\n", T3[0], T3[1], T3[2], T3[3]);
#endif

  if(plane == 0) {
    glw_vec4_set(T1, 2, 0.5);
    glw_vec4_set(T2, 2, 0.5);
    glw_vec4_set(T3, 2, 0.5);
    glw_vec4_set(T1, 3, 0.5);
    glw_vec4_set(T2, 3, 0.5);
    glw_vec4_set(T3, 3, 0.5);
  }

  Vec4 To1, To2, To3;
  
  glw_vec4_copy(To1, T1);
  glw_vec4_copy(To2, T2);
  glw_vec4_copy(To3, T3);
  float a;

  switch(plane) {
  case 0:
    // Left side
    a = 1 - grc->grc_stencil_edge[0];
    D1 = glw_vec34_dot(V1, grc->grc_stencil[0]) + a;
    D2 = glw_vec34_dot(V2, grc->grc_stencil[0]) + a;
    D3 = glw_vec34_dot(V3, grc->grc_stencil[0]) + a;

    a = 0.5 / grc->grc_stencil_edge[0];

    if(D1 < 0)
      glw_vec4_set(To1, 2, 0.5 + D1 * a);
    if(D2 < 0)
      glw_vec4_set(To2, 2, 0.5 + D2 * a);
    if(D3 < 0)
      glw_vec4_set(To3, 2, 0.5 + D3 * a);

    break;

  case 1:
    // Top
    a = 1 - grc->grc_stencil_edge[1];
    D1 = glw_vec34_dot(V1, grc->grc_stencil[1]) + a;
    D2 = glw_vec34_dot(V2, grc->grc_stencil[1]) + a;
    D3 = glw_vec34_dot(V3, grc->grc_stencil[1]) + a;

    a = 0.5 / grc->grc_stencil_edge[1];

    if(D1 < 0)
      glw_vec4_set(To1, 3, 0.5 + D1 * a);
    if(D2 < 0)
      glw_vec4_set(To2, 3, 0.5 + D2 * a);
    if(D3 < 0)
      glw_vec4_set(To3, 3, 0.5 + D3 * a);

    break;

  case 2:
    // Right
    a = 1 - grc->grc_stencil_edge[2];
    D1 = -glw_vec34_dot(V1, grc->grc_stencil[0]) + a;
    D2 = -glw_vec34_dot(V2, grc->grc_stencil[0]) + a;
    D3 = -glw_vec34_dot(V3, grc->grc_stencil[0]) + a;

    a = 0.5 / grc->grc_stencil_edge[2];

    if(D1 < 0)
      glw_vec4_set(To1, 2, 0.5 - D1 * a);
    if(D2 < 0)
      glw_vec4_set(To2, 2, 0.5 - D2 * a);
    if(D3 < 0)
      glw_vec4_set(To3, 2, 0.5 - D3 * a);
    
    break;

  case 3:
    // Bottom
    a = 1 - grc->grc_stencil_edge[3];
    D1 = -glw_vec34_dot(V1, grc->grc_stencil[1]) + a;
    D2 = -glw_vec34_dot(V2, grc->grc_stencil[1]) + a;
    D3 = -glw_vec34_dot(V3, grc->grc_stencil[1]) + a;

    a = 0.5 / grc->grc_stencil_edge[3];

    if(D1 < 0)
      glw_vec4_set(To1, 3, 0.5 - D1 * a);
    if(D2 < 0)
      glw_vec4_set(To2, 3, 0.5 - D2 * a);
    if(D3 < 0)
      glw_vec4_set(To3, 3, 0.5 - D3 * a);
    
    break;
  default:
    abort();
  }

  plane++;

  float s12;
  float s13;
  float s23;

  Vec4 V12, V13, V23;
  Vec4 C12, C13, C23;
  Vec4 T12, T13, T23;

  if(D1 >= 0) {
    if(D2 >= 0) {
      if(D3 >= 0) {
	// All inside
	stenciler(gr, grc, V1, V2, V3, C1, C2, C3, T1, T2, T3, plane);
	return;
      } else {
	s13 = D1 / (D1 - D3);
	s23 = D2 / (D2 - D3);
	glw_vec4_lerp(V13, s13, V1, V3);
	glw_vec4_lerp(V23, s23, V2, V3);

	glw_vec4_lerp(C13, s13, C1, C3);
	glw_vec4_lerp(C23, s23, C2, C3);

	glw_vec4_lerp(T13, s13, T1, T3);
	glw_vec4_lerp(T23, s23, T2, T3);

	stenciler(gr, grc, V1,  V2, V23, C1,  C2, C23, T1, T2, T23, plane);
	stenciler(gr, grc, V1, V23, V13, C1, C23, C13, T1, T23, T13, plane);
	// Outside
	stenciler(gr, grc, V23, V3, V13, C23, C3, C13, T23, To3, T13, plane);
	return;
      }

    } else {
      s12 = D1 / (D1 - D2);
      glw_vec4_lerp(V12, s12, V1, V2);
      glw_vec4_lerp(C12, s12, C1, C2);
      glw_vec4_lerp(T12, s12, T1, T2);

      if(D3 >= 0) {
	s23 = D2 / (D2 - D3);
	glw_vec4_lerp(V23, s23, V2, V3);
	glw_vec4_lerp(C23, s23, C2, C3);
	glw_vec4_lerp(T23, s23, T2, T3);

	stenciler(gr, grc, V1, V12, V23, C1, C12, C23, T1, T12, T23, plane);
	stenciler(gr, grc, V1, V23, V3,  C1, C23, C3,  T1, T23, T3, plane);
	// outside
	stenciler(gr, grc, V12, V2, V23, C12, C2, C23, T12, To2, T23, plane);

      } else {
	s13 = D1 / (D1 - D3);
	glw_vec4_lerp(V13, s13, V1, V3);
	glw_vec4_lerp(C13, s13, C1, C3);
	glw_vec4_lerp(T13, s13, T1, T3);

	stenciler(gr, grc, V1, V12, V13, C1,  C12, C13, T1,  T12, T13, plane);
	// outside
	stenciler(gr, grc, V12, V2, V3,  C12, C2,  C3,  T12, To2,  To3, plane);
	stenciler(gr, grc, V12, V3, V13, C12, C3,  C13, T12, To3,  T13, plane);

      }
      return;

    }
  } else {
    if(D2 >= 0) {
      s12 = D1 / (D1 - D2);
      glw_vec4_lerp(V12, s12, V1, V2);
      glw_vec4_lerp(C12, s12, C1, C2);
      glw_vec4_lerp(T12, s12, T1, T2);
      
      if(D3 >= 0) {

	s13 = D1 / (D1 - D3);
	glw_vec4_lerp(V13, s13, V1, V3);
	glw_vec4_lerp(C13, s13, C1, C3);
	glw_vec4_lerp(T13, s13, T1, T3);

	stenciler(gr, grc, V12, V2, V3,  C12, C2, C3,  T12, T2, T3, plane);
	stenciler(gr, grc, V12, V3, V13, C12, C3, C13, T12, T3, T13, plane);
	// outside
	stenciler(gr, grc, V1, V12, V13, C1, C12, C13, To1 ,T12, T13, plane);

      } else {
	s23 = D2 / (D2 - D3);
	glw_vec4_lerp(V23, s23, V2, V3);
	glw_vec4_lerp(C23, s23, C2, C3);
	glw_vec4_lerp(T23, s23, T2, T3);

	stenciler(gr, grc, V12, V2, V23, C12, C2, C23, T12, T2, T23, plane);
	// outside
	stenciler(gr, grc, V1, V12, V23, C1, C12, C23, To1, T12, T23, plane);
	stenciler(gr, grc, V1, V23, V3,  C1, C23, C3,  To1, T23, To3, plane);

      }
      return;
    } else {
      if(D3 >= 0) {
	s13 = D1 / (D1 - D3);
	s23 = D2 / (D2 - D3);
	
	glw_vec4_lerp(V13, s13, V1, V3);
	glw_vec4_lerp(V23, s23, V2, V3);

	glw_vec4_lerp(C13, s13, C1, C3);
	glw_vec4_lerp(C23, s23, C2, C3);

	glw_vec4_lerp(T13, s13, T1, T3);
	glw_vec4_lerp(T23, s23, T2, T3);

	stenciler(gr, grc, V13, V23, V3, C13, C23, C3, T13, T23, T3, plane);
	// outside
	stenciler(gr, grc, V1, V2, V23,  C1, C2, C23,  To1, To2, T23, plane);
	stenciler(gr, grc, V1, V23, V13, C1, C23, C13, To1, T23, T13, plane);
	return;
      }
    }
  }
  // outside
  stenciler(gr, grc, V1, V2, V3, C1, C2, C3, To1, To2, To3, plane);
}



/**
 *
 */
static void
glw_renderer_tesselate(glw_renderer_t *gr, glw_root_t *root,
		       const glw_rctx_t *rc, glw_renderer_cache_t *grc)
{
  int i;
  uint16_t *ip = gr->gr_indices;
  const float *a = gr->gr_vertices;
  PMtx pmtx;
  
  root->gr_vtmp_cur = 0;

  memcpy(grc->grc_mtx, rc->rc_mtx, sizeof(Mtx));

  grc->grc_active_clippers  = root->gr_active_clippers;
  grc->grc_active_faders    = root->gr_active_faders;
  grc->grc_blurred = 0;

  for(i = 0; i < NUM_CLIPPLANES; i++)
    if((1 << i) & root->gr_active_clippers)
      memcpy(&grc->grc_clip[i], &root->gr_clip[i], sizeof(Vec4));

  grc->grc_stencil_width = root->gr_stencil_width;
  if(grc->grc_stencil_width) {
    grc->grc_stencil_height = root->gr_stencil_height;

    for(i = 0; i < 2; i++)
      memcpy(&grc->grc_stencil[i], &root->gr_stencil[i], sizeof(Vec4));

    for(i = 0; i < 4; i++)
      grc->grc_stencil_border[i] = root->gr_stencil_border[i];

    for(i = 0; i < 4; i++)
      grc->grc_stencil_edge[i] = root->gr_stencil_edge[i];
  }

  for(i = 0; i < NUM_FADERS; i++)
    if((1 << i) & root->gr_active_faders) {
      memcpy(&grc->grc_fader[i], &root->gr_fader[i], sizeof(Vec4));
      grc->grc_fader_alpha[i] = root->gr_fader_alpha[i];
      grc->grc_fader_blur[i] = root->gr_fader_blur[i];
    }

  glw_pmtx_mul_prepare(pmtx, rc->rc_mtx);

  for(i = 0; i < gr->gr_num_triangles; i++) {
    int v1 = *ip++;
    int v2 = *ip++;
    int v3 = *ip++;

    Vec4 V1, V2, V3;

    glw_pmtx_mul_vec4_i(V1, pmtx, glw_vec4_get(a + v1*VERTEX_SIZE));
    glw_pmtx_mul_vec4_i(V2, pmtx, glw_vec4_get(a + v2*VERTEX_SIZE));
    glw_pmtx_mul_vec4_i(V3, pmtx, glw_vec4_get(a + v3*VERTEX_SIZE));

    stenciler(root, grc,
	      V1, V2, V3,
	      glw_vec4_get(a + v1 * VERTEX_SIZE + 4),
	      glw_vec4_get(a + v2 * VERTEX_SIZE + 4),
	      glw_vec4_get(a + v3 * VERTEX_SIZE + 4),
	      glw_vec4_get(a + v1 * VERTEX_SIZE + 8),
	      glw_vec4_get(a + v2 * VERTEX_SIZE + 8),
	      glw_vec4_get(a + v3 * VERTEX_SIZE + 8),
	      0);
  }

  int size = root->gr_vtmp_cur * sizeof(float) * VERTEX_SIZE;

  if(root->gr_vtmp_cur != grc->grc_num_vertices) {
    grc->grc_num_vertices = root->gr_vtmp_cur;
    grc->grc_vertices = realloc(grc->grc_vertices, size);
  }

  if(size)
    memcpy(grc->grc_vertices, root->gr_vtmp_buffer, size);
}


/**
 *
 */
static int
glw_renderer_clippers_cmp(glw_renderer_cache_t *grc, glw_root_t *root)
{
  int i;

  if(grc->grc_active_clippers != root->gr_active_clippers)
    return 1;

  for(i = 0; i < NUM_CLIPPLANES; i++)
    if((1 << i) & root->gr_active_clippers)
      if(memcmp(&grc->grc_clip[i], &root->gr_clip[i], sizeof(Vec4)))
	return 1;
  return 0;
}



/**
 *
 */
static int
glw_renderer_stencilers_cmp(glw_renderer_cache_t *grc, glw_root_t *root)
{
  int i;

  if(grc->grc_stencil_width  != root->gr_stencil_width ||
     grc->grc_stencil_height != root->gr_stencil_height)
    return 1;

  for(i = 0; i < 2; i++)
    if(memcmp(&grc->grc_stencil[i], &root->gr_stencil[i], sizeof(Vec4)))
      return 1;

  for(i = 0; i < 4; i++) {
    if(grc->grc_stencil_border[i] != root->gr_stencil_border[i])
      return 1;
    if(grc->grc_stencil_edge[i] != root->gr_stencil_edge[i])
      return 1;
  }
  return 0;
}


/**
 *
 */
static int
glw_renderer_faders_cmp(glw_renderer_cache_t *grc, glw_root_t *root)
{
  int i;

  if(grc->grc_active_faders != root->gr_active_faders)
    return 1;

  for(i = 0; i < NUM_FADERS; i++)
    if((1 << i) & root->gr_active_faders)
      if(memcmp(&grc->grc_fader[i], &root->gr_fader[i], sizeof(Vec4)) ||
	 grc->grc_fader_alpha[i] != root->gr_fader_alpha[i] ||
	 grc->grc_fader_blur[i]  != root->gr_fader_blur[i])
	return 1;
  return 0;
}


/**
 *
 */
static glw_renderer_cache_t *
glw_renderer_get_cache(glw_root_t *root, glw_renderer_t *gr)
{
  int idx;
  if((root->gr_frames & 0xff ) != gr->gr_framecmp) {
    gr->gr_cacheptr = 0;
    gr->gr_framecmp = root->gr_frames & 0xff;
  } else {
    gr->gr_cacheptr = (gr->gr_cacheptr + 1) & (GLW_RENDERER_CACHES - 1);
  }
  idx = gr->gr_cacheptr;

  if(gr->gr_cache[idx] == NULL)
    gr->gr_cache[idx] = calloc(1, sizeof(glw_renderer_cache_t));
  return gr->gr_cache[idx];
}


/**
 * This is the entry point of the rendering pipeline
 */
void
glw_renderer_draw(glw_renderer_t *gr, glw_root_t *root,
		  const glw_rctx_t *rc,
		  const struct glw_backend_texture *tex,
		  const struct glw_rgb *rgb_mul,
		  const struct glw_rgb *rgb_off,
		  float alpha, float blur,
		  glw_program_t *p)
{
  rgb_mul = rgb_mul ?: &white;

  int flags = 
    gr->gr_color_attributes ? GLW_RENDER_COLOR_ATTRIBUTES : 0;

  if(root->gr_need_sw_clip || root->gr_active_faders ||
     root->gr_stencil_width) {
    glw_renderer_cache_t *grc = glw_renderer_get_cache(root, gr);

    if(gr->gr_dirty || 
       memcmp(grc->grc_mtx, rc->rc_mtx, sizeof(Mtx)) ||
       glw_renderer_clippers_cmp(grc, root) ||
       glw_renderer_stencilers_cmp(grc, root) ||
       glw_renderer_faders_cmp(grc, root)) {
      glw_renderer_tesselate(gr, root, rc, grc);
    }

    if(grc->grc_blurred)
      flags |= GLW_RENDER_BLUR_ATTRIBUTE;

    if(grc->grc_colored)
      flags |= GLW_RENDER_COLOR_ATTRIBUTES;

    root->gr_render(root, NULL, tex, root->gr_stencil_texture,
		    rgb_mul, rgb_off, alpha, blur,
		    grc->grc_vertices, grc->grc_num_vertices,
		    NULL, 0, flags, p, rc);
  } else {
    root->gr_render(root, rc->rc_mtx, tex, NULL, rgb_mul, rgb_off, alpha, blur,
		    gr->gr_vertices, gr->gr_num_vertices,
		    gr->gr_indices,  gr->gr_num_triangles,
		    flags, p, rc);
  }
  gr->gr_dirty = 0;
}




/**
 *
 */
static const float clip_planes[4][3] = {
  [GLW_CLIP_TOP]    = { 0.0, -1.0, 0.0},
  [GLW_CLIP_BOTTOM] = { 0.0,  1.0, 0.0},
  [GLW_CLIP_LEFT]   = { 1.0,  0.0, 0.0},
  [GLW_CLIP_RIGHT]  = {-1.0,  0.0, 0.0},
};


/**
 *
 */
int
glw_clip_enable(glw_root_t *gr, const glw_rctx_t *rc, glw_clip_boundary_t how,
		float distance)
{
  int i;
  Vec4 v4;
  for(i = 0; i < NUM_CLIPPLANES; i++)
    if(!(gr->gr_active_clippers & (1 << i)))
      break;

  if(i == NUM_CLIPPLANES)
    return -1;

  glw_vec4_copy(v4, glw_vec4_make(clip_planes[how][0],
				  clip_planes[how][1],
				  clip_planes[how][2],
				  1 - (distance * 2)));

  if(gr->gr_set_hw_clipper != NULL) {
    gr->gr_set_hw_clipper(rc, i, v4);

  } else {
    Mtx inv;

    if(!glw_mtx_invert(inv, rc->rc_mtx))
      return -1;

    glw_mtx_trans_mul_vec4(gr->gr_clip[i], inv, v4);
    gr->gr_need_sw_clip = 1;
  }

  gr->gr_active_clippers |= (1 << i);
  return i;
}



/**
 *
 */
void
glw_clip_disable(glw_root_t *gr, int which)
{
  if(which == -1)
    return;

  gr->gr_active_clippers &= ~(1 << which);

  if(gr->gr_clr_hw_clipper != NULL)
    gr->gr_clr_hw_clipper(which);
  else
    gr->gr_need_sw_clip = gr->gr_active_clippers;
}



/**
 *
 */
void
glw_stencil_enable(glw_root_t *gr, const glw_rctx_t *rc,
		   const struct glw_backend_texture *tex,
		   const int16_t *border)
{
  Vec4 v4;
  Mtx inv;

  if(!glw_mtx_invert(inv, rc->rc_mtx))
    return;

  gr->gr_stencil_texture = tex;
#if 0
  gr->gr_stencil_edge[0] = 2.0 * border[0] / rc->rc_width;
  gr->gr_stencil_edge[1] = 2.0 * border[1] / rc->rc_height;
  gr->gr_stencil_edge[2] = 2.0 * border[2] / rc->rc_width;
  gr->gr_stencil_edge[3] = 2.0 * border[3] / rc->rc_height;
#else
  gr->gr_stencil_edge[0] = (float)glw_tex_width(tex)  / rc->rc_width;
  gr->gr_stencil_edge[1] = (float)glw_tex_height(tex) / rc->rc_height;
  gr->gr_stencil_edge[2] = (float)glw_tex_width(tex)  / rc->rc_width;
  gr->gr_stencil_edge[3] = (float)glw_tex_height(tex) / rc->rc_height;
#endif

  memcpy(gr->gr_stencil_border, border, sizeof(int16_t) * 4);
  gr->gr_stencil_width  = glw_tex_width(tex);
  gr->gr_stencil_height = glw_tex_height(tex);

  glw_vec4_copy(v4, glw_vec4_make(1, 0, 0, 0));
  glw_mtx_trans_mul_vec4(gr->gr_stencil[0], inv, v4);
  glw_vec4_copy(v4, glw_vec4_make( 0, -1, 0, 0));
  glw_mtx_trans_mul_vec4(gr->gr_stencil[1], inv, v4);
}



/**
 *
 */
void
glw_stencil_disable(glw_root_t *gr)
{
  gr->gr_stencil_width = 0;
  gr->gr_stencil_texture = NULL;
}


/**
 *
 */
int
glw_fader_enable(glw_root_t *gr, const glw_rctx_t *rc, const float *plane,
		 float a, float b)
{
  int i;
  Vec4 v4;
  for(i = 0; i < NUM_FADERS; i++)
    if(!(gr->gr_active_faders & (1 << i)))
      break;

  if(i == NUM_FADERS)
    return -1;

  glw_vec4_copy(v4, glw_vec4_make(plane[0],
				  plane[1],
				  plane[2],
				  plane[3]));

  Mtx inv;
  
  if(!glw_mtx_invert(inv, rc->rc_mtx))
    return -1;
  
  glw_mtx_trans_mul_vec4(gr->gr_fader[i], inv, v4);
  gr->gr_fader_alpha[i] = a;
  gr->gr_fader_blur[i] = b;

  gr->gr_active_faders |= (1 << i);
  return i;
}


/**
 *
 */
void
glw_fader_disable(glw_root_t *gr, int which)
{
  if(which == -1)
    return;

  gr->gr_active_faders &= ~(1 << which);
}

