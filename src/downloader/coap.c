/*
 * Copyright 2017 AVSystem <avsystem@avsystem.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>

#include <inttypes.h>

#include <avsystem/commons/coap/msg_builder.h>
#include <avsystem/commons/coap/msg_opt.h>
#include <avsystem/commons/utils.h>

#define ANJAY_DOWNLOADER_INTERNALS

#include "private.h"

VISIBILITY_SOURCE_BEGIN

typedef struct {
    anjay_download_ctx_common_t common;

    anjay_url_t uri;
    size_t bytes_downloaded;
    size_t block_size;
    anjay_etag_t etag;

    avs_net_abstract_socket_t *socket;
    avs_coap_msg_identity_t last_req_id;

    /*
     * After calling @ref _anjay_downloader_download:
     *     handle to a job that sends the initial request.
     * During the download (after sending the initial request):
     *     handle to retransmission job.
     * After receiving a separate ACK:
     *     handle to a job aborting the transfer if no Separate Response was
     *     received.
     */
    anjay_sched_handle_t sched_job;
} anjay_coap_download_ctx_t;

static void cleanup_coap_transfer(anjay_downloader_t *dl,
                                  AVS_LIST(anjay_download_ctx_t) *ctx_ptr) {
    anjay_coap_download_ctx_t *ctx = (anjay_coap_download_ctx_t *) *ctx_ptr;
    if (ctx->sched_job) {
        _anjay_sched_del(_anjay_downloader_get_anjay(dl)->sched,
                         &ctx->sched_job);
    }
    _anjay_url_cleanup(&ctx->uri);
#ifndef ANJAY_TEST
    avs_net_socket_cleanup(&ctx->socket);
#endif // ANJAY_TEST
    AVS_LIST_DELETE(ctx_ptr);
}

static int fill_coap_request_info(avs_coap_msg_info_t *req_info,
                                  const anjay_coap_download_ctx_t *ctx) {
    req_info->type = AVS_COAP_MSG_CONFIRMABLE;
    req_info->code = AVS_COAP_CODE_GET;
    req_info->identity = ctx->last_req_id;

    AVS_LIST(anjay_string_t) elem;
    AVS_LIST_FOREACH(elem, ctx->uri.uri_path) {
        if (avs_coap_msg_info_opt_string(req_info, AVS_COAP_OPT_URI_PATH,
                                         elem->c_str)) {
            return -1;
        }
    }
    AVS_LIST_FOREACH(elem, ctx->uri.uri_query) {
        if (avs_coap_msg_info_opt_string(req_info, AVS_COAP_OPT_URI_QUERY,
                                         elem->c_str)) {
            return -1;
        }
    }

    avs_coap_block_info_t block2 = {
        .type = AVS_COAP_BLOCK2,
        .valid = true,
        .seq_num = (uint32_t)(ctx->bytes_downloaded / ctx->block_size),
        .size = (uint16_t)ctx->block_size,
        .has_more = false
    };
    if (avs_coap_msg_info_opt_block(req_info, &block2)) {
        return -1;
    }

    return 0;
}

static int request_coap_block(anjay_downloader_t *dl,
                              anjay_coap_download_ctx_t *ctx);

static int request_coap_block_job(anjay_t *anjay,
                                  void *id_) {
    uintptr_t id = (uintptr_t)id_;

    AVS_LIST(anjay_download_ctx_t) *ctx =
            _anjay_downloader_find_ctx_ptr_by_id(&anjay->downloader, id);
    if (!ctx) {
        dl_log(DEBUG, "download id = %" PRIuPTR " not found (expired?)", id);
        return 0;
    }

    request_coap_block(&anjay->downloader, (anjay_coap_download_ctx_t *) *ctx);

    // return non-zero to ensure job retries
    return -1;
}

static int schedule_coap_retransmission(anjay_downloader_t *dl,
                                        anjay_coap_download_ctx_t *ctx) {
    anjay_t *anjay = _anjay_downloader_get_anjay(dl);
    const avs_coap_tx_params_t *tx_params = &anjay->udp_tx_params;

    avs_coap_retry_state_t retry_state = { 0, { 0,  0 } };

    // first retry
    avs_coap_update_retry_state(&retry_state, tx_params, &dl->rand_seed);
    avs_time_duration_t delay = retry_state.recv_timeout;

    // second retry
    avs_coap_update_retry_state(&retry_state, tx_params, &dl->rand_seed);
    anjay_sched_retryable_backoff_t backoff = {
        .delay = retry_state.recv_timeout,
        .max_delay = avs_coap_max_transmit_span(tx_params)
    };

    _anjay_sched_del(anjay->sched, &ctx->sched_job);
    return _anjay_sched_retryable(anjay->sched, &ctx->sched_job, delay, backoff,
                                  request_coap_block_job,
                                  (void*)ctx->common.id);
}

