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

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>

#include "showtime.h"
#include "settings.h"
#include "event.h"
#include "navigator.h"
#include "backend/backend.h"
#include "backend/backend_prop.h"
#include "prop/prop_nodefilter.h"
#include "prop/prop_concat.h"
#include "htsmsg/htsmsg_store.h"
#include "db/kvstore.h"
#include "misc/minmax.h"

#include <netinet/in.h>
#include <arpa/inet.h>


#define SETTINGS_URL "settings:"
static prop_t *settings_root;
static prop_t *settings_nodes;


/**
 *
 */
struct setting {
  LIST_ENTRY(setting) s_group_link;
  void *s_opaque;
  void *s_callback;
  prop_sub_t *s_sub;
  prop_t *s_root;
  prop_t *s_val;
  prop_t *s_current_origin;

  prop_sub_t *s_inherited_value_sub;
  prop_sub_t *s_inherited_origin_sub;
  struct setting *s_parent;

  settings_saver_t *s_saver;
  void *s_saver_opaque;
  htsmsg_t *s_store;
  prop_t *s_ext_value;

  char *s_id;

  char *s_pending_value;

  char *s_store_name;
  prop_sub_t *s_sub_enabler;

  prop_t *s_current_value;

  rstr_t *s_default_str;

  char s_origin[10];

  atomic_t s_refcount;

  int s_flags;
  char s_type;

  char s_enable_writeback : 1;
  char s_kvstore          : 1;
  char s_on_group_list    : 1;
  char s_value_set        : 1;
};

static void init_dev_settings(void);


/**
 *
 */
static void
set_title2(prop_t *root, prop_t *title)
{
  prop_link(title, prop_create(prop_create(root, "metadata"), "title"));
}


/**
 *
 */
static prop_t *
setting_get(prop_t *parent, int flags)
{
  prop_t *p;


  if(flags & SETTINGS_FIRST) {
    parent = parent ? prop_create(parent, "nodes") : settings_nodes;
    p = prop_create_root(NULL);
    prop_t *before = prop_first_child(parent);
    if(prop_set_parent_ex(p, parent, before, NULL)) {
      prop_destroy(p);
      return NULL;
    }
    prop_ref_dec(before);
  } else if(flags & SETTINGS_RAW_NODES) {
    p = prop_create(parent, NULL);
  } else {
    p = prop_create(parent ? prop_create(parent, "nodes") : settings_nodes,
		    NULL);
  }
  return p;
}

/**
 *
 */
static prop_t *
setting_add(prop_t *parent, prop_t *title, const char *type, int flags)
{
  prop_t *p = setting_get(parent, flags);
  if(title != NULL)
    set_title2(p, title);
  prop_set_string(prop_create(p, "type"), type);
  prop_set_int(prop_create(p, "enabled"), 1);
  return p;
}


/**
 *
 */
static prop_t *
setting_add_cstr(prop_t *parent, const char *title, const char *type, int flags)
{
  prop_t *p = setting_get(parent, flags);
  prop_set_string(prop_create(prop_create(p, "metadata"), "title"), title);
  prop_set_string(prop_create(p, "type"), type);
  return p;
}


/**
 *
 */
static void
settings_add_dir_sup(prop_t *root,
		     const char *url, const char *icon,
		     const char *subtype)
{
  rstr_t *url2 = backend_prop_make(root, url);
  prop_set_rstring(prop_create(root, "url"), url2);
  rstr_release(url2);

  prop_t *metadata = prop_create(root, "metadata");

  prop_set_string(prop_create(root, "subtype"), subtype);

  if(icon != NULL)
    prop_set_string(prop_create(metadata, "icon"), icon);
}



/**
 *
 */
prop_t *
settings_add_dir(prop_t *parent, prop_t *title, const char *subtype,
		 const char *icon, prop_t *shortdesc,
		 const char *url)
{
  prop_t *p = setting_add(parent, title, "settings", 0);
  prop_t *metadata = prop_create(p, "metadata");

  if(shortdesc != NULL)
    prop_link(shortdesc, prop_create(metadata, "shortdesc"));

  settings_add_dir_sup(p, url, icon, subtype);
  return p;
}


/**
 *
 */
prop_t *
settings_add_url(prop_t *parent, prop_t *title,
		 const char *subtype, const char *icon,
		 prop_t *shortdesc, const char *url,
		 int flags)
{
  prop_t *p = setting_add(parent, title, "settings", flags);
  prop_t *metadata = prop_create(p, "metadata");

  if(shortdesc != NULL)
    prop_link(shortdesc, prop_create(metadata, "shortdesc"));

  prop_set_string(prop_create(p, "url"), url);
  prop_set_string(prop_create(p, "subtype"), subtype);

  if(icon != NULL)
    prop_set_string(prop_create(metadata, "icon"), icon);
  return p;
}


/**
 *
 */
