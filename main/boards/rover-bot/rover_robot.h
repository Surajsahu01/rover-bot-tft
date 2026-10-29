// rover_robot.h - low level robot control for Rover-Bot
//   * 2 x N20 motors through an L298N (PWM on the IN pins, ENA/ENB jumpers ON)
//   * 3 x hobby servos (head, left arm, right arm) driven by LEDC at 50 Hz
//   * gesture player, talking animation synced to the AI speech state
//   * pick-up / put-down sequences
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <string>

#include <driver/ledc.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "application.h"
#include "config.h"
#include "device_state.h"
#include "settings.h"

#define ROVER_TAG "RoverRobot"

// LEDC allocation. The camera driver owns LEDC timer 0 / channel 0 (XCLK).
#define ROVER_MOTOR_TIMER   LEDC_TIMER_1
#define ROVER_SERVO_TIMER   LEDC_TIMER_2
#define ROVER_MOTOR_FREQ    5000
#define ROVER_MOTOR_RES     LEDC_TIMER_10_BIT
#define ROVER_SERVO_RES     LEDC_TIMER_14_BIT

class RoverRobot {
public:
    enum Gesture {
        kGestureWave, kGestureNod, kGestureShake, kGestureHappy, kGestureSad,
        kGestureSurprised, kGestureThink, kGestureCheer, kGestureDance,
        kGestureHug, kGestureShy, kGestureCount
    };

    static const char* GestureName(int g) {
        static const char* names[kGestureCount] = {
            "wave", "nod", "shake_head", "happy", "sad", "surprised",
            "think", "cheer", "dance", "hug", "shy"};
        return (g >= 0 && g < kGestureCount) ? names[g] : "";
    }
    static int GestureFromName(const std::string& n) {
        for (int i = 0; i < kGestureCount; i++) {
            if (n == GestureName(i)) return i;
        }
        return -1;
    }