static int request_coap_block(anjay_downloader_t *dl,
                              anjay_coap_download_ctx_t *ctx) {
    anjay_t *anjay = _anjay_downloader_get_anjay(dl);
    avs_coap_msg_info_t info = avs_coap_msg_info_init();
    int result = -1;

    if (fill_coap_request_info(&info, (anjay_coap_download_ctx_t *) ctx)) {
        goto finish;
    }

    const size_t required_storage_size =
            avs_coap_msg_info_get_packet_storage_size(&info, 0);
    if (required_storage_size > anjay->out_buffer_size) {
        dl_log(ERROR, "CoAP output buffer too small to hold download request "
                      "(at least %zu bytes is needed)",
               required_storage_size);
        goto finish;
    }
    avs_coap_msg_builder_t builder;
    avs_coap_msg_builder_init(
            &builder,avs_coap_ensure_aligned_buffer(anjay->out_buffer),
            anjay->out_buffer_size, &info);

    const avs_coap_msg_t *msg = avs_coap_msg_builder_get_msg(&builder);

    result = avs_coap_ctx_send(anjay->coap_ctx, ctx->socket, msg);

    if (result) {
        dl_log(ERROR, "could not send request: %d", result);
    }

finish:
    avs_coap_msg_info_reset(&info);
    return result;
}

static int request_next_coap_block(anjay_downloader_t *dl,
                                   AVS_LIST(anjay_download_ctx_t) *ctx_ptr) {
    anjay_coap_download_ctx_t *ctx = (anjay_coap_download_ctx_t *) *ctx_ptr;
    ctx->last_req_id = _anjay_coap_id_source_get(dl->id_source);

    if (request_coap_block(dl, ctx)
            || schedule_coap_retransmission(dl, ctx)) {
        dl_log(WARNING, "could not request block starting at %zu for download "
               "id = %" PRIuPTR, ctx->bytes_downloaded, ctx->common.id);
        _anjay_downloader_abort_transfer(dl, ctx_ptr,
                                         ANJAY_DOWNLOAD_ERR_FAILED);
        return -1;
    }

    return 0;
}

static int request_next_coap_block_job(anjay_t *anjay, void *id_) {
    uintptr_t id = (uintptr_t)id_;
    AVS_LIST(anjay_download_ctx_t) *ctx =
            _anjay_downloader_find_ctx_ptr_by_id(&anjay->downloader, id);
    if (!ctx) {
        dl_log(DEBUG, "download id = %" PRIuPTR "expired", id);
        return 0;
    }

    return request_next_coap_block(&anjay->downloader, ctx);
}

static inline const char *etag_to_string(char *buf,
                                         size_t buf_size,
                                         const anjay_etag_t *etag) {
    assert(buf_size >= sizeof(etag->value) * 3 + 1
            && "buffer too small to hold ETag");

    for (size_t i = 0; i < etag->size; ++i) {
        snprintf(&buf[i * 3], buf_size - i * 3, "%02x ", etag->value[i]);
    }

    return buf;
}

#define ETAG_STR(EtagPtr) etag_to_string(&(char[32]){0}[0], 32, (EtagPtr))

static int read_etag(const avs_coap_msg_t *msg,
                     anjay_etag_t *out_etag) {
    const avs_coap_opt_t *etag_opt = NULL;
    int result = avs_coap_msg_find_unique_opt(msg, AVS_COAP_OPT_ETAG,
                                              &etag_opt);
    if (!etag_opt) {
        dl_log(TRACE, "no ETag option");
        out_etag->size = 0;
        return 0;
    }

    if (etag_opt && result) {
        dl_log(DEBUG, "multiple ETag options found");
        return -1;
    }

    uint32_t etag_size = avs_coap_opt_content_length(etag_opt);
    if (etag_size > sizeof(out_etag->value)) {
        dl_log(DEBUG, "invalid ETag option size");
        return -1;
    }

    out_etag->size = (uint8_t)etag_size;
    memcpy(out_etag->value, avs_coap_opt_value(etag_opt), out_etag->size);

    dl_log(TRACE, "ETag: %s", ETAG_STR(out_etag));
    return 0;
}

static inline bool etag_matches(const anjay_etag_t *a,
                                const anjay_etag_t *b) {
    return a->size == b->size && !memcmp(a->value, b->value, a->size);
}

