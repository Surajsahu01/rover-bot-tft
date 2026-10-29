#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

// ============================================================================
//  Rover-Bot : ESP32-S3 (N16R8 / N8R8, octal PSRAM) + OV2640 camera
//  Pin map - see docs/WIRING.md for the wiring diagram.
// ============================================================================

// ---------------- Audio: INMP441 mic + MAX98357A amp share BCLK and WS ------
#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_I2S_GPIO_WS    GPIO_NUM_1    // INMP441 WS  + MAX98357A LRC
#define AUDIO_I2S_GPIO_BCLK  GPIO_NUM_2    // INMP441 SCK + MAX98357A BCLK
#define AUDIO_I2S_GPIO_DIN   GPIO_NUM_42   // INMP441 SD  (mic data in)
#define AUDIO_I2S_GPIO_DOUT  GPIO_NUM_41   // MAX98357A DIN (speaker data out)

#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define BUILTIN_LED_GPIO        GPIO_NUM_NC

// ---------------- Camera (standard ESP32-S3-CAM / Freenove style pinout) -----
#define CAMERA_PIN_D0   GPIO_NUM_11
#define CAMERA_PIN_D1   GPIO_NUM_9
#define CAMERA_PIN_D2   GPIO_NUM_8
#define CAMERA_PIN_D3   GPIO_NUM_10
#define CAMERA_PIN_D4   GPIO_NUM_12
#define CAMERA_PIN_D5   GPIO_NUM_18
#define CAMERA_PIN_D6   GPIO_NUM_17
#define CAMERA_PIN_D7   GPIO_NUM_16
#define CAMERA_PIN_XCLK GPIO_NUM_15
#define CAMERA_PIN_PCLK GPIO_NUM_13
#define CAMERA_PIN_VSYNC GPIO_NUM_6
#define CAMERA_PIN_HREF GPIO_NUM_7
#define CAMERA_PIN_SIOC GPIO_NUM_5
#define CAMERA_PIN_SIOD GPIO_NUM_4
#define CAMERA_PIN_PWDN GPIO_NUM_NC
#define CAMERA_PIN_RESET GPIO_NUM_NC
#define XCLK_FREQ_HZ 20000000

// ---------------- 2.4" SPI TFT, ILI9341 240x320 (used in landscape 320x240) ---
// The ESP32-S3 N16R8 has no spare pins left, so the screen uses the minimum of 3 wires
// plus tie-offs on the module:
//   module SCK/CLK -> GPIO21     module SDI/MOSI -> GPIO47     module DC/RS -> GPIO43 (TX0)
//   module CS  -> GND  (always selected)     module RESET -> 3V3     module LED/BL -> 3V3
//   module VCC -> 3V3  module GND -> GND     (MISO / touch / SD pins stay unconnected)
// GPIO43 is the serial-log pin, so the boot log no longer prints (the screen is more useful).
// Got a spare pin? (e.g. set ROVER_TOUCH_GPIO to -1 to free GPIO45) -> give CS / RST / BL a pin
// below instead of tying them.
#define DISPLAY_SCLK_PIN GPIO_NUM_21
#define DISPLAY_MOSI_PIN GPIO_NUM_47
#define DISPLAY_DC_PIN   GPIO_NUM_43
#define DISPLAY_CS_PIN   GPIO_NUM_NC     // NC = module CS wired to GND
#define DISPLAY_RST_PIN  GPIO_NUM_NC     // NC = module RESET wired to 3V3 (software reset)
#define DISPLAY_BL_PIN   GPIO_NUM_NC     // NC = module LED wired to 3V3 (backlight always on)
#define DISPLAY_SPI_HOST SPI3_HOST
#define DISPLAY_SPI_HZ   (40 * 1000 * 1000)   // lower to 20 MHz if the picture has noise/streaks
#ifdef CONFIG_ROVER_TFT_FLIP                  // rotate the picture 180 degrees
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y true
#else
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#endif
#ifdef CONFIG_ROVER_TFT_RGB_ORDER             // colours red <-> blue swapped? use this variant
#define DISPLAY_RGB_ORDER LCD_RGB_ELEMENT_ORDER_RGB
#else
#define DISPLAY_RGB_ORDER LCD_RGB_ELEMENT_ORDER_BGR
#endif
#ifdef CONFIG_ROVER_TFT_INVERT                // colours look like a negative? use this variant
#define DISPLAY_INVERT_COLOR true
#else
#define DISPLAY_INVERT_COLOR false
#endif
#define ROVER_TFT_EYE_COLOR 0x00E5FF   // 0xRRGGBB colour of the eyes / text
#define ROVER_TFT_BG_COLOR  0x000000

// Live camera video on the TFT. QVGA (320x240) = smooth, VGA (640x480) = sharper but slower.
// Photos for the AI ("what is this?") are always taken in VGA.
#define ROVER_CAM_PREVIEW_FRAMESIZE FRAMESIZE_QVGA

// ---------------- L298N motor driver (remove the ENA / ENB jumper caps!) -----
#define MOTOR_L_IN1 GPIO_NUM_38
#define MOTOR_L_IN2 GPIO_NUM_39
#define MOTOR_R_IN3 GPIO_NUM_40
#define MOTOR_R_IN4 GPIO_NUM_14
#define MOTOR_L_EN  GPIO_NUM_19   // ENA  (PWM speed, left motor)
#define MOTOR_R_EN  GPIO_NUM_20   // ENB  (PWM speed, right motor)

// ---------------- Servos (powered from their own 5 V rail!) ------------------
#define SERVO_HEAD_GPIO       GPIO_NUM_48
#define SERVO_LEFT_ARM_GPIO   GPIO_NUM_46
#define SERVO_RIGHT_ARM_GPIO  GPIO_NUM_3
#define SERVO_MIN_US 500
#define SERVO_MAX_US 2500

// ---------------- Touch sensor (TTP223 etc., output HIGH when touched) -------
// Idle LOW is required: do not touch the sensor while the robot powers up (GPIO45 is a boot pin).
#define ROVER_TOUCH_GPIO 45        // set to -1 if you have no touch sensor
#define ROVER_EYES_FOLLOW_AI 1     // 1 = eyes also show the AI's emotion while it talks, 0 = touch only

// ---------------- Manual web control app (http://<robot-ip>:8080) ------------
#define ROVER_WEB_PIN  "1234"     // CHANGE THIS to your own digits, then rebuild
#define ROVER_WEB_PORT 8080

// ---------------- Default motion calibration (changeable by voice later) -----
#define MOTOR_MIN_DUTY_PCT   35    // N20 motors rarely start below this
#define DEFAULT_TURN_MS_PER_90DEG 520   // spin time for a 90 degree turn @ speed 70
#define DEFAULT_CM_PER_SEC_AT_70  14    // forward speed at speed 70

// ---------------- Arm / head poses (degrees) ---------------------------------
// Arms: 0 = hanging down, 90 = pointing forward, 180 = straight up.
// Head: 90 = centre, limited to 45..135.
#define HEAD_CENTER      90
#define ARM_REST         10
#define ARM_SCOOP_LOW    28     // forearms near the floor, ready to scoop
#define ARM_CARRY        105    // object cradled and lifted
#define PICKUP_PUSH_MS   650    // creep forward while scooping

#endif // _BOARD_CONFIG_H_
