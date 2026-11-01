/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <zmk/dynamic_settings/store.h>
#include <zmk/nemu_link/hop_radio.h>

LOG_MODULE_REGISTER(nemu_link_full_scan, LOG_LEVEL_INF);

#define SCAN_CHANNEL_COUNT NEMU_LINK_FULL_SCAN_CHANNEL_COUNT
#define SCAN_RETRY_MS 5
#define SCAN_CHANNEL_GAP_MS 20

struct channel_stats {
    int8_t minimum;
    int8_t maximum;
    int32_t sum;
    uint16_t count;
};

static struct channel_stats channel_stats[SCAN_CHANNEL_COUNT];
static struct k_spinlock scan_lock;
static bool scan_running;
static uint8_t current_channel;
static int64_t scan_deadline;

static void scan_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(scan_work, scan_work_handler);

static bool parse_channel_name(const char *name, uint8_t *channel) {
    static const char prefix[] = "channel-";
    if (!name || !channel || strncmp(name, prefix, sizeof(prefix) - 1) != 0) {
        return false;
    }

    const char *digit = name + sizeof(prefix) - 1;
    if (*digit == '\0') {
        return false;
    }
    unsigned int value = 0;
    for (; *digit; digit++) {
        if (*digit < '0' || *digit > '9') {
            return false;
        }
        value = value * 10U + (unsigned int)(*digit - '0');
        if (value >= SCAN_CHANNEL_COUNT) {
            return false;
        }
    }
    *channel = value;
    return true;
}

static void record_channel(uint8_t channel, int8_t minimum, int8_t maximum, int8_t mean,
                           uint16_t samples) {
    if (!samples) {
        return;
    }
    k_spinlock_key_t key = k_spin_lock(&scan_lock);
    struct channel_stats *stats = &channel_stats[channel];
    if (!stats->count) {
        stats->minimum = minimum;
        stats->maximum = maximum;
    } else {
        if (minimum < stats->minimum) {
            stats->minimum = minimum;
        }
        if (maximum > stats->maximum) {
            stats->maximum = maximum;
        }
    }
    stats->sum += mean;
    if (stats->count < UINT16_MAX) {
        stats->count++;
    }
    k_spin_unlock(&scan_lock, key);
}

static void scan_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    k_spinlock_key_t key = k_spin_lock(&scan_lock);
    bool running = scan_running;
    uint8_t channel = current_channel;
    int64_t deadline = scan_deadline;
    k_spin_unlock(&scan_lock, key);
    if (!running) {
        return;
    }
    if (k_uptime_get() >= deadline) {
        key = k_spin_lock(&scan_lock);
        scan_running = false;
        k_spin_unlock(&scan_lock, key);
        LOG_INF("Full-band scan complete");
        return;
    }

    int8_t minimum;
    int8_t maximum;
    int8_t mean;
    uint16_t samples;
    int err = nemu_link_radio_scan_frequency(channel, &minimum, &maximum, &mean, &samples);
    if (err == -EBUSY) {
        k_work_reschedule(&scan_work, K_MSEC(SCAN_RETRY_MS));
        return;
    }
    if (err) {
        key = k_spin_lock(&scan_lock);
        scan_running = false;
        k_spin_unlock(&scan_lock, key);
        LOG_WRN("Full-band scan stopped at channel %u: %d", channel, err);
        return;
    }

    record_channel(channel, minimum, maximum, mean, samples);
    key = k_spin_lock(&scan_lock);
    current_channel = (uint8_t)((channel + 1U) % SCAN_CHANNEL_COUNT);
    k_spin_unlock(&scan_lock, key);
    k_work_reschedule(&scan_work, K_MSEC(SCAN_CHANNEL_GAP_MS));
}

int nemu_link_scan_command_changed(const struct dynamic_setting_descriptor *descriptor,
                                   const struct dynamic_setting_value *new_value,
                                   enum dynamic_setting_change_reason reason, const void *context) {
    ARG_UNUSED(descriptor);
    ARG_UNUSED(context);
    if (reason != DYNAMIC_SETTING_CHANGE_WRITE_TEMP || !new_value || !new_value->length ||
        new_value->data[0] == 0) {
        return 0;
    }

    k_spinlock_key_t key = k_spin_lock(&scan_lock);
    if (scan_running) {
        k_spin_unlock(&scan_lock, key);
        return -EBUSY;
    }
    memset(channel_stats, 0, sizeof(channel_stats));
    current_channel = 0;
    scan_deadline = k_uptime_get() + CONFIG_NEMU_LINK_FULL_SCAN_DURATION_MS;
    scan_running = true;
    k_spin_unlock(&scan_lock, key);
    k_work_reschedule(&scan_work, K_NO_WAIT);
    LOG_INF("Full-band scan started for %d ms", CONFIG_NEMU_LINK_FULL_SCAN_DURATION_MS);
    return 0;
}

int nemu_link_scan_read(const struct dynamic_setting_descriptor *descriptor,
                        struct dynamic_setting_value *value, const void *context) {
    ARG_UNUSED(context);
    if (!descriptor || !value || descriptor->type != DYNAMIC_SETTING_TYPE_U32) {
        return -EINVAL;
    }
    value->type = descriptor->type;
    value->length = sizeof(uint32_t);

    uint8_t channel;
    if (!parse_channel_name(descriptor->name, &channel)) {
        return -ENOENT;
    }

    /* The Studio RPC reader must not block on a work item retuning the radio. */
    const struct channel_stats stats = channel_stats[channel];
    uint32_t packed = UINT32_MAX;
    if (stats.count) {
        int32_t mean = stats.sum / stats.count;
        uint8_t count = stats.count > UINT8_MAX ? UINT8_MAX : (uint8_t)stats.count;
        packed = (uint32_t)(uint8_t)stats.minimum | ((uint32_t)(uint8_t)stats.maximum << 8) |
                 ((uint32_t)(uint8_t)mean << 16) | ((uint32_t)count << 24);
    }
    sys_put_le32(packed, value->data);
    return 0;
}
