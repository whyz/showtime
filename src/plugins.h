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

#pragma once

void plugins_init(const char *loadme);

void plugins_init2(void);

int plugins_upgrade_check(void);

void plugin_open_file(prop_t *page, const char *url);

void plugins_reload_dev_plugin(void);

void plugin_props_from_file(prop_t *prop, const char *zipfile);

void plugin_add_static(const char *id, const char *category,
		       const char *title, const char *icon,
		       const char *synopsis,
		       const char *description,
		       void (*cb)(int enabled));

void plugin_select_view(const char *plugin_id, const char *filename);

struct htsmsg;
struct htsmsg *plugins_get_installed_list(void);