    RoverRobot() {
        LoadCalibration();
        InitMotors();
        InitServos();
        gesture_queue_ = xQueueCreate(4, sizeof(int));
        const esp_timer_create_args_t args = {
            .callback = &RoverRobot::DriveTimeoutCb,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "drive_timeout",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &drive_timer_));
        xTaskCreate(&RoverRobot::ServoTaskThunk, "servo", 3072, this, 5, nullptr);
        xTaskCreate(&RoverRobot::GestureTaskThunk, "gesture", 4096, this, 4, nullptr);
        xTaskCreate(&RoverRobot::TalkTaskThunk, "talk_sync", 3072, this, 3, nullptr);
        GoRest(true);
    }

    // ------------------------------------------------------------------ drive
    // left / right: -100..100 (percent). duration_ms: auto-stop watchdog.
    void Drive(int left, int right, int duration_ms) {
        esp_timer_stop(drive_timer_);
        left = std::clamp(left, -100, 100);
        right = std::clamp(right, -100, 100);
        if (calib_.swap_motors) std::swap(left, right);
        SetMotor(true, calib_.motor_l_inv ? -left : left);
        SetMotor(false, calib_.motor_r_inv ? -right : right);
        moving_ = (left != 0 || right != 0);
        if (moving_) {
            esp_timer_start_once(drive_timer_, (uint64_t)std::clamp(duration_ms, 50, 600000) * 1000ULL);
        }
    }

    void Stop() {
        esp_timer_stop(drive_timer_);
        SetMotor(true, 0);
        SetMotor(false, 0);
        moving_ = false;
    }

    void WaitUntilStopped(int timeout_ms) {
        int waited = 0;
        while (moving_ && waited < timeout_ms) {
            vTaskDelay(pdMS_TO_TICKS(20));
            waited += 20;
        }
    }

    // Time needed to travel `distance_cm` at `speed` (from the calibrated cm/s).
    int EstimateMoveMs(int distance_cm, int speed) const {
        speed = std::clamp(speed, 30, 100);
        float cm_per_s = calib_.cm_per_sec_70 * speed / 70.0f;
        return std::clamp((int)(distance_cm / std::max(cm_per_s, 1.0f) * 1000.0f), 80, 120000);
    }

    // Move straight by an approximate distance, then stop by itself (timer watchdog).
    // wait=false returns at once; the robot still stops after the distance is covered.
    int Move(bool forward, int distance_cm, int speed, bool wait) {
        speed = std::clamp(speed, 30, 100);
        int ms = EstimateMoveMs(distance_cm, speed);
        Drive(forward ? speed : -speed, forward ? speed : -speed, ms);
        if (wait) WaitUntilStopped(ms + 300);
        return ms;
    }

    // Keep moving until Stop() is called (safety: stops by itself after max_seconds).
    // dir: 0 forward, 1 backward, 2 left (spin), 3 right (spin)
    void StartContinuous(int dir, int speed, int max_seconds) {
        speed = std::clamp(speed, 30, 100);
        int l = speed, r = speed;
        if (dir == 1) { l = -speed; r = -speed; }
        else if (dir == 2) { l = -speed; r = speed; }
        else if (dir == 3) { l = speed; r = -speed; }
        Drive(l, r, std::clamp(max_seconds, 1, 600) * 1000);
    }

    // Spin in place by an approximate angle.
    int Turn(bool left, int degrees, int speed, bool wait) {
        speed = std::clamp(speed, 40, 100);
        float ms_f = calib_.turn_ms_90 * (degrees / 90.0f) * (70.0f / speed);
        int ms = std::clamp((int)ms_f, 60, 20000);
        Drive(left ? -speed : speed, left ? speed : -speed, ms);
        if (wait) WaitUntilStopped(ms + 300);
        return ms;
    }

    bool IsMoving() const { return moving_; }

    // ----------------------------------------------------------------- servos
    // head 45..135 ; arms 0..180 ; value < 0 leaves the servo unchanged.
    void SetPose(int head, int left_arm, int right_arm, int speed_dps, bool wait) {
        servo_speed_dps_ = std::clamp(speed_dps, 10, 400);
        if (head >= 0) servos_[0].target = std::clamp(head, 45, 135);
        if (left_arm >= 0) servos_[1].target = std::clamp(left_arm, 0, 180);
        if (right_arm >= 0) servos_[2].target = std::clamp(right_arm, 0, 180);
        if (wait) WaitServos(4000);
    }

    void WaitServos(int timeout_ms) {
        int waited = 0;
        while (waited < timeout_ms) {
            bool done = true;
            for (auto& s : servos_) {
                if (std::fabs(s.target - s.current) > 1.5f) done = false;
            }
            if (done) return;
            vTaskDelay(pdMS_TO_TICKS(20));
            waited += 20;
        }
    }

    void GoRest(bool instant = false) {
        carrying_ = false;
        if (instant) {
            for (int i = 0; i < 3; i++) {
                servos_[i].current = servos_[i].target = (i == 0) ? HEAD_CENTER : ARM_REST;
            }
        }
        SetPose(HEAD_CENTER, ARM_REST, ARM_REST, 120, false);
    }

    // ---------------------------------------------------------------- gestures
    bool PlayGesture(int g, bool wait) {
        if (g < 0 || g >= kGestureCount) return false;
        xQueueSend(gesture_queue_, &g, 0);
        if (wait) {
            vTaskDelay(pdMS_TO_TICKS(60));
            int waited = 0;
            while ((gesture_busy_ || uxQueueMessagesWaiting(gesture_queue_)) && waited < 8000) {
                vTaskDelay(pdMS_TO_TICKS(30));
                waited += 30;
            }
        }
        return true;
    }

    // Called from the display when the AI picks an emotion for its sentence.
    void OnEmotion(const char* emotion) {
        if (!emotion || ManualActive() || gesture_busy_ || uxQueueMessagesWaiting(gesture_queue_) > 0) return;
        std::string e(emotion);
        int g = -1;
        if (e == "happy" || e == "laughing" || e == "funny" || e == "silly") g = kGestureHappy;
        else if (e == "loving" || e == "kissy") g = carrying_ ? kGestureShy : kGestureHug;
        else if (e == "sad" || e == "crying") g = kGestureSad;
        else if (e == "surprised" || e == "shocked") g = kGestureSurprised;
        else if (e == "thinking" || e == "confused") g = kGestureThink;
        else if (e == "winking" || e == "cool" || e == "confident") g = kGestureNod;
        else if (e == "angry") g = kGestureShake;
        else if (e == "embarrassed") g = kGestureShy;
        if (g >= 0) xQueueSend(gesture_queue_, &g, 0);
    }

    // ------------------------------------------------------------ manipulation
    // Scoop sequence: lower the arms, creep forward under the object, lift.
    void PickUp() {
        carrying_ = false;
        gesture_hold_ = true;  // pause talk animation while manipulating
        SetPose(HEAD_CENTER, ARM_SCOOP_LOW, ARM_SCOOP_LOW, 100, true);
        vTaskDelay(pdMS_TO_TICKS(150));
        Drive(45, 45, PICKUP_PUSH_MS);
        WaitUntilStopped(PICKUP_PUSH_MS + 300);
        vTaskDelay(pdMS_TO_TICKS(150));
        SetPose(-1, ARM_CARRY, ARM_CARRY, 45, true);  // slow, gentle lift
        carrying_ = true;
        gesture_hold_ = false;
    }

    void PutDown() {
        gesture_hold_ = true;
        SetPose(-1, ARM_SCOOP_LOW, ARM_SCOOP_LOW, 40, true);
        vTaskDelay(pdMS_TO_TICKS(200));
        Drive(-45, -45, 450);  // back away from the object
        WaitUntilStopped(750);
        carrying_ = false;
        SetPose(HEAD_CENTER, ARM_REST, ARM_REST, 100, true);
        gesture_hold_ = false;
    }

    bool IsCarrying() const { return carrying_; }

    // Called by the manual web app: pauses the automatic talking animation and
    // emotion gestures for 8 s so they do not fight the person holding the sliders.
    void NoteManualControl() { manual_until_ms_ = esp_timer_get_time() / 1000 + 8000; }
    bool ManualActive() const { return esp_timer_get_time() / 1000 < manual_until_ms_; }

    // ------------------------------------------------------------- calibration
    struct Calibration {
        bool motor_l_inv = false;
        bool motor_r_inv = false;
        bool swap_motors = false;
        bool arm_l_inv = false;
        bool arm_r_inv = false;
        int head_trim = 0;
        int larm_trim = 0;
        int rarm_trim = 0;
        int turn_ms_90 = DEFAULT_TURN_MS_PER_90DEG;
        int cm_per_sec_70 = DEFAULT_CM_PER_SEC_AT_70;
    };

    bool SetCalibration(const std::string& key, int value) {
        if (key == "motor_left_invert") calib_.motor_l_inv = value != 0;
        else if (key == "motor_right_invert") calib_.motor_r_inv = value != 0;
        else if (key == "swap_motors") calib_.swap_motors = value != 0;
        else if (key == "left_arm_invert") calib_.arm_l_inv = value != 0;
        else if (key == "right_arm_invert") calib_.arm_r_inv = value != 0;
        else if (key == "head_trim") calib_.head_trim = std::clamp(value, -30, 30);
        else if (key == "left_arm_trim") calib_.larm_trim = std::clamp(value, -30, 30);
        else if (key == "right_arm_trim") calib_.rarm_trim = std::clamp(value, -30, 30);
        else if (key == "turn_ms_per_90deg") calib_.turn_ms_90 = std::clamp(value, 100, 3000);
        else if (key == "cm_per_sec") calib_.cm_per_sec_70 = std::clamp(value, 2, 80);
        else return false;
        SaveCalibration();
        return true;
    }

    std::string CalibrationJson() const {
        char buf[420];
        snprintf(buf, sizeof(buf),
                 "{\"motor_left_invert\":%d,\"motor_right_invert\":%d,\"swap_motors\":%d,"
                 "\"left_arm_invert\":%d,\"right_arm_invert\":%d,\"head_trim\":%d,"
                 "\"left_arm_trim\":%d,\"right_arm_trim\":%d,\"turn_ms_per_90deg\":%d,"
                 "\"cm_per_sec\":%d}",
                 calib_.motor_l_inv, calib_.motor_r_inv, calib_.swap_motors, calib_.arm_l_inv,
                 calib_.arm_r_inv, calib_.head_trim, calib_.larm_trim, calib_.rarm_trim,
                 calib_.turn_ms_90, calib_.cm_per_sec_70);
        return buf;
    }

    std::string StatusJson() const {
        char buf[200];
        snprintf(buf, sizeof(buf),
                 "{\"moving\":%s,\"carrying_object\":%s,\"head\":%d,\"left_arm\":%d,\"right_arm\":%d}",
                 moving_ ? "true" : "false", carrying_ ? "true" : "false",
                 (int)servos_[0].current, (int)servos_[1].current, (int)servos_[2].current);
        return buf;
    }

    // Emergency stop: wheels off, gestures cleared, arms back to rest.
    void EmergencyStop() {
        Stop();
        xQueueReset(gesture_queue_);
        gesture_abort_ = true;
        SetPose(HEAD_CENTER, ARM_REST, ARM_REST, 150, false);
    }

