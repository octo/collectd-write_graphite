/**
 * collectd - src/write_graphite.c
 * Copyright (C) 2012       Pierre-Yves Ritschard
 * Copyright (C) 2011       Scott Sanders
 * Copyright (C) 2009       Paul Sadauskas
 * Copyright (C) 2009       Doug MacEachern
 * Copyright (C) 2007-2012  Florian octo Forster
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; only version 2 of the License is applicable.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * Authors:
 *   Florian octo Forster <octo at collectd.org>
 *   Doug MacEachern <dougm at hyperic.com>
 *   Paul Sadauskas <psadauskas at gmail.com>
 *   Scott Sanders <scott at jssjr.com>
 *   Pierre-Yves Ritschard <pyr at spootnik.org>
 *
 * Based on the write_http plugin.
 **/

 /* write_graphite plugin configuation example
  *
  * <Plugin write_graphite>
  *   <Carbon>
  *     Host "localhost"
  *     Port "2003"
  *     Prefix "collectd"
  *   </Carbon>
  * </Plugin>
  */

#include "collectd.h"
#include "common.h"
#include "plugin.h"
#include "configfile.h"

#include "utils_cache.h"
#include "utils_parse_option.h"

/* Folks without pthread will need to disable this plugin. */
#include <pthread.h>

#include <sys/socket.h>
#include <netdb.h>

#ifndef WG_FORMAT_NAME
#define WG_FORMAT_NAME(ret, ret_len, vl, cb, ds_name) \
        wg_format_name (ret, ret_len, (vl)->host, \
                         (vl)->plugin, (vl)->plugin_instance, \
                         (vl)->type, (vl)->type_instance, \
                         (cb)->prefix, (cb)->postfix, \
                         ds_name, (cb)->escape_char)
#endif

#ifndef WG_DEFAULT_NODE
# define WG_DEFAULT_NODE "localhost"
#endif

#ifndef WG_DEFAULT_SERVICE
# define WG_DEFAULT_SERVICE "2003"
#endif

#ifndef WG_SEND_BUF_SIZE
# define WG_SEND_BUF_SIZE 4096
#endif

/*
 * Private variables
 */
struct wg_callback
{
    int      sock_fd;

    char    *node;
    char    *service;
    char    *prefix;
    char    *postfix;
    char     escape_char;

    char     send_buf[WG_SEND_BUF_SIZE];
    size_t   send_buf_free;
    size_t   send_buf_fill;
    cdtime_t send_buf_init_time;

    pthread_mutex_t send_lock;
};


/*
 * Functions
 */
static void wg_reset_buffer (struct wg_callback *cb)
{
    memset (cb->send_buf, 0, sizeof (cb->send_buf));
    cb->send_buf_free = sizeof (cb->send_buf);
    cb->send_buf_fill = 0;
    cb->send_buf_init_time = cdtime ();
}

static int wg_send_buffer (struct wg_callback *cb)
{
    int status = 0;

    status = write (cb->sock_fd, cb->send_buf, strlen (cb->send_buf));
    if (status < 0)
    {
        ERROR ("write_graphite plugin: send failed with "
                "status %i (%s)",
                status,
                strerror (errno));

        close (cb->sock_fd);
        cb->sock_fd = -1;

        return (-1);
    }
    return (0);
}

/* NOTE: You must hold cb->send_lock when calling this function! */
static int wg_flush_nolock (cdtime_t timeout, struct wg_callback *cb)
{
    int status;

    DEBUG ("write_graphite plugin: wg_flush_nolock: timeout = %.3f; "
            "send_buf_fill = %zu;",
            (double)timeout,
            cb->send_buf_fill);

    /* timeout == 0  => flush unconditionally */
    if (timeout > 0)
    {
        cdtime_t now;

        now = cdtime ();
        if ((cb->send_buf_init_time + timeout) > now)
            return (0);
    }

    if (cb->send_buf_fill <= 0)
    {
        cb->send_buf_init_time = cdtime ();
        return (0);
    }

    status = wg_send_buffer (cb);
    wg_reset_buffer (cb);

    return (status);
}

