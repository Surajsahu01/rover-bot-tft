# Rover-Bot: Xiaozhi AI robot - complete guide

Firmware = the official **xiaozhi-esp32** project (pinned snapshot in this folder) plus a new board
`main/boards/rover-bot/`. Voice, wake word, emoji face, vision and cloud AI come from Xiaozhi; the
robot body is exposed to the AI as MCP tools (move, turn, gesture, pickup ...).

> Honest status: I could not compile or test on real hardware here. The robot logic passed a C++
> syntax check, and the board follows the upstream camera/duplex-audio/MCP patterns, but expect a
> little calibration (motor direction, arm direction, turn time) on first run. All of it is fixable
> by voice, no re-flash.

## 1. Build the hardware
Follow `docs/WIRING.md` (pin table + power diagram). Parts: ESP32-S3 N16R8 CAM board, OV2640,
INMP441, MAX98357A + 3 W speaker, 2.4 inch SPI TFT (ILI9341, 240x320), L298N, 2 x N20
(6 V gearmotor), 3 x SG90/MG90S servos, 2S battery, 5 V 3 A buck, capacitors.

## 2. Get the firmware (.bin) - no PC toolchain needed
1. Create a free GitHub account and an empty repository, then push this folder (see chat steps).
2. Open **Actions -> Build Rover-Bot firmware -> Run workflow**. After ~10 min download the artifact:
   * rover-bot-tft - try this first
   * rover-bot-tft-flip - picture upside down (rotated 180 degrees)
   * rover-bot-tft-rgb - red and blue colours swapped (skin looks blue)
   * rover-bot-tft-flip-rgb - both of the above
   * rover-bot-tft-invert - picture looks like a negative
Local build alternative: install ESP-IDF 6.1, then python scripts/build.py rover-bot --name rover-bot-tft --language en-US
(output build/merged-binary.bin).

