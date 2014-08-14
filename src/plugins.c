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

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <assert.h>

#include "showtime.h"
#include "fileaccess/fileaccess.h"
#include "plugins.h"
#include "settings.h"
#include "htsmsg/htsmsg_json.h"
#include "htsmsg/htsmsg_store.h"
#include "backend/backend.h"
#include "misc/str.h"
#include "prop/prop_nodefilter.h"
#include "prop/prop_concat.h"
#include "notifications.h"
#include "misc/strtab.h"
#include "arch/arch.h"

#if ENABLE_SPIDERMONKEY
#include "js/js.h"
#endif

#include "ecmascript/ecmascript.h"

typedef enum {
  PLUGIN_CAT_TV,
  PLUGIN_CAT_VIDEO,
  PLUGIN_CAT_MUSIC,
  PLUGIN_CAT_CLOUD,
  PLUGIN_CAT_GLWVIEW,
  PLUGIN_CAT_SUBTITLES,
  PLUGIN_CAT_OTHER,
  PLUGIN_CAT_GLWOSK,
  PLUGIN_CAT_num,
} plugin_type_t;

static struct strtab catnames[] = {
  { "tv",          PLUGIN_CAT_TV }, 
  { "video",       PLUGIN_CAT_VIDEO },
  { "music",       PLUGIN_CAT_MUSIC },
  { "cloud",       PLUGIN_CAT_CLOUD },
  { "other",       PLUGIN_CAT_OTHER },
  { "glwview",     PLUGIN_CAT_GLWVIEW },
  { "glwosk",      PLUGIN_CAT_GLWOSK },
  { "subtitles",   PLUGIN_CAT_SUBTITLES },
};



static const char *plugin_repo_url = "http://showtime.lonelycoder.com/plugins/plugins-v1.json";
static char *plugin_alt_repo_url;
static char *plugin_beta_passwords;
static hts_mutex_t plugin_mutex;
static char *devplugin;

static prop_t *plugin_root_list;
static prop_t *plugin_start_model;
static prop_t *plugin_repo_model;

LIST_HEAD(plugin_list, plugin);
LIST_HEAD(plugin_view_list, plugin_view);
LIST_HEAD(plugin_view_entry_list, plugin_view_entry);

static struct plugin_list plugins;

static htsmsg_t *static_apps_state;

typedef struct plugin {
  LIST_ENTRY(plugin) pl_link;
  char *pl_id;
  char *pl_package;
  char *pl_title;

  char *pl_inst_ver;
  char *pl_repo_ver;
  char *pl_showtime_min_version;

  prop_t *pl_status;

  void (*pl_unload)(struct plugin *pl);

  struct plugin_view_entry_list pl_views;

  void (*pl_enable_cb)(int enabled);

  char pl_loaded;
  char pl_installed;
  char pl_can_upgrade;
  char pl_new_version_avail;
  char pl_mark;

} plugin_t;

static int plugin_install(plugin_t *pl, const char *package);
static void plugin_remove(plugin_t *pl);
static void plugin_autoupgrade(void);
static void plugins_view_settings_init(void);
static void plugins_view_add(plugin_t *pl, const char *uit, const char *class,
			     const char *title, const char *fullpath,
                             int select_now, const char *filename);
static void plugin_unload_views(plugin_t *pl);

static int autoupgrade;

/**
 *
 */
static const char *
get_repo(void)
{
  return plugin_alt_repo_url && *plugin_alt_repo_url ?
    plugin_alt_repo_url : plugin_repo_url;
}

/**
 *
 */
static void
set_alt_repo_url(void *opaque, const char *value) 
{
  mystrset(&plugin_alt_repo_url, value);
}


/**
 *
 */
static void
set_beta_passwords(void *opaque, const char *value) 
{
  mystrset(&plugin_beta_passwords, value);
}

/**
 *
 */
static void
set_autoupgrade(void *opaque, int value) 
{
  autoupgrade = value;
  plugin_autoupgrade();
}

/**
 *
 */
static plugin_t *
plugin_find(const char *id)
{
  plugin_t *pl;
  LIST_FOREACH(pl, &plugins, pl_link)
    if(!strcmp(pl->pl_id, id))
      return pl;
  
  pl = calloc(1, sizeof(plugin_t));
  pl->pl_id = strdup(id);

  pl->pl_status = prop_create_root(NULL);

  LIST_INSERT_HEAD(&plugins, pl, pl_link);
  return pl;
}


/**
 *
 */
static void
update_global_state(void)
{
  plugin_t *pl;
  int num_upgradable = 0;
  LIST_FOREACH(pl, &plugins, pl_link)
    if(pl->pl_new_version_avail)
      num_upgradable++;

  prop_setv(prop_get_global(), "plugins", "status", "upgradeable", NULL,
	    PROP_SET_INT, num_upgradable);
}


/**
 *
 */