prop_t *
settings_add_dir_cstr(prop_t *parent, const char *title, const char *subtype,
		      const char *icon, const char *shortdesc,
		      const char *url)
{
  prop_t *p = setting_add_cstr(parent, title, "settings", 0);
  prop_t *metadata = prop_create(p, "metadata");

  prop_set_string(prop_create(metadata, "shortdesc"), shortdesc);

  settings_add_dir_sup(p, url, icon, subtype);
  return p;
}


/**
 *
 */
static setting_t *
setting_create_leaf(prop_t *parent, prop_t *title, const char *type,
		    const char *valuename, int flags)
{
  setting_t *s = calloc(1, sizeof(setting_t));
  atomic_set(&s->s_refcount, 1);
  s->s_root = prop_ref_inc(setting_add(parent, title, type, flags));
  s->s_val = prop_create_r(s->s_root, valuename);
  return s;
}


/**
 *
 */
void 
settings_add_int(setting_t *s, int delta)
{
  prop_add_int(s->s_val, delta);
}


/**
 *
 */
void
settings_create_info(prop_t *parent, const char *image,
		     prop_t *description)
{
  prop_t *r = setting_add(parent, NULL, "info", 0);
  prop_link(description, prop_create(r, "description"));
  if(image != NULL)
    prop_set_string(prop_create(r, "image"), image);
}


/**
 *
 */
prop_t *
settings_create_separator(prop_t *parent, prop_t *caption)
{
  return setting_add(parent, caption, "separator", 0);
}


/**
 *
 */
setting_t *
settings_create_action(prop_t *parent, prop_t *title,
		       prop_callback_t *cb, void *opaque,
		       int flags, prop_courier_t *pc)
{
  setting_t *s = setting_create_leaf(parent, title, "action", "action", flags);
  s->s_sub = prop_subscribe(PROP_SUB_NO_INITIAL_UPDATE,
			    PROP_TAG_CALLBACK, cb, opaque,
			    PROP_TAG_ROOT, s->s_val,
			    PROP_TAG_COURIER, pc,
			    NULL);
  return s;
}



/**
 *
 */
prop_t *
settings_create_bound_string(prop_t *parent, prop_t *title, prop_t *value)
{
  prop_t *p = setting_add(parent, title, "string", 0);
  prop_link(value, prop_create(p, "value"));
  return p;
}


/**
 *
 */
void
setting_detach(setting_t *s)
{
  prop_unparent(s->s_root);
}


/**
 *
 */
static void
setting_release(setting_t *s)
{
  if(atomic_dec(&s->s_refcount))
    return;

  if(s->s_parent != NULL)
    setting_release(s->s_parent);

  prop_ref_dec(s->s_val);
  prop_ref_dec(s->s_current_origin);
  prop_ref_dec(s->s_root);
  prop_ref_dec(s->s_ext_value);
  prop_ref_dec(s->s_current_value);

  rstr_release(s->s_default_str);
  free(s->s_id);
  free(s->s_pending_value);
  free(s->s_store_name);
  free(s);
}

/**
 *
 */
void
setting_destroy(setting_t *s)
{
  if(s->s_on_group_list)
    LIST_REMOVE(s, s_group_link);
  s->s_callback = NULL;
  prop_unsubscribe(s->s_sub);
  prop_unsubscribe(s->s_inherited_value_sub);
  prop_unsubscribe(s->s_inherited_origin_sub);
  prop_destroy(s->s_root);
  setting_release(s);
}


/**
 *
 */
void
setting_group_destroy(struct setting_list *list)
{
  setting_t *s;
  while((s = LIST_FIRST(list)) != NULL)
    setting_destroy(s);
}

/**
 *
 */
void
setting_destroyp(setting_t **sp)
{
  if(*sp) {
    setting_destroy(*sp);
    *sp = NULL;
  }
}


/**
 *
 */
int
settings_get_type(const setting_t *s)
{
  return s->s_type;
}


/**
 *
 */
static void
setting_save_htsmsg(setting_t *s)
{
  if(s->s_store_name != NULL)
    htsmsg_store_save(s->s_store, s->s_store_name);
  if(s->s_saver)
    s->s_saver(s->s_saver_opaque, s->s_store);
}


/**
 *
 */
static void
settings_int_callback_ng(void *opaque, int v)
{
  setting_t *s = opaque;
  prop_callback_int_t *cb = s->s_callback;

  s->s_value_set = 1;

  if(cb) cb(s->s_opaque, v);

  if(s->s_ext_value)
    prop_set_int(s->s_ext_value, v);

  if(s->s_flags & SETTINGS_DEBUG)
    TRACE(TRACE_DEBUG, "Settings", "Value set to %d", v);

  if(!s->s_enable_writeback)
    return;

  if(s->s_store) {
    htsmsg_delete_field(s->s_store, s->s_id);
    htsmsg_add_s32(s->s_store, s->s_id, v);
    setting_save_htsmsg(s);
  }

  if(s->s_kvstore) {
    kv_url_opt_set_deferred(s->s_store_name, KVSTORE_DOMAIN_SETTING,
                            s->s_id, KVSTORE_SET_INT, v);
  }

  prop_set_string(s->s_current_origin, s->s_origin);
}


