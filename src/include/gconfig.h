/**
 * File: gconfig.h
 * Author: Diego Parrilla Santamaría
 * Date: November 2024
 * Copyright: 2024 - GOODDATA LABS SL
 * Description: Header file for the global configuration manager
 */

#ifndef GCONFIG_H
#define GCONFIG_H

#include "constants.h"
#include "debug.h"
#include "settings.h"

#define PARAM_BOOT_FEATURE "BOOT_FEATURE"
#define PARAM_HOSTNAME "HOSTNAME"
#define PARAM_WIFI_AUTH "WIFI_AUTH"
#define PARAM_WIFI_CONNECT_TIMEOUT "WIFI_CONNECT_TIMEOUT"
#define PARAM_WIFI_COUNTRY "WIFI_COUNTRY"
#define PARAM_WIFI_PASSWORD "WIFI_PASSWORD"
#define PARAM_WIFI_SCAN_SECONDS "WIFI_SCAN_SECONDS"
#define PARAM_WIFI_SSID "WIFI_SSID"
#define PARAM_WIFI_MODE "WIFI_MODE"
#define PARAM_MODE "MODE"
#define PARAM_MOUSE_SPEED "MOUSE_SPEED"
#define PARAM_USB_KB_LAYOUT "USB_KB_LAYOUT"
#define PARAM_USB_KB_TYPE "USB_KB_TYPE"
#define PARAM_JOYSTICK_USB "JOYSTICK_USB"
#define PARAM_JOYSTICK_USB_PORT "JOYSTICK_USB_PORT"
#define PARAM_JOYSTICK_USB_AUTOSHOOT "JOYSTICK_USB_SHOOT"
#define PARAM_MOUSE_ORIGINAL "MOUSE_ORIGINAL"
#define PARAM_BT_ENABLED "BT_ENABLED"
#define PARAM_BT_MODE "BT_MODE"
#define PARAM_BT_KEYBOARD "BT_KEYBOARD"
#define PARAM_BT_MOUSE "BT_MOUSE"
#define PARAM_BT_GAMEPAD "BT_GAMEPAD"
#define PARAM_BT_GAMEPADSHOOT "BT_GAMEPAD_SHOOT"
#define PARAM_BT_KB_LAYOUT "BT_KB_LAYOUT"
#define PARAM_BT_KB_TYPE "BT_KB_TYPE"

#define GCONFIG_SUCCESS 0
#define GCONFIG_INIT_ERROR -1
#define GCONFIG_MISMATCHED_APP -2

int gconfig_init(const char *currentAppName);
SettingsContext *gconfig_getContext(void);

#endif  // GCONFIG_H
