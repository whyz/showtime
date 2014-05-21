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
#include "event.h"

/**
 *
 */
typedef struct {
  glw_t w;
  glw_t *prev; // Widget we are transitioning from

  glw_transition_type_t efx_conf;
  float time;
  float delta;
  
  float v;
  char rev;

} glw_deck_t;




/**
 *
 */
static void
clear_constraints(glw_t *w)
{
  glw_set_constraints(w, 0, 0, 0, GLW_CONSTRAINT_X | GLW_CONSTRAINT_Y);
}



/**
 *
 */
static void
glw_deck_update_constraints(glw_t *w)
{
  glw_copy_constraints(w, w->glw_selected);
}


/**
 *
 */
static void
setprev(glw_deck_t *gd, glw_t *c)
{
  glw_t *l = gd->w.glw_selected;
  glw_t *p;
  int rev = 0;

  gd->prev = l;
  if(c == NULL)
    return;

  for(p = TAILQ_NEXT(c, glw_parent_link); p != NULL;
      p = TAILQ_NEXT(p, glw_parent_link)) {
    if(p == l) {
      rev = 1;
      break;
    }
  }
  gd->rev = rev;
}


/**
 *
 */
static int
deck_select_child(glw_t *w, glw_t *c, prop_t *origin)
{
  glw_deck_t *gd = (glw_deck_t *)w;
  if(w->glw_selected == c)
    return 0;

  glw_need_refresh(w->glw_root, 0);

  setprev(gd, c);
  w->glw_selected = c;
  if(w->glw_selected != NULL) {
    glw_focus_open_path_close_all_other(w->glw_selected);
    glw_deck_update_constraints(w);
  } else {
    clear_constraints(w);
  }

  if(gd->efx_conf != GLW_TRANS_NONE &&
     (gd->prev != NULL || !(w->glw_flags2 & GLW2_NO_INITIAL_TRANS)))
    gd->v = 0;

  glw_signal0(w, GLW_SIGNAL_RESELECT_CHANGED, NULL);
  return 1;
}


/**
 *
 */
static void
glw_deck_layout(glw_t *w, const glw_rctx_t *rc)
{
  glw_deck_t *gd = (glw_deck_t *)w;
  glw_t *c;

  gd->delta = 1 / (gd->time * (1000000 / w->glw_root->gr_frameduration));

  if(w->glw_alpha < 0.01)
    return;

  float v = GLW_MIN(gd->v + gd->delta, 1.0);
  if(v != gd->v) {
    gd->v = v;
    glw_need_refresh(w->glw_root, 0);
  }

  if(gd->v == 1)
    gd->prev = NULL;

  TAILQ_FOREACH(c, &w->glw_childs, glw_parent_link) {
    if(c == w->glw_selected || c == gd->prev || 
       c->glw_flags2 & GLW2_ALWAYS_LAYOUT)
      glw_layout0(c, rc);
  }
}


/**
 *
 */
static int
glw_deck_callback(glw_t *w, void *opaque, glw_signal_t signal, void *extra)
{
  glw_deck_t *gd = (glw_deck_t *)w;

  switch(signal) {
  default:
    break;

  case GLW_SIGNAL_CHILD_CONSTRAINTS_CHANGED:
    if(w->glw_selected == extra)
      glw_deck_update_constraints(w);
    return 1;

  case GLW_SIGNAL_CHILD_DESTROYED:
    if(w->glw_selected == extra)
      clear_constraints(w);

    if(gd->prev == extra)
      gd->prev = NULL;

  case GLW_SIGNAL_CHILD_CREATED:
  case GLW_SIGNAL_CHILD_MOVED:
  case GLW_SIGNAL_CHILD_HIDDEN:
  case GLW_SIGNAL_CHILD_UNHIDDEN:
    glw_signal0(w, GLW_SIGNAL_RESELECT_CHANGED, NULL);
    return 0;
  }

  return 0;
}

/**
 *
 */
