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


#include "apc_cache.h"
#include "apc_crc32.h"
#include "apc_nametable.h"
#include "apc_rwlock.h"
#include "apc_sem.h"
#include "apc_serialize.h"
#include "apc_shm.h"
#include "apc_smm.h"
#include "php_apc.h"
#include "apc_fcntl.h"
#include "apc_iface.h"
#include "apc_lib.h"

#include "zend.h"
#include "zend_hash.h"
#include <sys/types.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define USE_RWLOCK		/* synchronize the cache with a readers-writer lock;
                         * this should be more efficient in many cases, and
						 * where it is not necessary, the extra overhead is
						 * not significant */

#undef USE_FCNTL_LOCK	/* use fcntl locks instead of semaphore locks.
                         * you MUST undef USE_RWLOCK above to make this
						 * take effect. This is experimental */

enum { MAX_KEY_LEN = 256 };			/* must be >= maximum path length */
enum { DO_CHECKSUM = 0 };			/* if this is true, perform checksums */

extern zend_apc_globals apc_globals;

/* forward declarations of static retrieval functions */
static int apc_cache_retrieve_safe(apc_cache_t*, const char*, char**,
	int*, int*, int);
static int apc_cache_retrieve_fast(apc_cache_t*, const char*, char**,
	int*, int*, int);

/* declare retrieval function pointer and initialize to "safe" mode */
static int retrievaltype = APC_CACHE_RT_SAFE;
int (*apc_cache_retrieve)(apc_cache_t*, const char*, char**,
	int*, int*, int) = apc_cache_retrieve_safe;

typedef struct segment_t segment_t;
struct segment_t {
	int shmid;	/* shared memory id of the segment */
};

/* bucket_t. When a bucket is empty, its shmid is equal to EMPTY. After a
 * bucket is removed, its shmid is set to UNUSED, which is considered
 * non-empty when searching, but empty when inserting. Both values are
 * guaranteed to be less than zero */
enum { EMPTY = -1, UNUSED = -2 };
typedef struct bucket_t bucket_t;
struct bucket_t {
	char key[MAX_KEY_LEN+1];		/* bucket key */
	int shmid;						/* shm segment where data is stored */
	int offset;						/* pointer to data in shm segment */
	int length;						/* length of stored data in bytes */
	int hitcount;					/* number of hits to this bucket */
	int ttl;						/* private time-to-live */
	time_t lastaccess;				/* time of last access */
	time_t createtime;				/* time of creation */
	time_t mtime;					/* modification time of the source file */
	unsigned int checksum;			/* checksum of stored data */
};

typedef struct header_t header_t;
struct header_t {
	int magic;		/* magic number, indicates initialization state */
	int nbuckets;	/* number of buckets in cache */
	int maxseg;		/* maximum number of segments for cached data */
	int segsize;	/* size of each shared memory segment */
	int ttl;		/* default time to live for cache entries */
	int hits;		/* total successful hits in cache */
	int misses;		/* total unsuccessful hits in cache */
};

struct apc_cache_t {
	header_t* header;	/* cache header, stored in shared memory */
	char* pathname;		/* pathname used to create cache */
  #ifdef USE_RWLOCK
	apc_rwlock_t* lock;	/* readers-writer lock for entire cache */
  #else
	int lock;			/* binary semaphore lock */
  #endif
	int shmid;			/* shared memory segment of cache */
	void* shmaddr;		/* process (local) address of cache shm segment */
	segment_t* segments;/* start of segment_t array */
	bucket_t* buckets;	/* start of bucket_t array */
	apc_nametable_t* lcache;	/* local cache for APC_CACHE_RT_FAST */
};

/* a local partial-bucket structure */
typedef struct lbucket_t lbucket_t;
struct lbucket_t {
	int shmid;				/* all members copied from bucket_t */
	int offset;
	int length;
	time_t mtime;
};

/* if USE_RWLOCK is defined, a readers-write lock (defined in apc_rwlock.c)
 * will be used to synchronize the shared cache. otherwise, a simple binary
 * semaphore will be used. */

#if defined(USE_RWLOCK)
# define READLOCK(lock)  apc_rwl_readlock(lock)
# define WRITELOCK(lock) apc_rwl_writelock(lock)
# define UNLOCK(lock)    apc_rwl_unlock(lock)
#elif defined(USE_FCNTL_LOCK)
# define READLOCK(lock)  lock_reg(lock, F_SETLKW, F_RDLCK, 0, SEEK_SET, 0)
# define WRITELOCK(lock) lock_reg(lock, F_SETLKW, F_WRLCK, 0, SEEK_SET, 0)
# define UNLOCK(lock)    lock_reg(lock, F_SETLKW, F_UNLCK, 0, SEEK_SET, 0)
#else
# define READLOCK(lock)  apc_sem_lock(lock)
# define WRITELOCK(lock) apc_sem_lock(lock)
# define UNLOCK(lock)    apc_sem_unlock(lock)
#endif

#define MAGIC_INIT  0xC1A5 	/* magic initialization value */

/* computecachesize: compute size of cache, given nbuckets and maxseg */
static int computecachesize(int nbuckets, int maxseg)
{
	return sizeof(header_t) + maxseg*sizeof(segment_t) +
		nbuckets*sizeof(bucket_t);
}

/* isexpired: return true if bucket has expired */
static int isexpired(bucket_t* b, int mtime)
{
	/* if the time-to-live of this entry has been exceeded, or if the
	 * file modification time has increased, the entry is expired.
	 * note that a time-to-live of zero means "never expire" */

	return (b->ttl != 0 && time(0) > b->createtime + b->ttl) ||
	       (mtime > b->mtime);
}

/* initcache: perform full initialization of the cache. should execute once
 * for all processes (not for each process) */
static void initcache(apc_cache_t* cache, const char* pathname,
	int nbuckets, int maxseg, int segsize, int ttl)
{
	int cachesize;	/* total size of the cache */
	header_t* header;
	int i;

	cachesize = computecachesize(nbuckets, maxseg);

	memset(cache->shmaddr, 0, cachesize);
	header = cache->header;
	header->magic    = MAGIC_INIT;
	header->nbuckets = nbuckets;
	header->maxseg   = maxseg;
	header->segsize  = segsize;
	header->ttl      = ttl;
	header->hits     = 0;
	header->misses   = 0;

	/* initialize all the buckets */
	for (i = 0; i < nbuckets; i++) {
		cache->buckets[i].shmid = EMPTY;
	}

	/* create the first shared memory segment */
/*
	cache->segments[0].shmid  = apc_shm_create(pathname, 1, segsize);
	apc_smm_initsegment(cache->segments[0].shmid, segsize);
*/
}


