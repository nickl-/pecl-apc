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


#ifndef APC_SERIALIZE_H
#define APC_SERIALIZE_H

#include "apc_nametable.h"
#include "apc_list.h"
#include "zend.h"
#include "zend_compile.h"
#include "zend_llist.h"
#include "zend_hash.h"
#include <stdio.h>

/* request initialize/shutdown routines */
extern void apc_serializer_request_init();
extern void apc_serializer_request_shutdown();

/* By convention all apc_serialize_* functions serialize objects of the
 * specified type to the serialization buffer (dst). The apc_deserialize_*
 * functions deserialize objects of the specified type from the
 * deserialization buffer (src). The apc_create_* functions allocate
 * objects of the specified type, then call the appropriate deserialization
 * function. The apc_copy_* functions perform a deep copy of the object
 * in question into shared memory, returning the address of the 
 * duplicated object. */

extern void apc_init_serializer();
extern void apc_init_deserializer(char* input, int size);
extern void apc_serialize_debug(FILE* out);
extern void apc_get_serialized_data(char** bufptr, int* length);

/* ... */
extern void apc_serialize_debug(FILE* out);
extern void apc_save(const char* filename);
extern int apc_load(const char* filename);

/* general */
extern void apc_serialize_string(char* string);
extern void apc_create_string(char** string);
extern void apc_serialize_arg_types(zend_uchar* arg_types);
extern void apc_create_arg_types(zend_uchar** arg_types);

/* pre-compiler functions */
extern void apc_serialize_magic(void);
extern int apc_deserialize_magic(void);

/* zend_llist.h */
extern zend_llist* apc_copy_zend_llist(zend_llist* nlist, zend_llist* list, apc_malloc_t ctor);
extern void apc_serialize_zend_llist(zend_llist* list);
extern void apc_deserialize_zend_llist(zend_llist* list);
extern void apc_create_zend_llist(zend_llist** list);

/* zend_hash.h */
extern HashTable* apc_copy_hashtable(HashTable* nt, HashTable* ht, void* funcptr, int datasize, apc_malloc_t ctor);
extern void apc_serialize_hashtable(HashTable* ht, void* funcptr);
extern void apc_deserialize_hashtable(HashTable* ht, void* funcptr, int datasize);
extern void apc_create_hashtable(HashTable** ht, void* funcptr, int datasize);

/* zend.h */
extern zvalue_value* apc_copy_zvalue_value(zvalue_value* nv, zvalue_value* zv, int type, apc_malloc_t ctor);
extern void apc_serialize_zvalue_value(zvalue_value* zv, int type);
extern void apc_deserialize_zvalue_value(zvalue_value* zv, int type);
extern zval** apc_copy_zval_ptr(zval** nzvp, zval** zvp, apc_malloc_t ctor);
extern zval* apc_copy_zval(zval* nv, zval* zv, apc_malloc_t ctor);
extern void apc_serialize_zval_ptr(zval** zv);
extern void apc_serialize_zval(zval* zv);
extern void apc_deserialize_zval(zval* zv);
extern void apc_create_zval(zval** zv);
extern zend_function_entry* apc_copy_zend_function_entry(zend_function_entry* nfe, zend_function_entry* zfe, apc_malloc_t ctor);
extern void apc_serialize_zend_function_entry(zend_function_entry* zfe);
extern void apc_deserialize_zend_function_entry(zend_function_entry* zfe);
extern zend_property_reference* apc_copy_zend_property_reference(zend_property_reference* npr, zend_property_reference* zpr, apc_malloc_t ctor);
extern void apc_serialize_zend_property_reference(zend_property_reference* zpr);
extern void apc_deserialize_zend_property_reference(zend_property_reference* zpr);
extern zend_overloaded_element* apc_copy_zend_overloaded_element(zend_overloaded_element* noe, zend_overloaded_element* zoe, apc_malloc_t ctor);
extern void apc_serialize_zend_overloaded_element(zend_overloaded_element* zoe);
extern void apc_deserialize_zend_overloaded_element(zend_overloaded_element* zoe);
extern zend_class_entry* apc_copy_zend_class_entry(zend_class_entry* nce, zend_class_entry* zce, apc_malloc_t ctor);
extern void apc_serialize_zend_class_entry(zend_class_entry* zce);
extern void apc_deserialize_zend_class_entry(zend_class_entry* zce);
extern void apc_create_zend_class_entry(zend_class_entry** zce);
extern zend_utility_functions* apc_copy_zend_utility_functions( zend_utility_functions* nuf, zend_utility_functions* zuf, apc_malloc_t ctor);
extern void apc_serialize_zend_utility_functions(zend_utility_functions* zuf);
extern void apc_deserialize_zend_utility_functions(zend_utility_functions* zuf);
extern void apc_serialize_zend_utility_values(zend_utility_values* zuv);
extern void apc_deserialize_zend_utility_values(zend_utility_values* zuv);

/* zend_compile.h */
extern znode* apc_copy_znode(znode *nn, znode *zn, apc_malloc_t ctor);
extern void apc_serialize_znode(znode* zn);
extern void apc_deserialize_znode(znode* zn);
extern zend_op* apc_copy_zend_op(zend_op *no, zend_op* zo, apc_malloc_t ctor);
extern void apc_serialize_zend_op(zend_op* zo);
extern void apc_deserialize_zend_op(zend_op* zo);
zend_op_array* apc_copy_op_array(zend_op_array* noa, zend_op_array* zoa, apc_malloc_t ctor);
extern void apc_serialize_zend_op_array(zend_op_array* zoa);
extern void apc_deserialize_zend_op_array(zend_op_array* zoa, int master);
extern zend_internal_function* apc_copy_zend_internal_function(zend_internal_function* nif, zend_internal_function* zif, apc_malloc_t ctor);
extern void apc_serialize_zend_internal_function(zend_internal_function* zif);
extern void apc_deserialize_zend_internal_function(zend_internal_function* zif);
extern zend_overloaded_function* apc_copy_zend_overloaded_function(zend_overloaded_function* nof, zend_overloaded_function* zof, apc_malloc_t ctor);
extern void apc_serialize_zend_overloaded_function(zend_overloaded_function* zof);
extern void apc_deserialize_zend_overloaded_function(zend_overloaded_function* zof);
extern zend_function *apc_copy_zend_function(zend_function* nf, zend_function* zf, apc_malloc_t ctor);
extern void apc_serialize_zend_function(zend_function* zf);
extern void apc_deserialize_zend_function(zend_function* zf);
extern void apc_create_zend_function(zend_function** zf);

/* special purpose */
extern void apc_serialize_zend_function_table(HashTable* gft,
	apc_nametable_t* acc, apc_nametable_t* priv);
extern void apc_deserialize_zend_function_table(HashTable* gft,
	apc_nametable_t* acc, apc_nametable_t* priv);
extern void apc_serialize_zend_class_table(HashTable* gct,
	apc_nametable_t* acc, apc_nametable_t* priv);
extern int apc_deserialize_zend_class_table(HashTable* gct,
	apc_nametable_t* acc, apc_nametable_t* priv);

#endif
