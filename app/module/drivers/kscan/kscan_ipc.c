/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * kscan IPC driver (ARCH_POSIX / native_sim only)
 *
 * Opens a Unix domain socket server and feeds key events received from a
 * connected client into the ZMK kscan subsystem.
 *
 * Wire format (client → ZMK):
 *   [4-byte big-endian length][nanopb-encoded zmk_ipc_ClientMessage]
 *
 * The ClientMessage wraps a KeyEvent which supports two address formats:
 *
 *   key_pos { row: 0  col: 0 }   ← explicit row / column
 *   position: 5                  ← linear index (row = pos / columns,
 *                                                 col = pos % columns)
 *
 * Example client (Python):
 *   import socket, struct
 *   from zmk_ipc_pb2 import ClientMessage, KeyEvent, KeyPosition
 *   s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
 *   s.connect('/tmp/zmk_kscan_ipc.sock')
 *   def send_key(row, col, pressed):
 *       action = KeyEvent.PRESS if pressed else KeyEvent.RELEASE
 *       msg = ClientMessage(key_event=KeyEvent(
 *           action=action, key_pos=KeyPosition(row=row, col=col)))
 *       data = msg.SerializeToString()
 *       s.sendall(struct.pack('>I', len(data)) + data)
 *   send_key(0, 0, True)
 *   send_key(0, 0, False)
 */

#define DT_DRV_COMPAT zmk_kscan_ipc

#include <string.h>
#include <errno.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <zephyr/device.h>
#include <zephyr/drivers/kscan.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "zmk_ipc.pb.h"
#include "zmk_ipc_framing.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* -------------------------------------------------------------------------
 * Per-instance config (from DTS)
 * ------------------------------------------------------------------------- */

struct kscan_ipc_config {
    const char *socket_path;
    uint32_t    rows;
    uint32_t    columns;
};

/* -------------------------------------------------------------------------
 * Per-instance runtime data
 * ------------------------------------------------------------------------- */

struct kscan_ipc_data {
    kscan_callback_t  callback;
    const struct device *dev;

    int server_fd; /* listening socket (-1 = not open) */
    int client_fd; /* current accepted connection (-1 = none) */

    struct k_thread   read_thread;
    k_thread_stack_t *read_stack; /* set in per-instance init wrapper */

    bool enabled;
};

/* -------------------------------------------------------------------------
 * Decode and dispatch a received ClientMessage
 * ------------------------------------------------------------------------- */

static void dispatch_message(const struct device *dev,
                             const zmk_ipc_ClientMessage *msg) {
    struct kscan_ipc_data *data     = dev->data;
    const struct kscan_ipc_config *cfg = dev->config;

    if (msg->which_payload != zmk_ipc_ClientMessage_key_event_tag) {
        LOG_WRN("kscan IPC: unknown ClientMessage payload %d", msg->which_payload);
        return;
    }

    const zmk_ipc_KeyEvent *ev = &msg->payload.key_event;
    bool pressed;

    switch (ev->action) {
    case zmk_ipc_KeyEvent_Action_PRESS:
        pressed = true;
        break;
    case zmk_ipc_KeyEvent_Action_RELEASE:
        pressed = false;
        break;
    default:
        LOG_WRN("kscan IPC: unknown KeyEvent action %d", ev->action);
        return;
    }

    uint32_t row, col;

    switch (ev->which_address) {
    case zmk_ipc_KeyEvent_key_pos_tag:
        row = ev->address.key_pos.row;
        col = ev->address.key_pos.col;
        break;

    case zmk_ipc_KeyEvent_position_tag:
        if (cfg->columns == 0) {
            LOG_ERR("kscan IPC: position event received but columns == 0");
            return;
        }
        row = ev->address.position / cfg->columns;
        col = ev->address.position % cfg->columns;
        break;

    default:
        LOG_WRN("kscan IPC: KeyEvent has no address field");
        return;
    }

    LOG_DBG("kscan IPC event: row=%u col=%u pressed=%d", row, col, (int)pressed);

    if (data->enabled && data->callback) {
        data->callback(dev, row, col, pressed);
    }
}

/* -------------------------------------------------------------------------
 * Read thread
 *
 * Accepts one client at a time and processes incoming protobuf frames
 * (length-prefix + ClientMessage).
 * ------------------------------------------------------------------------- */

