/**
 ****************************************************************************************
 *
 * @file user_config.h
 *
 * @brief User configuration file.
 *
 * Copyright (C) 2015-2023 Renesas Electronics Corporation and/or its affiliates.
 * All rights reserved. Confidential Information.
 *
 * This software ("Software") is supplied by Renesas Electronics Corporation and/or its
 * affiliates ("Renesas"). Renesas grants you a personal, non-exclusive, non-transferable,
 * revocable, non-sub-licensable right and license to use the Software, solely if used in
 * or together with Renesas products. You may make copies of this Software, provided this
 * copyright notice and disclaimer ("Notice") is included in all such copies. Renesas
 * reserves the right to change or discontinue the Software at any time without notice.
 *
 * THE SOFTWARE IS PROVIDED "AS IS". RENESAS DISCLAIMS ALL WARRANTIES OF ANY KIND,
 * WHETHER EXPRESS, IMPLIED, OR STATUTORY, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. TO THE
 * MAXIMUM EXTENT PERMITTED UNDER LAW, IN NO EVENT SHALL RENESAS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, SPECIAL, INCIDENTAL OR CONSEQUENTIAL DAMAGES ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE, EVEN IF RENESAS HAS BEEN ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGES. USE OF THIS SOFTWARE MAY BE SUBJECT TO TERMS AND CONDITIONS CONTAINED IN
 * AN ADDITIONAL AGREEMENT BETWEEN YOU AND RENESAS. IN CASE OF CONFLICT BETWEEN THE TERMS
 * OF THIS NOTICE AND ANY SUCH ADDITIONAL LICENSE AGREEMENT, THE TERMS OF THE AGREEMENT
 * SHALL TAKE PRECEDENCE. BY CONTINUING TO USE THIS SOFTWARE, YOU AGREE TO THE TERMS OF
 * THIS NOTICE.IF YOU DO NOT AGREE TO THESE TERMS, YOU ARE NOT PERMITTED TO USE THIS
 * SOFTWARE.
 *
 ****************************************************************************************
 */

#ifndef _USER_CONFIG_H_
#define _USER_CONFIG_H_

/*
 * INCLUDE FILES
 ****************************************************************************************
 */

#include "app_user_config.h"
#include "arch_api.h"
#include "app_default_handlers.h"
#include "app_adv_data.h"
#include "co_bt.h"

/*
 * LOCAL VARIABLES
 ****************************************************************************************
 */

/*
 ****************************************************************************************
 *
 * Privacy / Addressing configuration
 *
 ****************************************************************************************
 */

/*************************************************************************
 * Privacy Capabilities and address configuration of local device:
 * - APP_CFG_ADDR_PUB               No Privacy, Public BDA
 * - APP_CFG_ADDR_STATIC            No Privacy, Random Static BDA
 * - APP_CFG_HOST_PRIV_RPA          Host Privacy, RPA, Public Identity
 * - APP_CFG_HOST_PRIV_NRPA         Host Privacy, NRPA (non-connectable ONLY)
 * - APP_CFG_CNTL_PRIV_RPA_PUB      Controller Privacy, RPA or PUB, Public Identity
 * - APP_CFG_CNTL_PRIV_RPA_RAND     Controller Privacy, RPA, Public Identity
 *
 * Select only one option for privacy / addressing configuration.
 **************************************************************************
 */
#define USER_CFG_ADDRESS_MODE       APP_CFG_ADDR_PUB

/*************************************************************************
 * Controller Privacy Mode:
 * - APP_CFG_CNTL_PRIV_MODE_NETWORK Controler Privacy Network mode (default)
 * - APP_CFG_CNTL_PRIV_MODE_DEVICE  Controler Privacy Device mode
 *
 * Select only one option for controller privacy mode configuration.
 **************************************************************************
 */
#define USER_CFG_CNTL_PRIV_MODE     APP_CFG_CNTL_PRIV_MODE_NETWORK


/******************************************
 * Default sleep mode. Possible values are:
 *
 * - ARCH_SLEEP_OFF
 * - ARCH_EXT_SLEEP_ON
 * - ARCH_EXT_SLEEP_OTP_COPY_ON
 *
 ******************************************
 */
static const sleep_state_t app_default_sleep_mode = ARCH_SLEEP_OFF;

/*
 ****************************************************************************************
 *
 * Advertising configuration
 *
 ****************************************************************************************
 */