/**
 *
 */
static void
settings_int_inherited_value(void *opaque, int v)
{
  setting_t *s = opaque;

  if(s->s_value_set)
    return;

  if(s->s_flags & SETTINGS_DEBUG)
    TRACE(TRACE_DEBUG, "Settings", "Value set to %d (inherited)", v);

  prop_callback_int_t *cb = s->s_callback;

  prop_set_int_ex(s->s_val, s->s_sub, v);

  if(cb) cb(s->s_opaque, v);

  if(s->s_ext_value)
    prop_set_int(s->s_ext_value, v);
}


/**
 *
 */
static void
settings_int_inherited_origin(void *opaque, rstr_t *origin)
{
  setting_t *s = opaque;

  if(!s->s_value_set)
    prop_set_rstring(s->s_current_origin, origin);
}


/**
 *
 */
static void
settings_string_callback_ng(void *opaque, rstr_t *rstr)
{
  setting_t *s = opaque;
  prop_callback_string_t *cb = s->s_callback;

  rstr_t *outval = rstr;

  if((rstr == NULL || rstr_get(rstr)[0] == 0) &&
     (s->s_flags & SETTINGS_EMPTY_IS_DEFAULT))
    outval = s->s_default_str;

  if(cb) cb(s->s_opaque, rstr_get(outval));

  if(s->s_ext_value)
    prop_set_rstring(s->s_ext_value, outval);

  if(!s->s_enable_writeback)
    return;

  if(s->s_store) {
    htsmsg_delete_field(s->s_store, s->s_id);
    if(rstr != NULL)
      htsmsg_add_str(s->s_store, s->s_id, rstr_get(rstr));
    setting_save_htsmsg(s);
  }

  if(s->s_kvstore)
    kv_url_opt_set_deferred(s->s_store_name, KVSTORE_DOMAIN_SETTING, s->s_id,
                            rstr ? KVSTORE_SET_STRING : KVSTORE_SET_VOID,
                            rstr_get(rstr));
}


/**
 *
 */
static void
settings_multiopt_callback_ng(void *opaque, prop_event_t event, ...)
{
  setting_t *s = opaque;
  prop_callback_string_t *cb;
  prop_t *c;
  va_list ap;
  va_start(ap, event);

  cb = s->s_callback;

  switch(event) {
  default:
    break;

  case PROP_SELECT_CHILD:
    if(s->s_pending_value) {
      free(s->s_pending_value);
      s->s_pending_value = NULL;
    }

    c = va_arg(ap, prop_t *);

    prop_ref_dec(s->s_current_value);
    s->s_current_value = prop_ref_inc(c);
    rstr_t *name = c ? prop_get_name(c) : NULL;

    if(s->s_ext_value)
      prop_set_rstring(s->s_ext_value, name);

    if(cb != NULL)
      cb(s->s_opaque, rstr_get(name));


    if(s->s_enable_writeback) {

      if(s->s_store) {
	htsmsg_delete_field(s->s_store, s->s_id);
	if(name != NULL)
	  htsmsg_add_str(s->s_store, s->s_id, rstr_get(name));
	setting_save_htsmsg(s);
      }

      if(s->s_kvstore)
	kv_url_opt_set_deferred(s->s_store_name, KVSTORE_DOMAIN_SETTING,
				s->s_id,
				name ? KVSTORE_SET_STRING : KVSTORE_SET_VOID,
				rstr_get(name));
    }

    rstr_release(name);

    break;

  case PROP_DEL_CHILD:
    c = va_arg(ap, prop_t *);

    if(c == s->s_current_value) {
      c = prop_first_child(s->s_val);
      if(c != NULL) {
	prop_select(c);
	prop_ref_dec(c);
      }
    }
    break;
  }
  va_end(ap);
}


/**
 *
 */
static void
settings_generic_set_int(void *opaque, int value)
{
  int *p = opaque;
  *p = value;
}


/**
 *
 */
static void
settings_generic_set_int_from_str(void *opaque, const char *str)
{
  int *p = opaque;
  *p = atoi(str);
}


/**
 *
 */
