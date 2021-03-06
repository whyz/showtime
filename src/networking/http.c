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

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "http.h"
#include "misc/time.h"
#include "misc/str.h"

/**
 *
 */
void
http_headers_free(struct http_header_list *headers)
{
  http_header_t *hh;

  if(headers == NULL)
    return;

  while((hh = LIST_FIRST(headers)) != NULL) {
    LIST_REMOVE(hh, hh_link);
    free(hh->hh_key);
    free(hh->hh_value);
    free(hh);
  }
}


/**
 *
 */
void
http_header_add_alloced(struct http_header_list *headers, const char *key,
			char *value, int append)
{
  http_header_t *hh;

  if(append) {
    hh = NULL;
  } else {
    LIST_FOREACH(hh, headers, hh_link) {
      if(!strcasecmp(hh->hh_key, key))
	break;
    }
  }
  
  if(hh == NULL) {
    hh = malloc(sizeof(http_header_t));
    hh->hh_key   = strdup(key);
    LIST_INSERT_HEAD(headers, hh, hh_link);
  } else {
    free(hh->hh_value);
  }
  hh->hh_value = value;
}

/**
 *
 */
void
http_header_add(struct http_header_list *headers, const char *key,
		const char *value, int append)
{
  http_header_add_alloced(headers, key, strdup(value), append);
}

/**
 *
 */
void
http_header_add_lws(struct http_header_list *headers, const char *data)
{
  http_header_t *hh;
  int cl;
  hh = LIST_FIRST(headers);
  if(hh == NULL)
    return;
  
  cl = strlen(hh->hh_value);
  hh->hh_value = realloc(hh->hh_value, strlen(data) + cl + 2);
  hh->hh_value[cl] = ' ';
  strcpy(hh->hh_value + cl + 1, data);
}


/**
 *
 */
void
http_header_add_int(struct http_header_list *headers, const char *key,
		    int value)
{
  char str[20];
  snprintf(str, sizeof(str), "%d", value);
  http_header_add(headers, key, str, 0);
}


/**
 *
 */
const char *
http_header_get(struct http_header_list *headers, const char *key)
{
  http_header_t *hh;

  LIST_FOREACH(hh, headers, hh_link)
    if(!strcasecmp(hh->hh_key, key))
      return hh->hh_value;
  return NULL;
}


/**
 *
 */
void
http_header_merge(struct http_header_list *dst,
		  const struct http_header_list *src)
{
  const http_header_t *hhs;
  http_header_t *hhd;

  LIST_FOREACH(hhs, src, hh_link) {
    LIST_FOREACH(hhd, dst, hh_link)
      if(!strcasecmp(hhs->hh_key, hhd->hh_key))
	break;
    if(hhd == NULL) {
      http_header_add(dst, hhs->hh_key, hhs->hh_value, 0);
    } else {
      free(hhd->hh_value);
      hhd->hh_value = strdup(hhs->hh_value);
    }
  }
}



static const char *http_months[12] = {
  "Jan", "Feb", "Mar", "Apr", "May", "Jun", 
  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static const char *http_weekdays[7] = {
  "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};


/**
 *
 */
int 
http_ctime(time_t *tp, const char *d)
{
  int year;
  int mday;
  char wday[4];
  char month[4];
  int hour;
  int min;
  int sec;
  int i;
  char dummy;

  if(sscanf(d, "%3s, %d%c%3s%c%d %d:%d:%d",
	    wday, &mday, &dummy, month, &dummy, &year, &hour, &min, &sec) != 9)
    return -1;

  for(i = 0; i < 12; i++)
    if(!strcasecmp(http_months[i], month))
      break;

  return mktime_utc(tp, year, i, mday, hour, min, sec);
}

/**
 *
 */
const char *
http_asctime(time_t tp, char *out, size_t outlen)
{
  struct tm tm = {0};

  gmtime_r(&tp, &tm);

  snprintf(out, outlen, "%s, %02d %s %04d %02d:%02d:%02d GMT",
	   http_weekdays[tm.tm_wday],
	   tm.tm_mday,
	   http_months[tm.tm_mon],
	   tm.tm_year + 1900,
	   tm.tm_hour,
	   tm.tm_min,
	   tm.tm_sec);
  return out;
}

/**
 * Parse arguments of a URI, not perfect, but works for us
 */
void
http_parse_uri_args(struct http_header_list *list, char *args,
		    int append)
{
  char *k, *v;

  while(args) {
    k = args;
    if((args = strchr(args, '=')) == NULL)
      break;
    *args++ = 0;
    v = args;
    args = strchr(args, '&');

    if(args != NULL)
      *args++ = 0;

    url_deescape(k);
    url_deescape(v);
    http_header_add(list, k, v, append);
  }
}
