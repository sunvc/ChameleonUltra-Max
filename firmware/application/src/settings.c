#include <stdbool.h>
#include "crc_utils.h"
#include "app_status.h"
#include "settings.h"
#include "fds_ids.h"
#include "fds_util.h"

#define NRF_LOG_MODULE_NAME settings
#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "nrf_log_default_backends.h"
NRF_LOG_MODULE_REGISTER();


static settings_data_t config;
static uint16_t m_config_crc;
static bool m_ble_pairing_enable_first_load_value;

static void update_config_crc(void) {
    calc_14a_crc_lut((uint8_t *)&config, sizeof(config), (uint8_t *)&m_config_crc);
}

static bool config_did_change(void) {
    uint16_t new_calc_crc;
    calc_14a_crc_lut((uint8_t *)&config, sizeof(config), (uint8_t *)&new_calc_crc);
    return new_calc_crc != m_config_crc;
}

void settings_update_version_for_config(void) {
    config.version = SETTINGS_CURRENT_VERSION;
}

// add on version2
void settings_init_button_press_config(void) {
    config.button_a_press = SettingsButtonCycleSlot;
    config.button_b_press = SettingsButtonCycleSlotDec;
}

// add on version3
void settings_init_button_long_press_config(void) {
    config.button_a_long_press = SettingsButtonCloneIcUid;
    config.button_b_long_press = SettingsButtonShowBattery;
}

// add on version4
void settings_init_ble_connect_key_config(void) {
    uint8_t p_key_u8[] = DEFAULT_BLE_PAIRING_KEY;
    settings_set_ble_connect_key(p_key_u8);
}

// add on version5
void settings_init_ble_pairing_enable_config(void) {
    config.ble_pairing_enable = false;
}

// add on version6
void settings_init_sleep_timeout_config(void) {
    config.sleep_timeout = SETTINGS_SLEEP_TIMEOUT_DEFAULT_S;
}

// add on version7
void settings_init_long_press_threshold_config(void) {
    config.long_press_threshold = SETTINGS_LONG_PRESS_THRESHOLD_DEFAULT_MS;
}

// add on version8
// add on version13
void settings_init_extra_long_press_config(void) {
    config.button_a_extra_long_press = SettingsButtonDisable;
    config.button_b_extra_long_press = SettingsButtonDisable;
    config.extra_long_press_threshold = SETTINGS_EXTRA_LONG_PRESS_THRESHOLD_DEFAULT_MS;
}

void settings_init_slot_polling_config(void) {
    config.slot_polling_enable = false;
    config.slot_polling_interval = SETTINGS_SLOT_POLLING_INTERVAL_DEFAULT;
    memset(config.slot_polling_slot_mask, SETTINGS_SLOT_POLLING_SLOT_MASK_DEFAULT, sizeof(config.slot_polling_slot_mask));
    config.slot_group_size = SETTINGS_SLOT_GROUP_SIZE_DEFAULT;
}

// add on version11
void settings_init_smart_polling_config(void) {
    config.smart_polling_enable = SETTINGS_SMART_POLLING_DEFAULT;
    config.ic_polling_enable = 1;
    config.id_polling_enable = 1;
}

void settings_init_config(void) {
    settings_update_version_for_config();
    config.animation_config = SettingsAnimationModeFull; // add on version1
    settings_init_button_press_config();
    settings_init_button_long_press_config();
    settings_init_ble_connect_key_config();
    settings_init_ble_pairing_enable_config();
    settings_init_sleep_timeout_config();
    settings_init_long_press_threshold_config();
    settings_init_slot_polling_config();
    settings_init_smart_polling_config();
    settings_init_extra_long_press_config();
}

// Version 11 and older stored the polling interval in 100 ms units; version 12
// stores it in 10 ms units so the app can use 10 ms granularity.
static void settings_migrate_polling_interval_to_10ms(void) {
    uint16_t interval = (uint16_t)config.slot_polling_interval * 10U;
    if (interval < SETTINGS_SLOT_POLLING_INTERVAL_MIN) {
        interval = SETTINGS_SLOT_POLLING_INTERVAL_MIN;
    }
    if (interval > SETTINGS_SLOT_POLLING_INTERVAL_MAX) {
        interval = SETTINGS_SLOT_POLLING_INTERVAL_MAX;
    }
    config.slot_polling_interval = (uint8_t)interval;
}

