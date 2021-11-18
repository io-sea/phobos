/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=4:tabstop=4:
 */
/*
 *  All rights reserved (c) 2014-2019 CEA/DAM.
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
 * \brief  Phobos Local Resource Scheduler (LRS)
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "lrs_cfg.h"
#include "lrs_sched.h"
#include "pho_common.h"
#include "pho_dss.h"
#include "pho_io.h"
#include "pho_ldm.h"
#include "pho_type_utils.h"

#include <assert.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include <jansson.h>

#define TAPE_TYPE_SECTION_CFG "tape_type \"%s\""
#define MODELS_CFG_PARAM "models"
#define DRIVE_RW_CFG_PARAM "drive_rw"
#define DRIVE_TYPE_SECTION_CFG "drive_type \"%s\""

static int sched_handle_release_reqs(struct lrs_sched *sched);
static void sched_req_free_wrapper(void *req);
static void sched_resp_free_wrapper(void *resp);

enum sched_operation {
    LRS_OP_NONE = 0,
    LRS_OP_READ,
    LRS_OP_WRITE,
    LRS_OP_FORMAT,
};

/**
 * Build a mount path for the given identifier.
 * @param[in] id    Unique drive identified on the host.
 * The result must be released by the caller using free(3).
 */
static char *mount_point(const char *id)
{
    const char  *mnt_cfg;
    char        *mnt_out;

    mnt_cfg = PHO_CFG_GET(cfg_lrs, PHO_CFG_LRS, mount_prefix);
    if (mnt_cfg == NULL)
        return NULL;

    /* mount the device as PHO_MNT_PREFIX<id> */
    if (asprintf(&mnt_out, "%s%s", mnt_cfg, id) < 0)
        return NULL;

    return mnt_out;
}

/** all needed information to select devices */
struct dev_descr {
    struct dev_info     *dss_dev_info;          /**< device info from DSS */
    struct lib_drv_info  lib_dev_info;          /**< device info from library
                                                  *  (for tape drives)
                                                  */
    struct ldm_dev_state sys_dev_state;         /**< device info from system */

    enum dev_op_status   op_status;             /**< operational status of
                                                  *  the device
                                                  */
    char                 dev_path[PATH_MAX];    /**< path to the device */
    struct media_info   *dss_media_info;        /**< loaded media info
                                                  *  from DSS, if any
                                                  */
    char                 mnt_path[PATH_MAX];    /**< mount path
                                                  *  of the filesystem
                                                  */
    bool                 ongoing_io;            /**< one I/O is ongoing */
    bool                 to_sync;               /**< device need to be synced */
    struct {
        GQueue          *release_queue;         /**< queue for release requests
                                                  *  with to_sync to do
                                                  */
        struct timespec  oldest_to_sync;        /**< oldest release request in
                                                  *  \p release_queue
                                                  */
        size_t           to_sync_size;          /**< total size of release
                                                  *  requests in
                                                  *  \p release_queue
                                                  */
    } sync_params;                              /**< sync information on the
                                                  *  mounted medium
                                                  */
};

/* Needed local function declarations */
static struct dev_descr *search_loaded_media(struct lrs_sched *sched,
                                             const char *name);

/** check that device info from DB is consistent with actual status */
static int check_dev_info(const struct dev_descr *dev)
{
    ENTRY;

    if (dev->dss_dev_info->rsc.model == NULL
        || dev->sys_dev_state.lds_model == NULL) {
        if (dev->dss_dev_info->rsc.model != dev->sys_dev_state.lds_model)
            LOG_RETURN(-EINVAL, "%s: missing or unexpected device model",
                       dev->dev_path);
        else
            pho_debug("%s: no device model is set", dev->dev_path);

    } else if (cmp_trimmed_strings(dev->dss_dev_info->rsc.model,
                                   dev->sys_dev_state.lds_model)) {
        LOG_RETURN(-EINVAL, "%s: configured device model '%s' differs from "
                   "actual device model '%s'", dev->dev_path,
                   dev->dss_dev_info->rsc.model, dev->sys_dev_state.lds_model);
    }

    if (dev->sys_dev_state.lds_serial == NULL) {
        if (dev->dss_dev_info->rsc.id.name != dev->sys_dev_state.lds_serial)
            LOG_RETURN(-EINVAL, "%s: missing or unexpected device serial",
                       dev->dev_path);
        else
            pho_debug("%s: no device serial is set", dev->dev_path);
    } else if (strcmp(dev->dss_dev_info->rsc.id.name,
                      dev->sys_dev_state.lds_serial) != 0) {
        LOG_RETURN(-EINVAL, "%s: configured device serial '%s' differs from "
                   "actual device serial '%s'", dev->dev_path,
                   dev->dss_dev_info->rsc.id.name,
                   dev->sys_dev_state.lds_serial);
    }

    return 0;
}

/**
 * Unlock a resource device at DSS level and clean the corresponding lock
 *
 * @param[in]   sched   current scheduler
 * @param[in]   type    DSS type of the resource to release
 * @param[in]   item    Resource to release
 * @param[out]  lock    lock to clean
 */
static int sched_resource_release(struct lrs_sched *sched, enum dss_type type,
                                  void *item, struct pho_lock *lock)
{
    int rc;

    ENTRY;

    rc = dss_unlock(&sched->dss, type, item, 1, false);
    if (rc)
        LOG_RETURN(rc, "Cannot unlock a resource");

    pho_lock_clean(lock);
    return 0;
}

static int sched_device_release(struct lrs_sched *sched, struct dev_descr *dev)
{
    int rc;

    rc = sched_resource_release(sched, DSS_DEVICE, dev->dss_dev_info,
                                &dev->dss_dev_info->lock);
    if (rc)
        pho_error(rc,
                  "Error when releasing device '%s' with current lock "
                  "(hostname %s, owner %d)", dev->dev_path,
                  dev->dss_dev_info->lock.hostname,
                  dev->dss_dev_info->lock.owner);

    return rc;
}

static int sched_medium_release(struct lrs_sched *sched,
                                struct media_info *medium)
{
    int rc;

    rc = sched_resource_release(sched, DSS_MEDIA, medium, &medium->lock);
    if (rc)
        pho_error(rc,
                  "Error when releasing medium '%s' with current lock "
                  "(hostname %s, owner %d)", medium->rsc.id.name,
                  medium->lock.hostname, medium->lock.owner);

    return rc;
}

/**
 * Lock the corresponding item into the global DSS and update the local lock
 *
 * @param[in]       dss     DSS handle
 * @param[in]       type    DSS type of the item
 * @param[in]       item    item to lock
 * @param[in, out]  lock    already allocated lock to update
 */
static int take_and_update_lock(struct dss_handle *dss, enum dss_type type,
                                void *item, struct pho_lock *lock)
{
    int rc2;
    int rc;

    pho_lock_clean(lock);
    rc = dss_lock(dss, type, item, 1);
    if (rc)
        pho_error(rc, "Unable to get lock on item for refresh");

    /* update lock values */
    rc2 = dss_lock_status(dss, type, item, 1, lock);
    if (rc2) {
        pho_error(rc2, "Unable to get status of new lock while refreshing");
        /* try to unlock before exiting */
        if (rc == 0) {
            dss_unlock(dss, type, item, 1, false);
            rc = rc2;
        }

        /* put a wrong lock value */
        init_pho_lock(lock, "error_on_hostname", 0, NULL);
    }

    return rc;
}

/**
 * If lock->owner is different from sched->lock_owner, renew the lock with
 * the current owner (PID).
 */
static int check_renew_owner(struct lrs_sched *sched, enum dss_type type,
                             void *item, struct pho_lock *lock)
{
    int rc;

    if (lock->owner != sched->lock_owner) {
        pho_warn("'%s' is already locked by owner %d, owner %d will "
                 "take ownership of this device",
                 dss_type_names[type], lock->owner, sched->lock_owner);

        /**
         * Unlocking here is dangerous if there is another process than the
         * LRS on the same node that also acquires locks. If it becomes the case
         * we have to warn and return an error and we must not take the
         * ownership of this resource again.
         */
        /* unlock previous owner */
        rc = dss_unlock(&sched->dss, type, item, 1, true);
        if (rc)
            LOG_RETURN(rc,
                       "Unable to clear previous lock (hostname: %s, owner "
                       " %d) on item",
                       lock->hostname, lock->owner);

        /* get the lock again */
        rc = take_and_update_lock(&sched->dss, type, item, lock);
        if (rc)
            LOG_RETURN(rc, "Unable to get and refresh lock");
    }

    return 0;
}

/**
 * First, check that lock->hostname is the same as sched->lock_hostname. If not,
 * -EALREADY is returned.
 *
 * Then, if lock->owner is different from sched->lock_owner, renew the lock with
 * the current owner (PID) by calling check_renew_owner.
 */
static int check_renew_lock(struct lrs_sched *sched, enum dss_type type,
                            void *item, struct pho_lock *lock)
{
    if (strcmp(lock->hostname, sched->lock_hostname)) {
        pho_warn("Resource already locked by host %s instead of %s",
                 lock->hostname, sched->lock_hostname);
        return -EALREADY;
    }

    return check_renew_owner(sched, type, item, lock);
}

/**
 * Acquire device lock if it is not already set.
 *
 * If lock is already set, check hostname and owner.
 * -EALREADY is returned if dev->lock.hostname is not the same as
 *  sched->lock_hostname.
 *  If dev->lock.owner is not the same as sched->lock_owner, the lock is
 *  re-taken from DSS to update the owner.
 */
static int check_and_take_device_lock(struct lrs_sched *sched,
                                      struct dev_info *dev)
{
    int rc;

    if (dev->lock.hostname) {
        rc = check_renew_lock(sched, DSS_DEVICE, dev, &dev->lock);
        if (rc)
            LOG_RETURN(rc,
                       "Unable to check and renew lock of one of our devices "
                       "'%s'", dev->rsc.id.name);
    } else {
        rc = take_and_update_lock(&sched->dss, DSS_DEVICE, dev, &dev->lock);
        if (rc)
            LOG_RETURN(rc,
                       "Unable to acquire and update lock on device '%s'",
                       dev->rsc.id.name);
    }

    return 0;
}

/**
 * Retrieve media info from DSS for the given ID.
 * @param pmedia[out] returned pointer to a media_info structure
 *                    allocated by this function.
 * @param id[in]      ID of the media.
 */
static int sched_fill_media_info(struct lrs_sched *sched,
                                 struct media_info **pmedia,
                                 const struct pho_id *id)
{
    struct dss_handle   *dss = &sched->dss;
    struct media_info   *media_res = NULL;
    struct dss_filter    filter;
    int                  mcnt = 0;
    int                  rc;

    if (id == NULL || pmedia == NULL)
        return -EINVAL;

    pho_debug("Retrieving media info for %s '%s'",
              rsc_family2str(id->family), id->name);

    rc = dss_filter_build(&filter,
                          "{\"$AND\": ["
                          "  {\"DSS::MDA::family\": \"%s\"},"
                          "  {\"DSS::MDA::id\": \"%s\"}"
                          "]}", rsc_family2str(id->family), id->name);
    if (rc)
        return rc;

    /* get media info from DB */
    rc = dss_media_get(dss, &filter, &media_res, &mcnt);
    if (rc)
        GOTO(out_nores, rc);

    if (mcnt == 0) {
        pho_info("No media found matching %s '%s'",
                 rsc_family2str(id->family), id->name);
        GOTO(out_free, rc = -ENXIO);
    } else if (mcnt > 1) {
        LOG_GOTO(out_free, rc = -EINVAL,
                 "Too many media found matching id '%s'", id->name);
    }

    media_info_free(*pmedia);
    *pmedia = media_info_dup(media_res);
    if (!*pmedia)
        LOG_GOTO(out_free, rc = -ENOMEM, "Couldn't duplicate media info");

    if ((*pmedia)->lock.hostname != NULL) {
        rc = check_renew_lock(sched, DSS_MEDIA, *pmedia, &(*pmedia)->lock);
        if (rc == -EALREADY) {
            LOG_GOTO(out_free, rc,
                     "Media '%s' is locked by (hostname: %s, owner: %d)",
                     id->name, (*pmedia)->lock.hostname, (*pmedia)->lock.owner);
        } else if (rc) {
            LOG_GOTO(out_free, rc,
                     "Error while checking media '%s' locked with hostname "
                     "'%s' and owner '%d'",
                     id->name, (*pmedia)->lock.hostname, (*pmedia)->lock.owner);
        }
    }

    pho_debug("%s: spc_free=%zd",
              (*pmedia)->rsc.id.name, (*pmedia)->stats.phys_spc_free);

    rc = 0;

out_free:
    dss_res_free(media_res, mcnt);
out_nores:
    dss_filter_free(&filter);
    return rc;
}

