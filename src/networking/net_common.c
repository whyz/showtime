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

#include <stdlib.h>

#include <sys/socket.h>

#include "misc/minmax.h"
#include "net_i.h"

/**
 *
 */
int
tcp_write_queue(tcpcon_t *tc, htsbuf_queue_t *q)
{
  htsbuf_data_t *hd;
  int l, r = 0;

  while((hd = TAILQ_FIRST(&q->hq_q)) != NULL) {
    TAILQ_REMOVE(&q->hq_q, hd, hd_link);

    l = hd->hd_data_len - hd->hd_data_off;
    r |= tc->write(tc, hd->hd_data + hd->hd_data_off, l);
    free(hd->hd_data);
    free(hd);
  }
  q->hq_size = 0;
  return 0;
}


/**
 *
 */
int
tcp_write_queue_dontfree(tcpcon_t *tc, htsbuf_queue_t *q)
{
  htsbuf_data_t *hd;
  int l, r = 0;

  TAILQ_FOREACH(hd, &q->hq_q, hd_link) {
    l = hd->hd_data_len - hd->hd_data_off;
    r |= tc->write(tc, hd->hd_data + hd->hd_data_off, l);
  }
  return 0;
}


/**
 *
 */
void
tcp_printf(tcpcon_t *tc, const char *fmt, ...)
{
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  int len = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  tc->write(tc, buf, len);
}


/**
 *
 */
static int
tcp_read_into_spill(tcpcon_t *tc)
{
  htsbuf_queue_t *hq = &tc->spill;
  htsbuf_data_t *hd = TAILQ_LAST(&hq->hq_q, htsbuf_data_queue);
  int c;

  if(hd != NULL) {
    /* Fill out any previous buffer */
    c = hd->hd_data_size - hd->hd_data_len;

    if(c > 0) {

      if((c = tc->read(tc, hd->hd_data + hd->hd_data_len, c, 0, NULL, 0)) < 0)
	return -1;

      hd->hd_data_len += c;
      hq->hq_size += c;
      return 0;
    }
  }
  
  hd = malloc(sizeof(htsbuf_data_t));
  
  hd->hd_data_size = 1000;
  hd->hd_data = malloc(hd->hd_data_size);

  if((c = tc->read(tc, hd->hd_data, hd->hd_data_size, 0, NULL, 0)) < 0) {
    free(hd->hd_data);
    free(hd);
    return -1;
  }
  hd->hd_data_len = c;
  hd->hd_data_off = 0;
  TAILQ_INSERT_TAIL(&hq->hq_q, hd, hd_link);
  hq->hq_size += c;
  return 0;
}


/**
 *
 */
int
tcp_read_line(tcpcon_t *tc, char *buf,const size_t bufsize)
{
  int len;

  while(1) {
    len = htsbuf_find(&tc->spill, 0xa);

    if(len == -1) {
      if(tcp_read_into_spill(tc) < 0)
	return -1;
      continue;
    }
    
    if(len >= bufsize - 1)
      return -1;

    htsbuf_read(&tc->spill, buf, len);
    buf[len] = 0;
    while(len > 0 && buf[len - 1] < 32)
      buf[--len] = 0;
    htsbuf_drop(&tc->spill, 1); /* Drop the \n */
    return 0;
  }
}


/**
 *
 */
int
tcp_read_data(tcpcon_t *tc, void *buf, size_t bufsize,
	      net_read_cb_t *cb, void *opaque)
{
  int r = buf ? htsbuf_read(&tc->spill, buf, bufsize) :
    htsbuf_drop(&tc->spill, bufsize);
  if(r == bufsize)
    return 0;

  if(buf != NULL)
    return tc->read(tc, buf + r, bufsize - r, 1, cb, opaque) < 0 ? -1 : 0;

  size_t remain = bufsize - r;

  buf = malloc(5000);

  while(remain > 0) {
    size_t n = MIN(remain, 5000);
    r = tc->read(tc, buf, n, 1, NULL, 0) < 0 ? -1 : 0;
    if(r != 0)
      break;
    remain -= n;
  }

  free(buf);
  return r;
}


/**
 *
 */
int
tcp_read_data_nowait(tcpcon_t *tc, char *buf, const size_t bufsize)
{
  int tot = htsbuf_read(&tc->spill, buf, bufsize);

  if(tot > 0)
    return tot;

  return tc->read(tc, buf + tot, bufsize - tot, 0, NULL, NULL);
}


#include <netinet/in.h>

/**
 *
 */
static void
net_addr_from_sockaddr_in(net_addr_t *na, const struct sockaddr_in *sin)
{
  na->na_family = 4;
  na->na_port = ntohs(sin->sin_port);
  memcpy(na->na_addr, &sin->sin_addr, 4);
}


/**
 *
 */
void
net_local_addr_from_fd(net_addr_t *na, int fd)
{
  socklen_t slen = sizeof(struct sockaddr_in);
  struct sockaddr_in self;

  if(!getsockname(fd, (struct sockaddr *)&self, &slen)) {
    net_addr_from_sockaddr_in(na, &self);
  } else {
    memset(na, 0, sizeof(net_addr_t));
  }
}


/**
 *
 */
void
net_remote_addr_from_fd(net_addr_t *na, int fd)
{
  socklen_t slen = sizeof(struct sockaddr_in);
  struct sockaddr_in self;

  if(!getpeername(fd, (struct sockaddr *)&self, &slen)) {
    net_addr_from_sockaddr_in(na, &self);
  } else {
    memset(na, 0, sizeof(net_addr_t));
  }
}


/**
 *
 */
void
net_fmt_host(char *dst, size_t dstlen, const net_addr_t *na)
{
  switch(na->na_family) {
  case 4:
    snprintf(dst, dstlen, "%d.%d.%d.%d",
             na->na_addr[0],
             na->na_addr[1],
             na->na_addr[2],
             na->na_addr[3]);
    break;

  default:
    if(dstlen)
      *dst = 0;
  }
}


/**
 *
 */
int
tcp_write_data(tcpcon_t *tc, const char *buf, const size_t bufsize)
{
  return tc->write(tc, buf, bufsize);
}


/**
 *
 */
int
tcp_get_fd(const tcpcon_t *tc)
{
  return tc->fd;
}
