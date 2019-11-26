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
 * \brief  Phobos communication data structure helper.
 *         'srl' stands for SeRiaLizer.
 */
#ifndef _PHO_SRL_REQREP_H
#define _PHO_SRL_REQREP_H

#include "pho_types.h"
#include "pho_proto_lrs.pb-c.h"

/******************************************************************************/
/* Typedefs *******************************************************************/
/******************************************************************************/

typedef PhoRequest                  pho_req_t;
typedef PhoRequest__Write           pho_req_write_t;
typedef PhoRequest__Write__Elt      pho_req_write_elt_t;
typedef PhoRequest__Read            pho_req_read_t;
typedef PhoRequest__Release         pho_req_release_t;
typedef PhoRequest__Release__Elt    pho_req_release_elt_t;

typedef PhoResponse                 pho_resp_t;
typedef PhoResponse__Write          pho_resp_write_t;
typedef PhoResponse__Write__Elt     pho_resp_write_elt_t;
typedef PhoResponse__Read__Elt      pho_resp_read_t;
typedef PhoResponse__Read__Elt      pho_resp_read_elt_t;
typedef PhoResponse__Release        pho_resp_release_t;
typedef PhoResponse__Error          pho_resp_error_t;

/******************************************************************************/
/* Macros & constants *********************************************************/
/******************************************************************************/

/**
 * Current version of the protocol.
 * If the protocol version is greater than 127, need to increase its size
 * to an integer size (4 bytes).
 */
#define PHO_PROTOCOL_VERSION      1
/**
 * Protocol version size in bytes.
 */
#define PHO_PROTOCOL_VERSION_SIZE 1

/******************************************************************************/
/** Type checkers *************************************************************/
/******************************************************************************/

/**
 * Request write alloc checker.
 *
 * \param[in]       req         Request.
 *
 * \return                      true if the request is a write alloc one,
 *                              false else.
 */
static inline bool pho_request_is_write(const pho_req_t *req)
{
    return req->walloc != NULL;
}

/**
 * Request read alloc checker.
 *
 * \param[in]       req         Request.
 *
 * \return                      true if the request is a read alloc one,
 *                              false else.
 */
static inline bool pho_request_is_read(const pho_req_t *req)
{
    return req->ralloc != NULL;
}

/**
 * Request release checker.
 *
 * \param[in]       req         Request.
 *
 * \return                      true if the request is a release one,
 *                              false else.
 */
static inline bool pho_request_is_release(const pho_req_t *req)
{
    return req->release != NULL;
}

/**
 * Response write alloc checker.
 *
 * \param[in]       req         Response.
 *
 * \return                      true if the response is a write alloc one,
 *                              false else.
 */
static inline bool pho_response_is_write(const pho_resp_t *resp)
{
    return resp->walloc != NULL;
}

/**
 * Response read alloc checker.
 *
 * \param[in]       req         Response.
 *
 * \return                      true if the response is a read alloc one,
 *                              false else.
 */
static inline bool pho_response_is_read(const pho_resp_t *resp)
{
    return resp->ralloc != NULL;
}

/**
 * Response release checker.
 *
 * \param[in]       req         Response.
 *
 * \return                      true if the response is a release one,
 *                              false else.
 */
static inline bool pho_response_is_release(const pho_resp_t *resp)
{
    return resp->release != NULL;
}

/**
 * Response error checker.
 *
 * \param[in]       req         Response.
 *
 * \return                      true if the response is an error one,
 *                              false else.
 */
static inline bool pho_response_is_error(const pho_resp_t *resp)
{
    return resp->error != NULL;
}

/******************************************************************************/
/** Converters to string ******************************************************/
/******************************************************************************/

/**
 * Human readable string converter for request kind.
 *
 * \param[in]       req         Request.
 *
 * \return                      String conversion of the request kind.
 */
const char *pho_srl_request_kind_str(pho_req_t *req);

/**
 * Human readable string converter for response kind.
 *
 * \param[in]       resp        Response.
 *
 * \return                      String conversion of the response kind.
 */
const char *pho_srl_response_kind_str(pho_resp_t *resp);

/**
 * Human readable string converter for request kind of an error response.
 *
 * \param[in]       err         Error response.
 *
 * \return                      String conversion of the request kind.
 */
const char *pho_srl_error_kind_str(pho_resp_error_t *err);

/******************************************************************************/
/** Allocators & Deallocators *************************************************/
/******************************************************************************/

