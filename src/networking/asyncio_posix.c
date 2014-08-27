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


#include <assert.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <netinet/in.h>

#include "showtime.h"
#include "arch/arch.h"
#include "arch/threads.h"
#include "asyncio.h"
#include "misc/queue.h"
#include "prop/prop.h"
#include "misc/minmax.h"
#include "fileaccess/fileaccess.h"

LIST_HEAD(asyncio_fd_list, asyncio_fd);
LIST_HEAD(asyncio_worker_list, asyncio_worker);
LIST_HEAD(asyncio_timer_list, asyncio_timer);
TAILQ_HEAD(asyncio_dns_req_queue, asyncio_dns_req);
LIST_HEAD(asyncio_http_req_list, asyncio_http_req);

static hts_thread_t asyncio_thread_id;

static struct asyncio_timer_list asyncio_timers;

static hts_mutex_t asyncio_worker_mutex;
static struct asyncio_worker_list asyncio_workers;

static int asyncio_pipe[2];
static struct asyncio_fd_list asyncio_fds;
static int asyncio_num_fds;

struct prop_courier *asyncio_courier;

static hts_mutex_t asyncio_dns_mutex;
static int asyncio_dns_worker;
static struct asyncio_dns_req_queue asyncio_dns_pending;
static struct asyncio_dns_req_queue asyncio_dns_completed;

static hts_mutex_t asyncio_http_mutex;
static int asyncio_http_worker;
static struct asyncio_http_req_list asyncio_http_completed;

static void adr_deliver_cb(void);

static void ahr_deliver_cb(void);

int64_t async_now;

static __inline void asyncio_verify_thread(void) {
  assert(hts_thread_current() == asyncio_thread_id);
}

/**
 *
 */
typedef struct asyncio_worker {
  LIST_ENTRY(asyncio_worker) link;
  void (*fn)(void);
  int id;
  int pending;
} asyncio_worker_t;


/**
 *
 */
struct asyncio_fd {
  LIST_ENTRY(asyncio_fd) af_link;
  asyncio_fd_callback_t *af_callback;
  void *af_opaque;
  char *af_name;
  union {
    asyncio_accept_callback_t *af_accept_callback;
    asyncio_udp_callback_t    *af_udp_callback;
    asyncio_error_callback_t  *af_error_callback;
  };


  asyncio_read_callback_t *af_read_callback;

  htsbuf_queue_t af_sendq;
  htsbuf_queue_t af_recvq;

  int64_t af_timeout;

  int af_refcount;
  int af_fd;
  int af_poll_events;
  int af_pending_errno;

  uint16_t af_port;
  uint16_t af_ext_events;
  uint8_t af_connected;
};


static void
no_sigpipe(int fd)
{
#ifdef SO_NOSIGPIPE
  int val = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &val, sizeof(val));
#endif
}


/**
 *
 */
static void
asyncio_wakeup(int id)
{
  char x = id;
  if(write(asyncio_pipe[1], &x, 1) != 1)
    TRACE(TRACE_ERROR, "TCP", "Pipe problems");
}

/**
 *
 */
static void
asyncio_courier_notify(void *opaque)
{
  asyncio_wakeup(0);
}


/**
 *
 */
void
asyncio_wakeup_worker(int id)
{
  return asyncio_wakeup(id);
}

/**
 *
 */
void
asyncio_timer_init(asyncio_timer_t *at, void (*fn)(void *opaque),
		   void *opaque)
{
  at->at_fn = fn;
  at->at_opaque = opaque;
  at->at_expire = 0;
}


/**
 *
 */
static int
at_compar(const asyncio_timer_t *a, const asyncio_timer_t *b)
{
  if(a->at_expire < b->at_expire)
    return -1;
  return 1;
}


/**
 *
 */
void
asyncio_timer_arm(asyncio_timer_t *at, int64_t expire)
{
  asyncio_verify_thread();
  if(at->at_expire)
    LIST_REMOVE(at, at_link);

  at->at_expire = expire;
  LIST_INSERT_SORTED(&asyncio_timers, at, at_link, at_compar, asyncio_timer_t);
}


/**
 *
 */
