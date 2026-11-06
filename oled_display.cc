#include "oled_display.h"
#include "assets/lang_config.h"
#include "lvgl_font.h"
#include "lvgl_theme.h"
#include "settings.h"

#include <algorithm>
#include <cctype>
#include <string>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_lvgl_port.h>
#include <material_symbols.h>
#include <noto_emoji.h>

#define TAG "OledDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_material_symbols_30_1);
LV_FONT_DECLARE(font_noto_emoji_30_1);

// ==================== 大眼睛表情 ====================
// 说明：单色 OLED 上「点亮点」= LVGL 的黑色，所以眼睛填充用 lv_color_black()。
namespace {

struct EyeShape {
    int left;   // 左眼睁开度，100 = 全开
    int right;  // 右眼睁开度
};

EyeShape EyesForEmotion(const char* emotion) {
    if (emotion == nullptr || emotion[0] == '\0') {
        return {100, 100};
    }
    std::string e(emotion);
    for (auto& c : e) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    if (e == "happy" || e == "laughing" || e == "funny") return {55, 55};
    if (e == "sad" || e == "crying") return {45, 45};
    if (e == "angry") return {38, 38};
    if (e == "sleepy") return {16, 16};
    if (e == "relaxed" || e == "loving" || e == "kissy") return {55, 55};
    if (e == "surprised" || e == "shocked") return {118, 118};
    if (e == "thinking") return {88, 52};
    if (e == "confused") return {52, 88};
    if (e == "winking") return {100, 6};
    if (e == "silly") return {100, 48};
    if (e == "embarrassed") return {42, 42};
    if (e == "cool" || e == "confident") return {70, 70};
    if (e == "delicious") return {62, 62};
    return {100, 100};  // neutral / listening / 未知
}

void EyeBlinkAnimCb(void* var, int32_t value) {
    static_cast<OledDisplay*>(var)->SetEyeScale(static_cast<int>(value));
}

// 按下发的「睁开度」摆一只眼睛：高度变化、垂直居中、圆角取短边一半
void ShapeEye(lv_obj_t* eye, int w, int h, int x, int y, int scale, int blink) {
    if (eye == nullptr) {
        return;
    }
    int hh = h * scale * blink / 10000;
    if (hh < 2) hh = 2;
    int maxh = h + h / 5;
    if (hh > maxh) hh = maxh;
    lv_obj_set_size(eye, w, hh);
    lv_obj_set_pos(eye, x, y + (h - hh) / 2);
    lv_obj_set_style_radius(eye, std::min(w, hh) / 2, 0);
}

}  // namespace

OledDisplay::OledDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                         int width, int height, bool mirror_x, bool mirror_y)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_material_symbols_30_1);
    auto emoji_font = std::make_shared<LvglBuiltInFont>(&font_noto_emoji_30_1);

    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);
    dark_theme->set_emoji_font(emoji_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("dark", dark_theme);
    current_theme_ = dark_theme;

    ESP_LOGI(TAG, "Initialize LVGL");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.task_stack = 6144;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding OLED display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * height_),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = true,
        .rotation =
            {
                .swap_xy = false,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = 1,
                .buff_spiram = 0,
                .sw_rotate = 0,
                .full_refresh = 0,
                .direct_mode = 0,
            },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    // Note: SetupUI() should be called by Application::Initialize(), not in constructor
    // to ensure lvgl objects are created after the display is fully initialized.
}

void OledDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    Display::SetupUI();  // Mark SetupUI as called
    if (height_ == 64) {
        SetupUI_128x64();
    } else {
        SetupUI_128x32();
    }
}

OledDisplay::~OledDisplay() {
    if (blink_timer_ != nullptr) {
        lv_timer_delete(blink_timer_);
        blink_timer_ = nullptr;
    }
    if (content_ != nullptr) {
        lv_obj_del(content_);
    }

    bool is_128x64_layout = (top_bar_ != nullptr);
    if (status_bar_ != nullptr && is_128x64_layout) {
        status_label_ = nullptr;
        notification_label_ = nullptr;
        lv_obj_del(status_bar_);
    }
    if (top_bar_ != nullptr) {
        network_label_ = nullptr;
        mute_label_ = nullptr;
        battery_label_ = nullptr;
        lv_obj_del(top_bar_);
    }
    if (side_bar_ != nullptr) {
        if (!is_128x64_layout) {
            status_label_ = nullptr;
            notification_label_ = nullptr;
            network_label_ = nullptr;
            mute_label_ = nullptr;
            battery_label_ = nullptr;
        }
        lv_obj_del(side_bar_);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
    }

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
    lvgl_port_deinit();
}