static const struct advertise_configuration user_adv_conf = {

    .addr_src = APP_CFG_ADDR_SRC(USER_CFG_ADDRESS_MODE),

    /// Minimum interval for advertising
    .intv_min = MS_TO_BLESLOTS(687.5),                    // 687.5ms

    /// Maximum interval for advertising
    .intv_max = MS_TO_BLESLOTS(687.5),                    // 687.5ms

    /**
     *  Advertising channels map:
     * - ADV_CHNL_37_EN:   Advertising channel map for channel 37.
     * - ADV_CHNL_38_EN:   Advertising channel map for channel 38.
     * - ADV_CHNL_39_EN:   Advertising channel map for channel 39.
     * - ADV_ALL_CHNLS_EN: Advertising channel map for channel 37, 38 and 39.
     */
    .channel_map = ADV_ALL_CHNLS_EN,

    /*************************
     * Advertising information
     *************************
     */

    /// Host information advertising data (GAPM_ADV_NON_CONN and GAPM_ADV_UNDIRECT)
    /// Advertising mode :
    /// - GAP_NON_DISCOVERABLE: Non discoverable mode
    /// - GAP_GEN_DISCOVERABLE: General discoverable mode
    /// - GAP_LIM_DISCOVERABLE: Limited discoverable mode
    /// - GAP_BROADCASTER_MODE: Broadcaster mode
    .mode = GAP_GEN_DISCOVERABLE,

    /// Host information advertising data (GAPM_ADV_NON_CONN and GAPM_ADV_UNDIRECT)
    /// - ADV_ALLOW_SCAN_ANY_CON_ANY: Allow both scan and connection requests from anyone
    /// - ADV_ALLOW_SCAN_ANY_CON_WLST: Allow both scan req from anyone and connection req from
    ///                                White List devices only
    .adv_filt_policy = ADV_ALLOW_SCAN_ANY_CON_ANY,

    /// Address of peer device
    /// NOTE: Meant for directed advertising (ADV_DIRECT_IND)
    .peer_addr = {0x1, 0x2, 0x3, 0x4, 0x5, 0x6},

    /// Address type of peer device (0=public/1=random)
    /// NOTE: Meant for directed advertising (ADV_DIRECT_IND)
    .peer_addr_type = 0,
};

/*
 ****************************************************************************************
 *
 * Advertising or scan response data for the following cases:
 *
 * - ADV_IND: Connectable undirected advertising event.
 *    - The maximum length of the user defined advertising data shall be 28 bytes.
 *    - The Flags data type are written by the related ROM function, hence the user shall
 *      not include them in the advertising data. The related ROM function adds 3 bytes in 
 *      the start of the advertising data that are to be transmitted over the air.
 *    - The maximum length of the user defined response data shall be 31 bytes.
 *
 * - ADV_NONCONN_IND: Non-connectable undirected advertising event.
 *    - The maximum length of the user defined advertising data shall be 31 bytes.
 *    - The Flags data type may be omitted, hence the user can use all the 31 bytes for 
 *      data.
 *    - The scan response data shall be empty.
 *
 * - ADV_SCAN_IND: Scannable undirected advertising event.
 *    - The maximum length of the user defined advertising data shall be 31 bytes.
 *    - The Flags data type may be omitted, hence the user can use all the 31 bytes for 
 *      data.
 *    - The maximum length of the user defined response data shall be 31 bytes.
 ****************************************************************************************
 */
/// Advertising data: the service UUIDs, so that a client can find this tag by
/// what it *is* rather than by what it is called.
///
/// This used to be empty, and clients matched on the device name. That is a poor
/// contract - the name is for people, and every time it changes (it has now
/// changed twice) every client breaks - so the services go on the air instead.
/// Bytes are little-endian, over the air and in the arrays in
/// custom_profile/user_custs1_def.h, which is where these are copied from.
///
/// `0x06` rather than `0x07`: this is the *incomplete* list of 128-bit service
/// UUIDs, which is the honest type, because the image service is deliberately
/// left out. There is not room for both, and one is enough to recognise a tag.
#define USER_ADV_UUID_CMD \
    "\x11\x06\x14\x17\x59\x0c\x4e\xe6\x6e\xab\xc5\x42\xc0\x1f\x60\xb2\x7f\x67"

