#include "tag_emulation.h"

#include "crc_utils.h"
#include "fds_ids.h"
#include "fds_util.h"
#include "lf_tag_em.h"
#include "nfc_14a.h"
#include "nfc_mf0_ntag.h"
#include "nfc_mf1.h"
#include "nfc_14a_4.h"
#include "nfc_seos.h"
#include "rgb_marquee.h"
#include "settings.h"
#include "tag_persistence.h"

#define NRF_LOG_MODULE_NAME tag_emu
#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "nrf_log_default_backends.h"
NRF_LOG_MODULE_REGISTER();

/*
 * A card slot can simulate up to two cards at the same time, one ID 125kHz EM410X, and one IC 13.56MHz 14A.(May be able to support more in the future)
 * When starting, you should start the startup listener on demand (there is no emulated card when there is no data, but you need to monitor the state on demand)
 * If the retrieved card slot configuration has a specified type of card, then loading the specified type of data should be carried out, and the necessary parameters of initialization should be performed.
 * When the on-field entry is detected, in addition to the relevant LED, you also need to start the emulation card according to whether the current data is loaded.
 * In the emulation card, all operations should be carried out based on the data loaded in RAM. After the analog card is over, the modified data should be preserved to Flash
 *
 * ......
 */

// Is the logo in the analog card?
bool g_is_tag_emulating = false;

static tag_specific_type_t tag_specific_type_old2new_lf_values[][2] = {TAG_SPECIFIC_TYPE_OLD2NEW_LF_VALUES};
static tag_specific_type_t tag_specific_type_old2new_hf_values[][2] = {TAG_SPECIFIC_TYPE_OLD2NEW_HF_VALUES};
static tag_specific_type_t tag_specific_type_lf_values[] = {TAG_SPECIFIC_TYPE_LF_VALUES};
static tag_specific_type_t tag_specific_type_hf_values[] = {TAG_SPECIFIC_TYPE_HF_VALUES};

bool is_tag_specific_type_valid(tag_specific_type_t tag_type) {
    bool valid = false;
    for (uint16_t i = 0; i < ARRAYLEN(tag_specific_type_lf_values); i++) {
        valid |= (tag_type == tag_specific_type_lf_values[i]);
    }
    for (uint16_t i = 0; i < ARRAYLEN(tag_specific_type_hf_values); i++) {
        valid |= (tag_type == tag_specific_type_hf_values[i]);
    }
    return valid;
}
// **********************  Specific parameters start **********************

/**
 * Tag data stored in flash. Total length must be aligned by 4 bytes (whole words).
 */
static uint8_t m_tag_data_buffer_lf[20];  // LF card data buffer
static uint16_t m_tag_data_lf_crc;
static tag_data_buffer_t m_tag_data_lf = {sizeof(m_tag_data_buffer_lf), m_tag_data_buffer_lf, &m_tag_data_lf_crc};

static uint8_t m_tag_data_buffer_hf[4500];  // HF card data buffer
static uint16_t m_tag_data_hf_crc;
static tag_data_buffer_t m_tag_data_hf = {sizeof(m_tag_data_buffer_hf), m_tag_data_buffer_hf, &m_tag_data_hf_crc};

/**
 * Up to 255 card slots, visible slot count is dynamic (default 8)
 */
static tag_slot_config_t slotConfig ALIGN_U32 = {
    // Configure activated card slot, default activation of the 0th card slot (the first card)
    .version = TAG_SLOT_CONFIG_CURRENT_VERSION,
    .active_slot = 0,
    .slot_count = TAG_SLOT_COUNT_MIN,
    // Configuration card slots
    // See tag_emulation_factory_init for actual tag content.
    // Slots not listed here keep the zero initializer: disabled and TAG_TYPE_UNDEFINED.
    .slots = {
        { .enabled_hf = true,  .enabled_lf = true,  .tag_hf = TAG_TYPE_MIFARE_1024, .tag_lf = TAG_TYPE_EM410X,    },  // 1
        { .enabled_hf = true,  .enabled_lf = false, .tag_hf = TAG_TYPE_MF0ICU1,     .tag_lf = TAG_TYPE_UNDEFINED, },  // 2
        { .enabled_hf = false, .enabled_lf = true,  .tag_hf = TAG_TYPE_UNDEFINED,   .tag_lf = TAG_TYPE_EM410X,    },  // 3
        { .enabled_hf = false, .enabled_lf = false, .tag_hf = TAG_TYPE_UNDEFINED,   .tag_lf = TAG_TYPE_UNDEFINED, },  // 4
        { .enabled_hf = false, .enabled_lf = false, .tag_hf = TAG_TYPE_UNDEFINED,   .tag_lf = TAG_TYPE_UNDEFINED, },  // 5
        { .enabled_hf = false, .enabled_lf = false, .tag_hf = TAG_TYPE_UNDEFINED,   .tag_lf = TAG_TYPE_UNDEFINED, },  // 6
        { .enabled_hf = false, .enabled_lf = false, .tag_hf = TAG_TYPE_UNDEFINED,   .tag_lf = TAG_TYPE_UNDEFINED, },  // 7
        { .enabled_hf = false, .enabled_lf = false, .tag_hf = TAG_TYPE_UNDEFINED,   .tag_lf = TAG_TYPE_UNDEFINED, },  // 8
    },
};
// The card slot configuration unique CRC, once the slot configuration changes, can be checked by CRC
static uint16_t m_slot_config_crc;

// Compact per-slot metadata cache and MRU list for smart polling.
static tag_slot_meta_t m_slot_meta[TAG_MAX_SLOT_NUM];
static uint8_t m_mru[TAG_SLOT_MRU_MAX];
static uint8_t m_mru_count;
static uint8_t m_smart_round;
static uint8_t m_last_reader_sector = 0xFF;

// ********************** Specific parameter ends **********************

/**
 * The data of the tag is loaded to the RAM and the mapping table of the operation of the regulating notification,
 * The mapping structure is:
 * Field -type detailed tag type Loading data The notification of the notification of the notification of the notification of the call recovery data before saving the data of the realization data of the function card data
 */
