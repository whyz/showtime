/*
 *  Various string manipulation functions
 *  Copyright (C) 2010 Andreas Öman
 *  Copyright (C) 2010 Mattias Wadman
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "misc/str.h"
#include "showtime.h"
#include "sha.h"
#include "i18n.h"

#include "unicode_casefolding.h"
#include "charset_detector.h"

/**
 * De-escape HTTP URL
 */
void
url_deescape(char *s)
{
  char v, *d = s;

  while(*s) {
    if(*s == '+') {
      *d++ = ' ';
      s++;
    } else if(*s == '%') {
      s++;
      switch(*s) {
      case '0' ... '9':
	v = (*s - '0') << 4;
	break;
      case 'a' ... 'f':
	v = (*s - 'a' + 10) << 4;
	break;
      case 'A' ... 'F':
	v = (*s - 'A' + 10) << 4;
	break;
      default:
	*d = 0;
	return;
      }
      s++;
      switch(*s) {
      case '0' ... '9':
	v |= (*s - '0');
	break;
      case 'a' ... 'f':
	v |= (*s - 'a' + 10);
	break;
      case 'A' ... 'F':
	v |= (*s - 'A' + 10);
	break;
      default:
	*d = 0;
	return;
      }
      s++;

      *d++ = v;
    } else {
      *d++ = *s++;
    }
  }
  *d = 0;
}

static const char hexchars[16] = "0123456789ABCDEF";

static const char url_escape_param[256] = {
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // 0x00
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // 0x10
  2,0,0,0, 0,0,0,0, 0,0,0,0, 0,1,1,0,   // 0x20
  1,1,1,1, 1,1,1,1, 1,1,0,0, 0,0,0,0,   // 0x30
  0,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1,   // 0x40
  1,1,1,1, 1,1,1,1, 1,1,1,0, 0,0,0,1,   // 0x50
  0,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1,   // 0x60
  1,1,1,1, 1,1,1,1, 1,1,1,0, 0,0,1,0,   // 0x70
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // 0x80
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // 0x90
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // 0xa0
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // 0xb0
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // 0xc0
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // 0xd0
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // 0xe0
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // 0xf0
};



static const char url_escape_path[256] = {
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // 0x00
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // 0x10
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,1,1,1,   // 0x20
  1,1,1,1, 1,1,1,1, 1,1,0,0, 0,0,0,0,   // 0x30
  0,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1,   // 0x40
  1,1,1,1, 1,1,1,1, 1,1,1,0, 0,0,0,1,   // 0x50
  0,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1,   // 0x60
  1,1,1,1, 1,1,1,1, 1,1,1,0, 0,0,1,0,   // 0x70
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // 0x80
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // 0x90
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // 0xa0
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // 0xb0
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // 0xc0
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // 0xd0
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // 0xe0
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // 0xf0
};

/**
 *
 */
int
url_escape(char *dst, const int size, const char *src, int how)
{
  unsigned char s;
  int r = 0;
  const char *table;

  if(how == URL_ESCAPE_PATH)
    table = url_escape_path;
  else
    table = url_escape_param;

  while((s = *src++) != 0) {
    switch(table[s]) {
    case 0:
      if(r < size - 3) {
	dst[r]   = '%';
	dst[r+1] = hexchars[(s >> 4) & 0xf];
	dst[r+2] = hexchars[s & 0xf];
      }
      r+= 3;
      break;

    case 2:
      s = '+';
      // FALLTHRU
    case 1:
      if(r < size - 1)
	dst[r] = s;
      r++;
      break;
    }
  }
  if(r < size)
    dst[r] = 0;
  return r+1;
}


/* inplace decode html entities, this relies on that no entity has a
 * code point in utf8 that is more bytes then the entity string */
void
html_entities_decode(char *s)
{
  char *e;
  int code;
  char name[10];

  for(; *s; s++) {
    if(*s != '&')
      continue;
    
    e = strchr(s, ';');
    if(e == NULL)
      continue;
    
    snprintf(name, sizeof(name), "%.*s", (int)(intptr_t)(e - s - 1), s + 1);
    code = html_entity_lookup(name);
    
    if(code == -1)
      continue;

    s += utf8_put(s, code);
    
    memmove(s, e + 1, strlen(e + 1) + 1);
    s--;
  }
}