setting_t *
setting_create(int type, prop_t *model, int flags, ...)
{
  setting_t *s = calloc(1, sizeof(setting_t));
  prop_courier_t *pc = NULL;
  hts_mutex_t *mtx = NULL;
  int tag;
  const char *str;
  int initial_int = 0;
  const char *initial_str = NULL;
  int min = 0, max = 100, step = 1; // Just something
  prop_t *opt;
  const char **optlist;
  va_list ap;
  int i32;
  struct setting_list *list;

  atomic_set(&s->s_refcount, 1);
  s->s_type = type;
  s->s_flags = flags;
  strcpy(s->s_origin, "local");
  va_start(ap, flags);

  if(model == NULL) {
    s->s_root = prop_ref_inc(prop_create_root(NULL));
  } else if(flags & SETTINGS_RAW_NODES) {
    s->s_root = prop_create_r(model, NULL);
  } else {
    prop_t *nodes = prop_create_r(model, "nodes");
    s->s_root = prop_create_r(nodes, NULL);
    prop_ref_dec(nodes);
  }

  switch(type) {
  case SETTING_INT:
  case SETTING_BOOL:
  case SETTING_STRING:
    s->s_val = prop_create_r(s->s_root, "value");
    break;

  case SETTING_MULTIOPT:
    s->s_val = prop_create_r(s->s_root, "options");
    break;


  case SETTING_ACTION:
    s->s_val = prop_create_r(s->s_root, "action");
    break;

  case SETTING_SEPARATOR:
    break;

  default:
    abort();
  }

  prop_t *m = prop_create_r(s->s_root, "metadata");
  prop_t *title = prop_create_r(m, "title");
  prop_t *enabled = prop_create_r(s->s_root, "enabled");
  s->s_current_origin = prop_create_r(s->s_root, "origin");

  do {
    tag = va_arg(ap, int);
    switch(tag) {
    case SETTING_TAG_TITLE:
      prop_link(va_arg(ap, prop_t *), title);
      break;

    case SETTING_TAG_TITLE_CSTR:
      str = va_arg(ap, const char *);
      prop_set_string(title, str);
      break;

    case SETTING_TAG_CALLBACK:
      s->s_callback = va_arg(ap, void *);
      s->s_opaque   = va_arg(ap, void *);
      break;

    case SETTING_TAG_COURIER:
      pc = va_arg(ap, prop_courier_t *);
      break;

    case SETTING_TAG_MUTEX:
      mtx = va_arg(ap, hts_mutex_t *);
      break;

    case SETTING_TAG_HTSMSG:
      assert(!s->s_kvstore);
      mystrset(&s->s_id, va_arg(ap, const char *));
      s->s_store = va_arg(ap, htsmsg_t *);
      mystrset(&s->s_store_name, va_arg(ap, const char *));
      break;

    case SETTING_TAG_HTSMSG_CUSTOM_SAVER:
      assert(!s->s_kvstore);
      mystrset(&s->s_id, va_arg(ap, const char *));
      s->s_store        = va_arg(ap, htsmsg_t *);
      s->s_saver        = va_arg(ap, settings_saver_t *);
      s->s_saver_opaque = va_arg(ap, void *);
      break;

    case SETTING_TAG_VALUE:
      switch(type) {
      case SETTING_INT:
      case SETTING_BOOL:
        initial_int = va_arg(ap, int);
        break;
      case SETTING_STRING:
      case SETTING_MULTIOPT:
        initial_str = va_arg(ap, const char *);
        break;
      }
      break;

    case SETTING_TAG_RANGE:
      min  = va_arg(ap, int);
      max  = va_arg(ap, int);

      prop_set_int_clipping_range(s->s_val, min, max);
      break;

    case SETTING_TAG_STEP:
      step = va_arg(ap, int);
      break;

    case SETTING_TAG_UNIT_CSTR:
      str = va_arg(ap, const char *);
      prop_set(s->s_root, "unit", PROP_SET_STRING, str);
      break;

    case SETTING_TAG_OPTION:
      opt = prop_create(s->s_val, va_arg(ap, const char *));
      prop_link(va_arg(ap, prop_t *), prop_create(opt, "title"));
      break;

    case SETTING_TAG_OPTION_CSTR:
      opt = prop_create(s->s_val, va_arg(ap, const char *));
      str = va_arg(ap, const char *);
      prop_set(opt, "title", PROP_SET_STRING, str);
      break;

    case SETTING_TAG_OPTION_LIST:
      optlist = va_arg(ap, const char **);
      if(optlist == NULL)
        break;

      while(*optlist) {
        prop_t *opt = prop_create(s->s_val, optlist[0]);
        prop_set(opt, "title", PROP_SET_STRING, optlist[1]);
        optlist += 2;
      }
      break;

    case SETTING_TAG_WRITE_INT:
      s->s_opaque = va_arg(ap, int *);

      switch(type) {
      case SETTING_INT:
      case SETTING_BOOL:
        s->s_callback = &settings_generic_set_int;
        break;
      case SETTING_STRING:
      case SETTING_MULTIOPT:
        s->s_callback = &settings_generic_set_int_from_str;
        break;
      }
      break;

    case SETTING_TAG_ZERO_TEXT:
      prop_link(va_arg(ap, prop_t *), prop_create(s->s_root, "zerotext"));
      break;

    case SETTING_TAG_WRITE_PROP:
      s->s_ext_value = prop_ref_inc(va_arg(ap, prop_t *));
      break;

    case SETTING_TAG_KVSTORE:
      assert(s->s_store == NULL);
      mystrset(&s->s_store_name, va_arg(ap, const char *));
      mystrset(&s->s_id, va_arg(ap, const char *));
      s->s_kvstore = 1;
      break;

    case SETTING_TAG_PROP_ENABLER:
      prop_link(va_arg(ap, prop_t *), enabled);
      prop_ref_dec(enabled);
      enabled = NULL;
      break;

    case SETTING_TAG_VALUE_ORIGIN:
      snprintf(s->s_origin, sizeof(s->s_origin), "%s",
               va_arg(ap, const char *));
      break;

    case SETTING_TAG_GROUP:
      list = va_arg(ap, struct setting_list *);
      LIST_INSERT_HEAD(list, s, s_group_link);
      s->s_on_group_list = 1;
      break;

    case SETTING_TAG_INHERIT:
      s->s_parent = va_arg(ap, setting_t *);
      atomic_inc(&s->s_parent->s_refcount);
      initial_int = INT32_MIN;
      break;

    case 0:
      break;

    default:
      fprintf(stderr, "%s: Unsupported tag value 0x%x\n",
              __FUNCTION__, tag);
      abort();
    }
  } while(tag);

  prop_set(s->s_root, "type", PROP_SET_STRING,
           (const char *[]) {
             [SETTING_INT]        = "integer",
               [SETTING_BOOL]     = "bool",
               [SETTING_STRING]   = "string",
               [SETTING_MULTIOPT] = "multiopt",
               [SETTING_ACTION]   = "action",
               [SETTING_SEPARATOR]= "separator",
               }[type]);

  switch(type) {

  case SETTING_INT:
    prop_set(s->s_root, "min",  PROP_SET_INT, min);
    prop_set(s->s_root, "max",  PROP_SET_INT, max);
    prop_set(s->s_root, "step", PROP_SET_INT, step);
    // FALLTHRU
  case SETTING_BOOL:

    i32 = INT32_MIN;

    if(s->s_store != NULL)
      i32 = htsmsg_get_s32_or_default(s->s_store, s->s_id, INT32_MIN);

    if(s->s_kvstore)
      i32 = kv_url_opt_get_int(s->s_store_name, KVSTORE_DOMAIN_SETTING,
                               s->s_id, INT32_MIN);

    if(i32 == INT32_MIN)
      i32 = initial_int;

    if(i32 != INT32_MIN) {
      // If this setting originated the value, then set it

      // Clamp value
      if(type == SETTING_INT) {


        int x = MIN(max, MAX(min, i32));

        if(x != i32)
          TRACE(TRACE_DEBUG, "Settings", "Value %d clamped to %d", i32, x);
        i32 = x;

      } else if(type == SETTING_BOOL) {
        i32 = !!i32;
      }

      s->s_value_set = 1;
      prop_set_string(s->s_current_origin, s->s_origin);
      prop_set_int(s->s_val, i32);
      if(flags & SETTINGS_INITIAL_UPDATE)
        settings_int_callback_ng(s, i32);
    }

    s->s_sub =
      prop_subscribe(PROP_SUB_NO_INITIAL_UPDATE | PROP_SUB_IGNORE_VOID,
                     PROP_TAG_CALLBACK_INT, settings_int_callback_ng, s,
                     PROP_TAG_ROOT, s->s_val,
                     PROP_TAG_COURIER, pc,
                     PROP_TAG_MUTEX, mtx,
                     NULL);

    if(s->s_parent != NULL) {
      s->s_inherited_value_sub =
        prop_subscribe(PROP_SUB_IGNORE_VOID,
                       PROP_TAG_CALLBACK_INT, settings_int_inherited_value, s,
                       PROP_TAG_ROOT, s->s_parent->s_val,
                       PROP_TAG_COURIER, pc,
                       PROP_TAG_MUTEX, mtx,
                       NULL);

      s->s_inherited_origin_sub =
        prop_subscribe(PROP_SUB_IGNORE_VOID,
                       PROP_TAG_CALLBACK_RSTR, settings_int_inherited_origin, s,
                       PROP_TAG_NAMED_ROOT, s->s_parent->s_root, "setting",
                       PROP_TAG_NAME("setting", "origin"),
                       PROP_TAG_COURIER, pc,
                       PROP_TAG_MUTEX, mtx,
                       NULL);
    }
    break;

  case SETTING_STRING:
    if(flags & SETTINGS_PASSWORD)
      prop_set(s->s_root, "password", PROP_SET_INT, 1);

    s->s_default_str = rstr_alloc(initial_str);

    rstr_t *initial = NULL;

    if(s->s_store != NULL) {
      const char *v = htsmsg_get_str(s->s_store, s->s_id);
      if(v != NULL && (!(flags & SETTINGS_EMPTY_IS_DEFAULT) || v[0] != 0)) {
	initial = rstr_alloc(v);
      }
    }

    if(initial == NULL)
      initial = rstr_dup(s->s_default_str);

    if(s->s_kvstore) {
      rstr_t *r = kv_url_opt_get_rstr(s->s_store_name, KVSTORE_DOMAIN_SETTING,
                                      s->s_id);
      if(r != NULL)
        rstr_set(&initial, r);
    }

    prop_set_rstring(s->s_val, initial);

    if(flags & SETTINGS_INITIAL_UPDATE)
      settings_string_callback_ng(s, initial);

    rstr_release(initial);

    s->s_sub =
      prop_subscribe(PROP_SUB_NO_INITIAL_UPDATE | PROP_SUB_IGNORE_VOID,
                     PROP_TAG_CALLBACK_RSTR, settings_string_callback_ng, s,
                     PROP_TAG_ROOT, s->s_val,
                     PROP_TAG_COURIER, pc,
                     PROP_TAG_MUTEX, mtx,
                     NULL);
    break;


  case SETTING_MULTIOPT:
    if(s->s_store != NULL)
      initial_str = htsmsg_get_str(s->s_store, s->s_id) ?: initial_str;

    prop_t *o = NULL;

    if(s->s_kvstore) {
      rstr_t *r = kv_url_opt_get_rstr(s->s_store_name, KVSTORE_DOMAIN_SETTING,
                                      s->s_id);
      if(r != NULL) {
        o = prop_find(s->s_val, rstr_get(r), NULL);
      }
    }

    if(o == NULL && initial_str != NULL)
      o = prop_find(s->s_val, initial_str, NULL);

    if(o == NULL) {
      mystrset(&s->s_pending_value, initial_str);
      o = prop_first_child(s->s_val);
    }

    if(o != NULL) {
      prop_select(o);

      if(flags & SETTINGS_INITIAL_UPDATE) {
        rstr_t *name = prop_get_name(o);
        settings_string_callback_ng(s, name);
        rstr_release(name);
      }
      prop_ref_dec(o);
    }

    s->s_sub =
      prop_subscribe(PROP_SUB_NO_INITIAL_UPDATE,
                     PROP_TAG_CALLBACK, settings_multiopt_callback_ng, s,
                     PROP_TAG_ROOT, s->s_val,
                     PROP_TAG_COURIER, pc,
                     PROP_TAG_MUTEX, mtx,
                     NULL);
    break;

  case SETTING_ACTION:
    s->s_sub =
      prop_subscribe(PROP_SUB_NO_INITIAL_UPDATE,
                     PROP_TAG_CALLBACK_EVENT, s->s_callback, s->s_opaque,
                     PROP_TAG_ROOT, s->s_val,
                     PROP_TAG_COURIER, pc,
                     PROP_TAG_MUTEX, mtx,
                     NULL);
    break;

  case SETTING_SEPARATOR:
    break;
  }


  if(enabled != NULL) {
    prop_set_int(enabled, 1);
    prop_ref_dec(enabled);
  }

  s->s_enable_writeback = 1;
  prop_ref_dec(title);
  prop_ref_dec(m);
  return s;
}