void settings_migrate(void) {
    if (config.version < 13) {
        settings_init_extra_long_press_config();
    }
    switch (config.version) {        case 0:
            NRF_LOG_ERROR("Unexpected configuration version detected!");
            settings_init_config();

        case 1:
            settings_init_button_press_config();

        case 2:
            settings_init_button_long_press_config();

        case 3:
            settings_init_ble_connect_key_config();

        case 4:
            settings_init_ble_pairing_enable_config();

        case 5:
            settings_init_sleep_timeout_config();

        case 6:
            settings_init_long_press_threshold_config();

        case 7:
            settings_init_slot_polling_config();
            settings_init_smart_polling_config();
            settings_update_version_for_config();
            break;
        case 8: {
            // Version 8 stored a 1-byte mask at offset 16 and the group size at
            // offset 17. New slots default to polled.
            const uint8_t *old = (const uint8_t *)&config;
            uint8_t old_mask = old[16];
            uint8_t old_group = old[17];
            config.slot_polling_slot_mask[0] = old_mask;
            memset(config.slot_polling_slot_mask + 1, SETTINGS_SLOT_POLLING_SLOT_MASK_DEFAULT,
                   sizeof(config.slot_polling_slot_mask) - 1);
            config.slot_group_size = old_group;
            settings_init_smart_polling_config();
            settings_migrate_polling_interval_to_10ms();
            settings_update_version_for_config();
            break;
        }
        case 9: {
            // Version 9 stored a 32-bit little-endian mask at offsets 16..19
            // and the group size at offset 20.
            const uint8_t *old = (const uint8_t *)&config;
            uint8_t old_mask[4];
            uint8_t old_group = old[20];
            memcpy(old_mask, old + 16, sizeof(old_mask));
            memcpy(config.slot_polling_slot_mask, old_mask, sizeof(old_mask));
            memset(config.slot_polling_slot_mask + 4, SETTINGS_SLOT_POLLING_SLOT_MASK_DEFAULT,
                   sizeof(config.slot_polling_slot_mask) - 4);
            config.slot_group_size = old_group;
            settings_init_smart_polling_config();
            settings_migrate_polling_interval_to_10ms();
            settings_update_version_for_config();
            break;
        }
        case 10:
            // Version 10 had no smart polling flag; default it to enabled.
            settings_init_smart_polling_config();
            settings_migrate_polling_interval_to_10ms();
            settings_update_version_for_config();
            break;
        case 11: {
            // Version 11 stored the polling interval in 100 ms units; version 12
            // stores it in 10 ms units so the app can use 10 ms granularity.
            settings_migrate_polling_interval_to_10ms();
            settings_update_version_for_config();
            break;
        }
        case 12:
            // Version 12 had no extra-long press fields.
            settings_update_version_for_config();
            break;
        case SETTINGS_CURRENT_VERSION:
            break;
        default:
            NRF_LOG_ERROR("Unsupported configuration migration attempted! (%d -> %d)", config.version, SETTINGS_CURRENT_VERSION);
            break;
    }
}

void settings_load_config(void) {
    uint16_t length = sizeof(config);
    bool ret = fds_read_sync(FDS_SETTINGS_FILE_ID, FDS_SETTINGS_RECORD_KEY, &length, (uint8_t *)&config);
    if (ret) {
        NRF_LOG_INFO("Load config done.");
        // After the reading is complete, we first save a copy of the current CRC, which can be used as a reference for comparison of changes when saving later
        update_config_crc();
    } else {
        NRF_LOG_WARNING("Config does not exist, loading default values...");
        settings_init_config();
    }
    if (config.version > SETTINGS_CURRENT_VERSION) {
        NRF_LOG_WARNING("Config version %d is greater than current firmware supports (%d). Default config will be loaded.", config.version, SETTINGS_CURRENT_VERSION);
        settings_init_config();
    }
    if (config.version < SETTINGS_CURRENT_VERSION) {
        NRF_LOG_INFO("Config version (%d) is not latest, performing migration to %d", config.version, SETTINGS_CURRENT_VERSION);
        settings_migrate();
    }
    if (config_did_change()) {
        settings_save_config();
    }

    // Assign values only after the first configuration load.
    m_ble_pairing_enable_first_load_value = config.ble_pairing_enable;
}