void
asyncio_timer_disarm(asyncio_timer_t *at)
{
  asyncio_verify_thread();
  if(at->at_expire) {
    LIST_REMOVE(at, at_link);
    at->at_expire = 0;
  }
}


/**
 *
 */
static void
af_release(asyncio_fd_t *af)
{
  asyncio_verify_thread();
  af->af_refcount--;
  if(af->af_refcount > 0)
    return;
  htsbuf_queue_flush(&af->af_recvq);
  htsbuf_queue_flush(&af->af_sendq);
  free(af->af_name);
  free(af);
}

/**
 *
 */
static void
asyncio_dopoll(void)
{
  asyncio_timer_t *at;

  while((at = LIST_FIRST(&asyncio_timers)) != NULL &&
        at->at_expire <= async_now) {
    LIST_REMOVE(at, at_link);
    at->at_expire = 0;
    at->at_fn(at->at_opaque);
  }

  asyncio_fd_t *af;
  struct pollfd *fds = alloca(asyncio_num_fds * sizeof(struct pollfd));
  asyncio_fd_t **afds  = alloca(asyncio_num_fds * sizeof(asyncio_fd_t *));
  int n = 0;

  int timeout = INT32_MAX;

  LIST_FOREACH(af, &asyncio_fds, af_link) {
    if(af->af_pending_errno) {
      af->af_callback(af, af->af_opaque, ASYNCIO_ERROR, af->af_pending_errno);
      goto release;
    }

    if(af->af_timeout) {
      if(af->af_timeout <= async_now) {
        af->af_timeout = 0;
        af->af_callback(af, af->af_opaque, ASYNCIO_TIMEOUT, 0);
        goto release;
      }
      timeout = MIN(timeout, (af->af_timeout - async_now + 999) / 1000);
    }
  
    fds[n].fd = af->af_fd;
    fds[n].events = af->af_poll_events;
    fds[n].revents = 0;
    afds[n] = af;

    af->af_refcount++;
    n++;
  }

  assert(n == asyncio_num_fds);

  if((at = LIST_FIRST(&asyncio_timers)) != NULL)
    timeout = MIN(timeout, (at->at_expire - async_now + 999) / 1000);

  if(timeout == INT32_MAX)
    timeout = -1;

  poll(fds, n, timeout);

  async_now = showtime_get_ts();

  for(int i = 0; i < n; i++) {
    af = afds[i];

    if(!(af->af_callback && fds[i].revents))
      continue;

    if(fds[i].revents & POLLHUP) {
      af->af_callback(af, af->af_opaque, ASYNCIO_ERROR, ECONNRESET);
      continue;
    }

    if(fds[i].revents & POLLERR) {
      int err;
      socklen_t errlen = sizeof(int);
    
      getsockopt(af->af_fd, SOL_SOCKET, SO_ERROR, (void *)&err, &errlen);
      af->af_callback(af, af->af_opaque, ASYNCIO_ERROR, err);
      continue;
    }

    af->af_callback(af,
                    af->af_opaque,
                    (fds[i].revents & POLLIN  ? ASYNCIO_READ  : 0) |
                    (fds[i].revents & POLLOUT ? ASYNCIO_WRITE : 0), 0);

    if(0) {
      int64_t now = showtime_get_ts();

      if(now - async_now > 10000) {
        TRACE(TRACE_ERROR, "ASYNCIO", "Long callback on socktet %s (%d µs)",
              af->af_name, (int) (now - async_now));
      }
      async_now = now;
    }
  }


 release:

  for(int i = 0; i < n; i++)
    af_release(afds[i]);
}


/**
 *
 */
void
asyncio_set_events(asyncio_fd_t *af, int events)
{
  asyncio_verify_thread();
  af->af_ext_events = events;

  af->af_poll_events =
    (events & ASYNCIO_READ  ? POLLIN            : 0) |
    (events & ASYNCIO_WRITE ? POLLOUT           : 0) |
    (events & ASYNCIO_ERROR ? (POLLHUP|POLLERR) : 0);
}


/**
 *
 */
void
asyncio_add_events(asyncio_fd_t *af, int events)
{
  asyncio_set_events(af, af->af_ext_events | events);
}