/**
 *
 */
prop_t *
setting_add_option(setting_t *s, const char *id,
                   const char *title, int sel)
{
  prop_t *opt = prop_create(s->s_val, id);
  prop_set(opt, "title", PROP_SET_STRING, title);

  if((s->s_pending_value && !strcmp(s->s_pending_value, id)) || sel) {
    free(s->s_pending_value);
    s->s_pending_value = NULL;
    prop_select(opt);
  }
  return opt;
}


/**
 *
 */
void
setting_set(setting_t *s, int type, ...)
{
  if(s->s_type != type)
    return;

  const char *str;
  int i32;
  va_list ap;
  va_start(ap, type);

  switch(type) {
  case SETTING_INT:
  case SETTING_BOOL:
    i32 = va_arg(ap, int);
    prop_set_int(s->s_val, i32);
    break;

  case SETTING_STRING:
    str = va_arg(ap, const char *);
    if((str == NULL || *str == 0) && (s->s_flags & SETTINGS_EMPTY_IS_DEFAULT))
      prop_set_rstring(s->s_val, s->s_default_str);
    else
      prop_set_string(s->s_val, str);
    break;

  case SETTING_MULTIOPT:
    str = va_arg(ap, const char *);
    prop_select_by_value(s->s_val, str);
    break;
  }
  va_end(ap);
}


