// rover_eyes.h - the TFT screen is a private "face" for the desk buddy.
// Xiaozhi's own UI does not draw on it. This file draws animated eyes (blink, look
// around, 9 moods), centred info pages (IP address, clock, any text) and reacts to a
// touch sensor (TTP223 or similar, output HIGH when touched).
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <driver/gpio.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_io.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "config.h"
#include "rover_font.h"
#include "rover_tft.h"

class RoverEyes {
public:
    enum Mood { kNeutral, kHappy, kSad, kAngry, kSurprised, kSleepy, kLove, kWink, kThinking };

    // called on touch events so the body can react too: "happy" (tap) or "loving" (hold)
    std::function<void(const char*)> on_touch_emotion;

    explicit RoverEyes(RoverTft* tft) : tft_(tft) {
        memset(fb_, 0, sizeof(fb_));
        memset(last_, 0xFF, sizeof(last_));
        xTaskCreate(&RoverEyes::RenderThunk, "eyes", 6144, this, 2, nullptr);
#if ROVER_TOUCH_GPIO >= 0
        gpio_config_t io = {};
        io.pin_bit_mask = 1ULL << ROVER_TOUCH_GPIO;
        io.mode = GPIO_MODE_INPUT;
        io.pull_down_en = GPIO_PULLDOWN_ENABLE;
        gpio_config(&io);
        xTaskCreate(&RoverEyes::TouchThunk, "touch", 3072, this, 3, nullptr);
#endif
    }

    // ------------------------------------------------------------------ public API
    void SetMood(Mood m, int hold_ms) {
        std::lock_guard<std::mutex> g(mu_);
        mood_ = m;
        mood_until_ms_ = hold_ms > 0 ? NowMs() + hold_ms : 0;
        last_activity_ms_ = NowMs();
    }

    // Emotion name (the same names Xiaozhi uses) -> mood. Returns false for unknown names.
    static bool MoodFromName(const std::string& s, Mood& m) {
        if (s == "neutral") m = kNeutral;
        else if (s == "happy" || s == "laughing" || s == "funny" || s == "silly" || s == "cool" ||
                 s == "confident" || s == "relaxed" || s == "delicious" || s == "embarrassed") m = kHappy;
        else if (s == "loving" || s == "kissy" || s == "love") m = kLove;
        else if (s == "winking" || s == "wink") m = kWink;
        else if (s == "sad" || s == "crying") m = kSad;
        else if (s == "angry") m = kAngry;
        else if (s == "surprised" || s == "shocked") m = kSurprised;
        else if (s == "thinking" || s == "confused") m = kThinking;
        else if (s == "sleepy") m = kSleepy;
        else return false;
        return true;
    }

    // Show an emotion by name for `ms` milliseconds (used by the AI tool and the web page).
    bool SetEmotionByName(const char* e, int ms = 4000) {
        Mood m;
        if (!e || !MoodFromName(e, m)) return false;
        if (m == kNeutral) { SetMood(kNeutral, 0); return true; }
        SetMood(m, ms);
        return true;
    }

    // Emotion chosen by the AI for the sentence it is speaking (4 s).
    void SetEmotionFromAI(const char* e) {
#if ROVER_EYES_FOLLOW_AI
        if (!e) return;
        Mood m;
        if (!MoodFromName(e, m)) return;
        if (m == kNeutral) {
            std::lock_guard<std::mutex> g(mu_);
            if (NowMs() > touch_mood_until_ms_) { mood_ = kNeutral; mood_until_ms_ = 0; }
            return;
        }
        SetMood(m, 4000);
#else
        (void)e;
#endif
    }

    // Live camera video on the screen (ms = 0: until HideCamera). The eyes pause meanwhile.
    void ShowCamera(int ms) { tft_->ShowCamera(ms); }
    void HideCamera() { tft_->HideCamera(); }

    // Centred info page. Each line has a font (0 small, 1 medium, 2 large) and text.
    struct Line { int font; std::string text; };
    void ShowLines(const std::vector<Line>& lines, int ms) {
        std::lock_guard<std::mutex> g(mu_);
        info_ = lines;
        info_until_ms_ = NowMs() + ms;
        clock_page_ = false;
        last_activity_ms_ = NowMs();
    }