/* resetcache: clears all entries from the cache without destroying existing
 * shared memory segments. assumes cache is locked for writing */
static void resetcache(apc_cache_t* cache)
{
	int nbuckets;
	int maxseg;
	int segsize;
	int i;

	/* reset buckets */
	nbuckets = cache->header->nbuckets;
	for (i = 0; i < nbuckets; i++) {
		if (cache->buckets[i].shmid >= 0) {
			void* shmaddr = apc_smm_attach(cache->buckets[i].shmid);
			apc_smm_free(shmaddr, cache->buckets[i].offset);
			cache->buckets[i].shmid = EMPTY;
		}
	}

	/* reset shared memory segments */
	maxseg  = cache->header->maxseg;
	segsize = cache->header->segsize;
	for (i = 0; i < maxseg; i++) {
		if (cache->segments[i].shmid == 0) {
			break;
		}
		apc_smm_initsegment(cache->segments[i].shmid, segsize);
	}

	/* reset header */
	cache->header->hits = cache->header->misses = 0;

	/* remove entries from local cache */
	apc_nametable_clear(cache->lcache, apc_efree);
}


/* emptybucket: clean out a bucket and free associated memory. assumes
 * cache is locked for writing */
static void emptybucket(bucket_t* bucket)
{
	void* shmaddr = apc_smm_attach(bucket->shmid);
	apc_smm_free(shmaddr, bucket->offset);
	bucket->shmid = UNUSED;
}


/* hash: compute hash value of a string */
static unsigned int hash(const char* v)
{
	unsigned int h = 0;
	for (; *v != 0; v++) {
		h = 127*h + *v;
	}
	return h;
}

/* hashtwo: second hash function for double hashing */
static unsigned int hashtwo(const char* v)
{
	unsigned int h = 0;
	for (; *v != 0; v++) {
		h = 37*h + *v;
	}
	return (h % 97) + 1; /* works well when cache size is <97 */
}

/* apc_cache_create: create a new cache */
apc_cache_t* apc_cache_create(const char* pname, int nbuckets,
	int maxseg, int segsize, int ttl)
{
	char* pathname;
	apc_cache_t* cache;
	int cachesize;

	#ifdef USE_FCNTL_LOCK
		pathname = apc_estrdup("/tmp/.apc.lock");
	#else
		pathname = (char *) pname;
	#endif
	if (apc_create_lock(pathname) < 0 ) {
	
	}	
	cache = (apc_cache_t*) apc_emalloc(sizeof(apc_cache_t));
	cachesize = computecachesize(nbuckets, maxseg);

	/* per-process initialization */
	cache->pathname = (char*) apc_estrdup(pathname);
  #ifdef USE_RWLOCK
	cache->lock     = apc_rwl_create(pathname);
  #elif defined(USE_FCNTL_LOCK)
  	cache->lock		= apc_flock_create(pathname);
  #else
	cache->lock     = apc_sem_create(pathname, 1, 1);
  #endif
	cache->shmid    = apc_shm_create(pathname, 0, cachesize);
	cache->shmaddr  = apc_shm_attach(cache->shmid);
	cache->header   = (header_t*) cache->shmaddr;
	cache->lcache	= apc_nametable_create(nbuckets);

	/* cache->segments and cache->buckets are "convenience" pointers
	 * to the beginning of buckets and of segments, respectively, in
	 * shared memory */

	cache->segments = (segment_t*)
		((char*) cache->shmaddr + sizeof(header_t));
	cache->buckets = (bucket_t*)
		((char*) cache->shmaddr + sizeof(header_t) + maxseg*sizeof(segment_t));

	/* instruct the OS to destroy the shm segment as soon as no processes
	 * are attached to it */
	apc_shm_destroy(cache->shmid);

	/* perform full initialization if necessary */
	if (cache->header->magic != MAGIC_INIT) {
		WRITELOCK(cache->lock);
		if (cache->header->magic != MAGIC_INIT) {
			/* cache not initialized (check twice to avoid race cond.) */
			initcache(cache, pathname, nbuckets, maxseg, segsize, ttl);
		}
		UNLOCK(cache->lock);
	}

	/* initialize retrieval type based on the php ini var 'cache_rt' */
	apc_cache_setretrievaltype(APCG(cache_rt));

	return cache;
}

/* apc_cache_destroy: destroys a cache */
void apc_cache_destroy(apc_cache_t* cache)
{
	int i;
	int maxseg;

	WRITELOCK(cache->lock);

	/* first, destroy the shared memory segments */
	maxseg = cache->header->maxseg;
	for (i = 0; i < maxseg; i++) {
		if (cache->segments[i].shmid != 0) {
			apc_shm_destroy(cache->segments[i].shmid);
		}
	}
	apc_shm_detach(cache->shmaddr);
	apc_shm_destroy(cache->shmid);

	UNLOCK(cache->lock);	/* race condition here! */

  #ifdef USE_RWLOCK
	apc_rwl_destroy(cache->lock);
  #else
	apc_sem_destroy(cache->lock);
  #endif

	apc_nametable_clear(cache->lcache, free);
	apc_nametable_destroy(cache->lcache);

	apc_efree(cache);
}

/* apc_cache_setretrievaltype: sets the cache retrieval method */
int apc_cache_setretrievaltype(int type)
{
	switch (type) {
	  case APC_CACHE_RT_SAFE:
		retrievaltype = type;
		apc_cache_retrieve = apc_cache_retrieve_safe;
		break;
	  case APC_CACHE_RT_FAST:
		retrievaltype = type;
		apc_cache_retrieve = apc_cache_retrieve_fast;
		break;
	  default:
		return -1; /* unsupported type */
	}
	return 0;
}

/* apc_cache_clear: clears the cache */
void apc_cache_clear(apc_cache_t* cache)
{
	WRITELOCK(cache->lock);
	resetcache(cache);
	UNLOCK(cache->lock);
}