/* table from w3 tidy entities.c */
static struct html_entity
{
  const char *name;
  int code;
} html_entities[] = {
  {"quot",    34},
  {"amp",     38},
  {"apos",    39},
  {"lt",      60},
  {"gt",      62},
  {"nbsp",   160},
  {"iexcl",  161},
  {"cent",   162},
  {"pound",  163},
  {"curren", 164},
  {"yen",    165},
  {"brvbar", 166},
  {"sect",   167},
  {"uml",    168},
  {"copy",   169},
  {"ordf",   170},
  {"laquo",  171},
  {"not",    172},
  {"shy",    173},
  {"reg",    174},
  {"macr",   175},
  {"deg",    176},
  {"plusmn", 177},
  {"sup2",   178},
  {"sup3",   179},
  {"acute",  180},
  {"micro",  181},
  {"para",   182},
  {"middot", 183},
  {"cedil",  184},
  {"sup1",   185},
  {"ordm",   186},
  {"raquo",  187},
  {"frac14", 188},
  {"frac12", 189},
  {"frac34", 190},
  {"iquest", 191},
  {"Agrave", 192},
  {"Aacute", 193},
  {"Acirc",  194},
  {"Atilde", 195},
  {"Auml",   196},
  {"Aring",  197},
  {"AElig",  198},
  {"Ccedil", 199},
  {"Egrave", 200},
  {"Eacute", 201},
  {"Ecirc",  202},
  {"Euml",   203},
  {"Igrave", 204},
  {"Iacute", 205},
  {"Icirc",  206},
  {"Iuml",   207},
  {"ETH",    208},
  {"Ntilde", 209},
  {"Ograve", 210},
  {"Oacute", 211},
  {"Ocirc",  212},
  {"Otilde", 213},
  {"Ouml",   214},
  {"times",  215},
  {"Oslash", 216},
  {"Ugrave", 217},
  {"Uacute", 218},
  {"Ucirc",  219},
  {"Uuml",   220},
  {"Yacute", 221},
  {"THORN",  222},
  {"szlig",  223},
  {"agrave", 224},
  {"aacute", 225},
  {"acirc",  226},
  {"atilde", 227},
  {"auml",   228},
  {"aring",  229},
  {"aelig",  230},
  {"ccedil", 231},
  {"egrave", 232},
  {"eacute", 233},
  {"ecirc",  234},
  {"euml",   235},
  {"igrave", 236},
  {"iacute", 237},
  {"icirc",  238},
  {"iuml",   239},
  {"eth",    240},
  {"ntilde", 241},
  {"ograve", 242},
  {"oacute", 243},
  {"ocirc",  244},
  {"otilde", 245},
  {"ouml",   246},
  {"divide", 247},
  {"oslash", 248},
  {"ugrave", 249},
  {"uacute", 250},
  {"ucirc",  251},
  {"uuml",   252},
  {"yacute", 253},
  {"thorn",  254},
  {"yuml",   255},
  {"fnof",     402},
  {"Alpha",    913},
  {"Beta",     914},
  {"Gamma",    915},
  {"Delta",    916},
  {"Epsilon",  917},
  {"Zeta",     918},
  {"Eta",      919},
  {"Theta",    920},
  {"Iota",     921},
  {"Kappa",    922},
  {"Lambda",   923},
  {"Mu",       924},
  {"Nu",       925},
  {"Xi",       926},
  {"Omicron",  927},
  {"Pi",       928},
  {"Rho",      929},
  {"Sigma",    931},
  {"Tau",      932},
  {"Upsilon",  933},
  {"Phi",      934},
  {"Chi",      935},
  {"Psi",      936},
  {"Omega",    937},
  {"alpha",    945},
  {"beta",     946},
  {"gamma",    947},
  {"delta",    948},
  {"epsilon",  949},
  {"zeta",     950},
  {"eta",      951},
  {"theta",    952},
  {"iota",     953},
  {"kappa",    954},
  {"lambda",   955},
  {"mu",       956},
  {"nu",       957},
  {"xi",       958},
  {"omicron",  959},
  {"pi",       960},
  {"rho",      961},
  {"sigmaf",   962},
  {"sigma",    963},
  {"tau",      964},
  {"upsilon",  965},
  {"phi",      966},
  {"chi",      967},
  {"psi",      968},
  {"omega",    969},
  {"thetasym", 977},
  {"upsih",    978},
  {"piv",      982},
  {"bull",     8226},
  {"hellip",   8230},
  {"prime",    8242},
  {"Prime",    8243},
  {"oline",    8254},
  {"frasl",    8260},
  {"weierp",   8472},
  {"image",    8465},
  {"real",     8476},
  {"trade",    8482},
  {"alefsym",  8501},
  {"larr",     8592},
  {"uarr",     8593},
  {"rarr",     8594},
  {"darr",     8595},
  {"harr",     8596},
  {"crarr",    8629},
  {"lArr",     8656},
  {"uArr",     8657},
  {"rArr",     8658},
  {"dArr",     8659},
  {"hArr",     8660},
  {"forall",   8704},
  {"part",     8706},
  {"exist",    8707},
  {"empty",    8709},
  {"nabla",    8711},
  {"isin",     8712},
  {"notin",    8713},
  {"ni",       8715},
  {"prod",     8719},
  {"sum",      8721},
  {"minus",    8722},
  {"lowast",   8727},
  {"radic",    8730},
  {"prop",     8733},
  {"infin",    8734},
  {"ang",      8736},
  {"and",      8743},
  {"or",       8744},
  {"cap",      8745},
  {"cup",      8746},
  {"int",      8747},
  {"there4",   8756},
  {"sim",      8764},
  {"cong",     8773},
  {"asymp",    8776},
  {"ne",       8800},
  {"equiv",    8801},
  {"le",       8804},
  {"ge",       8805},
  {"sub",      8834},
  {"sup",      8835},
  {"nsub",     8836},
  {"sube",     8838},
  {"supe",     8839},
  {"oplus",    8853},
  {"otimes",   8855},
  {"perp",     8869},
  {"sdot",     8901},
  {"lceil",    8968},
  {"rceil",    8969},
  {"lfloor",   8970},
  {"rfloor",   8971},
  {"lang",     9001},
  {"rang",     9002},
  {"loz",      9674},
  {"spades",   9824},
  {"clubs",    9827},
  {"hearts",   9829},
  {"diams",    9830},
  {"OElig",   338},
  {"oelig",   339},
  {"Scaron",  352},
  {"scaron",  353},
  {"Yuml",    376},
  {"circ",    710},
  {"tilde",   732},
  {"ensp",    8194},
  {"emsp",    8195},
  {"thinsp",  8201},
  {"zwnj",    8204},
  {"zwj",     8205},
  {"lrm",     8206},
  {"rlm",     8207},
  {"ndash",   8211},
  {"mdash",   8212},
  {"lsquo",   8216},
  {"rsquo",   8217},
  {"sbquo",   8218},
  {"ldquo",   8220},
  {"rdquo",   8221},
  {"bdquo",   8222},
  {"dagger",  8224},
  {"Dagger",  8225},
  {"permil",  8240},
  {"lsaquo",  8249},
  {"rsaquo",  8250},
  {"euro",    8364},
  {NULL, 0}
};