/**
 *
 */
void
asyncio_rem_events(asyncio_fd_t *af, int events)
{
  asyncio_set_events(af, af->af_ext_events & ~events);
}


/**
 *
 */
asyncio_fd_t *
asyncio_add_fd(int fd, int events, asyncio_fd_callback_t *cb, void *opaque,
	       const char *name)
{
  asyncio_verify_thread();
  asyncio_fd_t *af = calloc(1, sizeof(asyncio_fd_t));
  htsbuf_queue_init(&af->af_recvq, INT32_MAX);
  htsbuf_queue_init(&af->af_sendq, INT32_MAX);
  af->af_refcount = 1;
  af->af_fd = fd;
  af->af_name = strdup(name);
  asyncio_set_events(af, events);
  af->af_callback = cb;
  af->af_opaque = opaque;

  net_change_nonblocking(fd, 1);

  LIST_INSERT_HEAD(&asyncio_fds, af, af_link);
  asyncio_num_fds++;
  return af;
}


/**
 *
 */
void
asyncio_del_fd(asyncio_fd_t *af)
{
  asyncio_verify_thread();
  if(af->af_fd != -1)
    close(af->af_fd);
  af->af_fd = -1;
  LIST_REMOVE(af, af_link);
  asyncio_num_fds--;
  af->af_callback = NULL;
  af_release(af);
}


/**
 *
 */
void
asyncio_set_timeout(asyncio_fd_t *af, int64_t timeout)
{
  af->af_timeout = timeout;
}

/**
 *
 */
static void
asyncio_handle_pipe(asyncio_fd_t *af, void *opaque, int event, int error)
{
  char x;
  if(read(asyncio_pipe[0], &x, 1) != 1)
    return;

  if(x == 0) {
    prop_courier_poll(opaque);
    return;
  }

  asyncio_worker_t *aw;
  hts_mutex_lock(&asyncio_worker_mutex);

  LIST_FOREACH(aw, &asyncio_workers, link)
    if(aw->id == x)
      break;

  hts_mutex_unlock(&asyncio_worker_mutex);

  if(aw != NULL)
    aw->fn();
}


/**
 *
 */
int
asyncio_add_worker(void (*fn)(void))
{
  asyncio_worker_t *aw = calloc(1, sizeof(asyncio_worker_t));

  aw->fn = fn;

  static  int generator;

  hts_mutex_lock(&asyncio_worker_mutex);
  generator++;
  aw->id = generator;
  LIST_INSERT_HEAD(&asyncio_workers, aw, link);
  hts_mutex_unlock(&asyncio_worker_mutex);
  return aw->id;
}



/**
 *
 */
static void *
asyncio_thread(void *aux)
{
  hts_mutex_init(&asyncio_worker_mutex);

  arch_pipe(asyncio_pipe);

  asyncio_courier = prop_courier_create_notify(asyncio_courier_notify, NULL);

  asyncio_add_fd(asyncio_pipe[0], ASYNCIO_READ, asyncio_handle_pipe,
                 asyncio_courier, "Pipe");

  init_group(INIT_GROUP_ASYNCIO);

  TAILQ_INIT(&asyncio_dns_pending);
  TAILQ_INIT(&asyncio_dns_completed);

  hts_mutex_init(&asyncio_dns_mutex);
  asyncio_dns_worker = asyncio_add_worker(adr_deliver_cb);

  hts_mutex_init(&asyncio_http_mutex);
  asyncio_http_worker = asyncio_add_worker(ahr_deliver_cb);

  async_now = showtime_get_ts();

  while(1)
    asyncio_dopoll();
  return NULL;
}


/**
 *
 */
void
asyncio_init(void)
{
  hts_thread_create_joinable("asyncio", &asyncio_thread_id, asyncio_thread,
                             NULL, THREAD_PRIO_MODEL);
}


/**
 *
 */
static void
asyncio_tcp_accept(asyncio_fd_t *af, void *opaque, int events, int error)
{
  assert(events & ASYNCIO_READ);

  struct sockaddr_in si;
  socklen_t sl = sizeof(struct sockaddr_in);
  int fd, val;

  fd = accept(af->af_fd, (struct sockaddr *)&si, &sl);
  net_change_nonblocking(fd, 0);

  if(fd == -1) {
    TRACE(TRACE_ERROR, "TCP", "%s: Accept error: %s", strerror(errno));
    sleep(1);
    return;
  }

  val = 1;
  setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val));
#ifdef TCP_KEEPIDLE
  val = 30;
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &val, sizeof(val));
#endif

#ifdef TCP_KEEPINVL
  val = 15;
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &val, sizeof(val));
#endif

#ifdef TCP_KEEPCNT
  val = 5;
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &val, sizeof(val));
#endif

#ifdef TCP_NODELAY
  val = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));
#endif

  net_addr_t local, remote;
  net_local_addr_from_fd(&local, fd);
  net_remote_addr_from_fd(&remote, fd);

  af->af_accept_callback(af->af_opaque, fd, &local, &remote);
}


/**
 *
 */
asyncio_fd_t *
asyncio_listen(const char *name, int port, asyncio_accept_callback_t *cb,
               void *opaque, int bind_any)
{
  struct sockaddr_in si = {0};
  socklen_t sl = sizeof(struct sockaddr_in);
  int one = 1;
  int fd;

  if((fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == -1)
    return NULL;

  no_sigpipe(fd);

  si.sin_family = AF_INET;

  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(int));

  if(port) {

    si.sin_port = htons(port);
    if(bind(fd, (struct sockaddr *)&si, sizeof(struct sockaddr_in))) {

      if(!bind_any) {
        TRACE(TRACE_ERROR, "TCP", "%s: Bind failed -- %s", name,
              strerror(errno));
        close(fd);
        return NULL;
      } else {
        port = 0;
      }
    }
  }
  if(!port) {
    si.sin_port = 0;
    if(bind(fd, (struct sockaddr *)&si, sizeof(struct sockaddr_in)) == -1) {
      TRACE(TRACE_ERROR, "TCP", "%s: Unable to bind -- %s", name,
            strerror(errno));
      close(fd);
      return NULL;
    }
  }

  if(getsockname(fd, (struct sockaddr *)&si, &sl) == -1) {
    TRACE(TRACE_ERROR, "TCP", "%s: Unable to figure local port", name);
    close(fd);
    return NULL;
  }
  port = ntohs(si.sin_port);

  listen(fd, 100);

  TRACE(TRACE_INFO, "TCP", "%s: Listening on port %d", name, port);

  asyncio_fd_t *af = asyncio_add_fd(fd, ASYNCIO_READ,
                                    asyncio_tcp_accept, opaque, name);

  af->af_accept_callback = cb;
  af->af_port = port;
  return af;
}



/**
 *
 */
static void
do_write(asyncio_fd_t *af)
{
  char tmp[1024];

  while(1) {
    int avail = htsbuf_peek(&af->af_sendq, tmp, sizeof(tmp));
    if(avail == 0) {
      // Nothing more to send
      asyncio_rem_events(af, ASYNCIO_WRITE);
      return;
    }

#ifdef MSG_NOSIGNAL
    int r = send(af->af_fd, tmp, avail, MSG_NOSIGNAL);
#else
    int r = send(af->af_fd, tmp, avail, 0);
#endif
    if(r == 0)
      break;

    if(r == -1 && (errno == EAGAIN))
      break;

    if(r == -1) {
      asyncio_rem_events(af, ASYNCIO_WRITE);
      return;
    }

    htsbuf_drop(&af->af_sendq, r);
    if(r != avail)
      break;
  }
  asyncio_add_events(af, ASYNCIO_WRITE);
}



/**
 *
 */
static void
do_read(asyncio_fd_t *af)
{
  char tmp[1024];
  while(1) {
    int r = read(af->af_fd, tmp, sizeof(tmp));
    if(r == 0) {
      af->af_error_callback(af->af_opaque, "Connection reset");
      return;
    }

    if(r == -1 && (errno == EAGAIN))
      break;

    if(r == -1) {
      char buf[256];
      snprintf(buf, sizeof(buf), "%s", strerror(errno));
      af->af_error_callback(af->af_opaque, buf);
      return;
    }

    htsbuf_append(&af->af_recvq, tmp, r);
  }

  af->af_read_callback(af->af_opaque, &af->af_recvq);
}