/**
 * Retrieve device information from system and complementary info from DB.
 * - check DB device info is consistent with mtx output.
 * - get operationnal status from system (loaded or not).
 * - for loaded drives, the mounted volume + LTFS mount point, if mounted.
 * - get media information from DB for loaded drives.
 *
 * @param[in]  dss  handle to dss connection.
 * @param[in]  lib  library handler for tape devices.
 * @param[out] devd dev_descr structure filled with all needed information.
 */
static int sched_fill_dev_info(struct lrs_sched *sched, struct lib_adapter *lib,
                               struct dev_descr *devd)
{
    struct dev_adapter deva;
    struct dev_info   *devi;
    int                rc;

    ENTRY;

    if (devd == NULL)
        return -EINVAL;

    devi = devd->dss_dev_info;

    media_info_free(devd->dss_media_info);
    devd->dss_media_info = NULL;

    rc = get_dev_adapter(devi->rsc.id.family, &deva);
    if (rc)
        return rc;

    /* get path for the given serial */
    rc = ldm_dev_lookup(&deva, devi->rsc.id.name, devd->dev_path,
                        sizeof(devd->dev_path));
    if (rc) {
        pho_debug("Device lookup failed: serial '%s'", devi->rsc.id.name);
        return rc;
    }

    /* now query device by path */
    ldm_dev_state_fini(&devd->sys_dev_state);
    rc = ldm_dev_query(&deva, devd->dev_path, &devd->sys_dev_state);
    if (rc) {
        pho_debug("Failed to query device '%s'", devd->dev_path);
        return rc;
    }

    /* compare returned device info with info from DB */
    rc = check_dev_info(devd);
    if (rc)
        return rc;

    /* Query the library about the drive location and whether it contains
     * a media.
     */
    rc = ldm_lib_drive_lookup(lib, devi->rsc.id.name, &devd->lib_dev_info);
    if (rc) {
        pho_debug("Failed to query the library about device '%s'",
                  devi->rsc.id.name);
        return rc;
    }

    if (devd->lib_dev_info.ldi_full) {
        struct pho_id *medium_id;
        struct fs_adapter fsa;

        devd->op_status = PHO_DEV_OP_ST_LOADED;
        medium_id = &devd->lib_dev_info.ldi_medium_id;

        pho_debug("Device '%s' (S/N '%s') contains medium '%s'", devd->dev_path,
                  devi->rsc.id.name, medium_id->name);

        /* get media info for loaded drives */
        rc = sched_fill_media_info(sched, &devd->dss_media_info, medium_id);

        if (rc) {
            if (rc == -ENXIO)
                pho_error(rc,
                          "Device '%s' (S/N '%s') contains medium '%s', but "
                          "this medium cannot be found", devd->dev_path,
                          devi->rsc.id.name, medium_id->name);

            if (rc == -EALREADY)
                pho_error(rc,
                          "Device '%s' (S/N '%s') is owned by host %s but "
                          "contains medium '%s' which is locked by an other "
                          "hostname %s", devd->dev_path, devi->rsc.id.name,
                           devi->host, medium_id->name,
                           devd->dss_media_info->lock.hostname);

            return rc;
        }

        /* get lock for loaded media */
        if (!devd->dss_media_info->lock.hostname) {
            rc = take_and_update_lock(&sched->dss, DSS_MEDIA,
                                      devd->dss_media_info,
                                      &devd->dss_media_info->lock);
            if (rc)
                LOG_RETURN(rc,
                           "Unable to lock the media '%s' loaded in a owned "
                           "device '%s'", devd->dss_media_info->rsc.id.name,
                           devd->dev_path);
        }

        /* See if the device is currently mounted */
        rc = get_fs_adapter(devd->dss_media_info->fs.type, &fsa);
        if (rc)
            return rc;

        /* If device is loaded, check if it is mounted as a filesystem */
        rc = ldm_fs_mounted(&fsa, devd->dev_path, devd->mnt_path,
                            sizeof(devd->mnt_path));

        if (rc == 0) {
            pho_debug("Discovered mounted filesystem at '%s'",
                      devd->mnt_path);
            devd->op_status = PHO_DEV_OP_ST_MOUNTED;
        } else if (rc == -ENOENT) {
            /* not mounted, not an error */
            rc = 0;
        } else {
            LOG_RETURN(rc, "Cannot determine if device '%s' is mounted",
                       devd->dev_path);
        }
    } else {
        devd->op_status = PHO_DEV_OP_ST_EMPTY;
    }

    pho_debug("Drive '%s' is '%s'", devd->dev_path,
              op_status2str(devd->op_status));

    return rc;
}

/** Wrap library open operations
 * @param[out] lib  Library handler.
 */
static int wrap_lib_open(enum rsc_family dev_type, struct lib_adapter *lib)
{
    const char *lib_dev;
    int         rc;

    /* non-tape cases: dummy lib adapter (no open required) */
    if (dev_type != PHO_RSC_TAPE)
        return get_lib_adapter(PHO_LIB_DUMMY, lib);

    /* tape case */
    rc = get_lib_adapter(PHO_LIB_SCSI, lib);
    if (rc)
        LOG_RETURN(rc, "Failed to get library adapter");

    /* For now, one single configurable path to library device.
     * This will have to be changed to manage multiple libraries.
     */
    lib_dev = PHO_CFG_GET(cfg_lrs, PHO_CFG_LRS, lib_device);
    if (!lib_dev)
        LOG_RETURN(rc, "Failed to get default library device from config");

    return ldm_lib_open(lib, lib_dev);
}

static int load_device_list_from_dss(struct lrs_sched *sched)
{
    struct dev_info *devs = NULL;
    struct dss_filter filter;
    int dcnt;
    int rc;
    int i;

    rc = dss_filter_build(&filter,
                          "{\"$AND\": ["
                          "  {\"DSS::DEV::host\": \"%s\"},"
                          "  {\"DSS::DEV::adm_status\": \"%s\"},"
                          "  {\"DSS::DEV::family\": \"%s\"}"
                          "]}",
                          sched->lock_hostname,
                          rsc_adm_status2str(PHO_RSC_ADM_ST_UNLOCKED),
                          rsc_family2str(sched->family));
    if (rc)
        return rc;

    /* get all admin unlocked devices from DB for the given family */
    rc = dss_device_get(&sched->dss, &filter, &devs, &dcnt);
    dss_filter_free(&filter);
    if (rc)
        LOG_RETURN(rc, "Error when getting devices from DSS");

    /* Copy information from DSS to local device list */
    for (i = 0 ; i < dcnt; i++) {
        struct dev_descr device = {0};

        if (check_and_take_device_lock(sched, &devs[i]))
            continue;

        device.dss_dev_info = dev_info_dup(&devs[i]);
        if (!device.dss_dev_info) {
            pho_warn("Unable to dup dev_info of '%s'", devs[i].path);
            continue;
        }

        g_array_append_val(sched->devices, device);
    }

    if (sched->devices->len == 0)
        LOG_GOTO(clean, rc = -ENXIO,
                 "No usable device found (%s): check devices status",
                 rsc_family2str(sched->family));

clean:
    /* free devs array, as they have been copied to sched->devices */
    dss_res_free(devs, dcnt);
    return rc;
}

/**
 * Load device states into memory.
 * Do nothing if device status is already loaded.
 */
static int sched_load_dev_state(struct lrs_sched *sched)
{
    bool                clean_devices = false;
    struct lib_adapter  lib;
    int                 rc;
    int                 i;

    ENTRY;

    if (sched->devices->len == 0)
        LOG_RETURN(-ENXIO, "Try to load state of an empty list of devices");

    /* get a handle to the library to query it */
    rc = wrap_lib_open(sched->family, &lib);
    if (rc)
        LOG_RETURN(rc, "Error while loading devices when opening library");

    for (i = 0 ; i < sched->devices->len; i++) {
        struct dev_descr *dev = &g_array_index(sched->devices,
                                               struct dev_descr, i);
        rc = sched_fill_dev_info(sched, &lib, dev);
        if (rc) {
            pho_debug("Fail to init device '%s', marking it as failed and "
                      "releasing it", dev->dev_path);
            dev->op_status = PHO_DEV_OP_ST_FAILED;
            sched_device_release(sched, dev);
        } else {
            clean_devices = true;
        }
    }

    /* close handle to the library */
    rc = ldm_lib_close(&lib);
    if (rc)
        LOG_RETURN(rc,
                   "Error while closing the library handle after loading "
                   "device state");

    if (!clean_devices)
        LOG_RETURN(-ENXIO, "No functional device found");

    return 0;
}

static void dev_descr_fini(gpointer ptr)
{
    struct dev_descr *dev = (struct dev_descr *)ptr;
    dev_info_free(dev->dss_dev_info, true);
    dev->dss_dev_info = NULL;

    media_info_free(dev->dss_media_info);
    dev->dss_media_info = NULL;

    ldm_dev_state_fini(&dev->sys_dev_state);
}

/**
 * Unlocks all devices that were locked by a previous instance on this host and
 * that it doesn't own anymore.
 *
 * @param   sched       Scheduler handle.
 * @return              0 on success,
 *                      first encountered negative posix error on failure.
 */
static int sched_clean_device_locks(struct lrs_sched *sched)
{
    int rc;

    ENTRY;

    rc = dss_lock_device_clean(&sched->dss, rsc_family_names[sched->family],
                               sched->lock_hostname, sched->lock_owner);
    if (rc)
        pho_error(rc, "Failed to clean device locks");

    return rc;
}

/**
 * Unlocks all media that were locked by a previous instance on this host and
 * that are not loaded anymore in a device locked by this host.
 *
 * @param   sched       Scheduler handle.
 * @return              0 on success,
 *                      first encountered negative posix error on failure.
 */
static int sched_clean_medium_locks(struct lrs_sched *sched)
{
    struct media_info *media = NULL;
    int cnt = 0;
    int rc;
    int i;

    ENTRY;

    media = malloc(sched->devices->len * sizeof(*media));
    if (!media)
        LOG_RETURN(-errno, "Failed to allocate media list");

    for (i = 0; i < sched->devices->len; i++) {
        struct media_info *mda = g_array_index(sched->devices,
                                               struct dev_descr,
                                               i).dss_media_info;
        if (mda) {
            media[i] = *mda;
            cnt++;
        }
    }

    rc = dss_lock_media_clean(&sched->dss, media, cnt,
                              sched->lock_hostname, sched->lock_owner);
    if (rc)
        pho_error(rc, "Failed to clean media locks");

    free(media);
    return rc;
}

int sched_init(struct lrs_sched *sched, enum rsc_family family)
{
    int rc;

    sched->family = family;

    rc = fill_host_owner(&sched->lock_hostname, &sched->lock_owner);
    if (rc)
        LOG_RETURN(rc, "Failed to get hostname and PID");

    /* Connect to the DSS */
    rc = dss_init(&sched->dss);
    if (rc)
        return rc;

    sched->devices = g_array_new(FALSE, TRUE, sizeof(struct dev_descr));
    g_array_set_clear_func(sched->devices, dev_descr_fini);
    sched->req_queue = g_queue_new();
    sched->release_queue = g_queue_new();
    sched->response_queue = g_queue_new();

    /* Load devices from DSS -- not critical if no device is found */
    load_device_list_from_dss(sched);

    /* Load the device state -- not critical if no device is found */
    sched_load_dev_state(sched);

    rc = sched_clean_device_locks(sched);
    if (rc) {
        sched_fini(sched);
        return rc;
    }

    rc = sched_clean_medium_locks(sched);
    if (rc) {
        sched_fini(sched);
        return rc;
    }

    return 0;
}

