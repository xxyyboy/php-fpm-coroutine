/*
  +----------------------------------------------------------------------+
  | PHP Version 7                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2018 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author:                                                              |
  +----------------------------------------------------------------------+
*/

/* $Id$ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "php_coro_http.h"

#include "SAPI.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sys/queue.h>

#include "event2/http.h"
#include "event2/http_struct.h"
#include "event2/event.h"
#include "event2/buffer.h"
#include "event2/dns.h"
#include "event2/thread.h"
#include <event.h>

#include "ext/standard/fpm_coroutine.h"

#ifndef WIN32  
#include <sys/socket.h>  
#include <sys/types.h>  
#include <netinet/in.h>  
#include <arpa/inet.h>  
#endif  



/* If you declare any globals in php_coro_http.h uncomment this:
ZEND_DECLARE_MODULE_GLOBALS(coro_http)
*/

/* True global resources - no need for thread safety here */
static int le_coro_http;

/************************************************************************
 *   扩展需要用到的member
 *   SG(coroutine_info).base;                                   基础event_loop_base
 *   SG(coroutine_info).context                                 上下文
 *   SG(coroutine_info).test_log(char* str);                    输出LOG
 *   SG(coroutine_info).yield_coroutine_context();              释放当前协程
 *   SG(coroutine_info).checkout_coroutine_context(context);    切换到协程
 *   SG(coroutine_info).resume_coroutine_context();             继续执行上下文
 */

char tmpStr[4096];

int ReadHeaderDoneCallback(struct evhttp_request* remote_rsp, void* arg)
{
    strcpy(tmpStr,"");
}

void ReadChunkCallback(struct evhttp_request* remote_rsp, void* arg)
{
    char buf[1024];
    struct evbuffer* evbuf = evhttp_request_get_input_buffer(remote_rsp);
    int n = 0;
    while ((n = evbuffer_remove(evbuf, buf, 1024)) > 0)
    {
        strcat(tmpStr,buf);
    }
}

void RemoteReadCallback(struct evhttp_request* remote_rsp, void* arg)
{
    /**
     * 当远程调用成功，立刻切换协程，arg是evhttp新建请求时传递过来的协程context
     * 这里注意，一定要第一时间切换到当前请求的context
     * 这样才能保证emalloc、SG宏、EG宏，等全局宏可以正确拿到当前协程的内存
     */
    SG(coroutine_info).checkout_coroutine_context(arg);
    sapi_coroutine_context* context = arg;

    /**
     * return_value 是扩展函数执行时保存下来的返回值指针
     */
    zval* return_value = context->return_value;
    
    /**
     * 从全局变量中获取返回结果的buffer,并创建zend_string类型的结果
     */
    zend_string* result = zend_string_init(tmpStr,strlen(tmpStr)*sizeof(char),0);

    /**
     * 设置函数返回值
     */
    RETVAL_STR(result);

    /**
     * 继续执行当前协程的PHP脚本
     */
    SG(coroutine_info).resume_coroutine_context();
}

void RemoteRequestErrorCallback(enum evhttp_request_error error, void* arg)
{
    SG(coroutine_info).checkout_coroutine_context(arg);
    SG(coroutine_info).resume_coroutine_context();
}

void RemoteConnectionCloseCallback(struct evhttp_connection* connection, void* arg)
{
    SG(coroutine_info).checkout_coroutine_context(arg);
    SG(coroutine_info).resume_coroutine_context();
}

/* {{{ PHP_INI
 */
/* Remove comments and fill if you need to have entries in php.ini
PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("coro_http.global_value",      "42", PHP_INI_ALL, OnUpdateLong, global_value, zend_coro_http_globals, coro_http_globals)
    STD_PHP_INI_ENTRY("coro_http.global_string", "foobar", PHP_INI_ALL, OnUpdateString, global_string, zend_coro_http_globals, coro_http_globals)
PHP_INI_END()
*/
/* }}} */

/* Remove the following function when you have successfully modified config.m4
   so that your module can be compiled into PHP, it exists only for testing
   purposes. */

/* Every user-visible function in PHP should document itself in the source */
/* {{{ proto string confirm_coro_http_compiled(string arg)
   Return a string to confirm that the module is compiled in */
