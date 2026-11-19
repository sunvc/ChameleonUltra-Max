#include "ble_hci.h"
#include "ble_advdata.h"
#include "ble_advertising.h"
#include "ble_conn_params.h"

#include "nrf.h"
#include "nordic_common.h"
#include "nrf_sdh.h"
#include "nrf_ble_qwr.h"
#include "nrf_sdh_soc.h"
#include "nrf_sdh_ble.h"
#include "nrf_ble_gatt.h"
#include "nrf_ble_lesc.h"

#include "peer_manager.h"
#include "peer_manager_handler.h"

#include "app_timer.h"
#include "app_util_platform.h"

#include <string.h>

#include "syssleep.h"
#include "ble_main.h"
#include "findmy_beacon.h"
#include "dataframe.h"
#include "hw_connect.h"
#include "rfid_main.h"
#include "settings.h"

#define NRF_LOG_MODULE_NAME ble_main
#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "nrf_log_default_backends.h"
NRF_LOG_MODULE_REGISTER();


#define LESC_DEBUG_MODE                 0                                           /**< Set to 1 to use the LESC debug keys. The debug mode allows you to use a sniffer to inspect traffic. */
#define LESC_MITM_NC                    1                                           /**< Use MITM (Numeric Comparison). */

#define SEC_PARAMS_BOND                 1                                           /**< Perform bonding. */
#if LESC_MITM_NC
#define SEC_PARAMS_MITM                 1                                           /**< Man In The Middle protection required. */
#define SEC_PARAMS_IO_CAPABILITIES      BLE_GAP_IO_CAPS_DISPLAY_ONLY
#else
#define SEC_PARAMS_MITM                 0                                           /**< Man In The Middle protection required. */
#define SEC_PARAMS_IO_CAPABILITIES      BLE_GAP_IO_CAPS_DISPLAY_ONLY
#endif
#define SEC_PARAMS_LESC                 1                                           /**< LE Secure Connections pairing required. */
#define SEC_PARAMS_KEYPRESS             0                                           /**< Keypress notifications not required. */
#define SEC_PARAMS_OOB                  0                                           /**< Out Of Band data not available. */
#define SEC_PARAMS_MIN_KEY_SIZE         7                                           /**< Minimum encryption key size in octets. */
#define SEC_PARAMS_MAX_KEY_SIZE         16                                          /**< Maximum encryption key size in octets. */

#define APP_BLE_CONN_CFG_TAG            1                                           /**< A tag identifying the SoftDevice BLE configuration. */
#define NUS_SERVICE_UUID_TYPE           BLE_UUID_TYPE_VENDOR_BEGIN                  /**< UUID type for the Nordic UART Service (vendor specific). */

#define APP_BLE_OBSERVER_PRIO           3                                           /**< Application's BLE observer priority. You shouldn't need to modify this value. */
#define APP_ADV_INTERVAL                64                                          /**< The advertising interval (in units of 0.625 ms. This value corresponds to 40 ms). */

#define MIN_CONN_INTERVAL               MSEC_TO_UNITS(20, UNIT_1_25_MS)             /**< Minimum acceptable connection interval (20 ms), Connection interval uses 1.25 ms units. */
#define MAX_CONN_INTERVAL               MSEC_TO_UNITS(75, UNIT_1_25_MS)             /**< Maximum acceptable connection interval (75 ms), Connection interval uses 1.25 ms units. */
#define SLAVE_LATENCY                   0                                           /**< Slave latency. */
#define CONN_SUP_TIMEOUT                MSEC_TO_UNITS(4000, UNIT_10_MS)             /**< Connection supervisory timeout (4 seconds), Supervision Timeout uses 10 ms units. */
#define FIRST_CONN_PARAMS_UPDATE_DELAY  APP_TIMER_TICKS(5000)                       /**< Time from initiating event (connect or start of notification) to first time sd_ble_gap_conn_param_update is called (5 seconds). */
#define NEXT_CONN_PARAMS_UPDATE_DELAY   APP_TIMER_TICKS(30000)                      /**< Time between each call to sd_ble_gap_conn_param_update after the first call (30 seconds). */
#define MAX_CONN_PARAMS_UPDATE_COUNT    3                                           /**< Number of attempts before giving up the connection parameter negotiation. */

#define BATTERY_LEVEL_MEAS_INTERVAL     APP_TIMER_TICKS(5000)                       /**< Battery level measurement interval (ticks). This value corresponds to N seconds. */

#define ADC_REF_VOLTAGE_IN_MILLIVOLTS  600  //!< Reference voltage (in milli volts) used by ADC while doing conversion.
#define ADC_RES_12BIT                  16383 //!< Maximum digital value for 14-bit ADC conversion.
#define ADC_PRE_SCALING_COMPENSATION   12    //!< The ADC is configured to use VDD with 1/3 prescaling as input. And hence the result of conversion is to be multiplied by 3 to get the actual value of the battery voltage.
#define ADC_CHANNEL                    0
#define ADC_BUF_SIZE                   2048
#define ADC_BUF_COUNT                  2


/**@brief Macro to convert the result of ADC conversion in millivolts.
 *
 * @param[in]  ADC_VALUE   ADC result.
 *
 * @retval     Result converted to millivolts.
 */
#define ADC_RESULT_IN_MILLI_VOLTS(ADC_VALUE)\
        ((((ADC_VALUE) * ADC_REF_VOLTAGE_IN_MILLIVOLTS) / ADC_RES_12BIT) * ADC_PRE_SCALING_COMPENSATION)

APP_TIMER_DEF(m_battery_timer_id);                                                  /**< Battery measurement timer. */
BLE_BAS_DEF(m_bas);                                                                 /**< Battery service instance. */
BLE_NUS_DEF(m_nus, NRF_SDH_BLE_TOTAL_LINK_COUNT);                                   /**< BLE NUS service instance. */
NRF_BLE_GATT_DEF(m_gatt);                                                           /**< GATT module instance. */
NRF_BLE_QWR_DEF(m_qwr);                                                             /**< Context for the Queued Write module.*/
BLE_ADVERTISING_DEF(m_advertising);                                                 /**< Advertising module instance. */

uint16_t          batt_lvl_in_milli_volts = 0;
uint8_t           percentage_batt_lvl = 0;
static nrf_saadc_value_t adc_buf[ADC_BUF_COUNT][ADC_BUF_SIZE];
static uint16_t   m_conn_handle          = BLE_CONN_HANDLE_INVALID;                 /**< Handle of the current connection. */
static uint16_t   m_ble_nus_max_data_len = BLE_GATT_ATT_MTU_DEFAULT - 3;            /**< Maximum length of data (in bytes) that can be transmitted to the peer by the Nordic UART service module. */
lf_adc_callback_t m_lf_adc_callback      = NULL;