static void
update_state(plugin_t *pl)
{
  int canInstall = 0;
  int canUninstall = 0;
  int canUpgrade = 0;
  int cantUpgrade = 0;
  rstr_t *status = NULL;

  int version_dep_ok =
    pl->pl_showtime_min_version == NULL ||
    showtime_parse_version_int(pl->pl_showtime_min_version) <=
    showtime_get_version_int();

  prop_set(pl->pl_status, "minver", PROP_SET_VOID);
  pl->pl_new_version_avail = 0;

  if(pl->pl_installed == 0) {

    if(!version_dep_ok) {
      status = _("Not installable");
      prop_set(pl->pl_status, "minver", PROP_SET_STRING,
               pl->pl_showtime_min_version);

    } else {

      status = _("Not installed");
      canInstall = 1;
    }

  } else if(!strcmp(pl->pl_inst_ver ?: "", pl->pl_repo_ver ?: "")) {
    status = _("Up to date");
    canUninstall = 1;
  } else {
    canUninstall = 1;

    if(pl->pl_repo_ver != NULL) {
      pl->pl_new_version_avail = 1;

      int repo_ver = showtime_parse_version_int(pl->pl_repo_ver);
      if(pl->pl_inst_ver != NULL &&
	 repo_ver > showtime_parse_version_int(pl->pl_inst_ver)) {

	if(!version_dep_ok) {
	  status = _("Not upgradable");
          prop_set(pl->pl_status, "minver", PROP_SET_STRING,
                   pl->pl_showtime_min_version);
	  cantUpgrade = 1;
	} else {
	  status = _("Upgradable");
	  canUpgrade = 1;
	}
      } else {
	status = _("Installed version higher than available");
      }
    }
  }

  pl->pl_can_upgrade = canUpgrade;
  prop_set(pl->pl_status, "canInstall",   PROP_SET_INT, canInstall);
  prop_set(pl->pl_status, "canUninstall", PROP_SET_INT, canUninstall);
  prop_set(pl->pl_status, "canUpgrade",   PROP_SET_INT, canUpgrade);
  prop_set(pl->pl_status, "cantUpgrade",  PROP_SET_INT, cantUpgrade);
  prop_set(pl->pl_status, "installed",    PROP_SET_INT, pl->pl_installed);
  prop_set(pl->pl_status, "statustxt",    PROP_SET_RSTRING, status);
  prop_set(pl->pl_status, "loaded",       PROP_SET_INT, pl->pl_loaded);
  prop_set(pl->pl_status, "installedVersion", PROP_SET_STRING, pl->pl_inst_ver);
  prop_set(pl->pl_status, "availableVersion", PROP_SET_STRING, pl->pl_repo_ver);
  rstr_release(status);
}


/**
 *
 */
static void
plugin_event(void *opaque, prop_event_t event, ...)
{
  plugin_t *pl = opaque;
  va_list ap;
  prop_sub_t *s;
  event_t *e;
  prop_t *p;

  va_start(ap, event);

  switch(event) {
  default:
    break;

  case PROP_DESTROYED:
    s = va_arg(ap, prop_sub_t *);
    prop_unsubscribe(s);
    break;

  case PROP_EXT_EVENT:
    e = va_arg(ap, event_t *);
    p = va_arg(ap, prop_t *);

    if(event_is_type(e, EVENT_DYNAMIC_ACTION)) {
      const event_payload_t *ep = (const event_payload_t *)e;
      if(!strcmp(ep->payload, "install")) {
	rstr_t *package = prop_get_string(p, "package", NULL);
	plugin_install(pl, rstr_get(package));
	rstr_release(package);
      }
      else if(!strcmp(ep->payload, "upgrade"))
	plugin_install(pl, NULL);
      else if(!strcmp(ep->payload, "uninstall"))
	plugin_remove(pl);
    }
    break;
  }
}


/**
 *
 */
static void
plugin_fill_prop(struct htsmsg *pm, struct prop *p,
                 const char *basepath, plugin_t *pl)
{
  const char *title = htsmsg_get_str(pm, "title") ?: pl->pl_id;
  const char *icon  = htsmsg_get_str(pm, "icon");
  const char *cat   = htsmsg_get_str(pm, "category");

  if(cat != NULL)
    cat = val2str(str2val(cat, catnames), catnames);

  prop_subscribe(PROP_SUB_TRACK_DESTROY | PROP_SUB_SINGLETON,
		 PROP_TAG_CALLBACK, plugin_event, pl,
		 PROP_TAG_ROOT, p,
		 PROP_TAG_MUTEX, &plugin_mutex,
		 NULL);

  prop_t *tmp = prop_create_r(p, "status");
  prop_link(pl->pl_status, tmp);
  prop_ref_dec(tmp);

  prop_t *metadata = prop_create_r(p, "metadata");

  prop_set(metadata, "title",    PROP_SET_STRING, title);
  prop_set(metadata, "category", PROP_SET_STRING, cat ?: "other");

  tmp = prop_create_r(metadata, "description");
  prop_set_string_ex(tmp, NULL, htsmsg_get_str(pm, "description"),
		     PROP_STR_RICH);
  prop_ref_dec(tmp);

  prop_set(metadata, "synopsis", PROP_SET_STRING,
           htsmsg_get_str(pm, "synopsis"));

  prop_set(metadata, "author", PROP_SET_STRING,
           htsmsg_get_str(pm, "author"));

  prop_set(metadata, "version", PROP_SET_STRING,
           htsmsg_get_str(pm, "version"));

  if(icon != NULL) {
    if(basepath != NULL) {
      char url[512];
      snprintf(url, sizeof(url), "%s/%s", basepath, icon);
      prop_set(metadata, "icon", PROP_SET_STRING,url);
    } else {
      char *iconurl = url_resolve_relative_from_base(get_repo(), icon);
      prop_set(metadata, "icon", PROP_SET_STRING, iconurl);
      free(iconurl);
    }
  }
  prop_ref_dec(metadata);
}



/**
 *
 */
void
plugin_props_from_file(prop_t *prop, const char *zipfile)
{
  char path[200];
  char errbuf[200];
  buf_t *b;

  snprintf(path, sizeof(path), "zip://%s/plugin.json", zipfile);
  b = fa_load(path,
               FA_LOAD_ERRBUF(errbuf, sizeof(errbuf)),
               NULL);
  if(b == NULL) {
    TRACE(TRACE_ERROR, "plugins", "Unable to open %s -- %s", path, errbuf);
    return;
  }
  htsmsg_t *pm = htsmsg_json_deserialize(buf_cstr(b));
  buf_release(b);

  if(pm == NULL)
    return;

  const char *id = htsmsg_get_str(pm, "id");

  if(id != NULL) {
    hts_mutex_lock(&plugin_mutex);
    plugin_t *pl = plugin_find(id);


    snprintf(path, sizeof(path), "zip://%s", zipfile);
    plugin_fill_prop(pm, prop, path, pl);
    prop_set(prop, "package", PROP_SET_STRING, zipfile);
    update_state(pl);
    hts_mutex_unlock(&plugin_mutex);
  }
  htsmsg_release(pm);

}


/**
 *
 */