PHP_FUNCTION(coro_http_get)
{
    /**
     * 保存coro_http_get返回值指针到协程中，当evhttp返回时会用到
     */
    SG(coroutine_info).context->return_value = return_value; 

    zval* param;
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &param) == FAILURE){
        RETURN_FALSE;
    }
    char* url = ZSTR_VAL(zval_get_string(param));

    struct evhttp_uri* uri = evhttp_uri_parse(url);

    if (!uri)
    {
        RETURN_FALSE;
    }

    /**
     * 这里与正常使用evhttp不同的是，要使用协程里的event_base，并且后面不需要调用loop方法（使用系统的loop）
     */
    struct event_base *base = SG(coroutine_info).base;
    if (!base)
    {
        RETURN_FALSE;
    }

    struct evdns_base* dnsbase = evdns_base_new(base, 1);
    if (!dnsbase)
    {
        RETURN_FALSE;
    }
    assert(dnsbase);

    struct evhttp_request* request = evhttp_request_new(RemoteReadCallback, SG(coroutine_info).context);
    evhttp_request_set_header_cb(request, ReadHeaderDoneCallback);
    evhttp_request_set_chunked_cb(request, ReadChunkCallback);
    evhttp_request_set_error_cb(request, RemoteRequestErrorCallback);

    const char* host = evhttp_uri_get_host(uri);
    if (!host)
    {
        RETURN_FALSE;
    }

    int port = evhttp_uri_get_port(uri);
    if (port < 0) port = 80;

    const char* request_url = url;
    const char* path = evhttp_uri_get_path(uri);
    if (path == NULL || strlen(path) == 0)
    {
        request_url = "/";
    }

    struct evhttp_connection* connection =  evhttp_connection_base_new(base, dnsbase, host, port);
    if (!connection)
    {
        RETURN_FALSE;
    }

    evhttp_connection_set_closecb(connection, RemoteConnectionCloseCallback, base);
    evhttp_add_header(evhttp_request_get_output_headers(request), "Host", host);
    evhttp_make_request(connection, request, EVHTTP_REQ_GET, request_url);

    /**
     * 请求发出后，立刻释放当前协程，将控制权交给协程控制器(php-fpm)
     */
    SG(coroutine_info).yield_coroutine_context();
}
/* }}} */
/* The previous line is meant for vim and emacs, so it can correctly fold and
   unfold functions in source code. See the corresponding marks just before
   function definition, where the functions purpose is also documented. Please
   follow this convention for the convenience of others editing your code.
*/


/* {{{ php_coro_http_init_globals
 */
/* Uncomment this function if you have INI entries
static void php_coro_http_init_globals(zend_coro_http_globals *coro_http_globals)
{
	coro_http_globals->global_value = 0;
	coro_http_globals->global_string = NULL;
}
*/
/* }}} */

/* {{{ PHP_MINIT_FUNCTION
 */
PHP_MINIT_FUNCTION(coro_http)
{
	/* If you have INI entries, uncomment these lines
	REGISTER_INI_ENTRIES();
	*/
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(coro_http)
{
	/* uncomment this line if you have INI entries
	UNREGISTER_INI_ENTRIES();
	*/
	return SUCCESS;
}
/* }}} */

/* Remove if there's nothing to do at request start */
/* {{{ PHP_RINIT_FUNCTION
 */
PHP_RINIT_FUNCTION(coro_http)
{
#if defined(COMPILE_DL_CORO_HTTP) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif
	return SUCCESS;
}
/* }}} */

/* Remove if there's nothing to do at request end */
/* {{{ PHP_RSHUTDOWN_FUNCTION
 */
PHP_RSHUTDOWN_FUNCTION(coro_http)
{
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(coro_http)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "coro_http support", "enabled");
	php_info_print_table_end();

	/* Remove comments if you have entries in php.ini
	DISPLAY_INI_ENTRIES();
	*/
}
/* }}} */

/* {{{ coro_http_functions[]
 *
 * Every user visible function must have an entry in coro_http_functions[].
 */
const zend_function_entry coro_http_functions[] = {
	PHP_FE(coro_http_get,	NULL)		/* For testing, remove later. */
	PHP_FE_END	/* Must be the last line in coro_http_functions[] */
};
/* }}} */

/* {{{ coro_http_module_entry
 */
zend_module_entry coro_http_module_entry = {
	STANDARD_MODULE_HEADER,
	"coro_http",
	coro_http_functions,
	PHP_MINIT(coro_http),
	PHP_MSHUTDOWN(coro_http),
	PHP_RINIT(coro_http),		/* Replace with NULL if there's nothing to do at request start */
	PHP_RSHUTDOWN(coro_http),	/* Replace with NULL if there's nothing to do at request end */
	PHP_MINFO(coro_http),
	PHP_CORO_HTTP_VERSION,
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_CORO_HTTP
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(coro_http)
#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