static tag_base_handler_map_t tag_base_map[] = {
    // LF tag emulation
    {TAG_SENSE_LF, TAG_TYPE_EM410X,      lf_tag_data_loadcb,           lf_tag_em410x_data_savecb,    lf_tag_em410x_data_factory,    &m_tag_data_lf},
    {TAG_SENSE_LF, TAG_TYPE_EM410X_ELECTRA, lf_tag_data_loadcb,        lf_tag_em410x_data_savecb,    lf_tag_em410x_data_factory,    &m_tag_data_lf},
    {TAG_SENSE_LF, TAG_TYPE_HID_PROX,    lf_tag_data_loadcb,           lf_tag_hidprox_data_savecb,   lf_tag_hidprox_data_factory,   &m_tag_data_lf},
    {TAG_SENSE_LF, TAG_TYPE_IOPROX,      lf_tag_data_loadcb,           lf_tag_ioprox_data_savecb,    lf_tag_ioprox_data_factory,    &m_tag_data_lf},
    {TAG_SENSE_LF, TAG_TYPE_VIKING,      lf_tag_data_loadcb,           lf_tag_viking_data_savecb,    lf_tag_viking_data_factory,    &m_tag_data_lf},
    {TAG_SENSE_LF, TAG_TYPE_PAC,         lf_tag_data_loadcb,           lf_tag_pac_data_savecb,       lf_tag_pac_data_factory,       &m_tag_data_lf},
    {TAG_SENSE_LF, TAG_TYPE_JABLOTRON,   lf_tag_data_loadcb,           lf_tag_jablotron_data_savecb, lf_tag_jablotron_data_factory, &m_tag_data_lf},
    {TAG_SENSE_LF, TAG_TYPE_IDTECK,      lf_tag_data_loadcb,           lf_tag_idteck_data_savecb,    lf_tag_idteck_data_factory,    &m_tag_data_lf},
    {TAG_SENSE_LF, TAG_TYPE_FARPOINTE_PYRAMID, lf_tag_data_loadcb,     lf_tag_pyramid_data_savecb,   lf_tag_pyramid_data_factory,   &m_tag_data_lf},
    // MF1 tag emulation
    {TAG_SENSE_HF, TAG_TYPE_MIFARE_Mini, nfc_tag_mf1_data_loadcb,      nfc_tag_mf1_data_savecb,      nfc_tag_mf1_data_factory,      &m_tag_data_hf},
    {TAG_SENSE_HF, TAG_TYPE_MIFARE_1024, nfc_tag_mf1_data_loadcb,      nfc_tag_mf1_data_savecb,      nfc_tag_mf1_data_factory,      &m_tag_data_hf},
    {TAG_SENSE_HF, TAG_TYPE_MIFARE_2048, nfc_tag_mf1_data_loadcb,      nfc_tag_mf1_data_savecb,      nfc_tag_mf1_data_factory,      &m_tag_data_hf},
    {TAG_SENSE_HF, TAG_TYPE_MIFARE_4096, nfc_tag_mf1_data_loadcb,      nfc_tag_mf1_data_savecb,      nfc_tag_mf1_data_factory,      &m_tag_data_hf},
    // NTAG tag emulation
    {TAG_SENSE_HF, TAG_TYPE_NTAG_210,    nfc_tag_mf0_ntag_data_loadcb, nfc_tag_mf0_ntag_data_savecb, nfc_tag_mf0_ntag_data_factory, &m_tag_data_hf},
    {TAG_SENSE_HF, TAG_TYPE_NTAG_212,    nfc_tag_mf0_ntag_data_loadcb, nfc_tag_mf0_ntag_data_savecb, nfc_tag_mf0_ntag_data_factory, &m_tag_data_hf},
    {TAG_SENSE_HF, TAG_TYPE_NTAG_213,    nfc_tag_mf0_ntag_data_loadcb, nfc_tag_mf0_ntag_data_savecb, nfc_tag_mf0_ntag_data_factory, &m_tag_data_hf},
    {TAG_SENSE_HF, TAG_TYPE_NTAG_215,    nfc_tag_mf0_ntag_data_loadcb, nfc_tag_mf0_ntag_data_savecb, nfc_tag_mf0_ntag_data_factory, &m_tag_data_hf},
    {TAG_SENSE_HF, TAG_TYPE_NTAG_216,    nfc_tag_mf0_ntag_data_loadcb, nfc_tag_mf0_ntag_data_savecb, nfc_tag_mf0_ntag_data_factory, &m_tag_data_hf},
    // MF0 tag emulation
    {TAG_SENSE_HF, TAG_TYPE_MF0ICU1,     nfc_tag_mf0_ntag_data_loadcb, nfc_tag_mf0_ntag_data_savecb, nfc_tag_mf0_ntag_data_factory, &m_tag_data_hf},
    {TAG_SENSE_HF, TAG_TYPE_MF0ICU2,     nfc_tag_mf0_ntag_data_loadcb, nfc_tag_mf0_ntag_data_savecb, nfc_tag_mf0_ntag_data_factory, &m_tag_data_hf},
    {TAG_SENSE_HF, TAG_TYPE_MF0UL11,     nfc_tag_mf0_ntag_data_loadcb, nfc_tag_mf0_ntag_data_savecb, nfc_tag_mf0_ntag_data_factory, &m_tag_data_hf},
    {TAG_SENSE_HF, TAG_TYPE_MF0UL21,     nfc_tag_mf0_ntag_data_loadcb, nfc_tag_mf0_ntag_data_savecb, nfc_tag_mf0_ntag_data_factory, &m_tag_data_hf},
    // ISO14443-4 T=CL emulation
    {TAG_SENSE_HF, TAG_TYPE_HF14A_4,     nfc_tag_14a_4_data_loadcb,    nfc_tag_14a_4_data_savecb,    nfc_tag_14a_4_data_factory,    &m_tag_data_hf},
    {TAG_SENSE_HF, TAG_TYPE_SEOS,        nfc_tag_seos_data_loadcb,     nfc_tag_seos_data_savecb,     nfc_tag_seos_data_factory,     &m_tag_data_hf},
};

static void tag_emulation_load_config(void);
static void tag_emulation_save_config(void);

/**
 * get the data loader for the specific type of tag
 */
static tag_datas_loadcb_t get_data_loadcb_from_tag_type(tag_specific_type_t type) {
    for (int i = 0; i < ARRAY_SIZE(tag_base_map); i++) {
        if (tag_base_map[i].tag_type == type) {
            return tag_base_map[i].data_on_load;
        }
    }
    return NULL;
}

/**
 * accordingToTheSpecifiedDetailedLabelType,ObtainTheOperationFunctionBeforeTheDataPreservationOfTheData
 */
static tag_datas_savecb_t get_data_savecb_from_tag_type(tag_specific_type_t type) {
    for (int i = 0; i < ARRAY_SIZE(tag_base_map); i++) {
        if (tag_base_map[i].tag_type == type) {
            return tag_base_map[i].data_on_save;
        }
    }
    return NULL;
}

/**
 * get factory data for specific tag type
 */
static tag_datas_factory_t get_data_factory_from_tag_type(tag_specific_type_t type) {
    for (int i = 0; i < ARRAY_SIZE(tag_base_map); i++) {
        if (tag_base_map[i].tag_type == type) {
            return tag_base_map[i].data_factory;
        }
    }
    return NULL;
}

/**
 * accordingToTheSpecifiedDetailedLabelType,ObtainItsBasicFieldInductionType
 */
tag_sense_type_t get_sense_type_from_tag_type(tag_specific_type_t type) {
    for (int i = 0; i < ARRAY_SIZE(tag_base_map); i++) {
        if (tag_base_map[i].tag_type == type) {
            return tag_base_map[i].sense_type;
        }
    }
    return TAG_SENSE_NO;
}

/**
 * Get buffer data according to tag type.
 */
tag_data_buffer_t *get_buffer_by_tag_type(tag_specific_type_t type) {
    if (type == TAG_TYPE_UNDEFINED) {
        return NULL;
    }
    for (int i = 0; i < ARRAY_SIZE(tag_base_map); i++) {
        if (tag_base_map[i].tag_type == type) {
            return tag_base_map[i].data_buffer;
        }
    }
    NRF_LOG_ERROR("no buffer valid for tag type %d.", type);
    return NULL;
}

/**
 * Load data from memory to the emulated card data.
 */
bool tag_emulation_load_by_buffer(tag_specific_type_t tag_type, bool update_crc) {
    // data has been read to buffer,
    // here we load buffer to the emulator to config pwm seq for the activated card slot.
    tag_datas_loadcb_t loader = get_data_loadcb_from_tag_type(tag_type);
    if (loader == NULL) {
        NRF_LOG_INFO("no data loader exists for the tag type.");
        return false;
    }

    tag_data_buffer_t *buffer = get_buffer_by_tag_type(tag_type);
    if (buffer == NULL) {
        return false;
    }

    int length = loader(tag_type, buffer);
    if (length > 0 && update_crc) {
        // afterReadingIsCompleted,WeCanSaveACrcOfTheCurrentDataWhenItIsStoredLater,ItCanBeUsedAsAReferenceForChangesComparison
        calc_14a_crc_lut(buffer->buffer, length, (uint8_t *)buffer->crc);
        return true;
    }
    return false;
}

/**
 * Load card data based on tag type.
 */
static void load_data_by_tag_type(uint8_t slot, tag_specific_type_t tag_type) {
    // maybeTheCardSlotIsNotEnabledToUseTheemulationOfThisTypeOfLabel,AndSkipTheDataDirectlyToLoadThisData
    if (tag_type == TAG_TYPE_UNDEFINED) {
        return;
    }

    tag_data_buffer_t *buffer = get_buffer_by_tag_type(tag_type);
    if (buffer == NULL) {
        return;
    }

    tag_sense_type_t sense_type = get_sense_type_from_tag_type(tag_type);

    // get fds record for the card slot
    fds_slot_record_map_t map_info;
    get_fds_map_by_slot_sense_type_for_dump(slot, sense_type, &map_info);

    // load data to the buffer according to the card slot currently activated.
    // If the length of data does not match the length of the buffer,
    // it may be caused by the firmware update at this time, the data must be deleted and rebuilt.
    uint16_t length = buffer->length;
    bool ret = fds_read_sync(map_info.id, map_info.key, &length, buffer->buffer);
    if (false == ret) {
        NRF_LOG_INFO("tag slot data no exists.");
        return;
    }
    ret = tag_emulation_load_by_buffer(tag_type, true);
    if (ret) {
        NRF_LOG_INFO("load tag data in slot %d, type %d done.", slot, tag_type);
    }
}

/**
 * Save data according to the type
 */
static void save_data_by_tag_type(uint8_t slot, tag_specific_type_t tag_type) {
    // Maybe the card slot is not enabled to use the emulation of this type of label, and skip it directly to save this data
    if (tag_type == TAG_TYPE_UNDEFINED) {
        return;
    }

    tag_data_buffer_t *buffer = get_buffer_by_tag_type(tag_type);
    if (buffer == NULL) {
        return;
    }

    // The length of the data to be saved by the user should not exceed the size of the global buffer
    int data_byte_length = 0;
    tag_datas_savecb_t fn_savecb = get_data_savecb_from_tag_type(tag_type);
    if (fn_savecb == NULL) {  // Make sure that there is a real estate process
        NRF_LOG_INFO("Tag data saver no impl.");
        return;
    }

    data_byte_length = fn_savecb(tag_type, buffer);
    // Make sure to save data, we can judge whether the data has changed through CRC
    if (data_byte_length <= 0) {
        NRF_LOG_INFO("Tag type %d data no save.", tag_type);
        return;
    }
    // Make sure that the data to be stored is not greater than the size of the current buffer area
    if (data_byte_length > buffer->length) {
        NRF_LOG_ERROR("Tag data save length overflow.", tag_type);
        return;
    }
    uint16_t crc;
    calc_14a_crc_lut(buffer->buffer, data_byte_length, (uint8_t *)&crc);
    // Determine whether the data has changed
    if (crc == *buffer->crc) {
        NRF_LOG_INFO("Tag slot data no change, length = %d", data_byte_length);
        return;
    }
    tag_sense_type_t sense_type = get_sense_type_from_tag_type(tag_type);
    // Get the special card slot FDS record information
    fds_slot_record_map_t map_info;
    get_fds_map_by_slot_sense_type_for_dump(slot, sense_type, &map_info);
    // Call the blocked FDS to write the function, and write the data of the specified field type of the card slot into the Flash
    bool ret = fds_write_sync(map_info.id, map_info.key, data_byte_length, buffer->buffer);
    if (ret) {
        NRF_LOG_INFO("Save tag slot data success.");
    } else {
        NRF_LOG_ERROR("Save tag slot data error.");
    }
    // After the preservation is completed, the CRC of the BUFFER in the corresponding memory
    *buffer->crc = crc;
}

/**
 * Delete data according to the type
 */
static void delete_data_by_tag_type(uint8_t slot, tag_sense_type_t sense_type) {
    if (sense_type == TAG_SENSE_NO) {
        return;
    }
    fds_slot_record_map_t map_info;
    get_fds_map_by_slot_sense_type_for_dump(slot, sense_type, &map_info);
    int count = fds_delete_sync(map_info.id, map_info.key);
    NRF_LOG_INFO("Slot %d delete sense type %d data, record count: %d", slot, sense_type, count);
}

/**
 * Load the emulation card data data. Note that loading is just data operation,
 * Start the analog card, please call tag_emulation_sense_run function, otherwise you will not sensor the field event
 */
void tag_emulation_load_data(void) {
    uint8_t slot = tag_emulation_get_slot();
    load_data_by_tag_type(slot, slotConfig.slots[slot].tag_hf);
    load_data_by_tag_type(slot, slotConfig.slots[slot].tag_lf);
    tag_emulation_slot_mru_touch(slot);
    tag_emulation_slot_meta_update(slot);
}

/**
 *Save the emulated card configuration data. At the right time, this function should be called for data preservation of data
 */
void tag_emulation_save_data(void) {
    uint8_t slot = tag_emulation_get_slot();
    save_data_by_tag_type(slot, slotConfig.slots[slot].tag_hf);
    save_data_by_tag_type(slot, slotConfig.slots[slot].tag_lf);
    tag_emulation_slot_meta_update(slot);
}

/**
 * @brief Get the type of labeling of the emulation card from the corresponding card slot.
 *
 * @param slot Card slot
 * @param tag_type Label
 */
void tag_emulation_get_specific_types_by_slot(uint8_t slot, tag_slot_specific_type_t *tag_types) {
    tag_types->tag_hf = slotConfig.slots[slot].tag_hf;
    tag_types->tag_lf = slotConfig.slots[slot].tag_lf;
}

/**
 * Delete the data specified by a card slot, if it is the current activated card slot data, we also need to dynamically close the emulation of this card
 */
