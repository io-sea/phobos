/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=4:tabstop=4:
 */
/*
 *  All rights reserved (c) 2014-2022 CEA/DAM.
 *
 *  This file is part of Phobos.
 *
 *  Phobos is free software: you can redistribute it and/or modify it under
 *  the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation, either version 2.1 of the License, or
 *  (at your option) any later version.
 *
 *  Phobos is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with Phobos. If not, see <http://www.gnu.org/licenses/>.
 */
/**
 * \brief Phobos attribute management
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pho_attrs.h"
#include "pho_common.h"
#include "assert.h"
#include <malloc.h>
#include <string.h>
#include <errno.h>
#include <jansson.h>

void pho_attrs_free(struct pho_attrs *md)
{
    if (md == NULL || md->attr_set == NULL)
        return;

    g_hash_table_destroy(md->attr_set);
    md->attr_set = NULL;
}

void pho_attr_remove(struct pho_attrs *md, const char *key)
{
    if (md == NULL || md->attr_set == NULL)
        return;

    g_hash_table_remove(md->attr_set, key);
}

const char *pho_attr_get(struct pho_attrs *md, const char *key)
{
    if (md == NULL || md->attr_set == NULL)
        return NULL;

    return g_hash_table_lookup(md->attr_set, key);
}

void pho_attr_set(struct pho_attrs *md, const char *key, const char *value)
{
    char *safe_value;
    char *safe_key;

    if (md->attr_set == NULL) {
        md->attr_set = g_hash_table_new_full(g_str_hash, g_str_equal, free,
                                             free);
        assert(md->attr_set);
    }

    safe_value = xstrdup_safe(value);
    safe_key = xstrdup(key);

    /* use ght_replace, so that previous key and values are freed */
    g_hash_table_replace(md->attr_set, safe_key, safe_value);
}

/** callback function to dump a JSON to a GString
 * It must follow json_dump_callback_t prototype
 * and specified behavior.
 * @return 0 on success, -1 on error.
 */
static int dump_to_gstring(const char *buffer, size_t size, void *data)
{
    if ((ssize_t)size < 0)
        return -1;

    g_string_append_len((GString *)data, buffer, size);
    return 0;
}

static int attr_json_dump_cb(const char *key, const char *value, void *udata)
{
    return json_object_set_new((json_t *)udata, key, json_string(value));
}

/** Serialize an attribute set by converting it to JSON. */
int pho_attrs_to_json_raw(const struct pho_attrs *md, json_t *obj)
{
    return pho_attrs_foreach(md, attr_json_dump_cb, obj);
}

int pho_attrs_to_json(const struct pho_attrs *md, GString *str, int flags)
{
    json_t  *jdata;
    int      rc = 0;

    if (str == NULL)
        return -EINVAL;

    /* make sure the target string is empty */
    g_string_assign(str, "");

    /* return empty JSON object if attr list is empty */
    if (md == NULL || md->attr_set == NULL) {
        g_string_append(str, "{}");
        goto out_nojson;
    }

    jdata = json_object();
    if (jdata == NULL)
        return -ENOMEM;

    rc = pho_attrs_to_json_raw(md, jdata);
    if (rc != 0)
        goto out_free;

    rc = json_dump_callback(jdata, dump_to_gstring, str, flags);

out_free:
    json_decref(jdata);

out_nojson:
    /* jansson does not return a meaningful error code, assume EINVAL */
    return (rc != 0) ? -EINVAL : 0;
}

void pho_json_raw_to_attrs(struct pho_attrs *md, json_t *obj)
{
    void *iter;

    for (iter = json_object_iter(obj);
         iter != NULL;
         iter = json_object_iter_next(obj, iter)) {
        const char  *key = json_object_iter_key(iter);
        json_t      *val = json_object_iter_value(iter);

        pho_attr_set(md, key, json_string_value(val));
    }
}

int pho_json_to_attrs(struct pho_attrs *md, const char *str)
{
    json_error_t jerror;
    json_t *jdata;

    if (str == NULL)
        return -EINVAL;

    jdata = json_loads(str, JSON_REJECT_DUPLICATES, &jerror);
    if (jdata == NULL)
        LOG_RETURN(-EINVAL, "JSON parsing error: %s at position %d",
                   jerror.text, jerror.position);

    pho_json_raw_to_attrs(md, jdata);

    json_decref(jdata);
    return 0;
}

int pho_attrs_foreach(const struct pho_attrs *md, pho_attrs_iter_t cb,
                      void *udata)
{
    GHashTableIter  iter;
    gpointer        key;
    gpointer        value;
    int             rc = 0;

    if (md == NULL || md->attr_set == NULL)
        return 0;

    g_hash_table_iter_init(&iter, md->attr_set);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        rc = cb((const char *)key, (const char *)value, udata);
        if (rc != 0)
            break;
    }

    return rc;
}

static gboolean remove_null_attr(gpointer key, gpointer value, gpointer udata)
{
    if (value == NULL)
        return TRUE;

    return FALSE;
}

void pho_attrs_remove_null(struct pho_attrs *md)
{
    if (md == NULL || md->attr_set == NULL)
        return;

    g_hash_table_foreach_remove(md->attr_set, remove_null_attr, NULL);
}
