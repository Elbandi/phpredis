/* -*- Mode: C; tab-width: 4 -*- */
/*
  +----------------------------------------------------------------------+
  | PHP Version 5                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2009 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Original author: Alfonso Jimenez <yo@alfonsojimenez.com>             |
  | Maintainer: Nicolas Favre-Felix <n.favre-felix@owlient.eu>           |
  | Maintainer: Nasreddine Bouafif <n.bouafif@owlient.eu>                |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common.h"
#include "ext/standard/info.h"
#include "php_redis.h"
#include "redis_session.h"
#include <zend_exceptions.h>

#include "library.h"

#include "php.h"
#include "php_ini.h"
#include "php_variables.h"
#include "SAPI.h"
#include "ext/standard/url.h"

ps_module ps_mod_redis = {
	PS_MOD(redis)
};

typedef struct redis_pool_member_ {

	RedisSock *redis_sock;
	int weight;
	struct redis_pool_member_ *next;

} redis_pool_member;

typedef struct {

	int totalWeight;
	int count;

	redis_pool_member *head;

} redis_pool;

PHPAPI redis_pool*
redis_pool_new(TSRMLS_D) {
	return ecalloc(1, sizeof(redis_pool));
}

PHPAPI void
redis_pool_add(redis_pool *pool, RedisSock *redis_sock, int weight TSRMLS_DC) {

	redis_pool_member *rpm = ecalloc(1, sizeof(redis_pool_member));
	rpm->redis_sock = redis_sock;
	rpm->weight = weight;

	rpm->next = pool->head;
	pool->head = rpm;

	pool->totalWeight += weight;
}

PHPAPI void
redis_pool_free(redis_pool *pool TSRMLS_DC) {

	redis_pool_member *rpm, *next;
    rpm = pool->head;
	while(rpm) {
		next = rpm->next;
		redis_sock_disconnect(rpm->redis_sock TSRMLS_CC);
		efree(rpm->redis_sock);
		efree(rpm);
		rpm = next;
	}
	efree(pool);
}

PHPAPI RedisSock *
redis_pool_get_sock(redis_pool *pool, const char *key TSRMLS_DC) {

	unsigned int pos, i;
	memcpy(&pos, key, sizeof(pos));
	pos %= pool->totalWeight;

	redis_pool_member *rpm = pool->head;

	for(i = 0; i < pool->totalWeight;) {
		if(pos >= i && pos < i + rpm->weight) {
			redis_sock_server_open(rpm->redis_sock, 0 TSRMLS_CC);
			return rpm->redis_sock;
		}
		i += rpm->weight;
        rpm = rpm->next;
	}

	return NULL;
}

/* {{{ PS_OPEN_FUNC
 */
PS_OPEN_FUNC(redis)
{

	php_url *url;
	zval *params, **param;
	int i, j, path_len;

	redis_pool *pool = redis_pool_new(TSRMLS_C);

	for (i=0,j=0,path_len=strlen(save_path); i<path_len; i=j+1) {
		/* find beginning of url */
		while (i<path_len && (isspace(save_path[i]) || save_path[i] == ','))
			i++;

		/* find end of url */
		j = i;
		while (j<path_len && !isspace(save_path[j]) && save_path[j] != ',')
			 j++;

		if (i < j) {
			int weight = 1;
			double timeout = 86400.0;

			/* unix: isn't supported yet. */
			if (!strncmp(save_path+i, "unix:", sizeof("unix:")-1)) {
				char *path = estrndup(save_path+i, j-i);
				php_error_docref(NULL TSRMLS_CC, E_WARNING,
					"Failed to parse session.save_path (error at offset %d, url was '%s')", i, path);
				efree(path);
				redis_pool_free(pool TSRMLS_CC);
				PS_SET_MOD_DATA(NULL);
				return FAILURE;
			}
			else {
				url = php_url_parse_ex(save_path+i, j-i);
			}

			if (!url) {
				char *path = estrndup(save_path+i, j-i);
				php_error_docref(NULL TSRMLS_CC, E_WARNING,
					"Failed to parse session.save_path (error at offset %d, url was '%s')", i, path);
				efree(path);

				redis_pool_free(pool TSRMLS_CC);
				PS_SET_MOD_DATA(NULL);
				return FAILURE;
			}

			/* parse parameters */
			if (url->query != NULL) {
				MAKE_STD_ZVAL(params);
				array_init(params);

				sapi_module.treat_data(PARSE_STRING, estrdup(url->query), params TSRMLS_CC);

				if (zend_hash_find(Z_ARRVAL_P(params), "weight", sizeof("weight"), (void **) &param) != FAILURE) {
					convert_to_long_ex(param);
					weight = Z_LVAL_PP(param);
				}

				if (zend_hash_find(Z_ARRVAL_P(params), "timeout", sizeof("timeout"), (void **) &param) != FAILURE) {
					timeout = atof(Z_STRVAL_PP(param));
				}

				/* // not supported yet
				if (zend_hash_find(Z_ARRVAL_P(params), "retry_interval", sizeof("retry_interval"), (void **) &param) != FAILURE) {
					convert_to_long_ex(param);
					retry_interval = Z_LVAL_PP(param);
				}
				*/

				zval_ptr_dtor(&params);
			}

			if (url->host == NULL || weight <= 0 || timeout <= 0) {
				php_url_free(url);
				redis_pool_free(pool TSRMLS_CC);
				PS_SET_MOD_DATA(NULL);
				return FAILURE;
			}

			RedisSock *redis_sock = redis_sock_create(url->host, strlen(url->host), url->port, timeout, 0);
			redis_pool_add(pool, redis_sock, weight TSRMLS_CC);

			php_url_free(url);
		}
	}

	if (pool->head) {
		PS_SET_MOD_DATA(pool);
		return SUCCESS;
	}

	return FAILURE;
}
/* }}} */

/* {{{ PS_CLOSE_FUNC
 */
PS_CLOSE_FUNC(redis)
{
	redis_pool *pool = PS_GET_MOD_DATA();

	if(pool){
		redis_pool_free(pool TSRMLS_CC);
		PS_SET_MOD_DATA(NULL);
	}
	return SUCCESS;
}
/* }}} */

static char *
redis_session_key(const char *key, int key_len, int *session_len) {

	char *session;
	const char prefix[] = "PHPREDIS_SESSION:";

	/* build session key */
	*session_len = key_len + sizeof(prefix) - 1;
	session = ecalloc(*session_len + 1, 1);
	memcpy(session, prefix, sizeof(prefix)-1);
	memcpy(session + sizeof(prefix)-1, key, key_len);

	return session;
}


/* {{{ PS_READ_FUNC
 */
PS_READ_FUNC(redis)
{
	char *session, *cmd;
	int session_len, cmd_len;

	redis_pool *pool = PS_GET_MOD_DATA();
	RedisSock *redis_sock = redis_pool_get_sock(pool, key TSRMLS_CC);
	if(!redis_sock){
		return FAILURE;
	}

	/* send GET command */
	session = redis_session_key(key, strlen(key), &session_len);
	cmd_len = redis_cmd_format_static(&cmd, "GET", "s", session, session_len);
	efree(session);
	if(redis_sock_write(redis_sock, cmd, cmd_len TSRMLS_CC) < 0) {
		efree(cmd);
		return FAILURE;
	}
	efree(cmd);

	/* read response */
	if ((*val = redis_sock_read(redis_sock, vallen TSRMLS_CC)) == NULL) {
		return FAILURE;
	}

	return SUCCESS;
}
/* }}} */

/* {{{ PS_WRITE_FUNC
 */
PS_WRITE_FUNC(redis)
{
	char *cmd, *response, *session;
	int cmd_len, response_len, session_len;

	redis_pool *pool = PS_GET_MOD_DATA();
	RedisSock *redis_sock = redis_pool_get_sock(pool, key TSRMLS_CC);
	if(!redis_sock){
		return FAILURE;
	}

	/* send SET command */
	session = redis_session_key(key, strlen(key), &session_len);
	cmd_len = redis_cmd_format_static(&cmd, "SETEX", "sds", session, session_len, INI_INT("session.gc_maxlifetime"), val, vallen);
	efree(session);
	if(redis_sock_write(redis_sock, cmd, cmd_len TSRMLS_CC) < 0) {
		efree(cmd);
		return FAILURE;
	}
	efree(cmd);

	/* read response */
	if ((response = redis_sock_read(redis_sock, &response_len TSRMLS_CC)) == NULL) {
		return FAILURE;
	}

	if(response_len == 3 && strncmp(response, "+OK", 3) == 0) {
		efree(response);
		return SUCCESS;
	} else {
		efree(response);
		return FAILURE;
	}
}
/* }}} */

/* {{{ PS_DESTROY_FUNC
 */
PS_DESTROY_FUNC(redis)
{
	char *cmd, *response, *session;
	int cmd_len, response_len, session_len;

	redis_pool *pool = PS_GET_MOD_DATA();
	RedisSock *redis_sock = redis_pool_get_sock(pool, key TSRMLS_CC);
	if(!redis_sock){
		return FAILURE;
	}

    /* send DEL command */
	session = redis_session_key(key, strlen(key), &session_len);
	cmd_len = redis_cmd_format_static(&cmd, "DEL", "s", session, session_len);
	efree(session);
	if(redis_sock_write(redis_sock, cmd, cmd_len TSRMLS_CC) < 0) {
		efree(cmd);
		return FAILURE;
	}
	efree(cmd);

	/* read response */
	if ((response = redis_sock_read(redis_sock, &response_len TSRMLS_CC)) == NULL) {
		return FAILURE;
	}

	if(response_len == 2 && response[0] == ':' && (response[1] == '0' || response[1] == '1')) {
		efree(response);
		return SUCCESS;
	} else {
		efree(response);
		return FAILURE;
	}
}
/* }}} */

/* {{{ PS_GC_FUNC
 */
PS_GC_FUNC(redis)
{
	return SUCCESS;
}
/* }}} */

/* vim: set tabstop=4 expandtab: */

