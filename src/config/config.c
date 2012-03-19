/**
 *
 * Copyright (c) 2010, Zed A. Shaw and Mongrel2 Project Contributors.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 *     * Neither the name of the Mongrel2 Project, Zed A. Shaw, nor the names
 *       of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written
 *       permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "config/config.h"

#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>


#include "adt/tst.h"
#include "dir.h"
#include "dbg.h"
#include "mime.h"
#include "proxy.h"
#include "server.h"
#include "setting.h"
//#include "config/module.h"
//#include "config/db.h"
//#include "filter.h"
#include <dlfcn.h>

#include <lua.h>
#include <lauxlib.h>

extern tst_t *MIME_MAP;
extern tst_t *SETTINGS_MAP;


Handler *Config_load_handler(lua_State *L)
{
    // now, the top of stack is router item table
    check(lua_istable(L, -1), "wrong table parameter: router item.\n");

    lua_getfield(L, -1, "send_spec");
	bstring send_spec = bfromcstr(lua_tostring(L, -1));
	lua_getfield(L, -2, "send_ident");
	bstring send_ident = bfromcstr(lua_tostring(L, -1));
	lua_getfield(L, -3, "recv_spec");
	bstring recv_spec = bfromcstr(lua_tostring(L, -1));
	lua_getfield(L, -4, "recv_ident");
	bstring recv_ident = bfromcstr(lua_tostring(L, -1));
	// clean, make the stack top the router item table
	lua_pop(L, 4);

    Handler *handler = Handler_create(
            send_spec,
            send_ident,
            recv_spec,
            recv_ident
    );
    check(handler != NULL, "Failed to create handler.");

    handler->raw = 1;
    handler->protocol = HANDLER_PROTO_TNET;

    log_info("Loaded handler %s:%s:%s:%s",
            bdata(handler->send_spec),
            bdata(handler->send_ident),
            bdata(handler->recv_spec),
            bdata(handler->recv_ident));

    return handler;
error:
    return NULL;
}


Proxy *Config_load_proxy(lua_State *L)
{
    // now, the top of stack is router item table
    check(lua_istable(L, -1), "wrong table parameter: router item.\n");

    lua_getfield(L, -1, "addr");
	bstring addr = bfromcstr(lua_tostring(L, -1));
	lua_getfield(L, -2, "port");
	int port = lua_tointeger(L, -1);
	// clean, make the stack top the router item table
	lua_pop(L, 2);

    Proxy *proxy = Proxy_create(
            addr,
            port
	);
    check(proxy != NULL, "Failed to create proxy.");

    log_info("Loaded proxy %s:%d",
            bdata(proxy->server), proxy->port);

    return proxy;
error:
    return NULL;
}


Dir *Config_load_dir(lua_State *L)
{
    // now, the top of stack is router table
    check(lua_istable(L, -1), "wrong table parameter: router item.\n");

    lua_getfield(L, -1, "base");
	bstring base = bfromcstr(lua_tostring(L, -1));
	lua_getfield(L, -2, "index_file");
	bstring index_file = bfromcstr(lua_tostring(L, -1));
	lua_getfield(L, -3, "default_ctype");
	bstring default_ctype = bfromcstr(lua_tostring(L, -1));
	lua_getfield(L, -4, "cache_ttl");
	int cache_ttl = lua_tointeger(L, -1);
	// clean, make the stack top the router item table
	lua_pop(L, 4);

    Dir *dir = Dir_create(
    		base,
    		index_file,
    		default_ctype,
    		cache_ttl
	);
    check(dir != NULL, "Failed to create directory.");

    log_info("Loaded directory %s:%s",
            bdata(dir->base), bdata(dir->index_file));

    return dir;
error:
    return NULL;
}

static inline Handler *Config_push_unique_handler(Server *srv, Handler *handler)
{
    int i = 0;

    // roll through the list of handlers and break if we find one
    // this is because, unlike other backends, handlers can't have
    // the same send/recv idents.
    for(i = 0; i < darray_end(srv->handlers); i++) {
        Handler *test = darray_get(srv->handlers, i);
        int same_send = biseq(test->send_spec, handler->send_spec);
        int same_recv = biseq(test->recv_spec, handler->recv_spec);

        if(same_send && same_recv) {
            Handler_destroy(handler);
            // WARNING: this breaks out and ends the loop
            return test;
        } else if(same_send) {
            log_warn("You have two handlers with the same send_spec: %s",
                    bdata(handler->send_spec));
        } else if(same_recv) {
            log_warn("You have two handlers with the same recv_spec: %s",
                    bdata(handler->recv_spec));
        }
    }

    // nothing was found since the loop completed, so add this one
    // and return it as the one the caller should use
    darray_push(srv->handlers, handler);
    return handler;
}

int Config_load_routes(lua_State *L, Server *srv, Host *host, int host_id, int server_id)
{
    int i = 0, rc;
    
    // now, the top of stack is host item table
    lua_getfield(L, -1, "routes");
    check(lua_istable(L, -1), "wrong table parameter: routes.\n");

    // now, the top of stack is routes table    
    lua_pushnil(L);  // first key to pop
    // index -2 is the host table
    while (lua_next(L, -2) != 0) {
    	// key at -2, value at -1
    	bstring path = bfromcstr(lua_tostring(L, -2));
    	check(lua_istable(L, -1), "wrong table parameter: router item.\n");
    	lua_getfield(L, -1, "type");
    	bstring type = bfromcstr(lua_tostring(L, -1));
    	// make the router table at the top
    	lua_pop(L, 1);
    	
    	void *target = NULL;
        BackendType backend_type = 0;

        if (biseqcstr(type, "dir")) {
            target = Config_load_dir(L);
            backend_type = BACKEND_DIR;
        } else if (biseqcstr(type, "proxy")) {
            target = Config_load_proxy(L);
            backend_type = BACKEND_PROXY;
        } else if (biseqcstr(type, "handler")) {
            Handler *temp = Config_load_handler(L);
            target = Config_push_unique_handler(srv, temp);
            check(target != NULL, "Failure pushing handler %s.", bdata(path));
            backend_type = BACKEND_HANDLER;
        } else {
            sentinel("Invalid backend type: %s for route :%s.", bdata(type), bdata(path));
        }

        check(target != NULL, "Failed to load backend type: %s for route %s.",
                bdata(type), bdata(path));
        
        // add it to the route map for this host
        rc = Host_add_backend(host, path, backend_type, target);
        check(rc == 0, "Failed to add route %s to host %s.",
                bdata(path), bdata(host->name));

        log_info("Loaded route %s:%s for host %d:%s",
                bdata(path), bdata(type),
                host_id, bdata(host->name));
   
        /* removes 'value'; keeps 'key' for next iteration */
        lua_pop(L, 1);
        i++;
    }
    
    // pop the last key, make the stack top host item table
    lua_pop(L, 2);

    return i;

