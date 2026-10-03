/*
 * Copyright (c) 2021 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Kinesis Advantage 2 indicator LEDs.
 *
 * A single thread owns the LEDs. Event listeners only post small messages to
 * its queue, so the (slow, sleep-based) animations never block the key event
 * path and the LED state is never touched from two threads at once.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <dt-bindings/zmk/hid_usage.h>
#include <dt-bindings/zmk/hid_usage_pages.h>

#include <zmk/battery.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

enum led_idx { LED_CAP, LED_NUM, LED_SCR, LED_KEY, LED_COUNT };

static const struct gpio_dt_spec leds[LED_COUNT] = {
    [LED_CAP] = GPIO_DT_SPEC_GET(DT_NODELABEL(ledu1), gpios),
    [LED_NUM] = GPIO_DT_SPEC_GET(DT_NODELABEL(ledu2), gpios),
    [LED_SCR] = GPIO_DT_SPEC_GET(DT_NODELABEL(ledu3), gpios),
    [LED_KEY] = GPIO_DT_SPEC_GET(DT_NODELABEL(ledu4), gpios),
};

/* Lock LED states; only read or written by the LED thread. */
static bool lock_state[LED_COUNT];

enum led_cmd { CMD_SHOW_BATTERY, CMD_SHOW_VALUE, CMD_TOGGLE_LOCK };

struct led_msg {
    uint8_t cmd;
    uint8_t arg;
};

K_MSGQ_DEFINE(led_msgq, sizeof(struct led_msg), 8, 1);

/* Battery readout timing */
#define BATTERY_STEP_MS 50
#define BATTERY_HOLD_MS 500
#define BATTERY_LOW_BLINK_MS 200
#define BATTERY_LOW_BLINKS 5
#define BATTERY_LOW_LEVEL 10
/* Requests arriving within this long of the last readout are ignored. */
#define BATTERY_COOLDOWN_MS 1000
/* Give the battery driver time to take its first sample before the boot readout. */
#define BATTERY_BOOT_DELAY_MS 1500

/* Profile / layer readout timing */
#define VALUE_GAP_MS 300
#define VALUE_HOLD_MS 300

/* Set while a battery readout is queued or playing, so repeat requests are dropped. */
static atomic_t battery_busy;
static int64_t battery_last_shown_ms = -BATTERY_COOLDOWN_MS;

static void set_led(enum led_idx idx, bool on) { gpio_pin_set_dt(&leds[idx], on); }

static void all_off(void) {
    for (int i = 0; i < LED_COUNT; i++) {
        set_led(i, false);
    }
}

static void restore_lock_states(void) {
    for (int i = 0; i < LED_COUNT; i++) {
        set_led(i, lock_state[i]);
    }
}

static void show_battery(void) {
    uint8_t level = zmk_battery_state_of_charge();
    LOG_DBG("Displaying battery level %d", level);

    all_off();
    k_msleep(BATTERY_STEP_MS);

    if (level <= BATTERY_LOW_LEVEL) {
        for (int i = 0; i < BATTERY_LOW_BLINKS; i++) {
            set_led(LED_CAP, true);
            k_msleep(BATTERY_LOW_BLINK_MS);
            set_led(LED_CAP, false);
            k_msleep(BATTERY_LOW_BLINK_MS);
        }
    } else {
        /* One LED per quarter of charge, lit left to right. */
        const uint8_t thresholds[LED_COUNT] = {0, 25, 50, 75};
        for (int i = 0; i < LED_COUNT; i++) {
            if (level > thresholds[i]) {
                set_led(i, true);
                k_msleep(BATTERY_STEP_MS);
            }
        }
        k_msleep(BATTERY_HOLD_MS);
    }

    restore_lock_states();
}

/* Show a 1-based profile or layer number: LEDs 1-4 for 1-4, all four for 5 and up. */
static void show_value(uint8_t value) {
    all_off();
    k_msleep(VALUE_GAP_MS);

    if (value >= 1 && value <= LED_COUNT) {
        set_led(value - 1, true);
    } else if (value > LED_COUNT) {
        for (int i = 0; i < LED_COUNT; i++) {
            set_led(i, true);
        }
    }

    k_msleep(VALUE_HOLD_MS);
    restore_lock_states();
}

static void post(enum led_cmd cmd, uint8_t arg) {
    struct led_msg msg = {.cmd = cmd, .arg = arg};

    if (k_msgq_put(&led_msgq, &msg, K_NO_WAIT) != 0) {
        LOG_WRN("LED queue full, dropping command %d", cmd);
        if (cmd == CMD_SHOW_BATTERY) {
            atomic_clear(&battery_busy);
        }
    }
}

static void request_battery(void) {
    if (atomic_cas(&battery_busy, 0, 1)) {
        post(CMD_SHOW_BATTERY, 0);
    }
}

static void led_thread(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    for (int i = 0; i < LED_COUNT; i++) {
        if (!gpio_is_ready_dt(&leds[i])) {
            LOG_ERR("LED %d GPIO controller not ready", i);
            return;
        }
        int ret = gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE);
        if (ret != 0) {
            LOG_ERR("Failed to configure LED %d: %d", i, ret);
            return;
        }
    }

    /* ZMK's deep sleep wakes via a full reset, so this also covers "show battery on wake". */
    k_msleep(BATTERY_BOOT_DELAY_MS);
    request_battery();

    struct led_msg msg;
    while (true) {
        k_msgq_get(&led_msgq, &msg, K_FOREVER);

        switch (msg.cmd) {
        case CMD_SHOW_BATTERY:
            if (k_uptime_get() - battery_last_shown_ms >= BATTERY_COOLDOWN_MS) {
                show_battery();
                battery_last_shown_ms = k_uptime_get();
            }
            atomic_clear(&battery_busy);
            break;
        case CMD_SHOW_VALUE:
            show_value(msg.arg);
            break;
        case CMD_TOGGLE_LOCK:
            lock_state[msg.arg] = !lock_state[msg.arg];
            set_led(msg.arg, lock_state[msg.arg]);
            break;
        }
    }
}

K_THREAD_DEFINE(kinesis_leds_thread, 1024, led_thread, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

static void on_keycode_pressed(const struct zmk_keycode_state_changed *ev) {
    if (ev->usage_page != HID_USAGE_KEY) {
        return;
    }

    switch (ev->keycode) {
    case HID_USAGE_KEY_KEYBOARD_F24:
        request_battery();
        break;
    case HID_USAGE_KEY_KEYBOARD_CAPS_LOCK:
        post(CMD_TOGGLE_LOCK, LED_CAP);
        break;
    case HID_USAGE_KEY_KEYBOARD_SCROLL_LOCK:
        post(CMD_TOGGLE_LOCK, LED_SCR);
        break;
    case HID_USAGE_KEY_KEYPAD_NUM_LOCK_AND_CLEAR:
        post(CMD_TOGGLE_LOCK, LED_NUM);
        break;
    }
}

static int kinesis_leds_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *key_ev = as_zmk_keycode_state_changed(eh);
    if (key_ev != NULL) {
        if (key_ev->state) {
            on_keycode_pressed(key_ev);
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

    const struct zmk_ble_active_profile_changed *ble_ev = as_zmk_ble_active_profile_changed(eh);
    if (ble_ev != NULL) {
        post(CMD_SHOW_VALUE, ble_ev->index + 1);
        return ZMK_EV_EVENT_BUBBLE;
    }

    const struct zmk_layer_state_changed *layer_ev = as_zmk_layer_state_changed(eh);
    if (layer_ev != NULL && layer_ev->state) {
        post(CMD_SHOW_VALUE, layer_ev->layer + 1);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(kinesis_leds, kinesis_leds_listener);
ZMK_SUBSCRIPTION(kinesis_leds, zmk_keycode_state_changed);
ZMK_SUBSCRIPTION(kinesis_leds, zmk_ble_active_profile_changed);
ZMK_SUBSCRIPTION(kinesis_leds, zmk_layer_state_changed);
