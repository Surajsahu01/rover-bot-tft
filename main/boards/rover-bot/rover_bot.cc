// Rover-Bot board (desk buddy): ESP32-S3 + OV2640 + 2.4" SPI TFT (ILI9341) + INMP441 + MAX98357A
//                               + L298N / 2 x N20 motors + 3 servos (head, left arm, right arm)
#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "esp32_camera.h"
#include "rover_tft.h"

#include <esp_log.h>

#include "rover_robot.h"
#include "rover_eyes.h"
#include "rover_tools.h"
#include "rover_web.h"

#define TAG "RoverBot"

// Xiaozhi gets a display object that draws nothing: the TFT belongs to the eyes / camera view.
// It only forwards the AI's emotion so the eyes and the body can follow it.
class EyesDisplay : public NoDisplay {
public:
    void Attach(RoverEyes* eyes, RoverRobot* robot) { eyes_ = eyes; robot_ = robot; }

    void SetEmotion(const char* emotion) override {
        if (eyes_) eyes_->SetEmotionFromAI(emotion);
        if (robot_) robot_->OnEmotion(emotion);
    }

private:
    RoverEyes* eyes_ = nullptr;
    RoverRobot* robot_ = nullptr;
};

class RoverBot : public WifiBoard {
private:
    Button boot_button_;
    Display* display_ = nullptr;
    EyesDisplay* eyes_display_ = nullptr;  // same object as display_ (no RTTI on ESP-IDF)
    RoverEyes* eyes_ = nullptr;
    RoverTft* tft_ = nullptr;
    RoverCamera* camera_ = nullptr;
    RoverRobot* robot_ = nullptr;
    RoverTools* tools_ = nullptr;
    RoverWeb* web_ = nullptr;

    void InitializeDisplay() {
        tft_ = new RoverTft();
        eyes_ = new RoverEyes(tft_);
        eyes_display_ = new EyesDisplay();
        display_ = eyes_display_;
    }

    void InitializeCamera() {
        camera_config_t config = {};
        config.pin_d0 = CAMERA_PIN_D0;
        config.pin_d1 = CAMERA_PIN_D1;
        config.pin_d2 = CAMERA_PIN_D2;
        config.pin_d3 = CAMERA_PIN_D3;
        config.pin_d4 = CAMERA_PIN_D4;
        config.pin_d5 = CAMERA_PIN_D5;
        config.pin_d6 = CAMERA_PIN_D6;
        config.pin_d7 = CAMERA_PIN_D7;
        config.pin_xclk = CAMERA_PIN_XCLK;
        config.pin_pclk = CAMERA_PIN_PCLK;
        config.pin_vsync = CAMERA_PIN_VSYNC;
        config.pin_href = CAMERA_PIN_HREF;
        config.pin_sccb_sda = CAMERA_PIN_SIOD;
        config.pin_sccb_scl = CAMERA_PIN_SIOC;
        config.sccb_i2c_port = 0;
        config.pin_pwdn = CAMERA_PIN_PWDN;
        config.pin_reset = CAMERA_PIN_RESET;
        config.xclk_freq_hz = XCLK_FREQ_HZ;
        config.pixel_format = PIXFORMAT_RGB565;
        config.frame_size = FRAMESIZE_VGA;
        config.jpeg_quality = 12;
        // 2 frame buffers: Xiaozhi keeps one for the photo it sends to the AI, the live
        // video on the TFT uses the other. Always the newest frame -> low latency.
        config.fb_count = 2;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_LATEST;
        camera_ = new RoverCamera(config);
        if (tft_) tft_->AttachCamera(camera_);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
        // Long press on BOOT = emergency stop (wheels off, arms to rest).
        boot_button_.OnLongPress([this]() {
            if (robot_) robot_->EmergencyStop();
        });
    }

public:
    RoverBot() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeDisplay();
        InitializeButtons();
        InitializeCamera();
        robot_ = new RoverRobot();
        if (eyes_display_) eyes_display_->Attach(eyes_, robot_);
        if (eyes_) {
            // touching the sensor: the body reacts too (happy wiggle / hug)
            eyes_->on_touch_emotion = [this](const char* e) { if (robot_) robot_->OnEmotion(e); };
        }
        tools_ = new RoverTools(robot_, display_, eyes_);
        web_ = new RoverWeb(robot_, eyes_);
        web_->Start();  // manual control page, starts once Wi-Fi has an IP
        ESP_LOGI(TAG, "Rover-Bot ready");
    }

    virtual AudioCodec* GetAudioCodec() override {
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                              AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
                                              AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override { return display_; }

    virtual Camera* GetCamera() override { return camera_; }
};

DECLARE_BOARD(RoverBot);
