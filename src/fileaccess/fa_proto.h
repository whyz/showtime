/*
 *  File access, abstract interface for accessing files
 *  Copyright (C) 2008 Andreas Öman
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

#ifndef FA_PROTO_H__
#define FA_PROTO_H__

#include "fileaccess.h"


typedef enum {
  FAP_STAT_OK = 0,
  FAP_STAT_ERR = -1,
  FAP_STAT_NEED_AUTH = -2,
} fap_stat_code_t;


/**
 * File access protocol
 */
typedef struct fa_protocol {

  int fap_flags;
#define FAP_INCLUDE_PROTO_IN_URL 0x1
#define FAP_ALLOW_CACHE          0x2
#define FAP_NO_PARKING           0x4

  int fap_refcount;

  void (*fap_init)(void);

  void (*fap_fini)(struct fa_protocol *fap);

  LIST_ENTRY(fa_protocol) fap_link;

  const char *fap_name;

  /**
   * Directory scan url for files. 
   */
  int (*fap_scan)(struct fa_protocol *fap, fa_dir_t *fa, const char *url,
		  char *errbuf, size_t errsize);

  /**
   * Open url for reading
   */
  fa_handle_t *(*fap_open)(struct fa_protocol *fap, const char *url,
			   char *errbuf, size_t errsize, int flags,
			   struct prop *stats);

  /**
   * Close filehandle
   */
  void (*fap_close)(fa_handle_t *fh);

  /**
   * Read from file. Same semantics as POSIX read(2)
   */
  int (*fap_read)(fa_handle_t *fh, void *buf, size_t size);

  /**
   * Read from file. Same semantics as POSIX write(2)
   */
  int (*fap_write)(fa_handle_t *fh, const void *buf, size_t size);

  /**
   * Seek in file. Same semantics as POSIX lseek(2)
   */
  int64_t (*fap_seek)(fa_handle_t *fh, int64_t pos, int whence);

  /**
   * Return size of file
   */
  int64_t (*fap_fsize)(fa_handle_t *fh);

  /**
   * stat(2) file
   *
   * If non_interactive is set, this is probe request and it must not
   * ask for any user input (access credentials, etc)
   */
  int (*fap_stat)(struct fa_protocol *fap, const char *url, struct fa_stat *buf,
		  char *errbuf, size_t errsize, int non_interactive);

  /**
   * unlink (ie, delete) file
   */
  int (*fap_unlink)(const struct fa_protocol *fap, const char *url,
                    char *errbuf, size_t errsize);

  /**
   * delete directory
   */
  int (*fap_rmdir)(const struct fa_protocol *fap, const char *url,
                   char *errbuf, size_t errsize);

  /**
   * Add a reference to the url.
   *
   * Let underlying protocols know that we probably want to read more info
   * about the url in the future. Mostly useful for archive wrappers that
   * might want to keep the archive indexed in memory
   *
   * Optional
   */
  fa_handle_t *(*fap_reference)(struct fa_protocol *fap, const char *url);

  /**
   * Release a reference obtained by fap_reference()
   */
  void (*fap_unreference)(fa_handle_t *fh);

  /**
   * Monitor the filesystem directory described by url for changes
   *
   * If a change occures, change() is invoked
   */
  fa_handle_t *(*fap_notify_start)(struct fa_protocol *fap, const char *url,
                                   void *opaque,
                                   void (*change)(void *opaque,
                                                  fa_notify_op_t op,
                                                  const char *filename,
                                                  const char *url,
                                                  int type));

  void (*fap_notify_stop)(fa_handle_t *fh);

  /**
   * Load the 'url' into memory
   */
  buf_t *(*fap_load)(struct fa_protocol *fap, const char *url,
                     char *errbuf, size_t errlen,
                     char **etag, time_t *mtime, int *max_age,
                     int flags, fa_load_cb_t *cb, void *opaque);

  /**
   * Normalize the given URL.
   *
   * Remove any relative components and symlinks, etc
   */
  int (*fap_normalize)(struct fa_protocol *fap, const char *url,
		       char *dst, size_t dstlen);
  
  /**
   * Extract the last component of the URL (ie. the filename)
   */
  void (*fap_get_last_component)(struct fa_protocol *fap, const char *url,
				 char *dst, size_t dstlen);

  /**
   * Return true if the given file can seek in a good manner
   */
  int (*fap_seek_is_fast)(fa_handle_t *fh);

  /**
   * Return all parts that relates to the given URL
   *
   * For RAR archives this would be all part-files
   * 
   */
  int (*fap_get_parts)(fa_dir_t *fa, const char *url,
		       char *errbuf, size_t errsize);

  /**
   * Make directories
   *
   * Should do the equiv to POSIX mkdir -p
   */

  int (*fap_makedirs)(struct fa_protocol *fap, const char *url,
                      char *errbuf, size_t errsize);

} fa_protocol_t;



void fileaccess_register_dynamic(fa_protocol_t *fap);

void fileaccess_unregister_dynamic(fa_protocol_t *fap);

void fileaccess_register_entry(fa_protocol_t *fap);

#define FAP_REGISTER(name) \
  static void  __attribute__((constructor)) fap_register_ ## name(void) { \
    fileaccess_register_entry(&fa_protocol_ ## name);			\
  }

#endif /* FA_PROTO_H__ */
