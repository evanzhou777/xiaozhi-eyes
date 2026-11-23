#ifndef __MOSQUITO_DETECTOR_H__
#define __MOSQUITO_DETECTOR_H__

// ============================================================================
//  蚊子雷达（Mosquito Radar）
//
//  原理：
//    蚊子在飞行时翅膀振动频率约 350~800Hz，是一个窄带、频率稳定的单音。
//    人声、风扇、空调噪声同频段内是宽带且频率跳变的。所以判据是：
//      ① 带内能量明显高于自适应噪声底
//      ② 峰值能量占带内总能量比例够高（窄带性，排除人声/噪声）
//      ③ 峰值频率连续多帧稳定（排除瞬态咔哒声）
//
//  硬件：模拟麦克风模块（MAX4466 / MAX9814）AO -> ADC 引脚，3.3V 供电
//        只有一路，不做方位；只回答“附近有没有蚊子”
//
//  隔离性：
//    走独立 ADC 通道 + 独立低优先级任务，不碰 I2S 音频链路、不碰显示层。
//    即使本模块初始化失败或任务异常，也只会退化为“没有这个工具”，
//    不影响小智原有的语音、唤醒、配网、表情。
// ============================================================================

#include "mcp_server.h"
#include "application.h"
#include "mosquito_alert.h"

#include <esp_adc/adc_continuous.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#define MOSQ_TAG "MosquitoRadar"

// ---- 可调参数 --------------------------------------------------------------
#define MOSQ_SAMPLE_RATE 16000     // 采样率 Hz
#define MOSQ_FRAME 1024            // 每帧采样点数（64ms，频率分辨率 15.6Hz）
#define MOSQ_FMIN 380.0f           // 检测频段下界 Hz
#define MOSQ_FMAX 820.0f           // 检测频段上界 Hz
#define MOSQ_MIN_LEVEL_DB 9.0f     // 需高于噪声底多少 dB
#define MOSQ_MIN_TONE_RATIO 0.15f  // 峰值/带内总能量，低于此判为宽带噪声
#define MOSQ_STABLE_FRAMES 4       // 连续稳定帧数（x64ms）才确认
#define MOSQ_MAX_DRIFT 30.0f       // 帧间允许的频率漂移 Hz
#define MOSQ_WINDOW_MS 15000       // “最近听到过”的统计窗口
#define MOSQ_CALIB_MS 2000         // 校准时长

// 守望模式：ADC 本来就一直在采样，这里只控制「检出后要不要主动提醒」
#define MOSQ_ALERT_COOLDOWN_MS 60000  // 两次提醒最小间隔，防止连环误报
#define MOSQ_ALERT_SHOW_MS 10000      // 屏幕提示保留时长
#define MOSQ_ALERT_RETRY_MS 2000      // 设备忙（听/说）时的重试间隔
#define MOSQ_WATCH_DEFAULT_MIN 30     // 默认守望时长，0 = 一直盯到显式关闭

class MosquitoDetector {
public:
    explicit MosquitoDetector(gpio_num_t adc_gpio) : adc_gpio_(adc_gpio) {
        if (adc_gpio_ == GPIO_NUM_NC) {
            ESP_LOGW(MOSQ_TAG, "未配置麦克风引脚，蚊子雷达不启用");
            return;
        }
        if (!InitAdc()) {
            return;
        }
        RegisterTools();
        if (xTaskCreate(&MosquitoDetector::TaskEntry, "mosq_radar", 6144, this, 3, &task_) != pdPASS) {
            ESP_LOGE(MOSQ_TAG, "创建分析任务失败");
            task_ = nullptr;
        }
    }

private:
    gpio_num_t adc_gpio_;
    adc_continuous_handle_t handle_ = nullptr;
    TaskHandle_t task_ = nullptr;

    // ESP32-S3 每个 ADC 采样结果是 adc_digi_output_data_t（4 字节），不是 2 字节
    static constexpr int kResultBytes = (int)sizeof(adc_digi_output_data_t);
    uint8_t raw_buf_[MOSQ_FRAME * (int)sizeof(adc_digi_output_data_t)] = {0};

    float frame_[MOSQ_FRAME] = {0};
    float floor_db_ = -200.0f;  // 自适应噪声底（dB 域）
    float track_f_ = 0.0f;    // 跟踪中的峰值频率
    int streak_ = 0;          // 连续符合判据的帧数

