/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * ZMK IPC Observer  (native_sim / ARCH_POSIX only)
 *
 * Opens a Unix domain socket server and broadcasts protobuf-encoded
 * zmk_ipc_ZmkEvent messages (length-prefix framing) to all connected
 * clients for two event categories:
 *
 *   KscanEvent        – zmk_position_state_changed (before keymap processing)
 *   HidKeyboardReport – keyboard HID report, fired at zmk_endpoint_send_report
 *   HidConsumerReport – consumer HID report
 *   HidMouseReport    – mouse HID report (CONFIG_ZMK_POINTING)
 *
 * Wire format: [4-byte big-endian length][nanopb-encoded ZmkEvent]
 *
 * Example client (Python):
 *   import socket, struct
 *   from zmk_ipc_pb2 import ZmkEvent
 *   s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
 *   s.connect('/tmp/zmk_ipc.sock')
 *   while True:
 *       length = struct.unpack('>I', s.recv(4))[0]
 *       ev = ZmkEvent(); ev.ParseFromString(s.recv(length))
 *       print(ev)
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/hid.h>
#include <zmk/ipc_observer.h>

#include "zmk_ipc.pb.h"
#include "zmk_ipc_framing.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

LOG_MODULE_REGISTER(zmk_ipc_observer, CONFIG_ZMK_IPC_OBSERVER_LOG_LEVEL);

#define MAX_CLIENTS CONFIG_ZMK_IPC_OBSERVER_MAX_CLIENTS

static int server_fd = -1;
static int client_fds[MAX_CLIENTS];
static K_MUTEX_DEFINE(clients_mutex);

/* -------------------------------------------------------------------------
 * Internal broadcast helper
 * Encodes event once, then sends the same frame to every connected client.
 * Clients that fail to receive are closed and removed.
 * ------------------------------------------------------------------------- */

static void broadcast_event(const zmk_ipc_ZmkEvent *event) {
    uint8_t buf[zmk_ipc_ZmkEvent_size];
    size_t encoded_len;

    if (zmk_ipc_encode_event(event, buf, sizeof(buf), &encoded_len) != 0) {
        return; /* encode error already logged inside helper */
    }

    k_mutex_lock(&clients_mutex, K_FOREVER);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_fds[i] < 0) {
            continue;
        }
        int ret = zmk_ipc_frame_send(client_fds[i], buf, encoded_len);
        if (ret != 0) {
            LOG_DBG("IPC observer: client fd[%d] dropped (err %d)", i, ret);
            close(client_fds[i]);
            client_fds[i] = -1;
        }
    }
    k_mutex_unlock(&clients_mutex);
}

/* -------------------------------------------------------------------------
 * Build Endpoint sub-message from a transport string produced by
 * zmk_endpoint_instance_to_str()  (e.g. "USB", "BLE:0", "None")
 * ------------------------------------------------------------------------- */

static zmk_ipc_Endpoint endpoint_from_str(const char *transport_str) {
    zmk_ipc_Endpoint ep = zmk_ipc_Endpoint_init_zero;

    if (strncmp(transport_str, "USB", 3) == 0) {
        ep.transport = zmk_ipc_TransportType_TRANSPORT_USB;
    } else if (strncmp(transport_str, "BLE:", 4) == 0) {
        ep.transport        = zmk_ipc_TransportType_TRANSPORT_BLE;
        ep.ble_profile_idx  = (uint32_t)atoi(transport_str + 4);
    } else {
        ep.transport = zmk_ipc_TransportType_TRANSPORT_NONE;
    }

    return ep;
}

/* -------------------------------------------------------------------------
 * Public notification functions (called from endpoints.c)
 * ------------------------------------------------------------------------- */

void zmk_ipc_observer_notify_keyboard_report(const char *transport_str) {
    struct zmk_hid_keyboard_report *report = zmk_hid_get_keyboard_report();
    const size_t keys_size = sizeof(report->body.keys);

    zmk_ipc_HidKeyboardReport kb = zmk_ipc_HidKeyboardReport_init_zero;
    kb.has_endpoint  = true;
    kb.endpoint      = endpoint_from_str(transport_str);
    kb.modifiers     = report->body.modifiers;
    kb.keys.size     = (pb_size_t)MIN(keys_size, sizeof(kb.keys.bytes));
    memcpy(kb.keys.bytes, report->body.keys, kb.keys.size);

    zmk_ipc_ZmkEvent ev     = zmk_ipc_ZmkEvent_init_zero;
    ev.which_payload        = zmk_ipc_ZmkEvent_keyboard_tag;
    ev.payload.keyboard     = kb;

    broadcast_event(&ev);
}

