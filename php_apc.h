/* ==================================================================
 * APC Cache
 * Copyright (c) 2000-2001 Community Connect, Inc.
 * All rights reserved.
 * ==================================================================
 * This source code is made available free and without charge subject
 * to the terms of the QPL as detailed in bundled LICENSE file, which
 * is also available at http://apc.communityconnect.com/LICENSE.
 * ==================================================================
 * Daniel Cowgill <dan@mail.communityconnect.com>
 * George Schlossnagle <george@lethargy.org>
 * ==================================================================
*/


#ifndef PHP_APC_H
#define PHP_APC_H

#include "php.h"
#include "php_ini.h"

#include "zend.h"
#include "zend_API.h"
#include "zend_execute.h"
#include "zend_compile.h"
#include "zend_extensions.h"

#include <regex.h>

extern zend_module_entry apc_module_entry;

#define apc_module_ptr &apc_module_entry

ZEND_BEGIN_MODULE_GLOBALS(apc)
	int	mode;				/* mode apc is running (off/shm/mmap) */
	int cache_rt;     /* cache-retrieval policy */
	int	ttl;				/* default ttl for all cache objects */
	char *cachedir;			/* directory for compiled objects (mmap) */
	char *regex_text[10];		/* regex for filtering items from cache */
	regex_t regex[10];			/* compiled regex_text */
	int nmatches;			/* no. of regex filters */
	int hash_buckets;		/* no. of hash buckets in shared index (shm) */
	int shm_segments;		/* max no. of segments for object storage (shm) */
	int shm_segment_size;	/* max segment size for object storage (shm) */
	int check_mtime;		/* check modification time of files? (shm) */
	int relative_includes; /* provide generic support for relative includes? */
	int check_compiled_source; /* allows for compiled source files */
ZEND_END_MODULE_GLOBALS(apc)

#define APCG(v) (apc_globals.v)

#define SHM_MODE  2
#define MMAP_MODE 1
#define OFF_MODE  0
#define APC_SHM_MODE  APCG(mode) == SHM_MODE
#define APC_MMAP_MODE APCG(mode) == MMAP_MODE
#define APC_OFF_MODE  APCG(mode) == OFF_MODE

#define phpext_apc_ptr apc_module_ptr


#endif