static void
plugin_prop_setup(htsmsg_t *pm, plugin_t *pl, const char *basepath)
{
  prop_t *p;
  hts_mutex_assert(&plugin_mutex);
  p = prop_create(plugin_root_list, pl->pl_id);
  prop_set(p, "type", PROP_SET_STRING, "plugin");
  plugin_fill_prop(pm, p, basepath, pl);
}


#if ENABLE_SPIDERMONKEY
/**
 *
 */
static void
js_unload(plugin_t *pl)
{
  js_plugin_unload(pl->pl_id);
}
#endif


/**
 *
 */
static void
plugin_unload_ecmascript(plugin_t *pl)
{
  ecmascript_plugin_unload(pl->pl_id);
}



/**
 *
 */
static void
plugin_unload(plugin_t *pl)
{
  if(pl->pl_unload) {
    pl->pl_unload(pl);
    pl->pl_unload = NULL;
  }

  plugin_unload_views(pl);
}


/**
 *
 */
static int
plugin_load(const char *url, char *errbuf, size_t errlen, int force,
	    int as_installed, int by_user)
{
  char ctrlfile[URL_MAX];
  buf_t *b;
  htsmsg_t *ctrl;

  snprintf(ctrlfile, sizeof(ctrlfile), "%s/plugin.json", url);

  if((b = fa_load(ctrlfile,
                   FA_LOAD_ERRBUF(errbuf, errlen),
                   NULL)) == NULL)
    return -1;

  ctrl = htsmsg_json_deserialize2(buf_cstr(b), errbuf, errlen);
  buf_release(b);
  if(ctrl != NULL) {

    const char *type = htsmsg_get_str(ctrl, "type");
    const char *id   = htsmsg_get_str(ctrl, "id");

    if(type == NULL) {
      snprintf(errbuf, errlen, "Missing \"type\" element in control file %s",
	    ctrlfile);
      htsmsg_release(ctrl);
      return -1;
    }


    if(id == NULL) {
      snprintf(errbuf, errlen, "Missing \"id\" element in control file %s",
	    ctrlfile);
      htsmsg_release(ctrl);
      return -1;
    }

    plugin_t *pl = plugin_find(id);

    if(!force && pl->pl_loaded) {
      snprintf(errbuf, errlen, "Plugin \"%s\" already loaded",
	       id);
      htsmsg_release(ctrl);
      return -1;
    }

    plugin_unload(pl);

    int r;
    char fullpath[URL_MAX];

    if(!strcmp(type, "views")) {
      // No special tricks here, we always loads 'glwviews' from all plugins
      r = 0;

    } else if(!strcmp(type, "ecmascript")) {

      const char *file = htsmsg_get_str(ctrl, "file");
      if(file == NULL) {
	snprintf(errbuf, errlen, "Missing \"file\" element in control file %s",
		 ctrlfile);
	htsmsg_release(ctrl);
	return -1;
      }
      snprintf(fullpath, sizeof(fullpath), "%s/%s", url, file);

      hts_mutex_unlock(&plugin_mutex);
      r = ecmascript_plugin_load(id, fullpath, errbuf, errlen);
      hts_mutex_lock(&plugin_mutex);
      if(!r)
	pl->pl_unload = plugin_unload_ecmascript;


#if ENABLE_SPIDERMONKEY
    } else if(!strcmp(type, "javascript")) {

      const char *file = htsmsg_get_str(ctrl, "file");
      if(file == NULL) {
	snprintf(errbuf, errlen, "Missing \"file\" element in control file %s",
		 ctrlfile);
	htsmsg_release(ctrl);
	return -1;
      }
      snprintf(fullpath, sizeof(fullpath), "%s/%s", url, file);

      hts_mutex_unlock(&plugin_mutex);
      r = js_plugin_load(id, fullpath, errbuf, errlen);
      hts_mutex_lock(&plugin_mutex);
      if(!r)
	pl->pl_unload = js_unload;

#endif
    } else {
      snprintf(errbuf, errlen, "Unknown type \"%s\" in control file %s",
	       type, ctrlfile);
      r = -1;
    }


    // Load bundled views

    if(!r) {

      htsmsg_t *list = htsmsg_get_list(ctrl, "glwviews");

      if(list != NULL) {
	htsmsg_field_t *f;
	HTSMSG_FOREACH(f, list) {
	  htsmsg_t *o;
	  if((o = htsmsg_get_map_by_field(f)) == NULL)
	    continue;
	  const char *uit   = htsmsg_get_str(o, "uitype") ?: "standard";
	  const char *class = htsmsg_get_str(o, "class");
	  const char *title = htsmsg_get_str(o, "title");
	  const char *file  = htsmsg_get_str(o, "file");

	  if(class == NULL || title == NULL || file == NULL)
	    continue;
	  snprintf(fullpath, sizeof(fullpath), "%s/%s", url, file);

          int dosel = htsmsg_get_u32_or_default(o, "select", by_user);

	  plugins_view_add(pl, uit, class, title, fullpath, dosel, file);
	}
      }
    }

    if(!r) {

      if(as_installed) {
	plugin_prop_setup(ctrl, pl, url);
	pl->pl_installed = 1;
        mystrset(&pl->pl_inst_ver, htsmsg_get_str(ctrl, "version"));
      }

      mystrset(&pl->pl_title, htsmsg_get_str(ctrl, "title") ?: id);

      pl->pl_loaded = 1;
    }
    htsmsg_release(ctrl);
    update_state(pl);
    return r;

  } else {
    return -1;
  }
}




/**
 *
 */
static void
plugin_load_installed(void)
{
  char path[200];
  char errbuf[200];
  fa_dir_entry_t *fde;

  snprintf(path, sizeof(path), "file://%s/installedplugins",
	   gconf.persistent_path);

  fa_dir_t *fd = fa_scandir(path, NULL, 0);

  if(fd != NULL) {
    RB_FOREACH(fde, &fd->fd_entries, fde_link) {
      snprintf(path, sizeof(path), "zip://%s", rstr_get(fde->fde_url));
      if(plugin_load(path, errbuf, sizeof(errbuf), 0, 1, 0)) {
	TRACE(TRACE_ERROR, "plugins", "Unable to load %s\n%s", path, errbuf);
      }
    }
    fa_dir_free(fd);
  }
}


