#include "Trainer47.h"
#include "Trainer47Logic.h"

#include "IconsMaterialDesign.h"

#include "Hooks.h"
#include "Functions.h"
#include "Logging.h"

#include <Glacier/ZGameLoopManager.h>
#include <Glacier/ZGameTime.h>
#include <Glacier/ZHitman5.h>
#include <Glacier/ZItem.h>
#include <Glacier/ZInventory.h>
#include <Glacier/Enums.h>
#include <Glacier/ZOnlineManager.h>

#include <Windows.h>
#include <Xinput.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <iterator>

// ---------------------------------------------------------------------------
// XInput is loaded at runtime so we don't depend on a specific XInput DLL.
// Works with Xbox controllers natively and with PlayStation/other pads when
// Steam Input is on (Steam presents them to the game as an Xbox controller).
// ---------------------------------------------------------------------------
namespace {
    using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

    XInputGetStateFn GetXInputGetState() {
        static XInputGetStateFn s_Fn = [] () -> XInputGetStateFn {
            const wchar_t* s_Dlls[] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" };

            for (const auto* s_Dll : s_Dlls) {
                if (HMODULE s_Module = LoadLibraryExW(s_Dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
                    if (auto s_Proc = GetProcAddress(s_Module, "XInputGetState")) {
                        return reinterpret_cast<XInputGetStateFn>(s_Proc);
                    }
                    FreeLibrary(s_Module);
                }
            }

            return nullptr;
        }();

        return s_Fn;
    }

    // Which button must be held for the D-pad shortcuts.
    // Index into this table is what we save in the settings.
    struct ModifierOption {
        const char* m_Name;
        uint16_t m_Mask;
    };

    constexpr ModifierOption c_Modifiers[] = {
        { "LB (left bumper)", XINPUT_GAMEPAD_LEFT_SHOULDER },
        { "RB (right bumper)", XINPUT_GAMEPAD_RIGHT_SHOULDER },
        { "View / Back", XINPUT_GAMEPAD_BACK },
        { "L3 (click left stick)", XINPUT_GAMEPAD_LEFT_THUMB },
        { "R3 (click right stick)", XINPUT_GAMEPAD_RIGHT_THUMB },
    };

    constexpr const char* c_ActivationNames[] = {
        "Hold aim (LT / right mouse)",
        "Hold aim, starts after weapon-fire request",
    };

    bool IsGameFocused() {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        return pid == GetCurrentProcessId();
    }

    constexpr const char c_Section[] = "bullet_time"; // array, not pointer: ZString only converts from char arrays
    constexpr const char c_AmmoSection[] = "ammo";      // same reason

    // -----------------------------------------------------------------------
    // Infinite pistol ammo (v0.2.2)
    //
    // Only verified SDK interfaces are used:
    //   - ZHitman5::m_pCharacter -> ZCharacter::m_rSubcontrollerContainer
    //     -> m_aReferencedControllers                       (SDK headers)
    //   - ZEntityRef::QueryInterface<ZCharacterSubcontrollerInventory>()
    //     (game type registry, instead of Player's hard-coded controller [6])
    //   - Functions::ZCharacterSubcontrollerInventory_GetAmmoInPocketForType
    //     (the game's own getter, used to validate the SDK struct layout)
    //   - TCheatProtect<uint32>::Get / Set                  (SDK)
    // Nothing is written unless the game's getter and the SDK struct agree for
    // every ammo type, and every write is read back through the game's getter.
    // -----------------------------------------------------------------------
    enum AmmoResult : int {
        Ammo_Ok = 0,
        Ammo_NoCharacter,
        Ammo_NoInventory,
        Ammo_LayoutMismatch,
        Ammo_VerifyFailed,
        Ammo_Exception,
    };

    struct AmmoProbe {
        ZHitman5* m_Hitman = nullptr;
        bool m_Write = false;
        int m_Result = Ammo_Exception;
        uint32_t m_Game[Trainer47Logic::c_AmmoTypeCount] {};
        uint32_t m_Struct[Trainer47Logic::c_AmmoTypeCount] {};
        uint32_t m_ToppedUp = 0;
    };

    void ProbeAmmo(void* p_Ctx) {
        auto* s_Probe = static_cast<AmmoProbe*>(p_Ctx);

        auto* s_Character = s_Probe->m_Hitman->m_pCharacter.m_pInterfaceRef;
        if (!s_Character) {
            s_Probe->m_Result = Ammo_NoCharacter;
            return;
        }

        auto* s_Container = s_Character->m_rSubcontrollerContainer.m_pInterfaceRef;
        if (!s_Container) {
            s_Probe->m_Result = Ammo_NoInventory;
            return;
        }

        ZCharacterSubcontrollerInventory* s_Inventory = nullptr;
        for (auto& s_Controller : s_Container->m_aReferencedControllers) {
            s_Inventory = s_Controller.m_entityRef.QueryInterface<ZCharacterSubcontrollerInventory>();
            if (s_Inventory) break;
        }

        if (!s_Inventory) {
            s_Probe->m_Result = Ammo_NoInventory;
            return;
        }

        for (int i = 0; i < Trainer47Logic::c_AmmoTypeCount; ++i) {
            s_Probe->m_Game[i] = Functions::ZCharacterSubcontrollerInventory_GetAmmoInPocketForType->Call(
                s_Inventory, static_cast<eAmmoType>(i)
            );
            s_Probe->m_Struct[i] = s_Inventory->m_nAmmoInPocket[i].Get();
        }

        if (!Trainer47Logic::LayoutMatches(s_Probe->m_Game, s_Probe->m_Struct, Trainer47Logic::c_AmmoTypeCount)) {
            s_Probe->m_Result = Ammo_LayoutMismatch;
            return;
        }

        if (s_Probe->m_Write) {
            const int s_Types[] = { Trainer47Logic::c_AmmoGun, Trainer47Logic::c_AmmoLightPistol };

            for (const int s_Type : s_Types) {
                const uint32_t s_Max = s_Type == Trainer47Logic::c_AmmoGun
                                           ? s_Inventory->m_nMaxGunAmmo
                                           : s_Inventory->m_nMaxPistolLightAmmo;
                const uint32_t s_Target = Trainer47Logic::AmmoTarget(s_Max);

                if (!Trainer47Logic::NeedsTopUp(s_Probe->m_Game[s_Type], s_Target)) {
                    continue;
                }

                s_Inventory->m_nAmmoInPocket[s_Type].Set(s_Target);

                const uint32_t s_After = Functions::ZCharacterSubcontrollerInventory_GetAmmoInPocketForType->Call(
                    s_Inventory, static_cast<eAmmoType>(s_Type)
                );

                if (s_After != s_Target) {
                    s_Probe->m_Result = Ammo_VerifyFailed;
                    return;
                }

                ++s_Probe->m_ToppedUp;
            }
        }

        s_Probe->m_Result = Ammo_Ok;
    }

    // Structured exception guard: an access violation from a bad layout turns
    // into a clean "disabled" state instead of crashing the game.
    bool RunGuarded(void (*p_Fn)(void*), void* p_Ctx) {
#ifdef _MSC_VER
        __try {
            p_Fn(p_Ctx);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
#else
        p_Fn(p_Ctx);
        return true;
#endif
    }
}

Trainer47::~Trainer47() {
    const ZMemberDelegate<Trainer47, void(const SGameUpdateEvent&)> s_Delegate(this, &Trainer47::OnFrameUpdate);
    if (m_Registered && Globals::GameLoopManager) {
        Globals::GameLoopManager->UnregisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdatePlayMode);
    }
    std::scoped_lock lock(m_StateMutex);

    // Never leave the game stuck in slow motion if the mod is unloaded.
    RestoreNormalSpeed();
}

void Trainer47::Init() {
    // Mission exit / reload / scene change: never carry slow motion across scenes.
    Hooks::ZEntitySceneContext_ClearScene->AddDetour(this, &Trainer47::OnClearScene);
    // 47's real shots (owner-filtered), used by the "after first shot" activation mode.
    Hooks::ZHM5ItemWeapon_FireProjectiles->AddDetour(this, &Trainer47::ZHM5ItemWeapon_FireProjectiles);
}

void Trainer47::OnEngineInitialized() {
    std::scoped_lock lock(m_StateMutex);
    LoadSettings();
    if (m_Registered || !Globals::GameLoopManager) return;

    const ZMemberDelegate<Trainer47, void(const SGameUpdateEvent&)> s_Delegate(this, &Trainer47::OnFrameUpdate);
    Globals::GameLoopManager->RegisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdatePlayMode);
    m_Registered = true;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
Trainer47::PadState Trainer47::ReadPad() {
    PadState s_Pad;

    const auto s_GetState = GetXInputGetState();

    if (!s_GetState) {
        return s_Pad;
    }

    // Use the first connected controller.
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        XINPUT_STATE s_State {};

        if (s_GetState(i, &s_State) == ERROR_SUCCESS) {
            s_Pad.m_Connected = true;
            s_Pad.m_LeftTrigger = s_State.Gamepad.bLeftTrigger / 255.f;
            s_Pad.m_RightTrigger = s_State.Gamepad.bRightTrigger / 255.f;
            s_Pad.m_Buttons = s_State.Gamepad.wButtons;
            break;
        }
    }