private:
    struct Servo {
        ledc_channel_t channel;
        gpio_num_t gpio;
        std::atomic<float> current{90};
        std::atomic<float> target{90};
    };

    Servo servos_[3];
    Calibration calib_;
    std::atomic<bool> moving_{false};
    std::atomic<bool> carrying_{false};
    std::atomic<bool> gesture_busy_{false};
    std::atomic<bool> gesture_abort_{false};
    std::atomic<bool> gesture_hold_{false};
    std::atomic<int64_t> manual_until_ms_{0};
    std::atomic<int> servo_speed_dps_{120};
    esp_timer_handle_t drive_timer_ = nullptr;
    QueueHandle_t gesture_queue_ = nullptr;

    // ------------------------------------------------------------------ motors
    // L298N wiring: IN1..IN4 are plain direction pins, ENA / ENB carry the PWM speed signal.
    // (The ENA/ENB jumper caps on the L298N must be REMOVED.)
    static constexpr ledc_channel_t kEnCh[2] = {LEDC_CHANNEL_1, LEDC_CHANNEL_2};

    void InitMotors() {
        const gpio_num_t in_pins[4] = {MOTOR_L_IN1, MOTOR_L_IN2, MOTOR_R_IN3, MOTOR_R_IN4};
        for (int i = 0; i < 4; i++) {
            gpio_config_t io = {};
            io.pin_bit_mask = 1ULL << in_pins[i];
            io.mode = GPIO_MODE_OUTPUT;
            ESP_ERROR_CHECK(gpio_config(&io));
            gpio_set_level(in_pins[i], 0);
        }
        ledc_timer_config_t t = {};
        t.speed_mode = LEDC_LOW_SPEED_MODE;
        t.timer_num = ROVER_MOTOR_TIMER;
        t.duty_resolution = ROVER_MOTOR_RES;
        t.freq_hz = ROVER_MOTOR_FREQ;
        t.clk_cfg = LEDC_AUTO_CLK;
        ESP_ERROR_CHECK(ledc_timer_config(&t));
        const gpio_num_t en_pins[2] = {MOTOR_L_EN, MOTOR_R_EN};
        for (int i = 0; i < 2; i++) {
            ledc_channel_config_t c = {};
            c.gpio_num = en_pins[i];
            c.speed_mode = LEDC_LOW_SPEED_MODE;
            c.channel = kEnCh[i];
            c.timer_sel = ROVER_MOTOR_TIMER;
            c.duty = 0;
            c.hpoint = 0;
            ESP_ERROR_CHECK(ledc_channel_config(&c));
        }
    }

    void EnableDuty(int idx, int duty_pct) {
        uint32_t duty = (uint32_t)((1 << 10) - 1) * duty_pct / 100;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, kEnCh[idx], duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, kEnCh[idx]);
    }

    // pct > 0 : forward ; pct < 0 : reverse ; 0 : brake (both IN low, EN high).
    void SetMotor(bool left, int pct) {
        int idx = left ? 0 : 1;
        gpio_num_t in_a = left ? MOTOR_L_IN1 : MOTOR_R_IN3;
        gpio_num_t in_b = left ? MOTOR_L_IN2 : MOTOR_R_IN4;
        if (pct == 0) {
            gpio_set_level(in_a, 0);
            gpio_set_level(in_b, 0);
            EnableDuty(idx, 100);  // brake
            return;
        }
        int mag = std::abs(pct);
        int duty = MOTOR_MIN_DUTY_PCT + (100 - MOTOR_MIN_DUTY_PCT) * mag / 100;
        gpio_set_level(in_a, pct > 0 ? 1 : 0);
        gpio_set_level(in_b, pct > 0 ? 0 : 1);
        EnableDuty(idx, duty);
    }

    static void DriveTimeoutCb(void* arg) {
        auto* self = static_cast<RoverRobot*>(arg);
        self->SetMotor(true, 0);
        self->SetMotor(false, 0);
        self->moving_ = false;
    }

    // ------------------------------------------------------------------ servos
    void InitServos() {
        ledc_timer_config_t t = {};
        t.speed_mode = LEDC_LOW_SPEED_MODE;
        t.timer_num = ROVER_SERVO_TIMER;
        t.duty_resolution = ROVER_SERVO_RES;
        t.freq_hz = 50;
        t.clk_cfg = LEDC_AUTO_CLK;
        ESP_ERROR_CHECK(ledc_timer_config(&t));
        const gpio_num_t pins[3] = {SERVO_HEAD_GPIO, SERVO_LEFT_ARM_GPIO, SERVO_RIGHT_ARM_GPIO};
        const ledc_channel_t chans[3] = {LEDC_CHANNEL_5, LEDC_CHANNEL_6, LEDC_CHANNEL_7};
        for (int i = 0; i < 3; i++) {
            servos_[i].channel = chans[i];
            servos_[i].gpio = pins[i];
            servos_[i].current = servos_[i].target = (i == 0) ? HEAD_CENTER : ARM_REST;
            ledc_channel_config_t c = {};
            c.gpio_num = pins[i];
            c.speed_mode = LEDC_LOW_SPEED_MODE;
            c.channel = chans[i];
            c.timer_sel = ROVER_SERVO_TIMER;
            c.duty = 0;
            c.hpoint = 0;
            ESP_ERROR_CHECK(ledc_channel_config(&c));
        }
    }

    // logical angle -> physical angle (inversion + trim)
    int Physical(int idx, float logical) const {
        float a = logical;
        if (idx == 1) a = (calib_.arm_l_inv ? 180 - a : a) + calib_.larm_trim;
        else if (idx == 2) a = (calib_.arm_r_inv ? 180 - a : a) + calib_.rarm_trim;
        else a += calib_.head_trim;
        return (int)std::clamp(a, 0.0f, 180.0f);
    }

    void WriteServo(int idx, float logical) {
        int deg = Physical(idx, logical);
        uint32_t us = SERVO_MIN_US + (uint32_t)(SERVO_MAX_US - SERVO_MIN_US) * deg / 180;
        uint32_t duty = (uint32_t)((uint64_t)us * (1 << 14) / 20000);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, servos_[idx].channel, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, servos_[idx].channel);
    }

    static void ServoTaskThunk(void* arg) { static_cast<RoverRobot*>(arg)->ServoTask(); }

    void ServoTask() {
        const float dt = 0.02f;
        TickType_t last = xTaskGetTickCount();
        while (true) {
            float max_step = servo_speed_dps_ * dt;
            for (int i = 0; i < 3; i++) {
                float cur = servos_[i].current;
                float tgt = servos_[i].target;
                float diff = tgt - cur;
                if (std::fabs(diff) <= max_step) cur = tgt;
                else cur += (diff > 0 ? max_step : -max_step);
                servos_[i].current = cur;
                WriteServo(i, cur);
            }
            vTaskDelayUntil(&last, pdMS_TO_TICKS(20));
        }
    }

    // ---------------------------------------------------------------- gestures
    struct Key {
        int head, l, r, hold_ms;  // -1 = unchanged
    };

    void RunKeys(const Key* keys, int n, int speed) {
        for (int i = 0; i < n && !gesture_abort_; i++) {
            SetPose(keys[i].head, keys[i].l, keys[i].r, speed, false);
            vTaskDelay(pdMS_TO_TICKS(keys[i].hold_ms));
        }
    }

    static void GestureTaskThunk(void* arg) { static_cast<RoverRobot*>(arg)->GestureTask(); }

    void GestureTask() {
        int g;
        while (true) {
            if (xQueueReceive(gesture_queue_, &g, portMAX_DELAY) != pdTRUE) continue;
            gesture_abort_ = false;
            gesture_busy_ = true;
            bool hold = carrying_;  // while carrying, keep the arms still
            int h = HEAD_CENTER, R = ARM_REST;
            switch (g) {
                case kGestureWave: {
                    if (hold) { Key k[] = {{70, -1, -1, 250}, {110, -1, -1, 250}, {90, -1, -1, 200}}; RunKeys(k, 3, 300); break; }
                    Key k[] = {{-1, -1, 150, 300}, {-1, -1, 120, 220}, {-1, -1, 160, 220},
                               {-1, -1, 120, 220}, {-1, -1, 160, 220}, {h, R, R, 300}};
                    RunKeys(k, 6, 260);
                    break;
                }
                case kGestureNod: {
                    Key k[] = {{105, -1, -1, 200}, {80, -1, -1, 200}, {105, -1, -1, 200}, {h, -1, -1, 150}};
                    RunKeys(k, 4, 250);
                    break;
                }
                case kGestureShake: {
                    Key k[] = {{65, -1, -1, 180}, {115, -1, -1, 180}, {65, -1, -1, 180}, {115, -1, -1, 180}, {h, -1, -1, 150}};
                    RunKeys(k, 5, 350);
                    break;
                }
                case kGestureHappy: {
                    if (hold) { Key k[] = {{75, -1, -1, 200}, {105, -1, -1, 200}, {75, -1, -1, 200}, {h, -1, -1, 100}}; RunKeys(k, 4, 300); break; }
                    Key k[] = {{80, 120, 120, 250}, {100, 70, 70, 250}, {80, 130, 130, 250},
                               {100, 70, 70, 250}, {h, R, R, 250}};
                    RunKeys(k, 5, 300);
                    break;
                }
                case kGestureSad: {
                    Key k[] = {{105, 0, 0, 900}, {h, R, R, 200}};
                    RunKeys(k, 2, 60);
                    break;
                }
                case kGestureSurprised: {
                    if (hold) { Key k[] = {{110, -1, -1, 600}, {h, -1, -1, 100}}; RunKeys(k, 2, 400); break; }
                    Key k[] = {{h + 10, 150, 150, 700}, {h, R, R, 200}};
                    RunKeys(k, 2, 400);
                    break;
                }
                case kGestureThink: {
                    if (hold) { Key k[] = {{70, -1, -1, 900}, {h, -1, -1, 100}}; RunKeys(k, 2, 80); break; }
                    Key k[] = {{70, R, 110, 1000}, {h, R, R, 200}};
                    RunKeys(k, 2, 100);
                    break;
                }
                case kGestureCheer: {
                    if (hold) { Key k[] = {{70, -1, -1, 180}, {110, -1, -1, 180}, {70, -1, -1, 180}, {h, -1, -1, 100}}; RunKeys(k, 4, 300); break; }
                    Key k[] = {{h, 170, 170, 400}, {80, 140, 170, 200}, {100, 170, 140, 200},
                               {80, 140, 170, 200}, {h, R, R, 250}};
                    RunKeys(k, 5, 350);
                    break;
                }
                case kGestureDance: {
                    if (hold) { Key k[] = {{60, -1, -1, 250}, {120, -1, -1, 250}, {60, -1, -1, 250}, {120, -1, -1, 250}, {h, -1, -1, 100}}; RunKeys(k, 5, 300); break; }
                    Key k[] = {{70, 150, 30, 280}, {110, 30, 150, 280}, {70, 150, 30, 280},
                               {110, 30, 150, 280}, {90, 160, 160, 300}, {90, 30, 30, 300},
                               {70, 150, 30, 280}, {110, 30, 150, 280}, {h, R, R, 300}};
                    RunKeys(k, 9, 400);
                    break;
                }
                case kGestureHug: {
                    if (hold) break;
                    Key k[] = {{h, 100, 100, 500}, {h, 60, 60, 700}, {h, R, R, 200}};
                    RunKeys(k, 3, 160);
                    break;
                }
                case kGestureShy: {
                    if (hold) { Key k[] = {{105, -1, -1, 800}, {h, -1, -1, 100}}; RunKeys(k, 2, 80); break; }
                    Key k[] = {{110, 70, 70, 900}, {h, R, R, 200}};
                    RunKeys(k, 2, 120);
                    break;
                }
            }
            gesture_busy_ = false;
        }
    }

    // ------------------------------------------- talking animation (speech sync)
    static void TalkTaskThunk(void* arg) { static_cast<RoverRobot*>(arg)->TalkTask(); }

    void TalkTask() {
        DeviceState last = kDeviceStateUnknown;
        int64_t next_move_ms = 0;
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(80));
            DeviceState st = Application::GetInstance().GetDeviceState();
            int64_t now = esp_timer_get_time() / 1000;
            bool free_to_animate = !gesture_busy_ && !gesture_hold_ && !ManualActive() &&
                                   uxQueueMessagesWaiting(gesture_queue_) == 0;
            if (st != last) {
                if (st == kDeviceStateListening && free_to_animate) {
                    SetPose(HEAD_CENTER + 8, -1, -1, 80, false);  // "I'm listening" tilt
                } else if (st == kDeviceStateIdle && free_to_animate) {
                    SetPose(HEAD_CENTER, carrying_ ? -1 : ARM_REST, carrying_ ? -1 : ARM_REST, 60, false);
                }
                last = st;
            }
            if (st == kDeviceStateSpeaking && free_to_animate && now >= next_move_ms) {
                int head = HEAD_CENTER + (int)(esp_random() % 21) - 10;
                if (!carrying_) {
                    int la = ARM_REST + (int)(esp_random() % 26);
                    int ra = ARM_REST + (int)(esp_random() % 26);
                    SetPose(head, la, ra, 140, false);
                } else {
                    SetPose(head, -1, -1, 140, false);
                }
                next_move_ms = now + 350 + (esp_random() % 600);
            }
        }
    }

    // ----------------------------------------------------------- persistence
    void LoadCalibration() {
        Settings s("rover", false);
        calib_.motor_l_inv = s.GetBool("ml_inv", false);
        calib_.motor_r_inv = s.GetBool("mr_inv", false);
        calib_.swap_motors = s.GetBool("swap", false);
        calib_.arm_l_inv = s.GetBool("al_inv", false);
        calib_.arm_r_inv = s.GetBool("ar_inv", true);  // mirrored mounting is the common case
        calib_.head_trim = s.GetInt("h_trim", 0);
        calib_.larm_trim = s.GetInt("l_trim", 0);
        calib_.rarm_trim = s.GetInt("r_trim", 0);
        calib_.turn_ms_90 = s.GetInt("turn90", DEFAULT_TURN_MS_PER_90DEG);
        calib_.cm_per_sec_70 = s.GetInt("cmps", DEFAULT_CM_PER_SEC_AT_70);
    }

    void SaveCalibration() {
        Settings s("rover", true);
        s.SetBool("ml_inv", calib_.motor_l_inv);
        s.SetBool("mr_inv", calib_.motor_r_inv);
        s.SetBool("swap", calib_.swap_motors);
        s.SetBool("al_inv", calib_.arm_l_inv);
        s.SetBool("ar_inv", calib_.arm_r_inv);
        s.SetInt("h_trim", calib_.head_trim);
        s.SetInt("l_trim", calib_.larm_trim);
        s.SetInt("r_trim", calib_.rarm_trim);
        s.SetInt("turn90", calib_.turn_ms_90);
        s.SetInt("cmps", calib_.cm_per_sec_70);
    }
};