#define REPO_ERROR_NETWORK ((void *)-1)

/**
 *
 */
static htsmsg_t *
repo_get(const char *repo, char *errbuf, size_t errlen)
{
  buf_t *b;
  htsmsg_t *json;
  const char *qargs[32];
  int qp = 0;

  TRACE(TRACE_DEBUG, "plugins", "Loading repo from %s", repo);

  if(plugin_beta_passwords != NULL) {
    char *pws = mystrdupa(plugin_beta_passwords);
    char *tmp = NULL;

    while(qp < 30) {
      const char *p = strtok_r(pws, " ", &tmp);
      if(p == NULL)
	break;
      qargs[qp++] = "betapassword";
      qargs[qp++] = p;
      pws = NULL;
    }
  }
  qargs[qp] = 0;
  hts_mutex_unlock(&plugin_mutex);
  b = fa_load(repo,
              FA_LOAD_ERRBUF(errbuf, errlen),
              FA_LOAD_QUERY_ARGVEC(qargs),
              FA_LOAD_FLAGS(FA_COMPRESSION | FA_DISABLE_AUTH),
              NULL);

  hts_mutex_lock(&plugin_mutex);
  if(b == NULL)
    return REPO_ERROR_NETWORK;

  json = htsmsg_json_deserialize(buf_cstr(b));
  buf_release(b);

  if(json == NULL) {
    snprintf(errbuf, errlen, "Malformed JSON in repository");
    return NULL;
  }

  const int ver = htsmsg_get_u32_or_default(json, "version", 0);

  if(ver != 1) {
    snprintf(errbuf, errlen, "Unsupported repository version %d", ver);
    htsmsg_release(json);
    return NULL;
  }

  const char *msg = htsmsg_get_str(json, "message");
  if(msg != NULL) {
    snprintf(errbuf, errlen, "%s", msg);
    htsmsg_release(json);
    return NULL;
  }
  
  return json;
}



/**
 *
 */
static int
plugin_load_repo(void)
{
  plugin_t *pl, *next;
  char errbuf[512];
  htsmsg_t *msg = repo_get(get_repo(), errbuf, sizeof(errbuf));

  if(msg == REPO_ERROR_NETWORK || msg == NULL) {
    TRACE(TRACE_ERROR, "plugins", "Unable to load repo %s -- %s",
	  get_repo(), errbuf);
    return msg == REPO_ERROR_NETWORK ? -1 : 0;
  }

  htsmsg_t *r = htsmsg_get_list(msg, "plugins");
  if(r != NULL) {
    htsmsg_field_t *f;

    LIST_FOREACH(pl, &plugins, pl_link)
      pl->pl_mark = 1;

    HTSMSG_FOREACH(f, r) {
      htsmsg_t *pm;
      if((pm = htsmsg_get_map_by_field(f)) == NULL)
	continue;
      const char *id = htsmsg_get_str(pm, "id");
      if(id == NULL)
	continue;
      pl = plugin_find(id);
      pl->pl_mark = 0;
      plugin_prop_setup(pm, pl, NULL);
      mystrset(&pl->pl_repo_ver, htsmsg_get_str(pm, "version"));
      mystrset(&pl->pl_showtime_min_version,
	       htsmsg_get_str(pm, "showtimeVersion"));
      update_state(pl);

      const char *dlurl = htsmsg_get_str(pm, "downloadURL");
      if(dlurl != NULL) {
	char *package = url_resolve_relative_from_base(get_repo(), dlurl);
	free(pl->pl_package);
	pl->pl_package = package;
      }
    }

    for(pl = LIST_FIRST(&plugins); pl != NULL; pl = next) {
      next = LIST_NEXT(pl, pl_link);
      prop_set(pl->pl_status, "inRepo", PROP_SET_INT, !pl->pl_mark);
      if(pl->pl_mark && !pl->pl_enable_cb) {
	pl->pl_mark = 0;
      }
    }
  }

  r = htsmsg_get_list(msg, "blacklist");
  if(r != NULL) {
    htsmsg_field_t *f;
    HTSMSG_FOREACH(f, r) {
      htsmsg_t *pm;
      if((pm = htsmsg_get_map_by_field(f)) == NULL)
	continue;

      const char *id      = htsmsg_get_str(pm, "id");
      const char *version = htsmsg_get_str(pm, "version");

      if(id == NULL || version == NULL)
	continue;

      LIST_FOREACH(pl, &plugins, pl_link)
	if(!strcmp(id, pl->pl_id) && pl->pl_installed && pl->pl_inst_ver &&
	   !strcmp(version, pl->pl_inst_ver))
	  break;

      if(pl != NULL) {
	notify_add(NULL, NOTIFY_ERROR, NULL, 10, 
		   _("Plugin %s %s has been uninstalled because it may cause problems.\nYou may try reinstalling a different version manually."), pl->pl_title, pl->pl_inst_ver);
	plugin_remove(pl);
      }
    }
  }
  htsmsg_release(msg);
  return 0;
}


/**
 *
 */
static void
plugin_autoupgrade(void)
{
  plugin_t *pl;

  if(!autoupgrade)
    return;

  LIST_FOREACH(pl, &plugins, pl_link) {
    if(!pl->pl_can_upgrade)
      continue;
    if(plugin_install(pl, NULL))
      continue;
    notify_add(NULL, NOTIFY_INFO, NULL, 5, 
	       _("Upgraded plugin %s to version %s"), pl->pl_title,
	       pl->pl_inst_ver);
  }
  update_global_state();
}


/**
 *
 */