/* apc_cache_search: return 1 if key exists in cache, else 0 */
int apc_cache_search(apc_cache_t* cache, const char* key)
{
	unsigned slot;		/* initial hash value */
	unsigned k;			/* second hash value, for open-addressing */
	int nprobe;			/* running count of cache probes */
	bucket_t* buckets;
	int nbuckets;
	size_t keylen;

	if (!key) {
		return 0;
	}
	keylen = strlen(key);

	READLOCK(cache->lock);

	buckets  = cache->buckets;
	nbuckets = cache->header->nbuckets;

	slot = hash(key) % nbuckets;
	k = hashtwo(key) % nbuckets;

	nprobe = 0;
	while (buckets[slot].shmid != EMPTY && nprobe++ < nbuckets) {
		if (buckets[slot].shmid == UNUSED) {
			continue;
		}
		if (strncmp(buckets[slot].key, key, keylen) == 0) {
			if (isexpired(&buckets[slot], 0)) {
				break; /* the entry has expired */
			}
			UNLOCK(cache->lock);
			return 1;
		}
		slot = (slot+k) % nbuckets;
	}
	UNLOCK(cache->lock);
	return 0; /* not found */
}

/* apc_cache_retrieve_fast */
static int apc_cache_retrieve_fast(apc_cache_t* cache, const char* key,
	char** dataptr, int* length, int* maxsize, int mtime)
{
	unsigned slot;		/* initial hash value */
	unsigned k;			/* second hash value, for open-addressing */
	int nprobe;			/* running count of cache probes */
	char* shmaddr;		/* attached addr of data segment */
	bucket_t* buckets;
	int nbuckets;
	unsigned int checksum;
	size_t keylen;

	/* this version of apc_cache_retrieve does not synchronize on reads of
	 * the shared cache index, which may not be unworkably risky for these 
	 * reason: (1) writes to the index tend (in certain common domains) to
	 * come all-at-once at server-startup, (2) the cache structure is robust
	 * with respect to concurrent readers and a single writer (that is, the
	 * reader may end up with undefined data but will not enter an infinite
	 * loop or crash), and (3) the order in which writers write makes it
	 * easy for readers to confirm that their results are well-defined */

	if (!key) {
		return 0;
	}
	keylen = strlen(key);

	buckets  = cache->buckets;
	nbuckets = cache->header->nbuckets;

	slot = hash(key) % nbuckets;
	k = hashtwo(key) % nbuckets;

	nprobe = 0;
	while (buckets[slot].shmid != EMPTY && nprobe++ < nbuckets) {
		if (buckets[slot].shmid == UNUSED) {
			continue;
		}
		if (strncmp(buckets[slot].key, key, keylen) == 0) {
			lbucket_t* lbucket;

			if (isexpired(&buckets[slot], mtime)) {
				break; /* the entry has expired */
			}

			/* compare entry with locally cached partial entry */
			lbucket = apc_nametable_retrieve(cache->lcache, key);
			if (lbucket == 0                                    ||
			    lbucket->shmid      != buckets[slot].shmid      ||
				lbucket->offset     != buckets[slot].offset     ||
				lbucket->length     != buckets[slot].length     ||
				lbucket->mtime      != buckets[slot].mtime)
			{
				/* we got ill-defined results. resort to safe retrieval */
				return apc_cache_retrieve_safe(cache, key, dataptr,
					length, maxsize, mtime);
			}

			shmaddr = (char*) apc_smm_attach(buckets[slot].shmid);
			*length = buckets[slot].length;
			if (*maxsize < *length) {
				/* dataptr is too small, so expand it */
				*maxsize = *length;
				*dataptr = realloc(*dataptr, *maxsize);
			}
			memcpy(*dataptr, shmaddr + buckets[slot].offset, *length);

			/* compare checksums */
			if (DO_CHECKSUM && checksum != apc_crc32(*dataptr, *length)) {
				apc_eprint("checksum failed! data length is %d\n", *length);
				return 0; /* return failure */
			}

			/* we're done */
			return 1;
		}
		slot = (slot+k) % nbuckets;
	}
	return 0;
}

/* apc_cache_retrieve_safe */
static int apc_cache_retrieve_safe(apc_cache_t* cache, const char* key,
	char** dataptr, int* length, int* maxsize, int mtime)
{
	unsigned slot;		/* initial hash value */
	unsigned k;			/* second hash value, for open-addressing */
	int nprobe;			/* running count of cache probes */
	char* shmaddr;		/* attached addr of data segment */
	bucket_t* buckets;
	int nbuckets;
	unsigned int checksum;
	size_t keylen;

	if (!key) {
		return 0;
	}
	keylen = strlen(key);

	READLOCK(cache->lock);

	buckets  = cache->buckets;
	nbuckets = cache->header->nbuckets;

	slot = hash(key) % nbuckets;
	k = hashtwo(key) % nbuckets;

	nprobe = 0;
	while (buckets[slot].shmid != EMPTY && nprobe++ < nbuckets) {
		if (buckets[slot].shmid == UNUSED) {
			continue;
		}
		if (strncmp(buckets[slot].key, key, keylen) == 0) {
			lbucket_t* lbucket;

			if (isexpired(&buckets[slot], mtime)) {
				break; /* the entry has expired */
			}

			shmaddr = (char*) apc_smm_attach(buckets[slot].shmid);
			*length = buckets[slot].length;
			if (*maxsize < *length) {
				/* dataptr is too small, so expand it */
				*maxsize = *length;
				*dataptr = realloc(*dataptr, *maxsize);
			}
			memcpy(*dataptr, shmaddr + buckets[slot].offset, *length);

			/* update the cache */
			cache->header->hits++;
			buckets[slot].lastaccess = time(0);
			buckets[slot].hitcount++;

			UNLOCK(cache->lock);

			/* compare checksums */
			if (DO_CHECKSUM && checksum != apc_crc32(*dataptr, *length)) {
				apc_eprint("checksum failed! data length is %d\n", *length);
				return 0; /* return failure */
			}

			/* if fast retrieval is active, cache some pertinent data in
			 * local process-memory */
			if (!retrievaltype == APC_CACHE_RT_FAST) {
				lbucket = apc_nametable_retrieve(cache->lcache, key);

				if (lbucket == 0) {
					lbucket = (lbucket_t*) apc_emalloc(sizeof(lbucket_t));
					apc_nametable_insert(cache->lcache, key, lbucket);
				}
				lbucket->shmid      = buckets[slot].shmid;
				lbucket->offset     = buckets[slot].offset;
				lbucket->length     = buckets[slot].length;
				lbucket->mtime      = buckets[slot].mtime;
			}

			/* we're done */
			return 1;
		}
		slot = (slot+k) % nbuckets;
	}
	cache->header->misses++;
	UNLOCK(cache->lock);
	return 0;
}

