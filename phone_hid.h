#ifndef __PHONE_HID_H__
#define __PHONE_HID_H__

// ============================================================================
//  手机遥控（BLE HID）
//
//  小智作为标准 BLE HID 设备（键盘 + 消费类媒体键）连到 iPhone：
//    - iPhone 设置 -> 蓝牙 -> 选 "XiaoZhi-Phone" -> 输入配对码 123456
//    - 连上后通过 MCP 工具发按键，控制 iPhone 的音量/播放/切歌/翻页/文字输入
//
//  为什么走 HID 而不是私有 App：
//    iOS 不允许第三方 App 在后台模拟系统级按键；标准 HID 键盘是系统原生支持的
//    通道，不需要在手机装任何东西。
//
//  隔离性：
//    只开 BLE（S3 没有经典蓝牙），不碰 I2S / WiFi / 显示 / 现有按键。
//    初始化失败只会让这几个工具不可用，不影响小智原有功能。
//
//  实测注意：
//    - BLE 与 WiFi 共用 2.4G，语音流可能略有抖动，属于预期。
//    - HID 音量键控制的是 iPhone 的「媒体音量」，铃声音量不变。
// ============================================================================

#include "mcp_server.h"

#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_err.h>
#include <esp_gap_ble_api.h>
#include <esp_gatts_api.h>
#include <esp_hidd.h>
#include <esp_log.h>

// esp_hidd.h 在 IDF v6.1 未直接声明这个 GATTS 分发函数；
// 官方 example 也没有 include esp_hidd_gatts.h（那个头的宏条件是旧的
// CONFIG_GATTS_ENABLE，与 v6.1 的 CONFIG_BT_GATTS_ENABLE 不匹配）。
// 这里按官方 example 的调用方式自行声明，签名与 esp_hidd_gatts.c 一致。
extern "C" void esp_hidd_gatts_event_handler(esp_gatts_cb_event_t event,
                                             esp_gatt_if_t gatts_if,
                                             esp_ble_gatts_cb_param_t* param);

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>
#include <string>

#define PHONE_HID_TAG "PhoneHid"

#define PHONE_HID_NAME "XiaoZhi-Phone"
#define PHONE_HID_APPEARANCE 0x03C1  // ESP_HID_APPEARANCE_KEYBOARD
#define PHONE_HID_PASSKEY 123456     // 静态配对码，iPhone 弹窗输入这个

// ---- HID Report Map --------------------------------------------------------
// 一个 map 里放两个 Application Collection：
//   Report ID 1 = 标准 8 字节键盘（含 8 修饰键 + 6 按键）
//   Report ID 3 = 16-bit Consumer Control（直接发 usage id，iOS 兼容性最好）
static const uint8_t phone_hid_report_map[] = {
    // ---- Keyboard (Report ID 1) ----
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,        //   Usage Minimum (Left Control)
    0x29, 0xE7,        //   Usage Maximum (Right GUI)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data,Var,Abs) - modifiers
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x03,        //   Input (Cnst,Var,Abs) - reserved
    0x95, 0x05,        //   Report Count (5)
    0x75, 0x01,        //   Report Size (1)
    0x05, 0x08,        //   Usage Page (LEDs)
    0x19, 0x01,        //   Usage Minimum (Num Lock)
    0x29, 0x05,        //   Usage Maximum (Kana)
    0x91, 0x02,        //   Output (Data,Var,Abs)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x03,        //   Report Size (3)
    0x91, 0x03,        //   Output (Cnst,Var,Abs)
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x65,        //   Logical Maximum (101)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0x00,        //   Usage Minimum (0)
    0x29, 0x65,        //   Usage Maximum (101)
    0x81, 0x00,        //   Input (Data,Array,Abs)
    0xC0,              // End Collection

    // ---- Consumer Control (Report ID 3) ----
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x03,        //   Report ID (3)
    0x75, 0x10,        //   Report Size (16)
    0x95, 0x01,        //   Report Count (1)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x03,  //   Logical Maximum (1023)
    0x19, 0x00,        //   Usage Minimum (0)
    0x2A, 0xFF, 0x03,  //   Usage Maximum (1023)
    0x81, 0x00,        //   Input (Data,Array,Abs)
    0xC0,              // End Collection
};