/**
 * Unmount the filesystem of a 'mounted' device
 *
 * sched_umount must be called with:
 * - dev->op_status set to PHO_DEV_OP_ST_MOUNTED, a mounted dev->dess_media_info
 * - a global DSS lock on dev
 * - a global DSS lock on dev->dss_media_info
 *
 * On error, dev->ongoing_io is set to false, we try to release global DSS
 * locks on dev and dev->dss_media_info, and dev->op_status is set to
 * PHO_DEV_OP_FAILED.
 */
static int sched_umount(struct lrs_sched *sched, struct dev_descr *dev)
{
    struct fs_adapter fsa;
    int rc;

    ENTRY;

    pho_verb("Unmounting device '%s' mounted at '%s'",
             dev->dev_path, dev->mnt_path);

    rc = get_fs_adapter(dev->dss_media_info->fs.type, &fsa);
    if (rc)
        LOG_GOTO(out, rc,
                 "Unable to get fs adapter '%s' to unmount medium '%s' from "
                 "device '%s'", fs_type_names[dev->dss_media_info->fs.type],
                 dev->dss_media_info->rsc.id.name, dev->dev_path);

    rc = ldm_fs_umount(&fsa, dev->dev_path, dev->mnt_path);
    if (rc)
        LOG_GOTO(out, rc, "Failed to unmount device '%s' mounted at '%s'",
                 dev->dev_path, dev->mnt_path);

    /* update device state and unset mount path */
    dev->op_status = PHO_DEV_OP_ST_LOADED;
    dev->mnt_path[0] = '\0';

out:
    if (rc) {
        dev->op_status = PHO_DEV_OP_ST_FAILED;
        dev->ongoing_io = false;
        sched_medium_release(sched, dev->dss_media_info);
        sched_device_release(sched, dev);
    }

    return rc;
}

/**
 * Unload, unlock and free a media from a drive and set drive's op_status to
 * PHO_DEV_OP_ST_EMPTY
 *
 * Must be called with:
 * - dev->op_status set to PHO_DEV_OP_ST_LOADED and loaded dev->dss_media_info
 * - a global DSS lock on dev
 * - a global DSS lock on dev->dss_media_info
 *
 * On error, we try to release global DSS lock on dev in addition of unlocking
 * dev->media. dev->op_status is set to PHO_DEV_OP_ST_FAILED
 */
static int sched_unload_medium(struct lrs_sched *sched, struct dev_descr *dev)
{
    /* let the library select the target location */
    struct lib_item_addr    free_slot = { .lia_type = MED_LOC_UNKNOWN };
    struct lib_adapter      lib;
    int                     rc2;
    int                     rc;

    ENTRY;

    pho_verb("Unloading '%s' from '%s'", dev->dss_media_info->rsc.id.name,
             dev->dev_path);

    rc = wrap_lib_open(dev->dss_dev_info->rsc.id.family, &lib);
    if (rc)
        LOG_GOTO(out, rc,
                 "Unable to open lib '%s' to unload medium '%s' from device "
                 "'%s'", rsc_family_names[dev->dss_dev_info->rsc.id.family],
                 dev->dss_media_info->rsc.id.name, dev->dev_path);

    rc = ldm_lib_media_move(&lib, &dev->lib_dev_info.ldi_addr, &free_slot);
    if (rc != 0)
        /* Set operationnal failure state on this drive. It is incomplete since
         * the error can originate from a defective tape too...
         *  - consider marking both as failed.
         *  - consider maintaining lists of errors to diagnose and decide who to
         *    exclude from the cool game.
         */
        LOG_GOTO(out_close, rc, "Media move failed");

    dev->op_status = PHO_DEV_OP_ST_EMPTY;

out_close:
    rc2 = ldm_lib_close(&lib);
    if (rc2)
        rc = rc ? : rc2;

out:
    rc2 = sched_medium_release(sched, dev->dss_media_info);
    if (rc2)
        rc = rc ? : rc2;

    media_info_free(dev->dss_media_info);
    dev->dss_media_info = NULL;

    if (rc) {
        dev->op_status = PHO_DEV_OP_ST_FAILED;
        sched_device_release(sched, dev);
    }

    return rc;
}

static int sched_empty_dev(struct lrs_sched *sched, struct dev_descr *dev)
{
    int rc;

    if (dev->op_status == PHO_DEV_OP_ST_MOUNTED) {
        rc = sched_umount(sched, dev);
        if (rc)
            return rc;
    }

    /**
     * We follow up on unload.
     * (a successfull umount let the op_status to LOADED)
     */
    if (dev->op_status == PHO_DEV_OP_ST_LOADED)
        return sched_unload_medium(sched, dev);

    return 0;
}

/**
 * If the device contains a medium, this one is unmounted if needed and
 * unloaded, and the global DSS lock on this medium is released.
 *
 * The global DSS lock of the device is released.
 */
static int sched_empty_and_release_dev(struct lrs_sched *sched,
                                       struct dev_descr *dev)
{
    int rc;

    rc = sched_empty_dev(sched, dev);
    if (rc)
        return rc;

    return sched_device_release(sched, dev);
}

/**
 * Unmount, unload and release global DSS lock of all medium that are loaded
 * into devices with no ongoing I/O and that are not failed. The global DSS
 * locks on devices with no ongoing I/O and that are not failed are released.
 */
static void sched_release(struct lrs_sched *sched)
{
    int i;

    for (i = 0; i < sched->devices->len; i++) {
        struct dev_descr *dev = &g_array_index(sched->devices,
                                               struct dev_descr, i);

        if (dev->op_status != PHO_DEV_OP_ST_FAILED && !dev->ongoing_io)
            sched_empty_and_release_dev(sched, dev);
    }
}

void sched_fini(struct lrs_sched *sched)
{
    if (sched == NULL)
        return;

    /* Handle all pending release requests */
    sched_handle_release_reqs(sched);

    /* Release all devices and media without any ongoing IO */
    sched_release(sched);

    dss_fini(&sched->dss);

    g_queue_free_full(sched->req_queue, sched_req_free_wrapper);
    g_queue_free_full(sched->release_queue, sched_req_free_wrapper);
    g_queue_free_full(sched->response_queue, sched_resp_free_wrapper);
    g_array_free(sched->devices, TRUE);
}

/**
 * Build a filter string fragment to filter on a given tag set. The returned
 * string is allocated with malloc. NULL is returned when ENOMEM is encountered.
 *
 * The returned string looks like the following:
 * {"$AND": [{"DSS:MDA::tags": "tag1"}]}
 */
static char *build_tag_filter(const struct tags *tags)
{
    json_t *and_filter = NULL;
    json_t *tag_filters = NULL;
    char   *tag_filter_json = NULL;
    size_t  i;

    /* Build a json array to properly format tag related DSS filter */
    tag_filters = json_array();
    if (!tag_filters)
        return NULL;

    /* Build and append one filter per tag */
    for (i = 0; i < tags->n_tags; i++) {
        json_t *tag_flt;
        json_t *xjson;

        tag_flt = json_object();
        if (!tag_flt)
            GOTO(out, -ENOMEM);

        xjson = json_object();
        if (!xjson) {
            json_decref(tag_flt);
            GOTO(out, -ENOMEM);
        }

        if (json_object_set_new(tag_flt, "DSS::MDA::tags",
                                json_string(tags->tags[i]))) {
            json_decref(tag_flt);
            json_decref(xjson);
            GOTO(out, -ENOMEM);
        }

        if (json_object_set_new(xjson, "$XJSON", tag_flt)) {
            json_decref(xjson);
            GOTO(out, -ENOMEM);
        }

        if (json_array_append_new(tag_filters, xjson))
            GOTO(out, -ENOMEM);
    }

    and_filter = json_object();
    if (!and_filter)
        GOTO(out, -ENOMEM);

    /* Do not use the _new function and decref inconditionnaly later */
    if (json_object_set(and_filter, "$AND", tag_filters))
        GOTO(out, -ENOMEM);

    /* Convert to string for formatting */
    tag_filter_json = json_dumps(tag_filters, 0);

out:
    json_decref(tag_filters);

    /* json_decref(NULL) is safe but not documented */
    if (and_filter)
        json_decref(and_filter);

    return tag_filter_json;
}

static bool medium_in_devices(const struct media_info *medium,
                              struct dev_descr **devs, size_t n_dev)
{
    size_t i;

    for (i = 0; i < n_dev; i++) {
        if (devs[i]->dss_media_info == NULL)
            continue;
        if (pho_id_equal(&medium->rsc.id, &devs[i]->dss_media_info->rsc.id))
            return true;
    }

    return false;
}

/**
 * Get a suitable medium for a write operation.
 *
 * @param[in]  sched         Current scheduler
 * @param[out] p_media       Selected medium
 * @param[in]  required_size Size of the extent to be written.
 * @param[in]  family        Medium family from which getting the medium
 * @param[in]  tags          Tags used to filter candidate media, the
 *                           selected medium must have all the specified tags.
 * @param[in]  devs          Array of selected devices to write with.
 * @param[in]  n_dev         Nb in devs of already allocated devices with loaded
 *                           and mounted media
 */
static int sched_select_media(struct lrs_sched *sched,
                              struct media_info **p_media, size_t required_size,
                              enum rsc_family family, const struct tags *tags,
                              struct dev_descr **devs, size_t n_dev)
{
    struct media_info   *pmedia_res = NULL;
    struct media_info   *split_media_best;
    size_t               avail_size;
    struct media_info   *whole_media_best;
    struct media_info   *chosen_media;
    struct dss_filter    filter;
    char                *tag_filter_json = NULL;
    bool                 with_tags = tags != NULL && tags->n_tags > 0;
    int                  mcnt = 0;
    int                  rc;
    int                  i;

    ENTRY;

    if (with_tags) {
        tag_filter_json = build_tag_filter(tags);
        if (!tag_filter_json)
            LOG_GOTO(err_nores, rc = -ENOMEM, "while building tags dss filter");
    }

    rc = dss_filter_build(&filter,
                          "{\"$AND\": ["
                          /* Basic criteria */
                          "  {\"DSS::MDA::family\": \"%s\"},"
                          /* Check put media operation flags */
                          "  {\"DSS::MDA::put\": \"t\"},"
                          /* Exclude media locked by admin */
                          "  {\"DSS::MDA::adm_status\": \"%s\"},"
                          "  {\"$NOR\": ["
                               /* Exclude non-formatted media */
                          "    {\"DSS::MDA::fs_status\": \"%s\"},"
                               /* Exclude full media */
                          "    {\"DSS::MDA::fs_status\": \"%s\"}"
                          "  ]}"
                          "  %s%s"
                          "]}",
                          rsc_family2str(family),
                          rsc_adm_status2str(PHO_RSC_ADM_ST_UNLOCKED),
                          /**
                           * @TODO add criteria to limit the maximum number of
                           * data fragments:
                           * vol_free >= required_size/max_fragments
                           * with a configurable max_fragments of 4 for example)
                           */
                          fs_status2str(PHO_FS_STATUS_BLANK),
                          fs_status2str(PHO_FS_STATUS_FULL),
                          with_tags ? ", " : "",
                          with_tags ? tag_filter_json : "");

    free(tag_filter_json);
    if (rc)
        return rc;

    rc = dss_media_get(&sched->dss, &filter, &pmedia_res, &mcnt);
    dss_filter_free(&filter);
    if (rc)
        GOTO(err_nores, rc);

lock_race_retry:
    chosen_media = NULL;
    whole_media_best = NULL;
    split_media_best = NULL;
    avail_size = 0;

    /* get the best fit */
    for (i = 0; i < mcnt; i++) {
        struct media_info *curr = &pmedia_res[i];

        /* exclude medium already booked for this allocation */
        if (medium_in_devices(curr, devs, n_dev))
            continue;

        avail_size += curr->stats.phys_spc_free;

        /* already locked */
        if (curr->lock.hostname != NULL) {
            struct dev_descr *dev;

            if (check_renew_lock(sched, DSS_MEDIA, curr, &curr->lock))
                /* not locked by myself */
                continue;

            dev = search_loaded_media(sched, curr->rsc.id.name);
            if (dev && dev->ongoing_io)
                /* locked by myself but already in use */
                continue;
        }

        if (split_media_best == NULL ||
            curr->stats.phys_spc_free > split_media_best->stats.phys_spc_free)
            split_media_best = curr;

        if (curr->stats.phys_spc_free < required_size)
            continue;

        if (whole_media_best == NULL ||
            curr->stats.phys_spc_free < whole_media_best->stats.phys_spc_free)
            whole_media_best = curr;
    }