static ble_uuid_t m_adv_uuids[]          =                                          /**< Universally unique service identifier. */
{
    {BLE_UUID_NUS_SERVICE, NUS_SERVICE_UUID_TYPE},
    {BLE_UUID_BATTERY_SERVICE, BLE_UUID_TYPE_BLE},
};
volatile bool g_is_ble_connected = false;
volatile bool g_is_low_battery_shutdown = false;
static ble_opt_t m_static_pin_option;

// Find My / OpenHaystack beacon: a second, non-connectable advertising set.
// The S140 headers declare a maximum of one advertising set, so the two-set
// configuration is attempted at boot and the code falls back to sharing the
// primary (connectable) set when the stack refuses it.
#define APP_FINDMY_ADV_INTERVAL 3200                /**< 2 s in units of 0.625 ms, as required by the offline finding spec. */
static bool m_findmy_adv_two_sets_supported = false;
static uint8_t m_findmy_adv_handle = BLE_GAP_ADV_SET_HANDLE_NOT_SET;
static bool m_findmy_adv_active = false;
static bool m_findmy_shares_primary_set = false;
static bool m_findmy_address_applied = false;
static ble_gap_addr_t m_original_adv_addr;
static ble_gap_addr_t m_applied_adv_addr;
static uint8_t m_findmy_key[APP_FINDMY_BEACON_KEY_LEN];

static uint8_t m_findmy_adv_data[31];
static ble_gap_adv_params_t m_findmy_adv_params = {
    .properties = {
        .type = BLE_GAP_ADV_TYPE_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED,
        .anonymous = 0,
        .include_tx_power = 0,
    },
    .p_peer_addr = NULL,
    .interval = APP_FINDMY_ADV_INTERVAL,
    .duration = 0,
    .max_adv_evts = 0,
    .channel_mask = {0, 0, 0, 0, 0},
    .filter_policy = BLE_GAP_ADV_FP_ANY,
    .primary_phy = BLE_GAP_PHY_1MBPS,
    .secondary_phy = BLE_GAP_PHY_1MBPS,
    .set_id = 0,
    .scan_req_notification = 0,
};

// Simple function to provide an index to the next input buffer
// Will simply alernate between 0 and 1 when SAADC_BUF_COUNT is 2
static uint32_t next_free_buf_index(void) {
    static uint32_t buffer_index = -1;
    buffer_index = (buffer_index + 1) % ADC_BUF_COUNT;
    return buffer_index;
}


/**@brief Function for the ble connect key setup.
 *
 * @details This function will set up the ble connect passkey.
 */
void set_ble_connect_key(uint8_t *key) {
    static uint8_t passkey[BLE_PAIRING_KEY_LEN];
    memcpy(passkey, key, BLE_PAIRING_KEY_LEN);
    m_static_pin_option.gap_opt.passkey.p_passkey = passkey;
    // NRF_LOG_RAW_HEXDUMP_INFO(passkey, 6);
    APP_ERROR_CHECK(sd_ble_opt_set(BLE_GAP_OPT_PASSKEY, &m_static_pin_option));
}

/**@brief Function for the GAP initialization.
 *
 * @details This function will set up all the necessary GAP (Generic Access Profile) parameters of
 *          the device. It also sets the permissions and appearance.
 */
static void gap_params_init(void) {
    uint32_t                err_code;
    ble_gap_conn_params_t   gap_conn_params;
    ble_gap_conn_sec_mode_t sec_mode;

    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&sec_mode);

    err_code = sd_ble_gap_device_name_set(&sec_mode, (const uint8_t *) DEVICE_NAME_STR, strlen(DEVICE_NAME_STR));
    APP_ERROR_CHECK(err_code);

    memset(&gap_conn_params, 0, sizeof(gap_conn_params));

    gap_conn_params.min_conn_interval = MIN_CONN_INTERVAL;
    gap_conn_params.max_conn_interval = MAX_CONN_INTERVAL;
    gap_conn_params.slave_latency     = SLAVE_LATENCY;
    gap_conn_params.conn_sup_timeout  = CONN_SUP_TIMEOUT;

    err_code = sd_ble_gap_ppcp_set(&gap_conn_params);
    APP_ERROR_CHECK(err_code);
}

/**@brief Function for handling the Battery Service events.
 *
 * @details This function will be called for all Battery Service events which are passed to the
 |          application.
 *
 * @param[in] p_bas  Battery Service structure.
 * @param[in] p_evt  Event received from the Battery Service.
 */
static void on_bas_evt(ble_bas_t *p_bas, ble_bas_evt_t *p_evt) {
    switch (p_evt->evt_type) {
        case BLE_BAS_EVT_NOTIFICATION_ENABLED:
            break; // BLE_BAS_EVT_NOTIFICATION_ENABLED

        case BLE_BAS_EVT_NOTIFICATION_DISABLED:
            break; // BLE_BAS_EVT_NOTIFICATION_DISABLED

        default:
            // No implementation needed.
            break;
    }
}

/**@brief Function for handling the data from the Nordic UART Service.
 *
 * @details This function will process the data received from the Nordic UART BLE Service
 *
 * @param[in] p_evt       Nordic UART Service event.
 */
/**@snippet [Handling the data received over BLE] */
static void nus_data_handler(ble_nus_evt_t *p_evt) {
    if (p_evt->type == BLE_NUS_EVT_RX_DATA) {
        NRF_LOG_DEBUG("Received data from BLE NUS.");
        NRF_LOG_HEXDUMP_DEBUG(p_evt->params.rx_data.p_data, p_evt->params.rx_data.length);
        data_frame_receive((uint8_t *)(p_evt->params.rx_data.p_data), p_evt->params.rx_data.length);
    }
}
/**@snippet [Handling the data received over BLE] */

void nus_data_response(uint8_t *p_data, uint16_t length) {
    NRF_LOG_INFO("BLE nus service response data length: %d", length);
    NRF_LOG_HEXDUMP_DEBUG(p_data, length);

    ret_code_t err_code;
    uint16_t remain = length;
    uint16_t count = 0;
    do {
        remain = MIN(m_ble_nus_max_data_len, remain);
        err_code = ble_nus_data_send(&m_nus, p_data + count, &remain, m_conn_handle);
        // NRF_LOG_INFO("Data send length(amount): %d", remain);
        if (err_code == NRF_SUCCESS) {
            count += remain;
            remain = length - count;
        }
        // NRF_LOG_INFO("Data send length(count): %d", count);
        if (err_code == NRF_ERROR_BUSY) {
            continue;
        }
        if ((err_code != NRF_ERROR_INVALID_STATE) &&
                (err_code != NRF_ERROR_RESOURCES) &&
                (err_code != NRF_ERROR_NOT_FOUND)) {
            APP_ERROR_CHECK(err_code);
        }

    } while (count != length && g_is_ble_connected);
}

bool is_nus_working(void) {
    return g_is_ble_connected;
}

/**@brief Function for handling Queued Write Module errors.
 *
 * @details A pointer to this function will be passed to each service which may need to inform the
 *          application about an error.
 *
 * @param[in]   nrf_error   Error code containing information about what went wrong.
 */
static void nrf_qwr_error_handler(uint32_t nrf_error) {
    APP_ERROR_HANDLER(nrf_error);
}

// The battery curve is calibrated against the lowest stable full-charge voltage
// we have observed after unplugging USB and letting the cell settle. In issue
// #334 the reported range looks like a typo and the plausible value is
// 4160-4190 mV, so we use the conservative lower bound to keep 100% reachable.
#define BATTERY_FULL_CHARGE_REFERENCE_MV  4160U
#define BATTERY_CURVE_BASE_FULL_MV        4200U
#define BATTERY_CURVE_SCALE(mv) \
    ((uint32_t)(((uint32_t)(mv) * BATTERY_FULL_CHARGE_REFERENCE_MV + (BATTERY_CURVE_BASE_FULL_MV / 2U)) / BATTERY_CURVE_BASE_FULL_MV))

static uint32_t map_clamped(uint32_t x, uint32_t in_min, uint32_t in_max, uint32_t out_min, uint32_t out_max) {
    if (in_max <= in_min) {
        return out_min;
    }
    if (x <= in_min) {
        return out_min;
    }
    if (x >= in_max) {
        return out_max;
    }

    return out_min + ((x - in_min) * (out_max - out_min)) / (in_max - in_min);
}

// Battery voltage to percentage calculation
uint32_t BATVOL2PERCENT(uint16_t VOL) {
    // Based on https://github.com/RfidResearchGroup/ChameleonUltra/issues/167#issuecomment-1766908799
    // and recalibrated from the issue #334 full-charge measurements.
    const uint32_t p100vol = BATTERY_CURVE_SCALE(4200U);

#if defined(PROJECT_CHAMELEON_ULTRA)
    // Ultra
    const uint32_t p80vol = BATTERY_CURVE_SCALE(4034U);
    const uint32_t p60vol = BATTERY_CURVE_SCALE(3904U);
    const uint32_t p40vol = BATTERY_CURVE_SCALE(3824U);
    const uint32_t p20vol = BATTERY_CURVE_SCALE(3754U);
    const uint32_t p5vol = BATTERY_CURVE_SCALE(3644U);
#else
    // Lite
    const uint32_t p80vol = BATTERY_CURVE_SCALE(3934U);
    const uint32_t p60vol = BATTERY_CURVE_SCALE(3844U);
    const uint32_t p40vol = BATTERY_CURVE_SCALE(3784U);
    const uint32_t p20vol = BATTERY_CURVE_SCALE(3744U);
    const uint32_t p5vol = BATTERY_CURVE_SCALE(3644U);
#endif

    if (VOL >= p100vol) {
        return 100;
    } else if (VOL > p80vol) {
        // 80-100
        return map_clamped(VOL, p80vol, p100vol, 80, 100);
    } else if (VOL > p60vol) {
        // 60-80
        return map_clamped(VOL, p60vol, p80vol, 60, 80);
    } else if (VOL > p40vol) {
        // 40-60
        return map_clamped(VOL, p40vol, p60vol, 40, 60);
    } else if (VOL > p20vol) {
        // 20-40
        return map_clamped(VOL, p20vol, p40vol, 20, 40);
    } else if (VOL > p5vol) {
        // 5-20
        return map_clamped(VOL, p5vol, p20vol, 5, 20);
    }

    return 0;
}

/**@brief Function for initializing services that will be used by the application.
 */
static void services_init(void) {
    uint32_t           err_code;

    // -------------------------------------------------------------
    // Initialize Queued Write Module.
    nrf_ble_qwr_init_t qwr_init = {0};
    qwr_init.error_handler = nrf_qwr_error_handler;

    err_code = nrf_ble_qwr_init(&m_qwr, &qwr_init);
    APP_ERROR_CHECK(err_code);


    // -------------------------------------------------------------
    // Initialize NUS.
    ble_nus_init_t     nus_init;
    memset(&nus_init, 0, sizeof(nus_init));

    nus_init.data_handler = nus_data_handler;

    err_code = ble_nus_init(&m_nus, &nus_init);
    APP_ERROR_CHECK(err_code);

    // -------------------------------------------------------------
    // battery service

    ble_bas_init_t bas_init_obj;

    memset(&bas_init_obj, 0, sizeof(bas_init_obj));

    bas_init_obj.evt_handler          = on_bas_evt;
    bas_init_obj.support_notification = true;
    bas_init_obj.p_report_ref         = NULL;
    bas_init_obj.initial_batt_level   = 100;

    if (settings_get_ble_pairing_enable_first_load()) {
        bas_init_obj.bl_rd_sec        = SEC_MITM;
        bas_init_obj.bl_cccd_wr_sec   = SEC_MITM;
        bas_init_obj.bl_report_rd_sec = SEC_MITM;
    } else {
        bas_init_obj.bl_rd_sec        = SEC_OPEN;
        bas_init_obj.bl_cccd_wr_sec   = SEC_OPEN;
        bas_init_obj.bl_report_rd_sec = SEC_OPEN;
    }

    err_code = ble_bas_init(&m_bas, &bas_init_obj);
    APP_ERROR_CHECK(err_code);
}

/**@brief Function for handling an event from the Connection Parameters Module.
 *
 * @details This function will be called for all events in the Connection Parameters Module
 *          which are passed to the application.
 *
 * @note All this function does is to disconnect. This could have been done by simply setting
 *       the disconnect_on_fail config parameter, but instead we use the event handler
 *       mechanism to demonstrate its use.
 *
 * @param[in] p_evt  Event received from the Connection Parameters Module.
 */
static void on_conn_params_evt(ble_conn_params_evt_t *p_evt) {
    uint32_t err_code;

    if (p_evt->evt_type == BLE_CONN_PARAMS_EVT_FAILED) {
        err_code = sd_ble_gap_disconnect(m_conn_handle, BLE_HCI_CONN_INTERVAL_UNACCEPTABLE);
        APP_ERROR_CHECK(err_code);
    }
}

/**@brief Function for handling errors from the Connection Parameters module.
 *
 * @param[in] nrf_error  Error code containing information about what went wrong.
 */
static void conn_params_error_handler(uint32_t nrf_error) {
    APP_ERROR_HANDLER(nrf_error);
}

/**@brief Function for initializing the Connection Parameters module.
 */
static void conn_params_init(void) {
    uint32_t               err_code;
    ble_conn_params_init_t cp_init;

    memset(&cp_init, 0, sizeof(cp_init));

    cp_init.p_conn_params                  = NULL;
    cp_init.first_conn_params_update_delay = FIRST_CONN_PARAMS_UPDATE_DELAY;
    cp_init.next_conn_params_update_delay  = NEXT_CONN_PARAMS_UPDATE_DELAY;
    cp_init.max_conn_params_update_count   = MAX_CONN_PARAMS_UPDATE_COUNT;
    cp_init.start_on_notify_cccd_handle    = BLE_GATT_HANDLE_INVALID;
    cp_init.disconnect_on_fail             = false;
    cp_init.evt_handler                    = on_conn_params_evt;
    cp_init.error_handler                  = conn_params_error_handler;

    err_code = ble_conn_params_init(&cp_init);
    APP_ERROR_CHECK(err_code);
}

/**@brief Function for handling advertising events.
 *
 * @details This function will be called for advertising events which are passed to the application.
 *
 * @param[in] ble_adv_evt  Advertising event.
 */
static void on_adv_evt(ble_adv_evt_t ble_adv_evt) {
    switch (ble_adv_evt) {
        case BLE_ADV_EVT_FAST:
            NRF_LOG_INFO("BLE_ADV_EVT_FAST");
            break;
        case BLE_ADV_EVT_IDLE:
            NRF_LOG_INFO("BLE_ADV_EVT_IDLE");
            break;
        default:
            break;
    }
}

/**@brief Function for handling BLE events.
 *
 * @param[in]   p_ble_evt   Bluetooth stack event.
 * @param[in]   p_context   Unused.
 */
static void ble_evt_handler(ble_evt_t const *p_ble_evt, void *p_context) {
    ret_code_t err_code;

    switch (p_ble_evt->header.evt_id) {
        case BLE_GAP_EVT_CONNECTED:
            sleep_timer_stop();

            NRF_LOG_INFO("Connected");
            m_conn_handle = p_ble_evt->evt.gap_evt.conn_handle;
            err_code = nrf_ble_qwr_conn_handle_assign(&m_qwr, m_conn_handle);
            APP_ERROR_CHECK(err_code);
            g_is_ble_connected = true;
            // The active advertising set is consumed by the connection. It is
            // restarted from the disconnect handler when needed.
            m_findmy_adv_active = false;
            break;

        case BLE_GAP_EVT_DISCONNECTED:
            NRF_LOG_INFO("Disconnected");
            // LED indication will be changed when advertising starts.
            m_conn_handle = BLE_CONN_HANDLE_INVALID;
            g_is_ble_connected = false;
            if (findmy_beacon_is_enabled()) {
                // FindMy keeps the CPU awake for continuous broadcasting, but
                // the slot LED should not stay lit after the host disconnects.
                findmy_beacon_clear_indicator_leds();
            }
            // A pending FindMy address update could not be applied while the
            // link was up; retry it now that no identity address is bound.
            findmy_beacon_on_ble_disconnected();
            // Restart advertising right away: the device otherwise stays awake but
            // non-discoverable after a disconnect, and a button press while awake only
            // cancels the sleep timer (it does not restart advertising), so the host
            // cannot reconnect until the device sleeps and is woken again.
            if (findmy_beacon_is_enabled()) {
                findmy_beacon_start_if_enabled();
                if (findmy_beacon_connectable_window_active()) {
                    // The button wake window is still valid; keep the normal
                    // connectable advertisement alive until its timer expires.
                    advertising_start(false);
                }
            } else {
                advertising_start(false);
            }
            // call sleep_timer_start *after* unsetting g_is_ble_connected
            sleep_timer_start(SLEEP_DELAY_MS_BLE_DISCONNECTED);
            break;

        case BLE_GAP_EVT_PHY_UPDATE_REQUEST: {
            NRF_LOG_DEBUG("PHY update request.");
            ble_gap_phys_t const phys = {
                .rx_phys = BLE_GAP_PHY_AUTO,
                .tx_phys = BLE_GAP_PHY_AUTO,
            };
            err_code = sd_ble_gap_phy_update(p_ble_evt->evt.gap_evt.conn_handle, &phys);
            APP_ERROR_CHECK(err_code);
        }
        break;

        case BLE_GAP_EVT_SEC_PARAMS_REQUEST:
            // Pairing not supported? No, is supported now, hahahaha...
            // But... the pairing is enable?
            if (settings_get_ble_pairing_enable_first_load()) {
                NRF_LOG_DEBUG("Pairing is enable, The BLE_GAP_EVT_SEC_PARAMS_REQUEST event is handled by the pairing manager.");
            } else {
                err_code = sd_ble_gap_sec_params_reply(m_conn_handle, BLE_GAP_SEC_STATUS_PAIRING_NOT_SUPP, NULL, NULL);
                APP_ERROR_CHECK(err_code);
            }
            break;

        case BLE_GAP_EVT_PASSKEY_DISPLAY: {
            char passkey[BLE_GAP_PASSKEY_LEN + 1];
            memcpy(passkey, p_ble_evt->evt.gap_evt.params.passkey_display.passkey, BLE_GAP_PASSKEY_LEN);
            passkey[BLE_GAP_PASSKEY_LEN] = 0x00;
            NRF_LOG_INFO("=== PASSKEY: %s =====",   nrf_log_push(passkey));
        }
        break;

        case BLE_GATTS_EVT_SYS_ATTR_MISSING:
            // No system attributes have been stored.
            err_code = sd_ble_gatts_sys_attr_set(m_conn_handle, NULL, 0, 0);
            APP_ERROR_CHECK(err_code);
            break;

        case BLE_GATTC_EVT_TIMEOUT:
            // Disconnect on GATT Client timeout event.
            err_code = sd_ble_gap_disconnect(p_ble_evt->evt.gattc_evt.conn_handle,
                                             BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
            APP_ERROR_CHECK(err_code);
            break;

        case BLE_GATTS_EVT_TIMEOUT:
            // Disconnect on GATT Server timeout event.
            err_code = sd_ble_gap_disconnect(p_ble_evt->evt.gatts_evt.conn_handle,
                                             BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
            APP_ERROR_CHECK(err_code);
            break;

        default:
            // No implementation needed.
            UNUSED_VARIABLE(err_code);
            break;
    }
}


/**@brief Function for the SoftDevice initialization.
 *
 * @details This function initializes the SoftDevice and the BLE event interrupt.
 */
static void ble_stack_init(void) {
    ret_code_t err_code;

    err_code = nrf_sdh_enable_request();
    APP_ERROR_CHECK(err_code);

    // Configure the BLE stack using the default settings.
    // Fetch the start address of the application RAM.
    uint32_t ram_start = 0;
    err_code = nrf_sdh_ble_default_cfg_set(APP_BLE_CONN_CFG_TAG, &ram_start);
    APP_ERROR_CHECK(err_code);

    // Try to reserve a second advertising set for the Find My beacon. The
    // S140 headers declare a maximum of one set, so this may be rejected;
    // re-setting the role count is documented to be allowed before enable().
    ble_cfg_t ble_cfg;
    memset(&ble_cfg, 0, sizeof(ble_cfg));
    ble_cfg.gap_cfg.role_count_cfg.adv_set_count      = 2;
    ble_cfg.gap_cfg.role_count_cfg.periph_role_count  = 1;
    ble_cfg.gap_cfg.role_count_cfg.central_role_count = 0;
    ble_cfg.gap_cfg.role_count_cfg.central_sec_count  = 0;
    ret_code_t cfg_err = sd_ble_cfg_set(BLE_GAP_CFG_ROLE_COUNT, &ble_cfg, ram_start);
    m_findmy_adv_two_sets_supported = (cfg_err == NRF_SUCCESS);
    if (!m_findmy_adv_two_sets_supported) {
        NRF_LOG_WARNING("Two advertising sets refused by SoftDevice (%d)", cfg_err);
    }

    // Enable BLE stack.
    err_code = nrf_sdh_ble_enable(&ram_start);
    if ((err_code != NRF_SUCCESS) && m_findmy_adv_two_sets_supported) {
        // Not enough RAM was reserved for two sets; retry with the default.
        NRF_LOG_WARNING("BLE enable failed (%d) with two advertising sets, retrying with one", err_code);
        memset(&ble_cfg, 0, sizeof(ble_cfg));
        ble_cfg.gap_cfg.role_count_cfg.adv_set_count     = 1;
        ble_cfg.gap_cfg.role_count_cfg.periph_role_count = 1;
        APP_ERROR_CHECK(sd_ble_cfg_set(BLE_GAP_CFG_ROLE_COUNT, &ble_cfg, ram_start));
        m_findmy_adv_two_sets_supported = false;
        err_code = nrf_sdh_ble_enable(&ram_start);
    }
    APP_ERROR_CHECK(err_code);

    // Remember the factory random static address so the beacon can restore it
    // when the user turns the beacon off again.
    err_code = sd_ble_gap_addr_get(&m_original_adv_addr);
    if (err_code != NRF_SUCCESS) {
        memset(&m_original_adv_addr, 0, sizeof(m_original_adv_addr));
        m_original_adv_addr.addr_type = BLE_GAP_ADDR_TYPE_RANDOM_STATIC;
    }
    m_applied_adv_addr = m_original_adv_addr;
    NRF_LOG_INFO("FindMy mode: %s", m_findmy_adv_two_sets_supported ? "two advertising sets" : "shared advertising set");

    // Register a handler for BLE events.
    NRF_SDH_BLE_OBSERVER(m_ble_observer, APP_BLE_OBSERVER_PRIO, ble_evt_handler, NULL);
}

/**@brief Function for handling events from the GATT library. */
void gatt_evt_handler(nrf_ble_gatt_t *p_gatt, nrf_ble_gatt_evt_t const *p_evt) {
    if ((m_conn_handle == p_evt->conn_handle) && (p_evt->evt_id == NRF_BLE_GATT_EVT_ATT_MTU_UPDATED)) {
        m_ble_nus_max_data_len = p_evt->params.att_mtu_effective - OPCODE_LENGTH - HANDLE_LENGTH;
        NRF_LOG_INFO("Data len is set to 0x%X(%d)", m_ble_nus_max_data_len, m_ble_nus_max_data_len);
    }
    NRF_LOG_DEBUG("ATT MTU exchange completed. central 0x%x peripheral 0x%x",
                  p_gatt->att_mtu_desired_central,
                  p_gatt->att_mtu_desired_periph);
}


/**@brief Function for initializing the GATT library. */
void gatt_init(void) {
    ret_code_t err_code;

    err_code = nrf_ble_gatt_init(&m_gatt, gatt_evt_handler);
    APP_ERROR_CHECK(err_code);

    err_code = nrf_ble_gatt_att_mtu_periph_set(&m_gatt, NRF_SDH_BLE_GATT_MAX_MTU_SIZE);
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for initializing the Advertising functionality.
 */
static void advertising_init(void) {
    uint32_t               err_code;
    ble_advertising_init_t init;

    memset(&init, 0, sizeof(init));

    init.advdata.name_type          = BLE_ADVDATA_FULL_NAME;
    init.advdata.include_appearance = false;
    init.advdata.flags              = BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE;

    init.srdata.uuids_complete.uuid_cnt = sizeof(m_adv_uuids) / sizeof(m_adv_uuids[0]);
    init.srdata.uuids_complete.p_uuids  = m_adv_uuids;

    init.config.ble_adv_fast_enabled  = true;
    init.config.ble_adv_fast_interval = APP_ADV_INTERVAL;
    init.config.ble_adv_fast_timeout  = 0;
    init.evt_handler = on_adv_evt;

    err_code = ble_advertising_init(&m_advertising, &init);
    APP_ERROR_CHECK(err_code);

    ble_advertising_conn_cfg_tag_set(&m_advertising, APP_BLE_CONN_CFG_TAG);
}

/**@brief Clear bond information from persistent storage.
 */
void delete_bonds_all(void) {
    ret_code_t err_code;

    NRF_LOG_INFO("Erase bonds!");

    err_code = pm_peers_delete();
    APP_ERROR_CHECK(err_code);
}

/**@brief Function for setting filtered whitelist.
 *
 * @param[in] skip  Filter passed to @ref pm_peer_id_list.
 */
static void whitelist_set(pm_peer_id_list_skip_t skip) {
    pm_peer_id_t peer_ids[BLE_GAP_WHITELIST_ADDR_MAX_COUNT];
    uint32_t     peer_id_count = BLE_GAP_WHITELIST_ADDR_MAX_COUNT;

    ret_code_t err_code = pm_peer_id_list(peer_ids, &peer_id_count, PM_PEER_ID_INVALID, skip);
    APP_ERROR_CHECK(err_code);

    NRF_LOG_INFO("Whitelist peer cnt %d, MAX_PEERS_WLIST %d", peer_id_count, BLE_GAP_WHITELIST_ADDR_MAX_COUNT);

    err_code = pm_whitelist_set(peer_ids, peer_id_count);
    APP_ERROR_CHECK(err_code);
}

/**@brief Function for starting advertising.
 */
void advertising_start(bool erase_bonds) {
    if (m_findmy_shares_primary_set && m_findmy_adv_active) {
        // Shared-set mode: the FindMy beacon currently owns the only
        // advertising set. Keep the beacon on air until it is switched off;
        // it restarts the connectable advertisement by itself.
        return;
    }
    if (erase_bonds == true && settings_get_ble_pairing_enable_first_load()) {
        // Advertising is started by PM_EVT_PEERS_DELETE_SUCCEEDED event.
        // So we don't call `ble_advertising_start()` after `delete_bonds_all()`.
        delete_bonds_all();
    } else {
        if (settings_get_ble_pairing_enable_first_load()) {
            whitelist_set(PM_PEER_ID_LIST_SKIP_NO_ID_ADDR);
        }
        ret_code_t ret = ble_advertising_start(&m_advertising, BLE_ADV_MODE_FAST);
        APP_ERROR_CHECK(ret);
    }
}

/**
 * @brief Function for stop advertising.
 */
void advertising_stop(void) {
    if (m_findmy_shares_primary_set && m_findmy_adv_active) {
        // The beacon owns the shared set; stopping the application
        // advertisement must not take the beacon off air.
        return;
    }
    sd_ble_gap_adv_stop(m_advertising.adv_handle);
}

/**@brief Fill the 31-byte Find My (offline finding) advertisement in place.
 *
 * Layout, identical to the OpenHaystack reference firmware:
 *   0x1e, 0xff, 0x4c, 0x00, 0x12, 0x19, status(0),
 *   key[6..27] (22 bytes), key[0] >> 6, hint(0)
 */
static void findmy_adv_data_build(void) {
    m_findmy_adv_data[0] = 0x1e; // AD length: 30 bytes follow
    m_findmy_adv_data[1] = 0xff; // Manufacturer Specific Data
    m_findmy_adv_data[2] = 0x4c; // Apple company ID 0x004C, little endian
    m_findmy_adv_data[3] = 0x00;
    m_findmy_adv_data[4] = 0x12; // Offline Finding type
    m_findmy_adv_data[5] = 0x19; // Offline Finding data length (25)
    m_findmy_adv_data[6] = 0x00; // Status
    memcpy(&m_findmy_adv_data[7], &m_findmy_key[6], 22);
    m_findmy_adv_data[29] = m_findmy_key[0] >> 6; // top two bits displaced by the address marker
    m_findmy_adv_data[30] = 0x00;                 // Hint
}

void ble_findmy_adv_update(const uint8_t key[APP_FINDMY_BEACON_KEY_LEN]) {
    memcpy(m_findmy_key, key, sizeof(m_findmy_key));
    findmy_adv_data_build();
}

/**@brief Apply a new identity address, pausing advertising sets as needed.
 *
 * The SoftDevice only accepts an identity address change while nothing is
 * advertising and no link is up; the connectable advertisement is stopped for
 * the duration of the call and restarted afterwards. An active connection
 * still makes the call fail, which the caller defers.
 */
static bool findmy_apply_identity_addr(const ble_gap_addr_t *addr) {
    // Changing the identity address while a link is up can make the peer
    // disconnect. Keep the new key pending until the disconnect handler runs.
    if (is_nus_working()) {
        NRF_LOG_DEBUG("FindMy address update deferred while connected");
        return false;
    }

    bool restart_advertising = false;
    if (m_advertising.adv_mode_current != BLE_ADV_MODE_IDLE) {
        sd_ble_gap_adv_stop(m_advertising.adv_handle);
        m_advertising.adv_mode_current = BLE_ADV_MODE_IDLE;
        restart_advertising = true;
    }

    ret_code_t err;
    // In shared-set mode the address may only change while no set is running;
    // in two-set mode only the dedicated set has to be paused.
    bool findmy_paused = false;
    if (m_findmy_adv_active && m_findmy_adv_two_sets_supported) {
        sd_ble_gap_adv_stop(m_findmy_adv_handle);
        findmy_paused = true;
    }
    err = sd_ble_gap_addr_set(addr);
    if (findmy_paused) {
        sd_ble_gap_adv_start(m_findmy_adv_handle, APP_BLE_CONN_CFG_TAG);
    }
    if (err == NRF_SUCCESS) {
        m_applied_adv_addr = *addr;
        m_findmy_address_applied = true;
    } else {
        NRF_LOG_DEBUG("FindMy address set refused (%d), will retry later", err);
    }

    if (restart_advertising) {
        advertising_start(false);
    }
    return err == NRF_SUCCESS;
}

bool ble_findmy_set_address(const uint8_t key[APP_FINDMY_BEACON_KEY_LEN]) {
    ble_gap_addr_t addr;
    memset(&addr, 0, sizeof(addr));
    addr.addr_type = BLE_GAP_ADDR_TYPE_RANDOM_STATIC;
    // ble_gap_addr_t::addr is little endian (LSB = on-air octet 0). The key
    // bytes go most significant first on air: octet0 = key[0] | 0b11.
    addr.addr[5] = key[0] | 0xC0;
    addr.addr[4] = key[1];
    addr.addr[3] = key[2];
    addr.addr[2] = key[3];
    addr.addr[1] = key[4];
    addr.addr[0] = key[5];

    if (m_findmy_address_applied && memcmp(addr.addr, m_applied_adv_addr.addr, 6) == 0) {
        return true;
    }
    return findmy_apply_identity_addr(&addr);
}

bool ble_findmy_restore_address(void) {
    if (!m_findmy_address_applied) {
        return true;
    }
    bool ok = findmy_apply_identity_addr(&m_original_adv_addr);
    if (ok) {
        m_findmy_address_applied = false;
    }
    return ok;
}

bool ble_findmy_advertising_start(void) {
    if (m_findmy_adv_active) {
        return true;
    }

    ble_gap_adv_data_t adv_data;
    memset(&adv_data, 0, sizeof(adv_data));
    adv_data.adv_data.p_data = m_findmy_adv_data;
    adv_data.adv_data.len = sizeof(m_findmy_adv_data);

    if (m_findmy_adv_two_sets_supported) {
        // Dedicated second set; the connectable advertisement keeps running.
        ret_code_t err = sd_ble_gap_adv_set_configure(&m_findmy_adv_handle, &adv_data, &m_findmy_adv_params);
        if (err != NRF_SUCCESS) {
            if (err == NRF_ERROR_NO_MEM || err == NRF_ERROR_RESOURCES) {
                // The SoftDevice accepted the two-set configuration at boot
                // but cannot hand out a second handle; use the shared set.
                NRF_LOG_WARNING("Second advertising set unavailable (%d), falling back to shared set", err);
                m_findmy_adv_handle = BLE_GAP_ADV_SET_HANDLE_NOT_SET;
                m_findmy_adv_two_sets_supported = false;
                return ble_findmy_advertising_start();
            }
            NRF_LOG_ERROR("FindMy set configure failed: %d", err);
            return false;
        }
        err = sd_ble_gap_adv_start(m_findmy_adv_handle, APP_BLE_CONN_CFG_TAG);
        if (err != NRF_SUCCESS) {
            NRF_LOG_ERROR("FindMy set start failed: %d", err);
            return false;
        }
        m_findmy_adv_active = true;
        return true;
    }

    // Shared-set fallback: the beacon needs the only advertising set, so the
    // connectable advertisement pauses until the beacon is switched off. The
    // handle must be the ble_advertising module's own variable: the SoftDevice
    // hands out its single handle once, so configuring a separate handle here
    // would leave the module unable to advertise again.
    if (m_conn_handle != BLE_CONN_HANDLE_INVALID) {
        NRF_LOG_WARNING("FindMy beacon cannot take the shared set while connected");
        return false;
    }
    sd_ble_gap_adv_stop(m_advertising.adv_handle);
    m_advertising.adv_mode_current = BLE_ADV_MODE_IDLE;
    ble_findmy_set_address(m_findmy_key);
    ret_code_t err = sd_ble_gap_adv_set_configure(&m_advertising.adv_handle, &adv_data, &m_findmy_adv_params);
    if (err != NRF_SUCCESS) {
        NRF_LOG_ERROR("FindMy shared set configure failed: %d", err);
        advertising_start(false);
        return false;
    }
    m_findmy_shares_primary_set = true;
    err = sd_ble_gap_adv_start(m_advertising.adv_handle, APP_BLE_CONN_CFG_TAG);
    if (err != NRF_SUCCESS) {
        NRF_LOG_ERROR("FindMy shared set start failed: %d", err);
        m_findmy_shares_primary_set = false;
        advertising_start(false);
        return false;
    }
    m_findmy_adv_active = true;
    return true;
}

void ble_findmy_advertising_stop(void) {
    if (!m_findmy_adv_active) {
        return;
    }

    sd_ble_gap_adv_stop(m_findmy_shares_primary_set ? m_advertising.adv_handle : m_findmy_adv_handle);
    m_findmy_adv_active = false;

    if (m_findmy_shares_primary_set) {
        // Hand the set back to the ble_advertising module and restore the
        // factory identity address. The module keeps its handle, so it can
        // simply reconfigure and restart.
        m_findmy_shares_primary_set = false;
        ble_findmy_restore_address();
        advertising_start(false);
    }
}

bool ble_findmy_is_advertising(void) {
    return m_findmy_adv_active;
}

bool ble_findmy_enter_connectable_mode(void) {
    if (is_nus_working()) {
        return false;
    }
    if (m_findmy_adv_active) {
        // Shared-set mode restores the factory address and starts the normal
        // connectable advertisement. Two-set mode already has it running.
        ble_findmy_advertising_stop();
    } else if (m_advertising.adv_mode_current == BLE_ADV_MODE_IDLE) {
        advertising_start(false);
    }
    return true;
}

bool ble_findmy_uses_two_sets(void) {
    return m_findmy_adv_two_sets_supported;
}

/**@brief Function for handling Peer Manager events.
 *
 * @param[in] p_evt  Peer Manager event.
 */
static void pm_evt_handler(pm_evt_t const *p_evt) {
    pm_handler_on_pm_evt(p_evt);
    pm_handler_disconnect_on_sec_failure(p_evt);
    pm_handler_flash_clean(p_evt);

    switch (p_evt->evt_id) {
        case PM_EVT_CONN_SEC_SUCCEEDED:
            // p_evt->peer_id;
            break;

        case PM_EVT_PEERS_DELETE_SUCCEEDED:
            advertising_start(false);
            break;

        case PM_EVT_PEER_DATA_UPDATE_SUCCEEDED:
            if (p_evt->params.peer_data_update_succeeded.flash_changed
                    && (p_evt->params.peer_data_update_succeeded.data_id == PM_PEER_DATA_ID_BONDING)) {
                NRF_LOG_INFO("New Bond, add the peer to the whitelist if possible");
                // Note: You should check on what kind of white list policy your application should use.

                whitelist_set(PM_PEER_ID_LIST_SKIP_NO_ID_ADDR);
            }
            break;
        case PM_EVT_CONN_SEC_CONFIG_REQ: {
            pm_conn_sec_config_t cfg;
            cfg.allow_repairing = true;
            pm_conn_sec_config_reply(p_evt->conn_handle, &cfg);
        }
        break;
        default:
            break;
    }
}

/**@brief Function for the Peer Manager initialization.
 */
static void peer_manager_init(void) {
    ble_gap_sec_params_t sec_param;
    ret_code_t           err_code;

    err_code = pm_init();
    APP_ERROR_CHECK(err_code);

    memset(&sec_param, 0, sizeof(ble_gap_sec_params_t));

    // Security parameters to be used for all security procedures.
    sec_param.bond           = SEC_PARAMS_BOND;
    sec_param.mitm           = SEC_PARAMS_MITM;
    sec_param.lesc           = SEC_PARAMS_LESC;
    sec_param.keypress       = SEC_PARAMS_KEYPRESS;
    sec_param.io_caps        = SEC_PARAMS_IO_CAPABILITIES;
    sec_param.oob            = SEC_PARAMS_OOB;
    sec_param.min_key_size   = SEC_PARAMS_MIN_KEY_SIZE;
    sec_param.max_key_size   = SEC_PARAMS_MAX_KEY_SIZE;
    sec_param.kdist_own.enc  = 1;
    sec_param.kdist_own.id   = 1;
    sec_param.kdist_peer.enc = 1;
    sec_param.kdist_peer.id  = 1;

    err_code = pm_sec_params_set(&sec_param);
    APP_ERROR_CHECK(err_code);

    err_code = pm_register(pm_evt_handler);
    APP_ERROR_CHECK(err_code);
}

/**@brief Function for handling the ADC interrupt.
 *
 * @details  This function fetchs conversion result from the ADC, then callback lf with samples.
 */
void saadc_event_handler(nrfx_saadc_evt_t const *p_event) {
    if (p_event->type == NRFX_SAADC_EVT_DONE && m_lf_adc_callback != NULL) {
        ret_code_t err_code;
        err_code = nrfx_saadc_buffer_convert(&adc_buf[next_free_buf_index()][0], ADC_BUF_SIZE);
        APP_ERROR_CHECK(err_code);

        m_lf_adc_callback(p_event->data.done.p_buffer, p_event->data.done.size);
    }
}

/**@brief Function for configuring ADC to do battery level conversion.
 */
static void adc_configure(void) {
    ret_code_t err_code;

    nrfx_saadc_config_t cfg = NRFX_SAADC_DEFAULT_CONFIG;
    err_code = nrfx_saadc_init(&cfg, saadc_event_handler);
    APP_ERROR_CHECK(err_code);

    nrf_saadc_channel_config_t ch = NRFX_SAADC_DEFAULT_CHANNEL_CONFIG_SE(BAT_SENSE);
    err_code = nrfx_saadc_channel_init(ADC_CHANNEL, &ch);
    APP_ERROR_CHECK(err_code);
}

/**@brief Function for handling the Battery measurement timer timeout.
 *
 * @details This function will be called each time the battery level measurement timer expires.
 *          This function will start the ADC.
 *
 * @param[in] p_context   Pointer used for passing some arbitrary information (context) from the
 *                        app_start_timer() call to the timeout handler.
 */
static void battery_level_meas_timeout_handler(void *p_context) {
    UNUSED_PARAMETER(p_context);

    // When LF sample is enabled, PWM triggers ADC samples through PPI, at 125khz sample rate.
    // To avoid corrupt LF samples, here we ignore battery level measure timer for a while.
    if (m_lf_adc_callback != NULL) {
        return;
    }

    // Here we fetch the conversion result from the ADC, convert the value into
    // percentage and send it to peer.
    ret_code_t err_code;
    nrf_saadc_value_t adc_result;
    err_code = nrfx_saadc_sample_convert(ADC_CHANNEL, &adc_result);
    APP_ERROR_CHECK(err_code);

    batt_lvl_in_milli_volts = ADC_RESULT_IN_MILLI_VOLTS(adc_result) + 100;
    percentage_batt_lvl = BATVOL2PERCENT(batt_lvl_in_milli_volts);

    // if battery service is notification enable, we can send msg to device.
    err_code = ble_bas_battery_level_update(&m_bas, percentage_batt_lvl, BLE_CONN_HANDLE_ALL);
    if ((err_code != NRF_SUCCESS) &&
            (err_code != NRF_ERROR_INVALID_STATE) &&
            (err_code != NRF_ERROR_RESOURCES) &&
            (err_code != NRF_ERROR_BUSY) &&
            (err_code != NRF_ERROR_FORBIDDEN) &&
            (err_code != BLE_ERROR_GATTS_SYS_ATTR_MISSING)) {
        APP_ERROR_HANDLER(err_code);
    }

    // check low battery level, if level == 0, we can try to shutdown.
    if (percentage_batt_lvl == 0) {
        NRF_LOG_INFO("battery too low, try to shutdown...");
        g_is_low_battery_shutdown = true;
        sleep_timer_start(SLEEP_NO_BATTERY_SHUTDOWN);
    } else {
        g_is_low_battery_shutdown = false;
    }
}

void create_battery_timer(void) {
    ret_code_t err_code;
    // Create battery timer.
    err_code = app_timer_create(&m_battery_timer_id, APP_TIMER_MODE_REPEATED, battery_level_meas_timeout_handler);
    APP_ERROR_CHECK(err_code);
    // Start battery timer
    err_code = app_timer_start(m_battery_timer_id, BATTERY_LEVEL_MEAS_INTERVAL, NULL);
    APP_ERROR_CHECK(err_code);
}

/**
 * @brief Function for init ble slave.
 */
void ble_slave_init(void) {
    adc_configure();                    // ADC initialization
    create_battery_timer();             // Create a battery power update timer
    ble_stack_init();                   // BLE protocol stack initialization
    gap_params_init();                  // GAP parameter initialization
    gatt_init();                        // Gatt protocol initialization
    services_init();                    // Initialization of service characteristics
    advertising_init();                 // Broadcast parameter initialization
    conn_params_init();                 // Connection parameter initialization
    peer_manager_init();                // Peer manager Initialization
}

void register_lf_adc_callback(lf_adc_callback_t cb) {
    m_lf_adc_callback = cb;

    nrfx_saadc_uninit();

    ret_code_t err_code;

    nrfx_saadc_config_t cfg = NRFX_SAADC_DEFAULT_CONFIG;
    err_code = nrfx_saadc_init(&cfg, saadc_event_handler);
    APP_ERROR_CHECK(err_code);

    nrf_saadc_channel_config_t ch = NRFX_SAADC_DEFAULT_CHANNEL_CONFIG_SE(NRF_SAADC_INPUT_AIN5);
    ch.acq_time = NRF_SAADC_ACQTIME_5US;
    err_code = nrfx_saadc_channel_init(ADC_CHANNEL, &ch);
    APP_ERROR_CHECK(err_code);

    err_code = nrfx_saadc_buffer_convert(&adc_buf[next_free_buf_index()][0], ADC_BUF_SIZE);
    APP_ERROR_CHECK(err_code);

    err_code = nrfx_saadc_buffer_convert(&adc_buf[next_free_buf_index()][0], ADC_BUF_SIZE);
    APP_ERROR_CHECK(err_code);
}

void unregister_lf_adc_callback(void) {
    nrfx_saadc_uninit();
    adc_configure();
    m_lf_adc_callback = NULL;
}