uint8_t settings_save_config(void) {
    // We are saving the configuration, we need to calculate the crc code of the current configuration to judge whether the following data is updated
    if (config_did_change()) {    // Before saving, make sure that the configuration has changed
        NRF_LOG_INFO("Save config start.");
        bool ret = fds_write_sync(FDS_SETTINGS_FILE_ID, FDS_SETTINGS_RECORD_KEY, sizeof(config), (uint8_t *)&config);
        if (ret) {
            NRF_LOG_INFO("Save config success.");
            update_config_crc();
        } else {
            NRF_LOG_ERROR("Save config error.");
            return STATUS_FLASH_WRITE_FAIL;
        }
    } else {
        NRF_LOG_INFO("Config did not change.");
    }

    return STATUS_SUCCESS;
}

uint8_t settings_get_animation_config() {
    return config.animation_config;
}

void settings_set_animation_config(uint8_t value) {
    config.animation_config = value;
}

/**
 * @brief check the button type is valid?
 *
 * @param type Button type, 'a' or 'b' or 'A' or 'B'
 * @return true Button type valid.
 * @return false Button type Invalid.
 */
bool is_settings_button_type_valid(char type) {
    switch (type) {
        case 'a':
        case 'b':
        case 'A':
        case 'B':
            return true;
        default:
            return false;
    }
}

/**
 * @brief Get the button press config
 *
 * @param which 'a' or 'b'
 * @return uint8_t @link{ settings_button_function_t }
 */
uint8_t settings_get_button_press_config(char which) {
    switch (which) {
        case 'a':
        case 'A':
            return config.button_a_press;

        case 'b':
        case 'B':
            return config.button_b_press;

        default:
            // can't to here.
            APP_ERROR_CHECK_BOOL(false);
            break;
    }
    // can't to here.
    return SettingsButtonDisable;
}

/**
 * @brief Get the long button press config
 *
 * @param which 'a' or 'b'
 * @return uint8_t @link{ settings_button_function_t }
 */
uint8_t settings_get_long_button_press_config(char which) {
    switch (which) {
        case 'a':
        case 'A':
            return config.button_a_long_press;

        case 'b':
        case 'B':
            return config.button_b_long_press;

        default:
            // can't to here.
            APP_ERROR_CHECK_BOOL(false);
            break;
    }
    // can't to here.
    return SettingsButtonDisable;
}

/**
 * @brief Set the button press config
 *
 * @param which 'a' or 'b'
 * @param value @link{ settings_button_function_t }
 */
void settings_set_button_press_config(char which, uint8_t value) {
    switch (which) {
        case 'a':
        case 'A':
            config.button_a_press = value;
            break;

        case 'b':
        case 'B':
            config.button_b_press = value;
            break;

        default:
            // can't to here.
            APP_ERROR_CHECK_BOOL(false);
            break;
    }
}

/**
 * @brief Set the long button press config
 *
 * @param which 'a' or 'b'
 * @param value @link{ settings_button_function_t }
 */
void settings_set_long_button_press_config(char which, uint8_t value) {
    switch (which) {
        case 'a':
        case 'A':
            config.button_a_long_press = value;
            break;

        case 'b':
        case 'B':
            config.button_b_long_press = value;
            break;

        default:
            // can't to here.
            APP_ERROR_CHECK_BOOL(false);
            break;
    }
}

uint8_t *settings_get_ble_connect_key(void) {
    return config.ble_connect_key;
}

/**
 * @brief Pointer to 6-digit ASCII string (digit 0..9 only, no NULL termination) passkey to be used during pairing.
 *
 * @param key Ble connect key for your device
 */
void settings_set_ble_connect_key(uint8_t *key) {
    memcpy(config.ble_connect_key, key, BLE_PAIRING_KEY_LEN);
}

void settings_set_ble_pairing_enable(bool enable) {
    config.ble_pairing_enable = enable;
}

bool settings_get_ble_pairing_enable(void) {
    return config.ble_pairing_enable;
}

bool settings_get_ble_pairing_enable_first_load(void) {
    return m_ble_pairing_enable_first_load_value;
}

uint32_t settings_get_sleep_timeout(void) {
    return config.sleep_timeout * 1000U;
}

void settings_set_sleep_timeout(uint8_t seconds) {
    config.sleep_timeout = seconds;
}

uint16_t settings_get_long_press_threshold(void) {
    return config.long_press_threshold;
}

void settings_set_long_press_threshold(uint16_t duration) {
    if (duration < SETTINGS_LONG_PRESS_THRESHOLD_MIN_MS) {
        duration = SETTINGS_LONG_PRESS_THRESHOLD_MIN_MS;
    }
    config.long_press_threshold = duration;
}

