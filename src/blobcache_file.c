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

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>

#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>
#include <errno.h>

#include "showtime.h"
#include "blobcache.h"
#include "misc/pool.h"
#include "misc/sha.h"
#include "misc/md5.h"
#include "arch/arch.h"
#include "arch/threads.h"
#include "arch/atomic.h"
#include "settings.h"
#include "notifications.h"
#include "misc/minmax.h"
#include "fileaccess/fileaccess.h"

// Flags

#define BC2_MAGIC_07      0x62630207
#define BC2_MAGIC_06      0x62630206
#define BC2_MAGIC_05      0x62630205

typedef struct blobcache_item {
  struct blobcache_item *bi_link;
  char *bi_etag;
  uint64_t bi_key_hash;
  uint64_t bi_content_hash;
  uint32_t bi_lastaccess;
  uint32_t bi_expiry;
  uint32_t bi_modtime;
  uint32_t bi_size;
  uint8_t bi_content_type_len;
  uint8_t bi_flags;
} blobcache_item_t;

typedef struct blobcache_diskitem_06 {
  uint64_t di_key_hash;
  uint64_t di_content_hash;
  uint32_t di_lastaccess;
  uint32_t di_expiry;
  uint32_t di_modtime;
  uint32_t di_size;
  uint8_t di_etaglen;
  uint8_t di_content_type_len;
  uint8_t di_etag[0];
} __attribute__((packed)) blobcache_diskitem_06_t;

typedef struct blobcache_diskitem_07 {
  uint64_t di_key_hash;
  uint64_t di_content_hash;
  uint32_t di_lastaccess;
  uint32_t di_expiry;
  uint32_t di_modtime;
  uint32_t di_size;
  uint8_t di_flags;
  uint8_t di_etaglen;
  uint8_t di_content_type_len;
  uint8_t di_etag[0];
} __attribute__((packed)) blobcache_diskitem_07_t;


TAILQ_HEAD(blobcache_flush_queue, blobcache_flush);

typedef struct blobcache_flush {
  TAILQ_ENTRY(blobcache_flush) bf_link;
  uint64_t bf_key_hash;
  buf_t *bf_buf;
} blobcache_flush_t;



#define ITEM_HASH_SIZE 256
#define ITEM_HASH_MASK (ITEM_HASH_SIZE - 1)

static blobcache_item_t *hashvector[ITEM_HASH_SIZE];

static struct blobcache_flush_queue flush_queue;

static pool_t *item_pool;
static hts_mutex_t cache_lock;
static hts_cond_t cache_cond;
static hts_thread_t bcthread;
static int bcrun = 1;
static int index_dirty;

#define BLOB_CACHE_MINSIZE   (10 * 1000 * 1000)
#define BLOB_CACHE_MAXSIZE (1000 * 1000 * 1000)

static uint64_t current_cache_size;


/**
 *
 */
static uint64_t
blobcache_compute_maxsize(void)
{
  char path[PATH_MAX];
  fa_fsinfo_t ffi;

  snprintf(path, sizeof(path), "file://%s", gconf.cache_path);
  if(!fa_fsinfo(path, &ffi)) {
    uint64_t avail = ffi.ffi_avail + current_cache_size;
    avail = MAX(BLOB_CACHE_MINSIZE, MIN(avail / 10, BLOB_CACHE_MAXSIZE));
    return avail;
  }
  return BLOB_CACHE_MINSIZE;
}


/**
 *
 */
static uint64_t
digest_key(const char *key, const char *stash)
{
  union {
    uint8_t d[20];
    uint64_t u64;
  } u;
  sha1_decl(shactx);
  sha1_init(shactx);
  sha1_update(shactx, (const uint8_t *)key, strlen(key));
  sha1_update(shactx, (const uint8_t *)stash, strlen(stash));
  sha1_final(shactx, u.d);
  return u.u64;
}


/**
 *
 */
static uint64_t
digest_content(const void *data, size_t len)
{
  union {
    uint8_t d[16];
    uint64_t u64;
  } u;
  md5_decl(ctx);
  md5_init(ctx);
  md5_update(ctx, data, len);
  md5_final(ctx, u.d);
  return u.u64;
}


/**
 *
 */
static void
make_filename(char *buf, size_t len, uint64_t hash, int for_write)
{
  uint8_t dir = hash;
  if(for_write) {
    snprintf(buf, len, "%s/bc2/%02x", gconf.cache_path, dir);
    mkdir(buf, 0777);
  }
  snprintf(buf, len, "%s/bc2/%02x/%016"PRIx64, gconf.cache_path, dir, hash);
}