bool OledDisplay::Lock(int timeout_ms) { return lvgl_port_lock(timeout_ms); }

void OledDisplay::Unlock() { lvgl_port_unlock(); }

void OledDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        return;
    }

    // Replace all newlines with spaces
    std::string content_str = content;
    std::replace(content_str.begin(), content_str.end(), '\n', ' ');

    lv_anim_delete(chat_message_label_, nullptr);
    if (content_right_ == nullptr) {
        lv_label_set_text(chat_message_label_, content_str.c_str());
        has_chat_text_ = !(content == nullptr || content[0] == '\0');
        UpdateLayoutMode();
    } else {
        if (content == nullptr || content[0] == '\0') {
            lv_obj_add_flag(content_right_, LV_OBJ_FLAG_HIDDEN);
            if (content_left_ != nullptr && eyes_area_ != nullptr) {
                lv_obj_remove_flag(content_left_, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_label_set_text(chat_message_label_, content_str.c_str());
            lv_obj_remove_flag(content_right_, LV_OBJ_FLAG_HIDDEN);
            // 128x64 屏：有字幕时把整块宽度让给文字，眼睛暂时隐藏
            if (content_left_ != nullptr && eyes_area_ != nullptr && height_ > 32) {
                lv_obj_add_flag(content_left_, LV_OBJ_FLAG_HIDDEN);
            }
        }
        has_chat_text_ = !(content == nullptr || content[0] == '\0');
        UpdateLayoutMode();
    }
}

void OledDisplay::RemoveChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr || content == nullptr) {
        return;
    }
    // OLED has one text slot and does not distinguish message roles.
    std::string content_str = content;
    std::replace(content_str.begin(), content_str.end(), '\n', ' ');
    if (content_str != lv_label_get_text(chat_message_label_)) {
        return;
    }
    lv_anim_delete(chat_message_label_, nullptr);
    lv_label_set_text(chat_message_label_, "");
    if (content_right_ != nullptr) {
        lv_obj_add_flag(content_right_, LV_OBJ_FLAG_HIDDEN);
    }
    has_chat_text_ = false;
    UpdateLayoutMode();
}