/* apc_cache_insert: insert entry into cache */
int apc_cache_insert(apc_cache_t* cache, const char* key,
	const char* data, int size, int mtime)
{
	int i;
	unsigned slot;		/* initial hash value */
	unsigned k;			/* second hash value, for open-addressing */
	int nprobe;			/* running count of cache probes */
	char* shmaddr;		/* attached addr of current segment */
	bucket_t* buckets;
	int nbuckets;
	segment_t* segments;
	int maxseg;
	int segsize;
	int offset;
	unsigned int checksum;
	size_t keylen;

	if (!key) {
		return 0;
	}
	keylen = strlen(key);

	/* compute checksum of data (before locking) */
	checksum = DO_CHECKSUM ? apc_crc32(data, size) : 0;

	WRITELOCK(cache->lock);
	
	/* copy these values out of shared memory, for convenience */
	buckets  = cache->buckets;
	nbuckets = cache->header->nbuckets;
	segments = cache->segments;
	maxseg   = cache->header->maxseg;
	segsize  = cache->header->segsize;

	slot = hash(key) % nbuckets;
	k = hashtwo(key) % nbuckets;

	nprobe = 0;
	while (buckets[slot].shmid >= 0 && nprobe++ < nbuckets) {
		if (strncmp(buckets[slot].key, key, keylen) == 0) {
			emptybucket(&buckets[slot]);
			break;	/* overwrite existing entry */
		}
		if (isexpired(&buckets[slot], 0)) {
			emptybucket(&buckets[slot]);
			break;	/* this entry has expired, overwrite it */
		}
		slot = (slot+k) % nbuckets;
	}
	if (nprobe == nbuckets) {	/* did we find a slot? */
		UNLOCK(cache->lock);	/* no, return failure */
		return -1;
	}

	shmaddr = 0;
	offset = 0;
	for (i = 0; i < maxseg; i++) {
		if (segments[i].shmid == 0) { /* segment not initialized */
			segments[i].shmid = apc_shm_create(cache->pathname, i+1, segsize);
			apc_smm_initsegment(segments[i].shmid, segsize);
		}
		shmaddr = apc_smm_attach(segments[i].shmid);
		offset = apc_smm_alloc(shmaddr, size);
		if (offset >= 0) {
			break;
		}
	}
	if (i == maxseg) {
		/* not enough shared memory available */
		UNLOCK(cache->lock);
		return -1;
	}

	/* store data in segment and update its record */
	memcpy(shmaddr + offset, data, size);

	/* update the cache */
	strncpy(buckets[slot].key, key, MAX_KEY_LEN);
	buckets[slot].shmid      = segments[i].shmid;
	buckets[slot].offset     = offset;
	buckets[slot].length     = size;
	buckets[slot].hitcount   = 0;
	buckets[slot].checksum   = checksum;
	buckets[slot].ttl		 = cache->header->ttl;
	buckets[slot].lastaccess = buckets[slot].createtime;
	buckets[slot].createtime = time(0);
	buckets[slot].mtime      = mtime;
	
	UNLOCK(cache->lock);
	return 0;
}

/* apc_shm_rm: remove entry from cache */
int apc_shm_rm(apc_cache_t* cache, const char* key)
{
	unsigned slot;		/* initial hash value */
	unsigned k;			/* second hash value, for open-addressing */
	int nprobe;			/* running count of cache probes */
	bucket_t* buckets;
	int nbuckets;
	size_t keylen;

	if (!key) {
		return 0;
	}
	keylen = strlen(key);

	WRITELOCK(cache->lock);

	buckets  = cache->buckets;
	nbuckets = cache->header->nbuckets;

	slot = hash(key) % nbuckets;
	k = hashtwo(key) % nbuckets;

	nprobe = 0;
	while (buckets[slot].shmid != EMPTY && nprobe++ < nbuckets) {
		if (buckets[slot].shmid == UNUSED) {
			continue;
		}
		if (strncmp(buckets[slot].key, key, keylen) == 0) {
			/* found the key */
			emptybucket(&buckets[slot]);
			UNLOCK(cache->lock);
			return 1;
		}
		slot = (slot+k) % nbuckets;
	}
	UNLOCK(cache->lock);
	return 0;	/* not found */
}

int apc_cache_set_object_ttl(apc_cache_t* cache, const char* key, int ttl)
{
	unsigned slot;		/* initial hash value */
	unsigned k;			/* second hash value, for open-addressing */
	int nprobe;			/* running count of cache probes */
	bucket_t* buckets;
	int nbuckets;
	size_t keylen;

	if (!key) {
		return 0;
	}
	keylen = strlen(key);

	WRITELOCK(cache->lock);

	buckets  = cache->buckets;
	nbuckets = cache->header->nbuckets;

	slot = hash(key) % nbuckets;
	k = hashtwo(key) % nbuckets;

	nprobe = 0;
	while (buckets[slot].shmid != EMPTY && nprobe++ < nbuckets) {
		if (buckets[slot].shmid == UNUSED) {
			continue;
		}
		if (strncmp(buckets[slot].key, key, keylen) == 0) {
			/* found the key */
			buckets[slot].ttl = ttl;
			UNLOCK(cache->lock);
			return 1;
		}
		slot = (slot+k) % nbuckets;
	}
	UNLOCK(cache->lock);
	return 0; /* key doesn't exist */
}

