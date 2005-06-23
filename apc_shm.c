/*
  +----------------------------------------------------------------------+
  | APC                                                                  |
  +----------------------------------------------------------------------+
  | Copyright (c) 2005 The PHP Group                                     |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.0 of the PHP license,       |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_0.txt.                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Authors: Daniel Cowgill <dcowgill@communityconnect.com>              |
  |          Rasmus Lerdorf <rasmus@php.net>                             |
  +----------------------------------------------------------------------+

   This software was contributed to PHP by Community Connect Inc. in 2002
   and revised in 2005 by Yahoo! Inc. to add support for PHP 5.1.
   Future revisions and derivatives of this source code must acknowledge
   Community Connect Inc. as the original contributor of this module by
   leaving this note intact in the source code.

   All other licensing and usage conditions are those of the PHP Group.

 */

/* $Id$ */

#include "apc_shm.h"
#include "apc.h"
#include <sys/types.h>
#ifdef PHP_WIN32
/* shm functions are available in TSRM */
#include <tsrm/tsrm_win32.h>
#define key_t long
#else
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#endif

#ifndef SHM_R
# define SHM_R 0444 /* read permission */
#endif
#ifndef SHM_A
# define SHM_A 0222 /* write permission */
#endif

int apc_shm_create(const char* pathname, int proj, int size)
{
    int shmid;  /* shared memory id */
    int oflag;  /* permissions on shm */
    key_t key;  /* shm key returned by ftok */

    key = IPC_PRIVATE;
#ifndef PHP_WIN32
	/* no ftok yet for win32 */
    if (pathname != NULL) {
        if ((key = ftok(pathname, proj)) < 0) {
            apc_eprint("apc_shm_create: ftok failed:");
        }
    }
#endif

    oflag = IPC_CREAT | SHM_R | SHM_A;
    if ((shmid = shmget(key, size, oflag)) < 0) {
        apc_eprint("apc_shm_create: shmget(%d, %d,%d) failed: %s", key, size, oflag, strerror(errno));
    }

    return shmid;
}

void apc_shm_destroy(int shmid)
{
    /* we expect this call to fail often, so we do not check */
    shmctl(shmid, IPC_RMID, 0);
}

void* apc_shm_attach(int shmid)
{
    void* shmaddr;  /* the shared memory address */

    if ((int)(shmaddr = shmat(shmid, 0, 0)) == -1) {
        apc_eprint("apc_shm_attach: shmat failed:");
    }

    /*
     * We set the shmid for removal immediately after attaching to it. The
     * segment won't disappear until all processes have detached from it.
     */
    apc_shm_destroy(shmid);
    return shmaddr;
}

void apc_shm_detach(void* shmaddr)
{
    if (shmdt(shmaddr) < 0) {
        apc_eprint("apc_shm_detach: shmdt failed:");
    }
}

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */
