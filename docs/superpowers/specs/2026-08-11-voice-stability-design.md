# Voice Stability Optimization Design

## Goal

Make wake word, realtime conversation, voice playback, and music control work as one deterministic flow without losing the current song or starting microphone capture before StepFun applies the session configuration.

## Scope

- Resume music after local beep and realtime PCM playback.
- Keep the user's configured volume separate from temporary fade gain.
- Duck or stop music only after WakeNet detects the wake word.
- Emit `session.ready` only after StepFun acknowledges `session.updated`.
- Execute play, pause, previous, next, volume, and named-song commands after the spoken confirmation finishes.
- Serialize ownership of the microphone I2S port between WakeWord and VoiceAssistant.
- Reduce weather request heap pressure and preserve the last valid snapshot.

Firmware upload and serial-port access are outside this work.

## Audio Ownership

`AudioPlayer` remains the only owner of output I2S. Before a voice session it records whether music was playing and its URL/format through the existing MusicService-facing playback coordinator. Beep and PCM playback may destroy the decoder stream, so conversation completion requests a fresh open of the saved song URL instead of calling `resume()` on a deleted decoder.

User volume is persistent state. Temporary fades update output gain only; they must not overwrite the configured volume. Voice volume commands update the persistent value and remain effective after the conversation.

## Microphone Ownership

WakeWord remains the default microphone consumer. A handoff flag and acknowledgement coordinate this sequence:

1. WakeNet detects `你好小智`.
2. WakeWord uninstalls microphone I2S and acknowledges release.
3. VoiceAssistant connects to the server.
4. After `session.updated`, VoiceAssistant installs microphone I2S and starts capture.
5. On conversation exit, VoiceAssistant uninstalls I2S before WakeWord resumes.

No module may install the microphone driver until the previous owner confirms release.

## Protocol And Commands

`session.created` only indicates that the upstream socket exists. `session.updated` is the readiness boundary because it confirms voice, VAD, formats, and instructions.

Commands are parsed as complete objects. Named-song commands retain the `song` field. Device execution occurs after response PCM is drained so acknowledgement audio is not interrupted. Unsupported commands are logged and ignored.

Named-song playback requires a MusicService search endpoint. If the current backend lacks this endpoint, the device reports the command as unsupported instead of claiming successful playback.

## Weather And Resources

Weather keeps the previous valid snapshot on temporary failure. HTTP response parsing should avoid an additional whole-body copy where supported. Weather refresh must not overlap latency-sensitive voice startup. Task creation results and stack high-water values are logged so stack sizes can be tuned from evidence.

## Verification

- Server protocol tests prove `session.created` is not ready and `session.updated` is ready.
- Command tests preserve named-song payloads and reject unsupported actions.
- Firmware builds successfully with PlatformIO.
- No upload target or serial command is run.
- User-authorized hardware verification checks wake detection, one full conversation, music restoration, commands, and weather refresh under low-memory conditions.