int
html_entity_lookup(const char *name)
{
  struct html_entity *e;

  if(*name == '#') {
    if(name[1] == 'x')
      return strtol(name + 2, NULL, 16);
    return strtol(name + 1, NULL, 10);
  }

  for(e = &html_entities[0]; e->name != NULL; e++)
    if(strcmp(e->name, name) == 0)
      return e->code;

  return -1;
}


size_t
html_enteties_escape(const char *src, char *dst)
{
  size_t olen = 0;
  const char *entity = NULL;
  for(;*src;src++) {
    switch(*src) {
    case 38:
      entity = "amp";
      break;
    case 60:
      entity = "lt";
      break;
    case 62:
      entity = "gt";
      break;
    default:
      if(dst) dst[olen] = *src;
      olen++;
      continue;
    }
    if(dst) {
      dst[olen++] = '&';
      while(*entity)
	dst[olen++] = *entity++;
      dst[olen++] = ';';
    } else {
      olen += 2 + strlen(entity);
    }
  }
  if(dst)
    dst[olen] = 0;
  olen++;
  return olen;
}


/**
 * based on url_split form ffmpeg, renamed to 
 */
void 
url_split(char *proto, int proto_size,
	  char *authorization, int authorization_size,
	  char *hostname, int hostname_size,
	  int *port_ptr,
	  char *path, int path_size,
	  const char *url)
{
  const char *p, *ls, *at, *col, *brk;

  if (port_ptr)               *port_ptr = -1;
  if (proto_size > 0)         proto[0] = 0;
  if (authorization_size > 0) authorization[0] = 0;
  if (hostname_size > 0)      hostname[0] = 0;
  if (path_size > 0)          path[0] = 0;

  /* parse protocol */
  if ((p = strchr(url, ':'))) {
    snprintf(proto, MIN(proto_size, p + 1 - url), "%s", url);
    p++; /* skip ':' */
    if (*p == '/') p++;
    if (*p == '/') p++;
  } else {
    /* no protocol means plain filename */
    snprintf(path, path_size, "%s", url);
    return;
  }

  /* separate path from hostname */
  ls = strchr(p, '/');
  if(!ls)
    ls = strchr(p, '?');
  if(ls)
    snprintf(path, path_size, "%s", ls);
  else
    ls = &p[strlen(p)]; // XXX

  /* the rest is hostname, use that to parse auth/port */
  if (ls != p) {
    /* authorization (user[:pass]@hostname) */
    if ((at = strchr(p, '@')) && at < ls) {
      snprintf(authorization, MIN(authorization_size, at + 1 - p), "%s", p);
      p = at + 1; /* skip '@' */
    }

    if (*p == '[' && (brk = strchr(p, ']')) && brk < ls) {
      /* [host]:port */
      snprintf(hostname, MIN(hostname_size, brk - p), "%s", p + 1);
      if (brk[1] == ':' && port_ptr)
	*port_ptr = atoi(brk + 2);
    } else if ((col = strchr(p, ':')) && col < ls) {
      snprintf(hostname, MIN(col + 1 - p, hostname_size), "%s", p);
      if (port_ptr) *port_ptr = atoi(col + 1);
    } else
      snprintf(hostname, MIN(ls + 1 - p, hostname_size), "%s", p);
  }
}