/**
 *
 */
void
setting_reset(setting_t *s)
{
  if(s->s_parent == NULL)
    return;

  s->s_value_set = 0;

  if(s->s_store) {
    htsmsg_delete_field(s->s_store, s->s_id);
    setting_save_htsmsg(s);
  }

  if(s->s_kvstore)
    kv_url_opt_set_deferred(s->s_store_name, KVSTORE_DOMAIN_SETTING, s->s_id,
                            KVSTORE_SET_VOID);

  prop_sub_reemit(s->s_inherited_value_sub);
  prop_sub_reemit(s->s_inherited_origin_sub);
}


/**
 *
 */
void
setting_group_reset(struct setting_list *list)
{
  setting_t *s;
  LIST_FOREACH(s, list, s_group_link)
    setting_reset(s);
}


/**
 *
 */
void
setting_push_to_ancestor(setting_t *s, const char *ancestor)
{
  setting_t *a;

  for(a = s; a != NULL; a = a->s_parent) {
    if(!strcmp(ancestor, a->s_origin))
      break;
  }
  if(a != NULL)
    prop_copy(a->s_val, s->s_val);
}


/**
 *
 */
void
setting_group_push_to_ancestor(struct setting_list *list, const char *ancestor)
{
  setting_t *s;
  LIST_FOREACH(s, list, s_group_link)
    setting_push_to_ancestor(s, ancestor);
}