// Consumer Page usages（16-bit，小端发送）
#define HID_CC_PLAY_PAUSE 0x00CD
#define HID_CC_NEXT_TRACK 0x00B5
#define HID_CC_PREV_TRACK 0x00B6
#define HID_CC_VOLUME_UP  0x00E9
#define HID_CC_VOLUME_DOWN 0x00EA
#define HID_CC_MUTE       0x00E2

// 键盘修饰键 bit（HID modifier byte）
#define HID_MOD_LCTRL  0x01
#define HID_MOD_LSHIFT 0x02
#define HID_MOD_LALT   0x04
#define HID_MOD_LGUI   0x08
#define HID_MOD_RCTRL  0x10
#define HID_MOD_RSHIFT 0x20
#define HID_MOD_RALT   0x40
#define HID_MOD_RGUI   0x80

class PhoneHid {
public:
    PhoneHid() {
        instance_ = this;
        if (!InitBle()) {
            ESP_LOGW(PHONE_HID_TAG, "BLE HID 初始化失败，手机遥控工具未注册");
            ready_ = false;
            return;
        }
        ready_ = true;
        RegisterTools();
    }

    bool IsConnected() const {
        return ready_ && dev_ != nullptr && esp_hidd_dev_connected(dev_);
    }

    static const char* PairingCode() { return "123456"; }

private:
    static PhoneHid* instance_;
    esp_hidd_dev_t* dev_ = nullptr;
    bool ready_ = false;
    volatile bool adv_data_set_ = false;

    // ---- 键盘字符映射 ------------------------------------------------------
    // 只支持 ASCII，中文请用语音让小智自己说，不要走键盘输入
    static void CharToHid(char c, uint8_t& mod, uint8_t& key) {
        mod = 0;
        key = 0;
        if (c >= 'a' && c <= 'z') {
            key = 4 + (c - 'a');
        } else if (c >= 'A' && c <= 'Z') {
            mod = HID_MOD_LSHIFT;
            key = 4 + (c - 'A');
        } else if (c >= '0' && c <= '9') {
            key = (c == '0') ? 39 : (uint8_t)(30 + (c - '1'));
        } else {
            switch (c) {
                case ' ':  key = 0x2C; break;
                case '\n': key = 0x28; break;  // Enter
                case '\r': key = 0x28; break;
                case '\t': key = 0x2B; break;
                case '\b': key = 0x2A; break;  // Backspace
                case '.':  key = 0x37; break;
                case ',':  key = 0x36; break;
                case '/':  key = 0x38; break;
                case '?':  mod = HID_MOD_LSHIFT; key = 0x38; break;
                case '-':  key = 0x2D; break;
                case '_':  mod = HID_MOD_LSHIFT; key = 0x2D; break;
                case '=':  key = 0x2E; break;
                case '+':  mod = HID_MOD_LSHIFT; key = 0x2E; break;
                case '!':  mod = HID_MOD_LSHIFT; key = 30; break;
                case '@':  mod = HID_MOD_LSHIFT; key = 31; break;
                case '#':  mod = HID_MOD_LSHIFT; key = 32; break;
                case '$':  mod = HID_MOD_LSHIFT; key = 33; break;
                case '%':  mod = HID_MOD_LSHIFT; key = 34; break;
                case '^':  mod = HID_MOD_LSHIFT; key = 35; break;
                case '&':  mod = HID_MOD_LSHIFT; key = 36; break;
                case '*':  mod = HID_MOD_LSHIFT; key = 37; break;
                case '(':  mod = HID_MOD_LSHIFT; key = 38; break;
                case ')':  mod = HID_MOD_LSHIFT; key = 39; break;
                case '\'': key = 0x34; break;
                case '"':  mod = HID_MOD_LSHIFT; key = 0x34; break;
                case ';':  key = 0x33; break;
                case ':':  mod = HID_MOD_LSHIFT; key = 0x33; break;
                case '[':  key = 0x2F; break;
                case '{':  mod = HID_MOD_LSHIFT; key = 0x2F; break;
                case ']':  key = 0x30; break;
                case '}':  mod = HID_MOD_LSHIFT; key = 0x30; break;
                case '\\': key = 0x31; break;
                case '|':  mod = HID_MOD_LSHIFT; key = 0x31; break;
                case '`':  key = 0x35; break;
                case '~':  mod = HID_MOD_LSHIFT; key = 0x35; break;
                default:   key = 0; break;  // 不支持的字符，跳过
            }
        }
    }