    if (avail_size < required_size) {
        pho_warn("Available space on media : %zd, required size : %zd",
                 avail_size, required_size);
        GOTO(free_res, rc = -ENOSPC);
    }

    if (whole_media_best != NULL) {
        chosen_media = whole_media_best;
    } else if (split_media_best != NULL) {
        chosen_media = split_media_best;
        pho_info("Split %zd required_size on %zd avail size on %s medium",
                 required_size, chosen_media->stats.phys_spc_free,
                 chosen_media->rsc.id.name);
    } else {
        pho_info("No medium available, wait for one");
        GOTO(free_res, rc = -EAGAIN);
    }

    if (!chosen_media->lock.hostname) {
        pho_debug("Acquiring selected media '%s'", chosen_media->rsc.id.name);
        rc = take_and_update_lock(&sched->dss, DSS_MEDIA, chosen_media,
                                  &chosen_media->lock);
        if (rc) {
            pho_debug("Failed to lock media '%s', looking for another one",
                      chosen_media->rsc.id.name);
            goto lock_race_retry;
        }
    }

    pho_verb("Selected %s '%s': %zd bytes free", rsc_family2str(family),
             chosen_media->rsc.id.name,
             chosen_media->stats.phys_spc_free);

    *p_media = media_info_dup(chosen_media);
    if (*p_media == NULL) {
        sched_medium_release(sched, chosen_media);
        LOG_GOTO(free_res, rc = -ENOMEM,
                 "Unable to duplicate chosen media '%s'",
                 chosen_media->rsc.id.name);
    }

    rc = 0;

free_res:
    dss_res_free(pmedia_res, mcnt);

err_nores:
    return rc;
}

/**
 * Get the value of the configuration parameter that contains
 * the list of drive models for a given drive type.
 * e.g. "LTO6_drive" -> "ULTRIUM-TD6,ULT3580-TD6,..."
 *
 * @return 0 on success, a negative POSIX error code on failure.
 */
static int drive_models_by_type(const char *drive_type, const char **list)
{
    char *section_name;
    int rc;

    /* build drive_type section name */
    rc = asprintf(&section_name, DRIVE_TYPE_SECTION_CFG,
                  drive_type);
    if (rc < 0)
        return -ENOMEM;

    /* get list of drive models */
    rc = pho_cfg_get_val(section_name, MODELS_CFG_PARAM, list);
    if (rc)
        pho_error(rc, "Unable to find parameter "MODELS_CFG_PARAM" in section "
                  "'%s' for drive type '%s'", section_name, drive_type);

    free(section_name);
    return rc;
}

/**
 * Get the value of the configuration parameter that contains
 * the list of write-compatible drives for a given tape model.
 * e.g. "LTO5" -> "LTO5_drive,LTO6_drive"
 *
 * @return 0 on success, a negative POSIX error code on failure.
 */
static int rw_drive_types_for_tape(const char *tape_model, const char **list)
{
    char *section_name;
    int rc;

    /* build tape_type section name */
    rc = asprintf(&section_name, TAPE_TYPE_SECTION_CFG, tape_model);
    if (rc < 0)
        return -ENOMEM;

    /* get list of drive_rw types */
    rc = pho_cfg_get_val(section_name, DRIVE_RW_CFG_PARAM, list);
    if (rc)
        pho_error(rc, "Unable to find parameter "DRIVE_RW_CFG_PARAM
                  " in section '%s' for tape model '%s'",
                  section_name, tape_model);

    free(section_name);
    return rc;
}

/**
 * Search a given item in a coma-separated list.
 *
 * @param[in]  list     Comma-separated list of items.
 * @param[in]  str      Item to find in the list.
 * @param[out] res      true of the string is found, false else.
 *
 * @return 0 on success. A negative POSIX error code on error.
 */
static int search_in_list(const char *list, const char *str, bool *res)
{
    char *parse_list;
    char *item;
    char *saveptr;

    *res = false;

    /* copy input list to parse it */
    parse_list = strdup(list);
    if (parse_list == NULL)
        return -errno;

    /* check if the string is in the list */
    for (item = strtok_r(parse_list, ",", &saveptr);
         item != NULL;
         item = strtok_r(NULL, ",", &saveptr)) {
        if (strcmp(item, str) == 0) {
            *res = true;
            goto out_free;
        }
    }

out_free:
    free(parse_list);
    return 0;
}

/**
 * This function determines if the input drive and tape are compatible.
 *
 * @param[in]  tape  tape to check compatibility
 * @param[in]  drive drive to check compatibility
 * @param[out] res   true if the tape and drive are compatible, else false
 *
 * @return 0 on success, negative error code on failure and res is false
 */
static int tape_drive_compat(const struct media_info *tape,
                             const struct dev_descr *drive, bool *res)
{
    const char *rw_drives;
    char *parse_rw_drives;
    char *drive_type;
    char *saveptr;
    int rc;

    /* false by default */
    *res = false;

    /** XXX FIXME: this function is called for each drive for the same tape by
     *  the function dev_picker. Each time, we build/allocate same strings and
     *  we parse again the conf. This behaviour is heavy and not optimal.
     */
    rc = rw_drive_types_for_tape(tape->rsc.model, &rw_drives);
    if (rc)
        return rc;

    /* copy the rw_drives list to tokenize it */
    parse_rw_drives = strdup(rw_drives);
    if (parse_rw_drives == NULL)
        return -errno;

    /* For each compatible drive type, get list of associated drive models
     * and search the current drive model in it.
     */
    for (drive_type = strtok_r(parse_rw_drives, ",", &saveptr);
         drive_type != NULL;
         drive_type = strtok_r(NULL, ",", &saveptr)) {
        const char *drive_model_list;

        rc = drive_models_by_type(drive_type, &drive_model_list);
        if (rc)
            goto out_free;

        rc = search_in_list(drive_model_list, drive->dss_dev_info->rsc.model,
                            res);
        if (rc)
            goto out_free;
        /* drive model found: media is compatible */
        if (*res)
            break;
    }

out_free:
    free(parse_rw_drives);
    return rc;
}


/**
 * Device selection policy prototype.
 * @param[in]     required_size required space to perform the write operation.
 * @param[in]     dev_curr      the current device to consider.
 * @param[in,out] dev_selected  the currently selected device.
 * @retval <0 on error
 * @retval 0 to stop searching for a device
 * @retval >0 to check next devices.
 */
typedef int (*device_select_func_t)(size_t required_size,
                                    struct dev_descr *dev_curr,
                                    struct dev_descr **dev_selected);

/**
 * Select a device according to a given status and policy function.
 * Returns a device by setting its ongoing_io flag to true.
 *
 * @param dss     DSS handle.
 * @param op_st   Filter devices by the given operational status.
 *                No filtering is op_st is PHO_DEV_OP_ST_UNSPEC.
 * @param select_func    Drive selection function.
 * @param required_size  Required size for the operation.
 * @param media_tags     Mandatory tags for the contained media (for write
 *                       requests only).
 * @param pmedia         Media that should be used by the drive to check
 *                       compatibility (ignored if NULL)
 */
static struct dev_descr *dev_picker(struct lrs_sched *sched,
                                    enum dev_op_status op_st,
                                    device_select_func_t select_func,
                                    size_t required_size,
                                    const struct tags *media_tags,
                                    struct media_info *pmedia, bool is_write)
{
    struct dev_descr    *selected = NULL;
    int                  selected_i = -1;
    int                  i;
    int                  rc;

    ENTRY;

    for (i = 0; i < sched->devices->len; i++) {
        struct dev_descr *itr = &g_array_index(sched->devices,
                                               struct dev_descr, i);
        struct dev_descr *prev = selected;

        if (itr->ongoing_io) {
            pho_debug("Skipping busy device '%s'", itr->dev_path);
            continue;
        }

        if ((itr->op_status == PHO_DEV_OP_ST_FAILED) ||
            (op_st != PHO_DEV_OP_ST_UNSPEC && itr->op_status != op_st)) {
            pho_debug("Skipping device '%s' with incompatible status %s",
                      itr->dev_path, op_status2str(itr->op_status));
            continue;
        }

        /*
         * The intent is to write: exclude media that are administratively
         * locked, full, do not have the put operation flag and do not have the
         * requested tags
         */
        if (is_write && itr->dss_media_info) {
            if (itr->dss_media_info->rsc.adm_status !=
                    PHO_RSC_ADM_ST_UNLOCKED) {
                pho_debug("Media '%s' is not unlocked",
                          itr->dss_media_info->rsc.id.name);
                continue;
            }

            if (itr->dss_media_info->fs.status == PHO_FS_STATUS_FULL) {
                pho_debug("Media '%s' is full",
                          itr->dss_media_info->rsc.id.name);
                continue;
            }

            if (!itr->dss_media_info->flags.put) {
                pho_debug("Media '%s' has a false put operation flag",
                          itr->dss_media_info->rsc.id.name);
                continue;
            }

            if (media_tags->n_tags > 0 &&
                !tags_in(&itr->dss_media_info->tags, media_tags)) {
                pho_debug("Media '%s' does not match required tags",
                          itr->dss_media_info->rsc.id.name);
                continue;
            }
        }

        /* check tape / drive compat */
        if (pmedia) {
            bool res;

            if (tape_drive_compat(pmedia, itr, &res)) {
                selected = NULL;
                break;
            }

            if (!res)
                continue;
        }

        rc = select_func(required_size, itr, &selected);
        if (prev != selected)
            selected_i = i;

        if (rc < 0) {
            pho_debug("Device selection function failed");
            selected = NULL;
            break;
        } else if (rc == 0) /* stop searching */
            break;
    }

    if (selected != NULL) {
        pho_debug("Picked dev number %d (%s)", selected_i, selected->dev_path);
        selected->ongoing_io = true;
    } else {
        pho_debug("Could not find a suitable %s device", op_status2str(op_st));
    }

    return selected;
}

/**
 * Get the first device with enough space.
 * @retval 0 to stop searching for a device
 * @retval 1 to check next device.
 */
static int select_first_fit(size_t required_size,
                            struct dev_descr *dev_curr,
                            struct dev_descr **dev_selected)
{
    ENTRY;

    if (dev_curr->dss_media_info == NULL)
        return 1;

    if (dev_curr->dss_media_info->stats.phys_spc_free >= required_size) {
        *dev_selected = dev_curr;
        return 0;
    }
    return 1;
}

/**
 *  Get the device with the lower space to match required_size.
 * @return 1 to check next devices, unless an exact match is found (return 0).
 */
static int select_best_fit(size_t required_size,
                           struct dev_descr *dev_curr,
                           struct dev_descr **dev_selected)
{
    ENTRY;

    if (dev_curr->dss_media_info == NULL)
        return 1;

    /* does it fit? */
    if (dev_curr->dss_media_info->stats.phys_spc_free < required_size)
        return 1;

    /* no previous fit, or better fit */
    if (*dev_selected == NULL || (dev_curr->dss_media_info->stats.phys_spc_free
                      < (*dev_selected)->dss_media_info->stats.phys_spc_free)) {
        *dev_selected = dev_curr;

        if (required_size == dev_curr->dss_media_info->stats.phys_spc_free)
            /* exact match, stop searching */
            return 0;
    }
    return 1;
}

/**
 * Select any device without checking media or available size.
 * @return 0 on first device found, 1 else (to continue searching).
 */
static int select_any(size_t required_size,
                      struct dev_descr *dev_curr,
                      struct dev_descr **dev_selected)
{
    ENTRY;

    if (*dev_selected == NULL) {
        *dev_selected = dev_curr;
        /* found an item, stop searching */
        return 0;
    }
    return 1;
}

/* Get the device with the least space available on the loaded media.
 * If a tape is loaded, it just needs to be unloaded.
 * If the filesystem is mounted, umount is needed before unloading.
 * @return 1 (always check all devices).
 */
