/*
 * Astra Module: HTTP Module: MPEG-TS Streaming
 * http://cesbo.com/astra
 *
 * Copyright (C) 2014-2015, Andrey Dyldin <and@cesbo.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <astra.h>
#include "../http.h"

#define DEFAULT_BUFFER_SIZE (1024 * 1024)
#define DEFAULT_BUFFER_FILL (128 * 1024)

struct module_data_t
{
    int idx_callback;

    MODULE_STREAM_DATA();

    uint8_t *shared_buffer;
    size_t shared_size;
    size_t shared_count;
    size_t shared_write;
};

struct http_response_t
{
    MODULE_STREAM_DATA();

    module_data_t *mod;

    uint8_t *buffer;
    size_t buffer_count;
    size_t buffer_read;
    size_t buffer_write;

    size_t buffer_size;
    size_t buffer_fill;

    size_t burst_fill;
    bool is_burst_done;

    size_t burst_target;
    size_t burst_sent;

    bool is_socket_busy;
};

static size_t shared_buffer_copy(module_data_t *mod, uint8_t *dst, size_t dst_size)
{
    if(mod->shared_count == 0 || dst_size == 0)
        return 0;

    const size_t copy_len = (mod->shared_count < dst_size)
                            ? mod->shared_count
                            : dst_size;

    const size_t shared_read = (mod->shared_write + mod->shared_size - mod->shared_count)
                               % mod->shared_size;

    if(shared_read + copy_len <= mod->shared_size)
    {
        memcpy(dst, &mod->shared_buffer[shared_read], copy_len);
    }
    else
    {
        const size_t head = mod->shared_size - shared_read;
        memcpy(dst, &mod->shared_buffer[shared_read], head);
        memcpy(&dst[head], mod->shared_buffer, copy_len - head);
    }

    return copy_len;
}

/*
 * client->mod - http_server module
 * client->response->mod - http_upstream module
 */

static void on_upstream_ready(void *arg)
{
    http_client_t *client = (http_client_t *)arg;
    http_response_t *response = client->response;

    if(response->buffer_count > 0)
    {
        size_t block_size = (response->buffer_write > response->buffer_read)
                          ? (response->buffer_write - response->buffer_read)
                          : (response->buffer_size - response->buffer_read);

        if(block_size > response->buffer_count)
            block_size = response->buffer_count;

        const ssize_t send_size = asc_socket_send(  client->sock
                                                  , &response->buffer[response->buffer_read]
                                                  , block_size);

        if(send_size > 0)
        {
            response->buffer_count -= send_size;
            response->buffer_read += send_size;
            if(response->buffer_read >= response->buffer_size)
                response->buffer_read = 0;

            if(!response->is_burst_done)
            {
                response->burst_sent += send_size;
                if(response->burst_sent >= response->burst_target)
                    response->is_burst_done = true;
            }
        }
        else if(send_size == -1)
        {
            http_client_error(  client, "failed to send ts (%d bytes) [%s]"
                              , block_size, asc_socket_error());
            http_client_close(client);
            return;
        }
    }

    if(response->buffer_count == 0)
    {
        asc_socket_set_on_ready(client->sock, NULL);
        response->is_socket_busy = false;
    }
}

static void on_shared_ts(module_data_t *mod, const uint8_t *ts)
{
    if(mod->shared_size == 0)
        return;

    if(mod->shared_count + TS_PACKET_SIZE > mod->shared_size)
    {
        // overwrite the oldest packet to keep the newest burstable data
        mod->shared_count -= TS_PACKET_SIZE;
    }

    const size_t write_pos = mod->shared_write;
    if(write_pos + TS_PACKET_SIZE <= mod->shared_size)
    {
        memcpy(&mod->shared_buffer[write_pos], ts, TS_PACKET_SIZE);
        mod->shared_write = (write_pos + TS_PACKET_SIZE) % mod->shared_size;
    }
    else
    {
        const size_t head = mod->shared_size - write_pos;
        memcpy(&mod->shared_buffer[write_pos], ts, head);
        memcpy(mod->shared_buffer, &ts[head], TS_PACKET_SIZE - head);
        mod->shared_write = TS_PACKET_SIZE - head;
    }

    mod->shared_count += TS_PACKET_SIZE;
    if(mod->shared_count > mod->shared_size)
        mod->shared_count = mod->shared_size;
}

