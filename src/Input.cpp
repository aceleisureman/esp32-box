#include "Input.h"
#include "BluetoothA2DP.h"
#include "Display.h"

InputClass Input;

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

    _play    = {PIN_PLAY,     HIGH, 1, 0};
    _volUp   = {PIN_VOL_UP,   HIGH, 1, 0};
    _volDown = {PIN_VOL_DOWN, HIGH, 1, 0};
#if JOYSTICK_SW_PIN >= 0
    _joyPress = {JOYSTICK_SW_PIN, HIGH, 1, 0};
#else
    _joyPress = {0, HIGH, 1, 0};
#endif
    _joyDirection = 0;
}

void InputClass::update() {
    static uint32_t scanTick = 0;
    if (millis() - scanTick < 10) return;
    scanTick = millis();

    const bool volUpPressed = digitalRead(PIN_VOL_UP) == LOW;
    const bool volDownPressed = digitalRead(PIN_VOL_DOWN) == LOW;
    const bool bothVolumePressed = volUpPressed && volDownPressed;

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
            _volUp = {PIN_VOL_UP, HIGH, 1, millis()};
            _volDown = {PIN_VOL_DOWN, HIGH, 1, millis()};
        }
        return;
    }

    auto scan = [](Btn &b, void (*action)(void)) {
        bool raw = digitalRead(b.pin) == LOW;
        if (raw != (bool)b.state) {
            b.ms = millis();
            b.state = raw;
        }
        if (!raw && !b.triggered && (millis() - b.ms > 20)) {
            const bool wasBacklightOn = Display.isBacklightOn();
            Display.noteActivity();  // 任意键：亮背光 + 重置待机计时
            const bool wakeOnly = !wasBacklightOn;
            if (!wakeOnly) action();
            b.triggered = 1;
        }
        if (raw) {
            b.triggered = 0;
        }
    };

#if JOYSTICK_VRX_PIN >= 0 && JOYSTICK_VRY_PIN >= 0
    // 仅在离开中心死区时触发一次，回到中心后才允许下一次方向事件。
    constexpr int JOY_LOW = 1248;
    constexpr int JOY_HIGH = 2848;
    const int x = analogRead(JOYSTICK_VRX_PIN);
    const int y = analogRead(JOYSTICK_VRY_PIN);
    int8_t direction = 0;
    if (y < JOY_LOW) direction = 1;       // Up
    else if (y > JOY_HIGH) direction = 2; // Down
    else if (x < JOY_LOW) direction = 3;  // Left
    else if (x > JOY_HIGH) direction = 4; // Right

    if (direction != 0 && _joyDirection == 0) {
        const bool wakeOnly = !Display.isBacklightOn();
        Display.noteActivity();
        if (!wakeOnly) {
            Display.handleJoystick((DisplayClass::JoystickEvent)(direction - 1));
        }
    }
    _joyDirection = direction;
#endif

    scan(_play, []() {
        if (Display.isNetworkSettings()) Display.networkActivate();
        else BluetoothA2DP.togglePlayPause();
    });
    scan(_volUp, []() {
        if (Display.isNetworkSettings()) Display.networkMove(-1);
        else BluetoothA2DP.volumeUp();
    });
    scan(_volDown, []() {
        if (Display.isNetworkSettings()) Display.networkMove(1);
        else BluetoothA2DP.volumeDown();
    });

#if JOYSTICK_SW_PIN >= 0
    scan(_joyPress, []() { Display.handleJoystick(DisplayClass::JoystickEvent::Press); });
#endif
}