static int select_drive_to_free(size_t required_size,
                                struct dev_descr *dev_curr,
                                struct dev_descr **dev_selected)
{
    ENTRY;

    /* skip failed and busy drives */
    if (dev_curr->op_status == PHO_DEV_OP_ST_FAILED || dev_curr->ongoing_io) {
        pho_debug("Skipping drive '%s' with status %s%s",
                  dev_curr->dev_path, op_status2str(dev_curr->op_status),
                  dev_curr->ongoing_io ? " (busy)" : "");
        return 1;
    }

    /* if this function is called, no drive should be empty */
    if (dev_curr->op_status == PHO_DEV_OP_ST_EMPTY) {
        pho_warn("Unexpected drive status for '%s': '%s'",
                 dev_curr->dev_path, op_status2str(dev_curr->op_status));
        return 1;
    }

    /* less space available on this device than the previous ones? */
    if (*dev_selected == NULL || dev_curr->dss_media_info->stats.phys_spc_free
                    < (*dev_selected)->dss_media_info->stats.phys_spc_free) {
        *dev_selected = dev_curr;
        return 1;
    }

    return 1;
}

/**
 * Mount the filesystem of a ready device
 *
 * Must be called with :
 * - dev->ongoing_io set to true,
 * - dev->op_status set to PHO_DEV_OP_ST_LOADED and a loaded dev->dss_media_info
 * - a global DSS lock on dev
 * - a global DSS lock on dev->dss_media_info
 *
 * On error, we try to unload dev->media, dev->ongoing_io is set to false,
 * we try to release global DSS locks on dev and dev->dss_media_info,
 * dev->op_status is set to PHO_DEV_OP_ST_FAILED.
 */
static int sched_mount(struct lrs_sched *sched, struct dev_descr *dev)
{
    char                *mnt_root;
    struct fs_adapter    fsa;
    const char          *id;
    int                  rc;

    ENTRY;

    rc = get_fs_adapter(dev->dss_media_info->fs.type, &fsa);
    if (rc)
        goto out;

    rc = ldm_fs_mounted(&fsa, dev->dev_path, dev->mnt_path,
                        sizeof(dev->mnt_path));
    if (rc == 0) {
        dev->op_status = PHO_DEV_OP_ST_MOUNTED;
        return 0;
    }

    /**
     * @TODO If library indicates a media is in the drive but the drive
     * doesn't, we need to query the drive to load the tape.
     */

    id = basename(dev->dev_path);
    if (id == NULL)
        return -EINVAL;

    /* mount the device as PHO_MNT_PREFIX<id> */
    mnt_root = mount_point(id);
    if (!mnt_root)
        LOG_GOTO(out, rc = -ENOMEM, "Unable to get mount point of %s", id);

    pho_verb("Mounting device '%s' as '%s'", dev->dev_path, mnt_root);

    rc = ldm_fs_mount(&fsa, dev->dev_path, mnt_root,
                      dev->dss_media_info->fs.label);
    if (rc)
        LOG_GOTO(out_free, rc, "Failed to mount device '%s'",
                 dev->dev_path);

    /* update device state and set mount point */
    dev->op_status = PHO_DEV_OP_ST_MOUNTED;
    strncpy(dev->mnt_path,  mnt_root, sizeof(dev->mnt_path));
    dev->mnt_path[sizeof(dev->mnt_path) - 1] = '\0';

out_free:
    free(mnt_root);
out:
    if (rc != 0) {
        /**
         * sched_unload_medium is always unlocking dev->dss_media_info.
         * On error, sched_unload_medium is unlocking and setting dev to failed.
         */
        if (!sched_unload_medium(sched, dev)) {
            dev->op_status = PHO_DEV_OP_ST_FAILED;
            sched_device_release(sched, dev);
        }

        dev->ongoing_io = false;
    }

    return rc;
}

/**
 * Load a media into a drive.
 *
 * Must be called while owning a global DSS lock on dev and on media and with
 * the ongoing_io flag set to true on dev.
 *
 * On error, the dev's ongoing_io flag is removed, the media is unlocked and the
 * device is also unlocked is this one is set as FAILED.
 *
 * @return 0 on success, -error number on error. -EBUSY is returned when a
 * drive to drive media movement was prevented by the library or if the device
 * is empty.
 */
static int sched_load_media(struct lrs_sched *sched, struct dev_descr *dev,
                            struct media_info *media)
{
    bool                 failure_on_dev = false;
    struct lib_item_addr media_addr;
    struct lib_adapter   lib;
    int                  rc;
    int                  rc2;

    ENTRY;

    if (dev->op_status != PHO_DEV_OP_ST_EMPTY)
        LOG_GOTO(out, rc = -EAGAIN, "%s: unexpected drive status: status='%s'",
                 dev->dev_path, op_status2str(dev->op_status));

    if (dev->dss_media_info != NULL)
        LOG_GOTO(out, rc = -EAGAIN,
                 "No media expected in device '%s' (found '%s')",
                 dev->dev_path, dev->dss_media_info->rsc.id.name);

    pho_verb("Loading '%s' into '%s'", media->rsc.id.name, dev->dev_path);

    /* get handle to the library depending on device type */
    rc = wrap_lib_open(dev->dss_dev_info->rsc.id.family, &lib);
    if (rc)
        LOG_GOTO(out, rc, "Failed to open lib in %s", __func__);

    /* lookup the requested media */
    rc = ldm_lib_media_lookup(&lib, media->rsc.id.name, &media_addr);
    if (rc)
        LOG_GOTO(out_close, rc, "Media lookup failed");

    rc = ldm_lib_media_move(&lib, &media_addr, &dev->lib_dev_info.ldi_addr);
    /* A movement from drive to drive can be prohibited by some libraries.
     * If a failure is encountered in such a situation, it probably means that
     * the state of the library has changed between the moment it has been
     * scanned and the moment the media and drive have been selected. The
     * easiest solution is therefore to return EBUSY to signal this situation to
     * the caller.
     */
    if (rc == -EINVAL
            && media_addr.lia_type == MED_LOC_DRIVE
            && dev->lib_dev_info.ldi_addr.lia_type == MED_LOC_DRIVE) {
        pho_debug("Failed to move a media from one drive to another, trying "
                  "again later");
        /* @TODO: acquire source drive on the fly? */
        GOTO(out_close, rc = -EBUSY);
    } else if (rc != 0) {
        /* Set operationnal failure state on this drive. It is incomplete since
         * the error can originate from a defect tape too...
         *  - consider marking both as failed.
         *  - consider maintaining lists of errors to diagnose and decide who to
         *    exclude from the cool game.
         */
        LOG_GOTO(out_close, rc, "Media move failed");
    }

    /* update device status */
    dev->op_status = PHO_DEV_OP_ST_LOADED;
    /* associate media to this device */
    dev->dss_media_info = media;
    rc = 0;

out_close:
    rc2 = ldm_lib_close(&lib);
    if (rc2) {
        pho_error(rc2, "Unable to close lib on %s", __func__);
        rc = rc ? : rc2;
    }

out:
    if (rc) {
        sched_medium_release(sched, media);
        if (failure_on_dev) {
            dev->op_status = PHO_DEV_OP_ST_FAILED;
            sched_device_release(sched, dev);
        }

        dev->ongoing_io = false;
    }

    return rc;
}

/** return the device policy function depending on configuration */
static device_select_func_t get_dev_policy(void)
{
    const char *policy_str;

    ENTRY;

    policy_str = PHO_CFG_GET(cfg_lrs, PHO_CFG_LRS, policy);
    if (policy_str == NULL)
        return NULL;

    if (!strcmp(policy_str, "best_fit"))
        return select_best_fit;

    if (!strcmp(policy_str, "first_fit"))
        return select_first_fit;

    pho_error(EINVAL, "Invalid LRS policy name '%s' "
              "(expected: 'best_fit' or 'first_fit')", policy_str);

    return NULL;
}

/**
 * Return true if at least one compatible drive is found.
 *
 * The found compatible drive should be not failed, not locked by
 * administrator and not locked for the current operation.
 *
 * @param(in) pmedia          Media that should be used by the drive to check
 *                            compatibility (ignored if NULL, any not failed and
 *                            not administrator locked drive will fit.
 * @param(in) selected_devs   Devices already selected for this operation.
 * @param(in) n_selected_devs Number of devices already selected.
 * @return                    True if one compatible drive is found, else false.
 */
static bool compatible_drive_exists(struct lrs_sched *sched,
                                    struct media_info *pmedia,
                                    struct dev_descr *selected_devs,
                                    const int n_selected_devs)
{
    int i, j;

    for (i = 0; i < sched->devices->len; i++) {
        struct dev_descr *dev = &g_array_index(sched->devices,
                                               struct dev_descr, i);
        bool is_already_selected = false;

        if (dev->op_status == PHO_DEV_OP_ST_FAILED)
            continue;

        /* check the device is not already selected */
        for (j = 0; j < n_selected_devs; ++j)
            if (!strcmp(dev->dss_dev_info->rsc.id.name,
                        selected_devs[i].dss_dev_info->rsc.id.name)) {
                is_already_selected = true;
                break;
            }
        if (is_already_selected)
            continue;

        if (pmedia) {
            bool is_compat;

            if (tape_drive_compat(pmedia, dev, &is_compat))
                continue;

            if (is_compat)
                return true;
        }
    }

    return false;
}
/**
 * Free one of the devices to allow mounting a new media.
 * On success, the returned device is locked.
 * @param(out) dev_descr       Pointer to an empty drive.
 * @param(in)  pmedia          Media that should be used by the drive to check
 *                             compatibility (ignored if NULL)
 * @param(in)  selected_devs   Devices already selected for this operation.
 * @param(in)  n_selected_devs Number of devices already selected.
 */
static int sched_free_one_device(struct lrs_sched *sched,
                                 struct dev_descr **dev_descr,
                                 struct media_info *pmedia,
                                 struct dev_descr *selected_devs,
                                 const int n_selected_devs)
{
    struct dev_descr *tmp_dev;

    ENTRY;

    while (1) {

        /* get a drive to free (PHO_DEV_OP_ST_UNSPEC for any state) */
        tmp_dev = dev_picker(sched, PHO_DEV_OP_ST_UNSPEC, select_drive_to_free,
                             0, &NO_TAGS, pmedia, false);
        if (tmp_dev == NULL) {
            if (compatible_drive_exists(sched, pmedia, selected_devs,
                                        n_selected_devs))
                LOG_RETURN(-EAGAIN, "No suitable device to free");
            else
                LOG_RETURN(-ENODEV, "No compatible device exists not failed "
                                    "and not locked by admin");
        }

        if (sched_empty_dev(sched, tmp_dev))
            continue;

        /* success: we've got an empty device */
        *dev_descr = tmp_dev;
        return 0;
    }
}

/**
 * Get an additionnal prepared device to perform a write operation.
 * @param[in]     size          Size of the extent to be written.
 * @param[in]     tags          Tags used to filter candidate media, the
 *                              selected media must have all the specified tags.
 * @param[in/out] devs          Array of selected devices to write with.
 * @param[in]     new_dev_index Index of the new device to find. Devices from
 *                              0 to i-1 must be already allocated (with loaded
 *                              and mounted media)
 */
