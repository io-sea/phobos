/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=4:tabstop=4:
 */
/*
 *  All rights reserved (c) 2014-2023 CEA/DAM.
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
 * \brief  Phobos Raid5 Layout plugin
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "raid5.h"
#include "raid_common.h"
#include <errno.h>
#include <glib.h>
#include <string.h>
#include <unistd.h>

#include "pho_attrs.h"
#include "pho_cfg.h"
#include "pho_common.h"
#include "pho_dss.h"
#include "pho_io.h"
#include "pho_layout.h"
#include "pho_module_loader.h"
#include "pho_srl_common.h"
#include "pho_type_utils.h"

#define PLUGIN_NAME     "raid5"
#define PLUGIN_MAJOR    0
#define PLUGIN_MINOR    1

static struct module_desc RAID5_MODULE_DESC = {
    .mod_name  = PLUGIN_NAME,
    .mod_major = PLUGIN_MAJOR,
    .mod_minor = PLUGIN_MINOR,
};

__attribute__((unused))
static const struct pho_enc_ops RAID5_ENCODER_OPS = {
    .step       = raid_encoder_step,
    .destroy    = raid_encoder_destroy,
};

static const struct pho_layout_module_ops LAYOUT_RAID5_OPS = {
    .encode = NULL,
    .decode = NULL,
    .locate = NULL,
    .get_specific_attrs = NULL,
    .reconstruct = NULL,
};

/** Layout module registration entry point */
int pho_module_register(void *module, void *context)
{
    struct layout_module *self = (struct layout_module *) module;

    phobos_module_context_set(context);

    self->desc = RAID5_MODULE_DESC;
    self->ops = &LAYOUT_RAID5_OPS;

    return 0;
}