/**
 * Strict error checking UTF-8 decoder.
 * Based on the wikipedia article http://en.wikipedia.org/wiki/UTF-8
 * Checks for these errors:
 *
 * - Bytes 192, 193 and 245 - 255 must never appear.
 *
 * - Unexpected continuation byte.
 *
 * - Start byte not followed by enough continuation bytes.
 *
 * - A sequence that decodes to a value that should use a shorter
 *   sequence (an "overlong form").
 *
 */
int
utf8_get(const char **s)
{
  uint8_t c;
  int r, l, m;

  c = **s;
  *s = *s + 1;

  switch(c) {
  case 0 ... 127:
    return c;

  case 194 ... 223:
    r = c & 0x1f;
    l = 1;
    m = 0x80;
    break;

  case 224 ... 239:
    r = c & 0xf;
    l = 2;
    m = 0x800;
    break;

  case 240 ... 247:
    r = c & 0x7;
    l = 3;
    m = 0x10000;
    break;

  case 248 ... 251:
    r = c & 0x3;
    l = 4;
    m = 0x200000;
    break;

  case 252 ... 253:
    r = c & 0x1;
    l = 5;
    m = 0x4000000;
    break;
  default:
    return 0xfffd;
  }

  while(l-- > 0) {
    c = **s;
    if((c & 0xc0) != 0x80)
      return 0xfffd;
    *s = *s + 1;
    r = r << 6 | (c & 0x3f);
  }
  if(r < m)
    return 0xfffd; // overlong sequence

  return r;
}


/**
 * Return 1 iff the string is UTF-8 conformant
 */
int
utf8_verify(const char *str)
{
  int c;

  while((c = utf8_get(&str)) != 0) {
    if(c == 0xfffd)
      return 0;
  }
  return 1;
}


/**
 *
 */
int
utf8_put(char *out, int c)
{
  if(c == 0xfffe || c == 0xffff || (c >= 0xD800 && c < 0xE000))
    return 0;
  
  if (c < 0x80) {
    if(out)
      *out = c;
    return 1;
  }

  if(c < 0x800) {
    if(out) {
      *out++ = 0xc0 | (0x1f & (c >>  6));
      *out   = 0x80 | (0x3f &  c);
    }
    return 2;
  }

  if(c < 0x10000) {
    if(out) {
      *out++ = 0xe0 | (0x0f & (c >> 12));
      *out++ = 0x80 | (0x3f & (c >> 6));
      *out   = 0x80 | (0x3f &  c);
    }
    return 3;
  }

  if(c < 0x200000) {
    if(out) {
      *out++ = 0xf0 | (0x07 & (c >> 18));
      *out++ = 0x80 | (0x3f & (c >> 12));
      *out++ = 0x80 | (0x3f & (c >> 6));
      *out   = 0x80 | (0x3f &  c);
    }
    return 4;
  }
  
  if(c < 0x4000000) {
    if(out) {
      *out++ = 0xf8 | (0x03 & (c >> 24));
      *out++ = 0x80 | (0x3f & (c >> 18));
      *out++ = 0x80 | (0x3f & (c >> 12));
      *out++ = 0x80 | (0x3f & (c >>  6));
      *out++ = 0x80 | (0x3f &  c);
    }
    return 5;
  }

  if(out) {
    *out++ = 0xfc | (0x01 & (c >> 30));
    *out++ = 0x80 | (0x3f & (c >> 24));
    *out++ = 0x80 | (0x3f & (c >> 18));
    *out++ = 0x80 | (0x3f & (c >> 12));
    *out++ = 0x80 | (0x3f & (c >>  6));
    *out++ = 0x80 | (0x3f &  c);
  }
  return 6;
}


/**
 *
 */
char *
utf8_cleanup(const char *str)
{
  const char *s = str; 
  int outlen = 1;
  int c;
  int bad = 0;
  while((c = utf8_get(&s)) != 0) {
    if(c == 0xfffd)
      bad = 1;
    outlen += utf8_put(NULL, c);
  }

  if(!bad)
    return NULL;

  char *out = malloc(outlen);
  char *ret = out;
  while((c = utf8_get(&str)) != 0)
    out += utf8_put(out, c);

  *out = 0;
  return ret;
}


/**
 *
 */