    // ---- 底层发送 ----------------------------------------------------------
    void SendKeyboard(uint8_t mod, uint8_t key) {
        if (dev_ == nullptr) return;
        uint8_t report[8] = {mod, 0, key, 0, 0, 0, 0, 0};
        esp_hidd_dev_input_set(dev_, 0, 1, report, sizeof(report));
        vTaskDelay(pdMS_TO_TICKS(30));
        uint8_t release[8] = {0};
        esp_hidd_dev_input_set(dev_, 0, 1, release, sizeof(release));
    }

    void SendConsumer(uint16_t usage) {
        if (dev_ == nullptr) return;
        uint8_t report[2] = {(uint8_t)(usage & 0xFF), (uint8_t)(usage >> 8)};
        esp_hidd_dev_input_set(dev_, 0, 3, report, sizeof(report));
        vTaskDelay(pdMS_TO_TICKS(40));
        uint8_t release[2] = {0, 0};
        esp_hidd_dev_input_set(dev_, 0, 3, release, sizeof(release));
    }

    // ---- BLE 初始化 --------------------------------------------------------
    static void GapCallback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
        switch (event) {
            case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
                if (instance_ != nullptr) instance_->adv_data_set_ = true;
                ESP_LOGI(PHONE_HID_TAG, "广播数据已设置，开始广播 %s", PHONE_HID_NAME);
                esp_ble_gap_start_advertising(&instance_->adv_params_);
                break;
            case ESP_GAP_BLE_AUTH_CMPL_EVT:
                if (param->ble_security.auth_cmpl.success) {
                    ESP_LOGI(PHONE_HID_TAG, "iPhone 配对成功");
                } else {
                    ESP_LOGE(PHONE_HID_TAG, "配对失败 reason=0x%x",
                             param->ble_security.auth_cmpl.fail_reason);
                }
                break;
            case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
                ESP_LOGI(PHONE_HID_TAG, "配对码：%06u（在 iPhone 上输入）",
                         (unsigned)param->ble_security.key_notif.passkey);
                break;
            default:
                break;
        }
    }

    static void HidEventCallback(void* arg, esp_event_base_t base, int32_t id, void* event_data) {
        switch ((esp_hidd_event_t)id) {
            case ESP_HIDD_CONNECT_EVENT:
                ESP_LOGI(PHONE_HID_TAG, "iPhone 已连接，手机遥控就绪");
                break;
            case ESP_HIDD_DISCONNECT_EVENT: {
                ESP_LOGI(PHONE_HID_TAG, "iPhone 断开，重新开始广播");
                esp_ble_gap_start_advertising(&instance_->adv_params_);
                break;
            }
            default:
                break;
        }
    }

    bool InitBle() {
        esp_err_t ret;

        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        // S3 没有经典蓝牙，释放其控制器内存给 BLE / WiFi 用
        esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

        ret = esp_bt_controller_init(&bt_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(PHONE_HID_TAG, "bt controller init 失败: %s", esp_err_to_name(ret));
            return false;
        }
        ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
        if (ret != ESP_OK) {
            ESP_LOGE(PHONE_HID_TAG, "bt controller enable 失败: %s", esp_err_to_name(ret));
            return false;
        }

        esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
        ret = esp_bluedroid_init_with_cfg(&bluedroid_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(PHONE_HID_TAG, "bluedroid init 失败: %s", esp_err_to_name(ret));
            return false;
        }
        ret = esp_bluedroid_enable();
        if (ret != ESP_OK) {
            ESP_LOGE(PHONE_HID_TAG, "bluedroid enable 失败: %s", esp_err_to_name(ret));
            return false;
        }

        ret = esp_ble_gap_register_callback(GapCallback);
        if (ret != ESP_OK) {
            ESP_LOGE(PHONE_HID_TAG, "GAP callback 注册失败: %s", esp_err_to_name(ret));
            return false;
        }
        // 关键：HID GATT service 的事件必须交给 esp_hid 组件处理
        ret = esp_ble_gatts_register_callback(esp_hidd_gatts_event_handler);
        if (ret != ESP_OK) {
            ESP_LOGE(PHONE_HID_TAG, "GATTS callback 注册失败: %s", esp_err_to_name(ret));
            return false;
        }

        static esp_hid_raw_report_map_t report_maps[] = {
            {.data = phone_hid_report_map, .len = sizeof(phone_hid_report_map)},
        };
        static esp_hid_device_config_t hid_config = {
            .vendor_id = 0x16C0,
            .product_id = 0x05DF,
            .version = 0x0100,
            .device_name = PHONE_HID_NAME,
            .manufacturer_name = "XiaoZhi",
            .serial_number = "00000001",
            .report_maps = report_maps,
            .report_maps_len = 1,
        };
        ret = esp_hidd_dev_init(&hid_config, ESP_HID_TRANSPORT_BLE, HidEventCallback, &dev_);
        if (ret != ESP_OK || dev_ == nullptr) {
            ESP_LOGE(PHONE_HID_TAG, "HID device init 失败: %s", esp_err_to_name(ret));
            return false;
        }
        esp_hidd_dev_battery_set(dev_, 100);

        // 配对安全参数：与 ESP-IDF 官方 HID example 一致
        // iOS 需要 Secure Connections + passkey，Just Works 在部分 iOS 版本连不上
        esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_MITM_BOND;
        esp_ble_io_cap_t iocap = ESP_IO_CAP_OUT;  // 设备显示密码，iPhone 上输入
        uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
        uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
        uint8_t key_size = 16;
        uint32_t passkey = PHONE_HID_PASSKEY;
        esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));
        esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(uint8_t));
        esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(uint8_t));
        esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(uint8_t));
        esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(uint8_t));
        esp_ble_gap_set_security_param(ESP_BLE_SM_SET_STATIC_PASSKEY, &passkey, sizeof(uint32_t));

        ret = esp_ble_gap_set_device_name(PHONE_HID_NAME);
        if (ret != ESP_OK) {
            ESP_LOGE(PHONE_HID_TAG, "设置设备名失败: %s", esp_err_to_name(ret));
            return false;
        }

        const uint8_t hidd_uuid128[16] = {
            0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
            0x00, 0x10, 0x00, 0x00, 0x12, 0x18, 0x00, 0x00,
        };
        esp_ble_adv_data_t adv_data = {
            .set_scan_rsp = false,
            .include_name = true,
            .include_txpower = true,
            .min_interval = 0x0006,
            .max_interval = 0x0010,
            .appearance = PHONE_HID_APPEARANCE,
            .manufacturer_len = 0,
            .p_manufacturer_data = nullptr,
            .service_data_len = 0,
            .p_service_data = nullptr,
            .service_uuid_len = sizeof(hidd_uuid128),
            .p_service_uuid = const_cast<uint8_t*>(hidd_uuid128),
            .flag = 0x06,
        };
        ret = esp_ble_gap_config_adv_data(&adv_data);
        if (ret != ESP_OK) {
            ESP_LOGE(PHONE_HID_TAG, "配置广播数据失败: %s", esp_err_to_name(ret));
            return false;
        }
        // 广播在 GapCallback 收到 ADV_DATA_SET_COMPLETE 后启动
        ESP_LOGI(PHONE_HID_TAG, "BLE HID 已初始化，等待广播（配对码 %s）", PairingCode());
        return true;
    }

    // 广播参数放在实例里，回调断开后要重启广播
    esp_ble_adv_params_t adv_params_ = {
        .adv_int_min = 0x20,
        .adv_int_max = 0x30,
        .adv_type = ADV_TYPE_IND,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .channel_map = ADV_CHNL_ALL,
        .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
    };

    // ---- MCP 工具 ----------------------------------------------------------
    void RegisterTools() {
        auto& mcp = McpServer::GetInstance();

        mcp.AddTool(
            "self.phone.status",
            "Get the Bluetooth connection status of the phone remote. "
            "Returns connected flag, device name and the pairing code. "
            "Call this when the user asks 手机连上了吗 / 蓝牙状态 / 配对码是多少.",
            PropertyList(), [this](const PropertyList&) -> ReturnValue {
                char json[192];
                snprintf(json, sizeof(json),
                         "{\"connected\": %s, \"device_name\": \"%s\", "
                         "\"pairing_code\": \"%s\", "
                         "\"hint\": \"Open iPhone Settings > Bluetooth, connect XiaoZhi-Phone, enter code 123456\"}",
                         IsConnected() ? "true" : "false", PHONE_HID_NAME, PairingCode());
                return std::string(json);
            });

        mcp.AddTool(
            "self.phone.media",
            "Control the paired iPhone's media playback via Bluetooth HID. "
            "action values: play_pause, next, prev, vol_up, vol_down, mute. "
            "repeat is how many times to send the key (1-10). "
            "Call this when the user says 播放/暂停音乐, 下一首, 上一首, 手机音量调大/调小, 静音. "
            "Note: this controls the iPhone's MEDIA volume, not the ringtone volume.",
            PropertyList(std::vector<Property>{
                Property("action", kPropertyTypeString, "play_pause").SetMaxLength(16),
                Property("repeat", kPropertyTypeInteger, 1, 1, 10),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                if (!IsConnected()) {
                    return std::string("手机蓝牙还没连上。请在 iPhone 设置-蓝牙中连接 XiaoZhi-Phone，配对码 123456");
                }
                const std::string action = properties["action"].value<std::string>();
                const int repeat = properties["repeat"].value<int>();

                uint16_t usage = 0;
                if (action == "play_pause") usage = HID_CC_PLAY_PAUSE;
                else if (action == "next") usage = HID_CC_NEXT_TRACK;
                else if (action == "prev") usage = HID_CC_PREV_TRACK;
                else if (action == "vol_up") usage = HID_CC_VOLUME_UP;
                else if (action == "vol_down") usage = HID_CC_VOLUME_DOWN;
                else if (action == "mute") usage = HID_CC_MUTE;
                else return std::string("不支持的 action: " + action);

                for (int i = 0; i < repeat; ++i) {
                    SendConsumer(usage);
                }
                char json[128];
                snprintf(json, sizeof(json),
                         "{\"ok\": true, \"action\": \"%s\", \"repeat\": %d}",
                         action.c_str(), repeat);
                return std::string(json);
            });

        mcp.AddTool(
            "self.phone.key",
            "Send a keyboard key or shortcut to the paired iPhone via Bluetooth HID. "
            "combo format: single key name (enter, esc, tab, space, backspace, page_up, page_down, "
            "home, end, up, down, left, right) or modifier+key (ctrl+c, alt+tab, shift+end, cmd+space). "
            "Call this when the user says 在手机上按回车 / 翻页 / 复制 / 搜索 / 回到主屏幕. "
            "The keyboard only works when a text field is focused on the iPhone.",
            PropertyList(std::vector<Property>{
                Property("combo", kPropertyTypeString, "enter").SetMaxLength(32),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                if (!IsConnected()) {
                    return std::string("手机蓝牙还没连上，无法发送按键");
                }
                const std::string combo = properties["combo"].value<std::string>();

                uint8_t mod = 0;
                uint8_t explicit_mod = 0;  // 由 "cmd+c" 这类前缀显式指定
                std::string key_name = combo;
                // 解析 modifier+key
                size_t plus = combo.find('+');
                if (plus != std::string::npos) {
                    const std::string m = combo.substr(0, plus);
                    key_name = combo.substr(plus + 1);
                    if (m == "ctrl") explicit_mod = HID_MOD_LGUI;  // iOS 上 Ctrl 基本用不到，映射到 Command
                    else if (m == "cmd") explicit_mod = HID_MOD_LGUI;
                    else if (m == "shift") explicit_mod = HID_MOD_LSHIFT;
                    else if (m == "alt") explicit_mod = HID_MOD_LALT;
                    else if (m == "option") explicit_mod = HID_MOD_LALT;
                    else return std::string("不支持的修饰键: " + m);
                }

                uint8_t key = 0;
                if (key_name.size() == 1) {
                    uint8_t char_mod = 0;
                    CharToHid(key_name[0], char_mod, key);
                    mod = static_cast<uint8_t>(char_mod | explicit_mod);
                } else if (key_name == "enter") key = 0x28;
                else if (key_name == "esc" || key_name == "escape") key = 0x29;
                else if (key_name == "tab") key = 0x2B;
                else if (key_name == "space") key = 0x2C;
                else if (key_name == "backspace") key = 0x2A;
                else if (key_name == "delete") key = 0x4C;
                else if (key_name == "page_up") key = 0x4B;
                else if (key_name == "page_down") key = 0x4E;
                else if (key_name == "home") key = 0x4A;
                else if (key_name == "end") key = 0x4D;
                else if (key_name == "up") key = 0x52;
                else if (key_name == "down") key = 0x51;
                else if (key_name == "left") key = 0x50;
                else if (key_name == "right") key = 0x4F;
                else return std::string("不支持的按键: " + key_name);

                if (key == 0) return std::string("无法映射按键: " + combo);
                mod = static_cast<uint8_t>(mod | explicit_mod);
                SendKeyboard(mod, key);
                char json[128];
                snprintf(json, sizeof(json), "{\"ok\": true, \"combo\": \"%s\"}", combo.c_str());
                return std::string(json);
            });

        mcp.AddTool(
            "self.phone.type",
            "Type ASCII text into the focused text field on the paired iPhone via Bluetooth HID. "
            "Only ASCII letters, digits and common punctuation are supported; Chinese text is not "
            "supported by HID keyboard. Use this when the user says 在手机上输入 hello / 在备忘录里打字. "
            "The iPhone must have a text field focused first.",
            PropertyList(std::vector<Property>{
                Property("text", kPropertyTypeString, "").SetMaxLength(120),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                if (!IsConnected()) {
                    return std::string("手机蓝牙还没连上，无法输入文字");
                }
                const std::string text = properties["text"].value<std::string>();
                size_t sent = 0;
                for (char c : text) {
                    uint8_t mod = 0, key = 0;
                    CharToHid(c, mod, key);
                    if (key == 0) continue;  // 跳过不支持的字符
                    SendKeyboard(mod, key);
                    ++sent;
                }
                char json[96];
                snprintf(json, sizeof(json), "{\"ok\": true, \"sent_chars\": %u}", (unsigned)sent);
                return std::string(json);
            });

        ESP_LOGI(PHONE_HID_TAG, "手机遥控工具已注册（BLE HID）");
    }
};

PhoneHid* PhoneHid::instance_ = nullptr;

#endif // __PHONE_HID_H__