    std::mutex mutex_;
    float last_level_ = 0.0f;
    float last_ratio_ = 0.0f;
    float last_freq_ = 0.0f;
    int64_t last_detect_us_ = 0;
    float last_detect_freq_ = 0.0f;
    float last_detect_level_ = 0.0f;
    float last_detect_conf_ = 0.0f;

    std::atomic<bool> calibrating_{false};
    std::atomic<int64_t> calib_until_us_{0};

    // 守望状态（ADC 任务与 MCP 主任务共享，全部用 atomic）
    std::atomic<bool> watch_enabled_{false};
    std::atomic<int64_t> watch_until_us_{0};  // 0 = 不限时
    std::atomic<int64_t> last_alert_us_{0};
    std::atomic<int64_t> alert_clear_us_{0};
    std::atomic<int64_t> last_retry_us_{0};
    std::atomic<bool> alert_pending_{false};
    std::atomic<float> alert_freq_{0.0f};

    // ---- ADC 初始化 --------------------------------------------------------
    bool InitAdc() {
        adc_unit_t unit = ADC_UNIT_1;
        adc_channel_t channel = ADC_CHANNEL_0;

        esp_err_t err = adc_continuous_io_to_channel(adc_gpio_, &unit, &channel);
        if (err != ESP_OK) {
            ESP_LOGE(MOSQ_TAG, "GPIO%d 不是 ADC 引脚，蚊子雷达不启用", (int)adc_gpio_);
            return false;
        }
        if (unit != ADC_UNIT_1) {
            // ADC2 与 WiFi 冲突，必须用 ADC1
            ESP_LOGE(MOSQ_TAG, "GPIO%d 属于 ADC2，WiFi 开启时不可用，请换到 ADC1 引脚", (int)adc_gpio_);
            return false;
        }

        adc_continuous_handle_cfg_t hcfg;
        memset(&hcfg, 0, sizeof(hcfg));
        hcfg.max_store_buf_size = MOSQ_FRAME * kResultBytes * 4;
        hcfg.conv_frame_size = MOSQ_FRAME * kResultBytes;
        hcfg.flags.flush_pool = 1;

        err = adc_continuous_new_handle(&hcfg, &handle_);
        if (err != ESP_OK) {
            ESP_LOGE(MOSQ_TAG, "adc_continuous_new_handle 失败: %s", esp_err_to_name(err));
            return false;
        }

        adc_digi_pattern_config_t pattern;
        memset(&pattern, 0, sizeof(pattern));
        pattern.atten = ADC_ATTEN_DB_12;
        pattern.channel = (uint8_t)channel;
        pattern.unit = (uint8_t)unit;
        pattern.bit_width = ADC_BITWIDTH_12;  // 连续模式要求显式位宽，DEFAULT 会被拒

        adc_continuous_config_t ccfg;
        memset(&ccfg, 0, sizeof(ccfg));
        ccfg.pattern_num = 1;
        ccfg.adc_pattern = &pattern;
        ccfg.sample_freq_hz = MOSQ_SAMPLE_RATE;
        ccfg.conv_mode = ADC_CONV_SINGLE_UNIT_1;
        ccfg.format = ADC_DIGI_OUTPUT_FORMAT_TYPE2;

        err = adc_continuous_config(handle_, &ccfg);
        if (err != ESP_OK) {
            ESP_LOGE(MOSQ_TAG, "adc_continuous_config 失败: %s", esp_err_to_name(err));
            adc_continuous_deinit(handle_);
            handle_ = nullptr;
            return false;
        }

        err = adc_continuous_start(handle_);
        if (err != ESP_OK) {
            ESP_LOGE(MOSQ_TAG, "adc_continuous_start 失败: %s", esp_err_to_name(err));
            adc_continuous_deinit(handle_);
            handle_ = nullptr;
            return false;
        }

        ESP_LOGI(MOSQ_TAG, "麦克风已接 GPIO%d (ADC1_CH%d)，采样 %d Hz",
                 (int)adc_gpio_, (int)channel, MOSQ_SAMPLE_RATE);

        // 开机先静默学习 1.5 秒环境噪声，避免把电源启动噪声当成基线
        calib_until_us_.store(esp_timer_get_time() + 1500 * 1000);
        calibrating_.store(true);
        return true;
    }

