#pragma once

#include "IPluginInterface.h"

#include <Glacier/SGameUpdateEvent.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

class ZEntitySceneContext;
class ZHM5ItemWeapon;

// Trainer47 - offline/story-mode helper menu for HITMAN World of Assassination.
// v0.2.1: Bullet Time with verified offline gate, mission-unload restore and an
//       optional activation mode driven by 47's weapon-fire requests.
// v0.2.2: Optional infinite pistol reserve ammo (normal reloading kept), with a
//       runtime layout self-check that fails closed.
class Trainer47 : public IPluginInterface {
public:
    ~Trainer47() override;

    void Init() override;
    void OnEngineInitialized() override;
    void OnDrawMenu() override;
    void OnDrawUI(bool p_HasFocus) override;

private:
    struct PadState {
        bool m_Connected = false;
        float m_LeftTrigger = 0.f;  // 0..1
        float m_RightTrigger = 0.f; // 0..1
        uint16_t m_Buttons = 0;     // XINPUT_GAMEPAD_* bitmask
    };

    void OnFrameUpdate(const SGameUpdateEvent& p_UpdateEvent);

    // Verified SDK hooks (same signatures the bundled Player mod uses).
    DECLARE_PLUGIN_DETOUR(Trainer47, void, OnClearScene, ZEntitySceneContext* th, bool p_FullyUnloadScene);
    DECLARE_PLUGIN_DETOUR(Trainer47, bool, ZHM5ItemWeapon_FireProjectiles, ZHM5ItemWeapon* th, bool bMayStartSound);

    PadState ReadPad();
    bool IsAimHeld(const PadState& p_Pad);
    void HandleHotkeys(const PadState& p_Pad);
    bool SetGameSpeed(float p_Multiplier);
    void RestoreNormalSpeed();
    void ResetInputState();
    void Toast(const std::string& p_Message);
    void UpdateInfiniteAmmo(float p_RealDelta);

    void LoadSettings();
    void SaveSettings();

    // --- Settings (saved to mods.ini) ---
    bool m_BulletTimeEnabled = true;
    float m_SlowSpeed = 0.25f;        // world speed while aiming (1.0 = normal)
    float m_EaseInSeconds = 0.12f;    // how fast it slows down
    float m_EaseOutSeconds = 0.30f;   // how fast it returns to normal
    float m_TriggerThreshold = 0.30f; // how far LT must be pulled
    bool m_UseMouseAim = true;        // right mouse button also triggers it
    int m_ActivationMode = 0;         // Trainer47Logic::EActivation
    bool m_ShowIndicator = true;
    bool m_ControllerHotkeys = true;
    int m_ModifierIndex = 0;
    bool m_InfinitePistolAmmo = false; // off by default; reserve ammo only

    // --- Runtime state (guarded by m_StateMutex) ---
    std::mutex m_StateMutex;
    bool m_Registered = false;
    bool m_WasEnabled = false;
    bool m_PlayerPresent = false;
    bool m_OfflineOk = false;         // OnlineManager in an explicit offline/disconnected state
    int m_OnlineState = -1;           // raw ZOnlineManager_EState, for the status line
    bool m_MenuHasFocus = false;
    bool m_InputReady = false;
    bool m_PadWasConnected = false;
    bool m_LeftHeld = false;
    bool m_RightHeld = false;
    bool m_AimHeld = false;
    bool m_ShotLatched = false;       // a fire request happened during the current aim
    bool m_WaitForRelease = false;
    const void* m_OwnedTimeManager = nullptr;
    float m_LastAppliedSpeed = 1.f;
    bool m_ShowWindow = false;
    float m_CurrentSpeed = 1.f;
    bool m_WeChangedSpeed = false;
    uint16_t m_PrevButtons = 0;
    std::string m_ToastText;
    float m_ToastTimeLeft = 0.f;

    // Infinite pistol ammo runtime state.
    float m_AmmoTimer = 0.f;
    bool m_AmmoBlocked = false;        // layout/verify failure: off until game restart
    std::string m_AmmoStatus = "Off";
    uint32_t m_AmmoTopUps = 0;

    // Set from the weapon hook without taking the mutex.
    std::atomic<bool> m_PlayerShotPending { false };
};

DECLARE_ZHM_PLUGIN(Trainer47)
