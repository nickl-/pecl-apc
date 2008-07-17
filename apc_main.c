/*
  +----------------------------------------------------------------------+
  | APC                                                                  |
  +----------------------------------------------------------------------+
  | Copyright (c) 2006 The PHP Group                                     |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Authors: Daniel Cowgill <dcowgill@communityconnect.com>              |
  |          Rasmus Lerdorf <rasmus@php.net>                             |
  |          Arun C. Murthy <arunc@yahoo-inc.com>                        |
  |          Gopal Vijayaraghavan <gopalv@yahoo-inc.com>                 |
  +----------------------------------------------------------------------+

   This software was contributed to PHP by Community Connect Inc. in 2002
   and revised in 2005 by Yahoo! Inc. to add support for PHP 5.1.
   Future revisions and derivatives of this source code must acknowledge
   Community Connect Inc. as the original contributor of this module by
   leaving this note intact in the source code.

   All other licensing and usage conditions are those of the PHP Group.

 */

/* $Id$ */

#include "apc_php.h"
#include "apc_main.h"
#include "apc.h"
#include "apc_lock.h"
#include "apc_cache.h"
#include "apc_compile.h"
#include "apc_globals.h"
#include "apc_sma.h"
#include "apc_stack.h"
#include "apc_zend.h"
#include "apc_pool.h"
#include "SAPI.h"
#if PHP_API_VERSION <= 20020918
#if HAVE_APACHE
#ifdef APC_PHP4_STAT
#undef XtOffsetOf
#include "httpd.h"
#endif
#endif
#endif

/* {{{ module variables */

/* pointer to the original Zend engine compile_file function */
typedef zend_op_array* (zend_compile_t)(zend_file_handle*, int TSRMLS_DC);
static zend_compile_t *old_compile_file;

/* }}} */

/* {{{ get/set old_compile_file (to interact with other extensions that need the compile hook) */
static zend_compile_t* set_compile_hook(zend_compile_t *ptr)
{
    zend_compile_t *retval = old_compile_file;

    if (ptr != NULL) old_compile_file = ptr;
    return retval;
}
/* }}} */

/* {{{ install_function */
static int install_function(apc_function_t fn, apc_context_t* ctxt TSRMLS_DC)
{
    zend_function *func;
    int status;

    func = apc_copy_function_for_execution(fn.function, ctxt);

    status =  zend_hash_add(EG(function_table),
                      fn.name,
                      fn.name_len+1,
                      func, 
                      sizeof(fn.function[0]),
                      NULL);

    efree(func);

    if (status == FAILURE) {
        /* apc_eprint("Cannot redeclare %s()", fn.name); */
    }

    return status;
}
/* }}} */

/* {{{ install_class */
static int install_class(apc_class_t cl, apc_context_t* ctxt TSRMLS_DC)
{
    zend_class_entry* class_entry = cl.class_entry;
    zend_class_entry* parent = NULL;
    int status;
    zend_class_entry** allocated_ce = NULL;


    /* Special case for mangled names. Mangled names are unique to a file.
     * There is no way two classes with the same mangled name will occur,
     * unless a file is included twice. And if in case, a file is included
     * twice, all mangled name conflicts can be ignored and the class redeclaration
     * error may be deferred till runtime of the corresponding DECLARE_CLASS
     * calls.
     */

    if(cl.name_len != 0 && cl.name[0] == '\0') {
        if(zend_hash_exists(CG(class_table), cl.name, cl.name_len+1)) {
            return SUCCESS;
        }
    }

    /*
     * XXX: We need to free this somewhere...
     */
    allocated_ce = apc_php_malloc(sizeof(zend_class_entry*));

    if(!allocated_ce) {
        return FAILURE;
    }

    *allocated_ce =
    class_entry =
        apc_copy_class_entry_for_execution(cl.class_entry, ctxt);


    /* restore parent class pointer for compile-time inheritance */
    if (cl.parent_name != NULL) {
        zend_class_entry** parent_ptr = NULL;
        /*
         * zend_lookup_class has to be due to presence of __autoload,
         * just looking up the EG(class_table) is not enough in php5!
         * Even more dangerously, thanks to __autoload and people using
         * class names as filepaths for inclusion, this has to be case
         * sensitive. zend_lookup_class automatically does a case_fold
         * internally, but passes the case preserved version to __autoload.
         * Aside: Do NOT pass *strlen(cl.parent_name)+1* because
         * zend_lookup_class does it internally anyway!
         */
        status = zend_lookup_class(cl.parent_name,
                                    strlen(cl.parent_name),
                                    &parent_ptr TSRMLS_CC);
        if (status == FAILURE) {
            if(APCG(report_autofilter)) {
                apc_wprint("Dynamic inheritance detected for class %s", cl.name);
            }
            class_entry->parent = NULL;
            return status;
        }
        else {
            parent = *parent_ptr;
            class_entry->parent = parent;
            zend_do_inheritance(class_entry, parent TSRMLS_CC);
        }


    }

    status = zend_hash_add(EG(class_table),
                           cl.name,
                           cl.name_len+1,
                           allocated_ce,
                           sizeof(zend_class_entry*),
                           NULL);

    if (status == FAILURE) {
        apc_eprint("Cannot redeclare class %s", cl.name);
    }
    return status;
}
/* }}} */

