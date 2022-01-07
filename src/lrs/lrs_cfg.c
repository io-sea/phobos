/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=4:tabstop=4:
 */
/*
 *  All rights reserved (c) 2014-2017 CEA/DAM.
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
 * \brief  Phobos Local Resource Scheduler configuration utilities.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "lrs_cfg.h"

#include <stdlib.h>
#include <string.h>

const struct pho_config_item cfg_lrs[] = {
    [PHO_CFG_LRS_mount_prefix] = {
        .section = "lrs",
        .name    = "mount_prefix",
        .value   = "/mnt/phobos-"
    },
    [PHO_CFG_LRS_policy] = {
        .section = "lrs",
        .name    = "policy",
        .value   = "best_fit"
    },
    [PHO_CFG_LRS_families] = {
        .section = "lrs",
        .name    = "families",
        .value   = "tape,dir"
    },
    [PHO_CFG_LRS_lib_device] = {
        .section = "lrs",
        .name    = "lib_device",
        .value   = "/dev/changer"
    },
    [PHO_CFG_LRS_server_socket] = {
        .section = "lrs",
        .name    = "server_socket",
        .value   = "/run/phobosd/lrs"
    },
    [PHO_CFG_LRS_lock_file] = {
        .section = "lrs",
        .name    = "lock_file",
        .value   = "/run/phobosd/phobosd.lock"
    },
    [PHO_CFG_LRS_sync_time_threshold] = {
        .section = "lrs",
        .name    = "sync_time_threshold",
        .value   = "tape=10000,dir=10"
    },
    [PHO_CFG_LRS_sync_nb_req_threshold] = {
        .section = "lrs",
        .name    = "sync_nb_req_threshold",
        .value   = "tape=5,dir=5"
    },
};

static int _get_substring_value_from_token(const char *cfg_param,
                                           enum rsc_family family,
                                           char **substring)
{
    const char *cfg_val;
    char *token_dup;
    char *save_ptr;
    char *key;
    int rc;

    rc = pho_cfg_get_val("lrs", cfg_param, &cfg_val);
    if (rc)
        return rc;

    token_dup = strdup(cfg_val);
    if (token_dup == NULL)
        return -errno;

    key = strtok_r(token_dup, ",", &save_ptr);
    if (key == NULL)
        key = token_dup;

    rc = -EINVAL;

    do {
        char *value = strchr(key, '=');

        if (value == NULL)
            continue;

        *value++ = '\0';
        if (strcmp(key, rsc_family_names[family]) != 0)
            continue;

        rc = 0;

        *substring = strdup(value);
        if (*substring == NULL)
            rc = -errno;

        break;
    } while ((key = strtok_r(NULL, ",", &save_ptr)) != NULL);

    free(token_dup);

    return rc;
}

int get_cfg_time_threshold_value(enum rsc_family family,
                                 struct timespec *threshold)
{
    unsigned long num_milliseconds;
    char *endptr;
    char *value;
    int rc;

    rc = _get_substring_value_from_token("sync_time_threshold", family, &value);
    if (rc)
        return rc;

    if (value[0] == '-') {
        rc = -ERANGE;
        goto free_value;
    }

    num_milliseconds = strtoul(value, &endptr, 10);
    if (*endptr != '\0' || *endptr == *value) {
        rc = -EINVAL;
        goto free_value;
    }
    if (num_milliseconds == ULONG_MAX && errno != 0) {
        rc = -errno;
        goto free_value;
    }

    threshold->tv_sec = num_milliseconds / 1000;
    threshold->tv_nsec = (num_milliseconds % 1000) * 1000000;

free_value:
    free(value);

    return rc;
}

int get_cfg_nb_req_threshold_value(enum rsc_family family,
                                   unsigned int *threshold)
{
    unsigned long ul_value;
    char *endptr;
    char *value;
    int rc;

    rc = _get_substring_value_from_token("sync_nb_req_threshold", family,
                                         &value);
    if (rc)
        return rc;

    if (value[0] == '-') {
        rc = -ERANGE;
        goto free_value;
    }

    ul_value = strtoul(value, &endptr, 10);
    if (*endptr != '\0' || *endptr == *value) {
        rc = -EINVAL;
        goto free_value;
    }
    if (ul_value > UINT_MAX || ul_value == 0) {
        rc = -ERANGE;
        goto free_value;
    }

    *threshold = ul_value;

free_value:
    free(value);

    return rc;
}