static int parse_coap_response(const avs_coap_msg_t *msg,
                               anjay_coap_download_ctx_t *ctx,
                               avs_coap_block_info_t *out_block2,
                               anjay_etag_t *out_etag) {
    if (read_etag(msg, out_etag)) {
        return -1;
    }

    int result = avs_coap_get_block_info(msg, AVS_COAP_BLOCK2, out_block2);
    if (result) {
        dl_log(DEBUG, "malformed response");
        return -1;
    }

    if (!out_block2->valid) {
        dl_log(DEBUG, "BLOCK2 option missing");
        return -1;
    }

    if (out_block2->has_more
            && out_block2->size != avs_coap_msg_payload_length(msg)) {
        dl_log(DEBUG, "malformed response: mismatched size of intermediate "
               "packet");
        return -1;
    }


    const size_t requested_seq_num = ctx->bytes_downloaded / ctx->block_size;
    const size_t expected_offset = requested_seq_num * ctx->block_size;
    const size_t obtained_offset = out_block2->seq_num * out_block2->size;
    if (expected_offset != obtained_offset) {
        dl_log(DEBUG,
               "expected to get data from offset %zu but got %zu instead",
               expected_offset, obtained_offset);
        return -1;
    }

    if (out_block2->size > ctx->block_size) {
        dl_log(DEBUG, "block size renegotiation failed: requested %zu, got %zu",
               ctx->block_size, (size_t)out_block2->size);
        return -1;
    } else if (out_block2->size < ctx->block_size) {
        // Allow late block size renegotiation, as we may be in the middle of
        // a download resumption, in which case we have no idea what block size
        // is appropriate. If it is not the case, and the server decided to send
        // us smaller blocks instead, it won't hurt us to get them anyway.
        dl_log(DEBUG, "block size renegotiated: %zu -> %zu",
               (size_t) ctx->block_size, (size_t) out_block2->size);
        ctx->block_size = out_block2->size;
    }

    return 0;
}

static void handle_coap_response(const avs_coap_msg_t *msg,
                                 anjay_downloader_t *dl,
                                 AVS_LIST(anjay_download_ctx_t) *ctx_ptr) {
    const uint8_t code = avs_coap_msg_get_code(msg);
    if (code != AVS_COAP_CODE_CONTENT) {
        dl_log(DEBUG, "server responded with %s (expected %s)",
               AVS_COAP_CODE_STRING(code),
               AVS_COAP_CODE_STRING(AVS_COAP_CODE_CONTENT));
        _anjay_downloader_abort_transfer(dl, ctx_ptr, -code);
        return;
    }

    anjay_coap_download_ctx_t *ctx = (anjay_coap_download_ctx_t *) *ctx_ptr;
    avs_coap_block_info_t block2;
    anjay_etag_t etag;
    if (parse_coap_response(msg, ctx, &block2, &etag)) {
        _anjay_downloader_abort_transfer(dl, ctx_ptr,
                                         ANJAY_DOWNLOAD_ERR_FAILED);
        return;
    }

    if (ctx->bytes_downloaded == 0) {
        assert(ctx->etag.size == 0 && "overwriting ETag!? we're supposed "
                "to be handling the first packet!");
        ctx->etag = etag;
    } else if (!etag_matches(&etag, &ctx->etag)) {
        dl_log(DEBUG, "remote resource expired, aborting download");
        _anjay_downloader_abort_transfer(dl, ctx_ptr,
                                         ANJAY_DOWNLOAD_ERR_EXPIRED);
        return;
    }

    const void *payload = avs_coap_msg_payload(msg);
    size_t payload_size = avs_coap_msg_payload_length(msg);

    // Resumption from a non-multiple block-size
    size_t offset = ctx->bytes_downloaded % ctx->block_size;
    if (offset) {
        payload = (const char *) payload + offset;
        payload_size -= offset;
    }

    if (ctx->common.on_next_block(_anjay_downloader_get_anjay(dl),
                                  (const uint8_t *) payload, payload_size,
                                  &etag, ctx->common.user_data)) {
        _anjay_downloader_abort_transfer(dl, ctx_ptr,
                                         ANJAY_DOWNLOAD_ERR_FAILED);
        return;
    }

    ctx->bytes_downloaded += payload_size;
    if (!block2.has_more) {
        dl_log(INFO, "transfer id = %" PRIuPTR " finished", ctx->common.id);
        _anjay_downloader_abort_transfer(dl, ctx_ptr, 0);
    } else if (!request_next_coap_block(dl, ctx_ptr)) {
        dl_log(TRACE, "transfer id = %" PRIuPTR ": %zu B downloaded",
               ctx->common.id, ctx->bytes_downloaded);
    }
}