/* apc_cache_dump: outputs cache information as HTML */
void apc_cache_dump(apc_cache_t* cache, const char* linkurl,
	apc_outputfn_t outputfn)
{
	int i,j;
	double hitrate;

	READLOCK(cache->lock);

	hitrate = (1.0 * cache->header->hits) /
		(cache->header->hits + cache->header->misses);

	outputfn("<html>\n");

	/* display HEAD */
	outputfn("<head>\n");
	outputfn("<title>APC-SHM Cache Info</title>\n");
	outputfn("</head>\n");

	/* display cache header info */
	outputfn("<table border=1 cellpadding=2 cellspacing=1>\n");
	outputfn("<tr>\n");
	outputfn("<td colspan=2 bgcolor=#dde4ff>Cache Header</td>\n");
	outputfn("<tr>\n");
	outputfn("<td bgcolor=#ffffff>Name</td>\n");
	outputfn("<td bgcolor=#ffffff>Value</td>\n");
	outputfn("<tr>\n");
	outputfn("<td bgcolor=#eeeeee>magic</td>\n");
	outputfn("<td bgcolor=#eeeeee>0x%x</td>\n", cache->header->magic);
	outputfn("<tr>\n");
	outputfn("<td bgcolor=#eeeeee>total buckets</td>\n");
	outputfn("<td bgcolor=#eeeeee>%d</td>\n", cache->header->nbuckets);
	outputfn("<tr>\n");
	outputfn("<td bgcolor=#eeeeee>maximum shared memory segments</td>\n");
	outputfn("<td bgcolor=#eeeeee>%d</td>\n", cache->header->maxseg);
	outputfn("<tr>\n");
	outputfn("<td bgcolor=#eeeeee>shared memory segment size (B)</td>\n");
	outputfn("<td bgcolor=#eeeeee>%d</td>\n", cache->header->segsize);
	outputfn("<tr>\n");
	outputfn("<td bgcolor=#eeeeee>time-to-live</td>\n");
	outputfn("<td bgcolor=#eeeeee>%d</td>\n", cache->header->ttl);
	outputfn("<tr>\n");
	outputfn("<td bgcolor=#eeeeee>hits</td>\n");
	outputfn("<td bgcolor=#eeeeee>%d</td>\n", cache->header->hits);
	outputfn("<tr>\n");
	outputfn("<td bgcolor=#eeeeee>misses</td>\n");
	outputfn("<td bgcolor=#eeeeee>%d</td>\n", cache->header->misses);
	outputfn("<tr>\n");
	outputfn("<td bgcolor=#eeeeee>hit rate</td>\n");
	outputfn("<td bgcolor=#eeeeee>%.2f</td>\n", hitrate);
	outputfn("<tr>\n");
	for(j = 0; j < APCG(nmatches); j++) {
		outputfn("<td bgcolor=#eeeeee>cache filter (%d)</td>\n", j);
		outputfn("<td bgcolor=#eeeeee>%s</td>\n", APCG(regex_text)[j]? APCG(regex_text)[j]: "(none)");
	outputfn("<tr>\n");
	}
	outputfn("<td colspan=2 bgcolor=#ffffff>local information</td>\n");
	outputfn("<tr>\n");
	outputfn("<td bgcolor=#eeeeee>shared memory ID</td>\n");
	outputfn("<td bgcolor=#eeeeee>%d</td>\n", cache->shmid);
	outputfn("<tr>\n");
	outputfn("<td bgcolor=#eeeeee>local shared memory address</td>\n");
	outputfn("<td bgcolor=#eeeeee>%p</td>\n", cache->shmaddr);
	outputfn("<tr>\n");
	outputfn("<td bgcolor=#eeeeee>creation pathname</td>\n");
	outputfn("<td bgcolor=#eeeeee>%s</td>\n",
		cache->pathname ? cache->pathname : "(null)");
	outputfn("</table>\n");
	outputfn("<br>\n");
	outputfn("<br>\n");

	/* display bucket info */
	outputfn("<table border=1 cellpadding=2 cellspacing=1>\n");
	outputfn("<tr><form method=post action=%s>\n", linkurl);
	outputfn("<td colspan=%d bgcolor=#dde4ff>Bucket Data</td>\n",
		(linkurl != 0) ? 10 : 9);
	outputfn("<tr>\n");
	outputfn("<td bgcolor=#ffffff>Index</td>\n");
	if (linkurl != 0) {
		outputfn("<td bgcolor=#ffffff>Delete</td>\n");
	}
	outputfn("<td bgcolor=#ffffff>Key</td>\n");
	outputfn("<td bgcolor=#ffffff>Offset</td>\n");
	outputfn("<td bgcolor=#ffffff>Length (B)</td>\n");
	outputfn("<td bgcolor=#ffffff>Last Access</td>\n");
	outputfn("<td bgcolor=#ffffff>Hits</td>\n");
	outputfn("<td bgcolor=#ffffff>Expire Time</td>\n");
	outputfn("<td bgcolor=#ffffff>Modification Time</td>\n");
	outputfn("<td bgcolor=#ffffff>Checksum</td>\n");

	for (i = 0; i < cache->header->nbuckets; i++) {
		bucket_t* bucket;
		if (cache->buckets[i].shmid < 0) {
			continue;
		}
		bucket = &(cache->buckets[i]);
		outputfn("<tr>\n");
		outputfn("<td bgcolor=#eeeeee>%d</td>\n", i);

        if (linkurl != 0) {
            outputfn("<td bgcolor=#eeeeee><input type=checkbox "
			         "name=apc_rm[] value=%s>&nbsp</td>\n",
					 bucket->key);
            outputfn("<td bgcolor=#eeeeee><a href=%s?apc_info=%s>"
			         "%s</a></td>\n", linkurl, bucket->key, bucket->key);
		}
        else {
            outputfn("<td bgcolor=#eeeeee>%s</td>\n", bucket->key);
        }

		outputfn("<td bgcolor=#eeeeee>%d</td>\n", bucket->offset);
		outputfn("<td bgcolor=#eeeeee>%d</td>\n", bucket->length);
		outputfn("<td bgcolor=#eeeeee>%d</td>\n", bucket->lastaccess);
		outputfn("<td bgcolor=#eeeeee>%d</td>\n", bucket->hitcount);
		outputfn("<td bgcolor=#eeeeee>%d</td>\n",
			(bucket->ttl != 0) ? bucket->createtime + bucket->ttl : 0);
		outputfn("<td bgcolor=#eeeeee>%d</td>\n", bucket->mtime);
		outputfn("<td bgcolor=#eeeeee>%u</td>\n", bucket->checksum);
	}

	if (linkurl != 0) {
		outputfn("<tr><td><input type=submit name=submit "
		         "value=\"Delete Marked Objects\"></tr></td>\n");
	}
	outputfn("</form>\n");
	outputfn("</table>\n");
	outputfn("<br>\n");
	outputfn("<br>\n");

	/* display shared memory segment info */
	outputfn("<table border=1 cellpadding=2 cellspacing=1>\n");
	outputfn("<tr>\n");
	outputfn("<td bgcolor=#dde4ff>Shared Memory Info</td>\n");
	for (i = 0; i < cache->header->maxseg; i++) {
		if (cache->segments[i].shmid > 0) {
			outputfn("<tr>\n");
			outputfn("<td>\n");
			apc_smm_dump(apc_smm_attach(cache->segments[i].shmid), outputfn);
			outputfn("</td>\n");
		}
	}
	outputfn("</table>\n");

	outputfn("<br>\n");
	outputfn("<br>\n");

	outputfn("</html>\n");

	UNLOCK(cache->lock);
}

