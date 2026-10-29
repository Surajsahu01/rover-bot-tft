// rover_tft.h - 2.4" SPI TFT (ILI9341, 240x320, used in landscape 320x240).
//
// Replaces the old 128x64 OLED. Two things are drawn on it, both straight to the panel
// (no LVGL, Xiaozhi's own UI still does not draw here):
//   * the animated eyes / info pages from rover_eyes.h (rendered in a 128x64 mono buffer and
//     scaled up 2.5x to 320x160, centred vertically)
//   * the LIVE CAMERA VIDEO, full screen 320x240 (see RoverCamera + RoverTft::ShowCamera)
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include <driver/gpio.h>
#include <esp_attr.h>
#include <driver/spi_master.h>
#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <esp_lcd_ili9341.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "config.h"
#include "esp32_camera.h"

// ============================================================================
//  RoverCamera: the normal Xiaozhi camera (photo for "what is this?") that can ALSO hand
//  out frames for the live preview. A mutex makes sure the preview never fights with a
//  photo capture. While previewing, the sensor runs at QVGA (fast); a photo for the AI
//  switches to VGA first (sharper) and the preview switches back afterwards.
// ============================================================================
class RoverCamera : public Esp32Camera {
public:
    explicit RoverCamera(const camera_config_t& config) : Esp32Camera(config) {}

    // Photo for the AI (called by Xiaozhi's "take photo" tool).
    bool Capture() override {
        std::lock_guard<std::mutex> g(mu_);
        SetSize(FRAMESIZE_VGA);
        return Esp32Camera::Capture();
    }

    // Live preview: returns nullptr if the camera is not available. Every successful call
    // MUST be followed by ReleaseFrame() from the same task.
    camera_fb_t* AcquireFrame() {
        mu_.lock();
        if (!esp_camera_sensor_get()) {  // camera init failed
            mu_.unlock();
            return nullptr;
        }
        SetSize(ROVER_CAM_PREVIEW_FRAMESIZE);
        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) mu_.unlock();
        return fb;
    }

    void ReleaseFrame(camera_fb_t* fb) {
        esp_camera_fb_return(fb);
        mu_.unlock();
    }

private:
    std::mutex mu_;
    framesize_t size_ = FRAMESIZE_VGA;  // what the sensor was initialised with

    void SetSize(framesize_t want) {
        if (size_ == want) return;
        sensor_t* s = esp_camera_sensor_get();
        if (!s) return;
        if (s->set_framesize(s, want) != 0) return;
        size_ = want;
        for (int i = 0; i < 2; i++) {  // first frames after a size change are not clean
            camera_fb_t* fb = esp_camera_fb_get();
            if (!fb) break;
            esp_camera_fb_return(fb);
        }
    }
};

// ============================================================================
//  RoverTft: panel driver + eyes output + camera video
// ============================================================================
class RoverTft {
public:
    static constexpr int kW = 320;      // landscape
    static constexpr int kH = 240;
    static constexpr int kStrip = 20;   // rows per DMA transfer (320*20*2 = 12.8 kB)
    static constexpr int kMonoW = 128;  // logical size of the eyes renderer
    static constexpr int kMonoH = 64;
    static constexpr int kMonoDstH = 160;                 // 64 * 2.5
    static constexpr int kMonoDstY = (kH - kMonoDstH) / 2;  // 40

    // 0xRRGGBB -> RGB565 with the two bytes swapped (the order the SPI bus wants)
    static constexpr uint16_t Color(uint32_t rgb) {
        uint16_t c = (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x1F));
        return (uint16_t)((c >> 8) | (c << 8));
    }

    RoverTft() {
        InitSpiAndPanel();
        for (int x = 0; x < kW; x++) mono_xmap_[x] = (uint16_t)(x * kMonoW / kW);
        Clear();
        xTaskCreate(&RoverTft::VideoThunk, "tft_video", 4096, this, 2, nullptr);
    }

    void AttachCamera(RoverCamera* cam) { camera_ = cam; }

    // ------------------------------------------------------------------ camera view API
    // ms = 0: until HideCamera() is called.
    void ShowCamera(int ms) {
        until_ms_ = ms > 0 ? NowMs() + ms : 0;
        camera_on_ = true;
    }
    void HideCamera() {
        if (camera_on_.exchange(false)) repaint_ = true;
    }
    bool CameraActive() const { return camera_on_; }

    // The eyes renderer calls this each frame: true once after the camera view ended (or at
    // start-up) - it must then redraw everything.
    bool TakeRepaint() { return repaint_.exchange(false); }

    // ------------------------------------------------------------------ drawing
    void Clear(uint16_t bg = Color(ROVER_TFT_BG_COLOR)) {
        std::lock_guard<std::mutex> g(draw_mu_);
        DrawRows(0, kH, [bg](int, uint16_t* dst) {
            for (int x = 0; x < kW; x++) dst[x] = bg;
        });
    }

    // fb: 128x64, vertical tiled 1 bpp (byte = 8 rows, LSB = top row), as in rover_eyes.h
    void DrawMono(const uint8_t* fb) {
        const uint16_t fg = Color(ROVER_TFT_EYE_COLOR), bg = Color(ROVER_TFT_BG_COLOR);
        std::lock_guard<std::mutex> g(draw_mu_);
        DrawRows(kMonoDstY, kMonoDstY + kMonoDstH, [&](int y, uint16_t* dst) {
            int sy = (y - kMonoDstY) * kMonoH / kMonoDstH;
            const uint8_t* row = fb + (sy >> 3) * kMonoW;
            int bit = sy & 7;
            for (int x = 0; x < kW; x++) dst[x] = ((row[mono_xmap_[x]] >> bit) & 1) ? fg : bg;
        });
    }