/**
 *
 */
static void
save_index(void)
{
  char filename[PATH_MAX];
  uint8_t *out, *base;
  int i;
  blobcache_item_t *p;
  blobcache_diskitem_07_t *di;
  size_t siz;

  if(!index_dirty)
    return;

  snprintf(filename, sizeof(filename), "%s/bc2/index.dat", gconf.cache_path);
  
  int fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0666);
  if(fd == -1)
    return;
  
  int items = 0;
  siz = 12 + 20;

  for(i = 0; i < ITEM_HASH_SIZE; i++) {
    for(p = hashvector[i]; p != NULL; p = p->bi_link) {
      siz += sizeof(blobcache_diskitem_07_t);
      siz += p->bi_etag ? strlen(p->bi_etag) : 0;
      items++;
    }
  }

  base = out = mymalloc(siz);
  if(out == NULL) {
    close(fd);
    return;
  }
  *(uint32_t *)out = BC2_MAGIC_07;
  out += 4;
  *(uint32_t *)out = items;
  out += 4;
  *(uint32_t *)out = time(NULL);
  out += 4;
  for(i = 0; i < ITEM_HASH_SIZE; i++) {
    for(p = hashvector[i]; p != NULL; p = p->bi_link) {
      const int etaglen = p->bi_etag ? strlen(p->bi_etag) : 0;
      di = (blobcache_diskitem_07_t *)out;
      di->di_key_hash     = p->bi_key_hash;
      di->di_content_hash = p->bi_content_hash;
      di->di_lastaccess   = p->bi_lastaccess;
      di->di_expiry       = p->bi_expiry;
      di->di_modtime      = p->bi_modtime;
      di->di_size         = p->bi_size;
      di->di_flags        = p->bi_flags;
      di->di_etaglen      = etaglen;
      di->di_content_type_len = p->bi_content_type_len;
      out += sizeof(blobcache_diskitem_07_t);
      if(etaglen) {
	memcpy(out, p->bi_etag, etaglen);
	out += etaglen;
      }

    }
  }

  sha1_decl(shactx);
  sha1_init(shactx);
  sha1_update(shactx, base, siz - 20);
  sha1_final(shactx, out);

  if(write(fd, base, siz) != siz) {
    TRACE(TRACE_INFO, "blobcache", "Unable to store index file %s -- %s",
	  filename, strerror(errno));
  } else {
    index_dirty = 0;
  }

  free(base);
  close(fd);

}



/**
 *
 */