/// SUOTA's 16-bit UUID (0xFEF5), advertised only when the service is actually
/// present. This is what a generic SUOTA client scans for - the vendor's own app
/// will not list a tag that does not advertise it - so it is the difference
/// between "updatable over the air" being true and being usable.
#if defined(EPD_SUOTA) && (EPD_SUOTA)
    #define USER_ADV_UUID_SUOTA               "\x03\x03\xF5\xFE"
#else
    #define USER_ADV_UUID_SUOTA               ""
#endif

/// 4 + 18 = 22 bytes with SUOTA, 18 without, against a 28-byte budget (31 less
/// the 3-byte Flags the SDK prepends). The device name does not fit alongside
/// them, so the SDK puts it in the scan response instead - which is fine, and is
/// why the name is still visible in a scanner. See app.c, and note it decides
/// this by arithmetic on USER_DEVICE_NAME_LEN, so both are compile-time.
#define USER_ADVERTISE_DATA                   USER_ADV_UUID_SUOTA USER_ADV_UUID_CMD

/// Advertising data length - maximum 28 bytes, 3 bytes are reserved to set
#define USER_ADVERTISE_DATA_LEN               (sizeof(USER_ADVERTISE_DATA)-1)

/// Scan response data
#define USER_ADVERTISE_SCAN_RESPONSE_DATA     ""

/// Scan response data length- maximum 31 bytes
#define USER_ADVERTISE_SCAN_RESPONSE_DATA_LEN (sizeof(USER_ADVERTISE_SCAN_RESPONSE_DATA)-1)

/*
 ****************************************************************************************
 *
 * Device name.
 *
 * - If there is space left in the advertising or scan response data the device name is
 *   copied there. The device name can be anytime read by a connected peer, if the
 *   application supports it.
 * - The Bluetooth device name can be up to 248 bytes.
 *
 ****************************************************************************************
 */
/// Which tag this image is for. This header is force-included ahead of every
/// other, so the selection lands before epd_ssd1680.h picks its own defaults.
///
/// Board wiring and panel size both come from the one type number in
/// tag_types.h, and the build scripts set it - `tools/build.sh --type 3`.
/// There is nothing to edit here per tag, which is the point: the two used to
/// be set by hand in this file, independently, and keeping them consistent
/// with each other and with the flasher's --variant was left to whoever
/// remembered.
///
/// Getting the wiring wrong is silent in the worst way: the tag boots,
/// advertises and takes connections perfectly normally, and only the panel
/// stays dead. It cost a working tag in both directions before the image
/// carried a stamp the flasher could check. If a board goes quiet on the panel
/// alone, suspect the build target first.
#include "tag_types.h"

/// Panel-presence probe, off by default. Builds in epd_panel_present(), which
/// asks the controller directly with cmd 0x2F the way the retail firmware
/// does. Turn it on when a screen will not move: a disconnected panel leaves
/// BUSY reading idle, so the refresh returns instantly and looks exactly like
/// a bad init sequence. Costs ~240 bytes, so it stays out of a shipping image.
// #define EPD_PANEL_PROBE 1

/// Where the SDK's bond database lives in the boot flash.
///
/// This has to be set because SUOTA needs CFG_SPI_FLASH_ENABLE, and that macro
/// does double duty in the SDK: as well as pointing the SUOTA receiver at the
/// SPI flash, it is the only thing selecting where app_bond_db keeps its data
/// (app_bond_db.h). Its default is 0x1E000, and app_bond_db_store() **erases
/// the whole 4 KiB sector** around that address before writing.
///
/// 0x1E000 is inside an image bank on every layout we have measured:
///
///   Type 1     bank 1 0x002000..0x014000, bank 2 0x014000..0x038000
///              -> 0x1E000 lands inside the stock image in bank 2, which is
///                 the fallback a failed update relies on
///   Type 2/3/4 bank 1 0x004000..0x01F000, bank 2 0x01F000..0x038000
///              -> 0x1E000 lands in bank 1, past a 35 KB image today but
///                 inside the region a larger one would occupy
///
/// The database is inert in the current build only because CFG_SPI_FLASH_ENABLE
/// is undefined, so neither of app_bond_db's storage branches is selected.
/// Turning SUOTA on activates it, and app_bond_db is reached from
/// default_app_on_pairing_request - so any peer that asks to pair can trigger
/// that erase. Nothing in this firmware pairs deliberately, which is exactly
/// what would make it hard to find.
///
/// 0x040000 is the free upper half of the 512 KiB part, above the template
/// store at 0x03F000 and above everything the bootloader reads. On a 256 KiB
/// part it would be out of range and bonds simply would not persist, which is
/// the right way for this to fail.
#define USER_CFG_BOND_DB_DATA_OFFSET    (0x040000)

