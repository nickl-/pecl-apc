#include "apc_shm.h"
#include "apc_lib.h"
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>

#ifndef SHM_R
# define SHM_R 0444	/* read permission */
#endif
#ifndef SHM_A
# define SHM_A 0222	/* write permission */
#endif

/* apc_shm_create: create a shared memory segment of given size */
int apc_shm_create(const char* pathname, int proj, int size)
{
	int shmid;	/* shared memory id */
	int oflag;	/* permissions on shm */
	key_t key;	/* shm key returned by ftok */

	key = IPC_PRIVATE;
	if (pathname != NULL) {
		if ((key = ftok(pathname, proj)) < 0) {
			apc_eprint("apc_shm_create: ftok failed:");
		}
	}

	oflag = IPC_CREAT | SHM_R | SHM_A;
	if ((shmid = shmget(key, size, oflag)) < 0) {
		apc_eprint("apc_shmcreate: shmget failed:");
	}

	return shmid;
}

/* apc_shm_destroy: remove a shared memory segment */
void apc_shm_destroy(int shmid)
{
	/* we expect this call to fail often, so we do not check */
	shmctl(shmid, IPC_RMID, 0);
}

/* apc_shm_attach: get the address of the beginning of a shared
 * memory segment */
void* apc_shm_attach(int shmid)
{
	void* shmaddr;	/* shared memory address */

	if ((int)(shmaddr = shmat(shmid, 0, 0)) == -1) {
		apc_eprint("apc_shm_attach: shmat failed:");
	}
	return shmaddr;
}

/* apc_shm_detach: detach from a shared memory segment */
void apc_shm_detach(void* shmaddr)
{
	if (shmdt(shmaddr) < 0) {
		apc_eprint("apc_shm_detach: shmdt failed:");
	}
}