void tag_emulation_delete_data(uint8_t slot, tag_sense_type_t sense_type) {
    // delete data
    delete_data_by_tag_type(slot, sense_type);
    // Close the corresponding card type of the corresponding card slot
    switch (sense_type) {
        case TAG_SENSE_HF: {
            slotConfig.slots[slot].tag_hf = TAG_TYPE_UNDEFINED;
            slotConfig.slots[slot].enabled_hf = false;
        }
        break;
        case TAG_SENSE_LF: {
            slotConfig.slots[slot].tag_lf = TAG_TYPE_UNDEFINED;
            slotConfig.slots[slot].enabled_lf = false;
        }
        break;
        default:
            break;
    }
    // If the deleted card slot data is currently activated (being emulated), we also need to make dynamic shutdown
    if (slotConfig.active_slot == slot) {
        tag_emulation_sense_switch(sense_type, false);
    }
}

/**
 * Set the data of a card slot to the preset data from the factory
 */
bool tag_emulation_factory_data(uint8_t slot, tag_specific_type_t tag_type) {
    tag_datas_factory_t factory = get_data_factory_from_tag_type(tag_type);
    // The process of implementing the data formatting data!
    if (factory != NULL && factory(slot, tag_type)) {
        // If the current data card slot number currently set is the current activated card slot, then we need to update to the memory
        if (tag_emulation_get_slot() == slot) {
            load_data_by_tag_type(slot, tag_type);
        }
        return true;
    }
    return false;
}

/**
 * Switch field induction monitoring status
 * @param enable: Whether to make the field induction
 */
static void tag_emulation_sense_switch_all(bool enable) {
    uint8_t slot = tag_emulation_get_slot();
    // NRF_LOG_INFO("Slot %d tag type hf %d, lf %d", slot, slotConfig.slots[slot].tag_hf, slotConfig.slots[slot].tag_lf);
    if (enable && (slotConfig.slots[slot].enabled_hf) && (slotConfig.slots[slot].tag_hf != TAG_TYPE_UNDEFINED)) {
        nfc_tag_14a_sense_switch(true);
    } else {
        nfc_tag_14a_sense_switch(false);
    }
    if (enable && (slotConfig.slots[slot].enabled_lf) && (slotConfig.slots[slot].tag_lf != TAG_TYPE_UNDEFINED)) {
        lf_tag_125khz_sense_switch(true);
    } else {
        lf_tag_125khz_sense_switch(false);
    }
}

/**
 * Switch field induction monitoring status
 * @param type: Field sensor type
 * @param enable: Whether to enable this type of field induction
 */
void tag_emulation_sense_switch(tag_sense_type_t type, bool enable) {
    uint8_t slot = tag_emulation_get_slot();
    // Check the parameters, not allowed to switch non -normal field
    switch (type) {
        case TAG_SENSE_NO:
            APP_ERROR_CHECK(NRF_ERROR_INVALID_PARAM);
            break;
        case TAG_SENSE_HF:
            if (enable && (slotConfig.slots[slot].enabled_hf) &&
                    (slotConfig.slots[slot].tag_hf != TAG_TYPE_UNDEFINED)) {
                nfc_tag_14a_sense_switch(true);
            } else {
                nfc_tag_14a_sense_switch(false);
            }
            break;
        case TAG_SENSE_LF:
            if (enable && (slotConfig.slots[slot].enabled_lf) &&
                    (slotConfig.slots[slot].tag_lf != TAG_TYPE_UNDEFINED)) {
                lf_tag_125khz_sense_switch(true);
            } else {
                lf_tag_125khz_sense_switch(false);
            }
            break;
    }
}

// Layout written by slot config version 8 (8 slots, 68 bytes).
#define TAG_SLOT_CONFIG_V8_SLOT_NUM 8
#define TAG_SLOT_CONFIG_V8_SIZE     68

static void tag_emulation_migrate_slot_config_v0_to_v8(void) {
    // Copy old slotConfig content
    uint8_t tmpbuf[TAG_SLOT_CONFIG_V8_SIZE];
    memcpy(tmpbuf, (uint8_t *)&slotConfig, sizeof(tmpbuf));
    NRF_LOG_INFO("Migrating slotConfig v0...");
    NRF_LOG_HEXDUMP_INFO(tmpbuf, sizeof(tmpbuf));
    // Populate new slotConfig struct
    slotConfig.version = TAG_SLOT_CONFIG_CURRENT_VERSION;
    slotConfig.active_slot = tmpbuf[0];
    for (uint8_t i = 0; i < TAG_SLOT_CONFIG_V8_SLOT_NUM; i++) {
        bool enabled = tmpbuf[4 + (i * 4)] & 1;

        slotConfig.slots[i].tag_hf = tmpbuf[4 + (i * 4) + 2];
        for (uint8_t j = 0; j < ARRAYLEN(tag_specific_type_old2new_hf_values); j++) {
            if (slotConfig.slots[i].tag_hf == tag_specific_type_old2new_hf_values[j][0]) {
                slotConfig.slots[i].tag_hf = tag_specific_type_old2new_hf_values[j][1];
            }
        }
        slotConfig.slots[i].enabled_hf = slotConfig.slots[i].tag_hf != TAG_TYPE_UNDEFINED ? enabled : false;
        NRF_LOG_INFO("Slot %i HF: %02X->%04X enabled:%i", i, tmpbuf[4 + (i * 4) + 2], slotConfig.slots[i].tag_hf, slotConfig.slots[i].enabled_hf);

        slotConfig.slots[i].tag_lf = tmpbuf[4 + (i * 4) + 3];
        for (uint8_t j = 0; j < ARRAYLEN(tag_specific_type_old2new_lf_values); j++) {
            if (slotConfig.slots[i].tag_lf == tag_specific_type_old2new_lf_values[j][0]) {
                slotConfig.slots[i].tag_lf = tag_specific_type_old2new_lf_values[j][1];
            }
        }
        slotConfig.slots[i].enabled_lf = slotConfig.slots[i].tag_lf != TAG_TYPE_UNDEFINED ? enabled : false;
        NRF_LOG_INFO("Slot %i LF: %02X->%04X enabled:%i", i, tmpbuf[4 + (i * 4) + 3], slotConfig.slots[i].tag_lf, slotConfig.slots[i].enabled_lf);
    }
}

static void tag_emulation_migrate_slot_config(void) {
    uint8_t from_version = slotConfig.version;
    switch (from_version) {
        case 0:
        case 1:
        case 2:
        case 3:
        case 4:
        case 5:
        case 6:
        case 7:
            tag_emulation_migrate_slot_config_v0_to_v8();
            /* fall through */
        case 8:
        case 9: {
            // v8 had 8 slots; v9 was the fixed 32-slot build. Everything above
            // the previous count starts disabled and can be added on demand.
            uint8_t count = (from_version == 9) ? 32 : TAG_SLOT_COUNT_MIN;
            for (uint16_t i = count; i < TAG_MAX_SLOT_NUM; i++) {
                slotConfig.slots[i].enabled_hf = false;
                slotConfig.slots[i].enabled_lf = false;
                slotConfig.slots[i].tag_hf = TAG_TYPE_UNDEFINED;
                slotConfig.slots[i].tag_lf = TAG_TYPE_UNDEFINED;
            }
            slotConfig.slot_count = count;
            slotConfig.version = TAG_SLOT_CONFIG_CURRENT_VERSION;

            /*
             * Add new migration steps ABOVE THIS COMMENT
             * `tag_emulation_save_config()` and `break` statements should only be used on the last migration step, all the previous steps must fall
             * through to the next case.
             */

            tag_emulation_save_config();
            break;
        }
        case TAG_SLOT_CONFIG_CURRENT_VERSION:
            break;
        default:
            NRF_LOG_ERROR("Unsupported slotConfig migration attempted! (%d -> %d)", from_version, TAG_SLOT_CONFIG_CURRENT_VERSION);
            break;
    }
}

