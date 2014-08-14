/*
 *  Copyright (C) 2013 Andreas Öman
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

#include <string.h>
#include <unistd.h>

#include "navigator.h"
#include "backend/backend.h"


/**
 *
 */
static int
hls_canhandle(const char *url)
{
  return !strncmp(url, "magnet:", strlen("magnet:"));
}


/**
 *
 */
static backend_t be_hls = {
  .be_canhandle = hls_canhandle,
  .be_open = backend_open_video,
  .be_play_video = hls_playvideo,
};

BE_REGISTER(hls);