/// Advertised device name, and a template rather than the final string.
///
/// The last six characters are replaced at boot with the low three bytes of the
/// tag's own BD address, so a scanner listing several tags shows which is which:
///
///     T4BL-682F8D
///     ^^ ^^ ^^^^^^ the end of the MAC, as a scanner prints it
///     |  |+------- panel resolution, H or L, for reading at a glance
///     |  +-------- board variant, which is the wiring
///     +----------- tag type
///
/// No vendor name in it. These tags were decommissioned and run nothing of the
/// vendor's any more, so carrying "HemaEPD" said something untrue about them and
/// spent seven of the advertisement's bytes doing it. **Nothing matches on this
/// name** - clients find a tag by the service UUIDs in USER_ADVERTISE_DATA above,
/// which is a contract that does not break the next time the name changes. It has
/// changed twice.
///
/// Written as a placeholder of exactly the final length, and filled in rather
/// than rebuilt, because USER_DEVICE_NAME_LEN is a compile-time constant that
/// the SDK uses for real work: it sizes the name's slot in the advertising data
/// (app.c) and is asserted against BD_NAME_SIZE on every name read (app_task.c).
/// A runtime string of a different length would put those two out of step with
/// what is actually advertised.
///
/// Filled in by user_dev_name_init() in user_empty_peripheral_template.c, which
/// also explains why the patching has to happen on every advertising restart.
/// One name for every tag. It used to encode the type, variant and resolution -
/// "T4BL-000000" - which described the IMAGE, and there is no longer an image
/// per tag to describe. What distinguishes one tag from another in a scanner is
/// the address, which is filled in below; what it IS, a client reads over GATT
/// (render status bytes 10-13 give the real panel).
#define USER_DEVICE_NAME        "Tag-000000"

/// Device name length
#define USER_DEVICE_NAME_LEN    (sizeof(USER_DEVICE_NAME)-1)

/*
 ****************************************************************************************
 *
 * GAPM configuration
 *
 ****************************************************************************************
 */
static const struct gapm_configuration user_gapm_conf = {
    /// Device Role: Central, Peripheral, Observer, Broadcaster or All roles. (@see enum gap_role)
    .role = GAP_ROLE_PERIPHERAL,

    /// Maximal MTU. Shall be set to 23 if Legacy Pairing is used, 65 if Secure Connection is used,
    /// more if required by the application
    ///
    /// Raised from the template's 23 because drawing commands are longer than
    /// the 20 payload bytes a 23-byte MTU allows (e.g.
    /// "RECT(10,10,100,60,0,1,1)" is 25). A client's attempt to send more in
    /// one write falls back to a long/prepared write, which this SDK's custom
    /// characteristic rejects with ATT "Unlikely Error" (0x0E) - observed on
    /// hardware. 251 matches CFG_MAX_TX_PACKET_LENGTH so a whole
    /// DEF_CMD_CHAR_LEN (128) command batch fits in a single write.
    /// The parser also reassembles across writes, so this is an optimisation,
    /// not a correctness requirement.
    .max_mtu = 251,

    /// Device Address Type
    .addr_type = APP_CFG_ADDR_TYPE(USER_CFG_ADDRESS_MODE),
    /// Duration before regenerate the random private address when privacy is enabled
    .renew_dur = 15000,    // 15000 * 10ms = 150s is the minimum value

    /***********************
     * Privacy configuration
     ***********************
     */

    /// Private static address
    // NOTE: The address shall comply with the following requirements:
    // - the two most significant bits of the address shall be equal to 1,
    // - all the remaining bits of the address shall NOT be equal to 1,
    // - all the remaining bits of the address shall NOT be equal to 0.
    // In case the {0x00, 0x00, 0x00, 0x00, 0x00, 0x00} null address is used, a
    // random static address will be automatically generated.
    .addr = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},

    /// Device IRK used for resolvable random BD address generation (LSB first)
    .irk = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f},

    /****************************
     * ATT database configuration
     ****************************
     */

    /// Attribute database configuration (@see enum gapm_att_cfg_flag)
    ///    7     6    5     4     3    2    1    0
    /// +-----+-----+----+-----+-----+----+----+----+
    /// | DBG | RFU | SC | PCP | APP_PERM |NAME_PERM|
    /// +-----+-----+----+-----+-----+----+----+----+
    /// - Bit [0-1]: Device Name write permission requirements for peer device (@see device_name_write_perm)
    /// - Bit [2-3]: Device Appearance write permission requirements for peer device (@see device_appearance_write_perm)
    /// - Bit [4]  : Slave Preferred Connection Parameters present
    /// - Bit [5]  : Service change feature present in GATT attribute database.
    /// - Bit [6]  : Reserved
    /// - Bit [7]  : Enable Debug Mode
    .att_cfg = GAPM_MASK_ATT_SVC_CHG_EN,

    /// GAP service start handle
    .gap_start_hdl = 0,

    /// GATT service start handle
    .gatt_start_hdl = 0,

    /**************************************************
     * Data packet length extension configuration (4.2)
     **************************************************
     */

    /// Maximal MPS
    .max_mps = 0,

    /// Maximal Tx octets (connInitialMaxTxOctets value, as defined in 4.2 Specification)
    .max_txoctets = 0,

    /// Maximal Tx time (connInitialMaxTxTime value, as defined in 4.2 Specification)
    .max_txtime = 0,
};