    return s_Pad;
}

bool Trainer47::IsAimHeld(const PadState& p_Pad) {
    m_LeftHeld = p_Pad.m_Connected && Trainer47Logic::TriggerHeld(p_Pad.m_LeftTrigger, m_TriggerThreshold, m_LeftHeld);
    m_RightHeld = p_Pad.m_Connected && Trainer47Logic::TriggerHeld(p_Pad.m_RightTrigger, m_TriggerThreshold, m_RightHeld);
    bool aiming = m_LeftHeld;
    if (m_UseMouseAim) {
        aiming = aiming || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    }
    return aiming;
}

void Trainer47::HandleHotkeys(const PadState& p_Pad) {
    const uint16_t s_Now = p_Pad.m_Buttons;
    if (!p_Pad.m_Connected || !m_PadWasConnected) {
        m_PrevButtons = s_Now;
        m_PadWasConnected = p_Pad.m_Connected;
        return;
    }
    const uint16_t s_Pressed = s_Now & ~m_PrevButtons; // buttons that went down this frame
    m_PrevButtons = s_Now;

    if (!m_ControllerHotkeys || !p_Pad.m_Connected) {
        return;
    }

    const uint16_t s_Modifier = c_Modifiers[m_ModifierIndex].m_Mask;

    if ((s_Now & s_Modifier) == 0) {
        return;
    }

    char s_Buffer[96];

    if (s_Pressed & XINPUT_GAMEPAD_DPAD_UP) {
        m_BulletTimeEnabled = !m_BulletTimeEnabled;
        Toast(m_BulletTimeEnabled ? "Bullet Time: ON" : "Bullet Time: OFF");
        SaveSettings();
    }

    if (s_Pressed & XINPUT_GAMEPAD_DPAD_LEFT) {
        m_SlowSpeed = std::clamp(m_SlowSpeed - 0.05f, 0.05f, 0.9f);
        snprintf(s_Buffer, sizeof(s_Buffer), "Slow speed: %d%%", static_cast<int>(m_SlowSpeed * 100.f + 0.5f));
        Toast(s_Buffer);
        SaveSettings();
    }

    if (s_Pressed & XINPUT_GAMEPAD_DPAD_RIGHT) {
        m_SlowSpeed = std::clamp(m_SlowSpeed + 0.05f, 0.05f, 0.9f);
        snprintf(s_Buffer, sizeof(s_Buffer), "Slow speed: %d%%", static_cast<int>(m_SlowSpeed * 100.f + 0.5f));
        Toast(s_Buffer);
        SaveSettings();
    }

    if (s_Pressed & XINPUT_GAMEPAD_DPAD_DOWN) {
        // Cycle through presets: 50% -> 25% -> 10% -> 5% (almost paused) -> 50% ...
        m_SlowSpeed = Trainer47Logic::NextPreset(m_SlowSpeed);
        snprintf(s_Buffer, sizeof(s_Buffer), "Slow speed: %d%%", static_cast<int>(m_SlowSpeed * 100.f + 0.5f));
        Toast(s_Buffer);
        SaveSettings();
    }
}