/**
 * Load the emulated card configuration data, note that loading is just a card slot configuration
 */
static void tag_emulation_load_config(void) {
    uint16_t length = sizeof(slotConfig);
    // Read the card slot configuration data
    bool ret = fds_read_sync(FDS_EMULATION_CONFIG_FILE_ID, FDS_EMULATION_CONFIG_RECORD_KEY, &length, (uint8_t *)&slotConfig);
    if (ret) {
        // After the reading is completed, we will save a BCC of the current configuration. When it is stored later, it can be used as a reference for the contrast between changes.
        calc_14a_crc_lut((uint8_t *)&slotConfig, sizeof(slotConfig), (uint8_t *)&m_slot_config_crc);
        NRF_LOG_INFO("Load tag slot config done.");
        if (slotConfig.version < TAG_SLOT_CONFIG_CURRENT_VERSION) {  // old slotConfig, need to migrate
            tag_emulation_migrate_slot_config();
        }
    } else {
        NRF_LOG_INFO("Tag slot config does not exist.");
    }
}

/**
 * Save the emulated card configuration data
 */
static void tag_emulation_save_config(void) {
    // We are configured the card slot configuration, and we need to calculate the current card slot configuration CRC code to judge whether the data below is updated
    uint16_t new_calc_crc;
    calc_14a_crc_lut((uint8_t *)&slotConfig, sizeof(slotConfig), (uint8_t *)&new_calc_crc);
    if (new_calc_crc != m_slot_config_crc) {  // Before saving, make sure that the card slot configuration has changed
        NRF_LOG_INFO("Save tag slot config start.");
        bool ret = fds_write_sync(FDS_EMULATION_CONFIG_FILE_ID, FDS_EMULATION_CONFIG_RECORD_KEY, sizeof(slotConfig), (uint8_t *)&slotConfig);
        if (ret) {
            NRF_LOG_INFO("Save tag slot config success.");
            m_slot_config_crc = new_calc_crc;
        } else {
            NRF_LOG_ERROR("Save tag slot config error.");
        }
    } else {
        NRF_LOG_INFO("Tag slot config no change.");
    }
}

/**
 * Start tag emulation
 */
void tag_emulation_sense_run(void) {
    tag_emulation_sense_switch_all(true);
}

/**
 * Stop the tag emulation. Note that this function will absolutely block NFC-related events, including awakening MCU
 * If you still need to be awakened by NFC after the MCU is required, please do not call this function
 */
void tag_emulation_sense_end(void) {
    TAG_FIELD_LED_OFF();
    tag_emulation_sense_switch_all(false);
}

/**
 * Initialized tag emulation
 */
void tag_emulation_init(void) {
    tag_emulation_load_config();  // Configuration of loading the card slot of the emulation card
    tag_emulation_slot_meta_init();
    tag_emulation_load_data();    // Load the data of the emulated card
}

/**
 * Save the tag data (written from RAM to Flash)
 */
void tag_emulation_save(void) {
    tag_emulation_save_config();  // Save the card slot configuration
    tag_emulation_save_data();    // Save card slot data
}

uint8_t tag_emulation_get_slot_count(void) {
    uint8_t count = slotConfig.slot_count;
    if (count < TAG_SLOT_COUNT_MIN) {
        count = TAG_SLOT_COUNT_MIN;
    }
    if (count > TAG_MAX_SLOT_NUM) {
        count = TAG_MAX_SLOT_NUM;
    }
    return count;
}

/**
 * Load the compact per-slot metadata records from FDS.
 *
 * Existing card dumps predate the metadata record, so for HF types whose
 * persistent layout starts with the 14A collision response (MF1/MF0/NTAG/
 * HF14A-4) the UID is also preloaded from the saved dump at boot.
 */
void tag_emulation_slot_meta_init(void) {
    uint8_t count = tag_emulation_get_slot_count();
    memset(m_slot_meta, 0, sizeof(m_slot_meta));
    for (uint16_t slot = 0; slot < count; slot++) {
        uint16_t length = sizeof(m_slot_meta[slot]);
        if (!fds_read_sync(FDS_SLOT_META_FILE_ID_BASE + slot, 1, &length,
                           (uint8_t *)&m_slot_meta[slot]) ||
                length < sizeof(m_slot_meta[slot])) {
            memset(&m_slot_meta[slot], 0, sizeof(m_slot_meta[slot]));
        }
    }

    for (uint16_t slot = 0; slot < count; slot++) {
        tag_slot_specific_type_t types;
        tag_emulation_get_specific_types_by_slot((uint8_t)slot, &types);
        bool layout_has_coll_res =
            types.tag_hf == TAG_TYPE_MIFARE_Mini ||
            types.tag_hf == TAG_TYPE_MIFARE_1024 ||
            types.tag_hf == TAG_TYPE_MIFARE_2048 ||
            types.tag_hf == TAG_TYPE_MIFARE_4096 ||
            types.tag_hf == TAG_TYPE_MF0ICU1 ||
            types.tag_hf == TAG_TYPE_MF0ICU2 ||
            types.tag_hf == TAG_TYPE_MF0UL11 ||
            types.tag_hf == TAG_TYPE_MF0UL21 ||
            types.tag_hf == TAG_TYPE_NTAG_210 ||
            types.tag_hf == TAG_TYPE_NTAG_212 ||
            types.tag_hf == TAG_TYPE_NTAG_213 ||
            types.tag_hf == TAG_TYPE_NTAG_215 ||
            types.tag_hf == TAG_TYPE_NTAG_216 ||
            types.tag_hf == TAG_TYPE_HF14A_4;
        if (!layout_has_coll_res || m_slot_meta[slot].uid_len != 0) {
            continue;
        }

        fds_slot_record_map_t map;
        get_fds_map_by_slot_sense_type_for_dump((uint8_t)slot, TAG_SENSE_HF, &map);
        uint16_t length = m_tag_data_hf.length;
        if (!fds_read_sync(map.id, map.key, &length, m_tag_data_hf.buffer) ||
                length < 16) {
            continue;
        }

        tag_slot_meta_t meta = m_slot_meta[slot];
        uint8_t uid_len = m_tag_data_hf.buffer[0];
        if (uid_len != 4 && uid_len != 7 && uid_len != 10) {
            continue;
        }
        meta.valid = true;
        meta.tag_type_hf = types.tag_hf;
        meta.tag_type_lf = types.tag_lf;
        meta.uid_len = uid_len;
        memcpy(meta.uid, &m_tag_data_hf.buffer[4], uid_len);
        memcpy(meta.atqa, &m_tag_data_hf.buffer[1], sizeof(meta.atqa));
        meta.sak = m_tag_data_hf.buffer[3];
        if (memcmp(&meta, &m_slot_meta[slot], sizeof(meta)) != 0) {
            m_slot_meta[slot] = meta;
            fds_write_sync(FDS_SLOT_META_FILE_ID_BASE + slot, 1, sizeof(meta), &meta);
        }
    }
}