static int wg_callback_init (struct wg_callback *cb)
{
    struct addrinfo ai_hints;
    struct addrinfo *ai_list;
    struct addrinfo *ai_ptr;
    int status;

    const char *node = cb->node ? cb->node : WG_DEFAULT_NODE;
    const char *service = cb->service ? cb->service : WG_DEFAULT_SERVICE;

    if (cb->sock_fd > 0)
        return (0);

    memset (&ai_hints, 0, sizeof (ai_hints));
#ifdef AI_ADDRCONFIG
    ai_hints.ai_flags |= AI_ADDRCONFIG;
#endif
    ai_hints.ai_family = AF_UNSPEC;
    ai_hints.ai_socktype = SOCK_STREAM;

    ai_list = NULL;

    status = getaddrinfo (node, service, &ai_hints, &ai_list);
    if (status != 0)
    {
        ERROR ("write_graphite plugin: getaddrinfo (%s, %s) failed: %s",
                node, service, gai_strerror (status));
        return (-1);
    }

    assert (ai_list != NULL);
    for (ai_ptr = ai_list; ai_ptr != NULL; ai_ptr = ai_ptr->ai_next)
    {
        cb->sock_fd = socket (ai_ptr->ai_family, ai_ptr->ai_socktype,
                ai_ptr->ai_protocol);
        if (cb->sock_fd < 0)
            continue;

        status = connect (cb->sock_fd, ai_ptr->ai_addr, ai_ptr->ai_addrlen);
        if (status != 0)
        {
            close (cb->sock_fd);
            cb->sock_fd = -1;
            continue;
        }

        break;
    }

    freeaddrinfo (ai_list);

    if (cb->sock_fd < 0)
    {
        char errbuf[1024];
        ERROR ("write_graphite plugin: Connecting to %s:%s failed. "
                "The last error was: %s", node, service,
                sstrerror (errno, errbuf, sizeof (errbuf)));
        close (cb->sock_fd);
        return (-1);
    }

    wg_reset_buffer (cb);

    return (0);
}

static void wg_callback_free (void *data)
{
    struct wg_callback *cb;

    if (data == NULL)
        return;

    cb = data;

    pthread_mutex_lock (&cb->send_lock);

    wg_flush_nolock (/* timeout = */ 0, cb);

    close(cb->sock_fd);
    cb->sock_fd = -1;

    sfree(cb->node);
    sfree(cb->service);
    sfree(cb->prefix);
    sfree(cb->postfix);

    pthread_mutex_destroy (&cb->send_lock);

    sfree(cb);
}

static int wg_flush (cdtime_t timeout,
        const char *identifier __attribute__((unused)),
        user_data_t *user_data)
{
    struct wg_callback *cb;
    int status;

    if (user_data == NULL)
        return (-EINVAL);

    cb = user_data->data;

    pthread_mutex_lock (&cb->send_lock);

    if (cb->sock_fd < 0)
    {
        status = wg_callback_init (cb);
        if (status != 0)
        {
            ERROR ("write_graphite plugin: wg_callback_init failed.");
            pthread_mutex_unlock (&cb->send_lock);
            return (-1);
        }
    }

    status = wg_flush_nolock (timeout, cb);
    pthread_mutex_unlock (&cb->send_lock);

    return (status);
}