/* {{{ uninstall_class */
static int uninstall_class(apc_class_t cl TSRMLS_DC)
{
    int status;

    status = zend_hash_del(EG(class_table),
                           cl.name,
                           cl.name_len+1);
    if (status == FAILURE) {
        apc_eprint("Cannot delete class %s", cl.name);
    }
    return status;
}
/* }}} */

/* {{{ compare_file_handles */
static int compare_file_handles(void* a, void* b)
{
    zend_file_handle* fh1 = (zend_file_handle*)a;
    zend_file_handle* fh2 = (zend_file_handle*)b;
    return (fh1->type == fh2->type && 
            fh1->filename == fh2->filename &&
            fh1->opened_path == fh2->opened_path);
}
/* }}} */

/* {{{ cached_compile */
static zend_op_array* cached_compile(zend_file_handle* h,
                                        int type,
                                        apc_context_t* ctxt TSRMLS_DC)
{
    apc_cache_entry_t* cache_entry;
    int i, ii;

    cache_entry = (apc_cache_entry_t*) apc_stack_top(APCG(cache_stack));
    assert(cache_entry != NULL);

    if (cache_entry->data.file.classes) {
        for (i = 0; cache_entry->data.file.classes[i].class_entry != NULL; i++) {
            if(install_class(cache_entry->data.file.classes[i], ctxt TSRMLS_CC) == FAILURE) {
                goto default_compile;
            }
        }
    }

    if (cache_entry->data.file.functions) {
        for (i = 0; cache_entry->data.file.functions[i].function != NULL; i++) {
            install_function(cache_entry->data.file.functions[i], ctxt TSRMLS_CC);
        }
    }


    return apc_copy_op_array_for_execution(NULL, cache_entry->data.file.op_array, ctxt TSRMLS_CC);

default_compile:

    if(APCG(report_autofilter)) {
        apc_wprint("Autofiltering %s", h->opened_path);
    }

    if(cache_entry->data.file.classes) {
        for(ii = 0; ii < i ; ii++) {
            uninstall_class(cache_entry->data.file.classes[ii] TSRMLS_CC);
        }
    }

    apc_stack_pop(APCG(cache_stack)); /* pop out cache_entry */

    apc_cache_release(apc_cache, cache_entry);

    /* cannot free up cache data yet, it maybe in use */

    zend_llist_del_element(&CG(open_files), h, compare_file_handles); /* We leak fds without this hack */

    h->type = ZEND_HANDLE_FILENAME;

    return NULL;
}
/* }}} */

/* {{{ my_compile_file
   Overrides zend_compile_file */