static bool slot_meta_uid_equal(const tag_slot_meta_t *meta, const uint8_t *uid, uint8_t uid_len) {
    return meta->valid && meta->uid_len == uid_len && uid_len <= sizeof(meta->uid) &&
           memcmp(meta->uid, uid, uid_len) == 0;
}

/**
 * Best match for a reader SELECT UID. Prefer the most recently used slot when
 * several slots share a UID (clones); otherwise the first match.
 */
uint8_t tag_emulation_find_slot_by_uid(const uint8_t *uid, uint8_t uid_len) {
    uint8_t count = tag_emulation_get_slot_count();
    if (uid_len == 0 || uid_len > sizeof(m_slot_meta[0].uid)) {
        return 0xFF;
    }
    // Cloned cards can share a UID. Prefer a slot that recently handled the
    // same sector the reader is interested in.
    if (m_last_reader_sector != 0xFF) {
        for (uint8_t i = 0; i < m_mru_count; i++) {
            uint8_t slot = m_mru[i];
            if (slot < count && slot_meta_uid_equal(&m_slot_meta[slot], uid, uid_len) &&
                    m_slot_meta[slot].last_auth_sector == m_last_reader_sector) {
                return slot;
            }
        }
        for (uint16_t slot = 0; slot < count; slot++) {
            if (slot_meta_uid_equal(&m_slot_meta[slot], uid, uid_len) &&
                    m_slot_meta[slot].last_auth_sector == m_last_reader_sector) {
                return (uint8_t)slot;
            }
        }
    }
    for (uint8_t i = 0; i < m_mru_count; i++) {
        uint8_t slot = m_mru[i];
        if (slot < count && slot_meta_uid_equal(&m_slot_meta[slot], uid, uid_len)) {
            return slot;
        }
    }
    for (uint16_t slot = 0; slot < count; slot++) {
        if (slot_meta_uid_equal(&m_slot_meta[slot], uid, uid_len)) {
            return (uint8_t)slot;
        }
    }
    return 0xFF;
}

/**
 * Update the metadata for one slot from the slot configuration and, when it is
 * the active slot, from the currently loaded 14A collision response.
 */
void tag_emulation_slot_meta_update(uint8_t slot) {
    uint8_t count = tag_emulation_get_slot_count();
    if (slot >= count) {
        return;
    }

    tag_slot_meta_t meta = m_slot_meta[slot];
    tag_slot_specific_type_t types;
    tag_emulation_get_specific_types_by_slot(slot, &types);
    meta.tag_type_hf = types.tag_hf;
    meta.tag_type_lf = types.tag_lf;
    meta.valid = (types.tag_hf != TAG_TYPE_UNDEFINED) || (types.tag_lf != TAG_TYPE_UNDEFINED);

    if (types.tag_hf == TAG_TYPE_UNDEFINED) {
        meta.uid_len = 0;
        memset(meta.uid, 0, sizeof(meta.uid));
        memset(meta.atqa, 0, sizeof(meta.atqa));
        meta.sak = 0;
    } else if (slot == tag_emulation_get_slot()) {
        nfc_tag_14a_coll_res_reference_t *res = nfc_tag_14a_get_active_coll_res();
        if (res != NULL && res->size != NULL && res->uid != NULL &&
                res->atqa != NULL && res->sak != NULL) {
            uint8_t uid_len = *res->size;
            if (uid_len > sizeof(meta.uid)) {
                uid_len = sizeof(meta.uid);
            }
            meta.uid_len = uid_len;
            memcpy(meta.uid, res->uid, uid_len);
            memcpy(meta.atqa, res->atqa, sizeof(meta.atqa));
            meta.sak = res->sak[0];
        }
    }

    if (memcmp(&meta, &m_slot_meta[slot], sizeof(meta)) != 0) {
        m_slot_meta[slot] = meta;
        fds_write_sync(FDS_SLOT_META_FILE_ID_BASE + slot, 1, sizeof(meta), &meta);
    }
}

/**
 * Record the sector a reader just tried to authenticate. The block-to-sector
 * mapping follows MIFARE Classic: sectors 0..31 use 4 blocks, 32..39 use 16.
 */
void tag_emulation_slot_note_auth(uint8_t slot, uint8_t block) {
    if (slot >= tag_emulation_get_slot_count()) {
        return;
    }
    uint8_t sector;
    if (block < 128) {
        sector = block / 4;
    } else {
        sector = (uint8_t)(24U + block / 16U);
    }
    m_slot_meta[slot].last_auth_sector = sector;
    m_last_reader_sector = sector;
    tag_emulation_slot_mru_touch(slot);
}

void tag_emulation_slot_mru_touch(uint8_t slot) {
    if (slot >= tag_emulation_get_slot_count()) {
        return;
    }
    uint8_t index = m_mru_count;
    for (uint8_t i = 0; i < m_mru_count; i++) {
        if (m_mru[i] == slot) {
            index = i;
            break;
        }
    }
    if (index == m_mru_count) {
        if (m_mru_count < TAG_SLOT_MRU_MAX) {
            m_mru_count++;
        }
        index = m_mru_count - 1;
    }
    for (uint8_t i = index; i > 0; i--) {
        m_mru[i] = m_mru[i - 1];
    }
    m_mru[0] = slot;
}

bool tag_emulation_slot_has_data(uint8_t slot) {
    if (slot >= tag_emulation_get_slot_count()) {
        return false;
    }
    tag_slot_specific_type_t types;
    tag_emulation_get_specific_types_by_slot(slot, &types);
    return (types.tag_hf != TAG_TYPE_UNDEFINED) || (types.tag_lf != TAG_TYPE_UNDEFINED);
}

static bool slot_is_pollable(uint8_t slot, uint8_t current) {
    if (slot == current || slot >= tag_emulation_get_slot_count() ||
            !tag_emulation_slot_has_data(slot) ||
            !settings_get_slot_polling_slot_mask_bit(slot)) {
        return false;
    }
    tag_slot_specific_type_t types;
    tag_emulation_get_specific_types_by_slot(slot, &types);
    bool hf_ok = settings_get_ic_polling_enable() &&
                 types.tag_hf != TAG_TYPE_UNDEFINED &&
                 is_slot_enabled(slot, TAG_SENSE_HF);
    bool lf_ok = settings_get_id_polling_enable() &&
                 types.tag_lf != TAG_TYPE_UNDEFINED &&
                 is_slot_enabled(slot, TAG_SENSE_LF);
    return hf_ok || lf_ok;
}