error:
    return -1;
}

/*
int Config_load_filters(Server *srv, int server_id)
{
    tns_value_t *res = CONFIG_MODULE.load_filters(server_id);
    int cols = 0;
    int rows = DB_counts(res, &cols);
    int row_i = 0;
    int rc = 0;

    for(row_i = 0; row_i < rows; row_i++) {
        DB_check(res, row_i, 3, 
            tns_tag_number, tns_tag_string, tns_tag_string);

        int id = DB_get_as(res, row_i, 0, number);
        bstring filter_name = DB_get_as(res, row_i, 1, string);
        bstring raw_settings = DB_get_as(res, row_i, 2, string);

        char *remain = NULL;
        tns_value_t *config = tns_parse(bdata(raw_settings), blength(raw_settings), &remain);

        check(config != NULL, "Failed to parse the settings for Filter '%s' id='%d'",
                bdata(filter_name), id);
        check(tns_get_type(config) == tns_tag_dict,
                "Settings for a filter must be a dict.");

        rc = Filter_load(srv, filter_name, config);
        check(rc == 0, "Failed to load filter '%s' id='%d'", bdata(filter_name), id);
    }

    tns_value_destroy(res);
    return 0;

error:
    if(res) tns_value_destroy(res);
    return 1;
}
*/

int Config_load_hosts(lua_State *L, Server *srv, int server_id)
{
    int i, rc;

    // now the stack top is hosts table
    lua_getfield(L, -1, "hosts");
    check(lua_istable(L, -1), "wrong table parameter: hosts.\n");
    
	//printf("%d\n", lua_objlen(L, -1));    
    for (i=1; i<=lua_objlen(L, -1); i++) {
    	//printf("%d\n", i);
    	// push the host[i] into the stack
    	lua_rawgeti(L, -1, i);
        check(lua_istable(L, -1), "wrong table parameter: host %d.\n", i);

		lua_getfield(L, -1, "name");
        check(lua_isstring(L, -1), "wrong string parameter: host %d's name.\n", i);
        bstring name = bfromcstr(lua_tostring(L, -1));  // name

        lua_getfield(L, -2, "matching");
        check(lua_isstring(L, -1), "wrong string parameter: host %d's matching.\n", i);
        bstring matching = bfromcstr(lua_tostring(L, -1));  // matching                
        // clean the stack
        lua_pop(L, 2);
        
        Host *host = Host_create(
        		name,
        		matching
		);
        check_mem(host);

        int host_id = i;
        rc = Config_load_routes(L, srv, host, host_id, server_id);
        check(rc != -1, "Failed to load routes for host %s.", bdata(host->name));

        rc = Server_add_host(srv, host);
        check(rc == 0, "Failed to add host %s:%s to server.",
                bdata(host->name), bdata(host->matching));

        if(biseq(srv->default_hostname, host->name)) {
            srv->default_host = host;
        }
        
        //lua_pop(L, 1);
    }
    
    log_info("Loaded %d hosts for server %d:%s", i, server_id, bdata(srv->uuid));

    lua_pop(L, 1);
    return 0;

error:
    return -1;

}


