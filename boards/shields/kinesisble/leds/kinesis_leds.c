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

#include <dt-bindings/zmk/hid_indicators.h>

#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/events/layer_state_changed.h>

#include "kinesis_leds.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

enum led_idx { LED_CAP, LED_NUM, LED_SCR, LED_KEY, LED_COUNT };

static const struct gpio_dt_spec leds[LED_COUNT] = {
    [LED_CAP] = GPIO_DT_SPEC_GET(DT_NODELABEL(ledu1), gpios),
    [LED_NUM] = GPIO_DT_SPEC_GET(DT_NODELABEL(ledu2), gpios),
    [LED_SCR] = GPIO_DT_SPEC_GET(DT_NODELABEL(ledu3), gpios),
    [LED_KEY] = GPIO_DT_SPEC_GET(DT_NODELABEL(ledu4), gpios),
};

/*
 * Lock LED states as reported by the active host; only read or written by
 * the LED thread. The KEY LED has no host indicator and stays off.
 */
static bool lock_state[LED_COUNT];

enum led_cmd { CMD_SHOW_BATTERY, CMD_SHOW_PROFILE, CMD_FLASH_LAYER, CMD_SET_INDICATORS };

struct led_msg {
    uint8_t cmd;
    uint8_t arg;
    bool flag;
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

/* Profile readout timing: solid when connected, blinking while not connected */
#define PROFILE_GAP_MS 150
#define PROFILE_HOLD_MS 600
#define PROFILE_BLINK_MS 150
#define PROFILE_BLINKS 3

/* Layer flash: briefly invert the layer's LED, then restore */
#define LAYER_FLASH_MS 120

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

/* LEDs for a 1-based profile or layer number: LED N for 1-4, all four for 5 and up. */
static uint8_t number_mask(uint8_t number) {
    if (number == 0) {
        return 0;
    }
    if (number > LED_COUNT) {
        return BIT_MASK(LED_COUNT);
    }
    return BIT(number - 1);
}

static void set_mask(uint8_t mask, bool on) {
    for (int i = 0; i < LED_COUNT; i++) {
        if (mask & BIT(i)) {
            set_led(i, on);
        }
    }
}

static void show_profile(uint8_t number, bool connected) {
    uint8_t mask = number_mask(number);

    all_off();
    k_msleep(PROFILE_GAP_MS);

    if (connected) {
        set_mask(mask, true);
        k_msleep(PROFILE_HOLD_MS);
    } else {
        for (int i = 0; i < PROFILE_BLINKS; i++) {
            set_mask(mask, true);
            k_msleep(PROFILE_BLINK_MS);
            set_mask(mask, false);
            k_msleep(PROFILE_BLINK_MS);
        }
    }

    restore_lock_states();
}

static void flash_layer(uint8_t number) {
    uint8_t mask = number_mask(number);

    for (int i = 0; i < LED_COUNT; i++) {
        if (mask & BIT(i)) {
            set_led(i, !lock_state[i]);
        }
    }
    k_msleep(LAYER_FLASH_MS);
    restore_lock_states();
}

static void post(enum led_cmd cmd, uint8_t arg, bool flag) {
    struct led_msg msg = {.cmd = cmd, .arg = arg, .flag = flag};

    if (k_msgq_put(&led_msgq, &msg, K_NO_WAIT) != 0) {
        LOG_WRN("LED queue full, dropping command %d", cmd);
        if (cmd == CMD_SHOW_BATTERY) {
            atomic_clear(&battery_busy);
        }
    }
}

void kinesis_leds_show_battery(void) {
    if (atomic_cas(&battery_busy, 0, 1)) {
        post(CMD_SHOW_BATTERY, 0, false);
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
    kinesis_leds_show_battery();

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
        case CMD_SHOW_PROFILE:
            show_profile(msg.arg, msg.flag);
            break;
        case CMD_FLASH_LAYER:
            flash_layer(msg.arg);
            break;
        case CMD_SET_INDICATORS:
            lock_state[LED_CAP] = msg.arg & HID_INDICATOR_CAPS_LOCK;
            lock_state[LED_NUM] = msg.arg & HID_INDICATOR_NUM_LOCK;
            lock_state[LED_SCR] = msg.arg & HID_INDICATOR_SCROLL_LOCK;
            restore_lock_states();
            break;
        }
    }
}

K_THREAD_DEFINE(kinesis_leds_thread, 1024, led_thread, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

static int kinesis_leds_listener(const zmk_event_t *eh) {
    /* Raised when the active host sends new lock states, and on endpoint/profile switches. */
    const struct zmk_hid_indicators_changed *ind_ev = as_zmk_hid_indicators_changed(eh);
    if (ind_ev != NULL) {
        post(CMD_SET_INDICATORS, ind_ev->indicators, false);
        return ZMK_EV_EVENT_BUBBLE;
    }

    const struct zmk_ble_active_profile_changed *ble_ev = as_zmk_ble_active_profile_changed(eh);
    if (ble_ev != NULL) {
        /* Also raised when the active profile connects or disconnects. */
        post(CMD_SHOW_PROFILE, ble_ev->index + 1, zmk_ble_active_profile_is_connected());
        return ZMK_EV_EVENT_BUBBLE;
    }

    const struct zmk_layer_state_changed *layer_ev = as_zmk_layer_state_changed(eh);
    if (layer_ev != NULL && layer_ev->state) {
        post(CMD_FLASH_LAYER, layer_ev->layer + 1, false);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(kinesis_leds, kinesis_leds_listener);
ZMK_SUBSCRIPTION(kinesis_leds, zmk_ble_active_profile_changed);
ZMK_SUBSCRIPTION(kinesis_leds, zmk_hid_indicators_changed);
ZMK_SUBSCRIPTION(kinesis_leds, zmk_layer_state_changed);