/**
 * Pick the next slot for smart polling.
 * Three out of four calls return the next MRU candidate; every fourth call
 * performs a full sweep so slots never used since boot are still reachable.
 */
uint8_t tag_emulation_slot_next_smart(uint8_t current, uint8_t *sweep_cursor) {
    uint8_t count = tag_emulation_get_slot_count();
    bool use_sweep = ((m_smart_round++ & 0x03U) == 0x03U);

    if (!use_sweep) {
        for (uint8_t i = 0; i < m_mru_count; i++) {
            uint8_t slot = m_mru[i];
            if (slot_is_pollable(slot, current)) {
                return slot;
            }
        }
    }

    if (sweep_cursor != NULL) {
        for (uint16_t n = 0; n < count; n++) {
            uint8_t slot = (uint8_t)((*sweep_cursor + n) % count);
            if (slot_is_pollable(slot, current)) {
                *sweep_cursor = (uint8_t)((slot + 1) % count);
                return slot;
            }
        }
    }

    // Fallback: any enabled slot of an enabled sense, even if its data is not
    // present yet.
    for (uint16_t n = 0; n < count; n++) {
        uint8_t slot = (uint8_t)((current + 1 + n) % count);
        if (slot == current) {
            continue;
        }
        bool hf_ok = settings_get_ic_polling_enable() && is_slot_enabled(slot, TAG_SENSE_HF);
        bool lf_ok = settings_get_id_polling_enable() && is_slot_enabled(slot, TAG_SENSE_LF);
        if ((hf_ok || lf_ok) && settings_get_slot_polling_slot_mask_bit(slot)) {
            return slot;
        }
    }
    return current;
}

/**
 * Background sweep: ignore the MRU list so every pollable slot is visited.
 */
uint8_t tag_emulation_slot_next_sweep(uint8_t current, uint8_t *sweep_cursor) {
    uint8_t count = tag_emulation_get_slot_count();
    if (sweep_cursor != NULL) {
        for (uint16_t n = 0; n < count; n++) {
            uint8_t slot = (uint8_t)((*sweep_cursor + n) % count);
            if (slot_is_pollable(slot, current)) {
                *sweep_cursor = (uint8_t)((slot + 1) % count);
                return slot;
            }
        }
    }
    // Fallback: any enabled slot of an enabled sense.
    for (uint16_t n = 0; n < count; n++) {
        uint8_t slot = (uint8_t)((current + 1 + n) % count);
        if (slot == current) {
            continue;
        }
        bool hf_ok = settings_get_ic_polling_enable() && is_slot_enabled(slot, TAG_SENSE_HF);
        bool lf_ok = settings_get_id_polling_enable() && is_slot_enabled(slot, TAG_SENSE_LF);
        if ((hf_ok || lf_ok) && settings_get_slot_polling_slot_mask_bit(slot)) {
            return slot;
        }
    }
    return current;
}

/**
 * Get the currently activated card slot index
 */
uint8_t tag_emulation_get_slot(void) {
    uint8_t count = tag_emulation_get_slot_count();
    return (slotConfig.active_slot < count) ? slotConfig.active_slot : 0;
}

/**
 * Set the currently activated card slot index
 */
void tag_emulation_set_slot(uint8_t index) {
    if (index >= tag_emulation_get_slot_count()) {
        return;
    }
    slotConfig.active_slot = index;  // Re -set to the new switched card slot
    tag_emulation_slot_mru_touch(index);
    rgb_marquee_reset();             // force animation color refresh according to new slot
}

/**
 * Switch to the card slot of the specified index, this function will automatically complete the data loading
 */
void tag_emulation_change_slot(uint8_t index, bool sense_disable) {
    if (index >= tag_emulation_get_slot_count()) {
        return;
    }
    if (sense_disable) {
        // Turn off the analog card to avoid triggering the emulation when switching the card slot
        tag_emulation_sense_end();
    }
    tag_emulation_save_data();      // Save the data of the current card, in case of there is a change
    g_is_tag_emulating = false;     // Reset the emulating flag
    tag_emulation_set_slot(index);  // Update the index of the activated card slot
    tag_emulation_load_data();      // Then reload the data of the card slot
    if (sense_disable) {
        // According to the configuration of the new card slot, the monitoring status of our update
        tag_emulation_sense_run();
    }
}

/**
 * Switch slot while a reader field is still present. The NFCT/LF sense state is
 * left untouched; only the active slot data is swapped and the 14A protocol
 * state is reset for the next reader frame.
 */
bool tag_emulation_change_slot_live(uint8_t index) {
    uint8_t count = tag_emulation_get_slot_count();
    uint8_t current = tag_emulation_get_slot();
    if (index >= count || index == current) {
        return false;
    }

    tag_emulation_save_data();
    tag_emulation_set_slot(index);
    tag_emulation_load_data();

    tag_slot_specific_type_t types;
    tag_emulation_get_specific_types_by_slot(index, &types);
    if (types.tag_hf != TAG_TYPE_UNDEFINED) {
        nfc_tag_14a_slot_switched();
    }
    return true;
}

/**
 * Append one slot at the end of the visible range.
 * Flash space must be checked by the caller before calling this.
 */
bool tag_emulation_slot_add(tag_specific_type_t tag_type) {
    uint8_t count = tag_emulation_get_slot_count();
    if (count >= TAG_MAX_SLOT_NUM) {
        return false;
    }

    uint8_t slot = count;
    slotConfig.slots[slot].enabled_hf = false;
    slotConfig.slots[slot].enabled_lf = false;
    slotConfig.slots[slot].tag_hf = TAG_TYPE_UNDEFINED;
    slotConfig.slots[slot].tag_lf = TAG_TYPE_UNDEFINED;
    memset(&m_slot_meta[slot], 0, sizeof(m_slot_meta[slot]));

    if (tag_type != TAG_TYPE_UNDEFINED) {
        tag_sense_type_t sense = get_sense_type_from_tag_type(tag_type);
        if (sense == TAG_SENSE_HF) {
            slotConfig.slots[slot].tag_hf = tag_type;
            slotConfig.slots[slot].enabled_hf = true;
        } else if (sense == TAG_SENSE_LF) {
            slotConfig.slots[slot].tag_lf = tag_type;
            slotConfig.slots[slot].enabled_lf = true;
        }
    }

    slotConfig.slot_count = count + 1;
    tag_emulation_save_config();
    return true;
}

/**
 * Remove the last visible slot and delete its data and nickname records.
 */