Server *Config_load_server(lua_State *L, const char *name)
{
    int i, rc;
    int which = 0;
    
    // get the global table settings
    lua_getglobal(L, "servers");
    check(lua_istable(L, -1), "wrong table parameter: servers.\n");
    
    for (i=1; i<=lua_objlen(L, -1); i++) {
    	// push the servers[i] into the stack
    	lua_rawgeti(L, -1, i);
        check(lua_istable(L, -1), "wrong table parameter: server %d.\n", i);
		lua_getfield(L, -1, "name");
        check(lua_isstring(L, -1), "wrong string parameter: server %d's name.\n", i);
		const char *_name = lua_tostring(L, -1);
		// if the name are equal
		if (!strcmp(_name, name)) {
			which = i;
			lua_pop(L, 1);
			break;
		}
		
		lua_pop(L, 2);
    }
    
    check(which != 0, "No server find by this name.\n");
    
    // now the stack top is servers[which] table
    lua_getfield(L, -1, "uuid");
	bstring uuid = bfromcstr(lua_tostring(L, -1));
    lua_getfield(L, -2, "default_host");
	bstring default_host = bfromcstr(lua_tostring(L, -1));
    lua_getfield(L, -3, "bind_addr");
	bstring bind_addr = bfromcstr(lua_tostring(L, -1));
    lua_getfield(L, -4, "port");
	int port = lua_tointeger(L, -1);
    lua_getfield(L, -5, "access_log");
	bstring access_log = bfromcstr(lua_tostring(L, -1));
    lua_getfield(L, -6, "error_log");
	bstring error_log = bfromcstr(lua_tostring(L, -1));
    lua_getfield(L, -7, "pid_file");
	bstring pid_file = bfromcstr(lua_tostring(L, -1));
    lua_getfield(L, -8, "use_ssl");
	int use_ssl = lua_tointeger(L, -1);
	// clean stack, keep the server table at the top
	lua_pop(L, 8);
    
    Server *srv = Server_create(
			uuid,
			default_host,
			bind_addr,
			port,
			access_log,
			error_log,
			pid_file,
			use_ssl
	);
    check(srv != NULL, "Failed to create server %s", uuid);
    
    rc = Config_load_hosts(L, srv, which);
    check(rc == 0, "Failed to load the hosts for server: %s", bdata(srv->uuid));

    //rc = Config_load_filters(srv, which);
    //check(rc == 0, "Failed to load the filters for server: %s", bdata(srv->uuid));

    // pop the server[which] from the stack
    lua_pop(L, 1);
    return srv;

error:
    return NULL;
}

int Config_load_mimetypes(lua_State* L)
{
    // get the global table settings
    lua_getglobal(L, "mimetypes");
    check(lua_istable(L, -1), "wrong table parameter: mimetypes.\n");
    
    lua_pushnil(L);  /* first key */
    // index -2 is the settings table
    while (lua_next(L, -2) != 0) {
    	/* uses 'key' (at index -2) and 'value' (at index -1) */
    	bstring key_str = bfromcstr(lua_tostring(L, -2));
		bReverse(key_str);
		bstring value_str = bfromcstr(lua_tostring(L, -1));
	    check(!tst_search(MIME_MAP, bdata(key_str), blength(value_str)), 
            	"Mimetypes key %s already exists, can't add %s:%s", key_str, key_str, value_str);
		
		// printf("%s -- %s \n", bdata(key_str), bdata(value_str));
		// add to global SETTINGS_MAP structure
		MIME_MAP = tst_insert(MIME_MAP, bdata(key_str), blength(key_str), value_str);

	    bdestroy(key_str);
        /* removes 'value'; keeps 'key' for next iteration */
        lua_pop(L, 1);
    }
    
    lua_pop(L, 2);

    return 0;
error: // falthrough

    return -1;
}

int Config_load_settings(lua_State* L)
{
    // get the global table settings
    lua_getglobal(L, "settings");
    check(lua_istable(L, -1), "wrong table parameter: settings.\n");
    
    lua_pushnil(L);  /* first key */
    // index -2 is the settings table
    while (lua_next(L, -2) != 0) {
    	/* uses 'key' (at index -2) and 'value' (at index -1) */
    	bstring key_str = bfromcstr(lua_tostring(L, -2));
		bstring value_str = bfromcstr(lua_tostring(L, -1));
	    check(!tst_search(SETTINGS_MAP, bdata(key_str), blength(value_str)), 
            	"Setting key %s already exists, can't add %s:%s", key_str, key_str, value_str);

		// printf("%s == %s \n", bdata(key_str), bdata(value_str));		            																																					
		// add to global SETTINGS_MAP structure
		SETTINGS_MAP = tst_insert(SETTINGS_MAP, bdata(key_str), blength(key_str), value_str);

	    bdestroy(key_str);
        /* removes 'value'; keeps 'key' for next iteration */
        lua_pop(L, 1);
    }
    
    lua_pop(L, 2);

    return 0;
error: // falthrough

    return -1;
}

