/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Length-prefix framing for nanopb-encoded IPC messages.
 *
 * Wire format (same for both directions):
 *
 *   ┌──────────────────────┬─────────────────────────────────┐
 *   │ 4 bytes, big-endian  │ <length> bytes                  │
 *   │ encoded message size │ nanopb-encoded protobuf message  │
 *   └──────────────────────┴─────────────────────────────────┘
 *
 * The framing layer is intentionally transport-agnostic; the caller
 * supplies a connected file descriptor (Unix socket, TCP socket, etc.).
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "zmk_ipc.pb.h"

/* Maximum encoded sizes (from generated pb.h) */
#define ZMK_IPC_EVENT_FRAME_MAX   (4U + zmk_ipc_ZmkEvent_size)    /* ZMK → client */
#define ZMK_IPC_MSG_FRAME_MAX     (4U + zmk_ipc_ClientMessage_size) /* client → ZMK */

/**
 * @brief Encode @p event into @p buf (no length prefix).
 *
 * @param event     Message to encode.
 * @param buf       Output buffer (at least zmk_ipc_ZmkEvent_size bytes).
 * @param buf_size  Size of @p buf.
 * @param out_len   Set to the number of bytes written on success.
 * @retval 0 on success, -EIO on nanopb encode failure.
 */
int zmk_ipc_encode_event(const zmk_ipc_ZmkEvent *event,
                         uint8_t *buf, size_t buf_size, size_t *out_len);

/**
 * @brief Send a length-prefixed protobuf frame to @p fd (non-blocking).
 *
 * @p data must be the already-encoded protobuf bytes (no length prefix).
 * The function prepends the 4-byte big-endian length and sends both in
 * a single write to avoid partial-frame interleaving.
 *
 * Uses MSG_NOSIGNAL | MSG_DONTWAIT — returns -EAGAIN if the socket
 * send buffer is full (caller should close the connection).
 *
 * @retval 0 on success, negative errno on failure.
 */
int zmk_ipc_frame_send(int fd, const uint8_t *data, size_t len);

/**
 * @brief Blocking receive and decode of a ClientMessage frame from @p fd.
 *
 * Reads exactly 4 bytes (length), then exactly @p length bytes (body),
 * then decodes the body as a zmk_ipc_ClientMessage.
 *
 * @retval 0 on success.
 * @retval -ECONNRESET if the peer closed the connection.
 * @retval -EMSGSIZE   if the reported length exceeds the max message size.
 * @retval -EBADMSG    if nanopb decoding failed.
 * @retval negative errno for other recv errors.
 */
int zmk_ipc_frame_recv(int fd, zmk_ipc_ClientMessage *msg);