    void ShowAddress(const std::string& url, int ms) {
        std::string a = url;
        if (a.rfind("http://", 0) == 0) a = a.substr(7);
        ShowLines({{0, "OPEN IN BROWSER"}, {1, a}, {0, "SAME WI-FI"}}, ms);
    }

    // Free text, word-wrapped to the width of the screen (max 4 lines).
    void ShowText(const std::string& text, int ms) {
        const size_t cols = 18;
        std::vector<Line> lines;
        std::string line, word;
        auto flush_word = [&]() {
            while (word.size() > cols) {
                if (!line.empty()) { lines.push_back({1, line}); line.clear(); }
                lines.push_back({1, word.substr(0, cols)});
                word = word.substr(cols);
            }
            if (line.empty()) line = word;
            else if (line.size() + 1 + word.size() <= cols) line += " " + word;
            else { lines.push_back({1, line}); line = word; }
            word.clear();
        };
        for (char c : text) {
            if (c == ' ' || c == '\n') { if (!word.empty()) flush_word(); }
            else word += c;
        }
        if (!word.empty()) flush_word();
        if (!line.empty()) lines.push_back({1, line});
        if (lines.size() > 4) {
            lines.resize(4);
            lines[3].text = lines[3].text.substr(0, cols - 2) + "..";
        }
        ShowLines(lines, ms);
    }

    void ShowClock(int ms) {
        std::lock_guard<std::mutex> g(mu_);
        info_.clear();
        info_until_ms_ = NowMs() + ms;
        clock_page_ = true;
        last_activity_ms_ = NowMs();
    }

    void ClearInfo() {
        std::lock_guard<std::mutex> g(mu_);
        info_until_ms_ = 0;
    }

    void SetAddress(const std::string& url) {
        std::lock_guard<std::mutex> g(mu_);
        address_ = url.rfind("http://", 0) == 0 ? url.substr(7) : url;
    }

private:
    RoverTft* tft_;
    std::mutex mu_;
    uint8_t fb_[128 * 8];    // vertical-tiled 1 bpp frame (logical 128x64, scaled up onto the TFT)
    uint8_t last_[128 * 8];

    Mood mood_ = kNeutral;
    int64_t mood_until_ms_ = 0;
    int64_t touch_mood_until_ms_ = 0;
    int64_t last_activity_ms_ = 0;
    std::vector<Line> info_;
    int64_t info_until_ms_ = 0;
    bool clock_page_ = false;
    std::string address_;

    // animated eye parameters (current values chase the targets)
    float w_ = 34, h_ = 34, dx_ = 0, dy_ = 0;
    float angry_ = 0, sad_ = 0, happy_ = 0, love_ = 0, wink_ = 0, blink_ = 0;
    int64_t next_blink_ms_ = 0, blink_end_ms_ = 0, next_look_ms_ = 0;
    float look_x_ = 0, look_y_ = 0;
    float pulse_ = 0;

    static int64_t NowMs() { return esp_timer_get_time() / 1000; }
    static float Approach(float cur, float target, float k) { return cur + (target - cur) * k; }
    static int Rnd(int lo, int hi) { return lo + (int)(esp_random() % (uint32_t)(hi - lo + 1)); }

    // ------------------------------------------------------------------ pixel ops
    void Px(int x, int y, bool on) {
        if (x < 0 || x >= 128 || y < 0 || y >= 64) return;
        uint8_t& b = fb_[(y >> 3) * 128 + x];
        if (on) b |= (1 << (y & 7)); else b &= ~(1 << (y & 7));
    }

    void FillRoundRect(int x, int y, int w, int h, int r, bool on) {
        r = std::max(0, std::min(r, std::min(w, h) / 2));
        for (int j = 0; j < h; j++) {
            for (int i = 0; i < w; i++) {
                int cx = i < r ? r - i - 1 : (i >= w - r ? i - (w - r) : -1);
                int cy = j < r ? r - j - 1 : (j >= h - r ? j - (h - r) : -1);
                if (cx >= 0 && cy >= 0 && (cx * cx + cy * cy) >= r * r) continue;
                Px(x + i, y + j, on);
            }
        }
    }

    void FillEllipse(float cx, float cy, float rx, float ry, bool on) {
        for (int y = (int)(cy - ry) - 1; y <= (int)(cy + ry) + 1; y++) {
            for (int x = (int)(cx - rx) - 1; x <= (int)(cx + rx) + 1; x++) {
                float u = (x - cx) / rx, v = (y - cy) / ry;
                if (u * u + v * v <= 1.0f) Px(x, y, on);
            }
        }
    }