char *
utf8_from_bytes(const char *str, int len, const charset_t *cs,
		char *how, size_t howlen)
{
  char *r, *d;
  len = !len ? strlen(str) : len;

  if(cs == NULL) {
    cs = i18n_get_default_charset();
    if(cs == NULL) {
      const char *lang = NULL;
      const char *name = charset_detector(str, len, &lang);
      if(name != NULL) {
	cs = charset_get(name);
	if(cs != NULL)
	  snprintf(how, howlen, "Decoded as %s (detected language: %s)",
		   name, lang);
	else
	  TRACE(TRACE_ERROR, "STR", "Language %s not found internally",
		name);
      }
    } else {
      snprintf(how, howlen, "Decoded as %s (specified by user)",
	       cs->title);
    }
  } else {
    snprintf(how, howlen, "Decoded as %s (specified by request)",
	     cs->title);
  }

  const uint16_t *cp = cs ? cs->ptr : NULL;

  int i, olen = 0;
  for(i = 0; i < len; i++) {
    if(str[i] == 0)
      break;
    olen += utf8_put(NULL, cp ? cp[(uint8_t)str[i]] : str[i]);
  }
  d = r = malloc(olen + 1);
  for(i = 0; i < len; i++) {
    if(str[i] == 0)
      break;
    d += utf8_put(d, cp ? cp[(uint8_t)str[i]] : str[i]);
  }
  *d = 0;
  return r;
}


static uint16_t *casefoldtable;
static int casefoldtablelen;

/**
 *
 */
static int
unicode_casefold(unsigned int i)
{
  int r;
  if(i < casefoldtablelen) {
    r = casefoldtable[i];
    if(r)
      return r;
  }
  return i;
}


/**
 *
 */
static const char *
no_the(const char *a)
{
  if(!gconf.ignore_the_prefix)
    return a;

  if(a[0] != 't' && a[0] != 'T')
    return a;
  if(a[1] != 'h' && a[1] != 'H')
    return a;
  if(a[2] != 'e' && a[2] != 'E')
    return a;

  int i = 3;
  while(a[i] == ' ' || a[i] == '.')
    i++;
  return a + i;
}

/**
 *
 */
int
dictcmp(const char *a, const char *b)
{
  long int da, db;
  int ua, ub;

  a = no_the(a);
  b = no_the(b);


  while(1) {

    ua = utf8_get(&a);
    ub = utf8_get(&b);

    switch((ua >= '0' && ua <= '9' ? 1 : 0)|(ub >= '0' && ub <= '9' ? 2 : 0)) {
    case 0:  /* 0: a is not a digit, nor is b */

      if(ua != ub) {
	ua = unicode_casefold(ua);
	ub = unicode_casefold(ub);
	if(ua != ub)
	  return ua - ub;
      }
      if(ua == 0)
	return 0;
      break;
    case 1:  /* 1: a is a digit,  b is not */
    case 2:  /* 2: a is not a digit,  b is */
	return ua - ub;
    case 3:  /* both are digits, switch to integer compare */
      da = ua - '0';
      db = ub - '0';

      while(*a >= '0' && *a <= '9')
	da = da * 10L + *a++ - '0';
      while(*b >= '0' && *b <= '9')
	db = db * 10L + *b++ - '0';
      if(da != db)
	return da - db;
      break;
    }
  }
}


/**
 *
 */
const char *
mystrstr(const char *haystack, const char *needle)
{
  int h, n;
  const char *h1, *n1, *r;

  n = unicode_casefold(utf8_get(&needle));
    
  while(1) {
    r = haystack;
    h = unicode_casefold(utf8_get(&haystack));
    if(h == 0)
      return NULL;

    if(n == h) {
      h1 = haystack;
      n1 = needle;

      while(1) {
	n = unicode_casefold(utf8_get(&n1));
	if(n == 0)
	  return r;
	h = unicode_casefold(utf8_get(&h1));
	if(n != h)
	  break;
      }
    }
  }
}



/**
 *
 */
void
unicode_init(void)
{
  int i;
  int n = sizeof(unicode_casefolding) / 4;
  int x = unicode_casefolding[(n * 2) - 1];
  casefoldtablelen = x;
  casefoldtable = calloc(1, sizeof(uint16_t) * casefoldtablelen);

  for(i = 0 ; i < n; i++) {
    uint16_t from, to;
    from = unicode_casefolding[i * 2 + 0];
    to   = unicode_casefolding[i * 2 + 1];
    casefoldtable[from] = to;
  }
}





/**
 *
 */
char **
strvec_split(const char *str, char ch)
{
  const char *s;
  int c = 1, i;
  char **r;

  for(s = str; *s != 0; s++)
    if(*s == ch)
      c++;

  r = malloc(sizeof(char *) * (c + 1));
  for(i = 0; i < c; i++) {
    s = strchr(str, ch);
    if(s == NULL) {
      assert(i == c - 1);
      r[i] = strdup(str);
    } else {
      r[i] = malloc(s - str + 1);
      memcpy(r[i], str, s - str);
      r[i][s - str] = 0;
      str = s + 1;
    }
  }
  r[i] = NULL;
  return r;
}