    // ---- Goertzel：单个频点的归一化功率 -----------------------------------
    static float GoertzelPower(const float* x, int n, float freq) {
        const float w = 2.0f * 3.14159265358979f * freq / (float)MOSQ_SAMPLE_RATE;
        const float coeff = 2.0f * cosf(w);
        float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f;
        for (int i = 0; i < n; i++) {
            s0 = x[i] + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        const float re = s1 - s2 * cosf(w);
        const float im = s2 * sinf(w);
        const float mag2 = re * re + im * im;
        // 归一化：单音幅度 A 时 |X|=A*n/2，故 |X|^2/n^2 = A^2/4，与幅度可比的功率量
        return mag2 / ((float)n * (float)n);
    }

    // ---- 单帧分析 ----------------------------------------------------------
    void Analyze(const uint8_t* buf, uint32_t bytes) {
        const int n = (int)(bytes / kResultBytes);
        if (n != MOSQ_FRAME) {
            return;  // 只处理完整帧，保证频率轴对齐
        }
        const adc_digi_output_data_t* rec = (const adc_digi_output_data_t*)buf;

        // 去直流并归一化到 ±1（12bit -> ±2048）
        float mean = 0.0f;
        for (int i = 0; i < n; i++) {
            mean += (float)(rec[i].val & 0x0FFF);
        }
        mean /= (float)n;

        for (int i = 0; i < n; i++) {
            frame_[i] = ((float)(rec[i].val & 0x0FFF) - mean) * (1.0f / 2048.0f);
        }

        // 带内扫频，记录峰值与总能量
        const float step = (float)MOSQ_SAMPLE_RATE / (float)MOSQ_FRAME;
        float peak_pow = 0.0f, peak_f = 0.0f, band_pow = 0.0f;
        for (float f = MOSQ_FMIN; f <= MOSQ_FMAX + 0.01f; f += step) {
            float p = GoertzelPower(frame_, n, f);
            band_pow += p;
            if (p > peak_pow) {
                peak_pow = p;
                peak_f = f;
            }
        }

        const int64_t now_us = esp_timer_get_time();

        // 输入健康检查：模拟麦克风输出被偏置在 VDD/2（3.3V 供电约 2048）。
        // 引脚悬空或没接模块时直流电平会跑到 0 或 4095 附近，此时不做检测，
        // 避免把耦合噪声当成蚊子。
        const bool input_ok = (mean > 250.0f && mean < 3850.0f);
        if (!input_ok) {
            streak_ = 0;
            track_f_ = 0.0f;
            floor_db_ = -200.0f;  // 等输入恢复后重新学噪声底
            static int64_t last_warn_us = 0;
            if (now_us - last_warn_us > 10000000) {
                last_warn_us = now_us;
                ESP_LOGW(MOSQ_TAG, "麦克风未接或输出异常（直流 %.0f/4095），跳过检测", mean);
            }
            return;
        }

        const float db = 10.0f * log10f(band_pow + 1e-15f);

        // 校准模式：快速学习噪声底，不做检测
        if (calibrating_.load()) {
            floor_db_ = (floor_db_ < -100.0f) ? db : floor_db_ * 0.7f + db * 0.3f;
            if (now_us >= calib_until_us_.load()) {
                calibrating_.store(false);
                ESP_LOGI(MOSQ_TAG, "噪声底校准完成：%.1f dB", floor_db_);
            }
            return;
        }

        // 首帧先建立噪声底
        if (floor_db_ < -100.0f) {
            floor_db_ = db;
            return;
        }

        const float level_db = db - floor_db_;
        const float tone_ratio = (band_pow > 0.0f) ? peak_pow / band_pow : 0.0f;

        const bool loud = level_db >= MOSQ_MIN_LEVEL_DB;
        const bool tonal = tone_ratio >= MOSQ_MIN_TONE_RATIO;

        if (loud && tonal) {
            if (track_f_ <= 0.0f || fabsf(peak_f - track_f_) <= MOSQ_MAX_DRIFT) {
                track_f_ = (track_f_ <= 0.0f) ? peak_f : track_f_ * 0.7f + peak_f * 0.3f;
                if (streak_ < 1000) {
                    streak_++;
                }
            } else {
                // 频率跳变，重新起算
                track_f_ = peak_f;
                streak_ = 1;
            }
        } else {
            streak_ = 0;
            track_f_ = 0.0f;
            // 噪声底在 dB 域跟踪：变安静时跟得快，变吵时跟得慢，
            // 避免一声蚊子把底噪抬上去之后自己反而听不见了
            if (db < floor_db_) {
                floor_db_ = floor_db_ * 0.90f + db * 0.10f;
            } else {
                floor_db_ = floor_db_ * 0.999f + db * 0.001f;
            }
        }

        const bool detected = (streak_ >= MOSQ_STABLE_FRAMES);

        float conf = 0.0f;
        if (detected) {
            float lv = (level_db - MOSQ_MIN_LEVEL_DB) / 15.0f;
            if (lv < 0.0f) lv = 0.0f;
            if (lv > 1.0f) lv = 1.0f;
            float tr = tone_ratio / 0.5f;
            if (tr > 1.0f) tr = 1.0f;
            conf = 0.5f * lv + 0.5f * tr;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_level_ = level_db;
            last_ratio_ = tone_ratio;
            last_freq_ = peak_f;
            if (detected) {
                last_detect_us_ = now_us;
                last_detect_freq_ = track_f_;
                last_detect_level_ = level_db;
                last_detect_conf_ = conf;
            }
        }

        // ---- 守望模式：检出后主动提醒 --------------------------------------
        // 注意：本函数跑在独立的 ADC 任务里，所有 UI / 音频操作必须经
        // Application::Schedule 排到主任务，且只在 Idle 时执行，避免抢 TTS 通道。
        if (watch_enabled_.load()) {
            const int64_t until = watch_until_us_.load();
            if (until > 0 && now_us >= until) {
                watch_enabled_.store(false);
                alert_pending_.store(false);
                ESP_LOGI(MOSQ_TAG, "守望结束：已到设定时长");
            }
        }

        if (watch_enabled_.load() && detected &&
            now_us - last_alert_us_.load() >= (int64_t)MOSQ_ALERT_COOLDOWN_MS * 1000) {
            alert_freq_.store(track_f_);
            alert_pending_.store(true);
            ESP_LOGI(MOSQ_TAG, "守望命中：%.0fHz，安排提醒", track_f_);
        }

        if (alert_pending_.load() &&
            now_us - last_retry_us_.load() >= (int64_t)MOSQ_ALERT_RETRY_MS * 1000) {
            last_retry_us_.store(now_us);
            alert_pending_.store(false);
            last_alert_us_.store(now_us);
            const float freq = alert_freq_.load();
            Application::GetInstance().Schedule([this, freq]() {
                auto& app = Application::GetInstance();
                if (app.GetDeviceState() != kDeviceStateIdle) {
                    alert_pending_.store(true);  // 正在听/说，稍后重试
                    return;
                }
                char msg[96];
                snprintf(msg, sizeof(msg), "发现蚊子 %.0fHz", freq);
                app.Alert("蚊子警报", msg, "surprised", MOSQUITO_ALERT_SOUND);
                alert_clear_us_.store(esp_timer_get_time() +
                                      (int64_t)MOSQ_ALERT_SHOW_MS * 1000);
                ESP_LOGI(MOSQ_TAG, "已主动提醒：%s", msg);
            });
        }

        if (alert_clear_us_.load() > 0 && now_us >= alert_clear_us_.load()) {
            alert_clear_us_.store(0);
            Application::GetInstance().Schedule([]() {
                Application::GetInstance().DismissAlert();
            });
        }

        // 每 3 秒打一行状态，方便串口调试（直流 ~2048 说明麦克风供电正常）
        static int64_t last_log_us = 0;
        if (now_us - last_log_us > 3000000) {
            last_log_us = now_us;
            ESP_LOGI(MOSQ_TAG, "直流 %.0f，高于底噪 %.1fdB，峰值 %.0fHz，窄带度 %.2f%s",
                     mean, level_db, peak_f, tone_ratio, detected ? "  <<< 检出" : "");
        }
    }

    // ---- 任务 --------------------------------------------------------------
    static void TaskEntry(void* arg) {
        static_cast<MosquitoDetector*>(arg)->Run();
    }

    void Run() {
        while (true) {
            uint32_t got = 0;
            esp_err_t err = adc_continuous_read(handle_, raw_buf_, sizeof(raw_buf_), &got, 500);
            if (err != ESP_OK || got == 0) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            Analyze(raw_buf_, got);
        }
    }

    // ---- MCP 工具 ----------------------------------------------------------
    void RegisterTools() {
        auto& mcp = McpServer::GetInstance();

        mcp.AddTool(
            "self.mosquito.scan",
            "Report whether mosquito wingbeat sound has been detected in this room in the last 15 seconds. "
            "Returns detected flag, wingbeat frequency in Hz and confidence. "
            "Use this when the user asks whether there are mosquitoes nearby.",
            PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                std::lock_guard<std::mutex> lock(mutex_);
                const int64_t now_us = esp_timer_get_time();
                const int64_t delta_ms =
                    (last_detect_us_ > 0) ? (now_us - last_detect_us_) / 1000 : -1;
                const bool recent = (delta_ms >= 0 && delta_ms <= MOSQ_WINDOW_MS);

                char json[224];
                if (!recent) {
                    snprintf(json, sizeof(json),
                             "{\"detected\": false, \"ambient_level_db\": %.1f, "
                             "\"peak_frequency_hz\": %.0f}",
                             last_level_, last_freq_);
                } else {
                    snprintf(json, sizeof(json),
                             "{\"detected\": true, \"frequency_hz\": %.0f, \"level_db\": %.1f, "
                             "\"confidence\": %.2f, \"seconds_ago\": %.1f}",
                             last_detect_freq_, last_detect_level_, last_detect_conf_,
                             (float)delta_ms / 1000.0f);
                }
                return std::string(json);
            });

        mcp.AddTool(
            "self.mosquito.calibrate",
            "Recalibrate the background noise floor of the mosquito detector. "
            "Call this after the device has been moved, or when the room noise changed and the "
            "detector keeps reporting false positives.",
            PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                calib_until_us_.store(esp_timer_get_time() + (int64_t)MOSQ_CALIB_MS * 1000);
                calibrating_.store(true);
                return true;
            });