    static float Edge(float ax, float ay, float bx, float by, float px, float py) {
        return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
    }
    void FillTriangle(float ax, float ay, float bx, float by, float cx, float cy, bool on) {
        int x0 = (int)std::floor(std::min({ax, bx, cx})), x1 = (int)std::ceil(std::max({ax, bx, cx}));
        int y0 = (int)std::floor(std::min({ay, by, cy})), y1 = (int)std::ceil(std::max({ay, by, cy}));
        for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) {
                float e1 = Edge(ax, ay, bx, by, x + .5f, y + .5f);
                float e2 = Edge(bx, by, cx, cy, x + .5f, y + .5f);
                float e3 = Edge(cx, cy, ax, ay, x + .5f, y + .5f);
                if ((e1 >= 0 && e2 >= 0 && e3 >= 0) || (e1 <= 0 && e2 <= 0 && e3 <= 0)) Px(x, y, on);
            }
        }
    }

    void FillHeart(float cx, float cy, float size) {
        float half = size / 2;
        for (int y = (int)(cy - half) - 1; y <= (int)(cy + half) + 1; y++) {
            for (int x = (int)(cx - half) - 1; x <= (int)(cx + half) + 1; x++) {
                float u = (x - cx) / half * 1.15f;
                float v = -((y - cy) / half) * 1.15f + 0.15f;
                float a = u * u + v * v - 1.0f;
                if (a * a * a - u * u * v * v * v <= 0) Px(x, y, true);
            }
        }
    }

    const RoverFont& Font(int id) const { return id == 2 ? kFontL : (id == 1 ? kFontM : kFontS); }

    int TextWidth(const RoverFont& f, const std::string& s) const { return (int)s.size() * f.w; }

    void DrawText(const RoverFont& f, int x, int y, const std::string& s) {
        for (char ch : s) {
            const char* p = (const char*)memchr(f.chars, ch, f.count);
            if (p) {
                int idx = (int)(p - f.chars);
                for (int j = 0; j < f.h; j++) {
                    uint32_t bits = f.rows[idx * f.h + j];
                    for (int i = 0; i < f.w; i++) {
                        if ((bits >> (f.w - 1 - i)) & 1) Px(x + i, y + j, true);
                    }
                }
            }
            x += f.w;
        }
    }

    // ------------------------------------------------------------------ info pages
    void DrawLinesCentered(const std::vector<Line>& lines) {
        const int gap = 4;
        int total = 0;
        for (auto& l : lines) total += Font(l.font).h;
        if (lines.size() > 1) total += gap * ((int)lines.size() - 1);
        int y = std::max(0, (64 - total) / 2);
        for (auto& l : lines) {
            const RoverFont& f = Font(l.font);
            int x = std::max(0, (128 - TextWidth(f, l.text)) / 2);
            DrawText(f, x, y, l.text);
            y += f.h + gap;
        }
    }

    void DrawClockPage(const std::string& addr) {
        time_t now = time(nullptr);
        std::vector<Line> lines;
        if (now > 1700000000) {
            struct tm t;
            localtime_r(&now, &t);
            char hm[8], date[24];
            strftime(hm, sizeof(hm), "%H:%M", &t);
            strftime(date, sizeof(date), "%a %d %b", &t);
            std::string d(date);
            for (auto& c : d) c = (char)toupper((unsigned char)c);
            lines.push_back({2, hm});
            lines.push_back({1, d});
        } else {
            lines.push_back({1, "TIME NOT SET"});
        }
        if (!addr.empty()) lines.push_back({0, addr});
        DrawLinesCentered(lines);
    }

    // ------------------------------------------------------------------ eyes
    void DrawEye(int side, float cx, float cy, float w, float h) {
        float hh = std::max(3.0f, h * (1.0f - blink_));
        if (side > 0 && wink_ > 0.01f) hh = std::max(3.0f, hh * (1.0f - wink_) + 3.0f * wink_);
        int x = (int)(cx - w / 2), y = (int)(cy - hh / 2);
        FillRoundRect(x, y, (int)w, (int)hh, (int)std::min(10.0f, hh / 2), true);
        float x1 = x + w, y1 = y + hh;
        if (angry_ > 0.02f) {  // inner ends low  "\ /"
            float lid = angry_ * hh * 0.55f;
            if (side < 0) FillTriangle(x - 1, y - 1, x1 + 1, y - 1, x1 + 1, y + lid, false);
            else FillTriangle(x - 1, y - 1, x1 + 1, y - 1, x - 1, y + lid, false);
        }
        if (sad_ > 0.02f) {  // outer ends low  "/ \"
            float lid = sad_ * hh * 0.5f;
            if (side < 0) FillTriangle(x - 1, y - 1, x1 + 1, y - 1, x - 1, y + lid, false);
            else FillTriangle(x - 1, y - 1, x1 + 1, y - 1, x1 + 1, y + lid, false);
        }
        if (happy_ > 0.02f) {  // cheeks push up from below -> "^ ^"
            FillEllipse(cx, y1 + hh * 0.18f + (1.0f - happy_) * hh, w * 0.62f, hh * 0.62f, false);
        }
    }

    void DrawEyes(int64_t now) {
        Mood m;
        {
            std::lock_guard<std::mutex> g(mu_);
            if (mood_until_ms_ && now > mood_until_ms_) { mood_ = kNeutral; mood_until_ms_ = 0; }
            // fall asleep after 90 s without any activity
            if (mood_ == kNeutral && now - last_activity_ms_ > 90000) mood_ = kSleepy, mood_until_ms_ = 0;
            m = mood_;
        }
        // targets for this mood
        float tw = 34, th = 34, tang = 0, tsad = 0, thap = 0, tlove = 0, twink = 0;
        float lx = look_x_, ly = look_y_;
        switch (m) {
            case kHappy: thap = 1; break;
            case kSad: tw = 32; th = 30; tsad = 1; ly = 4; break;
            case kAngry: tw = 36; th = 28; tang = 1; lx = 0; ly = 0; break;
            case kSurprised: tw = 40; th = 46; lx = 0; ly = 0; break;
            case kSleepy: th = 8; lx = 0; ly = 3; break;
            case kLove: tlove = 1; break;
            case kWink: twink = 1; break;
            case kThinking: lx = -10; ly = -8; break;
            default: break;
        }
        if (m == kNeutral &&
            Application::GetInstance().GetDeviceState() == kDeviceStateListening) {
            tw = 38; th = 40; lx = 0; ly = 0;  // wide-eyed while listening
        }

        // idle behaviour: random blink and look-around
        if (m == kNeutral || m == kHappy || m == kWink) {
            if (now >= next_blink_ms_) {
                blink_end_ms_ = now + 120;
                next_blink_ms_ = now + Rnd(2000, 6000);
            }
            if (now >= next_look_ms_) {
                look_x_ = (float)Rnd(-12, 12);
                look_y_ = (float)Rnd(-6, 6);
                if (Rnd(0, 2) == 0) look_x_ = look_y_ = 0;
                next_look_ms_ = now + Rnd(1500, 4000);
            }
        }
        float tblink = (now < blink_end_ms_ && m != kSleepy) ? 1.0f : 0.0f;

        w_ = Approach(w_, tw, 0.35f);
        h_ = Approach(h_, th, 0.35f);
        dx_ = Approach(dx_, lx, 0.25f);
        dy_ = Approach(dy_, ly, 0.25f);
        angry_ = Approach(angry_, tang, 0.35f);
        sad_ = Approach(sad_, tsad, 0.35f);
        happy_ = Approach(happy_, thap, 0.35f);
        love_ = Approach(love_, tlove, 0.3f);
        wink_ = Approach(wink_, twink, 0.4f);
        blink_ = Approach(blink_, tblink, 0.6f);
        pulse_ += 0.25f;

        const float cy = 32 + dy_;
        const float cxl = 34 + dx_, cxr = 94 + dx_;
        if (love_ > 0.5f) {
            float s = 36 + 4 * sinf(pulse_);
            FillHeart(cxl, cy, s);
            FillHeart(cxr, cy, s);
        } else {
            DrawEye(-1, cxl, cy, w_, h_);
            DrawEye(+1, cxr, cy, w_, h_);
        }
    }

    // ------------------------------------------------------------------ render loop
    static void RenderThunk(void* arg) { static_cast<RoverEyes*>(arg)->RenderLoop(); }

    void RenderLoop() {
        next_blink_ms_ = NowMs() + 2500;
        next_look_ms_ = NowMs() + 2000;
        last_activity_ms_ = NowMs();
        while (true) {
            int64_t now = NowMs();
            if (tft_->CameraActive()) {  // live video owns the screen
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            if (tft_->TakeRepaint()) {  // start-up or camera view just ended
                tft_->Clear();
                memset(last_, 0xFF, sizeof(last_));
            }
            memset(fb_, 0, sizeof(fb_));
            bool info = false, clock = false;
            std::vector<Line> lines;
            std::string addr;
            {
                std::lock_guard<std::mutex> g(mu_);
                if (info_until_ms_ && now < info_until_ms_) {
                    info = true;
                    clock = clock_page_;
                    lines = info_;
                    addr = address_;
                } else if (info_until_ms_) {
                    info_until_ms_ = 0;
                    last_activity_ms_ = now;
                }
            }
            if (info) {
                if (clock) DrawClockPage(addr);
                else DrawLinesCentered(lines);
            } else {
                DrawEyes(now);
            }
            if (memcmp(fb_, last_, sizeof(fb_)) != 0) {  // only talk to the screen when the picture changed
                tft_->DrawMono(fb_);
                memcpy(last_, fb_, sizeof(fb_));
            }
            vTaskDelay(pdMS_TO_TICKS(info ? 250 : 50));
        }
    }

    // ------------------------------------------------------------------ touch sensor
    static void TouchThunk(void* arg) { static_cast<RoverEyes*>(arg)->TouchLoop(); }

    void Emote(const char* name) {
        if (on_touch_emotion) on_touch_emotion(name);
    }

    void TouchLoop() {
#if ROVER_TOUCH_GPIO >= 0
        bool prev = false, long_fired = false, pending_single = false;
        int64_t press_ms = 0, pending_deadline = 0, last_tap_ms = 0;
        int64_t taps[4] = {0, 0, 0, 0};
        int tap_idx = 0, cycle = 0;
        int stable = 0;
        bool raw_prev = false;
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(20));
            int64_t now = NowMs();
            bool raw = gpio_get_level((gpio_num_t)ROVER_TOUCH_GPIO) == 1;
            stable = (raw == raw_prev) ? stable + 1 : 0;
            raw_prev = raw;
            bool t = (stable >= 2) ? raw : prev;  // 40 ms debounce

            if (t && !prev) {
                press_ms = now;
                long_fired = false;
                { std::lock_guard<std::mutex> g(mu_); last_activity_ms_ = now; if (mood_ == kSleepy) { mood_ = kNeutral; } }
            }
            if (t && prev && !long_fired && now - press_ms >= 900) {  // hold -> love
                long_fired = true;
                pending_single = false;
                SetMood(kLove, 600000);
                { std::lock_guard<std::mutex> g(mu_); touch_mood_until_ms_ = now + 600000; }
                Emote("loving");
            }
            if (!t && prev) {  // released
                if (long_fired) {
                    SetMood(kLove, 1500);
                    std::lock_guard<std::mutex> g(mu_);
                    touch_mood_until_ms_ = now + 1500;
                } else {
                    taps[tap_idx++ & 3] = now;
                    int recent = 0;
                    for (int i = 0; i < 4; i++) if (taps[i] && now - taps[i] < 3000) recent++;
                    if (recent >= 4) {  // too much poking -> grumpy
                        memset(taps, 0, sizeof(taps));
                        pending_single = false;
                        SetMood(kAngry, 3000);
                        { std::lock_guard<std::mutex> g(mu_); touch_mood_until_ms_ = now + 3000; }
                    } else if (pending_single && now - last_tap_ms < 450) {  // double tap -> clock
                        pending_single = false;
                        ShowClock(6000);
                    } else {
                        pending_single = true;
                        pending_deadline = now + 450;
                    }
                    last_tap_ms = now;
                }
            }
            if (pending_single && now >= pending_deadline) {  // single tap
                pending_single = false;
                static const Mood kCycle[] = {kHappy, kWink, kSurprised, kHappy, kLove};
                Mood m = kCycle[cycle++ % 5];
                SetMood(m, 2500);
                { std::lock_guard<std::mutex> g(mu_); touch_mood_until_ms_ = now + 2500; }
                Emote(m == kLove ? "loving" : "happy");
            }
            prev = t;
        }
#endif
    }
};