static void
plugin_setup_start_model(void)
{
  prop_concat_t *pc;
  struct prop_nf *pnf;
  prop_t *d, *p;

  plugin_start_model = prop_create_root(NULL);
  prop_set(plugin_start_model, "safeui",  PROP_SET_INT, 1);
  prop_set(plugin_start_model, "contents", PROP_SET_STRING, "plugins");
  prop_set(plugin_start_model, "type",     PROP_SET_STRING, "directory");

  prop_link(_p("Apps"),
	    prop_create(prop_create(plugin_start_model, "metadata"), "title"));

  pc = prop_concat_create(prop_create(plugin_start_model, "nodes"), 0);

  // Top items

  prop_t *sta = prop_create_root(NULL);
  
  p = prop_create(sta, NULL);
  prop_set_string(prop_create(p, "type"), "directory");
  prop_link(_p("Browse available apps"),
	    prop_create(prop_create(p, "metadata"), "title"));
  prop_set_string(prop_create(p, "url"), "plugin:repo");

  prop_concat_add_source(pc, sta, NULL);

  // Installed plugins

  prop_t *inst = prop_create_root(NULL);
  pnf = prop_nf_create(inst, plugin_root_list, NULL, PROP_NF_AUTODESTROY);
  prop_nf_pred_int_add(pnf, "node.status.installed",
                       PROP_NF_CMP_NEQ, 1, NULL, PROP_NF_MODE_EXCLUDE);

  d = prop_create_root(NULL);
  prop_link(_p("Installed apps"),
	    prop_create(prop_create(d, "metadata"), "title"));
  prop_set_string(prop_create(d, "type"), "separator");
  prop_concat_add_source(pc, inst, d);
}

/**
 *
 */
static void
plugin_setup_repo_model(void)
{
  int i;
  struct prop_nf *pnf;
  prop_concat_t *pc;

  prop_t *model = prop_create_root(NULL);
  plugin_repo_model = model;
  prop_set(model, "type",     PROP_SET_STRING, "directory");
  prop_set(model, "safeui",   PROP_SET_INT,    1);
  prop_set(model, "contents", PROP_SET_STRING, "plugins");

  prop_link(_p("Available apps"),
	    prop_create(prop_create(model, "metadata"), "title"));

  pc = prop_concat_create(prop_create(model, "nodes"), 0);

  // Create filters per category

  for(i = 0; i < PLUGIN_CAT_num; i++) {
    const char *catname = val2str(i, catnames);
    prop_t *cat = prop_create(model, catname);
    pnf = prop_nf_create(cat, plugin_root_list, NULL, PROP_NF_AUTODESTROY);

    prop_nf_pred_str_add(pnf, "node.metadata.category",
			 PROP_NF_CMP_NEQ, catname, NULL,
			 PROP_NF_MODE_EXCLUDE);
    prop_nf_pred_int_add(pnf, "node.status.inRepo",
			 PROP_NF_CMP_NEQ, 1, NULL,
			 PROP_NF_MODE_EXCLUDE);
    prop_nf_sort(pnf, "node.metadata.title", 0, 0, NULL, 1);
    prop_nf_release(pnf);

    prop_t *header = prop_create_root(NULL);
    prop_set(header, "type", PROP_SET_STRING, "separator");

    prop_t *gn = NULL;
    switch(i) {
    case PLUGIN_CAT_TV:
      gn = _p("Online TV");
      break;

    case PLUGIN_CAT_VIDEO:
      gn = _p("Video streaming");
      break;

    case PLUGIN_CAT_MUSIC:
      gn = _p("Music streaming");
      break;

    case PLUGIN_CAT_CLOUD:
      gn = _p("Cloud services");
      break;

    case PLUGIN_CAT_GLWVIEW:
      gn = _p("User interface extensions");
      break;

    case PLUGIN_CAT_GLWOSK:
      gn = _p("On Screen Keyboards");
      break;

    case PLUGIN_CAT_SUBTITLES:
      gn = _p("Subtitles");
      break;

    default:
      gn = _p("Uncategorized");
      break;
    }

    prop_link(gn, prop_create(prop_create(header, "metadata"), "title"));
    prop_concat_add_source(pc, cat, header);
  }

}


/**
 *
 */
static void
plugins_setup_root_props(void)
{
  htsmsg_t *store = htsmsg_store_load("pluginconf") ?: htsmsg_create_map();
  prop_t *parent = prop_create(prop_get_global(), "plugins");

  plugin_root_list = prop_create(parent, "nodes");

  plugin_setup_start_model();
  plugin_setup_repo_model();

  // Settings

  settings_create_separator(gconf.settings_general,
			  _p("Plugins"));

  setting_create(SETTING_STRING, gconf.settings_general,
                 SETTINGS_INITIAL_UPDATE,
                 SETTING_HTSMSG("alt_repo", store, "pluginconf"),
                 SETTING_TITLE(_p("Alternate plugin Repository URL")),
                 SETTING_CALLBACK(set_alt_repo_url, NULL),
                 SETTING_MUTEX(&plugin_mutex),
                 NULL);

  setting_create(SETTING_STRING, gconf.settings_general,
                 SETTINGS_INITIAL_UPDATE,
                 SETTING_HTSMSG("betapasswords", store, "pluginconf"),
                 SETTING_TITLE(_p("Beta testing passwords")),
                 SETTING_CALLBACK(set_beta_passwords, NULL),
                 SETTING_MUTEX(&plugin_mutex),
                 NULL);

  setting_create(SETTING_BOOL, gconf.settings_general, SETTINGS_INITIAL_UPDATE,
                 SETTING_HTSMSG("autoupgrade", store, "pluginconf"),
                 SETTING_TITLE(_p("Automatically upgrade plugins")),
                 SETTING_VALUE(1),
                 SETTING_CALLBACK(set_autoupgrade, NULL),
                 SETTING_MUTEX(&plugin_mutex),
                 NULL);
}


static void install_static(plugin_t *pl);

/**
 *
 */