static int abort_transfer_job(anjay_t *anjay,
                              void *ctx_) {
    AVS_LIST(anjay_download_ctx_t) ctx = (AVS_LIST(anjay_download_ctx_t))ctx_;
    AVS_LIST(anjay_download_ctx_t) *ctx_ptr =
            AVS_LIST_FIND_PTR(&anjay->downloader.downloads, ctx);

    if (!ctx_ptr) {
        anjay_log(WARNING, "transfer already aborted");
    } else {
        anjay_log(WARNING, "aborting download: response not received");
        _anjay_downloader_abort_transfer(&anjay->downloader, ctx_ptr,
                                         ANJAY_DOWNLOAD_ERR_FAILED);
    }

    return 0;
}

static void handle_coap_message(anjay_downloader_t *dl,
                                AVS_LIST(anjay_download_ctx_t) *ctx_ptr) {
    anjay_t *anjay = _anjay_downloader_get_anjay(dl);
    assert(ctx_ptr);
    assert(*ctx_ptr);

    avs_coap_msg_t *msg = (avs_coap_msg_t *) avs_coap_ensure_aligned_buffer(
            anjay->in_buffer);

    anjay_coap_download_ctx_t *ctx = (anjay_coap_download_ctx_t *) *ctx_ptr;
    avs_coap_ctx_set_tx_params(anjay->coap_ctx, &anjay->udp_tx_params);
    int result = avs_coap_ctx_recv(anjay->coap_ctx, ctx->socket, msg,
                                   anjay->in_buffer_size);

    if (result) {
        dl_log(DEBUG, "recv result: %d", result);
        return;
    }

    bool msg_id_must_match = true;
    avs_coap_msg_type_t type = avs_coap_msg_get_type(msg);
    switch (type) {
    case AVS_COAP_MSG_RESET:
    case AVS_COAP_MSG_ACKNOWLEDGEMENT:
        break;
    case AVS_COAP_MSG_CONFIRMABLE:
        msg_id_must_match = false; // Separate Response
        break;
    case AVS_COAP_MSG_NON_CONFIRMABLE:
        dl_log(DEBUG, "unexpected msg type: %d, ignoring", (int)type);
        return;
    }

    if (!avs_coap_msg_token_matches(msg, &ctx->last_req_id)) {
        dl_log(DEBUG, "token mismatch, ignoring");
        return;
    }

    if (msg_id_must_match) {
        if (avs_coap_msg_get_id(msg) != ctx->last_req_id.msg_id) {
            dl_log(DEBUG, "msg id mismatch (got %u, expected %u), ignoring",
                   avs_coap_msg_get_id(msg), ctx->last_req_id.msg_id);
            return;
        } else if (type == AVS_COAP_MSG_RESET) {
            dl_log(DEBUG, "Reset response, aborting transfer");
            _anjay_downloader_abort_transfer(dl, ctx_ptr,
                                             ANJAY_DOWNLOAD_ERR_FAILED);
            return;
        } else if (type == AVS_COAP_MSG_ACKNOWLEDGEMENT
                   && avs_coap_msg_get_code(msg) == AVS_COAP_CODE_EMPTY) {
            avs_time_duration_t abort_delay = avs_coap_exchange_lifetime(
                    &anjay->udp_tx_params);
            dl_log(DEBUG, "Separate ACK received, waiting "
                          "%" PRId64 ".%09" PRId32 " for response",
                   abort_delay.seconds, abort_delay.nanoseconds);

            _anjay_sched_del(anjay->sched, &ctx->sched_job);
            _anjay_sched(anjay->sched, &ctx->sched_job, abort_delay,
                         abort_transfer_job, *ctx_ptr);
            return;
        }
    } else {
        dl_log(TRACE, "Separate Response received");
        avs_coap_ctx_send_empty(anjay->coap_ctx, ctx->socket,
                                AVS_COAP_MSG_ACKNOWLEDGEMENT,
                                avs_coap_msg_get_id(msg));
    }

    handle_coap_response(msg, dl, ctx_ptr);
}

static avs_net_abstract_socket_t *get_coap_socket(anjay_downloader_t *dl,
                                                  anjay_download_ctx_t *ctx) {
    (void) dl;
    return ((anjay_coap_download_ctx_t *) ctx)->socket;
}