int apc_cache_dump_entry(apc_cache_t* cache, const char* key,
	apc_outputfn_t outputfn)
{
	static const char NBSP[] = "&nbsp;";
	unsigned slot;		/* initial hash value */
	unsigned k;			/* second hash value, for open-addressing */
	int nprobe;			/* running count of cache probes */
	bucket_t* bp;		/* pointer to matching bucket */
	bucket_t* buckets;
	int nbuckets;
	int i;

	HashTable function_table;
	HashTable class_table;
	apc_nametable_t* dummy;
	zend_op_array* op_array;
	Bucket* p;
	Bucket* q;

	if (!key) {
		return 0;
	}

	READLOCK(cache->lock);

	buckets  = cache->buckets;
	nbuckets = cache->header->nbuckets;

	slot = hash(key) % nbuckets;
	k = hashtwo(key) % nbuckets;

	nprobe = 0;
	bp = 0;
	while (buckets[slot].shmid != EMPTY && nprobe++ < nbuckets) {
		if (buckets[slot].shmid == UNUSED) {
			continue;
		}
		if (strcmp(buckets[slot].key, key) == 0) {
			if (isexpired(&buckets[slot], 0)) {
				break; /* the entry has expired */
			}
			bp = &buckets[slot];
			break;
		}
		slot = (slot+k) % nbuckets;
	}

	if (!bp) {
		UNLOCK(cache->lock);
		return -1;
	}

	/* begin outer table */
	outputfn("<table border=0 cellpadding=2 cellspacing=1>\n");

	/* begin first row of outer table */
	outputfn("<tr>\n");

	/* display bucket info */
	outputfn("<td colspan=3 valign=top>");
	outputfn("<table border=1 cellpadding=2 cellspacing=1>\n");
	outputfn("<tr>\n");
	outputfn("<td colspan=9 bgcolor=#dde4ff>Bucket Data</td>\n");
	outputfn("<tr>\n");
	outputfn("<td bgcolor=#ffffff>Key</td>\n");
	outputfn("<td bgcolor=#ffffff>Offset</td>\n");
	outputfn("<td bgcolor=#ffffff>Length (B)</td>\n");
	outputfn("<td bgcolor=#ffffff>Last Access</td>\n");
	outputfn("<td bgcolor=#ffffff>Hits</td>\n");
	outputfn("<td bgcolor=#ffffff>Expire Time</td>\n");
	outputfn("<td bgcolor=#ffffff>Modification Time</td>\n");
	outputfn("<td bgcolor=#ffffff>Checksum</td>\n");
	outputfn("<tr>\n");
	outputfn("<td bgcolor=#eeeeee>%s</td>\n", bp->key);
	outputfn("<td bgcolor=#eeeeee>%d</td>\n", bp->offset);
	outputfn("<td bgcolor=#eeeeee>%d</td>\n", bp->length);
	outputfn("<td bgcolor=#eeeeee>%d</td>\n", bp->lastaccess);
	outputfn("<td bgcolor=#eeeeee>%d</td>\n", bp->hitcount);
	outputfn("<td bgcolor=#eeeeee>%d</td>\n",
			(bp->ttl != 0) ? bp->createtime + bp->ttl : 0);
	outputfn("<td bgcolor=#eeeeee>%d</td>\n", bp->mtime);
	outputfn("<td bgcolor=#eeeeee>%u</td>\n", bp->checksum);
	outputfn("</table>\n");
	outputfn("</td>\n");

	/* end first row of outer table */
	outputfn("</tr>\n");

	op_array = (zend_op_array*) malloc(sizeof(zend_op_array));
	zend_hash_init(&function_table, 13, NULL, NULL, 1);
	zend_hash_init(&class_table, 13, NULL, NULL, 1);
	dummy = apc_nametable_create(97);

	/* deserialize bucket and see what's inside */
	apc_init_deserializer((char *) ((char *)apc_smm_attach(bp->shmid) + bp->offset), bp->length);
	apc_deserialize_magic();
	apc_deserialize_zend_function_table(&function_table, dummy, dummy);
	apc_deserialize_zend_class_table(&class_table, dummy, dummy);
	apc_deserialize_zend_op_array(op_array);

	/* begin second row of outer table */
	outputfn("<tr>\n");

	/* display opcodes in the entry */
	outputfn("<td valign=top>");
	outputfn("<table border=1 cellpadding=2 cellspacing=1>\n");
	outputfn("<tr>\n");
	outputfn("<td colspan=3 bgcolor=#dde4ff>OpCodes</td>\n");
	outputfn("<tr>\n");
	outputfn("<td bgcolor=#ffffff>Value</td>\n");
	outputfn("<td bgcolor=#ffffff>Extended</td>\n");
	outputfn("<td bgcolor=#ffffff>Line</td>\n");
	for (i = 0; i < op_array->last; i++) {
		const char* name;

		outputfn("<tr>\n");

		/* print the regular opcode, or '&nbsp;' if empty */
		name = apc_get_zend_opname(op_array->opcodes[i].opcode);
		outputfn("<td bgcolor=#eeeeee>%s</td>\n", name);

		/* print the extended opcode, or '&nbsp;' if empty */
		if (op_array->opcodes[i].opcode != ZEND_NOP &&
			op_array->opcodes[i].opcode != ZEND_DECLARE_FUNCTION_OR_CLASS)
		{
			/* this opcode does not have an extended value */
			name = NBSP;
		}
		else {
			name = apc_get_zend_extopname(op_array->opcodes[i].extended_value);
		}
		outputfn("<td bgcolor=#eeeeee>%s</td>\n", name);

		/* print the line number of the opcode */
		outputfn("<td bgcolor=#eeeeee>%u</td>\n", op_array->opcodes[i].lineno);
	}
	outputfn("</table>\n");
	outputfn("</td>\n");

	/* display functions in the entry */
	outputfn("<td valign=top>");
	outputfn("<table border=1 cellpadding=2 cellspacing=1>\n");
	outputfn("<tr>\n");
	outputfn("<td bgcolor=#dde4ff>Functions</td>\n");
	p = function_table.pListHead;
	while (p) {
		zend_function* zf = (zend_function*) p->pData;
		outputfn("<tr>\n");
		outputfn("<td bgcolor=#eeeeee>%s</td>\n",
			zf->common.function_name);
		p = p->pListNext;
	}
	outputfn("</table>\n");
	outputfn("</td>\n");

	/* display classes in the entry */
	outputfn("<td valign=top>");
	outputfn("<table border=1 cellpadding=2 cellspacing=1>\n");
	outputfn("<tr>\n");
	outputfn("<td bgcolor=#dde4ff>Classes</td>\n");
	outputfn("<tr>\n");
	outputfn("<td bgcolor=#ffffff>Class</td>\n");
	outputfn("<td bgcolor=#ffffff>Function</td>\n");
	p = class_table.pListHead;
	while (p) {
		zend_class_entry* zc = (zend_class_entry*) p->pData;
		outputfn("<tr>\n");
		outputfn("<td bgcolor=#eeeeee>%s</td><td bgcolor=#eeeeee>&nbsp</td>\n",
			zc->name);
        q = zc->function_table.pListHead;
        while(q) {
            zend_function* zf = (zend_function*) q->pData;
            outputfn("<tr>\n");
            outputfn("<td bgcolor=#eeeeee>&nbsp</td>\n");
            outputfn("<td bgcolor=#eeeeee>%s</td>\n",
                zf->common.function_name);
            q = q->pListNext;
        }

		p = p->pListNext;
	}
	outputfn("</table>\n");
	outputfn("</td>\n");

	/* end second row of outer table */
	outputfn("</tr>\n");

	/* close outer table */
	outputfn("</table>\n");

	/* clean up */
	zend_hash_clean(&class_table);
	zend_hash_clean(&function_table);
	destroy_op_array(op_array);
	free(op_array);

	UNLOCK(cache->lock);
	return 0;
}