static void
plugin_static_event(void *opaque, prop_event_t event, ...)
{
  plugin_t *pl = opaque;
  va_list ap;
  event_t *e;

  va_start(ap, event);

  switch(event) {
  default:
    break;

  case PROP_DESTROYED:
    prop_unsubscribe(va_arg(ap, prop_sub_t *));
    break;

  case PROP_EXT_EVENT:
    e = va_arg(ap, event_t *);

    if(event_is_type(e, EVENT_DYNAMIC_ACTION)) {
      const event_payload_t *ep = (const event_payload_t *)e;
      if(!strcmp(ep->payload, "install")) {
	pl->pl_installed = 1;
	install_static(pl);
      } else if(!strcmp(ep->payload, "uninstall")) {
	pl->pl_installed = 0;
      } else {
	break;
      }
    }
    prop_set(pl->pl_status, "installed", PROP_SET_INT, pl->pl_installed);
    pl->pl_enable_cb(pl->pl_installed);
    htsmsg_delete_field(static_apps_state, pl->pl_id);
    htsmsg_add_u32(static_apps_state, pl->pl_id, pl->pl_installed);
    htsmsg_store_save(static_apps_state, "staticapps");
    update_state(pl);
    break;
  }
}



/**
 *
 */
static void
install_static(plugin_t *pl)
{
  prop_t *p;
  p = prop_create(plugin_root_list, pl->pl_id);
  prop_set(p, "type", PROP_SET_STRING, "plugin");

  prop_link(pl->pl_status, prop_create(p, "status"));

  prop_subscribe(PROP_SUB_TRACK_DESTROY | PROP_SUB_SINGLETON,
		 PROP_TAG_CALLBACK, plugin_static_event, pl,
		 PROP_TAG_ROOT, p,
		 PROP_TAG_MUTEX, &plugin_mutex,
		 NULL);
}


/**
 *
 */
void
plugin_add_static(const char *id, const char *category,
		  const char *title, const char *icon,
		  const char *synopsis,
		  const char *description,
		  void (*cb)(int enabled))
{
  plugin_t *pl = plugin_find(id);
  pl->pl_enable_cb = cb;
  prop_t *p = prop_create(plugin_root_list, pl->pl_id);
  prop_set(p, "type", PROP_SET_STRING, "plugin");

  prop_t *metadata = prop_create(p, "metadata");
  prop_set(metadata, "title", PROP_SET_STRING, title);
  prop_set(metadata, "category", PROP_SET_STRING, category);
  prop_set(metadata, "icon", PROP_SET_STRING, icon);
  prop_set(metadata, "synopsis", PROP_SET_STRING, synopsis);
  prop_set_string_ex(prop_create(metadata, "description"),
		     NULL,
		     description,
		     PROP_STR_RICH);

  prop_link(pl->pl_status, prop_create(p, "status"));

  pl->pl_installed = htsmsg_get_u32_or_default(static_apps_state, id, 0);
  update_state(pl);

  prop_subscribe(0,
		 PROP_TAG_CALLBACK, plugin_static_event, pl,
		 PROP_TAG_ROOT, p,
		 PROP_TAG_MUTEX, &plugin_mutex,
		 NULL);

  if(pl->pl_installed)
    install_static(pl);

  cb(pl->pl_installed);
  
}


/**
 *
 */
void
plugins_init2(void)
{
  hts_mutex_lock(&plugin_mutex);
  plugin_load_installed();
  static_apps_state = htsmsg_store_load("staticapps") ?: htsmsg_create_map();
  init_group(INIT_GROUP_STATIC_APPS);
  hts_mutex_unlock(&plugin_mutex);
}


/**
 *
 */
int
plugins_upgrade_check(void)
{
  hts_mutex_lock(&plugin_mutex);
  int r = plugin_load_repo();
  if(!r) {
    update_global_state();
    plugin_autoupgrade();
  }
  hts_mutex_unlock(&plugin_mutex);
  return r;
}


/**
 *
 */
void
plugins_init(const char *loadme)
{

  plugins_view_settings_init();

  hts_mutex_init(&plugin_mutex);

  plugins_setup_root_props();

  hts_mutex_lock(&plugin_mutex);

  if(loadme != NULL) {
    char errbuf[200];
    char url[PATH_MAX];
    if(fa_normalize(loadme, url, sizeof(url))) {
      TRACE(TRACE_ERROR, "plugins",
	    "Unable to load development plugin fa_normalize() failed");
    } else {
      devplugin = strdup(url);
      if(plugin_load(devplugin, errbuf, sizeof(errbuf), 1, 0, 0)) {
	TRACE(TRACE_ERROR, "plugins",
	      "Unable to load development plugin: %s\n%s", loadme, errbuf);
      } else {
	TRACE(TRACE_INFO, "plugins", "Loaded dev plugin %s", devplugin);
      }
    }
  }
  hts_mutex_unlock(&plugin_mutex);
}


/**
 *
 */
void
plugins_reload_dev_plugin(void)
{
  char errbuf[200];
  if(devplugin == NULL)
    return;

  hts_mutex_lock(&plugin_mutex);

  if(plugin_load(devplugin, errbuf, sizeof(errbuf), 1, 0, 1))
    TRACE(TRACE_ERROR, "plugins", 
	  "Unable to reload development plugin: %s\n%s", devplugin, errbuf);
  else
    TRACE(TRACE_INFO, "plugins", "Reloaded dev plugin %s", devplugin);

  hts_mutex_unlock(&plugin_mutex);
}


/**
 *
 */
static int
plugin_canhandle(const char *url)
{
  return !strncmp(url, "plugin:", strlen("plugin:"));
}


/**
 *
 */
static void
plugin_remove(plugin_t *pl)
{
  char path[PATH_MAX];

  snprintf(path, sizeof(path), "%s/installedplugins/%s.zip",
	   gconf.persistent_path, pl->pl_id);
  unlink(path);

  TRACE(TRACE_DEBUG, "plugin", "Uninstalling %s", pl->pl_id);
  htsmsg_store_remove("plugins/%s", pl->pl_id);

  plugin_unload(pl);

  pl->pl_installed = 0;
  pl->pl_loaded = 0;
  mystrset(&pl->pl_inst_ver, NULL);
  update_state(pl);
}


/**
 *
 */