static int wg_format_values (char *ret, size_t ret_len,
        int ds_num, const data_set_t *ds, const value_list_t *vl,
        _Bool store_rates)
{
    size_t offset = 0;
    int status;
    gauge_t *rates = NULL;

    assert (0 == strcmp (ds->type, vl->type));

    memset (ret, 0, ret_len);

#define BUFFER_ADD(...) do { \
    status = ssnprintf (ret + offset, ret_len - offset, \
            __VA_ARGS__); \
    if (status < 1) \
    { \
        sfree (rates); \
        return (-1); \
    } \
    else if (((size_t) status) >= (ret_len - offset)) \
    { \
        sfree (rates); \
        return (-1); \
    } \
    else \
    offset += ((size_t) status); \
} while (0)

    if (ds->ds[ds_num].type == DS_TYPE_GAUGE)
        BUFFER_ADD ("%f", vl->values[ds_num].gauge);
    else if (store_rates)
    {
        if (rates == NULL)
            rates = uc_get_rate (ds, vl);
        if (rates == NULL)
        {
            WARNING ("format_values: "
                    "uc_get_rate failed.");
            return (-1);
        }
        BUFFER_ADD ("%g", rates[ds_num]);
    }
    else if (ds->ds[ds_num].type == DS_TYPE_COUNTER)
        BUFFER_ADD ("%llu", vl->values[ds_num].counter);
    else if (ds->ds[ds_num].type == DS_TYPE_DERIVE)
        BUFFER_ADD ("%"PRIi64, vl->values[ds_num].derive);
    else if (ds->ds[ds_num].type == DS_TYPE_ABSOLUTE)
        BUFFER_ADD ("%"PRIu64, vl->values[ds_num].absolute);
    else
    {
        ERROR ("format_values plugin: Unknown data source type: %i",
                ds->ds[ds_num].type);
        sfree (rates);
        return (-1);
    }

#undef BUFFER_ADD

    sfree (rates);
    return (0);
}

static void wg_copy_escape_part (char *dst, const char *src, size_t dst_len,
    char escape_char)
{
    size_t i;

    memset (dst, 0, dst_len);

    if (src == NULL)
        return;

    for (i = 0; i < dst_len; i++)
    {
        if ((src[i] == '.')
                || isspace ((int) src[i])
                || iscntrl ((int) src[i]))
            dst[i] = escape_char;
        else
            dst[i] = src[i];

        if (src[i] == 0)
            break;
    }
}

static int wg_format_name (char *ret, int ret_len,
        const char *hostname,
        const char *plugin, const char *plugin_instance,
        const char *type, const char *type_instance,
        const char *prefix, const char *postfix,
        const char *ds_name, char escape_char)
{
    char n_hostname[DATA_MAX_NAME_LEN];
    char n_plugin[DATA_MAX_NAME_LEN];
    char n_plugin_instance[DATA_MAX_NAME_LEN];
    char n_type[DATA_MAX_NAME_LEN];
    char n_type_instance[DATA_MAX_NAME_LEN];
    int  status;

    assert (hostname != NULL);
    assert (plugin != NULL);
    assert (type != NULL);
    assert (ds_name != NULL);

    if (prefix == NULL)
        prefix = "";

    if (postfix == NULL)
        postfix = "";

    wg_copy_escape_part (n_hostname, hostname,
            sizeof (n_hostname), escape_char);
    wg_copy_escape_part (n_plugin, plugin,
            sizeof (n_plugin), escape_char);
    wg_copy_escape_part (n_plugin_instance, plugin_instance,
            sizeof (n_plugin_instance), escape_char);
    wg_copy_escape_part (n_type, type,
            sizeof (n_type), escape_char);
    wg_copy_escape_part (n_type_instance, type_instance,
            sizeof (n_type_instance), escape_char);

    if (n_plugin_instance[0] == '\0')
    {
        if (n_type_instance[0] == '\0')
        {
            status = ssnprintf (ret, ret_len, "%s%s%s.%s.%s.%s",
                    prefix, n_hostname, postfix, n_plugin, n_type, ds_name);
        }
        else
        {
            status = ssnprintf (ret, ret_len, "%s%s%s.%s.%s-%s.%s",
                    prefix, n_hostname, postfix, n_plugin, n_type,
                    n_type_instance, ds_name);
        }
    }
    else
    {
        if (n_type_instance[0] == '\0')
        {
            status = ssnprintf (ret, ret_len, "%s%s%s.%s.%s.%s.%s",
                    prefix, n_hostname, postfix, n_plugin,
                    n_plugin_instance, n_type, ds_name);
        }
        else
        {
            status = ssnprintf (ret, ret_len, "%s%s%s.%s.%s.%s-%s.%s",
                    prefix, n_hostname, postfix, n_plugin,
                    n_plugin_instance, n_type, n_type_instance, ds_name);
        }
    }

    if ((status < 1) || (status >= ret_len))
        return (-1);

    return (0);
}

