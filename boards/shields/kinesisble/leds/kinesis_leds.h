/*
 * Copyright (c) 2021 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

/* Ask the LED thread to show the battery level. Debounced; safe from any thread. */
void kinesis_leds_show_battery(void);