static int
plugin_install(plugin_t *pl, const char *package)
{
  char errbuf[200];
  char path[200];

  if(package == NULL)
    package = pl->pl_package;

  prop_t *status = prop_create_r(pl->pl_status, "statustxt");

  if(package == NULL) {
    prop_unlink(status);
    prop_set_string(status, "No package file specified");
    prop_ref_dec(status);
    return -1;
  }

  TRACE(TRACE_INFO, "plugins", "Downloading plugin %s from %s",
	pl->pl_id, package);

  prop_link(_p("Downloading"), status);
  prop_set(pl->pl_status, "canInstall", PROP_SET_INT, 0);

  hts_mutex_unlock(&plugin_mutex);

  buf_t *b = fa_load(package, FA_LOAD_ERRBUF(errbuf, sizeof(errbuf)), NULL);

  hts_mutex_lock(&plugin_mutex);

  if(b == NULL) {
    prop_unlink(status);
    prop_set_string(status, errbuf);
  cleanup:
    prop_set(pl->pl_status, "canInstall", PROP_SET_INT, 1);
    prop_ref_dec(status);
    return -1;
  }

  const uint8_t *buf = buf_c8(b);

  if(b->b_size < 4 ||
     buf[0] != 0x50 || buf[1] != 0x4b || buf[2] != 0x03 || buf[3] != 0x04) {
    prop_link(_p("Corrupt plugin bundle"), status);
    goto cleanup;
  }

  TRACE(TRACE_INFO, "plugins", "Plugin %s valid ZIP archive %d bytes",
	pl->pl_id, (int)b->b_size);

  prop_link(_p("Installing"), status);

  snprintf(path, sizeof(path), "%s/installedplugins", gconf.persistent_path);
  mkdir(path, 0770);

  plugin_unload(pl);

  snprintf(path, sizeof(path), "%s/installedplugins/%s.zip",
	   gconf.persistent_path, pl->pl_id);
  if(unlink(path)) {
    TRACE(TRACE_DEBUG, "plugins", "First unlinking %s -- %s",
	  path, strerror(errno));
  }

  int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0660);
  if(fd == -1) {
    TRACE(TRACE_ERROR, "plugins", "Unable to write to %s -- %s",
	  path, strerror(errno));
    prop_link(_p("File open error"), status);
    buf_release(b);
    goto cleanup;
  }
  size_t bsize = b->b_size;
  size_t r = write(fd, buf, bsize);
  buf_release(b);
  if(close(fd) || r != bsize) {
    TRACE(TRACE_ERROR, "plugins", "Unable to write to %s -- %s",
	  path, strerror(errno));
    buf_release(b);
    prop_link(_p("Disk write error"), status);
    goto cleanup;
  }


  snprintf(path, sizeof(path),
	   "zip://file://%s/installedplugins/%s.zip", gconf.persistent_path,
	   pl->pl_id);

#ifdef STOS
  arch_sync_path(path);
#endif

  if(plugin_load(path, errbuf, sizeof(errbuf), 1, 1, 1)) {
    prop_unlink(status);
    TRACE(TRACE_ERROR, "plugins", "Unable to load %s -- %s", path, errbuf);
    prop_set_string(status, errbuf);
    goto cleanup;
  }
  prop_ref_dec(status);
  return 0;
}


/**
 *
 */
static int
plugin_open_url(prop_t *page, const char *url, int sync)
{
  if(!strcmp(url, "plugin:start")) {
    prop_link(plugin_start_model, prop_create(page, "model"));
  } else if(!strcmp(url, "plugin:repo")) {
    prop_link(plugin_repo_model, prop_create(page, "model"));
    plugins_upgrade_check();
  } else {
    nav_open_error(page, "Invalid URI");
  }
  return 0;
}




/**
 *
 */
static backend_t be_plugin = {
  .be_canhandle = plugin_canhandle,
  .be_open = plugin_open_url,
};

BE_REGISTER(plugin);

/**
 *
 */
void
plugin_open_file(prop_t *page, const char *url)
{
  char path[200];
  char errbuf[200];
  buf_t *b;

  snprintf(path, sizeof(path), "zip://%s/plugin.json", url);
  b = fa_load(path,
               FA_LOAD_ERRBUF(errbuf, sizeof(errbuf)),
               NULL);
  if(b == NULL) {
    nav_open_errorf(page, _("Unable to load plugin.json: %s"), errbuf);
    return;
  }

  htsmsg_t *pm = htsmsg_json_deserialize(buf_cstr(b));
  buf_release(b);

  if(pm == NULL) {
    nav_open_errorf(page, _("Unable to load plugin.json: Malformed JSON"));
    return;
  }

  const char *id = htsmsg_get_str(pm, "id");

  if(id != NULL) {
    hts_mutex_lock(&plugin_mutex);
    plugin_t *pl = plugin_find(id);
    plugin_install(pl, url);
    hts_mutex_unlock(&plugin_mutex);
  } else {
    nav_open_errorf(page, _("Field \"id\" not found in plugin.json"));
  }
  htsmsg_release(pm);
}

/**
 *
 */

static struct plugin_view_list plugin_views;

typedef struct plugin_view_entry {
  LIST_ENTRY(plugin_view_entry) pve_plugin_link;
  LIST_ENTRY(plugin_view_entry) pve_type_link;
  char *pve_key;
  char *pve_filename;
  prop_t *pve_type_prop;
  prop_t *pve_setting_prop; // Can be NULL (for "default" option)
} plugin_view_entry_t;


typedef struct plugin_view {
  LIST_ENTRY(plugin_view) pv_link;
  const char *pv_type;
  const char *pv_class;
  setting_t *pv_s;
  struct plugin_view_entry_list pv_entries;
} plugin_view_t;


/**
 *
 */
static void
pvs_cb(void *opaque, const char *str)
{
  plugin_view_t *pv = opaque;
  plugin_view_entry_t *pve;

  LIST_FOREACH(pve, &pv->pv_entries, pve_type_link) {
    if(!strcmp(pve->pve_key, str))
      break;
  }
  if(pve != NULL)
    prop_select(pve->pve_type_prop);
}


/**
 *
 */