static zend_op_array* my_compile_file(zend_file_handle* h,
                                               int type TSRMLS_DC)
{
    apc_cache_key_t key;
    apc_cache_entry_t* cache_entry;
    zend_op_array* op_array;
    int num_functions, num_classes, ret;
    zend_op_array* alloc_op_array;
    apc_function_t* alloc_functions;
    apc_class_t* alloc_classes;
    time_t t;
    char *path;
    apc_context_t ctxt = {0,};

    if (!APCG(enabled) || apc_cache_busy(apc_cache)) {
        return old_compile_file(h, type TSRMLS_CC);
    }

#if PHP_API_VERSION < 20041225
#if HAVE_APACHE && defined(APC_PHP4_STAT)
    t = ((request_rec *)SG(server_context))->request_time;
#else
    t = time(0);
#endif
#else
    t = sapi_get_request_time(TSRMLS_C);
#endif

#ifdef __DEBUG_APC__
    fprintf(stderr,"1. h->opened_path=[%s]  h->filename=[%s]\n", h->opened_path?h->opened_path:"null",h->filename);
#endif

    /* try to create a cache key; if we fail, give up on caching */
    if (!apc_cache_make_file_key(&key, h->filename, PG(include_path), t TSRMLS_CC)) {
        return old_compile_file(h, type TSRMLS_CC);
    }


    if(!APCG(force_file_update)) {
        /* search for the file in the cache */
        cache_entry = apc_cache_find(apc_cache, key, t);
    } else {
        cache_entry = NULL;
    }

    if (cache_entry != NULL) {
        int dummy = 1;

        ctxt.pool = apc_pool_create(APC_UNPOOL, apc_php_malloc, apc_php_free);
        ctxt.copy = APC_COPY_OUT_OPCODE;

        if (h->opened_path == NULL) {
            h->opened_path = estrdup(cache_entry->data.file.filename);
        }
        zend_hash_add(&EG(included_files), h->opened_path, strlen(h->opened_path)+1, (void *)&dummy, sizeof(int), NULL);

        zend_llist_add_element(&CG(open_files), h); /* We leak fds without this hack */

        apc_stack_push(APCG(cache_stack), cache_entry);
        op_array = cached_compile(h, type, &ctxt TSRMLS_CC);
        if(op_array) {
#ifdef APC_FILEHITS
            /* If the file comes from the cache, add it to the global request file list */
            add_next_index_string(APCG(filehits), h->filename, 1);
#endif
            return op_array;
        }
        if(APCG(report_autofilter)) {
            apc_wprint("Recompiling %s", h->opened_path);
        }
        /* TODO: check what happens with EG(included_files) */
    }

    /* remember how many functions and classes existed before compilation */
    num_functions = zend_hash_num_elements(CG(function_table));
    num_classes   = zend_hash_num_elements(CG(class_table));

    /* compile the file using the default compile function */
    op_array = old_compile_file(h, type TSRMLS_CC);
    if (op_array == NULL) {
        return NULL;
    }

    /* check our regular expression filters */
    if (APCG(filters) && apc_compiled_filters) {
        int ret = apc_regex_match_array(apc_compiled_filters, h->opened_path);
        if(ret == APC_NEGATIVE_MATCH || (ret != APC_POSITIVE_MATCH && !APCG(cache_by_default))) {
            /* never cache, never find */
            return op_array;
        }
    } else if(!APCG(cache_by_default)) {
        return op_array;
    }

    /* Make sure the mtime reflects the files last known mtime in the case of fpstat==0 */
    if(key.type == APC_CACHE_KEY_FPFILE) {
        apc_fileinfo_t fileinfo;
        struct stat *tmp_buf = NULL;
        if(!strcmp(SG(request_info).path_translated, h->filename)) {
            tmp_buf = sapi_get_stat(TSRMLS_C);  /* Apache has already done this stat() for us */
        }
        if(tmp_buf) { 
            fileinfo.st_buf.sb = *tmp_buf;
        } else {
            if (apc_search_paths(h->filename, PG(include_path), &fileinfo) != 0) {
#ifdef __DEBUG_APC__
                fprintf(stderr,"Stat failed %s - bailing (%s) (%d)\n",filename,SG(request_info).path_translated);
#endif
                return op_array;
            }
        }
        key.mtime = fileinfo.st_buf.sb.st_mtime;
    }

    HANDLE_BLOCK_INTERRUPTIONS();

#if NONBLOCKING_LOCK_AVAILABLE
    if(APCG(write_lock)) {
        if(!apc_cache_write_lock(apc_cache)) {
            HANDLE_UNBLOCK_INTERRUPTIONS();
            return op_array;
        }
    }
#endif

    APCG(current_cache) = apc_cache;
    ctxt.pool = apc_pool_create(APC_MEDIUM_POOL, apc_sma_malloc, apc_sma_free);
    ctxt.copy = APC_COPY_IN_OPCODE;

    if(!(alloc_op_array = apc_copy_op_array(NULL, op_array, &ctxt TSRMLS_CC))) {
        goto freepool;
    }

    if(!(alloc_functions = apc_copy_new_functions(num_functions, &ctxt TSRMLS_CC))) {
        goto freepool;
    }
    if(!(alloc_classes = apc_copy_new_classes(op_array, num_classes, &ctxt TSRMLS_CC))) {
        goto freepool;
    }

    path = h->opened_path;
    if(!path) path=h->filename;

#ifdef __DEBUG_APC__
    fprintf(stderr,"2. h->opened_path=[%s]  h->filename=[%s]\n", h->opened_path?h->opened_path:"null",h->filename);
#endif

    if(!(cache_entry = apc_cache_make_file_entry(path, alloc_op_array, alloc_functions, alloc_classes, &ctxt))) {
        goto freepool;
    }

    if ((ret = apc_cache_insert(apc_cache, key, cache_entry, &ctxt, t)) != 1) {
freepool:
        apc_pool_destroy(ctxt.pool);
        ctxt.pool = NULL;
    }

    APCG(current_cache) = NULL;

#if NONBLOCKING_LOCK_AVAILABLE
    if(APCG(write_lock)) {
        apc_cache_write_unlock(apc_cache);
    }
#endif
    HANDLE_UNBLOCK_INTERRUPTIONS();

    return op_array;
}
/* }}} */

/* {{{ module init and shutdown */