/**
 *
 */
static void
asyncio_tcp_connected(asyncio_fd_t *af, void *opaque, int events, int error)
{
  if(events & ASYNCIO_TIMEOUT) {
    af->af_error_callback(af->af_opaque, "Connection timed out");
    return;
  }

  if(events & ASYNCIO_ERROR) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", strerror(error));
    af->af_timeout = 0;
    af->af_error_callback(af->af_opaque, buf);
    return;
  }

  if(events & ASYNCIO_READ) {
    af->af_timeout = 0;
    do_read(af);
    return;
  }

  if(events & ASYNCIO_WRITE) {

    af->af_timeout = 0;

    if(af->af_connected) {
      do_write(af);
      return;
    }

    asyncio_rem_events(af, ASYNCIO_WRITE);
    int err;
    socklen_t errlen = sizeof(int);
    
    getsockopt(af->af_fd, SOL_SOCKET, SO_ERROR, (void *)&err, &errlen);
    
    if(err) {
      char buf[256];
      snprintf(buf, sizeof(buf), "%s", strerror(errno));
      af->af_error_callback(af->af_opaque, buf);
    } else {
      af->af_connected = 1;
      af->af_error_callback(af->af_opaque, NULL);
      do_write(af);
    }
  }
}




/**
 *
 */
void
asyncio_send(asyncio_fd_t *af, const void *buf, size_t len, int cork)
{
  asyncio_verify_thread();
  htsbuf_append(&af->af_sendq, buf, len);
  if(af->af_fd != -1 && !cork)
    do_write(af);
}


/**
 *
 */
void
asyncio_sendq(asyncio_fd_t *af, htsbuf_queue_t *q, int cork)
{
  asyncio_verify_thread();
  htsbuf_appendq(&af->af_sendq, q);
  if(af->af_fd != -1 && !cork)
    do_write(af);
}


/**
 *
 */