// ---------------------------------------------------------------------------
// Game speed
// ---------------------------------------------------------------------------
bool Trainer47::SetGameSpeed(float p_Multiplier) {
    auto* manager = Globals::GameTimeManager;
    if (!manager) {
        m_WeChangedSpeed = false;
        m_OwnedTimeManager = nullptr;
        m_CurrentSpeed = 1.f;
        return false;
    }

    // Never fight another time-scale writer, and do not assume a new manager
    // still contains the speed that we wrote to the previous one.
    const float actual = manager->m_fGameTimeMultiplier;
    if (m_WeChangedSpeed && (m_OwnedTimeManager != manager ||
            !Trainer47Logic::Near(actual, m_LastAppliedSpeed))) {
        m_WeChangedSpeed = false;
        m_OwnedTimeManager = nullptr;
        m_CurrentSpeed = 1.f;
        m_WaitForRelease = true;
        Toast("Time-scale conflict: release aim");
        return false;
    }
    if (!m_WeChangedSpeed && !Trainer47Logic::Near(actual, 1.f)) {
        m_CurrentSpeed = 1.f;
        m_WaitForRelease = true;
        Toast("Time-scale busy: release aim");
        return false;
    }

    manager->m_fGameTimeMultiplier = p_Multiplier;
    m_LastAppliedSpeed = p_Multiplier;
    m_CurrentSpeed = p_Multiplier;
    m_WeChangedSpeed = p_Multiplier != 1.f;
    m_OwnedTimeManager = m_WeChangedSpeed ? manager : nullptr;
    return true;
}

