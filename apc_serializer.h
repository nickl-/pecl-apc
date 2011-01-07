/*
  +----------------------------------------------------------------------+
  | APC                                                                  |
  +----------------------------------------------------------------------+
  | Copyright (c) 2006-2011 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt.                                 |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Authors: Gopal Vijayaraghavan <gopalv@php.net>                       | 
  +----------------------------------------------------------------------+

 */

/* $Id: $ */

#ifndef APC_SERIALIZER_H
#define APC_SERIALIZER_H

/* this is a shipped .h file, do not include any other header in this file */
#define APC_SERIALIZER_NAME(module) module##_apc_serializer
#define APC_UNSERIALIZER_NAME(module) module##_apc_unserializer

#define APC_SERIALIZER_ARGS unsigned char **buf, size_t *buf_len, const zval *value, void *config TSRMLS_DC
#define APC_UNSERIALIZER_ARGS zval **value, unsigned char *buf, size_t buf_len, void *config TSRMLS_DC

typedef int (*apc_serialize_t)(APC_SERIALIZER_ARGS);
typedef int (*apc_unserialize_t)(APC_UNSERIALIZER_ARGS);

typedef int (*apc_register_serializer_t)(const char* name, 
                                        apc_serialize_t serialize, 
                                        apc_unserialize_t unserialize,
                                        void *config TSRMLS_DC);

/*
 * ABI version for constant hooks. Increment this any time you make any changes 
 * to any function in this file.
 */
#define APC_SERIALIZER_ABI "0"
#define APC_SERIALIZER_CONSTANT "\000apc_register_serializer-" APC_SERIALIZER_ABI

static int apc_register_serializer(const char* name, 
                                    apc_serialize_t serialize,
                                    apc_unserialize_t unserialize,
                                    void *config TSRMLS_DC)
{
    zval *apc_magic_constant = NULL;
    
    ALLOC_INIT_ZVAL(apc_magic_constant);

    if (zend_get_constant(APC_SERIALIZER_CONSTANT, sizeof(APC_SERIALIZER_CONSTANT)-1, apc_magic_constant)) { 
        if(apc_magic_constant) {
            apc_register_serializer_t register_func = (apc_register_serializer_t)(Z_LVAL_P(apc_magic_constant));
            if(register_func) {
                zval_dtor(apc_magic_constant);
                return register_func(name, serialize, unserialize, NULL TSRMLS_CC);
           }
       }
    }

    zval_dtor(apc_magic_constant);

    return 0;
}

#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */
