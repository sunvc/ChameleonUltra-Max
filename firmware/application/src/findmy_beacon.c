#include <string.h>

#include "app_util.h"
#include "app_timer.h"
#include "bsp_delay.h"
#include "fds_ids.h"
#include "fds_util.h"
#include "findmy_beacon.h"
#include "ble_main.h"
#include "rfid_main.h"
#include "rgb_marquee.h"
#include "syssleep.h"
#include "utils.h"

#define NRF_LOG_MODULE_NAME findmy
#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "nrf_log_default_backends.h"
NRF_LOG_MODULE_REGISTER();

// Flash record holding the host generated public key. Placed between the slot
// metadata range (0x1300 + slot) and the Peer Manager range (> 0xBFFF).
#define FDS_FINDMY_BEACON_FILE_ID 0x1400
#define FDS_FINDMY_BEACON_RECORD_KEY 0x1

static findmy_beacon_config_t m_config;
// FDS rounds record sizes up to 32-bit words; a 30-byte record would write and
// read two bytes past the struct, so the size must be exactly 32 bytes.
STATIC_ASSERT(sizeof(findmy_beacon_config_t) == 32, "FindMy config must be 32 bytes for FDS");

static bool m_config_enabled = false;
static bool m_pending_address = false;
static bool m_led_feedback_enabled = false;
static bool m_connectable_window_active = false;

extern volatile bool g_usb_led_marquee_enable;

APP_TIMER_DEF(m_connectable_window_timer);

static void findmy_beacon_apply(void);

static void findmy_beacon_connectable_window_timeout(void *p_context) {
    UNUSED_VARIABLE(p_context);
    m_connectable_window_active = false;
    if (is_nus_working()) {
        // The app is connected. Restore FindMy after the link is gone.
        return;
    }
    findmy_beacon_clear_indicator_leds();
    findmy_beacon_apply();
}

/**
 * @brief (Re)configure and start or stop the FindMy advertising set.
 *
 * The random static address embeds the key, so it must be applied while no
 * advertising or connection is active. If the SoftDevice refuses (link up),
 * the update is deferred to findmy_beacon_on_ble_disconnected().
 */
static void findmy_beacon_apply(void) {
    if (!m_config_enabled) {
        ble_findmy_advertising_stop();
        return;
    }

    ble_findmy_advertising_stop();
    m_pending_address = !ble_findmy_set_address(m_config.key);
    ble_findmy_adv_update(m_config.key);
    ble_findmy_advertising_start();
}

static bool findmy_beacon_store(void) {
    if (!fds_write_sync(FDS_FINDMY_BEACON_FILE_ID, FDS_FINDMY_BEACON_RECORD_KEY, sizeof(m_config), &m_config)) {
        NRF_LOG_ERROR("FindMy config write failed");
        return false;
    }
    return true;
}

/**@brief Short RGB blink so the on/off state is visible without the app.
 *        Called from the button handler; keeps the delay small.
 */
static void findmy_beacon_led_feedback(bool enabled) {
    if (!m_led_feedback_enabled) {
        return;
    }
    set_slot_light_color(enabled ? RGB_GREEN : RGB_RED);
    bsp_delay_ms(200);
    light_up_by_slot();
}

void findmy_beacon_init(void) {
    uint16_t length = sizeof(m_config);
    memset(&m_config, 0, sizeof(m_config));

    if (!fds_read_sync(FDS_FINDMY_BEACON_FILE_ID, FDS_FINDMY_BEACON_RECORD_KEY, &length, (uint8_t *)&m_config) ||
            m_config.version != FINDMY_BEACON_RECORD_VERSION) {
        NRF_LOG_INFO("FindMy config not found or outdated, beacon disabled");
        memset(&m_config, 0, sizeof(m_config));
        m_config.version = FINDMY_BEACON_RECORD_VERSION;
        m_config.enable = 0;
    } else {
        NRF_LOG_INFO("FindMy config loaded, enable=%d", m_config.enable);
    }

    m_config_enabled = m_config.enable != 0;

    ret_code_t timer_err = app_timer_create(&m_connectable_window_timer, APP_TIMER_MODE_SINGLE_SHOT,
        findmy_beacon_connectable_window_timeout);
    if (timer_err != NRF_SUCCESS) {
        NRF_LOG_ERROR("FindMy connectable timer create failed: %d", timer_err);
    }

    if (m_config_enabled) {
        findmy_beacon_apply();
    }
}

bool findmy_beacon_set_config(const findmy_beacon_config_t *cfg) {
    if (cfg == NULL) {
        return false;
    }

    findmy_beacon_config_t next = *cfg;
    next.version = FINDMY_BEACON_RECORD_VERSION;
    next.enable = next.enable ? 1 : 0;

    bool enable_changed = m_config_enabled != (next.enable != 0);
    bool changed = enable_changed ||
                   memcmp(m_config.key, next.key, sizeof(m_config.key)) != 0;
    if (!changed) {
        m_led_feedback_enabled = false;
        return true;
    }

    findmy_beacon_config_t prev = m_config;
    bool prev_enabled = m_config_enabled;

    m_config = next;
    m_config_enabled = next.enable != 0;

    if (!findmy_beacon_store()) {
        // Roll back so RAM and flash stay consistent.
        m_config = prev;
        m_config_enabled = prev_enabled;
        m_led_feedback_enabled = false;
        return false;
    }

    if (!m_config_enabled) {
        m_connectable_window_active = false;
        app_timer_stop(m_connectable_window_timer);
    }

    findmy_beacon_apply();

    // Keep the device awake while the beacon is on, and re-arm the normal
    // sleep timeout when it goes off (mirrors the NFC field generator).
    if (m_config_enabled) {
        sleep_timer_stop();
    } else {
        sleep_timer_start(SLEEP_DELAY_MS_BUTTON_CLICK);
    }

    findmy_beacon_led_feedback(m_config_enabled);
    m_led_feedback_enabled = false;
    return true;
}

void findmy_beacon_set_led_feedback(bool enable) {
    m_led_feedback_enabled = enable;
}

bool findmy_beacon_set_enable(bool enable) {
    findmy_beacon_config_t cfg = m_config;
    cfg.enable = enable ? 1 : 0;
    return findmy_beacon_set_config(&cfg);
}

void findmy_beacon_get_config(findmy_beacon_config_t *cfg) {
    if (cfg != NULL) {
        *cfg = m_config;
    }
}

bool findmy_beacon_is_enabled(void) {
    return m_config_enabled;
}

bool findmy_beacon_has_key(void) {
    for (uint8_t i = 0; i < sizeof(m_config.key); i++) {
        if (m_config.key[i] != 0) {
            return true;
        }
    }
    return false;
}

bool findmy_beacon_is_active(void) {
    return m_config_enabled && ble_findmy_is_advertising();
}

void findmy_beacon_on_ble_disconnected(void) {
    if (m_config_enabled && m_pending_address && !m_connectable_window_active) {
        // The link is gone; the identity address can be changed again. The
        // advertising set kept running with the old address meanwhile.
        m_pending_address = !ble_findmy_set_address(m_config.key);
        ble_findmy_adv_update(m_config.key);
    }
}

void findmy_beacon_start_if_enabled(void) {
    if (m_config_enabled && !m_connectable_window_active) {
        findmy_beacon_apply();
    }
}

void findmy_beacon_start_connectable_window(uint32_t duration_ms) {
    if (!m_config_enabled || is_nus_working()) {
        return;
    }
    if (duration_ms < 1000) {
        duration_ms = 1000;
    }
    if (!ble_findmy_enter_connectable_mode()) {
        return;
    }
    m_connectable_window_active = true;
    app_timer_stop(m_connectable_window_timer);
    ret_code_t err = app_timer_start(m_connectable_window_timer, APP_TIMER_TICKS(duration_ms), NULL);
    if (err != NRF_SUCCESS) {
        NRF_LOG_WARNING("FindMy connectable timer start failed: %d", err);
    }
    NRF_LOG_INFO("FindMy paused for connectable window");
}

bool findmy_beacon_connectable_window_active(void) {
    return m_connectable_window_active;
}

void findmy_beacon_clear_indicator_leds(void) {
    if (rgb_marquee_white_light_is_on()) {
        return;
    }
    g_usb_led_marquee_enable = false;
    rgb_marquee_stop();
    light_down_all();
}

bool findmy_beacon_should_stay_awake(void) {
    return m_config_enabled;
}