/**
 * Allocation of write request contents.
 *
 * \param[out]      req         Pointer to the request data structure.
 * \param[in]       n_media     Number of media targeted by the request.
 * \param[in]       n_tags      Array which contains, for each medium,
 *                              the number of tags it is defined by.
 *
 * \return                      0 on success, -ENOMEM on failure.
 */
int pho_srl_request_write_alloc(pho_req_t *req, size_t n_media, size_t *n_tags);

/**
 * Allocation of read request contents.
 *
 * \param[out]      req         Pointer to the request data structure.
 * \param[in]       n_media     Number of media targeted by the request.
 *
 * \return                      0 on success, -ENOMEM on failure.
 */
int pho_srl_request_read_alloc(pho_req_t *req, size_t n_media);

/**
 * Allocation of release request contents.
 *
 * \param[out]      req         Pointer to the request data structure.
 * \param[in]       n_media     Number of media targeted by the request.
 *
 * \return                      0 on success, -ENOMEM on failure.
 */
int pho_srl_request_release_alloc(pho_req_t *req, size_t n_media);

/**
 * Release of request contents.
 *
 * \param[in]       req         Pointer to the request data structure.
 * \param[in]       unpack      true if the request comes from an unpack,
 *                              false else.
 */
void pho_srl_request_free(pho_req_t *req, bool unpack);

/**
 * Allocation of write response contents.
 *
 * \param[out]      req         Pointer to the response data structure.
 * \param[in]       n_media     Number of media targeted by the response.
 *
 * \return                      0 on success, -ENOMEM on failure.
 */
int pho_srl_response_write_alloc(pho_resp_t *resp, size_t n_media);

/**
 * Allocation of read response contents.
 *
 * \param[out]      req         Pointer to the response data structure.
 * \param[in]       n_media     Number of media targeted by the response.
 *
 * \return                      0 on success, -ENOMEM on failure.
 */
int pho_srl_response_read_alloc(pho_resp_t *resp, size_t n_media);

/**
 * Allocation of release response contents.
 *
 * \param[out]      req         Pointer to the response data structure.
 * \param[in]       n_media     Number of media targeted by the response.
 *
 * \return                      0 on success, -ENOMEM on failure.
 */
int pho_srl_response_release_alloc(pho_resp_t *resp, size_t n_media);

/**
 * Allocation of error response contents.
 *
 * \param[out]      req         Pointer to the response data structure.
 *
 * \return                      0 on success, -ENOMEM on failure.
 */
int pho_srl_response_error_alloc(pho_resp_t *resp);

/**
 * Release of response contents.
 * \param[in]       req         Pointer to the response data structure.
 * \param[in]       unpack      true if the response comes from an unpack,
 *                              false else.
 */
void pho_srl_response_free(pho_resp_t *resp, bool unpack);

/******************************************************************************/
/* Packers & Unpackers ********************************************************/
/******************************************************************************/

/**
 * Serialization of a request.
 *
 * The allocation of the buffer is made in this function. Its release is made
 * when calling pho_srl_request_unpack(buf).
 *
 * \param[in]       req         Request data structure.
 * \param[out]      buf         Serialized buffer data structure.
 *
 * \return                      0 on success, -ENOMEM on failure.
 */
int pho_srl_request_pack(pho_req_t *req, struct pho_buff *buf);

/**
 * Deserialization of a request.
 *
 * Once the request in unpacked, the buffer is released. The request structure
 * must be freed using pho_srl_request_free(r, true).
 *
 * \param[in]       buf         Serialized buffer data structure.
 *
 * \return                      Request data structure.
 */
pho_req_t *pho_srl_request_unpack(struct pho_buff *buf);

/**
 * Serialization of a response.
 *
 * The allocation of the buffer is made in this function. Its release is made
 * when calling pho_srl_response_unpack(buf).
 *
 * \param[in]       resp        Response data structure.
 * \param[out]      buf         [output] Serialized buffer data structure.
 *
 * \return                      0 on success, -ENOMEM on failure.
 */
int pho_srl_response_pack(pho_resp_t *resp, struct pho_buff *buf);

/**
 * Deserialization of a response.
 *
 * Once the response is unpacked, the buffer is released. The response structure
 * must be freed using pho_srl_response_free(r, true).
 *
 * \param[in]       buf         Serialized buffer data structure.
 *
 * \return                      Response data structure.
 */
pho_resp_t *pho_srl_response_unpack(struct pho_buff *buf);

#endif