/*
 ****************************************************************************************
 *
 * Parameter update configuration
 *
 ****************************************************************************************
 */
static const struct connection_param_configuration user_connection_param_conf = {
    /// Connection interval minimum measured in ble double slots (1.25ms)
    /// use the macro MS_TO_DOUBLESLOTS to convert from milliseconds (ms) to double slots
    .intv_min = MS_TO_DOUBLESLOTS(10),

    /// Connection interval maximum measured in ble double slots (1.25ms)
    /// use the macro MS_TO_DOUBLESLOTS to convert from milliseconds (ms) to double slots
    .intv_max = MS_TO_DOUBLESLOTS(20),

    /// Latency measured in connection events
    .latency = 0,

    /// Supervision timeout measured in timer units (10 ms)
    /// use the macro MS_TO_TIMERUNITS to convert from milliseconds (ms) to timer units
    .time_out = MS_TO_TIMERUNITS(1250),

    /// Minimum Connection Event Duration measured in ble double slots (1.25ms)
    /// use the macro MS_TO_DOUBLESLOTS to convert from milliseconds (ms) to double slots
    .ce_len_min = MS_TO_DOUBLESLOTS(0),

    /// Maximum Connection Event Duration measured in ble double slots (1.25ms)
    /// use the macro MS_TO_DOUBLESLOTS to convert from milliseconds (ms) to double slots
    .ce_len_max = MS_TO_DOUBLESLOTS(0),
};

/*
 ****************************************************************************************
 *
 * Default handlers configuration (applies only for @app_default_handlers.c)
 *
 ****************************************************************************************
 */
static const struct default_handlers_configuration  user_default_hnd_conf = {
    // Configure the advertise operation used by the default handlers
    // Possible values:
    //  - DEF_ADV_FOREVER
    //  - DEF_ADV_WITH_TIMEOUT
    .adv_scenario = DEF_ADV_FOREVER,

    // Configure the advertise period in case of DEF_ADV_WITH_TIMEOUT.
    // It is measured in timer units (3 min). Use MS_TO_TIMERUNITS macro to convert
    // from milliseconds (ms) to timer units.
    .advertise_period = MS_TO_TIMERUNITS(180000),

    // Configure the security start operation of the default handlers
    // if the security is enabled (CFG_APP_SECURITY)
    // Possible values:
    //  - DEF_SEC_REQ_NEVER
    //  - DEF_SEC_REQ_ON_CONNECT
    .security_request_scenario = DEF_SEC_REQ_NEVER
};

/*
 ****************************************************************************************
 *
 * Central configuration (not used by current example)
 *
 ****************************************************************************************
 */