void Trainer47::RestoreNormalSpeed() {
    auto* manager = Globals::GameTimeManager;
    if (m_WeChangedSpeed && manager && m_OwnedTimeManager == manager &&
            Trainer47Logic::Near(manager->m_fGameTimeMultiplier, m_LastAppliedSpeed)) {
        manager->m_fGameTimeMultiplier = 1.f;
    }
    m_CurrentSpeed = 1.f;
    m_LastAppliedSpeed = 1.f;
    m_WeChangedSpeed = false;
    m_OwnedTimeManager = nullptr;
}

void Trainer47::ResetInputState() {
    m_InputReady = false;
    m_PadWasConnected = false;
    m_LeftHeld = m_RightHeld = false;
    m_AimHeld = false;
    m_ShotLatched = false;
    m_WaitForRelease = true;
    m_WasEnabled = false;
}

void Trainer47::OnFrameUpdate(const SGameUpdateEvent& p_UpdateEvent) {
    std::scoped_lock lock(m_StateMutex);
    const float delta = Trainer47Logic::Bounded(static_cast<float>(p_UpdateEvent.m_RealTimeDelta), 0.f, 0.f, 0.1f);
    m_ToastTimeLeft = std::max(0.f, m_ToastTimeLeft - delta);

    // Verified offline gate: explicit eOFFLINE / eDISCONNECTED states only, plus the
    // IsConnected() check the bundled QuickSave mod uses. Unknown/connecting fails closed.
    m_OnlineState = Globals::OnlineManager ? static_cast<int>(Globals::OnlineManager->m_eState) : -1;
    m_OfflineOk = Globals::OnlineManager != nullptr && !Globals::OnlineManager->IsConnected() &&
                  Trainer47Logic::IsOfflineState(m_OnlineState);

    const bool shotThisFrame = m_PlayerShotPending.exchange(false);
    // The local player query is already used by the existing weapon hook.
    // No time-scale change without a live local-player reference.
    m_PlayerPresent = false;
    if (m_OfflineOk) {
        const auto localPlayer = SDK()->GetLocalPlayer();
        m_PlayerPresent = static_cast<bool>(localPlayer);
    }

    // Keep polling hotkeys even when the feature is disabled so it can be
    // re-enabled. Feature-specific activation is gated separately below.
    if (!Trainer47Logic::GatesOpen(m_OfflineOk && m_PlayerPresent, IsGameFocused(), m_MenuHasFocus, true)) {
        RestoreNormalSpeed();
        ResetInputState();
        return;
    }

    const PadState pad = ReadPad();
    if (!m_InputReady) {
        m_PrevButtons = pad.m_Buttons;
        m_PadWasConnected = pad.m_Connected;
        m_InputReady = true;
        m_WaitForRelease = true;
    }
    if (!m_PadWasConnected && pad.m_Connected) m_WaitForRelease = true;
    HandleHotkeys(pad);
    UpdateInfiniteAmmo(delta);

    const bool wasAimHeld = m_AimHeld;
    m_AimHeld = IsAimHeld(pad);
    if (!m_BulletTimeEnabled) {
        m_WasEnabled = false;
        m_ShotLatched = false;
        m_WaitForRelease = true;
        RestoreNormalSpeed();
        return;
    }
    if (!m_WasEnabled) {
        m_WasEnabled = true;
        m_ShotLatched = false;
        m_WaitForRelease = true;
    }
    if (!m_AimHeld) m_WaitForRelease = false;
    if (m_WaitForRelease) {
        m_ShotLatched = false;
        RestoreNormalSpeed();
        return;
    }
    m_ShotLatched = Trainer47Logic::UpdateShotLatch(wasAimHeld, m_AimHeld, m_ShotLatched, shotThisFrame);
    const bool wantSlow = Trainer47Logic::WantSlow(m_ActivationMode, m_AimHeld, m_ShotLatched);
    const float target = wantSlow ? m_SlowSpeed : 1.f;
    const float seconds = wantSlow ? m_EaseInSeconds : m_EaseOutSeconds;
    const float next = Trainer47Logic::Advance(m_CurrentSpeed, target, m_SlowSpeed, seconds, delta);
    if (next != 1.f || m_WeChangedSpeed)
        SetGameSpeed(next);
}

