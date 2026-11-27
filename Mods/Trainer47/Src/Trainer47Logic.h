#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace Trainer47Logic {
inline float Bounded(float value, float fallback, float minimum, float maximum) {
    return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
}

inline bool TriggerHeld(float value, float threshold, bool wasHeld) {
    // A small release deadband avoids repeated toggles around the threshold.
    return value >= (wasHeld ? std::max(0.01f, threshold - 0.05f) : threshold);
}

inline float NextPreset(float current) {
    constexpr std::array presets{0.5f, 0.25f, 0.1f, 0.05f};
    for (float preset : presets)
        if (current > preset + 0.001f)
            return preset;
    return presets.front();
}

inline float Advance(float current, float target, float slowSpeed, float seconds, float delta) {
    if (seconds <= 0.001f) return target;
    const float step = (1.f - slowSpeed) / seconds * Bounded(delta, 0.f, 0.f, 0.1f);
    if (current < target) return std::min(current + step, target);
    return std::max(current - step, target);
}

inline bool Near(float lhs, float rhs) {
    return std::isfinite(lhs) && std::isfinite(rhs) && std::abs(lhs - rhs) < 0.0001f;
}

// How bullet time is triggered.
enum EActivation : int {
    // Slow while the aim input (LT / right mouse) is held.
    Activation_HoldAim = 0,
    // Slow after a local-player weapon fire request during an established aim.
    // The pre-call hook does not prove a projectile was successfully created.
    Activation_AimAfterShot = 1,
    Activation_Count = 2,
};

// Keeps the "a real shot happened during this aim" latch up to date.
inline bool UpdateShotLatch(bool wasAimHeld, bool aimHeld, bool latched, bool shotThisFrame) {
    // Conservatively reject a queued shot when aim has only just started:
    // it might have been a hip-fire request earlier in the same frame.
    if (!wasAimHeld || !aimHeld) return false;
    return latched || shotThisFrame;
}

inline bool WantSlow(int mode, bool aimHeld, bool shotLatched) {
    if (!aimHeld) return false;
    if (mode == Activation_HoldAim) return true;
    if (mode == Activation_AimAfterShot) return shotLatched;
    return false;
}

// Explicit offline states from the SDK's ZOnlineManager_EState (Enums.h):
// eOFFLINE = 2, eDISCONNECTED = 4. Every other state (including the whole
// connecting/authenticating flow) fails closed. IsConnected() alone is not enough:
// it is false while the game is still connecting.
inline bool IsOfflineState(int state) {
    return state == 2 || state == 4;
}

// ---------------------------------------------------------------------------
// v0.2.2 infinite pistol ammo (reserve ammo only; the magazine is never touched,
// so normal reloading is preserved).
// ---------------------------------------------------------------------------

// eAmmoType values from the SDK's Enums.h.
constexpr int c_AmmoGun = 0;          // eAmmoGun: standard + silenced pistols
constexpr int c_AmmoLightPistol = 10; // eAmmoLightPistol
constexpr int c_AmmoTypeCount = 17;   // eUnknownAmmoType: size of m_nAmmoInPocket

inline bool IsPistolAmmo(int type) {
    return type == c_AmmoGun || type == c_AmmoLightPistol;
}

// Reserve target: the inventory's own max for that ammo type when it looks sane,
// otherwise a conservative fallback.
inline uint32_t AmmoTarget(uint32_t maxFromInventory) {
    return (maxFromInventory >= 1 && maxFromInventory <= 500) ? maxFromInventory : 60u;
}

// Only ever raise reserve ammo, never lower it.
inline bool NeedsTopUp(uint32_t current, uint32_t target) {
    return current < target;
}

// The SDK struct layout is trusted only if the game's own getter and our decoded
// struct read agree for every ammo type. Any difference means the game build and
// the SDK headers disagree, so nothing may be written.
inline bool LayoutMatches(const uint32_t* gameValues, const uint32_t* structValues, int count) {
    if (!gameValues || !structValues || count <= 0) return false;
    for (int i = 0; i < count; ++i)
        if (gameValues[i] != structValues[i]) return false;
    return true;
}

// Bullet time may only run when every gate is open. Unknown online state fails closed.
inline bool GatesOpen(bool offlineOk, bool gameFocused, bool menuHasFocus, bool enabled) {
    return offlineOk && gameFocused && !menuHasFocus && enabled;
}
}