static const struct central_configuration user_central_conf = {
    /// GAPM requested operation:
    /// - GAPM_CONNECTION_DIRECT: Direct connection operation
    /// - GAPM_CONNECTION_AUTO: Automatic connection operation
    /// - GAPM_CONNECTION_SELECTIVE: Selective connection operation
    /// - GAPM_CONNECTION_NAME_REQUEST: Name Request operation (requires to start a direct
    ///   connection)
    .code = GAPM_CONNECTION_DIRECT,

    /// Own BD address source of the device:
    .addr_src = APP_CFG_ADDR_SRC(USER_CFG_ADDRESS_MODE),

    /// Scan interval
    .scan_interval = 0x180,

    /// Scan window size
    .scan_window = 0x160,

     /// Minimum of connection interval
    .con_intv_min = 100,

    /// Maximum of connection interval
    .con_intv_max = 100,

    /// Connection latency
    .con_latency = 0,

    /// Link supervision timeout
    .superv_to = 0x1F4,

     /// Minimum CE length
    .ce_len_min = 0,

    /// Maximum CE length
    .ce_len_max = 0x5,

    /**************************************************************************************
     * Peer device information (maximum number of peers = 8)
     **************************************************************************************
     */

    /// BD Address of device
    .peer_addr_0 = {0x0, 0x0, 0x0, 0x0, 0x0, 0x0},

    /// Address type of the device 0=public/1=random
    .peer_addr_0_type = 0,

    /// BD Address of device
    .peer_addr_1 = {0x0, 0x0, 0x0, 0x0, 0x0, 0x0},

    /// Address type of the device 0=public/1=random
    .peer_addr_1_type = 0,

    /// BD Address of device
    .peer_addr_2 = {0x0, 0x0, 0x0, 0x0, 0x0, 0x0},

    /// Address type of the device 0=public/1=random
    .peer_addr_2_type = 0,

    /// BD Address of device
    .peer_addr_3 = {0x0, 0x0, 0x0, 0x0, 0x0, 0x0},

    /// Address type of the device 0=public/1=random
    .peer_addr_3_type = 0,

    /// BD Address of device
    .peer_addr_4 = {0x0, 0x0, 0x0, 0x0, 0x0, 0x0},

    /// Address type of the device 0=public/1=random
    .peer_addr_4_type = 0,

    /// BD Address of device
    .peer_addr_5 = {0x0, 0x0, 0x0, 0x0, 0x0, 0x0},

    /// Address type of the device 0=public/1=random
    .peer_addr_5_type = 0,

    /// BD Address of device
    .peer_addr_6 = {0x0, 0x0, 0x0, 0x0, 0x0, 0x0},

    /// Address type of the device 0=public/1=random
    .peer_addr_6_type = 0,

    /// BD Address of device
    .peer_addr_7 = {0x0, 0x0, 0x0, 0x0, 0x0, 0x0},

    /// Address type of the device 0=public/1=random
    .peer_addr_7_type = 0,
};

/*
 ****************************************************************************************
 *
 * Security related configuration
 *
 ****************************************************************************************
 */
static const struct security_configuration user_security_conf = {
    // IO Capabilities
    #if defined (USER_CFG_FEAT_IO_CAP)
    .iocap          = USER_CFG_FEAT_IO_CAP,
    #else
    .iocap          = GAP_IO_CAP_NO_INPUT_NO_OUTPUT,
    #endif

    // OOB Capabilities
    #if defined (USER_CFG_FEAT_OOB)
    .oob            = USER_CFG_FEAT_OOB,
    #else
    .oob            = GAP_OOB_AUTH_DATA_NOT_PRESENT,
    #endif

    // Authentication Requirements
    #if defined (USER_CFG_FEAT_AUTH_REQ)
    .auth           = USER_CFG_FEAT_AUTH_REQ,
    #else
    .auth           = GAP_AUTH_NONE,
    #endif

    // LTK size
    #if defined (USER_CFG_FEAT_KEY_SIZE)
    .key_size       = USER_CFG_FEAT_KEY_SIZE,
    #else
    .key_size       = KEY_LEN,
    #endif

    // Initiator key distribution
    #if defined (USER_CFG_FEAT_INIT_KDIST)
    .ikey_dist      = USER_CFG_FEAT_INIT_KDIST,
    #else
    .ikey_dist      = GAP_KDIST_NONE,
    #endif

    // Responder key distribution
    #if defined (USER_CFG_FEAT_RESP_KDIST)
    .rkey_dist      = USER_CFG_FEAT_RESP_KDIST,
    #else
    .rkey_dist      = GAP_KDIST_ENCKEY,
    #endif

    // Security requirements (minimum security level)
    #if defined (USER_CFG_FEAT_SEC_REQ)
    .sec_req        = USER_CFG_FEAT_SEC_REQ,
    #else
    .sec_req        = GAP_NO_SEC,
    #endif
};

#endif // _USER_CONFIG_H_
