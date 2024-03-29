/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=4:tabstop=4:
 */
/*
 *  All rights reserved (c) 2014-2024 CEA/DAM.
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
 * \brief  Resource file of Phobos's Distributed State Service.
 */

#include "pho_common.h"

#include "device.h"
#include "resources.h"

static const struct dss_resource_ops *get_resource_ops(enum dss_type type)
{
    switch (type) {
    case DSS_DEVICE:
        return &device_ops;
    default:
        return NULL;
    }

    UNREACHED();
}

int get_insert_query(enum dss_type type, PGconn *conn, void *void_resource,
                     int item_count, GString *request)
{
    const struct dss_resource_ops *resource_ops = get_resource_ops(type);

    if (resource_ops == NULL)
        return -ENOTSUP;

    return resource_ops->insert_query(conn, void_resource, item_count, request);
}

int get_update_query(enum dss_type type, void *void_resource, int item_count,
                     int64_t fields, GString *request)
{
    const struct dss_resource_ops *resource_ops = get_resource_ops(type);

    if (resource_ops == NULL)
        return -ENOTSUP;

    return resource_ops->update_query(void_resource, item_count, fields,
                                      request);
}

int get_select_query(enum dss_type type, GString *conditions, GString *request)
{
    const struct dss_resource_ops *resource_ops = get_resource_ops(type);

    if (resource_ops == NULL)
        return -ENOTSUP;

    return resource_ops->select_query(conditions, request);
}

int get_delete_query(enum dss_type type, void *void_resource, int item_count,
                     GString *request)
{
    const struct dss_resource_ops *resource_ops = get_resource_ops(type);

    if (resource_ops == NULL)
        return -ENOTSUP;

    return resource_ops->delete_query(void_resource, item_count, request);
}

int create_resource(enum dss_type type, struct dss_handle *handle,
                    void *void_resource, PGresult *res, int row_num)
{
    const struct dss_resource_ops *resource_ops = get_resource_ops(type);

    if (resource_ops == NULL)
        return -ENOTSUP;

    return resource_ops->create(handle, void_resource, res, row_num);
}

// XXX: this will be changed a simple "free" function when all resources are
// managed
res_destructor_t get_free_function(enum dss_type type)
{
    const struct dss_resource_ops *resource_ops = get_resource_ops(type);

    if (resource_ops == NULL)
        return NULL;

    return resource_ops->free;
}

size_t get_resource_size(enum dss_type type)
{
    const struct dss_resource_ops *resource_ops = get_resource_ops(type);

    if (resource_ops == NULL)
        return -ENOTSUP;

    return resource_ops->size;
}