static void on_ts(void *arg, const uint8_t *ts)
{
    http_client_t *client = (http_client_t *)arg;
    http_response_t *response = client->response;

    if(response->buffer_count + TS_PACKET_SIZE >= response->buffer_size)
    {
        // overflow
        response->buffer_count = 0;
        response->buffer_read = 0;
        response->buffer_write = 0;
        response->burst_sent = 0;
        if(response->is_socket_busy)
        {
            asc_socket_set_on_ready(client->sock, NULL);
            response->is_socket_busy = false;
        }
        return;
    }

    const size_t buffer_write = response->buffer_write + TS_PACKET_SIZE;
    if(buffer_write < response->buffer_size)
    {
        memcpy(&response->buffer[response->buffer_write], ts, TS_PACKET_SIZE);
        response->buffer_write = buffer_write;
    }
    else if(buffer_write > response->buffer_size)
    {
        const size_t ts_head = response->buffer_size - response->buffer_write;
        memcpy(&response->buffer[response->buffer_write], ts, ts_head);
        response->buffer_write = TS_PACKET_SIZE - ts_head;
        memcpy(response->buffer, &ts[ts_head], response->buffer_write);
    }
    else
    {
        memcpy(&response->buffer[response->buffer_write], ts, TS_PACKET_SIZE);
        response->buffer_write = 0;
    }
    response->buffer_count += TS_PACKET_SIZE;

    size_t start_fill = response->is_burst_done
                       ? response->buffer_fill
                       : TS_PACKET_SIZE;

    if(   response->is_socket_busy == false
       && response->buffer_count >= start_fill)
    {
        asc_socket_set_on_ready(client->sock, on_upstream_ready);
        response->is_socket_busy = true;
    }
}

static void on_upstream_read(void *arg)
{
    http_client_t *client = (http_client_t *)arg;

    ssize_t size = asc_socket_recv(client->sock, client->buffer, HTTP_BUFFER_SIZE);
    if(size <= 0)
        http_client_close(client);
}

static void on_upstream_send(void *arg)
{
    http_client_t *client = (http_client_t *)arg;

    module_stream_t *upstream = NULL;
    module_data_t *mod = client->response->mod;

    client->response->buffer_size = DEFAULT_BUFFER_SIZE;
    client->response->buffer_fill = DEFAULT_BUFFER_FILL;
    client->response->burst_fill = DEFAULT_BUFFER_FILL;
    client->response->burst_target = DEFAULT_BUFFER_FILL;
    client->response->burst_sent = 0;
    client->response->is_burst_done = true;

    if(lua_istable(lua, 3))
    {
        lua_getfield(lua, 3, "upstream");
        if(lua_islightuserdata(lua, -1))
            upstream = (module_stream_t *)lua_touserdata(lua, -1);
        lua_pop(lua, 1);

        lua_getfield(lua, 3, "buffer_size");
        if(lua_isnumber(lua, -1))
        {
            client->response->buffer_size = lua_tonumber(lua, -1) * 1024;
            if(client->response->buffer_size == 0)
                client->response->buffer_size = DEFAULT_BUFFER_SIZE;
        }
        lua_pop(lua, 1);

        lua_getfield(lua, 3, "buffer_fill");
        if(lua_isnumber(lua, -1))
        {
            client->response->buffer_fill = lua_tonumber(lua, -1) * 1024;
            if(client->response->buffer_fill == 0)
                client->response->buffer_fill = DEFAULT_BUFFER_FILL;
        }
        lua_pop(lua, 1);

        lua_getfield(lua, 3, "burst_size");
        if(lua_isnumber(lua, -1))
        {
            client->response->burst_fill = lua_tonumber(lua, -1) * 1024;
            if(client->response->burst_fill == 0)
                client->response->burst_fill = DEFAULT_BUFFER_FILL;
            else
            {
                client->response->is_burst_done = false;
                client->response->burst_target = client->response->burst_fill;
            }
        }
        lua_pop(lua, 1);

        const size_t min_size = TS_PACKET_SIZE + 1;

        if(client->response->buffer_size <= client->response->buffer_fill)
        {
            http_client_error(client, "buffer_size must be greater than buffer_fill");
            http_client_abort(client, 500, "server configuration error");
            return;
        }
        if(client->response->buffer_size < client->response->buffer_fill + min_size)
        {
            http_client_error(client, "buffer_size must exceed buffer_fill by at least one TS packet (%u bytes)", TS_PACKET_SIZE);
            http_client_abort(client, 500, "server configuration error");
            return;
        }

        if(client->response->buffer_size <= client->response->burst_fill)
        {
            http_client_error(client, "buffer_size must be greater than burst_size");
            http_client_abort(client, 500, "server configuration error");
            return;
        }
        if(client->response->buffer_size < client->response->burst_fill + min_size)
        {
            http_client_error(client, "buffer_size must exceed burst_size by at least one TS packet (%u bytes)", TS_PACKET_SIZE);
            http_client_abort(client, 500, "server configuration error");
            return;
        }
    }
    else if(lua_islightuserdata(lua, 3))
    {
        upstream = (module_stream_t *)lua_touserdata(lua, 3);
    }

    if(!upstream)
    {
        http_client_abort(client, 500, ":send() client instance required");
        return;
    }

    if(mod->__stream.parent == NULL)
    {
        mod->__stream.on_ts = (void (*)(module_data_t *, const uint8_t *))on_shared_ts;
        mod->__stream.self = mod;
        __module_stream_init(&mod->__stream);
        __module_stream_attach(upstream, &mod->__stream);
    }

    const size_t desired_shared = (client->response->burst_fill > client->response->buffer_size)
                                  ? client->response->burst_fill
                                  : client->response->buffer_size;

    if(mod->shared_size < desired_shared)
    {
        mod->shared_buffer = (uint8_t *)realloc(mod->shared_buffer, desired_shared);
        mod->shared_size = desired_shared;
        mod->shared_count = 0;
        mod->shared_write = 0;
    }

    client->response->buffer = (uint8_t *)malloc(client->response->buffer_size);

    if(mod->shared_buffer && mod->shared_count > 0)
    {
        const size_t preload = shared_buffer_copy(  mod
                                                 , client->response->buffer
                                                 , (client->response->burst_target < client->response->buffer_size)
                                                    ? client->response->burst_target
                                                    : (client->response->buffer_size - TS_PACKET_SIZE));
        client->response->buffer_count = preload;
        client->response->buffer_write = preload % client->response->buffer_size;
        client->response->buffer_read = 0;
        if(preload > 0)
            client->response->is_burst_done = false;
    }

    // like module_stream_init()
    client->response->__stream.self = (void *)client;
    client->response->__stream.on_ts = (void (*)(module_data_t *, const uint8_t *))on_ts;
    __module_stream_init(&client->response->__stream);
    __module_stream_attach(upstream, &client->response->__stream);

    client->on_read = on_upstream_read;
    client->on_ready = NULL;

    const char *content_type = lua_isstring(lua, 4)
                             ? lua_tostring(lua, 4)
                             : "application/octet-stream";

    http_response_code(client, 200, NULL);
    http_response_header(client, "Cache-Control: no-cache");
    http_response_header(client, "Pragma: no-cache");
    http_response_header(client, "Content-Type: %s", content_type);
    http_response_header(client, "Connection: close");
    http_response_send(client);
}