TFT wiring (2.4 inch ILI9341, only 3 signal wires - see `docs/WIRING.md`): VCC->3V3, GND->GND, SCK->GPIO21,
SDI/MOSI->GPIO47, DC->GPIO43, and on the module **CS->GND, RESET->3V3, LED->3V3**. The screen is the robot's
**eyes** and, on request, its **live camera view** (Xiaozhi's own UI does not draw on it).
The camera works for "what is this?", for fetching objects and now also as live video on the TFT (6a).

## 3. Flash
Unzip artifact -> `merged-binary.bin`. Flash at address 0x0:
* Browser: https://espressif.github.io/esptool-js/ (Chrome), baud 460800, address `0x0`.
* or `esptool --chip esp32s3 -p COMx write-flash 0x0 merged-binary.bin`
Use the board's UART USB port (any USB port works). If not detected: hold BOOT, tap RESET.

## 4. First start and AI account
1. The robot opens Wi-Fi setup (hotspot "Xiaozhi-xxxx"): connect with phone, enter home Wi-Fi.
2. It speaks a 6-digit code. Log in at **https://xiaozhi.me**, "Add device", enter the code.
3. In the console create/edit the agent, paste `docs/AGENT_PROMPT.md` as role prompt, pick a
   voice and a language. Vision ("what is this?") works through the service's built-in vision model.
4. Talk: tap **BOOT** once to start/stop a chat (or use the wake word). **Hold BOOT = emergency stop.**

## 5. First-run checks (say these, robot on a stand, wheels in the air)
1. "Run the robot self test." Order: servos sweep, forward, backward, left turn, right turn.
2. Wheels wrong way: "Calibrate: motor_left_invert 1" / "motor_right_invert 1". Forward and back right
   but turn wrong: "swap_motors 1".
3. Arm moves wrong way: "calibrate left_arm_invert 1" (right arm is inverted by default, set 0 if wrong).
   Arm not level: "left_arm_trim -5". Same for head_trim.
4. Turning angle: command "turn right 90 degrees", measure, adjust `turn_ms_per_90deg` (default 520).
   Distance: "move forward 20 cm", measure, adjust `cm_per_sec` (default 14).
Settings persist in flash.

## 6. How to use it
| Say | What happens |
|---|---|
| "Hello Rover" | Talks; head/arms gesture in sync (emotion of each sentence -> gesture, plus random talking motion) |
| "Go forward 20 cm / turn left 90" | Drives (max ~40 cm per step) |
| "Stop" | Wheels stop (also hold BOOT) |
| "Wave / dance / cheer / nod" | Gestures |
| "What is this?" (hold object 20-40 cm from camera) | Photo -> AI describes it and tells facts |
| "Find the red ball and pick it up" | Looks, turns, approaches in small steps (re-checking the camera each step), scoops, verifies |
| "Put it down" | Lowers arms, releases, backs off |
| Emoji | Face changes automatically with the AI's emotion; "show a sleepy face" works too |

## 6a. Eyes, touch sensor and info screen
* The TFT shows animated eyes: blinking, looking around, falling asleep after 90 s idle. Moods: happy, sad,
  angry, surprised, sleepy, love (hearts), wink, thinking. The eyes also follow the AI's emotion while it talks
  (`ROVER_EYES_FOLLOW_AI 0` in `config.h` = touch/web/voice commands only).
* **Touch sensor** (TTP223 on GPIO45): tap = happy/wink/surprised/love (cycling) + a little body wiggle;
  **hold** = love (hearts) while held; **double tap** = clock + date; poking 4 times quickly = grumpy.
* **Live camera video:** say "show me what you see" / "show the camera" (stays 60 s; "show the camera until I
  say stop" keeps it on) and "hide the camera" / "eyes" to go back. The web control page has **Camera view** and
  **Eyes** buttons. Video is 320x240 at roughly 15-25 frames per second; a "what is this?" photo still works while
  it is on (the picture pauses for ~1 s while the sensor switches to sharp VGA). Settings: `ROVER_CAM_PREVIEW_FRAMESIZE`
  in `config.h` (`FRAMESIZE_VGA` = sharper, slower). Camera upside down/mirrored: `camera_hmirror` / `camera_vflip` in `config.json`.
* **Info screen:** the IP address of the control page is shown **centred** for 15 s after Wi-Fi connects. Say
  "show the time" or ask a question and the AI can put a short fact on the screen (`show_info`, `show_clock`).
  The web page has buttons for every eye mood, the clock and the IP.

## 6b. Manual control app (phone / PC browser)
The robot hosts its own control page - nothing to install.
1. Robot and phone on the **same Wi-Fi**. About 5 seconds after the robot connects, the screen shows
   an address like `http://192.168.1.45:8080` (15 s). Or ask: "What is your control address?"
   (you can also find the robot's IP in your router's device list).
2. Open that address in the phone browser. Default PIN is **1234** (type it in the PIN box at the
   bottom if asked). **Change it:** edit `ROVER_WEB_PIN` in `main/boards/rover-bot/config.h`, push, rebuild.
3. The page has: STOP button, drive pad (hold to move, release to stop; W/A/S/D on a PC keyboard),
   speed slider, two tank sliders for the left and right N20 motor separately (spring back to 0),
   three sliders for head / left arm / right arm, pose buttons, gesture buttons, pick up / put down,
   and a Setup box (motor/arm direction, trims) - same settings as the voice calibration.
Safety: every drive command stops by itself after ~0.35 s if the phone stops sending (lost Wi-Fi,
locked screen). Leaving the browser tab also sends STOP. The page is not encrypted and the PIN is
simple - use it only on your own home network.

## 6c. How movement commands work
| You do / say | Robot |
|---|---|
| Hold a web-page arrow | keeps driving while held, stops when released (also if the phone loses Wi-Fi / the tab is hidden) |
| "Go forward" (no distance) | drives continuously until you say **"Stop"** (safety stop after 60 s) |
| "Stop" | stops at once (also web STOP button, or hold BOOT) |
| "Go 30 cm" / "move back 1 metre" | moves that distance, then stops by itself |
| "Turn right 90 degrees" | turns about that angle, then stops |
Tips: motor noise can hide your voice, so keep the chat open and speak close to the mic; the web STOP button
and BOOT long-press always work. **Calibrate the distance once:** say "go 50 cm", measure how far it went,
then set `cm_per_sec` = (current value) x (measured cm / 50) with the Setup box or by voice. Do the same for
`turn_ms_per_90deg` (increase it if turns are too small). Battery level changes the speed a little.

## 7. Limits you should know
* **Picking up:** the arms only swing up/down, there is no gripper. Rover *scoops* light objects
  (ball, plush, plastic cup) onto the arms. For good results fit 3D-printed or cardboard scoop paddles
  with a lip on the arm tips, and use a smooth floor. Tune `ARM_SCOOP_LOW`, `ARM_CARRY`,
  `PICKUP_PUSH_MS` in `config.h`.
* **Approaching** is closed-loop through the cloud vision model: every look takes ~2-4 s, so it is slow
  and approximate, not real-time tracking. Good lighting and a plain floor matter.
* No distance/edge sensors: use on the floor, supervise it, keep it away from stairs.
* Tool calls run on the main loop, so each move call is limited to ~3 s.
* Voice, vision and the AI itself need internet and the xiaozhi.me service.

## 8. Customising
* Pins, servo limits, poses: `main/boards/rover-bot/config.h`.
* Gestures: `rover_robot.h` (keyframe tables). Tools/AI instructions: `rover_tools.h`.
* Different upstream version: copy `main/boards/rover-bot/` into it and run `python tools/add_rover_board.py`.

## Troubleshooting
* Motors do not run -> see the L298N checklist in WIRING.md (remove ENA/ENB caps, wire ENA=19 ENB=20, supply >= 7 V).
* Resets when motors start -> power problem (separate buck, capacitors, common GND).
* No sound -> check MAX98357A SD/GAIN, 5 V supply; mic silent -> INMP441 L/R pin to GND.
* Servo jitter -> shared ground and bigger capacitor on the servo rail.
* White/blank TFT -> check VCC/GND, that CS is tied to GND and RESET/LED to 3V3, and SCK=21, MOSI=47, DC=43. Streaks
  or noise -> `DISPLAY_SPI_HZ` 20 MHz. Upside down -> `-flip` build. Blue skin -> `-rgb` build. Negative -> `-invert` build.
* Video choppy or the voice stutters while video is on -> `hide the camera`; or keep `FRAMESIZE_QVGA` (default).
* Camera init failed -> module is not the standard pinout; edit the CAMERA_PIN_* values.