/**
 *
 */
static void
set_system_name(void *opaque, const char *str)
{
  snprintf(gconf.system_name, sizeof(gconf.system_name), "%s", str);
}


/**
 *
 */
void
settings_init(void)
{
  prop_t *n, *d;
  prop_t *s1;

  settings_root = prop_create(prop_get_global(), "settings");
  prop_set_string(prop_create(settings_root, "type"), "settings");
  set_title2(settings_root, _p("Global settings"));

  settings_nodes = prop_create_root(NULL);
  s1 = prop_create_root(NULL);

  struct prop_nf *pnf;

  pnf = prop_nf_create(s1, settings_nodes, NULL, PROP_NF_AUTODESTROY);
  prop_nf_sort(pnf, "node.metadata.title", 0, 0, NULL, 1);

  gconf.settings_apps = prop_create_root(NULL);
  gconf.settings_sd = prop_create_root(NULL);

  prop_concat_t *pc;

  pc = prop_concat_create(prop_create(settings_root, "nodes"), 0);

  prop_concat_add_source(pc, s1, NULL);

  // Applications and plugins

  n = prop_create(gconf.settings_apps, "nodes");

  d = prop_create_root(NULL);
  set_title2(d, _p("Applications and installed plugins"));
  prop_set_string(prop_create(d, "type"), "separator");
  prop_concat_add_source(pc, n, d);

  d = prop_create_root(NULL);
  set_title2(d, _p("Discovered media sources"));
  prop_set_string(prop_create(d, "type"), "separator");

  n = prop_create(gconf.settings_sd, "nodes");
  prop_concat_add_source(pc, n, d);

  // General settings

  gconf.settings_general =
    settings_add_dir(NULL, _p("General"), NULL, NULL,
		     _p("System related settings"),
		     "settings:general");


  gconf.settings_network =
    settings_add_dir(NULL, _p("Network settings"), "network", NULL,
                     _p("Network services, etc"),
                     "settings:network");

  // Add configurable system name

  htsmsg_t *s = htsmsg_store_load("netinfo") ?: htsmsg_create_map();
  
  const char *sysname = NULL;
#if !defined(STOS) && (defined(linux) || defined(__APPLE__))
  char hname[64];
  if(!gethostname(hname, sizeof(hname)))
    sysname = hname;
#endif

  if(sysname == NULL)
    sysname = showtime_get_system_type();

  

  char default_name[64];
  snprintf(default_name, sizeof(default_name), "Showtime on %s", sysname);
	   

  setting_create(SETTING_STRING, gconf.settings_network,
		 SETTINGS_INITIAL_UPDATE | SETTINGS_EMPTY_IS_DEFAULT,
                 SETTING_TITLE(_p("System name")),
		 SETTING_VALUE(default_name),
                 SETTING_CALLBACK(set_system_name, NULL),
                 SETTING_HTSMSG("systemname", s, "netinfo"),
                 NULL);


  // Look and feel settings

  prop_t *lnf =
    settings_add_dir(NULL, _p("Look and feel"),
		     "display", NULL,
		     _p("Fonts and user interface styling"),
		     "settings:lookandfeel");

  gconf.settings_look_and_feel =
    prop_concat_create(prop_create(lnf, "nodes"), 0);

  // Developer settings, only available via its URI

  init_dev_settings();
}



/**
 *
 */
static int
be_settings_canhandle(const char *url)
{
  return !strncmp(url, SETTINGS_URL, strlen(SETTINGS_URL));
}





/**
 *
 */
