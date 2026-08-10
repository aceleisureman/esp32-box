#include "Input.h"
#include "BluetoothA2DP.h"
#include "Display.h"
#include "AudioPlayer.h"

InputClass Input;

namespace {

// 音量键支持长按连发；播放/确认键只在按下瞬间触发一次
constexpr bool kRepeatable = true;
constexpr bool kSingleShot = false;

}  // namespace

void InputClass::init() {
    pinMode(PIN_PLAY,     INPUT_PULLUP);
    pinMode(PIN_VOL_UP,   INPUT_PULLUP);
    pinMode(PIN_VOL_DOWN, INPUT_PULLUP);

#if JOYSTICK_VRX_PIN >= 0
    pinMode(JOYSTICK_VRX_PIN, INPUT);
#endif
#if JOYSTICK_VRY_PIN >= 0
    pinMode(JOYSTICK_VRY_PIN, INPUT);
#endif
#if JOYSTICK_SW_PIN >= 0
    pinMode(JOYSTICK_SW_PIN, INPUT_PULLUP);
#endif

    _play    = {PIN_PLAY,     HIGH, 1, 0, 0};
    _volUp   = {PIN_VOL_UP,   HIGH, 1, 0, 0};
    _volDown = {PIN_VOL_DOWN, HIGH, 1, 0, 0};
#if JOYSTICK_SW_PIN >= 0
    _joyPress = {JOYSTICK_SW_PIN, HIGH, 1, 0, 0};
#else
    _joyPress = {0, HIGH, 1, 0, 0};
#endif
    _joyDirection = 0;
    _joyRepeatMs = 0;

    Serial.printf("[INPUT] joystick VRX=%d VRY=%d SW=%d\n",
                  JOYSTICK_VRX_PIN, JOYSTICK_VRY_PIN, JOYSTICK_SW_PIN);
}