int apc_cache_index_shm(apc_cache_t* cache, zval **hash) {
	int i;
	READLOCK(cache->lock);

	for (i = 0; i < cache->header->nbuckets; i++) {
  	bucket_t* bucket;
  	zval *array = NULL;
  	if (cache->buckets[i].shmid < 0) {
    	continue;
  	}
	ALLOC_ZVAL(array);
	INIT_PZVAL(array);
  	bucket = &(cache->buckets[i]);
  	if(array_init(array) == FAILURE) {
			UNLOCK(cache->lock);
  	  return 1;
  	}
  	add_next_index_long(array, bucket->offset);
  	add_next_index_long(array, bucket->length);
  	add_next_index_long(array, bucket->lastaccess);
  	add_next_index_long(array, bucket->hitcount);
  	add_next_index_long(array, bucket->ttl);
  	add_next_index_long(array, bucket->mtime);
  	zend_hash_update((*hash)->value.ht, bucket->key, strlen(bucket->key) + 1, (void *) &array, sizeof(zval *), NULL);
	}

	UNLOCK(cache->lock);
	return 0;
}

int apc_cache_info_shm(apc_cache_t* cache, zval **hash) {
        int i,j;
        double hitrate;
        long total_mem;
        long free_mem;
        char buf[20];
	
        READLOCK(cache->lock);

        hitrate = (1.0 * cache->header->hits) /
                (cache->header->hits + cache->header->misses);

        total_mem = 0;
        free_mem  = 0;

        array_init(*hash);

        snprintf(buf, sizeof(buf)-1, "0x%x", cache->header->magic);
        add_assoc_string(*hash, "magic", buf, 1);
				add_assoc_string(*hash, "mode", "SHM", 1);
        add_assoc_string(*hash, "version", (char *)apc_version(), 1);
        add_assoc_long(*hash, "total buckets", cache->header->nbuckets);
        add_assoc_long(*hash, "maximum shared memory segments", cache->header->maxseg);
        add_assoc_long(*hash, "shared memory segment size", cache->header->segsize);
        add_assoc_long(*hash, "time-to-live", cache->header->ttl);
        add_assoc_long(*hash, "hits", cache->header->hits);
        add_assoc_long(*hash, "misses", cache->header->misses);
        add_assoc_double(*hash, "hit rate", hitrate);
		for(j = 0; j < APCG(nmatches); j++) {
			snprintf(buf, sizeof(buf)-1, "cache filter (%d)", j);
        	add_assoc_string(*hash, buf, 
				APCG(regex_text)[j]?APCG(regex_text)[j]:"(none)", 1);
		}
        add_assoc_long(*hash, "shared memory ID", cache->shmid);

        snprintf(buf, sizeof(buf)-1, "0x%x", (int)(cache->shmaddr));
        add_assoc_string(*hash, "local shared memory address", buf, 1);

        add_assoc_string(*hash, "creation pathname", cache->pathname ?cache->pathname : "(null)", 1);

        for (i = 0; i < cache->header->maxseg; i++) {
                if (cache->segments[i].shmid > 0) {
                   apc_smm_memory_info(apc_smm_attach(cache->segments[i].shmid), &total_mem, &free_mem);
                }
        }

        add_assoc_long(*hash, "total size", total_mem);
        add_assoc_long(*hash, "total available", free_mem);
		add_assoc_long(*hash, "check file modification times", APCG(check_mtime));
		add_assoc_long(*hash, "support relative includes", APCG(relative_includes));
		add_assoc_long(*hash, "check for compiled source", APCG(check_compiled_source));

        UNLOCK(cache->lock);
        return 0;
}