private:
    esp_lcd_panel_io_handle_t io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    uint16_t* buf_[2] = {nullptr, nullptr};  // internal DMA memory
    int buf_idx_ = 0;
    SemaphoreHandle_t buf_sem_ = nullptr;     // counts free DMA buffers
    std::mutex draw_mu_;                      // one picture at a time on the panel
    uint16_t mono_xmap_[kW];
    std::vector<uint16_t> cam_xmap_;
    int cam_xmap_w_ = 0;

    RoverCamera* camera_ = nullptr;
    std::atomic<bool> camera_on_{false};
    std::atomic<bool> repaint_{true};
    int64_t until_ms_ = 0;

    static int64_t NowMs() { return esp_timer_get_time() / 1000; }

    static bool IRAM_ATTR OnColorDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*,
                                      void* ctx) {
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(static_cast<RoverTft*>(ctx)->buf_sem_, &woken);
        return woken == pdTRUE;
    }

    void InitSpiAndPanel() {
        for (int i = 0; i < 2; i++) {
            buf_[i] = (uint16_t*)heap_caps_malloc(kW * kStrip * sizeof(uint16_t),
                                                  MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
            ESP_ERROR_CHECK(buf_[i] ? ESP_OK : ESP_ERR_NO_MEM);
        }
        buf_sem_ = xSemaphoreCreateCounting(2, 2);

        if (DISPLAY_BL_PIN != GPIO_NUM_NC) {  // optional backlight pin (else LED wired to 3V3)
            gpio_config_t bl = {};
            bl.pin_bit_mask = 1ULL << (int)(DISPLAY_BL_PIN < 0 ? 0 : DISPLAY_BL_PIN);
            bl.mode = GPIO_MODE_OUTPUT;
            gpio_config(&bl);
            gpio_set_level(DISPLAY_BL_PIN, 1);
        }

        spi_bus_config_t bus = {};
        bus.mosi_io_num = DISPLAY_MOSI_PIN;
        bus.miso_io_num = GPIO_NUM_NC;
        bus.sclk_io_num = DISPLAY_SCLK_PIN;
        bus.quadwp_io_num = GPIO_NUM_NC;
        bus.quadhd_io_num = GPIO_NUM_NC;
        bus.max_transfer_sz = kW * kStrip * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

        esp_lcd_panel_io_spi_config_t io = {};
        io.cs_gpio_num = DISPLAY_CS_PIN;
        io.dc_gpio_num = DISPLAY_DC_PIN;
        io.spi_mode = 0;
        io.pclk_hz = DISPLAY_SPI_HZ;
        io.trans_queue_depth = 4;
        io.lcd_cmd_bits = 8;
        io.lcd_param_bits = 8;
        io.on_color_trans_done = &RoverTft::OnColorDone;
        io.user_ctx = this;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(DISPLAY_SPI_HOST, &io, &io_));

        esp_lcd_panel_dev_config_t pc = {};
        pc.reset_gpio_num = DISPLAY_RST_PIN;  // NC = software reset
        pc.rgb_ele_order = DISPLAY_RGB_ORDER;
        pc.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io_, &pc, &panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, DISPLAY_INVERT_COLOR));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_, true));  // landscape
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));
    }

    // Draw rows [y0, y1) in strips. fill(row, dst) writes one 320-pixel row. draw_mu_ held.
    template <class F>
    void DrawRows(int y0, int y1, F fill) {
        for (int y = y0; y < y1; y += kStrip) {
            int rows = std::min(kStrip, y1 - y);
            xSemaphoreTake(buf_sem_, portMAX_DELAY);  // wait for a free buffer
            uint16_t* b = buf_[buf_idx_];
            buf_idx_ ^= 1;
            for (int r = 0; r < rows; r++) fill(y + r, b + r * kW);
            if (esp_lcd_panel_draw_bitmap(panel_, 0, y, kW, y + rows, b) != ESP_OK) {
                xSemaphoreGive(buf_sem_);  // no completion callback will come
            }
        }
    }

    // ------------------------------------------------------------------ camera video
    void DrawCamera(const camera_fb_t* fb) {
        if (fb->format != PIXFORMAT_RGB565 || fb->width == 0 || fb->height == 0) return;
        const int w = fb->width, h = fb->height;
        if (cam_xmap_w_ != w) {
            cam_xmap_.resize(kW);
            for (int x = 0; x < kW; x++) cam_xmap_[x] = (uint16_t)(x * w / kW);
            cam_xmap_w_ = w;
        }
        // The sensor delivers RGB565 high byte first - exactly the byte order the SPI bus
        // wants - so pixels are copied unchanged (no swapping needed).
        const uint16_t* src = reinterpret_cast<const uint16_t*>(fb->buf);
        std::lock_guard<std::mutex> g(draw_mu_);
        DrawRows(0, kH, [&](int y, uint16_t* dst) {
            const uint16_t* row = src + (size_t)(y * h / kH) * w;
            if (w == kW) {
                memcpy(dst, row, kW * sizeof(uint16_t));
            } else {
                for (int x = 0; x < kW; x++) dst[x] = row[cam_xmap_[x]];
            }
        });
    }

    static void VideoThunk(void* arg) { static_cast<RoverTft*>(arg)->VideoLoop(); }

    void VideoLoop() {
        while (true) {
            if (!camera_on_ || !camera_) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            if (until_ms_ && NowMs() > until_ms_) {
                HideCamera();
                continue;
            }
            camera_fb_t* fb = camera_->AcquireFrame();
            if (!fb) {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            DrawCamera(fb);
            camera_->ReleaseFrame(fb);
            vTaskDelay(1);  // let the idle task / watchdog run
        }
    }
};