/**
 *
 */
void
strvec_free(char **s)
{
  void *m = s;
  for(;*s != NULL; s++)
    free(*s);
  free(m);
}


/**
 *
 */
void 
strvec_addpn(char ***strvp, const char *v, size_t len)
{
  char **strv = *strvp;
  int i = 0;
  if(strv == NULL) {
    strv = malloc(sizeof(char *) * 2);
  } else {
    while(strv[i] != NULL)
      i++;
    strv = realloc(strv, sizeof(char *) * (i + 2));
  }
  strv[i] = memcpy(malloc(len + 1), v, len);
  strv[i][len] = 0;
  strv[i+1] = NULL;
  *strvp = strv;
}

/**
 *
 */
void 
strvec_addp(char ***strvp, const char *v)
{
  strvec_addpn(strvp, v, strlen(v));
}


/**
 *
 */
void
strappend(char **strp, const char *src)
{
  if(*strp == NULL)
    *strp = strdup(src);
  else {
    size_t a = strlen(*strp);
    size_t b = strlen(src);
    *strp = realloc(*strp, a + b + 1);
    memcpy(*strp + a, src, b + 1);
  }
}


/**
 *
 */
int
hex2bin(uint8_t *buf, size_t buflen, const char *str)
{
  int hi, lo;
  size_t bl = buflen;
  while(*str) {
    if(buflen == 0)
      return -1;
    if((hi = hexnibble(*str++)) == -1)
      return -1;
    if((lo = hexnibble(*str++)) == -1)
      return -1;

    *buf++ = hi << 4 | lo;
    buflen--;
  }
  return bl - buflen;
}


/**
 *
 */
void
bin2hex(char *dst, size_t dstlen, const uint8_t *src, size_t srclen)
{
  while(dstlen > 2 && srclen > 0) {
    *dst++ = "0123456789abcdef"[*src >> 4];
    *dst++ = "0123456789abcdef"[*src & 0xf];
    src++;
    srclen--;
    dstlen -= 2;
  }
  *dst = 0;
}


/**
 * Create URL using ref referred from base
 */
char *
url_resolve_relative(const char *proto, const char *hostname, int port,
		     const char *path, const char *ref)
{
  char out[512];
  int l;
  
  if(strstr(ref, "://"))
    return strdup(ref);

  if(port != -1)
    l = snprintf(out, sizeof(out), "%s://%s:%d", proto, hostname, port);
  else
    l = snprintf(out, sizeof(out), "%s://%s", proto, hostname);
  

  if(*ref != '/') {

    const char *r = strrchr(path, '/');
    if(r != NULL) {
      size_t l2 = r + 1 - path;

      if(l2 + l > sizeof(out) - 1)
	return NULL;

      memcpy(out + l, path, l2);
      l += l2;

      size_t l3 = strlen(ref) + 1;
      if(l3 + l > sizeof(out) - 1)
	return NULL;
      
      memcpy(out + l, ref, l3);
      return strdup(out);
    }
  }
  snprintf(out + l, sizeof(out) - l, "%s", ref); 
  return strdup(out);
}


/**
 * Create URL using ref referred from base
 */
char *
url_resolve_relative_from_base(const char *base, const char *url)
{
  char proto[20];
  char hostname[200];
  char path[200];
  int port;

  url_split(proto, sizeof(proto), NULL, 0, hostname, sizeof(hostname),
	    &port, path, sizeof(path), base);

  return url_resolve_relative(proto, hostname, port, path, url);
}


/**
 *
 */
int
hexnibble(char c)
{
  switch(c) {
  case '0' ... '9':    return c - '0';
  case 'a' ... 'f':    return c - 'a' + 10;
  case 'A' ... 'F':    return c - 'A' + 10;
  default:
    return -1;
  }
}

// ISO-8859-X  ->  UTF-8

#define ISO_8859_1 NULL
extern const uint16_t ISO_8859_2[];
extern const uint16_t ISO_8859_3[];
extern const uint16_t ISO_8859_4[];
extern const uint16_t ISO_8859_5[];
extern const uint16_t ISO_8859_6[];
extern const uint16_t ISO_8859_7[];
extern const uint16_t ISO_8859_8[];
extern const uint16_t ISO_8859_9[];
extern const uint16_t ISO_8859_10[];
extern const uint16_t ISO_8859_11[];
extern const uint16_t ISO_8859_13[];
extern const uint16_t ISO_8859_14[];
extern const uint16_t ISO_8859_15[];
extern const uint16_t ISO_8859_16[];
extern const uint16_t CP1250[];
extern const uint16_t CP1251[];
extern const uint16_t CP1252[];
extern const uint16_t CP1253[];
extern const uint16_t CP1254[];
extern const uint16_t CP1255[];
extern const uint16_t CP1256[];
extern const uint16_t CP1257[];
extern const uint16_t CP1258[];

