/*
 *  Showtime Mediacenter
 *  Copyright (C) 2007-2013 Lonelycoder AB
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
 *
 *  This program is also available under a commercial proprietary license.
 *  For more information, contact andreas@lonelycoder.com
 */

#include "glw.h"
#include "glw_transitions.h"


typedef struct glw_view_loader {
  glw_t w;
  
  struct prop *prop;
  struct prop *prop_parent;
  struct prop *prop_clone;
  struct prop *prop_parent_override;
  struct prop *prop_self_override;
  struct prop *args;

  float delta;
  float time;

  glw_transition_type_t efx_conf;
  rstr_t *url;
  rstr_t *alt_url;

} glw_view_loader_t;


#define glw_parent_vl_cur glw_parent_val[0].f
#define glw_parent_vl_tgt glw_parent_val[1].f


/**
 *
 */
static void
glw_loader_layout(glw_t *w, const glw_rctx_t *rc)
{
  glw_view_loader_t *a = (void *)w;
  glw_root_t *gr = w->glw_root;
  glw_t *c, *n;

  a->delta = 1 / (a->time * (1000000 / w->glw_root->gr_frameduration));

  for(c = TAILQ_FIRST(&w->glw_childs); c != NULL; c = n) {
    n = TAILQ_NEXT(c, glw_parent_link);

    float n =
      GLW_MIN(c->glw_parent_vl_cur + a->delta, c->glw_parent_vl_tgt);

    if(n != c->glw_parent_vl_cur)
      glw_need_refresh(gr, 0);

    c->glw_parent_vl_cur = n;

    if(c->glw_parent_vl_cur == 1) {
      glw_destroy(c);

      if((c = TAILQ_FIRST(&w->glw_childs)) != NULL) {
        glw_copy_constraints(w, c);
      }
    } else {
      glw_layout0(c, rc);
    }
  }
}


/**
 *
 */
static int
glw_loader_callback(glw_t *w, void *opaque, glw_signal_t signal, void *extra)
{
  glw_t *c, *n;
  glw_view_loader_t *a = (void *)w;

  switch(signal) {
  default:
    break;

  case GLW_SIGNAL_CHILD_CREATED:
    c = extra;

    if(TAILQ_FIRST(&w->glw_childs) == c &&
       TAILQ_NEXT(c, glw_parent_link) == NULL &&
       w->glw_flags2 & GLW2_NO_INITIAL_TRANS) {
      c->glw_parent_vl_cur = 0;
    } else {
      c->glw_parent_vl_cur = -1;
    }

    c->glw_parent_vl_tgt = 0;
    
    glw_focus_open_path_close_all_other(c);

    TAILQ_FOREACH(n, &w->glw_childs, glw_parent_link) {
      if(c == n)
	continue;
      n->glw_parent_vl_tgt = 1;
    }

    if(c == TAILQ_FIRST(&w->glw_childs)) {
      glw_copy_constraints(w, c);
    }

    break;

  case GLW_SIGNAL_CHILD_CONSTRAINTS_CHANGED:
    c = extra;
    if(c == TAILQ_FIRST(&w->glw_childs))
      glw_copy_constraints(w, c);
    return 1;

  case GLW_SIGNAL_DESTROY:
    prop_destroy(a->args);
    prop_ref_dec(a->prop_parent_override);
    prop_ref_dec(a->prop_self_override);
    break;

  }
  return 0;
}


/**
 *
 */
static void
glw_view_loader_render(glw_t *w, const glw_rctx_t *rc)
{
  float alpha = rc->rc_alpha * w->glw_alpha;
  float sharpness  = rc->rc_sharpness  * w->glw_sharpness;
  glw_view_loader_t *a = (glw_view_loader_t *)w;
  glw_t *c;
  glw_rctx_t rc0;

  TAILQ_FOREACH(c, &w->glw_childs, glw_parent_link) {
    
    rc0 = *rc;
    if(c->glw_parent_vl_cur == 0) {
      rc0.rc_alpha = alpha;
      rc0.rc_sharpness = sharpness;
      glw_render0(c, &rc0);
      continue;
    }
    
    glw_transition_render(a->efx_conf, c->glw_parent_vl_cur, alpha, &rc0);
    glw_render0(c, &rc0);
  }
}


/**
 *
 */
static void
glw_view_loader_retire_child(glw_t *w, glw_t *c)
{
  glw_suspend_subscriptions(c);
  c->glw_parent_vl_tgt = 1;
}


/**
 *
 */
static void 
glw_view_loader_ctor(glw_t *w)
{
  glw_view_loader_t *a = (void *)w;
  a->time = 1.0;
  a->args = prop_create_root("args");
  w->glw_flags2 |= GLW2_EXPEDITE_SUBSCRIPTIONS;
}


/**
 *
 */