        // 守望开关：ADC 一直采样，这里决定「检出后是否主动提醒用户」
        mcp.AddTool(
            "self.mosquito.watch",
            "Start or stop proactive mosquito watching. The microphone is always being "
            "analysed; while watching is on, the device itself alerts the user with a screen "
            "message and an alert sound as soon as a mosquito wingbeat is confirmed, without "
            "the user needing to ask again. "
            "Call enabled=true when the user says 帮我盯着蚊子 / 有蚊子提醒我 / 持续监测蚊子 / "
            "睡觉时帮我看着蚊子 / 别让蚊子咬我 / keep watch for mosquitoes and alert me. "
            "Call enabled=false when the user says 别盯了 / 停止监测 / 不用提醒了 / 取消蚊子警报. "
            "minutes=0 means keep watching until explicitly stopped; default is 30 minutes.",
            PropertyList(std::vector<Property>{
                Property("enabled", kPropertyTypeBoolean, true),
                Property("minutes", kPropertyTypeInteger, MOSQ_WATCH_DEFAULT_MIN, 0, 1440),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                const bool enabled = properties["enabled"].value<bool>();
                const int minutes = properties["minutes"].value<int>();
                const int64_t now = esp_timer_get_time();
                char json[192];
                if (!enabled) {
                    watch_enabled_.store(false);
                    watch_until_us_.store(0);
                    alert_pending_.store(false);
                    alert_clear_us_.store(0);
                    ESP_LOGI(MOSQ_TAG, "守望已关闭");
                    snprintf(json, sizeof(json),
                             "{\"watching\": false, \"message\": \"已停止蚊子监测\"}");
                } else {
                    watch_until_us_.store(minutes == 0 ? 0 : now + (int64_t)minutes * 60 * 1000000);
                    watch_enabled_.store(true);
                    // 重新学 1.2 秒环境底噪，避免搬动设备后旧基线造成误报
                    calib_until_us_.store(now + 1200000);
                    calibrating_.store(true);
                    ESP_LOGI(MOSQ_TAG, "守望已开启：%d 分钟（0=不限时）", minutes);
                    snprintf(json, sizeof(json),
                             "{\"watching\": true, \"minutes\": %d, "
                             "\"message\": \"已开始持续监测，发现蚊子会主动提醒\"}",
                             minutes);
                }
                return std::string(json);
            });

        ESP_LOGI(MOSQ_TAG, "工具已注册：scan / calibrate / watch");
    }
};

#endif  // __MOSQUITO_DETECTOR_H__