void InputClass::update() {
    static uint32_t scanTick = 0;
    if (millis() - scanTick < 10) return;
    scanTick = millis();

    const bool volUpPressed = digitalRead(PIN_VOL_UP) == LOW;
    const bool volDownPressed = digitalRead(PIN_VOL_DOWN) == LOW;
    const bool bothVolumePressed = volUpPressed && volDownPressed;

    // 双音量键长按 800ms：进入/退出配网页
    if (bothVolumePressed) {
        if (!_comboActive) {
            _comboActive = true;
            _comboTriggered = false;
            _comboMs = millis();
        }
        if (!_comboTriggered && millis() - _comboMs >= 800) {
            const bool wakeOnly = !Display.isBacklightOn();
            Display.noteActivity();
            if (!wakeOnly) {
                if (Display.isNetworkSettings()) Display.exitNetworkSettings();
                else Display.enterNetworkSettings();
            }
            _comboTriggered = true;
        }
        return;
    }

    if (_comboActive) {
        // 组合键结束前屏蔽单独音量动作，避免进入设置时误调音量。
        if (!volUpPressed && !volDownPressed) {
            _comboActive = false;
            _comboTriggered = false;
            _comboMs = 0;
            _volUp = {PIN_VOL_UP, HIGH, 1, millis(), 0};
            _volDown = {PIN_VOL_DOWN, HIGH, 1, millis(), 0};
        }
        return;
    }

    // 统一的按键扫描：消抖 + 首次触发 +（可选）长按连发。
    // 背光熄灭时任意键只唤醒，不执行动作，避免误操作。
    auto scan = [](Btn &b, bool repeatable, void (*action)(void)) {
        const bool raw = digitalRead(b.pin) == LOW;
        const uint32_t now = millis();

        if (raw != (bool)b.state) {
            b.ms = now;
            b.state = raw;
        }

        if (raw) {                 // 释放：复位状态
            b.triggered = 0;
            b.repeatMs = 0;
            return;
        }
        if (now - b.ms <= kDebounceMs) return;   // 抖动窗口内不响应

        const bool wasBacklightOn = Display.isBacklightOn();
        if (!b.triggered) {        // 按下瞬间触发一次
            Display.noteActivity();
            if (wasBacklightOn) action();
            b.triggered = 1;
            b.repeatMs = repeatable ? now + kRepeatDelayMs : 0;
            return;
        }
        if (b.repeatMs && now >= b.repeatMs) {   // 长按连发
            Display.noteActivity();
            if (wasBacklightOn) action();
            b.repeatMs = now + kRepeatRateMs;
        }
    };

#if JOYSTICK_VRX_PIN >= 0 && JOYSTICK_VRY_PIN >= 0
    // 摇杆方向：离开中心死区触发一次，推住不放则进入连发。
    // 带滞回：上次在上方向时，必须读数回到中心附近的回中带才复位，
    // 避免 ADC 漂移/射频干扰在死区边界抖动导致误触发。
    constexpr int JOY_CENTER = 2048;
    constexpr int JOY_HYST   = 500;    // 滞回：比死区宽，回中更"黏"
    constexpr int JOY_LOW    = 1248;   // 上/左方向阈值（中心以下）
    constexpr int JOY_HIGH   = 2848;   // 下/右方向阈值（中心以上）

    const int x = analogRead(JOYSTICK_VRX_PIN);
    const int y = analogRead(JOYSTICK_VRY_PIN);

    // 当前方向：只在明确越过阈值时判定，否则沿用上一个方向（滞回）
    int8_t direction = 0;
    if (_joyDirection == 0) {
        // 空闲态：需要越过死区才判定方向
        if (y < JOY_LOW) direction = 1;       // Up
        else if (y > JOY_HIGH) direction = 2; // Down
        else if (x < JOY_LOW) direction = 3;  // Left
        else if (x > JOY_HIGH) direction = 4; // Right
    } else {
        // 已有方向：必须回到中心附近的回中带才清空
        const int cx = x - JOY_CENTER;
        const int cy = y - JOY_CENTER;
        const bool nearCenter = (cx < JOY_HYST && cx > -JOY_HYST) &&
                                (cy < JOY_HYST && cy > -JOY_HYST);
        direction = nearCenter ? 0 : _joyDirection;
    }

    const uint32_t now = millis();
    if (direction == 0) {
        _joyRepeatMs = 0;                 // 回中：复位连发
    } else if (direction != _joyDirection) {
        // 新方向：立即触发一次，并起算连发首次延迟
        const bool wakeOnly = !Display.isBacklightOn();
        Display.noteActivity();
        if (!wakeOnly) {
            Display.handleJoystick((DisplayClass::JoystickEvent)(direction - 1));
        }
        _joyRepeatMs = now + kRepeatDelayMs;
    } else if (_joyRepeatMs && now >= _joyRepeatMs) {
        // 保持同方向：按固定间隔连发
        const bool wakeOnly = !Display.isBacklightOn();
        Display.noteActivity();
        if (!wakeOnly) {
            Display.handleJoystick((DisplayClass::JoystickEvent)(direction - 1));
        }
        _joyRepeatMs = now + kRepeatRateMs;
    }
    _joyDirection = direction;
#endif

    // ---- 功能键：配网页里复用为导航键，其余场合控制音频播放 ----
    scan(_play, kSingleShot, []() {
        if (Display.isNetworkSettings()) Display.networkActivate();
        else AudioPlayer.togglePause();
    });
    scan(_volUp, kRepeatable, []() {
        if (Display.isNetworkSettings()) Display.networkMove(-1);
        else AudioPlayer.volumeUp();
    });
    scan(_volDown, kRepeatable, []() {
        if (Display.isNetworkSettings()) Display.networkMove(1);
        else AudioPlayer.volumeDown();
    });

#if JOYSTICK_SW_PIN >= 0
    scan(_joyPress, kSingleShot, []() {
        Display.handleJoystick(DisplayClass::JoystickEvent::Press);
    });
#endif
}