#define ALIAS(x...) (const char *[]){x, NULL}

const static charset_t charsets[] = {
   // ISO-8869-1 must be first
  {"ISO-8859-1", "ISO-8859-1 (Latin-1)", ISO_8859_1,
   ALIAS("iso-ir-100",
         "ISO_8859-1",
         "latin1",
         "l1",
         "IBM819",
         "CP819",
         "csISOLatin1")},
  {"ISO-8859-2", "ISO-8859-2 (Latin-2)", ISO_8859_2,
   ALIAS("iso-ir-101",
         "ISO_8859-2",
         "latin2",
         "l2",
         "csISOLatin2")},
  {"ISO-8859-3", "ISO-8859-3 (Latin-3)", ISO_8859_3,
   ALIAS("iso-ir-109",
         "ISO_8859-3",
         "latin3",
         "l3",
         "csISOLatin3")},
  {"ISO-8859-4", "ISO-8859-4 (Latin-4)", ISO_8859_4,
   ALIAS("iso-ir-110",
         "ISO_8859-4",
         "latin4",
         "l4",
         "csISOLatin4")},
  {"ISO-8859-5", "ISO-8859-5 (Latin/Cyrillic)", ISO_8859_5,
   ALIAS("iso-ir-144",
         "ISO_8859-5",
         "cyrillic",
         "csISOLatinCyrillic")},
  {"ISO-8859-6", "ISO-8859-6 (Latin/Arabic)", ISO_8859_6,
   ALIAS("iso-ir-127",
         "ISO_8859-6",
         "ECMA-114",
         "ASMO-708",
         "arabic",
         "csISOLatinArabic")},
  {"ISO-8859-7", "ISO-8859-7 (Latin/Greek)", ISO_8859_7,
   ALIAS("iso-ir-126",
         "ISO_8859-7",
         "ELOT_928",
         "ECMA-118",
         "greek",
         "greek8",
         "csISOLatinGreek")},
  {"ISO-8859-8", "ISO-8859-8 (Latin/Hebrew)", ISO_8859_8,
   ALIAS("iso-ir-138",
         "ISO_8859-8",
         "hebrew",
         "csISOLatinHebrew")},
  {"ISO-8859-9", "ISO-8859-9 (Latin-5/Turkish)", ISO_8859_9,
   ALIAS("iso-ir-148",
         "ISO_8859-9",
         "latin5",
         "l5",
         "csISOLatin5")},
  {"ISO-8859-10", "ISO-8859-10 (Latin-6)", ISO_8859_10,
   ALIAS("iso-ir-157",
         "l6",
         "ISO_8859-10:1992",
         "csISOLatin6",
         "latin6")},
  {"ISO-8859-11", "ISO-8859-11 (Latin/Thai)", ISO_8859_11},
  {"ISO-8859-13", "ISO-8859-13 (Baltic Rim)", ISO_8859_13},
  {"ISO-8859-14", "ISO-8859-14 (Celtic)", ISO_8859_14},
  {"ISO-8859-15", "ISO-8859-15 (Latin-9)", ISO_8859_15},
  {"ISO-8859-16", "ISO-8859-16 (Latin-10)", ISO_8859_16},
  {"CP1250", "Windows 1250", CP1250, ALIAS("windows-1250", "cswindows1250")},
  {"CP1251", "Windows 1251", CP1251, ALIAS("windows-1251", "cswindows1251")},
  {"CP1252", "Windows 1252", CP1252, ALIAS("windows-1252", "cswindows1252")},
  {"CP1253", "Windows 1253", CP1253, ALIAS("windows-1253", "cswindows1253")},
  {"CP1254", "Windows 1254", CP1254, ALIAS("windows-1254", "cswindows1254")},
  {"CP1255", "Windows 1255", CP1255, ALIAS("windows-1255", "cswindows1255")},
  {"CP1256", "Windows 1256", CP1256, ALIAS("windows-1256", "cswindows1256")},
  {"CP1257", "Windows 1257", CP1257, ALIAS("windows-1257", "cswindows1257")},
  {"CP1258", "Windows 1258", CP1258, ALIAS("windows-1258", "cswindows1258")},
};

#undef ALIAS

const charset_t *
charset_get_idx(unsigned int i)
{
  if(i < sizeof(charsets) / sizeof(charsets[0]))
    return &charsets[i];
  return NULL;
}

const charset_t *
charset_get(const char *id)
{
  int i;
  const char **a;
  if(id == NULL)
    return &charsets[0];

  for(i = 0; i < sizeof(charsets) / sizeof(charsets[0]); i++) {
    if(!strcasecmp(id, charsets[i].id))
      return &charsets[i];
    a = charsets[i].aliases;
    if(a == NULL)
      continue;
    while(*a) {
      if(!strcasecmp(id, *a))
        return &charsets[i];
      a++;
    }
  }

  return NULL;
}