static int
be_settings_open(prop_t *page, const char *url0, int sync)
{
  prop_link(settings_root, prop_create(page, "model"));
  return 0;
}


/**
 *
 */
static backend_t be_settings = {
  .be_canhandle = be_settings_canhandle,
  .be_open = be_settings_open,
};

BE_REGISTER(settings);


/**
 *
 */
prop_t *
makesep(prop_t *title)
{
  prop_t *d = prop_create_root(NULL);
  prop_link(title, prop_create(prop_create(d, "metadata"), "title"));
  prop_set_string(prop_create(d, "type"), "separator");
  return d;

}


/**
 *
 */
static void
add_dev_bool(htsmsg_t *s, const char *title, const char *id, int *val)
{
  setting_create(SETTING_BOOL, gconf.settings_dev, SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE_CSTR(title),
                 SETTING_WRITE_BOOL(val),
                 SETTING_HTSMSG(id, s, "dev"),
                 NULL);
}


/**
 *
 */
static void
set_netlog(void *opaque, const char *str)
{
  if(str == NULL) {
    gconf.log_server_ipv4 = 0;
    return;
  }
  char *msg = mystrdupa(str);

  char *p = strchr(msg, ':');
  if(p != NULL) {
    *p++ = 0;
    gconf.log_server_port = atoi(p);
  } else {
    gconf.log_server_port = 4000;
  }

  struct in_addr addr;

  if(inet_pton(AF_INET, msg, &addr) != 1) {
    gconf.log_server_ipv4 = 0;
  } else {
    gconf.log_server_ipv4 = addr.s_addr;
  }
}

/**
 *
 */
static void
init_dev_settings(void)
{
  htsmsg_t *s = htsmsg_store_load("dev") ?: htsmsg_create_map();

  gconf.settings_dev = settings_add_dir(prop_create_root(NULL),
				  _p("Developer settings"), NULL, NULL,
				  _p("Settings useful for developers"),
				  "settings:dev");

  prop_t *r = setting_add(gconf.settings_dev, NULL, "info", 0);
  prop_set_string(prop_create(r, "description"),
		  "Settings for developers. If you don't know what this is, don't touch it");

  add_dev_bool(s, "Various experimental features (Use at own risk)",
	       "experimental", &gconf.enable_experimental);

  add_dev_bool(s, "Enable binreplace",
	       "binreplace", &gconf.enable_bin_replace);

  add_dev_bool(s, "Enable omnigrade",
	       "omnigrade", &gconf.enable_omnigrade);

  add_dev_bool(s, "Always close pages when pressing back",
	       "navalwaysclose", &gconf.enable_nav_always_close);

  add_dev_bool(s, "Disable HTTP connection reuse",
	       "nohttpreuse", &gconf.disable_http_reuse);

  add_dev_bool(s, "Log AV-diff stats",
	       "detailedavdiff", &gconf.enable_detailed_avdiff);
#ifdef PS3
  add_dev_bool(s, "Log memory usage",
	       "memdebug", &gconf.enable_mem_debug);
#endif

  setting_create(SETTING_STRING, gconf.settings_dev, SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE_CSTR("Network log destination"),
                 SETTING_CALLBACK(set_netlog, NULL),
                 SETTING_HTSMSG("netlogdest", s, "dev"),
                 NULL);

  // ---------- debug filtering

  setting_add_cstr(gconf.settings_dev,
                   "Debug log filtering", "separator", 0);

  add_dev_bool(s, "Debug all HTTP requests",
	       "httpdebug", &gconf.enable_http_debug);

  add_dev_bool(s, "Debug HLS",
	       "hlsdebug", &gconf.enable_hls_debug);

  add_dev_bool(s, "Debug FTP Client",
	       "ftpdebug", &gconf.enable_ftp_client_debug);

  add_dev_bool(s, "Debug FTP Server",
	       "ftpserverdebug", &gconf.enable_ftp_server_debug);

  add_dev_bool(s, "Debug CEC",
	       "cecdebug", &gconf.enable_cec_debug);

  add_dev_bool(s, "Debug directory listing",
	       "fascannerdebug", &gconf.enable_fa_scanner_debug);

  add_dev_bool(s, "Debug SMB/CIFS (Windows File Sharing)",
	       "smbdebug", &gconf.enable_smb_debug);

  add_dev_bool(s, "Debug read/writes to URL key/value store",
	       "kvstoredebug", &gconf.enable_kvstore_debug);

  add_dev_bool(s, "Debug icecast streaming",
	       "icecastdebug", &gconf.enable_icecast_debug);

  add_dev_bool(s, "Debug image loading and decoding",
	       "imagedebug", &gconf.enable_image_debug);

  add_dev_bool(s, "Debug settings store/load from disk",
	       "settingsdebug", &gconf.enable_settings_debug);

  add_dev_bool(s, "Debug threads",
	       "threadsdebug", &gconf.enable_thread_debug);

}