uint8_t settings_get_extra_long_button_press_config(char which) {
    switch (which) {
        case 'a':
        case 'A':
            return config.button_a_extra_long_press;

        case 'b':
        case 'B':
            return config.button_b_extra_long_press;

        default:
            APP_ERROR_CHECK_BOOL(false);
            return SettingsButtonDisable;
    }
}

void settings_set_extra_long_button_press_config(char which, uint8_t value) {
    switch (which) {
        case 'a':
        case 'A':
            config.button_a_extra_long_press = value;
            break;

        case 'b':
        case 'B':
            config.button_b_extra_long_press = value;
            break;

        default:
            APP_ERROR_CHECK_BOOL(false);
            break;
    }
}

uint16_t settings_get_extra_long_press_threshold(void) {
    uint16_t duration = config.extra_long_press_threshold;
    if (duration < SETTINGS_EXTRA_LONG_PRESS_THRESHOLD_MIN_MS) {
        duration = SETTINGS_EXTRA_LONG_PRESS_THRESHOLD_DEFAULT_MS;
    }
    return duration;
}

void settings_set_extra_long_press_threshold(uint16_t duration) {
    if (duration < SETTINGS_EXTRA_LONG_PRESS_THRESHOLD_MIN_MS) {
        duration = SETTINGS_EXTRA_LONG_PRESS_THRESHOLD_MIN_MS;
    }
    if (duration > SETTINGS_EXTRA_LONG_PRESS_THRESHOLD_MAX_MS) {
        duration = SETTINGS_EXTRA_LONG_PRESS_THRESHOLD_MAX_MS;
    }
    config.extra_long_press_threshold = duration;
}

bool settings_get_slot_polling_enable(void) {
    return config.slot_polling_enable != 0;
}

void settings_set_slot_polling_enable(bool enable) {
    config.slot_polling_enable = enable ? 1 : 0;
}

uint8_t settings_get_slot_polling_interval(void) {
    if (config.slot_polling_interval < SETTINGS_SLOT_POLLING_INTERVAL_MIN) {
        return SETTINGS_SLOT_POLLING_INTERVAL_DEFAULT;
    }
    return config.slot_polling_interval;
}

void settings_set_slot_polling_interval(uint8_t interval) {
    if (interval < SETTINGS_SLOT_POLLING_INTERVAL_MIN) {
        interval = SETTINGS_SLOT_POLLING_INTERVAL_MIN;
    }
    if (interval > SETTINGS_SLOT_POLLING_INTERVAL_MAX) {
        interval = SETTINGS_SLOT_POLLING_INTERVAL_MAX;
    }
    config.slot_polling_interval = interval;
}

const uint8_t *settings_get_slot_polling_slot_mask(void) {
    return config.slot_polling_slot_mask;
}

bool settings_get_slot_polling_slot_mask_bit(uint8_t slot) {
    return (config.slot_polling_slot_mask[slot >> 3] >> (slot & 7)) & 1;
}

void settings_set_slot_polling_slot_mask_bit(uint8_t slot, bool enable) {
    uint8_t mask = 1U << (slot & 7);
    if (enable) {
        config.slot_polling_slot_mask[slot >> 3] |= mask;
    } else {
        config.slot_polling_slot_mask[slot >> 3] &= ~mask;
    }
}

uint8_t settings_get_slot_polling_slot_mask_byte(uint8_t index) {
    return config.slot_polling_slot_mask[index];
}

void settings_set_slot_polling_slot_mask_byte(uint8_t index, uint8_t value) {
    config.slot_polling_slot_mask[index] = value;
}

uint8_t settings_get_slot_group_size(void) {
    return config.slot_group_size;
}

void settings_set_slot_group_size(uint8_t size) {
    // only 0 (disabled), 2 and 4 are meaningful group sizes; 8 equals disabled
    if (size != 0 && size != 2 && size != 4 && size != 8) {
        size = SETTINGS_SLOT_GROUP_SIZE_DEFAULT;
    }
    config.slot_group_size = size;
}

bool settings_get_smart_polling_enable(void) {
    return config.smart_polling_enable != 0;
}

void settings_set_smart_polling_enable(bool enable) {
    config.smart_polling_enable = enable ? 1 : 0;
}

bool settings_get_ic_polling_enable(void) {
    return config.ic_polling_enable != 0;
}

void settings_set_ic_polling_enable(bool enable) {
    config.ic_polling_enable = enable ? 1 : 0;
}

bool settings_get_id_polling_enable(void) {
    return config.id_polling_enable != 0;
}

void settings_set_id_polling_enable(bool enable) {
    config.id_polling_enable = enable ? 1 : 0;
}