static int sched_get_write_res(struct lrs_sched *sched, size_t size,
                               const struct tags *tags, struct dev_descr **devs,
                               size_t new_dev_index)
{
    struct dev_descr **new_dev = &devs[new_dev_index];
    device_select_func_t dev_select_policy;
    struct media_info *pmedia;
    int rc;

    ENTRY;

    /*
     * @FIXME: externalize this to sched_responses_get to load the device state
     * only once per sched_responses_get call.
     */
    rc = sched_load_dev_state(sched);
    if (rc != 0)
        return rc;

    dev_select_policy = get_dev_policy();
    if (!dev_select_policy)
        return -EINVAL;

    pmedia = NULL;

    /* 1a) is there a mounted filesystem with enough room? */
    *new_dev = dev_picker(sched, PHO_DEV_OP_ST_MOUNTED, dev_select_policy,
                          size, tags, NULL, true);
    if (*new_dev != NULL)
        return 0;

    /* 1b) is there a loaded media with enough room? */
    *new_dev = dev_picker(sched, PHO_DEV_OP_ST_LOADED, dev_select_policy, size,
                          tags, NULL, true);
    if (*new_dev != NULL) {
        /* mount the filesystem and return */
        rc = sched_mount(sched, *new_dev);
        if (rc)
            LOG_RETURN(rc,
                       "Unable to mount already loaded device '%s' from "
                       "writing", (*new_dev)->dev_path);

        return 0;
    }

    /* V1: release a drive and load a tape with enough room.
     * later versions:
     * 2a) is there an idle drive, to eject the loaded tape?
     * 2b) is there an operation that will end soon?
     */
    /* 2) For the next steps, we need a media to write on.
     * It will be loaded into a free drive.
     * Note: sched_select_media locks the media.
     */

    pho_verb("Not enough space on loaded media: selecting another one");
    rc = sched_select_media(sched, &pmedia, size, sched->family, tags,
                            devs, new_dev_index);
    if (rc)
        return rc;

    /**
     * Check if the media is already in a drive.
     *
     * We already look for loaded media with full available size.
     *
     * sched_select_media could find a "split" medium which is already loaded
     * if there is no medium with a enough available size.
     */
    *new_dev = search_loaded_media(sched, pmedia->rsc.id.name);
    if (*new_dev != NULL) {
        media_info_free(pmedia);
        (*new_dev)->ongoing_io = true;
        if ((*new_dev)->op_status != PHO_DEV_OP_ST_MOUNTED)
            return sched_mount(sched, *new_dev);

        return 0;
    }

    /* 3) is there a free drive? */
    *new_dev = dev_picker(sched, PHO_DEV_OP_ST_EMPTY, select_any, 0, &NO_TAGS,
                          pmedia, true);
    if (*new_dev == NULL) {
        pho_verb("No free drive: need to unload one");
        rc = sched_free_one_device(sched, new_dev, pmedia, *devs,
                                   new_dev_index);
        if (rc) {
            sched_medium_release(sched, pmedia);
            media_info_free(pmedia);
            /** TODO: maybe we can try to select an other type of media */
            return rc;
        }
    }

    /* 4) load the selected media into the selected drive */
    rc = sched_load_media(sched, *new_dev, pmedia);
    if (rc) {
        media_info_free(pmedia);
        return rc;
    }

    /* 5) mount the filesystem */
    return sched_mount(sched, *new_dev);
}

static struct dev_descr *search_loaded_media(struct lrs_sched *sched,
                                             const char *name)
{
    int         i;

    ENTRY;

    if (name == NULL)
        return NULL;

    for (i = 0; i < sched->devices->len; i++) {
        const char          *media_id;
        enum dev_op_status   op_st;
        struct dev_descr    *dev;

        dev = &g_array_index(sched->devices, struct dev_descr, i);
        op_st = dev->op_status;

        if (op_st != PHO_DEV_OP_ST_MOUNTED && op_st != PHO_DEV_OP_ST_LOADED)
            continue;

        /* The drive may contain a media unknown to phobos, skip it */
        if (dev->dss_media_info == NULL)
            continue;

        media_id = dev->dss_media_info->rsc.id.name;
        if (media_id == NULL) {
            pho_warn("Cannot retrieve media ID from device '%s'",
                     dev->dev_path);
            continue;
        }

        if (!strcmp(name, media_id))
            return dev;
    }
    return NULL;
}

static int sched_media_prepare_for_read_or_format(struct lrs_sched *sched,
                                                  const struct pho_id *id,
                                                  enum sched_operation op,
                                                  struct dev_descr **pdev,
                                                  struct media_info **pmedia)
{
    struct dev_descr    *dev;
    struct media_info   *med = NULL;
    bool                 post_fs_mount;
    int                  rc;

    ENTRY;

    *pdev = NULL;
    *pmedia = NULL;

    rc = sched_fill_media_info(sched, &med, id);
    if (rc == -EALREADY) {
        pho_debug("Media '%s' is locked, returning EAGAIN", id->name);
        GOTO(out, rc = -EAGAIN);
    } else if (rc) {
        return rc;
    }

    switch (op) {
    case LRS_OP_READ:
        if (!med->flags.get)
            LOG_RETURN(-EPERM, "Cannot do a get, get flag is false on '%s'",
                       id->name);
        if (med->fs.status == PHO_FS_STATUS_BLANK)
            LOG_RETURN(-EINVAL, "Cannot do I/O on unformatted media '%s'",
                       id->name);
        if (med->rsc.adm_status != PHO_RSC_ADM_ST_UNLOCKED)
            LOG_RETURN(-EPERM, "Cannot do I/O on an unavailable medium '%s'",
                       id->name);
        post_fs_mount = true;
        break;
    case LRS_OP_FORMAT:
        if (med->fs.status != PHO_FS_STATUS_BLANK)
            LOG_RETURN(-EINVAL, "Cannot format non-blank media '%s'",
                       id->name);
        post_fs_mount = false;
        break;
    default:
        LOG_RETURN(-ENOSYS, "Unknown operation %x", (int)op);
    }

    /* check if the media is already in a drive */
    dev = search_loaded_media(sched, id->name);
    if (dev != NULL) {
        if (dev->ongoing_io)
            LOG_GOTO(out, rc = -EAGAIN,
                     "Media '%s' is loaded in an already used drive '%s'",
                     id->name, dev->dev_path);

        dev->ongoing_io = true;
        /* Media is in dev, update dev->dss_media_info with fresh media info */
        media_info_free(dev->dss_media_info);
        dev->dss_media_info = med;
    } else {
        pho_verb("Media '%s' is not in a drive", id->name);

        if (med->lock.hostname) {
            rc = check_renew_lock(sched, DSS_MEDIA, med, &med->lock);
            if (rc)
                LOG_GOTO(out, rc,
                         "Unable to renew an existing lock on an unloaded "
                         "media to prepare");
        } else {
            rc = take_and_update_lock(&sched->dss, DSS_MEDIA, med, &med->lock);
            if (rc != 0)
                LOG_GOTO(out, rc = -EAGAIN,
                         "Unable to take lock on a media to prepare");
        }

        /* Is there a free drive? */
        dev = dev_picker(sched, PHO_DEV_OP_ST_EMPTY, select_any, 0, &NO_TAGS,
                         med, false);
        if (dev == NULL) {
            pho_verb("No free drive: need to unload one");
            rc = sched_free_one_device(sched, &dev, med, NULL, 0);
            if (rc != 0) {
                sched_medium_release(sched, med);
                LOG_GOTO(out, rc, "No device available");
            }
        }

        /* load the media in it */
        rc = sched_load_media(sched, dev, med);
        if (rc)
            LOG_GOTO(out, rc,
                     "Unable to load medium '%s' into device '%s' when "
                     "preparing media", med->rsc.id.name, dev->dev_path);
    }

    /* Mount only for READ/WRITE and if not already mounted */
    if (post_fs_mount && dev->op_status != PHO_DEV_OP_ST_MOUNTED)
        rc = sched_mount(sched, dev);

out:
    if (rc) {
        media_info_free(med);
        *pmedia = NULL;
        *pdev = NULL;
    } else {
        *pmedia = med;
        *pdev = dev;
    }
    return rc;
}

/**
 * Load and format a medium to the given fs type.
 *
 * \param[in]       sched       Initialized sched.
 * \param[in]       id          Medium ID for the medium to format.
 * \param[in]       fs          Filesystem type (only PHO_FS_LTFS for now).
 * \param[in]       unlock      Unlock tape if successfully formated.
 * \return                      0 on success, negative error code on failure.
 */
static int sched_format(struct lrs_sched *sched, const struct pho_id *id,
                        enum fs_type fs, bool unlock)
{
    struct dev_descr    *dev = NULL;
    struct media_info   *media_info = NULL;
    int                  rc;
    struct ldm_fs_space  spc = {0};
    struct fs_adapter    fsa;
    uint64_t             fields = 0;

    ENTRY;

    rc = sched_load_dev_state(sched);
    if (rc != 0)
        return rc;

    rc = sched_media_prepare_for_read_or_format(sched, id, LRS_OP_FORMAT, &dev,
                                                &media_info);
    if (rc != 0)
        return rc;

    /* -- from now on, device is owned -- */

    if (dev->dss_media_info == NULL)
        LOG_GOTO(err_out, rc = -EINVAL, "Invalid device state");

    pho_verb("Format media '%s' as %s", id->name, fs_type2str(fs));

    rc = get_fs_adapter(fs, &fsa);
    if (rc)
        LOG_GOTO(err_out, rc, "Failed to get FS adapter");

    rc = ldm_fs_format(&fsa, dev->dev_path, id->name, &spc);
    if (rc)
        LOG_GOTO(err_out, rc, "Cannot format media '%s'", id->name);

    /* Systematically use the media ID as filesystem label */
    strncpy(media_info->fs.label, id->name, sizeof(media_info->fs.label));
    media_info->fs.label[sizeof(media_info->fs.label) - 1] = '\0';
    fields |= FS_LABEL;

    media_info->stats.phys_spc_used = spc.spc_used;
    media_info->stats.phys_spc_free = spc.spc_avail;
    fields |= PHYS_SPC_USED | PHYS_SPC_FREE;

    /* Post operation: update media information in DSS */
    media_info->fs.status = PHO_FS_STATUS_EMPTY;
    fields |= FS_STATUS;

    if (unlock) {
        pho_verb("Unlocking media '%s'", id->name);
        media_info->rsc.adm_status = PHO_RSC_ADM_ST_UNLOCKED;
        fields |= ADM_STATUS;
    }

    rc = dss_media_set(&sched->dss, media_info, 1, DSS_SET_UPDATE, fields);
    if (rc != 0)
        LOG_GOTO(err_out, rc, "Failed to update state of media '%s'",
                 id->name);

err_out:
    dev->ongoing_io = false;

    /* Don't free media_info since it is still referenced inside dev */
    return rc;
}

static bool sched_mount_is_writable(const char *fs_root, enum fs_type fs_type)
{
    struct ldm_fs_space  fs_info = {0};
    struct fs_adapter    fsa;
    int                  rc;

    rc = get_fs_adapter(fs_type, &fsa);
    if (rc)
        LOG_RETURN(rc, "No FS adapter found for '%s' (type %s)",
                   fs_root, fs_type2str(fs_type));

    rc = ldm_fs_df(&fsa, fs_root, &fs_info);
    if (rc)
        LOG_RETURN(rc, "Cannot retrieve media usage information");

    return !(fs_info.spc_flags & PHO_FS_READONLY);
}

/**
 * Query to write a given amount of data by acquiring a new device with medium
 *
 * @param(in)     sched         Initialized LRS.
 * @param(in)     write_size    Size that will be written on the medium.
 * @param(in)     tags          Tags used to select a medium to write on, the
 *                              selected medium must have the specified tags.
 * @param(in/out) devs          Array of devices with the reserved medium
 *                              mounted and loaded in it (no need to free it).
 * @param(in)     new_dev_index Index in dev of the new device to alloc (devices
 *                              from 0 to i-1 must be already allocated : medium
 *                              mounted and loaded)
 *
 * @return 0 on success, -1 * posix error code on failure
 */
static int sched_write_prepare(struct lrs_sched *sched, size_t write_size,
                               const struct tags *tags,
                               struct dev_descr **devs, int new_dev_index)
{
    struct media_info  *media = NULL;
    struct dev_descr   *new_dev;
    int                 rc;

    ENTRY;

retry:
    rc = sched_get_write_res(sched, write_size, tags, devs, new_dev_index);
    if (rc != 0)
        return rc;

    new_dev = devs[new_dev_index];
    media = new_dev->dss_media_info;

    /* LTFS can cunningly mount almost-full tapes as read-only, and so would
     * damaged disks. Mark the media as full, let it be mounted and try to find
     * a new one.
     */
    if (!sched_mount_is_writable(new_dev->mnt_path,
                                 media->fs.type)) {
        pho_warn("Media '%s' OK but mounted R/O, marking full and retrying...",
                 media->rsc.id.name);

        media->fs.status = PHO_FS_STATUS_FULL;
        new_dev->ongoing_io = false;

        rc = dss_media_set(&sched->dss, media, 1, DSS_SET_UPDATE, FS_STATUS);
        if (rc)
            LOG_RETURN(rc, "Unable to update DSS media '%s' status to FULL",
                       media->rsc.id.name);

        new_dev = NULL;
        media = NULL;
        goto retry;
    }