void zmk_ipc_observer_notify_consumer_report(const char *transport_str) {
    struct zmk_hid_consumer_report *report = zmk_hid_get_consumer_report();
    const size_t keys_size = sizeof(report->body.keys);

    zmk_ipc_HidConsumerReport cr = zmk_ipc_HidConsumerReport_init_zero;
    cr.has_endpoint  = true;
    cr.endpoint      = endpoint_from_str(transport_str);
    cr.keys.size     = (pb_size_t)MIN(keys_size, sizeof(cr.keys.bytes));
    memcpy(cr.keys.bytes, report->body.keys, cr.keys.size);

    zmk_ipc_ZmkEvent ev     = zmk_ipc_ZmkEvent_init_zero;
    ev.which_payload        = zmk_ipc_ZmkEvent_consumer_tag;
    ev.payload.consumer     = cr;

    broadcast_event(&ev);
}

#if IS_ENABLED(CONFIG_ZMK_POINTING)
void zmk_ipc_observer_notify_mouse_report(const char *transport_str) {
    struct zmk_hid_mouse_report *report = zmk_hid_get_mouse_report();

    zmk_ipc_HidMouseReport mr = zmk_ipc_HidMouseReport_init_zero;
    mr.has_endpoint  = true;
    mr.endpoint      = endpoint_from_str(transport_str);
    mr.buttons       = report->body.buttons;
    mr.dx            = report->body.d_x;
    mr.dy            = report->body.d_y;
    mr.scroll_x      = report->body.d_scroll_x;
    mr.scroll_y      = report->body.d_scroll_y;

    zmk_ipc_ZmkEvent ev  = zmk_ipc_ZmkEvent_init_zero;
    ev.which_payload     = zmk_ipc_ZmkEvent_mouse_tag;
    ev.payload.mouse     = mr;

    broadcast_event(&ev);
}
#endif /* IS_ENABLED(CONFIG_ZMK_POINTING) */

/* -------------------------------------------------------------------------
 * keyscan (position_state_changed) event listener
 * ------------------------------------------------------------------------- */

static int ipc_position_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *pos = as_zmk_position_state_changed(eh);
    if (!pos) {
        return 0;
    }

    zmk_ipc_KscanEvent kscan = zmk_ipc_KscanEvent_init_zero;
    kscan.source    = pos->source;
    kscan.position  = pos->position;
    kscan.pressed   = pos->state;
    kscan.timestamp = pos->timestamp;

    zmk_ipc_ZmkEvent ev         = zmk_ipc_ZmkEvent_init_zero;
    ev.which_payload            = zmk_ipc_ZmkEvent_kscan_event_tag;
    ev.payload.kscan_event      = kscan;

    broadcast_event(&ev);
    return 0;
}

ZMK_LISTENER(zmk_ipc_position_listener, ipc_position_listener);
ZMK_SUBSCRIPTION(zmk_ipc_position_listener, zmk_position_state_changed);

/* -------------------------------------------------------------------------
 * Accept thread: blocks on accept(), adds new clients to the list
 * ------------------------------------------------------------------------- */

static void ipc_accept_thread_func(void *a, void *b, void *c) {
    LOG_INF("ZMK IPC observer: waiting for clients on %s",
            CONFIG_ZMK_IPC_OBSERVER_SOCKET_PATH);

    for (;;) {
        int client = accept(server_fd, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG_ERR("IPC observer: accept() failed (errno=%d)", errno);
            k_sleep(K_MSEC(100));
            continue;
        }

        LOG_INF("IPC observer: client connected (fd=%d)", client);

        bool accepted = false;
        k_mutex_lock(&clients_mutex, K_FOREVER);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (client_fds[i] < 0) {
                client_fds[i] = client;
                accepted = true;
                break;
            }
        }
        k_mutex_unlock(&clients_mutex);

        if (!accepted) {
            LOG_WRN("IPC observer: max clients (%d) reached, rejecting", MAX_CLIENTS);
            close(client);
        }
    }
}

K_THREAD_DEFINE(zmk_ipc_accept_thread,
                CONFIG_ZMK_IPC_OBSERVER_THREAD_STACK_SIZE,
                ipc_accept_thread_func,
                NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

/* -------------------------------------------------------------------------
 * Initialisation
 * ------------------------------------------------------------------------- */

static int ipc_observer_init(void) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_fds[i] = -1;
    }

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        LOG_ERR("IPC observer: socket() failed (errno=%d)", errno);
        return -errno;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONFIG_ZMK_IPC_OBSERVER_SOCKET_PATH,
            sizeof(addr.sun_path) - 1);

    unlink(CONFIG_ZMK_IPC_OBSERVER_SOCKET_PATH);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERR("IPC observer: bind() failed (errno=%d)", errno);
        close(server_fd);
        server_fd = -1;
        return -errno;
    }

    if (listen(server_fd, 5) < 0) {
        LOG_ERR("IPC observer: listen() failed (errno=%d)", errno);
        close(server_fd);
        server_fd = -1;
        return -errno;
    }

    LOG_INF("ZMK IPC observer listening on %s (protobuf/length-prefix framing)",
            CONFIG_ZMK_IPC_OBSERVER_SOCKET_PATH);
    return 0;
}

SYS_INIT(ipc_observer_init, APPLICATION, CONFIG_ZMK_IPC_OBSERVER_INIT_PRIORITY);