static int module_call(module_data_t *mod)
{
    http_client_t *client = (http_client_t *)lua_touserdata(lua, 3);

    if(lua_isnil(lua, 4))
    {
        if(client->response)
        {
            lua_rawgeti(lua, LUA_REGISTRYINDEX, client->response->mod->idx_callback);
            lua_pushvalue(lua, 2);
            lua_pushvalue(lua, 3);
            lua_pushvalue(lua, 4);
            lua_call(lua, 3, 0);

            module_stream_destroy(client->response);

            free(client->response->buffer);
            free(client->response);
            client->response = NULL;
        }
        return 0;
    }

    client->response = (http_response_t *)calloc(1, sizeof(http_response_t));
    client->response->mod = mod;

    client->on_send = on_upstream_send;

    lua_rawgeti(lua, LUA_REGISTRYINDEX, client->response->mod->idx_callback);
    lua_pushvalue(lua, 2);
    lua_pushvalue(lua, 3);
    lua_pushvalue(lua, 4);
    lua_call(lua, 3, 0);

    return 0;
}

static int __module_call(lua_State *L)
{
    module_data_t *mod = (module_data_t *)lua_touserdata(L, lua_upvalueindex(1));
    return module_call(mod);
}

static void module_init(module_data_t *mod)
{
    lua_getfield(lua, MODULE_OPTIONS_IDX, "callback");
    asc_assert(lua_isfunction(lua, -1), "[http_upstream] option 'callback' is required");
    mod->idx_callback = luaL_ref(lua, LUA_REGISTRYINDEX);

    mod->__stream.self = NULL;
    mod->shared_buffer = NULL;
    mod->shared_size = 0;
    mod->shared_count = 0;
    mod->shared_write = 0;

    // Deprecated
    bool is_deprecated = false;

    lua_getfield(lua, MODULE_OPTIONS_IDX, "buffer_size");
    if(!lua_isnil(lua, -1))
        is_deprecated = true;
    lua_pop(lua, 1);

    lua_getfield(lua, MODULE_OPTIONS_IDX, "buffer_fill");
    if(!lua_isnil(lua, -1))
        is_deprecated = true;
    lua_pop(lua, 1);

    if(is_deprecated)
        asc_log_error("[http_upstream] deprecated usage of the buffer_size/buffer_fill options");
    //

    // Set callback for http route
    lua_getmetatable(lua, 3);
    lua_pushlightuserdata(lua, (void *)mod);
    lua_pushcclosure(lua, __module_call, 1);
    lua_setfield(lua, -2, "__call");
    lua_pop(lua, 1);
}

static void module_destroy(module_data_t *mod)
{
    if(mod->idx_callback)
    {
        luaL_unref(lua, LUA_REGISTRYINDEX, mod->idx_callback);
        mod->idx_callback = 0;
    }

    if(mod->__stream.self)
    {
        __module_stream_destroy(&mod->__stream);
        mod->__stream.self = NULL;
    }

    free(mod->shared_buffer);
    mod->shared_buffer = NULL;
    mod->shared_size = 0;
    mod->shared_count = 0;
    mod->shared_write = 0;
}

MODULE_LUA_METHODS()
{
    { NULL, NULL }
};

MODULE_LUA_REGISTER(http_upstream)