    pho_verb("Writing to media '%s' using device '%s' (free space: %zu bytes)",
             media->rsc.id.name, new_dev->dev_path,
             new_dev->dss_media_info->stats.phys_spc_free);

    return 0;
}

/**
 * Query to read from a given set of medium.
 *
 * @param(in)     sched   Initialized LRS.
 * @param(in)     id      The id of the medium to load
 * @param(out)    dev     Device with the required medium mounted and loaded in
 *                        it (no need to free it).
 *
 * @return 0 on success, -1 * posix error code on failure
 */
static int sched_read_prepare(struct lrs_sched *sched,
                              const struct pho_id *id, struct dev_descr **dev)
{
    struct media_info   *media_info = NULL;
    int                  rc;

    ENTRY;

    rc = sched_load_dev_state(sched);
    if (rc != 0)
        return rc;

    /* Fill in information about media and mount it if needed */
    rc = sched_media_prepare_for_read_or_format(sched, id, LRS_OP_READ, dev,
                                                &media_info);
    if (rc)
        return rc;

    if ((*dev)->dss_media_info == NULL)
        LOG_GOTO(out, rc = -EINVAL, "Invalid device state, expected media '%s'",
                 id->name);

out:
    /* Don't free media_info since it is still referenced inside dev */
    return rc;
}

/** Update media_info stats and push its new state to the DSS */
static int sched_media_update(struct lrs_sched *sched,
                              struct media_info *media_info,
                              size_t size_written, int media_rc,
                              const char *fsroot, bool is_full)
{
    struct ldm_fs_space  spc = {0};
    struct fs_adapter    fsa;
    enum fs_type         fs_type = media_info->fs.type;
    uint64_t             fields = 0;
    int                  rc;

    /* do we have an update to do ? */
    if (!(size_written || media_info->fs.status == PHO_FS_STATUS_EMPTY ||
        is_full || media_info->stats.phys_spc_free == 0))
        return 0;

    rc = get_fs_adapter(fs_type, &fsa);
    if (rc)
        LOG_RETURN(rc, "No FS adapter found for '%s' (type %s)",
                   fsroot, fs_type2str(fs_type));

    rc = ldm_fs_df(&fsa, fsroot, &spc);
    if (rc)
        LOG_RETURN(rc, "Cannot retrieve media usage information");

    if (size_written) {
        media_info->stats.nb_obj = 1;
        media_info->stats.phys_spc_used = spc.spc_used;
        media_info->stats.phys_spc_free = spc.spc_avail;
        fields |= NB_OBJ_ADD | PHYS_SPC_USED | PHYS_SPC_FREE;

        if (media_rc == 0) {
            media_info->stats.logc_spc_used = size_written;
            fields |= LOGC_SPC_USED_ADD;
        }
    }

    if (media_info->fs.status == PHO_FS_STATUS_EMPTY) {
        media_info->fs.status = PHO_FS_STATUS_USED;
        fields |= FS_STATUS;
    }

    if (is_full || media_info->stats.phys_spc_free == 0) {
        media_info->fs.status = PHO_FS_STATUS_FULL;
        fields |= FS_STATUS;
    }

    /* TODO update nb_load, nb_errors, last_load */

    /* @FIXME: this DSS update could be done when releasing the media */
    assert(fields);
    rc = dss_media_set(&sched->dss, media_info, 1, DSS_SET_UPDATE, fields);
    if (rc)
        LOG_RETURN(rc, "Cannot update media information");

    return 0;
}

/*
 * @TODO: support releasing multiple medias at a time (handle a
 * full media_release_req).
 */
static int sched_io_complete(struct lrs_sched *sched,
                             struct media_info *media_info, size_t size_written,
                             int media_rc, const char *fsroot)
{
    struct io_adapter    ioa;
    bool                 is_full = false;
    int                  rc;

    ENTRY;

    rc = get_io_adapter(media_info->fs.type, &ioa);
    if (rc)
        LOG_RETURN(rc, "No suitable I/O adapter for filesystem type: '%s'",
                   fs_type2str(media_info->fs.type));

    rc = ioa_medium_sync(&ioa, fsroot);
    pho_debug("sync: medium=%s rc=%d", media_info->rsc.id.name, rc);
    if (rc)
        LOG_RETURN(rc, "Cannot flush media at: %s", fsroot);

    if (is_medium_global_error(media_rc) || is_medium_global_error(rc))
        is_full = true;

    rc = sched_media_update(sched, media_info, size_written, media_rc, fsroot,
                            is_full);
    if (rc)
        LOG_RETURN(rc, "Cannot update media information");

    return 0;
}

/******************************************************************************/
/* Request/response manipulation **********************************************/
/******************************************************************************/

static int sched_device_add(struct lrs_sched *sched, enum rsc_family family,
                            const char *name)
{
    struct dev_descr device = {0};
    struct dev_info *devi = NULL;
    struct dss_filter filter;
    struct lib_adapter lib;
    int dev_cnt = 0;
    int rc = 0;

    pho_verb("Adding device '%s' to lrs\n", name);
    rc = dss_filter_build(&filter,
                          "{\"$AND\": ["
                          "  {\"DSS::DEV::host\": \"%s\"},"
                          "  {\"DSS::DEV::family\": \"%s\"},"
                          "  {\"DSS::DEV::serial\": \"%s\"},"
                          "  {\"DSS::DEV::adm_status\": \"%s\"}"
                          "]}",
                          get_hostname(),
                          rsc_family2str(family),
                          name,
                          rsc_adm_status2str(PHO_RSC_ADM_ST_UNLOCKED));
    if (rc)
        goto err;

    rc = dss_device_get(&sched->dss, &filter, &devi, &dev_cnt);
    dss_filter_free(&filter);
    if (rc)
        goto err;

    if (dev_cnt == 0) {
        pho_info("No usable device found (%s:%s): check device status",
                 rsc_family2str(family), name);
        GOTO(err_res, rc = -ENXIO);
    }

    rc = check_and_take_device_lock(sched, devi);
    if (rc)
        LOG_GOTO(err_res, rc,
                 "Unable to acquire device '%s'", devi->rsc.id.name);

    device.dss_dev_info = dev_info_dup(devi);
    if (!device.dss_dev_info)
        LOG_GOTO(err_res, rc = -ENOMEM, "Device info duplication failed");

    /* get a handle to the library to query it */
    rc = wrap_lib_open(device.dss_dev_info->rsc.id.family, &lib);
    if (rc)
        goto err_dev;

    rc = sched_fill_dev_info(sched, &lib, &device);
    if (rc)
        goto err_lib;

    /* Add the newly initialized device to the device list */
    g_array_append_val(sched->devices, device);

err_lib:
    ldm_lib_close(&lib);

err_dev:
    if (rc)
        dev_info_free(device.dss_dev_info, true);

err_res:
    dss_res_free(devi, dev_cnt);

err:
    return rc;
}

/**
 * Remove the locked device from the local device array.
 * It will be inserted back once the device status is changed to 'unlocked'.
 */
static int sched_device_lock(struct lrs_sched *sched, const char *name)
{
    struct dev_descr *dev;
    int i;

    for (i = 0; i < sched->devices->len; ++i) {
        dev = &g_array_index(sched->devices, struct dev_descr, i);
        if (!strcmp(name, dev->dss_dev_info->rsc.id.name)) {
            g_array_remove_index_fast(sched->devices, i);
            pho_verb("Removed locked device '%s' from the local database",
                     name);
            return 0;
        }
    }

    pho_verb("Cannot find local device info for '%s', not critical, "
             "will continue", name);

    return 0;
}

/**
 * Update local admin status of device to 'unlocked',
 * or fetch it from the database if unknown
 */
static int sched_device_unlock(struct lrs_sched *sched, const char *name)
{
    struct dev_descr *dev;
    int i;

    for (i = 0; i < sched->devices->len; ++i) {
        dev = &g_array_index(sched->devices, struct dev_descr, i);

        if (!strcmp(name, dev->dss_dev_info->rsc.id.name)) {
            pho_verb("Updating device '%s' state to unlocked", name);
            dev->dss_dev_info->rsc.adm_status = PHO_RSC_ADM_ST_UNLOCKED;
            return 0;
        }
    }

    pho_verb("Cannot find local device info for '%s', will fetch it "
             "from the database", name);

    return sched_device_add(sched, sched->family, name);
}

/** Wrapper of sched_req_free to be used as glib callback */
static void sched_req_free_wrapper(void *reqc)
{
    pho_srl_request_free(((struct req_container *)reqc)->req, true);
    free(((struct req_container *)reqc)->req);
}

/** Wrapper of sched_resp_free to be used as glib callback */
static void sched_resp_free_wrapper(void *respc)
{
    pho_srl_response_free(((struct resp_container *)respc)->resp, false);
    free(((struct resp_container *)respc)->resp);
}

int sched_request_enqueue(struct lrs_sched *sched, struct req_container *reqc)
{
    pho_req_t *req;

    if (!reqc)
        return -EINVAL;

    req = reqc->req;

    if (!req)
        return -EINVAL;

    if (pho_request_is_release(req))
        g_queue_push_tail(sched->release_queue, reqc);
    else
        g_queue_push_tail(sched->req_queue, reqc);

    return 0;
}

/**
 * Flush, update dss status and release locks on a medium and its associated
 * device.
 */
static int sched_handle_medium_release(struct lrs_sched *sched,
                                       pho_req_release_elt_t *medium)
{
    int rc = 0;
    struct dev_descr *dev;

    /* Find the where the media is loaded */
    dev = search_loaded_media(sched, medium->med_id->name);
    if (dev == NULL) {
        LOG_RETURN(-ENOENT,
                 "Could not find '%s' mount point, the media is not loaded",
                 medium->med_id->name);
    }

    /* Flush media and update media info in dss */
    if (medium->to_sync)
        rc = sched_io_complete(sched, dev->dss_media_info, medium->size_written,
                               medium->rc, dev->mnt_path);

    /* mark IO as ended */
    dev->ongoing_io = false;

    return rc;
}

/**
 * Flush and update dss status for all media with to-sync flag from a release
 * request.
 */
static int sched_handle_media_release(struct lrs_sched *sched,
                                      pho_req_release_t *req)
{
    size_t i;
    int rc = 0;

    for (i = 0; i < req->n_media; i++) {
        int rc2 = sched_handle_medium_release(sched, req->media[i]);

        rc = rc ? : rc2;
    }

    return rc;
}

/*
 * @FIXME: this assumes one media is reserved for only one request. In the
 * future, we may want to give a media allocation to multiple requests, we will
 * therefore need to be more careful not to call sched_device_release or
 * sched_medium_release too early, or count nested locks.
 */
/**
 * Handle a write allocation request by finding an appropriate medias to write
 * to and mounting them.
 *
 * The request succeeds totally or all the performed allocations are rolled
 * back.
 */
