#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>

#include "utils.h"

#define SETTINGS_CURRENT_VERSION 16
#define SETTINGS_SLEEP_TIMEOUT_DEFAULT_S 8   // default wake timeout in seconds (matches SLEEP_DELAY_MS_BUTTON_WAKEUP)
#define SETTINGS_SLEEP_TIMEOUT_MIN_S      5
#define SETTINGS_SLEEP_TIMEOUT_MAX_S      60
#define SETTINGS_LONG_PRESS_THRESHOLD_DEFAULT_MS 1000
#define SETTINGS_LONG_PRESS_THRESHOLD_MIN_MS     200
#define SETTINGS_EXTRA_LONG_PRESS_THRESHOLD_DEFAULT_MS 3000
#define SETTINGS_EXTRA_LONG_PRESS_THRESHOLD_MIN_MS     1500
#define SETTINGS_EXTRA_LONG_PRESS_THRESHOLD_MAX_MS     5000
#define BLE_PAIRING_KEY_LEN 6
#define DEFAULT_BLE_PAIRING_KEY "123456"  // length must == 6

// Slot auto-polling settings (add on version8)
// interval is in 10 ms units, e.g. 50 -> 500 ms between slot switches
#define SETTINGS_SLOT_POLLING_INTERVAL_DEFAULT   50
#define SETTINGS_SLOT_POLLING_INTERVAL_MIN       1
#define SETTINGS_SLOT_POLLING_INTERVAL_MAX       100
#define SETTINGS_SLOT_POLLING_SLOT_MASK_DEFAULT  0xFF // each byte covers 8 slots
// slot group size: 0 or 8 = groups disabled, valid values: 0, 2, 4, 8
#define SETTINGS_SLOT_GROUP_SIZE_DEFAULT         0
// Smart polling: UID/MRU first, reader-event fast switch, background sweep fallback
#define SETTINGS_SMART_POLLING_DEFAULT           1

typedef enum {
    SettingsAnimationModeFull = 0U,
    SettingsAnimationModeMinimal = 1U,
    SettingsAnimationModeSymmetric = 3U,
    SettingsAnimationModeNone = 2U,
    SettingsAnimationModeMAX = 4U,
} settings_animation_mode_t;

typedef enum {
    // Set this button to have no function
    //    (But always can wakeup device, why didn't to disable this function? i dont known, you can ask chatgpt.)
    SettingsButtonDisable = 0U,
    // Card slot number sequence will increase after pressing
    SettingsButtonCycleSlot = 1U,
    // Card slot number sequence decreases after pressing
    SettingsButtonCycleSlotDec = 2U,
    // Read the UID card number immediately after pressing, continue searching, and simulate immediately after reading the card
    SettingsButtonCloneIcUid = 3U,
    SettingsButtonShowBattery = 4U,
    // Toggle NFC field generator on/off (Ultra only, must be in reader mode)
    SettingsButtonNfcFieldGenerator = 5U,
    // Read a full Mifare Classic card into the current slot (dictionary based, offline)
    SettingsButtonFullClone = 6U,
    // Save all data and settings, then reboot the device
    SettingsButtonSoftReboot = 7U,
    // Switch to the next slot group (requires slot group size configured)
    SettingsButtonCycleGroup = 8U,
    // Toggle slot auto-polling on/off
    SettingsButtonToggleSlotPolling = 9U,
    // Toggle the Apple Find My (OpenHaystack) beacon on/off
    SettingsButtonToggleFindMyBeacon = 10U,
} settings_button_function_t;

