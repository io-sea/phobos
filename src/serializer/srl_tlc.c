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
 * \brief  Phobos TLC communication data structure helper.
 *         'srl' stands for SeRiaLizer.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>

#include "pho_common.h"
#include "pho_srl_tlc.h"
#include "pho_types.h"

void pho_srl_tlc_request_ping_alloc(pho_tlc_req_t *req)
{
    pho_tlc_request__init(req);
    req->has_ping = true;
    req->ping = true;
}

void pho_srl_tlc_request_drive_lookup_alloc(pho_tlc_req_t *req)
{
    pho_tlc_request__init(req);
    req->drive_lookup = xmalloc(sizeof(*req->drive_lookup));

    pho_tlc_request__drive_lookup__init(req->drive_lookup);
}

void pho_srl_tlc_request_load_alloc(pho_tlc_req_t *req)
{
    pho_tlc_request__init(req);
    req->load = xmalloc(sizeof(*req->load));
    pho_tlc_request__load__init(req->load);
}

void pho_srl_tlc_request_unload_alloc(pho_tlc_req_t *req)
{
    pho_tlc_request__init(req);
    req->unload = xmalloc(sizeof(*req->unload));
    pho_tlc_request__unload__init(req->unload);
}

void pho_srl_tlc_request_status_alloc(pho_tlc_req_t *req)
{
    pho_tlc_request__init(req);
    req->status = xmalloc(sizeof(*req->status));
    pho_tlc_request__status__init(req->status);
}

void pho_srl_tlc_request_refresh_alloc(pho_tlc_req_t *req)
{
    pho_tlc_request__init(req);
    req->has_refresh = true;
    req->refresh = true;
}

void pho_srl_tlc_request_free(pho_tlc_req_t *req, bool unpack)
{
    if (unpack) {
        pho_tlc_request__free_unpacked(req, NULL);
        return;
    }

    req->has_ping = false;
    req->ping = false;

    if (req->drive_lookup) {
        free(req->drive_lookup->serial);
        free(req->drive_lookup);
        req->drive_lookup = NULL;
    }

    if (req->load) {
        free(req->load->drive_serial);
        free(req->load->tape_label);
        free(req->load);
        req->load = NULL;
    }

    if (req->unload) {
        free(req->unload->drive_serial);
        free(req->unload->tape_label);
        free(req->unload);
        req->unload = NULL;
    }

    if (req->status) {
        free(req->status);
        req->status = NULL;
    }

    req->has_refresh = false;
    req->refresh = false;
}

void pho_srl_tlc_response_ping_alloc(pho_tlc_resp_t *resp)
{
    pho_tlc_response__init(resp);
    resp->ping = xmalloc(sizeof(*resp->ping));

    pho_tlc_response__ping__init(resp->ping);
}

void pho_srl_tlc_response_drive_lookup_alloc(pho_tlc_resp_t *resp)
{
    pho_tlc_response__init(resp);
    resp->drive_lookup = xmalloc(sizeof(*resp->drive_lookup));

    pho_tlc_response__drive_lookup__init(resp->drive_lookup);
}

void pho_srl_tlc_response_load_alloc(pho_tlc_resp_t *resp)
{
    pho_tlc_response__init(resp);
    resp->load = xmalloc(sizeof(*resp->load));
    pho_tlc_response__load__init(resp->load);
}

void pho_srl_tlc_response_unload_alloc(pho_tlc_resp_t *resp)
{
    pho_tlc_response__init(resp);
    resp->unload = xmalloc(sizeof(*resp->unload));
    pho_tlc_response__unload__init(resp->unload);
}

void pho_srl_tlc_response_status_alloc(pho_tlc_resp_t *resp)
{
    pho_tlc_response__init(resp);
    resp->status = xmalloc(sizeof(*resp->status));
    pho_tlc_response__status__init(resp->status);
}

void pho_srl_tlc_response_refresh_alloc(pho_tlc_resp_t *resp)
{
    pho_tlc_response__init(resp);
    resp->has_refresh = true;
    resp->refresh = true;
}