static void
load_index(void)
{
  char filename[PATH_MAX];
  const uint8_t *in;
  void *base;
  int i;
  blobcache_item_t *p;
  struct stat st;
  uint8_t digest[20];

  snprintf(filename, sizeof(filename), "%s/bc2/index.dat", gconf.cache_path);
  
  int fd = open(filename, O_RDONLY, 0);
  if(fd == -1)
    return;

  if(fstat(fd, &st) || st.st_size <= 20) {
    close(fd);
    return;
  }

  in = base = mymalloc(st.st_size);
  if(base == NULL) {
    close(fd);
    return;
  }

  size_t r = read(fd, base, st.st_size);
  close(fd);
  if(r != st.st_size) {
    free(base);
    return;
  }


  sha1_decl(shactx);
  sha1_init(shactx);
  sha1_update(shactx, in, st.st_size - 20);
  sha1_final(shactx, digest);

  if(memcmp(digest, in + st.st_size - 20, 20)) {
    free(base);
    TRACE(TRACE_INFO, "blobcache", "Index file corrupt, throwing away cache");
    return;
  }

  uint32_t magic = *(uint32_t *)in;
  in += 4;
  int items = *(uint32_t *)in;
  in += 4;

  TRACE(TRACE_DEBUG, "blobcache", "Cache magic 0x%08x %d items", magic, items);

  switch(magic) {
  case BC2_MAGIC_06:
    TRACE(TRACE_INFO, "blobcache", "Upgrading from older format 0x%08x", magic);
    // FALLTHRU
  case BC2_MAGIC_07: {
    if(*(uint32_t *)in > time(NULL)) {
      TRACE(TRACE_INFO, "blobcache",
	    "Clock going backwards, throwing away cache");
      free(base);
      return;
    }
    in += 4;
    break;
  }

  case BC2_MAGIC_05:
    TRACE(TRACE_INFO, "blobcache", "Upgrading from older format 0x%08x", magic);
    break;

  default:
    TRACE(TRACE_INFO, "blobcache", "Invalid magic 0x%08x", magic);
    free(base);
    return;
  }


  for(i = 0; i < items; i++) {
    p = pool_get(item_pool);
    int etaglen;

    switch(magic) {
    case BC2_MAGIC_05:
    case BC2_MAGIC_06: {
      const blobcache_diskitem_06_t *di = (blobcache_diskitem_06_t *)in;

      p->bi_key_hash         = di->di_key_hash;
      p->bi_content_hash     = di->di_content_hash;
      p->bi_lastaccess       = di->di_lastaccess;
      p->bi_expiry           = di->di_expiry;
      p->bi_modtime          = di->di_modtime;
      p->bi_size             = di->di_size;
      p->bi_content_type_len = di->di_content_type_len;
      p->bi_flags            = 0;
      etaglen                = di->di_etaglen;
      in += sizeof(blobcache_diskitem_06_t);
    }
      break;

    case BC2_MAGIC_07: {
      const blobcache_diskitem_07_t *di = (blobcache_diskitem_07_t *)in;

      p->bi_key_hash         = di->di_key_hash;
      p->bi_content_hash     = di->di_content_hash;
      p->bi_lastaccess       = di->di_lastaccess;
      p->bi_expiry           = di->di_expiry;
      p->bi_modtime          = di->di_modtime;
      p->bi_size             = di->di_size;
      p->bi_content_type_len = di->di_content_type_len;
      p->bi_flags            = di->di_flags;
      etaglen                = di->di_etaglen;
      in += sizeof(blobcache_diskitem_07_t);
    }
      break;
    default:
      abort(); // Prevent compilers whining about etaglen not initialized
    }

    if(etaglen) {
      p->bi_etag = malloc(etaglen+1);
      memcpy(p->bi_etag, in, etaglen);
      p->bi_etag[etaglen] = 0;
      in += etaglen;
    } else {
      p->bi_etag = NULL;
    }
    p->bi_link = hashvector[p->bi_key_hash & ITEM_HASH_MASK];
    hashvector[p->bi_key_hash & ITEM_HASH_MASK] = p;
    current_cache_size += p->bi_size;
  }
  free(base);
}



/**
 *
 */
int
blobcache_put(const char *key, const char *stash, buf_t *b,
              int maxage, const char *etag, time_t mtime,
              int flags)
{
  uint64_t dk = digest_key(key, stash);
  uint64_t dc = digest_content(b->b_ptr, b->b_size);
  uint32_t now = time(NULL);
  blobcache_item_t *p;

  if(etag != NULL && strlen(etag) > 255)
    etag = NULL;

  hts_mutex_lock(&cache_lock);
  if(!bcrun) {
    hts_mutex_unlock(&cache_lock);
    return 0;
  }

  for(p = hashvector[dk & ITEM_HASH_MASK]; p != NULL; p = p->bi_link)
    if(p->bi_key_hash == dk)
      break;

  hts_cond_signal(&cache_cond);
  index_dirty = 1;

  if(p != NULL && p->bi_content_hash == dc && p->bi_size == b->b_size) {
    p->bi_modtime = mtime;
    p->bi_expiry = now + maxage;
    p->bi_lastaccess = now;
    p->bi_flags = flags;
    mystrset(&p->bi_etag, etag);
    hts_mutex_unlock(&cache_lock);
    return 1;
  }

  blobcache_flush_t *bf = pool_get(item_pool);
  bf->bf_key_hash = dk;
  bf->bf_buf = buf_retain(b);
  TAILQ_INSERT_TAIL(&flush_queue, bf, bf_link);
  hts_cond_signal(&cache_cond);

  if(p == NULL) {
    p = pool_get(item_pool);
    p->bi_key_hash = dk;
    p->bi_size = 0;
    p->bi_content_type_len = 0;
    p->bi_link = hashvector[dk & ITEM_HASH_MASK];
    p->bi_etag = NULL;
    hashvector[dk & ITEM_HASH_MASK] = p;
  }

  int64_t expiry = (int64_t)maxage + now;

  p->bi_modtime = mtime;
  mystrset(&p->bi_etag, etag);
  p->bi_expiry = MIN(INT32_MAX, expiry);
  p->bi_lastaccess = now;
  p->bi_content_hash = dc;
  current_cache_size -= p->bi_size;
  p->bi_size = b->b_size;
  current_cache_size += p->bi_size;
  p->bi_content_type_len = b->b_content_type ?
    strlen(rstr_get(b->b_content_type)) : 0;
  p->bi_flags = flags;
  hts_mutex_unlock(&cache_lock);
  return 0;
}


