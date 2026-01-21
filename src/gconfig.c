#include "include/gconfig.h"

static SettingsConfigEntry defaultEntries[] = {
    {PARAM_BOOT_FEATURE, SETTINGS_TYPE_STRING, "IKBD"},
    {PARAM_HOSTNAME, SETTINGS_TYPE_STRING, "croissant"},
    {PARAM_WIFI_AUTH, SETTINGS_TYPE_INT, "0"},
    {PARAM_WIFI_CONNECT_TIMEOUT, SETTINGS_TYPE_INT, "30"},
    {PARAM_WIFI_COUNTRY, SETTINGS_TYPE_STRING, "XX"},
    {PARAM_WIFI_DHCP, SETTINGS_TYPE_BOOL, "true"},
    {PARAM_WIFI_DNS, SETTINGS_TYPE_STRING, "8.8.8.8"},
    {PARAM_WIFI_GATEWAY, SETTINGS_TYPE_STRING, ""},
    {PARAM_WIFI_IP, SETTINGS_TYPE_STRING, ""},
    {PARAM_WIFI_MODE, SETTINGS_TYPE_INT, "0"},
    {PARAM_WIFI_NETMASK, SETTINGS_TYPE_STRING, ""},
    {PARAM_WIFI_PASSWORD, SETTINGS_TYPE_STRING, ""},
    {PARAM_WIFI_POWER, SETTINGS_TYPE_INT, "0"},
    {PARAM_WIFI_RSSI, SETTINGS_TYPE_BOOL, "true"},
    {PARAM_WIFI_SCAN_SECONDS, SETTINGS_TYPE_INT, "10"},
    {PARAM_WIFI_SSID, SETTINGS_TYPE_STRING, ""},
    {PARAM_MOUSE_SPEED, SETTINGS_TYPE_INT, "5"},
    {PARAM_USB_KB_LAYOUT, SETTINGS_TYPE_STRING, "US"},
    {PARAM_USB_KB_TYPE, SETTINGS_TYPE_INT, "0"},
    {PARAM_JOYSTICK_USB, SETTINGS_TYPE_BOOL, "false"},
    {PARAM_JOYSTICK_USB_PORT, SETTINGS_TYPE_INT, "1"},
    {PARAM_MOUSE_ORIGINAL, SETTINGS_TYPE_BOOL, "false"},
    {PARAM_BT_ENABLED, SETTINGS_TYPE_BOOL, "true"},
    {PARAM_BT_MODE, SETTINGS_TYPE_INT, "2"},
    {PARAM_BT_KB_LAYOUT, SETTINGS_TYPE_STRING, "US"},
    {PARAM_BT_KB_TYPE, SETTINGS_TYPE_INT, "0"},
    {PARAM_BT_KEYBOARD, SETTINGS_TYPE_STRING, ""},
    {PARAM_BT_MOUSE, SETTINGS_TYPE_STRING, ""},
    {PARAM_BT_GAMEPAD, SETTINGS_TYPE_STRING, ""},
    {PARAM_MODE, SETTINGS_TYPE_INT,
     "255"}};  // 0 -> Native, 1 -> USB, 2 -> BT, 255 -> Config

enum {
  CONFIG_BUFFER_SIZE = 4096,
  CONFIG_MAGIC_NUMBER = 0x1234,
  CONFIG_VERSION_NUMBER = 0x0001
};

// Create a global context for our settings
static SettingsContext gSettingsCtx;

/**
 * @brief Initializes the global configuration settings.
 *
 * This function initializes the global configuration settings using the
 * provided default entries. If the settings are not initialized, it initializes
 * them with the default values. If a current application name is provided, it
 * checks if the current application matches the one in the settings.
 *
 * @param current_app_name The name of the current application. If NULL, the
 * function will ignore the application name check.
 * @return int Returns GCONFIG_SUCCESS on success, GCONFIG_INIT_ERROR if there
 * is an error initializing the settings, or GCONFIG_MISMATCHED_APP if the
 * current application does not match the one in the settings.
 */
int gconfig_init(const char *currentAppName) {
  DPRINTF("Initializing settings\n");
  int err = settings_init(&gSettingsCtx, defaultEntries,
                          sizeof(defaultEntries) / sizeof(defaultEntries[0]),
                          (unsigned int)&_global_config_flash_start - XIP_BASE,
                          CONFIG_BUFFER_SIZE, CONFIG_MAGIC_NUMBER,
                          CONFIG_VERSION_NUMBER);

  // If the settings are not initialized, then we must initialize them with the
  // default values in the Booster application
  if (err < 0) {
    DPRINTF("Error initializing settings.\n");
    return GCONFIG_INIT_ERROR;
  }

  // If the current app as argument is not null, check if the current app is the
  // same as the one in the settings Otherwise, ignore and continue
  if (currentAppName != NULL) {
    // If we are here, it means that the settings were initialized correctly
    // We now must read the flash address of the configuration settings of the
    // current application
    SettingsConfigEntry *entry =
        settings_find_entry(&gSettingsCtx, PARAM_BOOT_FEATURE);
    if ((entry == NULL) || (entry->value == NULL) ||
        (strcmp(currentAppName, entry->value) != 0)) {
      // If the entry is found but the content is empty, or not equal to the
      // current app name then go to the Booster application
      DPRINTF(
          "The current app (%s) is not the same as the one in the settings "
          "(%s)\n",
          currentAppName, entry->value);
      return GCONFIG_MISMATCHED_APP;
    }
  } else {
    DPRINTF("The current app is not provided as argument. Booster app?\n");
  }

  DPRINTF("Settings loaded.\n");

  settings_print(&gSettingsCtx, NULL);

  return GCONFIG_SUCCESS;
}

/**
 * @brief Returns a pointer to the global settings context.
 *
 * This function allows other parts of the application to retrieve a pointer
 * to the global settings context at any time.
 *
 * @return SettingsContext* Pointer to the global settings context.
 */
SettingsContext *gconfig_getContext(void) { return &gSettingsCtx; }