static int wg_send_message (const char* key, const char* value,
        cdtime_t time, struct wg_callback *cb)
{
    int status;
    size_t message_len;
    char message[1024];

    message_len = (size_t) ssnprintf (message, sizeof (message),
            "%s %s %.0f\n",
            key,
            value,
            CDTIME_T_TO_DOUBLE(time));
    if (message_len >= sizeof (message)) {
        ERROR ("write_graphite plugin: message buffer too small: "
                "Need %zu bytes.", message_len + 1);
        return (-1);
    }


    pthread_mutex_lock (&cb->send_lock);

    if (cb->sock_fd < 0)
    {
        status = wg_callback_init (cb);
        if (status != 0)
        {
            ERROR ("write_graphite plugin: wg_callback_init failed.");
            pthread_mutex_unlock (&cb->send_lock);
            return (-1);
        }
    }

    if (message_len >= cb->send_buf_free)
    {
        status = wg_flush_nolock (/* timeout = */ 0, cb);
        if (status != 0)
        {
            pthread_mutex_unlock (&cb->send_lock);
            return (status);
        }
    }
    assert (message_len < cb->send_buf_free);

    /* `message_len + 1' because `message_len' does not include the
     * trailing null byte. Neither does `send_buffer_fill'. */
    memcpy (cb->send_buf + cb->send_buf_fill,
            message, message_len + 1);
    cb->send_buf_fill += message_len;
    cb->send_buf_free -= message_len;

    DEBUG ("write_graphite plugin: <%s:%s> buf %zu/%zu (%g%%) \"%s\"",
            cb->node,
            cb->service,
            cb->send_buf_fill, sizeof (cb->send_buf),
            100.0 * ((double) cb->send_buf_fill) / ((double) sizeof (cb->send_buf)),
            message);

    /* Check if we have enough space for this message. */
    pthread_mutex_unlock (&cb->send_lock);

    return (0);
}

static int wg_write_messages (const data_set_t *ds, const value_list_t *vl,
        struct wg_callback *cb)
{
    char key[10*DATA_MAX_NAME_LEN];
    char values[512];

    int status, i;

    if (0 != strcmp (ds->type, vl->type))
    {
        ERROR ("write_graphite plugin: DS type does not match "
                "value list type");
        return -1;
    }

    if (ds->ds_num > 1)
    {
        for (i = 0; i < ds->ds_num; i++)
        {
            /* Copy the identifier to `key' and escape it. */
            status = WG_FORMAT_NAME (key, sizeof (key), vl, cb, ds->ds[i].name);
            if (status != 0)
            {
                ERROR ("write_graphite plugin: error with format_name");
                return (status);
            }

            escape_string (key, sizeof (key));
            /* Convert the values to an ASCII representation and put that
             * into `values'. */
            status = wg_format_values (values, sizeof (values), i, ds, vl, 0);
            if (status != 0)
            {
                ERROR ("write_graphite plugin: error with "
                        "wg_format_values");
                return (status);
            }

            /* Send the message to graphite */
            status = wg_send_message (key, values, vl->time, cb);
            if (status != 0)
            {
                ERROR ("write_graphite plugin: error with "
                        "wg_send_message");
                return (status);
            }
        }
    }
    else
    {
        /* Copy the identifier to `key' and escape it. */
        status = WG_FORMAT_NAME (key, sizeof (key), vl, cb, NULL);
        if (status != 0)
        {
            ERROR ("write_graphite plugin: error with format_name");
            return (status);
        }

        escape_string (key, sizeof (key));
        /* Convert the values to an ASCII representation and put that into
         * `values'. */
        status = wg_format_values (values, sizeof (values), 0, ds, vl, 0);
        if (status != 0)
        {
            ERROR ("write_graphite plugin: error with "
                    "wg_format_values");
            return (status);
        }

        /* Send the message to graphite */
        status = wg_send_message (key, values, vl->time, cb);
        if (status != 0)
        {
            ERROR ("write_graphite plugin: error with "
                    "wg_send_message");
            return (status);
        }
    }

