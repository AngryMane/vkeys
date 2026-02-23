/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "zmk_ipc_framing.h"

#include <pb_encode.h>
#include <pb_decode.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>

#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* -------------------------------------------------------------------------
 * Internal helper: read exactly `len` bytes from fd (blocking)
 * ------------------------------------------------------------------------- */

static int recv_exact(int fd, uint8_t *buf, size_t len) {
    size_t received = 0;

    while (received < len) {
        ssize_t n = recv(fd, buf + received, len - received, 0);
        if (n == 0) {
            return -ECONNRESET; /* peer closed connection */
        }
        if (n < 0) {
            return -errno;
        }
        received += (size_t)n;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

int zmk_ipc_encode_event(const zmk_ipc_ZmkEvent *event,
                         uint8_t *buf, size_t buf_size, size_t *out_len) {
    pb_ostream_t stream = pb_ostream_from_buffer(buf, buf_size);

    if (!pb_encode(&stream, &zmk_ipc_ZmkEvent_msg, event)) {
        LOG_ERR("zmk_ipc: pb_encode ZmkEvent failed: %s", PB_GET_ERROR(&stream));
        return -EIO;
    }

    *out_len = stream.bytes_written;
    return 0;
}

int zmk_ipc_frame_send(int fd, const uint8_t *data, size_t len) {
    /*
     * Build a single contiguous buffer: [4-byte BE length][payload]
     * This avoids the race where a partial write of the length prefix
     * could be interleaved with another sender's data.
     */
    uint8_t frame[ZMK_IPC_EVENT_FRAME_MAX];

    if (len + 4 > sizeof(frame)) {
        return -EMSGSIZE;
    }

    uint32_t be_len = sys_cpu_to_be32((uint32_t)len);
    memcpy(frame, &be_len, 4);
    memcpy(frame + 4, data, len);

    ssize_t sent = send(fd, frame, len + 4, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (sent < 0) {
        return -errno;
    }
    if ((size_t)sent != len + 4) {
        /* Partial send — treat as error to avoid corrupted stream */
        return -EIO;
    }
    return 0;
}

int zmk_ipc_frame_recv(int fd, zmk_ipc_ClientMessage *msg) {
    /* Step 1: read 4-byte big-endian length */
    uint8_t len_buf[4];
    int ret = recv_exact(fd, len_buf, sizeof(len_buf));
    if (ret != 0) {
        return ret;
    }

    uint32_t msg_len = sys_be32_to_cpu(*(uint32_t *)len_buf);
    if (msg_len > zmk_ipc_ClientMessage_size) {
        LOG_WRN("zmk_ipc: incoming frame too large: %" PRIu32 " > %u",
                msg_len, (unsigned)zmk_ipc_ClientMessage_size);
        return -EMSGSIZE;
    }

    /* Step 2: read exactly msg_len bytes */
    uint8_t body[zmk_ipc_ClientMessage_size];
    ret = recv_exact(fd, body, (size_t)msg_len);
    if (ret != 0) {
        return ret;
    }

    /* Step 3: nanopb decode */
    pb_istream_t stream = pb_istream_from_buffer(body, (size_t)msg_len);
    *msg = (zmk_ipc_ClientMessage)zmk_ipc_ClientMessage_init_zero;

    if (!pb_decode(&stream, &zmk_ipc_ClientMessage_msg, msg)) {
        LOG_WRN("zmk_ipc: pb_decode ClientMessage failed: %s", PB_GET_ERROR(&stream));
        return -EBADMSG;
    }

    return 0;
}