/**
 *
 */
const char *
charset_get_name(const void *p)
{
  int i;
  for(i = 0; i < sizeof(charsets) / sizeof(charsets[0]); i++)
    if(p == charsets[i].ptr)
      return charsets[i].title;
  return "???";
}

/**
 *
 */
void
ucs2_to_utf8(uint8_t *dst, size_t dstlen, const uint8_t *src, size_t srclen,
	     int le)
{
  int c, r;
  while(dstlen > 3 && srclen >= 2) {
    c = le ? (src[0] | src[1] << 8) : (src[1] | src[0] << 8);
    if(c == 0)
      break;
    src += 2;
    srclen -= 2;
    r = utf8_put((char *)dst, c);
    dst += r;
    dstlen -= r;
  }
  *dst = 0;
}


/**
 *
 */
size_t
utf8_to_ucs2(uint8_t *dst, const char *src, int le)
{
  int c;
  size_t o = 0;
  while((c = utf8_get(&src)) != 0) {
    if(c > 0xffff)
      return -1;
    
    if(dst != NULL) {
      if(le) {
	dst[o] = c;
	dst[o+1] = c >> 8;
      } else {
	dst[o] = c >> 8;
	dst[o+1] = c;
      }
    }
    o+=2;
  }
  if(dst != NULL) {
    dst[o] = 0;
    dst[o+1] = 0;
  }
  o+=2;
  return o;
}


/**
 *
 */
size_t
utf8_to_ascii(uint8_t *dst, const char *src)
{
  int c;
  size_t o = 0;
  while((c = utf8_get(&src)) != 0) {
    if(c > 0xff)
      return -1;
    
    if(dst != NULL) {
      dst[o] = c;
    }
    o+=1;
  }
  if(dst != NULL) {
    dst[o] = 0;
  }
  o+=1;
  return o;
}



/**
 *
 */
uint32_t
html_makecolor(const char *str)
{
  uint8_t r, g, b;
  if(*str == '#')
    str++;
  if(strlen(str) == 3) {
    r = hexnibble(str[0]) * 0x11;
    g = hexnibble(str[1]) * 0x11;
    b = hexnibble(str[2]) * 0x11;
  } else if(strlen(str) == 6) {
    r = (hexnibble(str[0]) << 4) | hexnibble(str[1]);
    g = (hexnibble(str[2]) << 4) | hexnibble(str[3]);
    b = (hexnibble(str[4]) << 4) | hexnibble(str[5]);
  } else
    return 0;
  return b << 16  | g << 8 | r;
}


/**
 *
 */
void
utf16_to_utf8(char **bufp, size_t *lenp)
{
  void *freeme = *bufp;
  const char *src = *bufp;
  size_t len = *lenp;
  int le = 0;
  if(len < 2)
    return;

  if(src[0] == 0xff && src[1] == 0xfe) {
    le = 1;
    src += 2;
    len -= 2;
  } else if(src[0] == 0xfe && src[1] == 0xff) {
    src += 2;
    len -= 2;
  }

  const char *src2 = src;
  size_t len2 = len;

  int olen = 1;
  while(len >= 2) {
    int c = src[!le] | src[le] << 8;
    olen += utf8_put(NULL, c);
    src += 2;
    len -= 2;
  }
  freeme = *bufp;
  *lenp = olen - 1;
  char *o2 = *bufp = malloc(olen);
  while(len2 >= 2) {
    int c = src2[!le] | src2[le] << 8;
    o2 += utf8_put(o2, c);
    src2 += 2;
    len2 -= 2;
  }
  *o2++ = 0;
  assert(o2 == *bufp + olen);
  free(freeme);
}


/**
 *
 */
rstr_t *
get_random_string(void)
{
  sha1_decl(shactx);
  uint8_t d[20];
  char buf[40];
  static uint64_t seed;
  int i;
  seed ^= showtime_get_ts();

  sha1_init(shactx);
  sha1_update(shactx, (void *)&seed, sizeof(uint64_t));
  sha1_final(shactx, d);
  memcpy(&seed, d, sizeof(uint64_t));

  for(i = 0; i < 20; i++) {
    buf[i * 2 + 0] = "0123456789abcdef"[d[i] & 0xf];
    buf[i * 2 + 1] = "0123456789abcdef"[d[i] >> 4];
  }
  return rstr_allocl(buf, 40);
}


/**
 *
 */
char *
lp_get(char **lp)
{
  char *r;
  do {
    if(*lp == NULL)
      return NULL;
    r = *lp;
    int l = strcspn(r, "\r\n");
     if(r[l] == 0) {
      *lp = NULL;
    } else {
      r[l] = 0;
      char *s = r + l + 1;
      while(*s == '\r' || *s == '\n')
        s++;
      *lp = s;
    }
  } while(*r == 0);
  return r;
}