static int sched_handle_write_alloc(struct lrs_sched *sched, pho_req_t *req,
                                    pho_resp_t *resp)
{
    struct dev_descr **devs = NULL;
    size_t i;
    int rc = 0;
    pho_req_write_t *wreq = req->walloc;

    pho_debug("Write allocation request (%ld medias)", wreq->n_media);

    rc = pho_srl_response_write_alloc(resp, wreq->n_media);
    if (rc)
        return rc;

    devs = calloc(wreq->n_media, sizeof(*devs));
    if (devs == NULL)
        return -ENOMEM;

    resp->req_id = req->id;

    /*
     * @TODO: if media locking becomes ref counted, ensure all selected medias
     * are different
     */
    for (i = 0; i < wreq->n_media; i++) {
        struct tags t;

        pho_resp_write_elt_t *wresp = resp->walloc->media[i];

        pho_debug("Write allocation request media %ld: need %ld bytes",
                  i, wreq->media[i]->size);

        t.n_tags = wreq->media[i]->n_tags;
        t.tags = wreq->media[i]->tags;

        rc = sched_write_prepare(sched, wreq->media[i]->size, &t, devs, i);
        if (rc)
            goto out;

        /* build response */
        wresp->avail_size = devs[i]->dss_media_info->stats.phys_spc_free;
        wresp->med_id->family = devs[i]->dss_media_info->rsc.id.family;
        wresp->med_id->name = strdup(devs[i]->dss_media_info->rsc.id.name);
        wresp->root_path = strdup(devs[i]->mnt_path);
        wresp->fs_type = devs[i]->dss_media_info->fs.type;
        wresp->addr_type = devs[i]->dss_media_info->addr_type;

        pho_debug("Allocated media %s for write request", wresp->med_id->name);

        if (wresp->root_path == NULL) {
            /*
             * Increment i so that the currently selected media is released
             * as well in cleanup
             */
            i++;
            GOTO(out, rc = -ENOMEM);
        }
    }

out:
    free(devs);

    if (rc) {
        size_t n_media_acquired = i;

        /* Rollback device and media acquisition */
        for (i = 0; i < n_media_acquired; i++) {
            struct dev_descr *dev;
            pho_resp_write_elt_t *wresp = resp->walloc->media[i];

            dev = search_loaded_media(sched, wresp->med_id->name);
            assert(dev != NULL);
            dev->ongoing_io = false;
        }

        pho_srl_response_free(resp, false);
        if (rc != -EAGAIN) {
            int rc2 = pho_srl_response_error_alloc(resp);

            if (rc2)
                return rc2;

            resp->error->rc = rc;
            resp->error->req_kind = PHO_REQUEST_KIND__RQ_WRITE;

            /* Request processing error, not an LRS error */
            rc = 0;
        }
    }

    return rc;
}

/**
 * Handle a read allocation request by finding the specified medias and mounting
 * them.
 *
 * The request succeeds totally or all the performed allocations are rolled
 * back.
 */
static int sched_handle_read_alloc(struct lrs_sched *sched, pho_req_t *req,
                                   pho_resp_t *resp)
{
    struct dev_descr *dev = NULL;
    size_t n_selected = 0;
    size_t i;
    int rc = 0;
    pho_req_read_t *rreq = req->ralloc;

    rc = pho_srl_response_read_alloc(resp, rreq->n_required);
    if (rc)
        return rc;

    /*
     * FIXME: this is a very basic selection algorithm that does not try to
     * select the most available media first.
     */
    for (i = 0; i < rreq->n_med_ids; i++) {
        pho_resp_read_elt_t *rresp = resp->ralloc->media[n_selected];
        struct pho_id m;

        m.family = (enum rsc_family)rreq->med_ids[i]->family;
        pho_id_name_set(&m, rreq->med_ids[i]->name);

        rc = sched_read_prepare(sched, &m, &dev);
        if (rc)
            continue;

        n_selected++;

        rresp->fs_type = dev->dss_media_info->fs.type;
        rresp->addr_type = dev->dss_media_info->addr_type;
        rresp->root_path = strdup(dev->mnt_path);
        rresp->med_id->family = rreq->med_ids[i]->family;
        rresp->med_id->name = strdup(rreq->med_ids[i]->name);

        if (n_selected == rreq->n_required)
            break;
    }

    if (rc) {
        /* rollback */
        for (i = 0; i < n_selected; i++) {
            pho_resp_read_elt_t *rresp = resp->ralloc->media[i];

            dev = search_loaded_media(sched, rresp->med_id->name);
            assert(dev != NULL);
            dev->ongoing_io = false;
        }

        pho_srl_response_free(resp, false);
        if (rc != -EAGAIN) {
            int rc2 = pho_srl_response_error_alloc(resp);

            if (rc2)
                return rc2;

            resp->error->rc = rc;
            resp->error->req_kind = PHO_REQUEST_KIND__RQ_READ;

            /* Request processing error, not an LRS error */
            rc = 0;
        }
    }

    return rc;
}

static int to_sync_media_per_release(const pho_req_t *req)
{
    pho_req_release_t *rel = req->release;
    size_t n_media = 0;
    int i;

    assert(pho_request_is_release(req));

    for (i = 0; i < rel->n_media; i++)
        if (rel->media[i]->to_sync)
            n_media++;

    return n_media;
}

/**
 * Handle incoming release requests, appending corresponding release responses
 * to the \p sched's response_queue.
 *
 * Can be called with only_release at true to handle all pending release
 * requests without generating responses, for example when destroying an LRS
 * with sched_fini.
 */
static int sched_handle_release_reqs(struct lrs_sched *sched)
{
    struct req_container *reqc;

    while ((reqc = g_queue_pop_tail(sched->release_queue)) != NULL) {
        struct resp_container *respc = NULL;
        pho_req_t *req = reqc->req;
        size_t n_media;
        int rc = 0;

        rc = sched_handle_media_release(sched, req->release);
        n_media = to_sync_media_per_release(req);

        if (n_media == 0)
            goto free_req;

        respc = malloc(sizeof(*respc));
        if (!respc)
            goto free_req;

        respc->socket_id = reqc->socket_id;
        respc->resp = malloc(sizeof(*respc->resp));
        if (!respc->resp)
            goto free_respc;

        if (rc) {
            int rc2 = pho_srl_response_error_alloc(respc->resp);

            if (rc2) {
                rc = rc2;
                goto free_resp;
            }

            respc->resp->error->rc = rc;
            rc = 0;
            respc->resp->error->req_kind = PHO_REQUEST_KIND__RQ_RELEASE;
        } else {
            pho_req_release_t *rel = req->release;
            pho_resp_release_t *respl;
            size_t i, j;

            rc = pho_srl_response_release_alloc(respc->resp, n_media);
            if (rc)
                goto free_resp;

            /* Build the answer */
            respc->resp->req_id = req->id;
            respl = respc->resp->release;

            for (i = 0, j = 0; i < rel->n_media; ++i) {
                const char *name = rel->media[i]->med_id->name;

                if (!rel->media[i]->to_sync)
                    continue;

                respl->med_ids[j]->family = rel->media[i]->med_id->family;
                respl->med_ids[j]->name = strdup(name);
                j++;
            }
        }

        g_queue_push_tail(sched->response_queue, respc);
        goto free_req;

        /* Free incoming request */
free_resp:
        free(respc->resp);
free_respc:
        free(respc);
free_req:
        pho_srl_request_free(req, true);
        free(reqc);
        if (rc)
            return rc;
    }

    return 0;
}

static int sched_handle_format(struct lrs_sched *sched, pho_req_t *req,
                               pho_resp_t *resp)
{
    int rc = 0;
    pho_req_format_t *freq = req->format;
    struct pho_id m;

    rc = pho_srl_response_format_alloc(resp);
    if (rc)
        return rc;

    m.family = (enum rsc_family)freq->med_id->family;
    pho_id_name_set(&m, freq->med_id->name);

    rc = sched_format(sched, &m, (enum fs_type)freq->fs, freq->unlock);
    if (rc) {
        pho_srl_response_free(resp, false);
        if (rc != -EAGAIN) {
            int rc2 = pho_srl_response_error_alloc(resp);

            if (rc2)
                return rc2;

            resp->req_id = req->id;
            resp->error->rc = rc;
            resp->error->req_kind = PHO_REQUEST_KIND__RQ_FORMAT;

            /* Request processing error, not an LRS error */
            rc = 0;
        }
    } else {
        resp->req_id = req->id;
        resp->format->med_id->family = freq->med_id->family;
        resp->format->med_id->name = strdup(freq->med_id->name);
    }

    return rc;
}

static int sched_handle_notify(struct lrs_sched *sched, pho_req_t *req,
                               pho_resp_t *resp)
{
    pho_req_notify_t *nreq = req->notify;
    int rc = 0;

    rc = pho_srl_response_notify_alloc(resp);
    if (rc)
        return rc;

    switch (nreq->op) {
    case PHO_NTFY_OP_DEVICE_ADD:
        rc = sched_device_add(sched, (enum rsc_family)nreq->rsrc_id->family,
                              nreq->rsrc_id->name);
        break;
    case PHO_NTFY_OP_DEVICE_LOCK:
        rc = sched_device_lock(sched, nreq->rsrc_id->name);
        break;
    case PHO_NTFY_OP_DEVICE_UNLOCK:
        rc = sched_device_unlock(sched, nreq->rsrc_id->name);
        break;
    default:
        LOG_GOTO(err, rc = -EINVAL, "The requested operation is not "
                 "recognized");
    }

    if (rc)
        goto err;

    resp->req_id = req->id;
    resp->notify->rsrc_id->family = nreq->rsrc_id->family;
    resp->notify->rsrc_id->name = strdup(nreq->rsrc_id->name);

    return rc;

err:
    pho_srl_response_free(resp, false);

    if (rc != -EAGAIN) {
        int rc2 = pho_srl_response_error_alloc(resp);

        if (rc2)
            return rc2;

        resp->req_id = req->id;
        resp->error->rc = rc;
        resp->error->req_kind = PHO_REQUEST_KIND__RQ_NOTIFY;

        /* Request processing error, not an LRS error */
        rc = 0;
    }

    return rc;
}

int sched_responses_get(struct lrs_sched *sched, int *n_resp,
                        struct resp_container **respc)
{
    GArray *resp_array;
    size_t release_queue_len = g_queue_get_length(sched->release_queue);
    struct req_container *reqc;
    int rc = 0;

    /* At least release_queue_len responses will be emitted */
    resp_array = g_array_sized_new(FALSE, FALSE, sizeof(struct resp_container),
                                   release_queue_len);
    if (resp_array == NULL)
        return -ENOMEM;
    g_array_set_clear_func(resp_array, sched_resp_free_wrapper);

    /*
     * First release everything that can be.
     *
     * NOTE: in the future, media could be "released" as soon as possible, but
     * only flushed in batch later on. The response to the "release" request
     * would then have to wait for the full flush.
     *
     * TODO: if there are multiple release requests for one media, only release
     * it once but answer to all requests.
     */
    rc = sched_handle_release_reqs(sched);
    if (rc)
        goto out;

    /*
     * Very simple algorithm (FIXME): serve requests until the first EAGAIN is
     * encountered.
     */
    while ((reqc = g_queue_pop_tail(sched->req_queue)) != NULL) {
        pho_req_t *req = reqc->req;
        struct resp_container *respc;

        g_array_set_size(resp_array, resp_array->len + 1);
        respc = &g_array_index(resp_array, struct resp_container,
                               resp_array->len - 1);

        respc->socket_id = reqc->socket_id;
        respc->resp = malloc(sizeof(*respc->resp));
        if (!respc->resp)
            LOG_GOTO(out, rc = -ENOMEM, "lrs cannot allocate response");

        if (pho_request_is_write(req)) {
            pho_debug("lrs received write request (%p)", req);
            rc = sched_handle_write_alloc(sched, req, respc->resp);
        } else if (pho_request_is_read(req)) {
            pho_debug("lrs received read allocation request (%p)", req);
            rc = sched_handle_read_alloc(sched, req, respc->resp);
        } else if (pho_request_is_format(req)) {
            pho_debug("lrs received format request (%p)", req);
            rc = sched_handle_format(sched, req, respc->resp);
        } else if (pho_request_is_notify(req)) {
            pho_debug("lrs received notify request (%p)", req);
            rc = sched_handle_notify(sched, req, respc->resp);
        } else {
            /* Unexpected req->kind, very probably a programming error */
            pho_error(rc = -EPROTO,
                      "lrs received an invalid request "
                      "(no walloc, ralloc or release field)");
        }

        /*
         * Break on EAGAIN and mark the whole run as a success (but there may be
         * no response).
         */
        if (rc == -EAGAIN) {
            /* Requeue last request */
            g_queue_push_tail(sched->req_queue, reqc);
            g_array_remove_index(resp_array, resp_array->len - 1);
            rc = 0;
            break;
        }

        pho_srl_request_free(reqc->req, true);
        free(reqc);
    }

out:
    /* Error return means a fatal error for this LRS (FIXME) */
    if (rc) {
        g_array_free(resp_array, TRUE);
    } else {
        *n_resp = resp_array->len;
        *respc = (struct resp_container *)g_array_free(resp_array, FALSE);
    }

    /*
     * Media that have not been re-acquired at this point could be "globally
     * unlocked" here rather than at the beginning of this function.
     */

    return rc;
}