typedef struct ALIGN_U32 {
    uint16_t version;

    // 1 byte
    uint8_t animation_config : 2;
    uint8_t ble_pairing_enable : 1;
    uint8_t reserved0 : 5; // If you are add switch field, reallocating me.

    // 1 byte
    uint8_t button_a_press : 4;
    uint8_t button_b_press : 4;

    // 1 byte
    uint8_t button_a_long_press : 4;
    uint8_t button_b_long_press : 4;

    // 6 byte
    uint8_t ble_connect_key[6];

    // 1 byte (add on version6)
    uint8_t sleep_timeout; // wake timeout in seconds after button wakeup

    // 2 bytes (add on version7)
    uint16_t long_press_threshold; // long button press threshold in ms

    // 35 bytes (add on version10; version8/9 used a 1/4-byte mask here) slot auto-polling / card group configuration
    uint8_t slot_polling_enable;        // 0 = off, 1 = auto switch slots periodically
    uint8_t slot_polling_interval;      // interval in 10 ms units between slot switches
    uint8_t slot_polling_slot_mask[32]; // 256-bit mask, slot N is mask[N / 8] bit (N % 8)
    uint8_t slot_group_size;            // 0 or 8 = groups disabled, else 2 or 4 slots per group
    uint8_t smart_polling_enable;       // UID/MRU fast polling and reader-event switching
    uint8_t ic_polling_enable;          // include HF/IC slots in polling
    uint8_t id_polling_enable;          // include LF/ID slots in polling

    // Added at the end so older settings keep their field offsets (version 13).
    uint8_t button_a_extra_long_press : 4;
    uint8_t button_b_extra_long_press : 4;
    uint16_t extra_long_press_threshold; // extra-long press threshold in ms

    /*
     * Warning !!!!!!!!!!!!!!!!!!!!!! <-------------
     * If you need to add settings,
     * please be sure to consult the documentation of the bit field
     * and fully use the space of this structure before considering reallocating memory space.
     */
} settings_data_t;

void settings_init_config(void);
void settings_migrate(void);
void settings_load_config(void);
uint8_t settings_save_config(void);
uint8_t settings_get_animation_config(void);
void settings_set_animation_config(uint8_t value);
uint8_t settings_get_button_press_config(char which);
uint8_t settings_get_long_button_press_config(char which);
void settings_set_button_press_config(char which, uint8_t value);
void settings_set_long_button_press_config(char which, uint8_t value);
bool is_settings_button_type_valid(char type);
uint8_t *settings_get_ble_connect_key(void);
void settings_set_ble_connect_key(uint8_t *key);
void settings_set_ble_pairing_enable(bool enable);
bool settings_get_ble_pairing_enable(void);
bool settings_get_ble_pairing_enable_first_load(void);
uint32_t settings_get_sleep_timeout(void);
void settings_set_sleep_timeout(uint8_t seconds);
void settings_init_sleep_timeout_config(void);
uint16_t settings_get_long_press_threshold(void);
void settings_set_long_press_threshold(uint16_t duration);
void settings_init_long_press_threshold_config(void);
uint8_t settings_get_extra_long_button_press_config(char which);
void settings_set_extra_long_button_press_config(char which, uint8_t value);
uint16_t settings_get_extra_long_press_threshold(void);
void settings_set_extra_long_press_threshold(uint16_t duration);
void settings_init_extra_long_press_config(void);
// slot auto-polling / card group settings (add on version8)
void settings_init_slot_polling_config(void);
bool settings_get_slot_polling_enable(void);
void settings_set_slot_polling_enable(bool enable);
uint8_t settings_get_slot_polling_interval(void);
void settings_set_slot_polling_interval(uint8_t interval);
const uint8_t *settings_get_slot_polling_slot_mask(void);
bool settings_get_slot_polling_slot_mask_bit(uint8_t slot);
void settings_set_slot_polling_slot_mask_bit(uint8_t slot, bool enable);
uint8_t settings_get_slot_polling_slot_mask_byte(uint8_t index);
void settings_set_slot_polling_slot_mask_byte(uint8_t index, uint8_t value);
uint8_t settings_get_slot_group_size(void);
void settings_set_slot_group_size(uint8_t size);
bool settings_get_smart_polling_enable(void);
void settings_set_smart_polling_enable(bool enable);
void settings_init_smart_polling_config(void);
bool settings_get_ic_polling_enable(void);
void settings_set_ic_polling_enable(bool enable);
bool settings_get_id_polling_enable(void);
void settings_set_id_polling_enable(bool enable);
#endif
