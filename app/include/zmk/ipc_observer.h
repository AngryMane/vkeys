/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>

#if IS_ENABLED(CONFIG_ZMK_IPC_OBSERVER)

/**
 * Notify IPC clients that a keyboard HID report is being sent to an endpoint.
 * @param transport_str Human-readable transport name (e.g. "USB", "BLE:0", "None")
 */
void zmk_ipc_observer_notify_keyboard_report(const char *transport_str);

/**
 * Notify IPC clients that a consumer HID report is being sent to an endpoint.
 * @param transport_str Human-readable transport name
 */
void zmk_ipc_observer_notify_consumer_report(const char *transport_str);

#if IS_ENABLED(CONFIG_ZMK_POINTING)
/**
 * Notify IPC clients that a mouse HID report is being sent to an endpoint.
 * @param transport_str Human-readable transport name
 */
void zmk_ipc_observer_notify_mouse_report(const char *transport_str);
#endif /* IS_ENABLED(CONFIG_ZMK_POINTING) */

#else /* !IS_ENABLED(CONFIG_ZMK_IPC_OBSERVER) */

static inline void zmk_ipc_observer_notify_keyboard_report(const char *transport_str) {}
static inline void zmk_ipc_observer_notify_consumer_report(const char *transport_str) {}
#if IS_ENABLED(CONFIG_ZMK_POINTING)
static inline void zmk_ipc_observer_notify_mouse_report(const char *transport_str) {}
#endif

#endif /* IS_ENABLED(CONFIG_ZMK_IPC_OBSERVER) */