/**
 *
 */
buf_t *
blobcache_get(const char *key, const char *stash, int pad,
	      int *ignore_expiry, char **etagp, time_t *mtimep)
{
  uint64_t dk = digest_key(key, stash);
  blobcache_item_t *p, **q;
  char filename[PATH_MAX];
  struct stat st;
  uint32_t now;

  hts_mutex_lock(&cache_lock);

  if(!bcrun) {
    p = NULL;
  } else {
    for(q = &hashvector[dk & ITEM_HASH_MASK]; (p = *q); q = &p->bi_link)
      if(p->bi_key_hash == dk)
	break;
  }

  if(p == NULL) {
    hts_mutex_unlock(&cache_lock);
    return NULL;
  }

  now = time(NULL);

  int expired = now > p->bi_expiry;

  if(expired && ignore_expiry == NULL)
    goto bad;

  blobcache_flush_t *bf;
  buf_t *b = NULL;
  int fd = -1;
  TAILQ_FOREACH_REVERSE(bf, &flush_queue, blobcache_flush_queue, bf_link) {
    if(bf->bf_key_hash == p->bi_key_hash) {
      // Item is not yet written to disk
      b = buf_retain(bf->bf_buf);
      break;
    }
  }

  if(b == NULL) {
    make_filename(filename, sizeof(filename), p->bi_key_hash, 0);
    fd = open(filename, O_RDONLY, 0);
    if(fd == -1) {
    bad:
      *q = p->bi_link;
      pool_put(item_pool, p);
      hts_mutex_unlock(&cache_lock);
      return NULL;
    }

    if(fstat(fd, &st))
      goto bad;

    if(st.st_size != p->bi_size + p->bi_content_type_len) {
      unlink(filename);
      goto bad;
    }
  }

  if(mtimep)
    *mtimep = p->bi_modtime;

  if(etagp != NULL)
    *etagp = p->bi_etag ? strdup(p->bi_etag) : NULL;

  p->bi_lastaccess = now;
  index_dirty = 1; // We don't deem it important enough to wakeup on get

  if(ignore_expiry != NULL)
    *ignore_expiry = expired;

  hts_mutex_unlock(&cache_lock);

  if(b == NULL) {

    b = buf_create(p->bi_size + pad);
    if(b == NULL) {
      close(fd);
      return NULL;
    }

    if(p->bi_content_type_len) {
      b->b_content_type = rstr_allocl(NULL, p->bi_content_type_len);
      if(read(fd, rstr_data(b->b_content_type), p->bi_content_type_len) !=
	 p->bi_content_type_len) {
	buf_release(b);
	close(fd);
	return NULL;
      }
    }

    if(read(fd, b->b_ptr, p->bi_size) != p->bi_size) {
      buf_release(b);
      close(fd);
      return NULL;
    }
    memset(b->b_ptr + p->bi_size, 0, pad);
    close(fd);
  }
  return b;
}





/**
 *
 */
int
blobcache_get_meta(const char *key, const char *stash, 
		   char **etagp, time_t *mtimep)
{
  uint64_t dk = digest_key(key, stash);
  blobcache_item_t *p;
  int r;
  hts_mutex_lock(&cache_lock);
  if(!bcrun) {
    p = NULL;
  } else {
    for(p = hashvector[dk & ITEM_HASH_MASK]; p != NULL; p = p->bi_link)
      if(p->bi_key_hash == dk)
	break;
  }

  if(p != NULL) {
    r = 0;

    if(mtimep != NULL)
      *mtimep = p->bi_modtime;

    if(etagp != NULL)
      *etagp = p->bi_etag ? strdup(p->bi_etag) : NULL;

  } else {
    r = -1;
  }

  hts_mutex_unlock(&cache_lock);
  return r;
}


/**
 * Assume we're locked
 */
static blobcache_item_t *
lookup_item(uint64_t dk)
{
  blobcache_item_t *p;
  for(p = hashvector[dk & ITEM_HASH_MASK]; p != NULL; p = p->bi_link)
    if(p->bi_key_hash == dk)
      return p;
  return NULL;
}