void OledDisplay::SetupUI_128x64() {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);

    /* Layer 1: Top bar - for status icons */
    top_bar_ = lv_obj_create(container_);
    lv_obj_set_size(top_bar_, LV_HOR_RES, 16);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);

    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);

    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);

    /* Layer 2: Status bar - for center text labels */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, 16);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);        // Overlap with top_bar_

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    /* Content */
    content_ = lv_obj_create(container_);
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_style_pad_all(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_flex_main_place(content_, LV_FLEX_ALIGN_CENTER, 0);

    // ===== 大眼睛：内容区左侧整块作为眼睛舞台 =====
    content_left_ = lv_obj_create(content_);
    lv_obj_set_size(content_left_, LV_SIZE_CONTENT, LV_PCT(100));
    lv_obj_set_flex_grow(content_left_, 1);
    lv_obj_set_style_pad_all(content_left_, 0, 0);
    lv_obj_set_style_border_width(content_left_, 0, 0);
    lv_obj_set_style_bg_opa(content_left_, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(content_left_, LV_SCROLLBAR_MODE_OFF);
    SetupEyes(content_left_);
    (void)large_icon_font;

    content_right_ = lv_obj_create(content_);
    lv_obj_set_size(content_right_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(content_right_, 0, 0);
    lv_obj_set_style_border_width(content_right_, 0, 0);
    lv_obj_set_flex_grow(content_right_, 1);
    lv_obj_add_flag(content_right_, LV_OBJ_FLAG_HIDDEN);

    chat_message_label_ = lv_label_create(content_right_);
    lv_label_set_text(chat_message_label_, "");
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(chat_message_label_, width_ - 32);
    lv_obj_set_style_pad_top(chat_message_label_, 14, 0);

    // Start scrolling subtitle after a delay
    static lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_delay(&a, 1000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_obj_set_style_anim(chat_message_label_, &a, LV_PART_MAIN);
    lv_obj_set_style_anim_duration(chat_message_label_, lv_anim_speed_clamped(60, 300, 60000),
                                   LV_PART_MAIN);

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(low_battery_popup_, lv_color_black(), 0);
    lv_obj_set_style_radius(low_battery_popup_, 10, 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
}

void OledDisplay::SetupUI_128x32() {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_column(container_, 0, 0);

    /* Emotion label on the left side */
    content_ = lv_obj_create(container_);
    lv_obj_set_size(content_, 32, 32);
    lv_obj_set_style_pad_all(content_, 0, 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_radius(content_, 0, 0);

    /* 说话时：左侧 32x32 区域放一对小眼睛，右边滚字幕 */
    SetupEyes(content_);
    (void)large_icon_font;

    /* 待机时：全屏大眼睛，盖在 container_ 之上 */
    SetupFullScreenEyes(screen);
    UpdateLayoutMode();

    /* Right side */
    side_bar_ = lv_obj_create(container_);
    lv_obj_set_size(side_bar_, width_ - 32, 32);
    lv_obj_set_flex_flow(side_bar_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(side_bar_, 0, 0);
    lv_obj_set_style_border_width(side_bar_, 0, 0);
    lv_obj_set_style_radius(side_bar_, 0, 0);
    lv_obj_set_style_pad_row(side_bar_, 0, 0);

    /* Status bar */
    status_bar_ = lv_obj_create(side_bar_);
    lv_obj_set_size(status_bar_, width_ - 32, 16);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_flex_flow(status_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_column(status_bar_, 0, 0);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(status_label_, 1);
    lv_obj_set_style_pad_left(status_label_, 2, 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(notification_label_, 1);
    lv_obj_set_style_pad_left(notification_label_, 2, 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    mute_label_ = lv_label_create(status_bar_);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);

    network_label_ = lv_label_create(status_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);

    battery_label_ = lv_label_create(status_bar_);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);

    chat_message_label_ = lv_label_create(side_bar_);
    lv_obj_set_size(chat_message_label_, width_ - 32, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_left(chat_message_label_, 2, 0);
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(chat_message_label_, "");

    // Start scrolling subtitle after a delay
    static lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_delay(&a, 1000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_obj_set_style_anim(chat_message_label_, &a, LV_PART_MAIN);
    lv_obj_set_style_anim_duration(chat_message_label_, lv_anim_speed_clamped(60, 300, 60000),
                                   LV_PART_MAIN);
}

bool OledDisplay::SetEmotion(const char* emotion) {
    // 大眼睛模式：表情体现为眼睛的睁开度与左右不对称
    auto shape = EyesForEmotion(emotion);
    DisplayLockGuard lock(this);
    eye_scale_l_ = shape.left;
    eye_scale_r_ = shape.right;
    ApplyEyeShape();
    return true;
}

void OledDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    auto text_font = lvgl_theme->text_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
}

void OledDisplay::SetPowerSaveMode(bool on) {
    if (panel_) {
        Settings settings("wifi", false);
        if (settings.GetBool("power_save_display_off", false)) {
            esp_lcd_panel_disp_on_off(panel_, !on);
        }
    }
    LvglDisplay::SetPowerSaveMode(on);
}

// ==================== 大眼睛实现 ====================
void OledDisplay::SetupEyes(lv_obj_t* parent) {
    if (parent == nullptr) {
        return;
    }
    if (height_ <= 32) {
        // 说话时左侧 32x32 里的小眼睛
        eye_w_ = 11;
        eye_h_ = 20;
        eye_gap_ = 7;
    } else {
        // 0.96 寸 128x64
        eye_w_ = 26;
        eye_h_ = 34;
        eye_gap_ = 22;
    }

    eyes_area_ = lv_obj_create(parent);
    lv_obj_remove_style_all(eyes_area_);
    lv_obj_clear_flag(eyes_area_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(eyes_area_, eye_w_ * 2 + eye_gap_, eye_h_);
    lv_obj_set_style_bg_opa(eyes_area_, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(eyes_area_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_center(eyes_area_);

    eye_left_ = lv_obj_create(eyes_area_);
    lv_obj_remove_style_all(eye_left_);
    lv_obj_clear_flag(eye_left_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(eye_left_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(eye_left_, LV_OPA_COVER, 0);

    eye_right_ = lv_obj_create(eyes_area_);
    lv_obj_remove_style_all(eye_right_);
    lv_obj_clear_flag(eye_right_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(eye_right_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(eye_right_, LV_OPA_COVER, 0);

    ApplyEyeShape();

    // 眨眼：每 2.2~6 秒眨一次
    blink_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* self = static_cast<OledDisplay*>(lv_timer_get_user_data(timer));
            if (self == nullptr) {
                return;
            }
            lv_timer_set_period(timer, 2200 + (esp_random() % 3800));
            self->StartBlinkAnim();
        },
        3000, this);
}

void OledDisplay::SetupFullScreenEyes(lv_obj_t* parent) {
    if (parent == nullptr || height_ > 32) {
        return;  // 只有 128x32 做全屏眼睛
    }

    eyes_full_ = lv_obj_create(parent);
    lv_obj_remove_style_all(eyes_full_);
    lv_obj_clear_flag(eyes_full_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(eyes_full_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(eyes_full_, 0, 0);
    lv_obj_set_style_bg_opa(eyes_full_, LV_OPA_TRANSP, 0);

    eye_f_l_ = lv_obj_create(eyes_full_);
    lv_obj_remove_style_all(eye_f_l_);
    lv_obj_clear_flag(eye_f_l_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(eye_f_l_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(eye_f_l_, LV_OPA_COVER, 0);

    eye_f_r_ = lv_obj_create(eyes_full_);
    lv_obj_remove_style_all(eye_f_r_);
    lv_obj_clear_flag(eye_f_r_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(eye_f_r_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(eye_f_r_, LV_OPA_COVER, 0);

    ApplyEyeShape();
}

void OledDisplay::ApplyEyeShape() {
    // 小眼睛（128x64 的左栏 / 128x32 说话时的左 32px）
    if (eye_left_ != nullptr) {
        ShapeEye(eye_left_, eye_w_, eye_h_, 0, 0, eye_scale_l_, blink_pct_);
    }
    if (eye_right_ != nullptr) {
        ShapeEye(eye_right_, eye_w_, eye_h_, eye_w_ + eye_gap_, 0, eye_scale_r_, blink_pct_);
    }

    // 全屏大眼睛
    if (eye_f_l_ != nullptr && eye_f_r_ != nullptr) {
        int area_w = eye_f_w_ * 2 + eye_f_gap_;
        int ox = (LV_HOR_RES - area_w) / 2;
        int oy = (LV_VER_RES - eye_f_h_) / 2;
        ShapeEye(eye_f_l_, eye_f_w_, eye_f_h_, ox, oy, eye_scale_l_, blink_pct_);
        ShapeEye(eye_f_r_, eye_f_w_, eye_f_h_, ox + eye_f_w_ + eye_f_gap_, oy, eye_scale_r_,
                 blink_pct_);
    }
}

void OledDisplay::UpdateLayoutMode() {
    if (eyes_full_ == nullptr || container_ == nullptr) {
        return;  // 128x64 不走这套
    }
    if (has_chat_text_) {
        // 有字幕：显示状态栏 + 文字，收起全屏眼睛
        lv_obj_add_flag(eyes_full_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(container_, LV_OBJ_FLAG_HIDDEN);
    } else {
        // 待机：整屏都是眼睛
        lv_obj_remove_flag(eyes_full_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
    }
}

void OledDisplay::SetEyeScale(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    blink_pct_ = percent;
    ApplyEyeShape();
}

void OledDisplay::StartBlinkAnim() {
    if (eye_left_ == nullptr && eye_f_l_ == nullptr) {
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, this);
    lv_anim_set_exec_cb(&a, EyeBlinkAnimCb);
    lv_anim_set_values(&a, 100, 0);
    lv_anim_set_duration(&a, 85);
    lv_anim_set_playback_duration(&a, 85);
    lv_anim_start(&a);
}