int apc_module_init(int module_number TSRMLS_DC)
{
    /* apc initialization */
#if APC_MMAP
    apc_sma_init(APCG(shm_segments), APCG(shm_size)*1024*1024, APCG(mmap_file_mask));
#else
    apc_sma_init(APCG(shm_segments), APCG(shm_size)*1024*1024, NULL);
#endif
    apc_cache = apc_cache_create(APCG(num_files_hint), APCG(gc_ttl), APCG(ttl));
    apc_user_cache = apc_cache_create(APCG(user_entries_hint), APCG(gc_ttl), APCG(user_ttl));

    apc_compiled_filters = apc_regex_compile_array(APCG(filters) TSRMLS_CC);

    /* override compilation */
    old_compile_file = zend_compile_file;
    zend_compile_file = my_compile_file;
    REGISTER_LONG_CONSTANT("\000apc_magic", (long)&set_compile_hook, CONST_PERSISTENT | CONST_CS);

    apc_pool_init();

    APCG(initialized) = 1;
    return 0;
}

int apc_module_shutdown(TSRMLS_D)
{
    if (!APCG(initialized))
        return 0;

    /* restore compilation */
    zend_compile_file = old_compile_file;

    /*
     * In case we got interrupted by a SIGTERM or something else during execution
     * we may have cache entries left on the stack that we need to check to make
     * sure that any functions or classes these may have added to the global function
     * and class tables are removed before we blow away the memory that hold them.
     * 
     * This is merely to remove memory leak warnings - as the process is terminated
     * immediately after shutdown. The following while loop can be removed without
     * affecting anything else.
     */
    while (apc_stack_size(APCG(cache_stack)) > 0) {
        int i;
        apc_cache_entry_t* cache_entry = (apc_cache_entry_t*) apc_stack_pop(APCG(cache_stack));
        if (cache_entry->data.file.functions) {
            for (i = 0; cache_entry->data.file.functions[i].function != NULL; i++) {
                zend_hash_del(EG(function_table),
                    cache_entry->data.file.functions[i].name,
                    cache_entry->data.file.functions[i].name_len+1);
            }
        }
        if (cache_entry->data.file.classes) {
            for (i = 0; cache_entry->data.file.classes[i].class_entry != NULL; i++) {
                zend_hash_del(EG(class_table),
                    cache_entry->data.file.classes[i].name,
                    cache_entry->data.file.classes[i].name_len+1);
            }
        }
        apc_cache_release(apc_cache, cache_entry);
    }

    apc_cache_destroy(apc_cache);
    apc_cache_destroy(apc_user_cache);
    apc_sma_cleanup();

    APCG(initialized) = 0;
    return 0;
}

/* }}} */

/* {{{ process init and shutdown */
int apc_process_init(int module_number TSRMLS_DC)
{
    return 0;
}

int apc_process_shutdown(TSRMLS_D)
{
    return 0;
}
/* }}} */

/* {{{ apc_deactivate */
static void apc_deactivate(TSRMLS_D)
{
    /* The execution stack was unwound, which prevented us from decrementing
     * the reference counts on active cache entries in `my_execute`.
     */
    while (apc_stack_size(APCG(cache_stack)) > 0) {
        int i;
        zend_class_entry* zce = NULL;
        void ** centry = (void*)(&zce);
        zend_class_entry** pzce = NULL;

        apc_cache_entry_t* cache_entry =
            (apc_cache_entry_t*) apc_stack_pop(APCG(cache_stack));

        if (cache_entry->data.file.classes) {
            for (i = 0; cache_entry->data.file.classes[i].class_entry != NULL; i++) {
                centry = (void**)&pzce; /* a triple indirection to get zend_class_entry*** */
                if(zend_hash_find(EG(class_table), 
                    cache_entry->data.file.classes[i].name,
                    cache_entry->data.file.classes[i].name_len+1,
                    (void**)centry) == FAILURE)
                {
                    /* double inclusion of conditional classes ends up failing 
                     * this lookup the second time around.
                     */
                    continue;
                }

                zce = *pzce;
                zend_hash_del(EG(class_table),
                    cache_entry->data.file.classes[i].name,
                    cache_entry->data.file.classes[i].name_len+1);

                apc_free_class_entry_after_execution(zce);
            }
        }
        apc_cache_release(apc_cache, cache_entry);
    }
}
/* }}} */

/* {{{ request init and shutdown */

int apc_request_init(TSRMLS_D)
{
    apc_stack_clear(APCG(cache_stack));
    APCG(copied_zvals) = NULL;

#ifdef APC_FILEHITS
    ALLOC_INIT_ZVAL(APCG(filehits));
    array_init(APCG(filehits));
#endif

    return 0;
}

int apc_request_shutdown(TSRMLS_D)
{
    apc_deactivate(TSRMLS_C);

#ifdef APC_FILEHITS
    zval_ptr_dtor(&APCG(filehits));
#endif

    return 0;
}

/* }}} */


/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */
