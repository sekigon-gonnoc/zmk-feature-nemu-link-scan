/* SPDX-License-Identifier: MIT */

#include <errno.h>

#include <zephyr/kernel.h>

#include <zmk/dynamic_settings/store.h>

/* Keep generated dynamic-settings references linkable when full scanning is
 * disabled by Kconfig but its settings schema remains in the devicetree. */
int nemu_link_scan_command_changed(const struct dynamic_setting_descriptor *descriptor,
                                   const struct dynamic_setting_value *new_value,
                                   enum dynamic_setting_change_reason reason, const void *context) {
    (void)descriptor;
    (void)new_value;
    (void)reason;
    (void)context;
    return -ENOTSUP;
}

int nemu_link_scan_read(const struct dynamic_setting_descriptor *descriptor,
                        struct dynamic_setting_value *value, const void *context) {
    (void)descriptor;
    (void)value;
    (void)context;
    return -ENOTSUP;
}