/**
 *
 */
static void
prune_stale(void)
{
  DIR *d1, *d2;
  struct dirent *de1, *de2;
  char path[PATH_MAX];
  char path2[PATH_MAX];
  char path3[PATH_MAX];
  uint64_t k;

  snprintf(path, sizeof(path), "%s/bc2", gconf.cache_path);

  if((d1 = opendir(path)) == NULL)
    return;

  while((de1 = readdir(d1)) != NULL) {
    if(de1->d_name[0] != '.') {
      snprintf(path2, sizeof(path2), "%s/bc2/%s",
	       gconf.cache_path, de1->d_name);

      if((d2 = opendir(path2)) != NULL) {
	while((de2 = readdir(d2)) != NULL) {
          if(de2->d_name[0] != '.') {

	    snprintf(path3, sizeof(path3), "%s/bc2/%s/%s",
		     gconf.cache_path, de1->d_name,
		     de2->d_name);

	    if(sscanf(de2->d_name, "%016"PRIx64, &k) != 1 ||
	       lookup_item(k) == NULL) {
	      TRACE(TRACE_DEBUG, "Blobcache", "Removed stale file %s", path3);
	      unlink(path3);
	    }
	  }
	}
	closedir(d2);
      }
      rmdir(path2);
    }
  }
  closedir(d1);
}  




/**
 *
 */
static void
prune_item(blobcache_item_t *p)
{
  char filename[PATH_MAX];
  make_filename(filename, sizeof(filename), p->bi_key_hash, 0);
  unlink(filename);
  pool_put(item_pool, p);
}


/**
 *
 */
static int
accesstimecmp(const void *A, const void *B)
{
  const blobcache_item_t *a = *(const blobcache_item_t **)A;
  const blobcache_item_t *b = *(const blobcache_item_t **)B;

  const int a_imp = !!(a->bi_flags & BLOBCACHE_IMPORTANT_ITEM);
  const int b_imp = !!(b->bi_flags & BLOBCACHE_IMPORTANT_ITEM);

  if(a_imp != b_imp)
    return a_imp - b_imp;

  return a->bi_lastaccess - b->bi_lastaccess;
}


/**
 *
 */
static void
prune_to_size(uint64_t maxsize)
{
  int i, tot = 0, j = 0;
  blobcache_item_t *p, **sv;

  for(i = 0; i < ITEM_HASH_SIZE; i++)
    for(p = hashvector[i]; p != NULL; p = p->bi_link)
      tot++;

  sv = malloc(sizeof(blobcache_item_t *) * tot);
  current_cache_size = 0;
  for(i = 0; i < ITEM_HASH_SIZE; i++) {
    for(p = hashvector[i]; p != NULL; p = p->bi_link) {
      sv[j++] = p;
      current_cache_size += p->bi_size;
    }
    hashvector[i] = NULL;
  }

  assert(j == tot);

  qsort(sv, j, sizeof(blobcache_item_t *), accesstimecmp);
  for(i = 0; i < j; i++) {
    p = sv[i];
    if(current_cache_size < maxsize)
      break;
    current_cache_size -= p->bi_size;
    prune_item(p);
    index_dirty = 1;
  }

  for(; i < j; i++) {
    p = sv[i];
    p->bi_link = hashvector[p->bi_key_hash & ITEM_HASH_MASK];
    hashvector[p->bi_key_hash & ITEM_HASH_MASK] = p;
  }

  free(sv);
  save_index();
}



/**
 *
 */
static void
blobcache_prune_old(void)
{
  DIR *d1, *d2;
  struct dirent *de1, *de2;
  char path[PATH_MAX];
  char path2[PATH_MAX];
  char path3[PATH_MAX];

  snprintf(path, sizeof(path), "%s/blobcache", gconf.cache_path);

  if((d1 = opendir(path)) != NULL) {

    while((de1 = readdir(d1)) != NULL) {
      if(de1->d_name[0] != '.') {
	snprintf(path2, sizeof(path2), "%s/blobcache/%s",
		 gconf.cache_path, de1->d_name);

	if((d2 = opendir(path2)) != NULL) {
	  while((de2 = readdir(d2)) != NULL) {
	    if(de2->d_name[0] != '.') {

	      snprintf(path3, sizeof(path3), "%s/blobcache/%s/%s",
		       gconf.cache_path, de1->d_name,
		       de2->d_name);
	      unlink(path3);
	    }
	  }
	  closedir(d2);
	}
	rmdir(path2);
      }
    }
    closedir(d1);
    rmdir(path);
  }

  snprintf(path, sizeof(path), "%s/cachedb/cache.db", gconf.cache_path);
  unlink(path);
  snprintf(path, sizeof(path), "%s/cachedb/cache.db-shm", gconf.cache_path);
  unlink(path);
  snprintf(path, sizeof(path), "%s/cachedb/cache.db-wal", gconf.cache_path);
  unlink(path);
}  