static void kscan_ipc_read_thread_func(void *a, void *b, void *c) {
    const struct device *dev        = (const struct device *)a;
    struct kscan_ipc_data *data     = dev->data;
    const struct kscan_ipc_config *cfg = dev->config;

    for (;;) {
        /* Accept a new connection if we don't have one */
        if (data->client_fd < 0) {
            LOG_DBG("kscan IPC: waiting for client on %s", cfg->socket_path);
            int client = accept(data->server_fd, NULL, NULL);
            if (client < 0) {
                if (errno == EINTR) {
                    continue;
                }
                LOG_ERR("kscan IPC: accept() failed (errno=%d)", errno);
                k_sleep(K_MSEC(100));
                continue;
            }
            data->client_fd = client;
            LOG_INF("kscan IPC: client connected (fd=%d)", client);
        }

        /* Blocking receive of one length-prefixed protobuf frame */
        zmk_ipc_ClientMessage msg = zmk_ipc_ClientMessage_init_zero;
        int ret = zmk_ipc_frame_recv(data->client_fd, &msg);

        if (ret == 0) {
            dispatch_message(dev, &msg);
        } else if (ret == -ECONNRESET || ret == -EPIPE) {
            LOG_INF("kscan IPC: client disconnected");
            close(data->client_fd);
            data->client_fd = -1;
        } else if (ret == -EMSGSIZE) {
            LOG_WRN("kscan IPC: oversized frame, closing connection");
            close(data->client_fd);
            data->client_fd = -1;
        } else if (ret == -EBADMSG) {
            LOG_WRN("kscan IPC: decode error, skipping frame");
            /* Keep the connection — the stream may still be valid */
        } else {
            LOG_ERR("kscan IPC: recv error %d, closing connection", ret);
            close(data->client_fd);
            data->client_fd = -1;
        }
    }
}

/* -------------------------------------------------------------------------
 * kscan driver API
 * ------------------------------------------------------------------------- */

static int kscan_ipc_configure(const struct device *dev, kscan_callback_t callback) {
    struct kscan_ipc_data *data = dev->data;

    if (!callback) {
        return -EINVAL;
    }
    data->callback = callback;
    return 0;
}

static int kscan_ipc_enable_callback(const struct device *dev) {
    struct kscan_ipc_data *data = dev->data;

    data->enabled = true;
    return 0;
}

static int kscan_ipc_disable_callback(const struct device *dev) {
    struct kscan_ipc_data *data = dev->data;

    data->enabled = false;
    return 0;
}

static const struct kscan_driver_api kscan_ipc_driver_api = {
    .config           = kscan_ipc_configure,
    .enable_callback  = kscan_ipc_enable_callback,
    .disable_callback = kscan_ipc_disable_callback,
};

/* -------------------------------------------------------------------------
 * Common init helper (called from per-instance wrapper)
 * ------------------------------------------------------------------------- */

static int kscan_ipc_init(const struct device *dev) {
    struct kscan_ipc_data *data        = dev->data;
    const struct kscan_ipc_config *cfg = dev->config;

    data->dev       = dev;
    data->client_fd = -1;
    data->server_fd = -1;
    data->enabled   = false;
    data->callback  = NULL;

    data->server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (data->server_fd < 0) {
        LOG_ERR("kscan IPC: socket() failed (errno=%d)", errno);
        return -errno;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, cfg->socket_path, sizeof(addr.sun_path) - 1);

    unlink(cfg->socket_path);

    if (bind(data->server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERR("kscan IPC: bind() failed (errno=%d)", errno);
        close(data->server_fd);
        data->server_fd = -1;
        return -errno;
    }

    if (listen(data->server_fd, 1) < 0) {
        LOG_ERR("kscan IPC: listen() failed (errno=%d)", errno);
        close(data->server_fd);
        data->server_fd = -1;
        return -errno;
    }

    LOG_INF("kscan IPC: listening on %s (protobuf/length-prefix framing)",
            cfg->socket_path);

    k_thread_create(&data->read_thread,
                    data->read_stack,
                    CONFIG_ZMK_KSCAN_IPC_THREAD_STACK_SIZE,
                    kscan_ipc_read_thread_func,
                    (void *)dev, NULL, NULL,
                    K_LOWEST_APPLICATION_THREAD_PRIO, 0, K_NO_WAIT);

    k_thread_name_set(&data->read_thread, "kscan_ipc");

    return 0;
}

/* -------------------------------------------------------------------------
 * Per-DT-instance instantiation macro
 * ------------------------------------------------------------------------- */

#define KSCAN_IPC_INST_INIT(n)                                                              \
    K_THREAD_STACK_DEFINE(kscan_ipc_read_stack_##n,                                         \
                          CONFIG_ZMK_KSCAN_IPC_THREAD_STACK_SIZE);                          \
                                                                                            \
    static struct kscan_ipc_data kscan_ipc_data_##n;                                        \
                                                                                            \
    static const struct kscan_ipc_config kscan_ipc_config_##n = {                           \
        .socket_path = DT_INST_PROP(n, socket_path),                                        \
        .rows        = DT_INST_PROP(n, rows),                                               \
        .columns     = DT_INST_PROP(n, columns),                                            \
    };                                                                                      \
                                                                                            \
    static int kscan_ipc_init_##n(const struct device *dev) {                               \
        struct kscan_ipc_data *data = dev->data;                                            \
        data->read_stack = kscan_ipc_read_stack_##n;                                        \
        return kscan_ipc_init(dev);                                                         \
    }                                                                                       \
                                                                                            \
    DEVICE_DT_INST_DEFINE(n, kscan_ipc_init_##n, NULL,                                     \
                          &kscan_ipc_data_##n, &kscan_ipc_config_##n,                       \
                          POST_KERNEL, CONFIG_KSCAN_INIT_PRIORITY,                          \
                          &kscan_ipc_driver_api);

DT_INST_FOREACH_STATUS_OKAY(KSCAN_IPC_INST_INIT)