// ---------------------------------------------------------------------------
// Infinite pistol ammo
// Runs on the game thread from OnFrameUpdate, only after the offline, focus and
// local-player gates have passed.
// ---------------------------------------------------------------------------
void Trainer47::UpdateInfiniteAmmo(float p_RealDelta) {
    if (!m_InfinitePistolAmmo) {
        m_AmmoStatus = m_AmmoBlocked ? "Blocked (layout check failed)" : "Off";
        return;
    }

    if (m_AmmoBlocked) {
        return;
    }

    m_AmmoTimer -= p_RealDelta;
    if (m_AmmoTimer > 0.f) {
        return;
    }
    m_AmmoTimer = 0.5f;

    const auto s_LocalHitman = SDK()->GetLocalPlayer();
    if (!s_LocalHitman) {
        m_AmmoStatus = "Waiting for player";
        return;
    }

    AmmoProbe s_Probe;
    s_Probe.m_Hitman = s_LocalHitman.m_pInterfaceRef;
    s_Probe.m_Write = true;

    if (!RunGuarded(&ProbeAmmo, &s_Probe)) {
        s_Probe.m_Result = Ammo_Exception;
    }

    switch (s_Probe.m_Result) {
        case Ammo_Ok:
            m_AmmoTopUps += s_Probe.m_ToppedUp;
            m_AmmoStatus = "Active - pistol reserve " + std::to_string(s_Probe.m_Game[Trainer47Logic::c_AmmoGun]) +
                           (s_Probe.m_ToppedUp ? " (topped up)" : "");
            break;
        case Ammo_NoCharacter:
        case Ammo_NoInventory:
            m_AmmoStatus = "Waiting for inventory";
            break;
        default: {
            m_AmmoBlocked = true;
            const char* s_Reason = s_Probe.m_Result == Ammo_LayoutMismatch
                                       ? "game getter and SDK layout disagree"
                                       : s_Probe.m_Result == Ammo_VerifyFailed
                                       ? "write did not read back"
                                       : "memory access exception";
            m_AmmoStatus = std::string("Blocked: ") + s_Reason;
            Toast("Infinite ammo disabled - layout check failed");
            Logger::Warn("[Trainer47] Infinite pistol ammo disabled for this session: {}.", s_Reason);
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
DEFINE_PLUGIN_DETOUR(Trainer47, void, OnClearScene, ZEntitySceneContext* th, bool p_FullyUnloadScene) {
    {
        std::scoped_lock lock(m_StateMutex);
        RestoreNormalSpeed();
        ResetInputState();
        m_WaitForRelease = true;
        m_AmmoTimer = 1.0f; // let the next scene settle before the first ammo check
    }
    m_PlayerShotPending = false;

    return { HookAction::Continue() };
}

DEFINE_PLUGIN_DETOUR(Trainer47, bool, ZHM5ItemWeapon_FireProjectiles, ZHM5ItemWeapon* th, bool bMayStartSound) {
    // Pre-call notification: records a local weapon-fire request, not a
    // confirmed projectile. Same owner check as the bundled Player mod.
    if (th) {
        const auto s_LocalHitman = SDK()->GetLocalPlayer();

        if (s_LocalHitman && th->m_pOwner == s_LocalHitman.m_entityRef) {
            m_PlayerShotPending = true;
        }
    }

    return { HookAction::Continue() };
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------
void Trainer47::Toast(const std::string& p_Message) {
    m_ToastText = p_Message;
    m_ToastTimeLeft = 1.5f;
}

void Trainer47::OnDrawMenu() {
    std::scoped_lock lock(m_StateMutex);
    if (ImGui::Button(ICON_MD_SLOW_MOTION_VIDEO " TRAINER 47")) {
        m_ShowWindow = !m_ShowWindow;
    }
}

void Trainer47::OnDrawUI(bool p_HasFocus) {
    std::scoped_lock lock(m_StateMutex);
    m_MenuHasFocus = p_HasFocus;
    // --- Always-on overlay (visible during gameplay) ---
    if (m_ShowIndicator) {
        auto* s_DrawList = ImGui::GetForegroundDrawList();
        const ImVec2 s_Screen = ImGui::GetIO().DisplaySize;

        if (m_BulletTimeEnabled && m_CurrentSpeed < 0.98f) {
            char s_Text[64];
            snprintf(s_Text, sizeof(s_Text), "BULLET TIME  %d%%", static_cast<int>(m_CurrentSpeed * 100.f + 0.5f));

            const ImVec2 s_Size = ImGui::CalcTextSize(s_Text);
            const ImVec2 s_Pos((s_Screen.x - s_Size.x) * 0.5f, s_Screen.y * 0.08f);

            s_DrawList->AddRectFilled(
                ImVec2(s_Pos.x - 10.f, s_Pos.y - 5.f),
                ImVec2(s_Pos.x + s_Size.x + 10.f, s_Pos.y + s_Size.y + 5.f),
                IM_COL32(0, 0, 0, 150), 4.f
            );
            s_DrawList->AddText(s_Pos, IM_COL32(230, 40, 40, 255), s_Text);
        }

        std::string s_Toast;
        float s_ToastTime;
        s_Toast = m_ToastText;
        s_ToastTime = m_ToastTimeLeft;

        if (s_ToastTime > 0.f && !s_Toast.empty()) {
            const ImVec2 s_Size = ImGui::CalcTextSize(s_Toast.c_str());
            const ImVec2 s_Pos((s_Screen.x - s_Size.x) * 0.5f, s_Screen.y * 0.13f);
            const int s_Alpha = static_cast<int>(std::clamp(s_ToastTime / 0.4f, 0.f, 1.f) * 255.f);

            s_DrawList->AddRectFilled(
                ImVec2(s_Pos.x - 10.f, s_Pos.y - 5.f),
                ImVec2(s_Pos.x + s_Size.x + 10.f, s_Pos.y + s_Size.y + 5.f),
                IM_COL32(0, 0, 0, s_Alpha * 150 / 255), 4.f
            );
            s_DrawList->AddText(s_Pos, IM_COL32(255, 255, 255, s_Alpha), s_Toast.c_str());
        }
    }

    // --- Settings window (open the SDK menu with ~ and click TRAINER 47) ---
    if (!p_HasFocus || !m_ShowWindow) {
        return;
    }

    ImGui::PushFont(SDK()->GetImGuiBlackFont());
    const auto s_Expanded = ImGui::Begin("Trainer 47", &m_ShowWindow);
    ImGui::PushFont(SDK()->GetImGuiRegularFont());

    if (s_Expanded) {
        bool s_Changed = false;

        ImGui::SeparatorText("Status");
        if (m_OfflineOk && m_PlayerPresent) {
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.f), "Offline - local player detected");
        }
        else if (m_OfflineOk) {
            ImGui::TextDisabled("Offline - waiting for local player");
        }
        else {
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.3f, 1.f), "Online / unknown - trainer paused");
            ImGui::TextWrapped("Bullet Time only runs while the game is offline or disconnected.");
            ImGui::TextDisabled("Online state code: %d (2 = offline, 4 = disconnected)", m_OnlineState);
        }

        ImGui::SeparatorText("Bullet Time");

        s_Changed |= ImGui::Checkbox("Enabled", &m_BulletTimeEnabled);

        float s_Percent = m_SlowSpeed * 100.f;
        if (ImGui::SliderFloat("World speed while aiming", &s_Percent, 5.f, 90.f, "%.0f%%")) {
            m_SlowSpeed = s_Percent / 100.f;
            s_Changed = true;
        }

        s_Changed |= ImGui::SliderFloat("Slow-down time", &m_EaseInSeconds, 0.f, 1.f, "%.2f s");
        s_Changed |= ImGui::SliderFloat("Speed-up time", &m_EaseOutSeconds, 0.f, 1.f, "%.2f s");
        s_Changed |= ImGui::SliderFloat("Trigger pull needed", &m_TriggerThreshold, 0.05f, 0.95f, "%.2f");
        if (ImGui::BeginCombo("Activation", c_ActivationNames[m_ActivationMode])) {
            for (int i = 0; i < Trainer47Logic::Activation_Count; ++i) {
                if (ImGui::Selectable(c_ActivationNames[i], i == m_ActivationMode)) {
                    m_ActivationMode = i;
                    s_Changed = true;
                }
            }

            ImGui::EndCombo();
        }
        if (m_ActivationMode == Trainer47Logic::Activation_AimAfterShot) {
            ImGui::TextWrapped("Starts after a weapon-fire request during an established aim. The first shot is usually at normal speed.");
            ImGui::TextWrapped("Player Super accuracy / Rapid fire may prevent request detection.");
        }
        s_Changed |= ImGui::Checkbox("Right mouse button also works", &m_UseMouseAim);
        s_Changed |= ImGui::Checkbox("Show on-screen indicator", &m_ShowIndicator);

        ImGui::SeparatorText("Controller shortcuts");

        s_Changed |= ImGui::Checkbox("Enable shortcuts", &m_ControllerHotkeys);

        if (ImGui::BeginCombo("Hold button", c_Modifiers[m_ModifierIndex].m_Name)) {
            for (int i = 0; i < static_cast<int>(std::size(c_Modifiers)); ++i) {
                if (ImGui::Selectable(c_Modifiers[i].m_Name, i == m_ModifierIndex)) {
                    m_ModifierIndex = i;
                    s_Changed = true;
                }
            }

            ImGui::EndCombo();
        }

        ImGui::TextDisabled("+ D-pad Up     toggle Bullet Time");
        ImGui::TextDisabled("+ D-pad Left   slower");
        ImGui::TextDisabled("+ D-pad Right  faster");
        ImGui::TextDisabled("+ D-pad Down   cycle 50 / 25 / 10 / 5%%");

        const PadState s_Pad = ReadPad();
        ImGui::Text("Controller: %s", s_Pad.m_Connected ? "connected" : "not detected (turn on Steam Input?)");

        ImGui::SeparatorText("Infinite pistol ammo");

        s_Changed |= ImGui::Checkbox("Infinite pistol reserve ammo (you still reload)", &m_InfinitePistolAmmo);
        ImGui::TextDisabled("Status: %s", m_AmmoStatus.c_str());
        if (m_AmmoTopUps > 0) {
            ImGui::TextDisabled("Top-ups this session: %u", m_AmmoTopUps);
        }
        if (m_AmmoBlocked) {
            ImGui::TextWrapped(
                "Disabled until the game restarts: the SDK's inventory layout doesn't match this game build, "
                "so nothing was written. Wait for an SDK update."
            );
        }
        ImGui::TextWrapped("Turn off the Player mod's Infinite ammo and No reload while using this.");

        if (s_Changed) {
            SaveSettings();
        }
    }

    ImGui::PopFont();
    ImGui::End();
    ImGui::PopFont();
}