static size_t get_max_acceptable_block_size(size_t in_buffer_size) {
    size_t estimated_response_header_size =
            AVS_COAP_MAX_HEADER_SIZE
            + AVS_COAP_MAX_TOKEN_LENGTH
            + AVS_COAP_OPT_ETAG_MAX_SIZE
            + AVS_COAP_OPT_BLOCK_MAX_SIZE
            + 1; // payload marker
    size_t payload_capacity = in_buffer_size - estimated_response_header_size;
    size_t block_size =
            _anjay_max_power_of_2_not_greater_than(payload_capacity);

    if (block_size > AVS_COAP_MSG_BLOCK_MAX_SIZE) {
        block_size = AVS_COAP_MSG_BLOCK_MAX_SIZE;
    }

    dl_log(TRACE, "input buffer size: %zu; max acceptable block size: %zu",
           in_buffer_size, block_size);
    return block_size;
}

#ifdef ANJAY_TEST
#include "test/downloader_mock.h"
#endif // ANJAY_TEST

AVS_LIST(anjay_download_ctx_t)
_anjay_downloader_coap_ctx_new(anjay_downloader_t *dl,
                               const anjay_download_config_t *cfg,
                               uintptr_t id) {
    anjay_t *anjay = _anjay_downloader_get_anjay(dl);
    AVS_LIST(anjay_coap_download_ctx_t) ctx =
            AVS_LIST_NEW_ELEMENT(anjay_coap_download_ctx_t);
    if (!ctx) {
        dl_log(ERROR, "out of memory");
        return NULL;
    }

    static const anjay_download_ctx_vtable_t VTABLE = {
        .get_socket = get_coap_socket,
        .handle_packet = handle_coap_message,
        .cleanup = cleanup_coap_transfer
    };
    ctx->common.vtable = &VTABLE;

    if (_anjay_parse_url(cfg->url, &ctx->uri)) {
        dl_log(ERROR, "invalid URL: %s", cfg->url);
        goto error;
    }

    if (!cfg->on_next_block || !cfg->on_download_finished) {
        dl_log(ERROR, "invalid download config: handlers not set up");
        goto error;
    }

    avs_net_ssl_configuration_t ssl_config = {
        .version = anjay->dtls_version,
        .backend_configuration = anjay->udp_socket_config,
        .security = cfg->security_info
    };
    ssl_config.backend_configuration.reuse_addr = 1;

    avs_net_socket_type_t socket_type;
    const void *config;
    if (!avs_strcasecmp(ctx->uri.protocol, "coap")) {
        socket_type = AVS_NET_UDP_SOCKET;
        config = (const void *) &ssl_config.backend_configuration;
    } else if (!avs_strcasecmp(ctx->uri.protocol, "coaps")) {
        socket_type = AVS_NET_DTLS_SOCKET;
        config = (const void *) &ssl_config;
    } else {
        dl_log(ERROR, "unsupported protocol: %s", ctx->uri.protocol);
        goto error;
    }

    // Downloader sockets MUST NOT reuse the same local port as LwM2M sockets.
    // If they do, and the client attempts to download anything from the same
    // host:port as is used by an LwM2M server, we will get two sockets with
    // identical local/remote host/port tuples. Depending on the socket
    // implementation, we may not be able to create such socket, packets might
    // get duplicated between these "identical" sockets, or we may get some
    // kind of load-balancing behavior. In the last case, the client would
    // randomly handle or ignore LwM2M requests and CoAP download responses.
    ctx->socket = _anjay_create_connected_udp_socket(anjay, socket_type, NULL,
                                                     config, &ctx->uri);
    if (!ctx->socket) {
        dl_log(ERROR, "could not create CoAP socket");
        goto error;
    }

    ctx->common.id = id;
    ctx->common.on_next_block = cfg->on_next_block;
    ctx->common.on_download_finished = cfg->on_download_finished;
    ctx->common.user_data = cfg->user_data;
    ctx->bytes_downloaded = cfg->start_offset;
    ctx->block_size = get_max_acceptable_block_size(anjay->in_buffer_size);
    ctx->etag = cfg->etag;

    if (_anjay_sched_now(anjay->sched, &ctx->sched_job,
                         request_next_coap_block_job,
                         (void *) ctx->common.id)) {
        dl_log(ERROR, "could not schedule download job");
        goto error;
    }

    return (AVS_LIST(anjay_download_ctx_t)) ctx;
error:
    cleanup_coap_transfer(dl, (AVS_LIST(anjay_download_ctx_t) *) &ctx);
    return NULL;
}

#ifdef ANJAY_TEST
#include "test/downloader.c"
#endif // ANJAY_TEST
