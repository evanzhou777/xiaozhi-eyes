#ifndef OLED_DISPLAY_H
#define OLED_DISPLAY_H

#include "lvgl_display.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

class OledDisplay : public LvglDisplay {
private:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* content_left_ = nullptr;
    lv_obj_t* content_right_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* emotion_label_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;

    // ---- 大眼睛表情 ----
    lv_obj_t* eyes_area_ = nullptr;
    lv_obj_t* eye_left_ = nullptr;
    lv_obj_t* eye_right_ = nullptr;
    lv_timer_t* blink_timer_ = nullptr;
    int eye_w_ = 26;
    int eye_h_ = 34;
    int eye_gap_ = 22;
    int eye_scale_l_ = 100;
    int eye_scale_r_ = 100;
    int blink_pct_ = 100;

    // ---- 全屏大眼睛（仅 128x32 待机时） ----
    lv_obj_t* eyes_full_ = nullptr;
    lv_obj_t* eye_f_l_ = nullptr;
    lv_obj_t* eye_f_r_ = nullptr;
    int eye_f_w_ = 40;
    int eye_f_h_ = 26;
    int eye_f_gap_ = 30;
    bool has_chat_text_ = false;

    void SetupEyes(lv_obj_t* parent);
    void SetupFullScreenEyes(lv_obj_t* parent);
    void ApplyEyeShape();
    void UpdateLayoutMode();

    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

    void SetupUI_128x64();
    void SetupUI_128x32();

public:
    OledDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                int height, bool mirror_x, bool mirror_y);
    ~OledDisplay();

    virtual void SetupUI() override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void RemoveChatMessage(const char* role, const char* content) override;
    virtual bool SetEmotion(const char* emotion) override;
    virtual void SetTheme(Theme* theme) override;
    virtual bool IsMonochrome() const override { return true; }
    void SetPowerSaveMode(bool on) override;

    // 大眼睛动画接口（眨眼用）
    void SetEyeScale(int percent);
    void StartBlinkAnim();
};

#endif  // OLED_DISPLAY_H