// ---------------------------------------------------------------------------
// Settings (stored by the SDK in mods.ini)
// ---------------------------------------------------------------------------
void Trainer47::LoadSettings() {
    m_BulletTimeEnabled = GetSettingBool(c_Section, "enabled", m_BulletTimeEnabled);
    m_SlowSpeed = static_cast<float>(GetSettingDouble(c_Section, "slow_speed", m_SlowSpeed));
    m_EaseInSeconds = static_cast<float>(GetSettingDouble(c_Section, "ease_in", m_EaseInSeconds));
    m_EaseOutSeconds = static_cast<float>(GetSettingDouble(c_Section, "ease_out", m_EaseOutSeconds));
    m_TriggerThreshold = static_cast<float>(GetSettingDouble(c_Section, "trigger_threshold", m_TriggerThreshold));
    m_UseMouseAim = GetSettingBool(c_Section, "mouse_aim", m_UseMouseAim);
    m_ActivationMode = static_cast<int>(GetSettingInt(c_Section, "activation_mode", m_ActivationMode));
    m_ShowIndicator = GetSettingBool(c_Section, "show_indicator", m_ShowIndicator);
    m_ControllerHotkeys = GetSettingBool(c_Section, "controller_hotkeys", m_ControllerHotkeys);
    m_ModifierIndex = static_cast<int>(GetSettingInt(c_Section, "hotkey_modifier", m_ModifierIndex));
    m_InfinitePistolAmmo = GetSettingBool(c_AmmoSection, "infinite_pistol_ammo", m_InfinitePistolAmmo);

    m_SlowSpeed = Trainer47Logic::Bounded(m_SlowSpeed, 0.25f, 0.05f, 0.9f);
    m_EaseInSeconds = Trainer47Logic::Bounded(m_EaseInSeconds, 0.12f, 0.f, 1.f);
    m_EaseOutSeconds = Trainer47Logic::Bounded(m_EaseOutSeconds, 0.30f, 0.f, 1.f);
    m_TriggerThreshold = Trainer47Logic::Bounded(m_TriggerThreshold, 0.30f, 0.05f, 0.95f);
    m_ModifierIndex = std::clamp(m_ModifierIndex, 0, static_cast<int>(std::size(c_Modifiers)) - 1);
    m_ActivationMode = std::clamp(m_ActivationMode, 0, Trainer47Logic::Activation_Count - 1);
}

void Trainer47::SaveSettings() {
    SetSettingBool(c_Section, "enabled", m_BulletTimeEnabled);
    SetSettingDouble(c_Section, "slow_speed", m_SlowSpeed);
    SetSettingDouble(c_Section, "ease_in", m_EaseInSeconds);
    SetSettingDouble(c_Section, "ease_out", m_EaseOutSeconds);
    SetSettingDouble(c_Section, "trigger_threshold", m_TriggerThreshold);
    SetSettingBool(c_Section, "mouse_aim", m_UseMouseAim);
    SetSettingInt(c_Section, "activation_mode", m_ActivationMode);
    SetSettingBool(c_Section, "show_indicator", m_ShowIndicator);
    SetSettingBool(c_Section, "controller_hotkeys", m_ControllerHotkeys);
    SetSettingInt(c_Section, "hotkey_modifier", m_ModifierIndex);
    SetSettingBool(c_AmmoSection, "infinite_pistol_ammo", m_InfinitePistolAmmo);
}

DEFINE_ZHM_PLUGIN(Trainer47);
