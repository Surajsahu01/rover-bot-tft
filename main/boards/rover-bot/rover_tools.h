// rover_tools.h - registers the MCP tools that let the Xiaozhi AI drive the robot.
// The tool descriptions are what the language model reads, so they are written
// as instructions for it.
#pragma once

#include <string>

#include <esp_log.h>

#include "board.h"
#include "mcp_server.h"
#include "rover_robot.h"
#include "rover_eyes.h"
#include "rover_web.h"

class RoverTools {
public:
    RoverTools(RoverRobot* robot, Display* display, RoverEyes* eyes)
        : robot_(robot), display_(display), eyes_(eyes) {
        Register();
    }

private:
    RoverRobot* robot_;
    Display* display_;
    RoverEyes* eyes_;

    void Register() {
        auto& mcp = McpServer::GetInstance();

        mcp.AddTool(
            "self.robot.move",
            "Drive straight for an EXACT distance and then stop by itself. Use it when the user "
            "gives a distance, e.g. 'go 30 cm', 'move back half a metre' (50 cm). The robot does "
            "not need a stop command afterwards. For 'go forward' WITHOUT a distance use "
            "self.robot.drive_continuous instead.\n"
            "Args:\n"
            "  `direction`: 'forward' or 'backward'\n"
            "  `distance_cm`: distance in centimetres, 1-1000\n"
            "  `speed`: 30-100 (default 70)\n"
            "  `wait`: true = return when finished (default). Long moves return immediately "
            "and finish on their own.",
            PropertyList({Property("direction", kPropertyTypeString),
                          Property("distance_cm", kPropertyTypeInteger, 10, 1, 1000),
                          Property("speed", kPropertyTypeInteger, 70, 30, 100),
                          Property("wait", kPropertyTypeBoolean, true)}),
            [this](const PropertyList& p) -> ToolResult {
                std::string dir = p["direction"].value<std::string>();
                if (dir != "forward" && dir != "backward") {
                    return std::unexpected("direction must be 'forward' or 'backward'");
                }
                int cm = p["distance_cm"].value<int>();
                int speed = p["speed"].value<int>();
                int est = robot_->EstimateMoveMs(cm, speed);
                // Tool calls run on the main loop: only block for short moves.
                bool wait = p["wait"].value<bool>() && est <= 2500;
                int ms = robot_->Move(dir == "forward", cm, speed, wait);
                return std::string("{\"ok\":true,\"distance_cm\":") + std::to_string(cm) +
                       ",\"duration_ms\":" + std::to_string(ms) +
                       (wait ? ",\"finished\":true}" : ",\"finished\":false,\"note\":\"moving, stops by itself\"}");
            });

        mcp.AddTool(
            "self.robot.drive_continuous",
            "Start driving and KEEP driving until self.robot.stop is called. Use it when the "
            "user says 'go forward', 'move back', 'turn left', 'keep going' without a distance. "
            "Return immediately and keep talking; when the user says stop, halt, freeze or "
            "'that's enough', call self.robot.stop at once. Safety: it stops by itself after "
            "`max_seconds`.\n"
            "Args:\n"
            "  `direction`: 'forward', 'backward', 'left' or 'right' (left/right spin on the spot)\n"
            "  `speed`: 30-100 (default 70)\n"
            "  `max_seconds`: safety limit, 1-600 (default 60)",
            PropertyList({Property("direction", kPropertyTypeString),
                          Property("speed", kPropertyTypeInteger, 70, 30, 100),
                          Property("max_seconds", kPropertyTypeInteger, 60, 1, 600)}),
            [this](const PropertyList& p) -> ToolResult {
                std::string d = p["direction"].value<std::string>();
                int dir = d == "forward" ? 0 : d == "backward" ? 1 : d == "left" ? 2 : d == "right" ? 3 : -1;
                if (dir < 0) return std::unexpected("direction must be forward, backward, left or right");
                robot_->StartContinuous(dir, p["speed"].value<int>(), p["max_seconds"].value<int>());
                return std::string("{\"ok\":true,\"moving\":true}");
            });

        mcp.AddTool(
            "self.robot.turn",
            "Rotate the robot on the spot to the left or right (counter-clockwise / clockwise "
            "seen from above). Use this to centre an object in the camera picture: an object "
            "on the left of the picture needs a left turn. Typical correction: 10-30 degrees.\n"
            "Args:\n"
            "  `direction`: 'left' or 'right'\n"
            "  `degrees`: approximate angle, 5-720 (default 30)\n"
            "  `speed`: 40-100 (default 70)\n"
            "  `wait`: true = return when finished (default)",
            PropertyList({Property("direction", kPropertyTypeString),
                          Property("degrees", kPropertyTypeInteger, 30, 5, 720),
                          Property("speed", kPropertyTypeInteger, 70, 40, 100),
                          Property("wait", kPropertyTypeBoolean, true)}),
            [this](const PropertyList& p) -> ToolResult {
                std::string dir = p["direction"].value<std::string>();
                if (dir != "left" && dir != "right") {
                    return std::unexpected("direction must be 'left' or 'right'");
                }
                int deg = p["degrees"].value<int>(), spd = p["speed"].value<int>();
                bool wait = p["wait"].value<bool>() && deg <= 360;
                int ms = robot_->Turn(dir == "left", deg, spd, wait);
                return std::string("{\"ok\":true,\"turned_ms\":") + std::to_string(ms) + "}";
            });

        mcp.AddTool("self.robot.stop",
                    "Immediately stop the wheels. Call this if the user says stop, halt, freeze "
                    "or if something looks dangerous.",
                    PropertyList(), [this](const PropertyList&) -> ToolResult {
                        robot_->Stop();
                        return true;
                    });

        mcp.AddTool(
            "self.robot.set_pose",
            "Move the head and/or arms to exact angles. Pass -1 to leave a part unchanged.\n"
            "Args:\n"
            "  `head`: 45-135 (90 = looking straight ahead)\n"
            "  `left_arm`, `right_arm`: 0-180 (0 = hanging down, 90 = pointing forward, "
            "180 = straight up)\n"
            "  `speed`: degrees per second, 10-300 (default 120)\n"
            "  `wait`: wait until the pose is reached (default false)\n"
            "Do not raise the arms while carrying an object unless asked to put it down.",
            PropertyList({Property("head", kPropertyTypeInteger, -1, -1, 135),
                          Property("left_arm", kPropertyTypeInteger, -1, -1, 180),
                          Property("right_arm", kPropertyTypeInteger, -1, -1, 180),
                          Property("speed", kPropertyTypeInteger, 120, 10, 300),
                          Property("wait", kPropertyTypeBoolean, false)}),
            [this](const PropertyList& p) -> ToolResult {
                robot_->SetPose(p["head"].value<int>(), p["left_arm"].value<int>(),
                                p["right_arm"].value<int>(), p["speed"].value<int>(),
                                p["wait"].value<bool>());
                return true;
            });

        mcp.AddTool(
            "self.robot.gesture",
            "Play a body-language gesture with head and arms. Call it (with wait=false) at the "
            "same moment you start speaking so the movement is in sync with your words.\n"
            "Args:\n"
            "  `name`: one of wave, nod, shake_head, happy, sad, surprised, think, cheer, "
            "dance, hug, shy\n"
            "  `wait`: wait for the gesture to finish (default false)\n"
            "Examples: greeting -> wave; saying yes -> nod; saying no -> shake_head; good "
            "news -> cheer; asked to dance -> dance.",
            PropertyList({Property("name", kPropertyTypeString),
                          Property("wait", kPropertyTypeBoolean, false)}),
            [this](const PropertyList& p) -> ToolResult {
                int g = RoverRobot::GestureFromName(p["name"].value<std::string>());
                if (g < 0) {
                    return std::unexpected(
                        "unknown gesture; use wave, nod, shake_head, happy, sad, surprised, "
                        "think, cheer, dance, hug or shy");
                }
                robot_->PlayGesture(g, p["wait"].value<bool>());
                return true;
            });

        mcp.AddTool(
            "self.robot.pickup_object",
            "Pick up the object directly in front of the robot by scooping it with both arms "
            "and lifting. ONLY call this when a fresh camera check shows the object is centred "
            "and very close (about 5-10 cm in front of the robot, filling the lower middle of "
            "the picture). Works for light, small items (ball, toy, cup of plastic, plush). "
            "Afterwards take a photo to verify the object is really on the arms and tell the "
            "user honestly whether it worked.",
            PropertyList(), [this](const PropertyList&) -> ToolResult {
                robot_->PickUp();
                return std::string("{\"ok\":true,\"note\":\"scoop sequence done; verify with the camera\"}");
            });

        mcp.AddTool("self.robot.put_down_object",
                    "Lower the arms, release the carried object on the floor, back off a little "
                    "and return to the rest pose.",
                    PropertyList(), [this](const PropertyList&) -> ToolResult {
                        robot_->PutDown();
                        return true;
                    });

        mcp.AddTool(
            "self.robot.show_emotion",
            "Change the expression of the robot's EYES (the screen shows eyes, not emoji). Args: `emotion`: one of neutral, happy, "
            "laughing, funny, sad, angry, crying, loving, embarrassed, surprised, shocked, "
            "thinking, winking, cool, relaxed, delicious, kissy, confident, sleepy, silly, "
            "confused",
            PropertyList({Property("emotion", kPropertyTypeString)}),
            [this](const PropertyList& p) -> ToolResult {
                if (eyes_) eyes_->SetEmotionByName(p["emotion"].value<std::string>().c_str(), 5000);
                return true;
            });

        mcp.AddTool(
            "self.robot.show_info",
            "Show a short piece of information on the robot's small screen, centred and "
            "word-wrapped (about 18 letters per line, max 4 lines), e.g. a key fact, a number, "
            "a short answer or a reminder. Plain ASCII letters and digits only. The eyes return "
            "afterwards.\nArgs:\n  `text`: the text to show\n  `seconds`: 2-60 (default 8)",
            PropertyList({Property("text", kPropertyTypeString),
                          Property("seconds", kPropertyTypeInteger, 8, 2, 60)}),
            [this](const PropertyList& p) -> ToolResult {
                if (!eyes_) return std::unexpected("no screen");
                eyes_->ShowText(p["text"].value<std::string>(), p["seconds"].value<int>() * 1000);
                return true;
            });

        mcp.AddTool("self.robot.show_clock",
                    "Show the current time and date on the screen for 8 seconds.",
                    PropertyList(), [this](const PropertyList&) -> ToolResult {
                        if (!eyes_) return std::unexpected("no screen");
                        eyes_->ShowClock(8000);
                        return true;
                    });

        mcp.AddTool(
            "self.robot.show_camera",
            "Show the LIVE VIDEO of the robot's own camera on its screen (the eyes are hidden "
            "meanwhile). Use it when the user says 'show me what you see', 'show the camera' or "
            "'turn on the camera view'. Taking a photo for 'what is this?' still works while it "
            "is on.\nArgs:\n  `seconds`: 5-600, or 0 = until hide_camera is called (default 60)",
            PropertyList({Property("seconds", kPropertyTypeInteger, 60, 0, 600)}),
            [this](const PropertyList& p) -> ToolResult {
                if (!eyes_) return std::unexpected("no screen");
                eyes_->ShowCamera(p["seconds"].value<int>() * 1000);
                return true;
            });

        mcp.AddTool("self.robot.hide_camera",
                    "Stop the live camera video on the screen and show the eyes again.",
                    PropertyList(), [this](const PropertyList&) -> ToolResult {
                        if (!eyes_) return std::unexpected("no screen");
                        eyes_->HideCamera();
                        return true;
                    });

        mcp.AddTool("self.robot.get_control_url",
                    "Get the web address of the manual control app (open it in a phone browser "
                    "on the same Wi-Fi to drive the wheels and move the servos by hand). Tell "
                    "the user the address; the PIN is set in the firmware.",
                    PropertyList(), [](const PropertyList&) -> ToolResult {
                        std::string u = RoverWeb::CurrentUrl();
                        if (u.empty()) return std::unexpected("not connected to Wi-Fi yet");
                        return u;
                    });

        mcp.AddTool("self.robot.get_status",
                    "Get the robot's state: moving or not, carrying an object, and the angles of "
                    "head and arms.",
                    PropertyList(), [this](const PropertyList&) -> ToolResult {
                        return robot_->StatusJson();
                    });

        mcp.AddTool(
            "self.robot.calibrate",
            "SETUP ONLY - use only when the owner explicitly asks to calibrate or fix the "
            "robot's movement. The value is saved permanently.\n"
            "Args:\n"
            "  `setting`: motor_left_invert | motor_right_invert | swap_motors | "
            "left_arm_invert | right_arm_invert (value 0 or 1); head_trim | left_arm_trim | "
            "right_arm_trim (value -30..30 degrees); turn_ms_per_90deg (100-3000); "
            "cm_per_sec (2-80, forward speed at speed 70)\n"
            "  `value`: integer",
            PropertyList({Property("setting", kPropertyTypeString),
                          Property("value", kPropertyTypeInteger, -3000, 3000)}),
            [this](const PropertyList& p) -> ToolResult {
                if (!robot_->SetCalibration(p["setting"].value<std::string>(),
                                            p["value"].value<int>())) {
                    return std::unexpected("unknown setting");
                }
                return robot_->CalibrationJson();
            });

        mcp.AddTool("self.robot.get_calibration",
                    "Read the saved calibration values.", PropertyList(),
                    [this](const PropertyList&) -> ToolResult { return robot_->CalibrationJson(); });

        mcp.AddTool(
            "self.robot.self_test",
            "SETUP ONLY - run a short hardware test: servos sweep, then the robot drives "
            "forward, backward, turns left and turns right (each ~0.5 s). Make sure there is "
            "free floor space around the robot before calling.",
            PropertyList(), [this](const PropertyList&) -> ToolResult {
                robot_->SetPose(60, 90, 90, 120, true);
                robot_->SetPose(120, 10, 10, 120, true);
                robot_->SetPose(90, 10, 10, 120, true);
                robot_->Move(true, 6, 70, true);
                vTaskDelay(pdMS_TO_TICKS(250));
                robot_->Move(false, 6, 70, true);
                vTaskDelay(pdMS_TO_TICKS(250));
                robot_->Turn(true, 40, 70, true);
                vTaskDelay(pdMS_TO_TICKS(250));
                robot_->Turn(false, 40, 70, true);
                return std::string("{\"ok\":true,\"expected\":\"forward, backward, left turn, right turn\"}");
            });
    }
};