int apc_object_info_shm(apc_cache_t* cache, char const*filename, zval **arr) {
  unsigned slot;          /* initial hash value */
  unsigned k;             /* second hash value, for open-addressing */
  int nprobe;             /* running count of cache probes */
  bucket_t* bp;           /* pointer to matching bucket */
  bucket_t* buckets;
  int nbuckets;
  char buf[20];

  HashTable function_table;
  HashTable class_table;
  apc_nametable_t* dummy;
  zend_op_array* op_array;
  Bucket* p;
  Bucket* q;
  int i;

  zval *functions_array = NULL;
  zval *classes_array   = NULL;
  zval *bucket_array    = NULL;
  zval *opcode_array    = NULL;

  printf("apc_object_info_shm(.., %s, ..)\n", filename);
  
  if (!filename) {
          return 0;
  }

  READLOCK(cache->lock);

  if (array_init(*arr) == FAILURE) {
    UNLOCK(cache->lock);
    return 0;
  }

  ALLOC_ZVAL(functions_array);
  INIT_PZVAL(functions_array);
  if(array_init(functions_array) == FAILURE) {
     UNLOCK(cache->lock);
     return 0;
  }

  ALLOC_ZVAL(classes_array);
  INIT_PZVAL(classes_array);
  if(array_init(classes_array) == FAILURE) {
     UNLOCK(cache->lock);
     return 0;
  }

  ALLOC_ZVAL(bucket_array);
  INIT_PZVAL(bucket_array);
  if(array_init(bucket_array) == FAILURE) {
     UNLOCK(cache->lock);
     return 0;
  }

  ALLOC_ZVAL(opcode_array);
  INIT_PZVAL(opcode_array);
  if(array_init(opcode_array) == FAILURE) {
     UNLOCK(cache->lock);
     return 0;
  }

  buckets  = cache->buckets;
  nbuckets = cache->header->nbuckets;

  slot = hash(filename) % nbuckets;
  k = hashtwo(filename) % nbuckets;

  nprobe = 0;
  bp = 0;
  while (buckets[slot].shmid != EMPTY && nprobe++ < nbuckets) {
          if (buckets[slot].shmid == UNUSED) {
                  continue;
          }
          if (strcmp(buckets[slot].key, filename) == 0) {
                  if (isexpired(&buckets[slot], 0)) {
                    break; /* the entry has expired */
                  }
                  bp = &buckets[slot];
                  break;
          }
          slot = (slot+k) % nbuckets;
  }

  if (!bp) {
          UNLOCK(cache->lock);
          return -1;
  }

  op_array = (zend_op_array*) malloc(sizeof(zend_op_array));
  zend_hash_init(&function_table, 13, NULL, NULL, 1);
  zend_hash_init(&class_table, 13, NULL, NULL, 1);
  dummy = apc_nametable_create(97);

  /* deserialize bucket and see what's inside */
  apc_init_deserializer((char *)apc_smm_attach(bp->shmid) + bp->offset, bp->length);
  apc_deserialize_magic();
  apc_deserialize_zend_function_table(&function_table, dummy, dummy);
  apc_deserialize_zend_class_table(&class_table, dummy, dummy);

  apc_deserialize_zend_op_array(op_array);


  add_assoc_string(bucket_array, "key", bp->key, 1);
  add_assoc_long(bucket_array, "offset", bp->offset);
  add_assoc_long(bucket_array, "length", bp->length);
  add_assoc_long(bucket_array, "lastaccess", bp->lastaccess);
  add_assoc_long(bucket_array, "hitcount", bp->hitcount);
  add_assoc_long(bucket_array, "ttl",  bp->ttl);
  add_assoc_long(bucket_array, "mtime",    bp->mtime);
  add_assoc_long(bucket_array, "checksum", bp->checksum);

  zend_hash_update((*arr)->value.ht, "info", strlen("info") + 1, (void *) &bucket_array, sizeof(zval *), NULL);

  p = function_table.pListHead;
  while (p) {
     zend_function* zf = (zend_function*) p->pData;
     add_next_index_string(functions_array, zf->common.function_name, 1);
     p = p->pListNext;
  }
  zend_hash_update((*arr)->value.ht, "functions", strlen("functions") + 1, (void *) &functions_array, sizeof(zval *), NULL);

  p = class_table.pListHead;
  while (p) {
    zval *class_functions = NULL;
    zend_class_entry* zc = (zend_class_entry*) p->pData;

    ALLOC_ZVAL(class_functions);
    INIT_PZVAL(class_functions);
    if(array_init(class_functions) == FAILURE) {
       UNLOCK(cache->lock);
       return 0;
    }

    q = zc->function_table.pListHead;
    while(q) {
      zend_function* zf = (zend_function*) q->pData;
      add_next_index_string(class_functions, zf->common.function_name, 1);
      q = q->pListNext;
    }

    zend_hash_update(classes_array->value.ht, zc->name, strlen(zc->name) + 1, (void *) &class_functions, sizeof(zval *), NULL);
    p = p->pListNext;
  }


  zend_hash_update((*arr)->value.ht, "classes", strlen("classes") + 1, (void *) &classes_array, sizeof(zval *), NULL);

  for (i = 0; i < op_array->last; i++) {
     zval *opcode_arr = NULL;
     char const * name;

     snprintf(buf, sizeof(buf)-1, "%d", i);

     ALLOC_ZVAL(opcode_arr);
     INIT_PZVAL(opcode_arr);
     if(array_init(opcode_arr) == FAILURE) {
        UNLOCK(cache->lock);
        return 0;
     }

     
     /* print the regular opcode, or '&nbsp;' if empty */
     name = apc_get_zend_opname(op_array->opcodes[i].opcode);
     add_next_index_string(opcode_arr, (char *)name, 1);

     if (op_array->opcodes[i].opcode != ZEND_NOP &&
       op_array->opcodes[i].opcode != ZEND_DECLARE_FUNCTION_OR_CLASS)
     {
       /* this opcode does not have an extended value */
       add_next_index_string(opcode_arr, "", 1);
     }
     else {
       name = apc_get_zend_extopname(op_array->opcodes[i].extended_value);
       add_next_index_string(opcode_arr, (char *)name, 1);
     }
     /* print the line number of the opcode */
     add_next_index_long(opcode_arr, op_array->opcodes[i].lineno);
     zend_hash_update(opcode_array->value.ht, buf, strlen(buf) + 1, (void *) &opcode_arr, sizeof(zval *), NULL);
  }
  zend_hash_update((*arr)->value.ht, "opcodes", strlen("opcodes") + 1, (void *) &opcode_array, sizeof(zval *), NULL);



  zend_hash_clean(&class_table);
  zend_hash_clean(&function_table);
  destroy_op_array(op_array);
  free(op_array);

  UNLOCK(cache->lock);
  return 0;
}