static void
cache_clear(void *opaque, prop_event_t event, ...)
{
  int i;
  blobcache_item_t *p, *n;

  hts_mutex_lock(&cache_lock);

  for(i = 0; i < ITEM_HASH_SIZE; i++) {
    for(p = hashvector[i]; p != NULL; p = n) {
      n = p->bi_link;
      prune_item(p);
      index_dirty = 1;
    }
    hashvector[i] = NULL;
  }
  current_cache_size = 0;
  save_index();
  hts_mutex_unlock(&cache_lock);
  notify_add(NULL, NOTIFY_INFO, NULL, 3, _("Cache cleared"));
}



/**
 *
 */
static void *
flushthread(void *aux)
{
  blobcache_flush_t *bf;

  hts_mutex_lock(&cache_lock);

  while(bcrun) {

    if((bf = TAILQ_FIRST(&flush_queue)) == NULL) {

      if(index_dirty) {
        if(hts_cond_wait_timeout(&cache_cond, &cache_lock, 5000))
          save_index();
      } else {
        hts_cond_wait(&cache_cond, &cache_lock);
      }
      continue;
    }

    hts_mutex_unlock(&cache_lock);
    char filename[PATH_MAX];
    make_filename(filename, sizeof(filename), bf->bf_key_hash, 1);
    buf_t *b = bf->bf_buf;

    int fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if(fd != -1) {

      if(b->b_content_type != NULL) {
	const char *str = rstr_get(b->b_content_type);
	size_t len = strlen(str);
	if(write(fd, str, len) != len)
	  unlink(filename);
      }

      if(write(fd, b->b_ptr, b->b_size) != b->b_size)
        unlink(filename);

      if(close(fd))
        unlink(filename);
    }
    hts_mutex_lock(&cache_lock);

    assert(TAILQ_FIRST(&flush_queue) == bf);
    TAILQ_REMOVE(&flush_queue, bf, bf_link);
    buf_release(bf->bf_buf);
    pool_put(item_pool, bf);

    uint64_t maxsize = blobcache_compute_maxsize();

    if(maxsize < current_cache_size)
      prune_to_size(maxsize);
  }
  save_index();
  hts_mutex_unlock(&cache_lock);
  return NULL;
}

static_assert(sizeof(blobcache_flush_t) <= sizeof(blobcache_item_t),
              "blobcache_flush too big");

/**
 *
 */
void
blobcache_init(void)
{
  char buf[256];

  TAILQ_INIT(&flush_queue);

  blobcache_prune_old();
  snprintf(buf, sizeof(buf), "%s/bc2", gconf.cache_path);
  if(mkdir(buf, 0777) && errno != EEXIST)
    TRACE(TRACE_ERROR, "blobcache", "Unable to create cache dir %s -- %s",
	  buf, strerror(errno));

  hts_mutex_init(&cache_lock);
  hts_cond_init(&cache_cond, &cache_lock);
  item_pool = pool_create("blobcacheitems", sizeof(blobcache_item_t), 0);


  load_index();
  prune_stale();

  uint64_t maxsize = blobcache_compute_maxsize();
  prune_to_size(maxsize);

  TRACE(TRACE_INFO, "blobcache",
	"Initialized: %d items consuming %.2f MB "
        "(out of maximum %.2f MB) on disk in %s",
	pool_num(item_pool), current_cache_size / 1000000.0,
        maxsize / 1000000.0, buf);

  settings_create_action(gconf.settings_general, _p("Clear cached files"),
			 cache_clear, NULL, 0, NULL);

  hts_thread_create_joinable("blobcache", &bcthread, flushthread, NULL,
                             THREAD_PRIO_BGTASK);
}


void
blobcache_fini(void)
{
  hts_mutex_lock(&cache_lock);
  bcrun = 0;
  hts_cond_signal(&cache_cond);
  hts_mutex_unlock(&cache_lock);
  hts_thread_join(&bcthread);
}