static void
add_view_type(htsmsg_t *store, prop_t *p,
              const char *type, const char *class, prop_t *title)
{
  char id[256];
  plugin_view_t *pv = calloc(1, sizeof(plugin_view_t));
  LIST_INSERT_HEAD(&plugin_views, pv, pv_link);
  pv->pv_type  = type;
  pv->pv_class = class;
  snprintf(id, sizeof(id), "%s-%s", type, class);
  pv->pv_s =
    setting_create(SETTING_MULTIOPT, p, SETTINGS_INITIAL_UPDATE,
                   SETTING_TITLE(title),
                   SETTING_HTSMSG(id, store, "selectedviews"),
                   SETTING_CALLBACK(pvs_cb, pv),
                   SETTING_OPTION("default", _p("Default")),
                   SETTING_MUTEX(&plugin_mutex),
                   NULL);

  plugin_view_entry_t *pve = calloc(1, sizeof(plugin_view_entry_t));
  prop_t *r = prop_create(prop_create(prop_get_global(), "glw"), "views");
  r = prop_create(prop_create(r, type), class);
  pve->pve_type_prop = prop_create_r(r, NULL);
  pve->pve_key = strdup("default");
   LIST_INSERT_HEAD(&pv->pv_entries, pve, pve_type_link);
}


/**
 *
 */
static void
plugins_view_settings_init(void)
{
  prop_t *p = prop_create_root(NULL);

  htsmsg_t *s = htsmsg_store_load("selectedviews") ?: htsmsg_create_map();

  prop_concat_add_source(gconf.settings_look_and_feel,
			 prop_create(p, "nodes"),
			 makesep(_p("Preferred views from plugins")));

  add_view_type(s, p, "standard", "background",  _p("Background"));
  add_view_type(s, p, "standard", "loading",     _p("Loading screen"));
  add_view_type(s, p, "standard", "screensaver", _p("Screen saver"));
  add_view_type(s, p, "standard", "home",        _p("Home page"));
  add_view_type(s, p, "standard", "osk",         _p("On Screen Keyboards"));

  settings_create_separator(p, _p("Browsing"));

  add_view_type(s, p, "standard", "tracks",     _p("Audio tracks"));
  add_view_type(s, p, "standard", "album",      _p("Album"));
  add_view_type(s, p, "standard", "albums",     _p("List of albums"));
  add_view_type(s, p, "standard", "artist",     _p("Artist"));
  add_view_type(s, p, "standard", "tvchannels", _p("TV channels"));
  add_view_type(s, p, "standard", "images",     _p("Images"));
  add_view_type(s, p, "standard", "movies",     _p("Movies"));
}


/**
 *
 */
static void
plugins_view_add(plugin_t *pl,
		 const char *type, const char *class,
		 const char *title, const char *path,
                 int select_now, const char *filename)
{
  plugin_view_t *pv;

  hts_mutex_assert(&plugin_mutex);

  prop_t *r = prop_create(prop_create(prop_get_global(), "glw"), "views");
  r = prop_create(prop_create(r, type), class);

  TRACE(TRACE_INFO, "plugins",
	"Added view uitype:%s class:%s title:%s from %s%s",
	type, class, title, path, select_now ? " (selected)" : "");

  LIST_FOREACH(pv, &plugin_views, pv_link)
    if(!strcmp(pv->pv_class, class) && !strcmp(pv->pv_type, type))
      break;

  plugin_view_entry_t *pve = calloc(1, sizeof(plugin_view_entry_t));
  pve->pve_type_prop = prop_create_r(r, path);
  prop_set_link(pve->pve_type_prop, title, path);

  LIST_INSERT_HEAD(&pl->pl_views, pve, pve_plugin_link);
  pve->pve_key = strdup(path);
  pve->pve_filename = strdup(filename);

  if(pv != NULL) {
    pve->pve_setting_prop = setting_add_option(pv->pv_s, path, title,
                                               select_now);
    LIST_INSERT_HEAD(&pv->pv_entries, pve, pve_type_link);
  }
}


/**
 *
 */
void
plugin_select_view(const char *plugin_id, const char *filename)
{
  plugin_t *pl;

  TRACE(TRACE_DEBUG, "plugins", "Selecting view %s in plugin %s",
        filename, plugin_id);

  hts_mutex_lock(&plugin_mutex);

  LIST_FOREACH(pl, &plugins, pl_link)
    if(!strcmp(pl->pl_id, plugin_id))
      break;

  if(pl != NULL) {
    plugin_view_entry_t *pve;
    LIST_FOREACH(pve, &pl->pl_views, pve_plugin_link) {
      if(!strcmp(pve->pve_filename, filename)) {
        prop_select(pve->pve_setting_prop);
      }
    }
  }
  hts_mutex_unlock(&plugin_mutex);
}


/**
 *
 */
static void
plugin_unload_views(plugin_t *pl)
{
  plugin_view_entry_t *pve;

  while((pve = LIST_FIRST(&pl->pl_views)) != NULL) {
    LIST_REMOVE(pve, pve_plugin_link);
    if(pve->pve_setting_prop != NULL) {
      prop_destroy(pve->pve_setting_prop);
      LIST_REMOVE(pve, pve_type_link);
    }
    free(pve->pve_key);
    free(pve->pve_filename);
    prop_destroy(pve->pve_type_prop);
    prop_ref_dec(pve->pve_type_prop);
    free(pve);
  }
}


/**
 *
 */
htsmsg_t *
plugins_get_installed_list(void)
{
  htsmsg_t *m = htsmsg_create_map();
  plugin_t *pl;

  hts_mutex_lock(&plugin_mutex);

  LIST_FOREACH(pl, &plugins, pl_link) {
    if(!pl->pl_installed)
      continue;

    htsmsg_t *p = htsmsg_create_map();
    if(pl->pl_inst_ver != NULL)
      htsmsg_add_str(p, "v", pl->pl_inst_ver);
    htsmsg_add_msg(m, pl->pl_id, p);
  }

  hts_mutex_unlock(&plugin_mutex);
  return m;
}