asyncio_fd_t *
asyncio_connect(const char *name, const net_addr_t *addr,
		asyncio_error_callback_t *error_cb,
		asyncio_read_callback_t *read_cb,
		void *opaque, int timeout)
{
  struct sockaddr_in si = {0};
  int fd;

  if((fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == -1)
    return NULL;

  no_sigpipe(fd);

  net_change_nonblocking(fd, 1);

  net_change_ndelay(fd, 1);

  si.sin_family = AF_INET;
  si.sin_port = htons(addr->na_port);
  memcpy(&si.sin_addr, addr->na_addr, 4);

  asyncio_fd_t *af = asyncio_add_fd(fd, ASYNCIO_READ,
				    asyncio_tcp_connected, opaque,
                                    name);

  af->af_fd = fd;
  af->af_error_callback = error_cb;
  af->af_read_callback  = read_cb;
  af->af_timeout = showtime_get_ts() + timeout * 1000;

  int r = connect(fd, (struct sockaddr *)&si, sizeof(struct sockaddr_in));
  if(r == -1) {

    if(errno == EINPROGRESS) {
      asyncio_add_events(af, ASYNCIO_WRITE);
    } else {
      // Got fail directly, but we still want to notify the user about
      // the error asynchronously. Just to make things easier
      af->af_pending_errno = errno;
    }
  } else {
    asyncio_add_events(af, ASYNCIO_WRITE);
  }
  return af;
}



int
asyncio_get_port(asyncio_fd_t *af)
{
  return af->af_port;
}


/**
 * DNS handling
 */

struct asyncio_dns_req {
  TAILQ_ENTRY(asyncio_dns_req) adr_link;
  char *adr_hostname;
  void *adr_opaque;
  void (*adr_cb)(void *opaque, int status, const void *data);

  int adr_status;
  const void *adr_data;
  const char *adr_errmsg;
  net_addr_t adr_addr;
};


static int adr_resolver_running;

/**
 *
 */
static int
adr_resolve(asyncio_dns_req_t *adr)
{
  return net_resolve(adr->adr_hostname, &adr->adr_addr, &adr->adr_errmsg);
}


/**
 *
 */
static void *
adr_resolver(void *aux)
{
  asyncio_dns_req_t *adr;
  hts_mutex_lock(&asyncio_dns_mutex);
  while((adr = TAILQ_FIRST(&asyncio_dns_pending)) != NULL) {
    TAILQ_REMOVE(&asyncio_dns_pending, adr, adr_link);

    hts_mutex_unlock(&asyncio_dns_mutex);

    
    if(adr_resolve(adr)) {
      adr->adr_status = ASYNCIO_DNS_STATUS_FAILED;
      adr->adr_data = adr->adr_errmsg;
    } else {
      adr->adr_status = ASYNCIO_DNS_STATUS_COMPLETED;
      adr->adr_data = &adr->adr_addr;
    }
    hts_mutex_lock(&asyncio_dns_mutex);
    TAILQ_INSERT_TAIL(&asyncio_dns_completed, adr, adr_link);
    asyncio_wakeup(asyncio_dns_worker);
  }

  adr_resolver_running = 0;
  hts_mutex_unlock(&asyncio_dns_mutex);
  return NULL;
}

/**
 *
 */
asyncio_dns_req_t *
asyncio_dns_lookup_host(const char *hostname, 
			void (*cb)(void *opaque,
				   int status,
				   const void *data),
			void *opaque)
{
  asyncio_dns_req_t *adr;

  adr = calloc(1, sizeof(asyncio_dns_req_t));
  adr->adr_hostname = strdup(hostname);
  adr->adr_cb = cb;
  adr->adr_opaque = opaque;
  
  hts_mutex_lock(&asyncio_dns_mutex);
  TAILQ_INSERT_TAIL(&asyncio_dns_pending, adr, adr_link);
  if(!adr_resolver_running) {
    adr_resolver_running = 1;
    hts_thread_create_detached("DNS resolver", adr_resolver, NULL, 
			       THREAD_PRIO_BGTASK);
  }
  hts_mutex_unlock(&asyncio_dns_mutex);
  return adr;
}


/**
 * Return async DNS requests to caller
 */
static void
adr_deliver_cb(void)
{
  asyncio_dns_req_t *adr;

  hts_mutex_lock(&asyncio_dns_mutex);

  while((adr = TAILQ_FIRST(&asyncio_dns_completed)) != NULL) {
    TAILQ_REMOVE(&asyncio_dns_completed, adr, adr_link);
    hts_mutex_unlock(&asyncio_dns_mutex);
    adr->adr_cb(adr->adr_opaque, adr->adr_status, adr->adr_data);

    free(adr->adr_hostname);
    free(adr);
    hts_mutex_lock(&asyncio_dns_mutex);
  } 
  hts_mutex_unlock(&asyncio_dns_mutex);
}



/*************************************************************************
 * UDP
 *************************************************************************/

static uint8_t udp_recv_buf[8192];

/**
 *
 */
static void
asyncio_udp_event(asyncio_fd_t *af, void *opaque, int events, int error)
{
  assert(events & ASYNCIO_READ);

  struct sockaddr_in sin;
  socklen_t sl = sizeof(struct sockaddr_in);


  int r = recvfrom(af->af_fd, &udp_recv_buf, sizeof(udp_recv_buf), 0,
		   (struct sockaddr *)&sin, &sl);
  if(r <= 0)
    return;
  net_addr_t na = {0};

  na.na_family = 4;
  na.na_port = ntohs(sin.sin_port);
  memcpy(na.na_addr, &sin.sin_addr, 4);
  af->af_udp_callback(opaque, udp_recv_buf, r, &na);
}


/**
 *
 */
asyncio_fd_t *
asyncio_udp_bind(const char *name,
		 int port,
		 asyncio_udp_callback_t *cb,
		 void *opaque,
		 int bind_any)
{
  struct sockaddr_in si = {0};
  socklen_t sl = sizeof(struct sockaddr_in);
  int one = 1;
  int fd;

  if((fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
    return NULL;

  no_sigpipe(fd);

  si.sin_family = AF_INET;

  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(int));

  if(port) {

    si.sin_port = htons(port);
    if(bind(fd, (struct sockaddr *)&si, sizeof(struct sockaddr_in))) {

      if(!bind_any) {
        TRACE(TRACE_ERROR, "TCP", "%s: Bind failed -- %s", name,
              strerror(errno));
        close(fd);
        return NULL;
      } else {
        port = 0;
      }
    }
  }
  if(!port) {
    si.sin_port = 0;
    if(bind(fd, (struct sockaddr *)&si, sizeof(struct sockaddr_in)) == -1) {
      TRACE(TRACE_ERROR, "TCP", "%s: Unable to bind -- %s", name,
            strerror(errno));
      close(fd);
      return NULL;
    }
  }

  if(getsockname(fd, (struct sockaddr *)&si, &sl) == -1) {
    TRACE(TRACE_ERROR, "TCP", "%s: Unable to figure local port", name);
    close(fd);
    return NULL;
  }
  port = ntohs(si.sin_port);

  TRACE(TRACE_INFO, "UDP", "%s: Listening on port %d", name, port);

  asyncio_fd_t *af = asyncio_add_fd(fd, ASYNCIO_READ,
                                    asyncio_udp_event, opaque, name);
  af->af_udp_callback = cb;
  af->af_port = port;
  return af;
}


/**
 *
 */
void
asyncio_udp_send(asyncio_fd_t *af, const void *data, int size,
		 const net_addr_t *remote_addr)
{
  struct sockaddr_in sin;
  memset(&sin, 0, sizeof(sin));

  sin.sin_family = AF_INET;
  sin.sin_port = htons(remote_addr->na_port);
  memcpy(&sin.sin_addr, remote_addr->na_addr, 4);
  sendto(af->af_fd, data, size, 0, 
	 (const struct sockaddr *)&sin, sizeof(struct sockaddr_in));
}


/*************************************************************************
 * HTTP(S)
 *************************************************************************/

struct asyncio_http_req {
  LIST_ENTRY(asyncio_http_req) ahr_link;
  int ahr_cancelled;

  http_req_aux_t *ahr_req;

  void (*ahr_cb)(http_req_aux_t *hra, void *opaque);
  void *ahr_opaque;
};


/**
 *
 */
static void
asyncio_http_cb(http_req_aux_t *hra, void *opaque, int error)
{
  asyncio_http_req_t *ahr = opaque;
  ahr->ahr_req = http_req_retain(hra);

  // This arrives on a different thread so we need to reschedule

  hts_mutex_lock(&asyncio_http_mutex);
  LIST_INSERT_HEAD(&asyncio_http_completed, ahr, ahr_link);
  hts_mutex_unlock(&asyncio_http_mutex);
  asyncio_wakeup(asyncio_http_worker);
}


/**
 *
 */
asyncio_http_req_t *
asyncio_http_req(const char *url,
                 void (*cb)(http_req_aux_t *req, void *opaque),
                 void *opaque,
                 ...)
{
  asyncio_http_req_t *ahr = calloc(1, sizeof(asyncio_http_req_t));
  va_list ap;

  ahr->ahr_cb = cb;
  ahr->ahr_opaque = opaque;

  va_start(ap, opaque);
  http_reqv(url, ap, asyncio_http_cb, ahr);
  va_end(ap);
  return ahr;
}


void
asyncio_http_cancel(asyncio_http_req_t *ahr)
{
  asyncio_verify_thread();
  ahr->ahr_cancelled = 1;
}


static void
ahr_deliver_cb(void)
{
  asyncio_http_req_t *ahr;

  hts_mutex_lock(&asyncio_http_mutex);

  while((ahr = LIST_FIRST(&asyncio_http_completed)) != NULL) {
    LIST_REMOVE(ahr, ahr_link);
    hts_mutex_unlock(&asyncio_http_mutex);

    if(!ahr->ahr_cancelled)
      ahr->ahr_cb(ahr->ahr_req, ahr->ahr_opaque);

    http_req_release(ahr->ahr_req);
    free(ahr);


    hts_mutex_lock(&asyncio_http_mutex);
  }

  hts_mutex_unlock(&asyncio_http_mutex);
}