bool tag_emulation_slot_remove_last(void) {
    uint8_t count = tag_emulation_get_slot_count();
    if (count <= TAG_SLOT_COUNT_MIN) {
        return false;
    }

    uint8_t slot = count - 1;
    if (slotConfig.active_slot == slot) {
        tag_emulation_change_slot(slot - 1, true);
    }

    tag_emulation_delete_data(slot, TAG_SENSE_HF);
    tag_emulation_delete_data(slot, TAG_SENSE_LF);

    fds_slot_record_map_t map;
    get_fds_map_by_slot_sense_type_for_nick(slot, TAG_SENSE_HF, &map);
    fds_delete_sync(map.id, map.key);
    get_fds_map_by_slot_sense_type_for_nick(slot, TAG_SENSE_LF, &map);
    fds_delete_sync(map.id, map.key);

    slotConfig.slots[slot].enabled_hf = false;
    slotConfig.slots[slot].enabled_lf = false;
    slotConfig.slots[slot].tag_hf = TAG_TYPE_UNDEFINED;
    slotConfig.slots[slot].tag_lf = TAG_TYPE_UNDEFINED;

    fds_delete_sync(FDS_SLOT_META_FILE_ID_BASE + slot, 1);
    memset(&m_slot_meta[slot], 0, sizeof(m_slot_meta[slot]));

    // Remove the slot from the MRU list.
    uint8_t dst = 0;
    for (uint8_t i = 0; i < m_mru_count; i++) {
        if (m_mru[i] != slot) {
            m_mru[dst++] = m_mru[i];
        }
    }
    m_mru_count = dst;

    slotConfig.slot_count = count - 1;
    tag_emulation_save_config();
    return true;
}

/**
 * Determine whether the specified card slot is enabled
 */
bool is_slot_enabled(uint8_t slot, tag_sense_type_t sense_type) {
    if (sense_type == TAG_SENSE_LF) {
        return slotConfig.slots[slot].enabled_lf;
    }
    if (sense_type == TAG_SENSE_HF) {
        return slotConfig.slots[slot].enabled_hf;
    }
    return false;
}

/**
 * Set whether the specified card slot is enabled
 */
void tag_emulation_slot_set_enable(uint8_t slot, tag_sense_type_t sense_type, bool enable) {
    // Set the capacity of the corresponding card slot directly
    if (sense_type == TAG_SENSE_LF) {
        slotConfig.slots[slot].enabled_lf = enable;
    }
    if (sense_type == TAG_SENSE_HF) {
        slotConfig.slots[slot].enabled_hf = enable;
    }
}

/**
 * Find the next valid card slot
 */
uint8_t tag_emulation_slot_find_next(uint8_t slot_now) {
    uint8_t count = tag_emulation_get_slot_count();
    if (slot_now >= count) {
        slot_now = 0;
    }
    uint8_t start_slot = (slot_now + 1 == count) ? 0 : slot_now + 1;
    for (uint8_t i = start_slot;;) {
        if (i == slot_now) return slot_now;                                              // No other activated card slots were found after a loop
        if (slotConfig.slots[i].enabled_hf || slotConfig.slots[i].enabled_lf) return i;  // Check whether the card slot that is currently traversed is enabled, so that the capacity determines that the current card slot is the card slot that can effectively enable capacity
        i++;
        if (i == count) {  // Continue the next cycle
            i = 0;
        }
    }
    return slot_now;  // If you cannot find it, the specified return value of the pass is returned by default
}

/**
 * Find the previous valid card slot
 */
uint8_t tag_emulation_slot_find_prev(uint8_t slot_now) {
    uint8_t count = tag_emulation_get_slot_count();
    if (slot_now >= count) {
        slot_now = 0;
    }
    uint8_t start_slot = (slot_now == 0) ? (count - 1) : slot_now - 1;
    for (uint8_t i = start_slot;;) {
        if (i == slot_now) return slot_now;                                              // No other activated card slots were found after a loop
        if (slotConfig.slots[i].enabled_hf || slotConfig.slots[i].enabled_lf) return i;  // Check whether the card slot that is currently traversed is enabled, so that the capacity determines that the current card slot is the card slot that can effectively enable capacity
        if (i == 0) {                                                                    // Continue the next cycle
            i = count - 1;
        } else {
            i--;
        }
    }
    return slot_now;  // If you cannot find it, the specified return value of the pass is returned by default
}

/**
 * Set the card specified by the specified card slot card slot card type card to the specified type
 */
void tag_emulation_change_type(uint8_t slot, tag_specific_type_t tag_type) {
    tag_sense_type_t sense_type = get_sense_type_from_tag_type(tag_type);
    NRF_LOG_INFO("sense type = %d", sense_type);
    switch (sense_type) {
        case TAG_SENSE_LF: {
            slotConfig.slots[slot].tag_lf = tag_type;
            break;
        }
        case TAG_SENSE_HF: {
            slotConfig.slots[slot].tag_hf = tag_type;
            break;
        }
        default:
            break;  // never happen
    }
    NRF_LOG_INFO("tag type = %d", tag_type);
    // After the update is completed, we need to notify the relevant data in the update of the memory
    if (sense_type != TAG_SENSE_NO) {
        load_data_by_tag_type(slot, tag_type);
        NRF_LOG_INFO("reload data success.");
    }
}

/**
 * The factory initialization function of the card emulation.
 * Defaults to a dual-frequency card in slot 1, a hf M1 card in slot 2, and a lf em410x card in slot 3.
 */
void tag_emulation_factory_init(void) {
    fds_slot_record_map_t map_info;

    // Initialized a dual -frequency card in the card slot, if there is no historical record, it is a new state of factory.
    if (slotConfig.slots[0].enabled_hf && slotConfig.slots[0].tag_hf == TAG_TYPE_MIFARE_1024) {
        // Initialize a high -frequency M1 card in the card slot 1, if it does not exist.
        get_fds_map_by_slot_sense_type_for_dump(0, TAG_SENSE_HF, &map_info);
        if (!fds_is_exists(map_info.id, map_info.key)) {
            tag_emulation_factory_data(0, slotConfig.slots[0].tag_hf);
        }
    }

    if (slotConfig.slots[0].enabled_lf && slotConfig.slots[0].tag_lf == TAG_TYPE_EM410X) {
        // Initialize a low -frequency EM410X card in slot 1, if it does not exist.
        get_fds_map_by_slot_sense_type_for_dump(0, TAG_SENSE_LF, &map_info);
        if (!fds_is_exists(map_info.id, map_info.key)) {
            tag_emulation_factory_data(0, slotConfig.slots[0].tag_lf);
        }
    }

    if (slotConfig.slots[1].enabled_hf && slotConfig.slots[1].tag_hf == TAG_TYPE_MF0ICU1) {
        // Initialize a high -frequency M1 card in the card slot 2, if it does not exist.
        get_fds_map_by_slot_sense_type_for_dump(1, TAG_SENSE_HF, &map_info);
        if (!fds_is_exists(map_info.id, map_info.key)) {
            tag_emulation_factory_data(1, slotConfig.slots[1].tag_hf);
        }
    }

    if (slotConfig.slots[2].enabled_lf && slotConfig.slots[2].tag_lf == TAG_TYPE_EM410X) {
        // Initialize a low -frequency EM410X card in slot 3, if it does not exist.
        get_fds_map_by_slot_sense_type_for_dump(2, TAG_SENSE_LF, &map_info);
        if (!fds_is_exists(map_info.id, map_info.key)) {
            tag_emulation_factory_data(2, slotConfig.slots[2].tag_lf);
        }
    }
}
