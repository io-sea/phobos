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
 * \brief  Phobos RADOS I/O adapter.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "io_posix_common.h"
#include "pho_attrs.h"
#include "pho_common.h"
#include "pho_io.h"
#include "pho_ldm.h"
#include "pho_module_loader.h"

#include <attr/xattr.h>
#include <attr/attributes.h>
#include <rados/librados.h>
#include <sys/types.h>

#define PLUGIN_NAME     "rados"
#define PLUGIN_MAJOR    0
#define PLUGIN_MINOR    1

static struct module_desc IO_ADAPTER_RADOS_MODULE_DESC = {
    .mod_name  = PLUGIN_NAME,
    .mod_major = PLUGIN_MAJOR,
    .mod_minor = PLUGIN_MINOR,
};

struct pho_rados_io_ctx {
    rados_ioctx_t pool_io_ctx;
    struct lib_handle lib_hdl;
};

/**
 * Return a new pho_rados_io_ctx initialized to null.
 *
 * To free this io_ctx, call pho_rados_close.
 */
static struct pho_rados_io_ctx *alloc_pho_rados_io_ctx(void)
{
    struct pho_rados_io_ctx *io_ctx;

    io_ctx = malloc(sizeof(*io_ctx));
    if (io_ctx == NULL)
        return NULL;

    io_ctx->pool_io_ctx = NULL;
    io_ctx->lib_hdl.lh_lib = NULL;
    io_ctx->lib_hdl.ld_module = NULL;

    return io_ctx;
}

/* set an extended attribute (or remove it if value is NULL) */
static int pho_rados_setxattr(rados_ioctx_t pool_io_ctx, const char *extentname,
                              const char *name, const char *value, int flags)
{
     char *tmp_name;
     char tmp_val[15];
     int rc = 0;

     ENTRY;

     if (name == NULL || name[0] == '\0')
        return -EINVAL;

     tmp_name = full_xattr_name(name);
     if (tmp_name == NULL)
        return -ENOMEM;

     if (value != NULL) {
        if ((flags & XATTR_CREATE) &&
            rados_getxattr(pool_io_ctx, extentname, tmp_name, tmp_val, 15) > 0)
            LOG_GOTO(free_tmp, rc = -EEXIST, "setxattr failed : Extended "
                                             "attribute already exists");

        /* By default rados_setxattr replaces the xattr value */
        rc = rados_setxattr(pool_io_ctx, extentname, tmp_name, value,
                            strlen(value));
        if (rc != 0)
            LOG_GOTO(free_tmp, rc = -errno, "setxattr failed");

     } else if (flags & XATTR_REPLACE) {
        rc = rados_rmxattr(pool_io_ctx, extentname, tmp_name);
        if (rc != 0)
            LOG_GOTO(free_tmp, rc = -errno, "removexattr failed");
     } /* else : noop */

free_tmp:
     free(tmp_name);
     return rc;
}

static int pho_rados_getxattr(rados_ioctx_t pool_io_ctx, const char *extentname,
                              const char *name, char **value)
{
    char *tmp_name;
    char *buff;
    int rc;

    ENTRY;

    if (value == NULL)
        return -EINVAL;

    if (name == NULL || name[0] == '\0')
        return -EINVAL;

    tmp_name = full_xattr_name(name);
    if (tmp_name == NULL)
        return -ENOMEM;

    buff = calloc(1, ATTR_MAX_VALUELEN);
    if (buff == NULL)
        GOTO(free_tmp, rc = -ENOMEM);

    rc = rados_getxattr(pool_io_ctx, extentname, tmp_name, buff,
                        ATTR_MAX_VALUELEN);
    if (rc < 0) {
        free(buff);
        LOG_GOTO(free_tmp, rc = -errno, "getxattr failed");
    }

    if (rc == 0) {
        free(buff);
        *value = NULL;
    } else {
        *value = buff;
    }

    rc = 0;

free_tmp:
    free(tmp_name);
    return rc;
}

struct md_iter {
    rados_ioctx_t pool_io_ctx;
    const char *extentname;
    int flags;
    struct pho_attrs *attrs;
};

static int setxattr_cb(const char *key, const char *value, void *udata)
{
    struct md_iter *arg = (struct md_iter *)udata;

    return pho_rados_setxattr(arg->pool_io_ctx, arg->extentname, key,
                              value, arg->flags);
}

static int pho_rados_md_set(struct pho_rados_io_ctx *rados_io_ctx,
                            struct pho_buff extent_addr,
                            const struct pho_attrs *attrs,
                            enum pho_io_flags flags)
{
    struct md_iter args;

    ENTRY;

    args.pool_io_ctx = rados_io_ctx->pool_io_ctx;
    args.extentname = extent_addr.buff;
    args.flags = (flags & PHO_IO_REPLACE) ? XATTR_REPLACE : XATTR_CREATE;
    args.extentname = extent_addr.buff;

    return pho_attrs_foreach(attrs, setxattr_cb, &args);
}

static int getxattr_cb(const char *key, const char *value, void *udata)
{
    struct md_iter *arg = (struct md_iter *)udata;
    char *tmp_val = NULL;
    int rc;

    rc = pho_rados_getxattr(arg->pool_io_ctx, arg->extentname, key,
                            &tmp_val);
    if (rc != 0)
        return rc;

    rc = pho_attr_set(arg->attrs, key, tmp_val);
    if (rc != 0)
        return rc;

    free(tmp_val);

    return 0;
}

static int pho_rados_md_get(struct pho_rados_io_ctx *rados_io_ctx,
                            struct pho_buff extent_addr,
                            struct pho_attrs *attrs)
{
    struct md_iter args;
    int rc;

    ENTRY;