    return (0);
}

static int wg_write (const data_set_t *ds, const value_list_t *vl,
        user_data_t *user_data)
{
    struct wg_callback *cb;
    int status;

    if (user_data == NULL)
        return (-EINVAL);

    cb = user_data->data;

    status = wg_write_messages (ds, vl, cb);

    return (status);
}

static int config_set_char (char *dest,
        oconfig_item_t *ci)
{
    if ((ci->values_num != 1) || (ci->values[0].type != OCONFIG_TYPE_STRING))
    {
        WARNING ("write_graphite plugin: The `%s' config option "
                "needs exactly one string argument.", ci->key);
        return (-1);
    }

    *dest = ci->values[0].value.string[0];

    return (0);
}

static int wg_config_carbon (oconfig_item_t *ci)
{
    struct wg_callback *cb;
    user_data_t user_data;
    int i;

    cb = malloc (sizeof (*cb));
    if (cb == NULL)
    {
        ERROR ("write_graphite plugin: malloc failed.");
        return (-1);
    }
    memset (cb, 0, sizeof (*cb));
    cb->sock_fd = -1;
    cb->node = NULL;
    cb->service = NULL;
    cb->prefix = NULL;
    cb->postfix = NULL;
    cb->escape_char = '_';

    pthread_mutex_init (&cb->send_lock, /* attr = */ NULL);

    for (i = 0; i < ci->children_num; i++)
    {
        oconfig_item_t *child = ci->children + i;

        if (strcasecmp ("Host", child->key) == 0)
            cf_util_get_string (child, &cb->node);
        else if (strcasecmp ("Port", child->key) == 0)
            cf_util_get_string (child, &cb->service);
        else if (strcasecmp ("Prefix", child->key) == 0)
            cf_util_get_string (child, &cb->prefix);
        else if (strcasecmp ("Postfix", child->key) == 0)
            cf_util_get_string (child, &cb->postfix);
        else if (strcasecmp ("EscapeCharacter", child->key) == 0)
            config_set_char (&cb->escape_char, child);
        else
        {
            ERROR ("write_graphite plugin: Invalid configuration "
                        "option: %s.", child->key);
        }
    }

    DEBUG ("write_graphite: Registering write callback to carbon agent %s:%s",
            cb->node ? cb->node : WG_DEFAULT_NODE,
            cb->service ? cb->service : WG_DEFAULT_SERVICE);

    memset (&user_data, 0, sizeof (user_data));
    user_data.data = cb;
    user_data.free_func = NULL;
    plugin_register_flush ("write_graphite", wg_flush, &user_data);

    user_data.free_func = wg_callback_free;
    plugin_register_write ("write_graphite", wg_write, &user_data);

    return (0);
}

static int wg_config (oconfig_item_t *ci)
{
    int i;

    for (i = 0; i < ci->children_num; i++)
    {
        oconfig_item_t *child = ci->children + i;

        if (strcasecmp ("Carbon", child->key) == 0)
            wg_config_carbon (child);
        else
        {
            ERROR ("write_graphite plugin: Invalid configuration "
                    "option: %s.", child->key);
        }
    }

    return (0);
}

void module_register (void)
{
    plugin_register_complex_config ("write_graphite", wg_config);
}

/* vim: set sw=4 ts=4 sts=4 tw=78 et : */
