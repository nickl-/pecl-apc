/* ==================================================================
 * APC Cache
 * Copyright (c) 2000 Community Connect, Inc.
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
#ifndef INCLUDED_APC_NAMETABLE
#define INCLUDED_APC_NAMETABLE

#include "apc_lib.h"

#define T apc_nametable_t*
typedef struct apc_nametable_t apc_nametable_t;

/*
 * apc_nametable_create: creates a new nametable with the specified
 * number of buckets
 */
T apc_nametable_create(int nbuckets);

/*
 * apc_nametable_destroy: frees all memory associated with a name
 * table, includings its keys. Does not destroy associated values
 */
extern void apc_nametable_destroy(T table);

/*
 * apc_nametable_insert: adds a new key-value mapping to a name table.
 * Returns 1 if the key was successfully added, or 0 if the key is a
 * duplicate
 */
extern int apc_nametable_insert(T table, const char* key, void* value);

/*
 * apc_nametable_search: returns 1 if the specified key exists in
 * the table, else 0
 */
extern int apc_nametable_search(T table, const char* key);

/*
 * apc_nametable_retrieve: returns the value associated with the
 * specified key, or null if the key does not exist
 */
extern void* apc_nametable_retrieve(T table, const char* key);

/*
 * apc_nametable_remove: removes the specified key from the table.
 * Returns the key's associated value if the key existed and was
 * removed, or null if the key does not exist
 */
extern void* apc_nametable_remove(T table, const char* key);

/*
 * apc_nametable_clear: removes all keys from the table and frees
 * their associated memory. Optionally provide a destructor to be
 * called for every value in the table (dtor may be null)
 */
extern void apc_nametable_clear(T table, void (*dtor)(void*));

/*
 * apc_nametable_union: inserts all elements in table b into table
 * a, if and only if they do not already exist in table a
 */
extern void apc_nametable_union(T a, T b);

/*
 * apc_nametable_difference: removes all elements in table b from
 * table a
 */
extern void apc_nametable_difference(T a, T b);

/*
 * apc_nametable_size: returns number of elements in table
 */
extern int apc_nametable_size(T table);

/*
 * apc_nametable_dump: debugging display function
 */
extern void apc_nametable_dump(T table, apc_outputfn_t outputfn);

#undef T
#endif