void pho_srl_tlc_response_error_alloc(pho_tlc_resp_t *resp)
{
    pho_tlc_response__init(resp);
    resp->error = xmalloc(sizeof(*resp->error));
    pho_tlc_response__error__init(resp->error);
}

void pho_srl_tlc_response_free(pho_tlc_resp_t *resp, bool unpack)
{
    if (unpack) {
        pho_tlc_response__free_unpacked(resp, NULL);
        return;
    }

    if (resp->ping) {
        free(resp->ping);
        resp->ping = NULL;
    }

    if (resp->drive_lookup) {
        free(resp->drive_lookup->medium_name);
        free(resp->drive_lookup);
        resp->drive_lookup = NULL;
    }

    if (resp->error) {
        free(resp->error->message);
        free(resp->error);
        resp->error = NULL;
    }

    if (resp->load) {
        free(resp->load->message);
        free(resp->load);
        resp->load = NULL;
    }

    if (resp->unload) {
        free(resp->unload->tape_label);
        free(resp->unload->message);
        free(resp->unload);
        resp->unload = NULL;
    }

    if (resp->status) {
        free(resp->status->lib_data);
        free(resp->status->message);
        free(resp->status);
        resp->status = NULL;
    }

    resp->has_refresh = false;
    resp->refresh = false;
}

void pho_srl_tlc_request_pack(pho_tlc_req_t *req, struct pho_buff *buf)
{
    buf->size = pho_tlc_request__get_packed_size(req) +
               PHO_TLC_PROTOCOL_VERSION_SIZE;
    buf->buff = xmalloc(buf->size);

    buf->buff[0] = PHO_TLC_PROTOCOL_VERSION;
    pho_tlc_request__pack(req,
                          (uint8_t *)buf->buff + PHO_TLC_PROTOCOL_VERSION_SIZE);
}

pho_tlc_req_t *pho_srl_tlc_request_unpack(struct pho_buff *buf)
{
    pho_tlc_req_t *req = NULL;

    if (buf->buff[0] != PHO_TLC_PROTOCOL_VERSION)
        LOG_GOTO(out_free, -EPROTONOSUPPORT,
                 "The tlc protocol version '%d' is not correct, requested "
                 "version is '%d'", buf->buff[0], PHO_TLC_PROTOCOL_VERSION);

    req = pho_tlc_request__unpack(NULL,
                                  buf->size - PHO_TLC_PROTOCOL_VERSION_SIZE,
                                  (uint8_t *)buf->buff +
                                      PHO_TLC_PROTOCOL_VERSION_SIZE);

    if (!req)
        pho_error(-EINVAL, "Failed to unpack TLC request");

out_free:
    free(buf->buff);
    return req;
}

void pho_srl_tlc_response_pack(pho_tlc_resp_t *resp, struct pho_buff *buf)
{
    buf->size = pho_tlc_response__get_packed_size(resp) +
                PHO_TLC_PROTOCOL_VERSION_SIZE;
    buf->buff = xmalloc(buf->size);

    buf->buff[0] = PHO_TLC_PROTOCOL_VERSION;
    pho_tlc_response__pack(resp,
                           (uint8_t *)buf->buff +
                               PHO_TLC_PROTOCOL_VERSION_SIZE);
}

pho_tlc_resp_t *pho_srl_tlc_response_unpack(struct pho_buff *buf)
{
    pho_tlc_resp_t *resp = NULL;

    if (buf->buff[0] != PHO_TLC_PROTOCOL_VERSION)
        LOG_GOTO(out_free, -EPROTONOSUPPORT,
                 "The TLC protocol version '%d' is not correct, requested "
                 "version is '%d'", buf->buff[0], PHO_TLC_PROTOCOL_VERSION);

     resp = pho_tlc_response__unpack(NULL,
                                     buf->size - PHO_TLC_PROTOCOL_VERSION_SIZE,
                                     (uint8_t *)buf->buff +
                                         PHO_TLC_PROTOCOL_VERSION_SIZE);

    if (!resp)
        pho_error(-EINVAL, "Failed to unpack TLC request");

out_free:
    free(buf->buff);
    return resp;
}