static void 
glw_view_loader_dtor(glw_t *w)
{
  glw_view_loader_t *a = (void *)w;
  rstr_release(a->url);
  rstr_release(a->alt_url);
}


/**
 *
 */
static void
set_source(glw_t *w, rstr_t *url)
{
  glw_view_loader_t *a = (glw_view_loader_t *)w;
  glw_t *c, *d;
  
  if(w->glw_flags2 & GLW2_DEBUG)
    TRACE(TRACE_DEBUG, "GLW", "Loader loading %s", 
	  rstr_get(url) ?: "(void)");

  if(!strcmp(rstr_get(url) ?: "", rstr_get(a->url) ?: ""))
    return;

  rstr_release(a->url);
  a->url = rstr_dup(url);

  TAILQ_FOREACH(c, &w->glw_childs, glw_parent_link)
    glw_suspend_subscriptions(c);

  if(url && rstr_get(url)[0]) {
    d = glw_view_create(w->glw_root, url, w, 
			a->prop_self_override ?: a->prop, 
			a->prop_parent_override ?: a->prop_parent, a->args, 
			a->prop_clone, 1, 0);
    if(d != NULL)
      return;
  }

  if(a->alt_url != NULL) {
    d = glw_view_create(w->glw_root, a->alt_url, w, 
			a->prop_self_override ?: a->prop, 
			a->prop_parent_override ?: a->prop_parent, a->args, 
			a->prop_clone, 1, 1);
    if(d != NULL)
      return;
  }

  TAILQ_FOREACH(c, &w->glw_childs, glw_parent_link)
    c->glw_parent_vl_tgt = 1;
}


/**
 *
 */
static void
set_alt(glw_t *w, rstr_t *url)
{
  glw_view_loader_t *a = (glw_view_loader_t *)w;
  rstr_set(&a->alt_url, url);
}


/**
 *
 */
static int
glw_view_loader_set_int(glw_t *w, glw_attribute_t attrib, int value)
{
  glw_view_loader_t *vl = (void *)w;

  switch(attrib) {
  case GLW_ATTRIB_TRANSITION_EFFECT:
    if(vl->efx_conf == value)
      return 0;
    vl->efx_conf = value;
    break;

  default:
    return -1;
  }
  return 1;
}


/**
 *
 */
static int
glw_view_loader_set_float(glw_t *w, glw_attribute_t attrib, float value)
{
  glw_view_loader_t *vl = (void *)w;

  switch(attrib) {
  case GLW_ATTRIB_TIME:
    value = GLW_MAX(value, 0.00001);
    if(vl->time == value)
      return 0;
    vl->time = value;
    break;

  default:
    return -1;
  }
  return 1;
}


/**
 *
 */
static void
glw_view_loader_set_roots(glw_t *w, prop_t *self, prop_t *parent, prop_t *clone)
{
  glw_view_loader_t *vl = (void *)w;

  vl->prop        = self;
  vl->prop_parent = parent;
  vl->prop_clone  = clone;
}



/**
 *
 */
static int
glw_view_loader_set_prop(glw_t *w, glw_attribute_t attrib, prop_t *p)
{
  glw_view_loader_t *vl = (void *)w;

  switch(attrib) {
  case GLW_ATTRIB_ARGS:
    prop_link_ex(p, vl->args, NULL, PROP_LINK_XREFED_IF_ORPHANED, 0);
    return 0;

  case GLW_ATTRIB_PROP_PARENT:
    prop_ref_dec(vl->prop_parent_override);
    vl->prop_parent_override = prop_ref_inc(p);
    return 0;

  case GLW_ATTRIB_PROP_SELF:
    prop_ref_dec(vl->prop_self_override);
    vl->prop_self_override = prop_ref_inc(p);
    return 0;

  default:
    return -1;
  }
  return 1;
}


/**
 *
 */
static const char *
get_identity(glw_t *w)
{
  glw_view_loader_t *l = (glw_view_loader_t *)w;
  return rstr_get(l->url) ?: "NULL";
}



/**
 *
 */
static glw_class_t glw_view_loader = {
  .gc_name = "loader",
  .gc_instance_size = sizeof(glw_view_loader_t),
  .gc_ctor = glw_view_loader_ctor,
  .gc_dtor = glw_view_loader_dtor,
  .gc_set_int = glw_view_loader_set_int,
  .gc_set_float = glw_view_loader_set_float,
  .gc_set_prop = glw_view_loader_set_prop,
  .gc_set_roots = glw_view_loader_set_roots,
  .gc_layout = glw_loader_layout,
  .gc_render = glw_view_loader_render,
  .gc_retire_child = glw_view_loader_retire_child,
  .gc_signal_handler = glw_loader_callback,
  .gc_set_source = set_source,
  .gc_get_identity = get_identity,
  .gc_set_alt = set_alt,
};

GLW_REGISTER_CLASS(glw_view_loader);
