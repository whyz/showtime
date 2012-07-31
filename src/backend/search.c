/*
 *  Unified search
 *  Copyright (C) 2010 Andreas Öman
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

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#include "showtime.h"
#include "settings.h"
#include "event.h"
#include "navigator.h"
#include "prop/prop_nodefilter.h"
#include "backend/backend.h"
#include "backend/backend_prop.h"
#include "backend/search.h"


/**
 *
 */
int
search_class_create(prop_t *parent, prop_t **nodesp, prop_t **entriesp,
		    const char *title, const char *icon)
{
  prop_t *p = prop_create_root(NULL);
  prop_t *m = prop_create(p, "metadata");
  prop_t *n, *e;
  
  rstr_t *url = backend_prop_make(p, NULL);
  prop_set_rstring(prop_create(p, "url"), url);
  rstr_release(url);

  prop_set_string(prop_create(m, "title"), title);
  if(icon != NULL)
    prop_set_string(prop_create(m, "icon"), icon);
  prop_set_string(prop_create(p, "type"), "directory");
      
  n = prop_create(p, "nodes");
  e = prop_create(p, "entries");
  prop_set_int(e, 0);

  *nodesp = prop_ref_inc(n);
  *entriesp = prop_ref_inc(e);

  if(prop_set_parent(p, parent)) {
    prop_destroy(p);
    return 1;
  }
  return 0;
}


/**
 *
 */
prop_t *
search_get_settings(void)
{
  static prop_t *p;

  if(p == NULL)
    p = settings_add_dir(NULL, _p("Search"), "search", NULL, NULL,
			 "settings:search");
  return p;
}


/**
 *
 */
static int
search_canhandle(const char *url)
{
  return !strncmp(url, "search:", strlen("search:"));
}


/**
 *
 */
static int
search_open(prop_t *page, const char *url0)
{
  const char *url;
  prop_t *model, *meta, *source;
  char title[256];
  if((url = strchr(url0, ':')) == NULL)
    abort();
  url++;

  if(!backend_open(page, url))
    return 0;
  
  model = prop_create(page, "model");
  prop_set_string(prop_create(model, "type"), "directory");
  
  meta = prop_create(model, "metadata");
  rstr_t *fmt = _("Search result for: %s");
  snprintf(title, sizeof(title), rstr_get(fmt), url);
  rstr_release(fmt);
  prop_set_string(prop_create(meta, "title"), title);


  source = prop_create(page, "source");

  struct prop_nf *pnf;

  pnf = prop_nf_create(prop_create(model, "nodes"),
		       prop_create(source, "nodes"),
		       NULL, PROP_NF_AUTODESTROY);

  prop_nf_sort(pnf, "node.metadata.title", 0, 2, NULL, 1);

  prop_nf_pred_int_add(pnf, "node.entries",
		       PROP_NF_CMP_EQ, 0, NULL, 
		       PROP_NF_MODE_EXCLUDE);

  prop_nf_release(pnf);

  backend_search(source, url);
  return 0;
}

/**
 *
 */
static backend_t be_search = {
  .be_canhandle = search_canhandle,
  .be_open = search_open,
};

BE_REGISTER(search);