    args.pool_io_ctx = rados_io_ctx->pool_io_ctx;
    args.extentname = extent_addr.buff;
    args.attrs = attrs;

    rc = pho_attrs_foreach(attrs, getxattr_cb, &args);
    if (rc != 0)
        pho_attrs_free(attrs);
    return rc;
}

static int pho_rados_close(struct pho_io_descr *iod)
{
    struct pho_rados_io_ctx *rados_io_ctx = iod->iod_ctx;
    int rc = 0;

    if (!iod->iod_ctx)
        return 0;

    rados_ioctx_destroy(rados_io_ctx->pool_io_ctx);
    rados_io_ctx->pool_io_ctx = NULL;

    rc = ldm_lib_close(&rados_io_ctx->lib_hdl);
    if (rc)
        LOG_GOTO(out, rc, "Closing RADOS library failed");

    rados_io_ctx->lib_hdl.ld_module = NULL;

out:
    free(rados_io_ctx);
    iod->iod_ctx = NULL;
    return rc;
}

static int pho_rados_open_put(struct pho_io_descr *iod)
{
    struct pho_rados_io_ctx *rados_io_ctx = iod->iod_ctx;
    char *extent_name;
    char buffer;
    int rc;

    rc = pho_rados_md_set(rados_io_ctx, iod->iod_loc->extent->address,
                          &iod->iod_attrs, iod->iod_flags);

    if (rc != 0 || iod->iod_flags & PHO_IO_MD_ONLY)
        goto free_io_ctx;

    extent_name = iod->iod_loc->extent->address.buff;

    /* The function rados_read is called to check if the extent already exists
     * in RADOS when PHO_IO_REPLACE flag is not set.
     */
    if (!(iod->iod_flags & PHO_IO_REPLACE) &&
        rados_read(rados_io_ctx->pool_io_ctx, extent_name, &buffer,
                   1, 0) >= 0) {
        LOG_GOTO(free_io_ctx, rc = -EEXIST,
                 "Object '%s' already exists in pool '%s' but 'replace' "
                 "flag is not set",
                 extent_name, iod->iod_loc->extent->media.name);
    }

    return 0;

free_io_ctx:
    pho_rados_close(iod);
    return rc;
}

static int pho_rados_open_get(struct pho_io_descr *iod)
{
    int rc;

    /* get entry MD, if requested */
    rc = pho_rados_md_get(iod->iod_ctx, iod->iod_loc->extent->address,
                          &iod->iod_attrs);
    if (rc != 0 || (iod->iod_flags & PHO_IO_MD_ONLY))
        goto free_io_ctx;
    return 0;

free_io_ctx:
    pho_rados_close(iod);
    return rc;
}

static int pho_rados_open(const char *extent_key, const char *extent_desc,
                          struct pho_io_descr *iod, bool is_put)
{
    struct pho_rados_io_ctx *rados_io_ctx;
    rados_t cluster_hdl;
    int rc2 = 0;
    int rc = 0;

    ENTRY;

    /* generate entry address, if it is not already set */
    if (!is_ext_addr_set(iod->iod_loc)) {
        if (!is_put)
            LOG_RETURN(-EINVAL, "Object has no address stored in database");

        rc = pho_posix_set_addr(extent_key, extent_desc,
                                iod->iod_loc->addr_type,
                                &iod->iod_loc->extent->address);
        if (rc)
            return rc;
    }
    /* allocate io_ctx */
    rados_io_ctx = alloc_pho_rados_io_ctx();
    if (!rados_io_ctx)
        return -ENOMEM;

    iod->iod_ctx = rados_io_ctx;

    /* Connect to cluster */
    rc = get_lib_adapter(PHO_LIB_RADOS, &rados_io_ctx->lib_hdl.ld_module);
    if (rc)
        LOG_GOTO(out, rc, "Could not get RADOS library adapter");

    rc = ldm_lib_open(&rados_io_ctx->lib_hdl, "");
    if (rc)
        LOG_GOTO(out, rc, "Could not connect to Ceph cluster");


    cluster_hdl = rados_io_ctx->lib_hdl.lh_lib;

    /* Connect to pool */
    rc = rados_ioctx_create(cluster_hdl, iod->iod_loc->extent->media.name,
                            &rados_io_ctx->pool_io_ctx);
    if (rc)
        LOG_GOTO(out, rc, "Could not create the pool's I/O context");

    return is_put ? pho_rados_open_put(iod) : pho_rados_open_get(iod);

out:
    rc2 = ldm_lib_close(&rados_io_ctx->lib_hdl);
    if (rc2)
        pho_error(rc2, "Closing RADOS library failed");

    return rc;
}

static int pho_rados_write(struct pho_io_descr *iod, const void *buf,
                           size_t count)
{
    return -ENOTSUP;
}

static int pho_rados_get(const char *extent_key, const char *extent_desc,
                         struct pho_io_descr *iod)
{
    return -ENOTSUP;
}

static int pho_rados_del(struct pho_ext_loc *loc)
{
    return -ENOTSUP;
}

/** RADOS adapter */
static const struct pho_io_adapter_module_ops IO_ADAPTER_RADOS_OPS = {
    .ioa_get            = pho_rados_get,
    .ioa_del            = pho_rados_del,
    .ioa_open           = pho_rados_open,
    .ioa_write          = pho_rados_write,
    .ioa_close          = pho_rados_close,
    .ioa_medium_sync    = NULL,
    .ioa_preferred_io_size = NULL,
};

/** IO adapter module registration entry point */
int pho_module_register(void *module)
{
    struct io_adapter_module *self = (struct io_adapter_module *) module;

    self->desc = IO_ADAPTER_RADOS_MODULE_DESC;
    self->ops = &IO_ADAPTER_RADOS_OPS;

    return 0;
}