static int
glw_deck_event(glw_t *w, event_t *e)
{
  glw_t *c = w->glw_selected;
  glw_t *n;

  if(c != NULL && event_is_action(e, ACTION_INCR)) {
    n = glw_get_next_n(c, 1);
  } else if(c != NULL && event_is_action(e, ACTION_DECR)) {
    n = glw_get_prev_n(c, 1);
  } else {
    if(w->glw_selected != NULL) {
      if(glw_send_event(w->glw_selected, e))
	return 1;
    }
    n = NULL;
  }

  if(n != c && n != NULL) {

    if(n->glw_originating_prop) {
      // This will bounce back via .gc_select_child
      prop_select(n->glw_originating_prop);
    } else {
      deck_select_child(w, n, NULL);
    }
    return 1;
  }
  return 0;
}




/**
 *
 */
static void
deck_render(const glw_rctx_t *rc, glw_deck_t *gd, glw_t *w, float v)
{
  if(gd->efx_conf != GLW_TRANS_NONE) {
    glw_rctx_t rc0 = *rc;
    if(gd->rev)
      v = 1 - (v + 1);
    glw_transition_render(gd->efx_conf, v, 
			  rc->rc_alpha * gd->w.glw_alpha, &rc0);
    glw_render0(w, &rc0);
  } else {
    glw_render0(w, rc);
  }
}


/**
 *
 */
static void 
glw_deck_render(glw_t *w, const glw_rctx_t *rc)
{
  glw_deck_t *gd = (glw_deck_t *)w;

  if(w->glw_alpha < 0.01)
    return;

  if(gd->prev != NULL)
    deck_render(rc, gd, gd->prev, gd->v);

  if(w->glw_selected != NULL)
    deck_render(rc, gd, w->glw_selected, -1 + gd->v);
}


/**
 *
 */
static int
set_page(glw_deck_t *gd, int n)
{
  glw_t *c;
  TAILQ_FOREACH(c, &gd->w.glw_childs, glw_parent_link) {
    if(!n--)
      break;
  }
  return deck_select_child(&gd->w, c, NULL);
}

/**
 *
 */
static int
glw_deck_set_page_by_id(glw_t *w, const char *str)
{
  glw_deck_t *gd = (glw_deck_t *)w;
  glw_t *c;
  if(str == NULL)
    return 1;
  TAILQ_FOREACH(c, &gd->w.glw_childs, glw_parent_link)
    if(c->glw_id_rstr != NULL && !strcmp(rstr_get(c->glw_id_rstr), str))
      break;
  return deck_select_child(&gd->w, c, NULL);
}


/**
 *
 */
static int
deck_can_select_child(glw_t *w, int next)
{
  glw_t *c = w->glw_selected;
  return c != NULL && (next ? glw_get_next_n(c, 1) :
		       glw_get_prev_n(c, 1));
}



/**
 *
 */
static void
glw_deck_ctor(glw_t *w)
{
  glw_deck_t *gd = (glw_deck_t *)w;
  gd->v = 1.0;
  clear_constraints(w);
}

/**
 *
 */
static int
glw_deck_set_int(glw_t *w, glw_attribute_t attrib, int value)
{
  glw_deck_t *gd = (glw_deck_t *)w;

  switch(attrib) {
  case GLW_ATTRIB_TRANSITION_EFFECT:
    if(gd->efx_conf == value)
      return 0;
    gd->efx_conf = value;
    break;
  case GLW_ATTRIB_PAGE:
    return set_page(gd, value);
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
glw_deck_set_float(glw_t *w, glw_attribute_t attrib, float value)
{
  glw_deck_t *gd = (glw_deck_t *)w;

  switch(attrib) {
  case GLW_ATTRIB_TIME:
    if(gd->time == value)
      return 0;
    gd->time = value;
    break;
  default:
    return -1;
  }
  return 1;
}


/**
 *
 */
static glw_class_t glw_deck = {
  .gc_name = "deck",
  .gc_instance_size = sizeof(glw_deck_t),
  .gc_flags = GLW_CAN_HIDE_CHILDS,
  .gc_nav_descend_mode = GLW_NAV_DESCEND_SELECTED,
  .gc_layout = glw_deck_layout,
  .gc_render = glw_deck_render,
  .gc_set_int = glw_deck_set_int,
  .gc_set_float = glw_deck_set_float,
  .gc_set_page_id = glw_deck_set_page_by_id,
  .gc_ctor = glw_deck_ctor,
  .gc_signal_handler = glw_deck_callback,
  .gc_select_child = deck_select_child,
  .gc_can_select_child = deck_can_select_child,
  .gc_send_event = glw_deck_event,
};

GLW_REGISTER_CLASS(glw_deck);
