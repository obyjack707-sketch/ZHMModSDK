# Trainer47 v0.2.2

Offline story-mode bullet time for HITMAN World of Assassination, built as a ZHMModSDK mod.

Built against ZHMModSDK master `1c6b0dca` (26 Nov 2026). Build it inside that SDK checkout
so the loader and the mod DLL come from the same revision.

## What's in it

- **Bullet Time**: hold LT (or right mouse) and the world eases down to the chosen speed
  (default 25%), then eases back to 100% when you let go.
- **Activation modes**
  - *Hold aim*: slows as soon as you aim.
  - *Hold aim, starts after 47's first real shot*: slows only after 47's own gun fires while
    aiming. Cameras, binoculars and menus never fire a projectile, so they never trigger it.
- **Controller shortcuts** (hold LB by default): D-pad Up toggle, Left/Right slower/faster,
  Down cycles 50/25/10/5%. The hold button can be changed.
- **Menu**: press `~`, then click TRAINER 47. Settings save to `mods.ini` `[bullet_time]`.

## v0.2.2: infinite pistol ammo (off by default)

Menu: TRAINER 47 > Infinite pistol ammo. Keeps 47's **reserve** pistol ammo (`eAmmoGun`, which
covers the silenced pistol, plus `eAmmoLightPistol`) topped up to the inventory's own max. The
magazine is never touched, so you still reload normally. Offline-only: it runs from the same
gated frame update as bullet time (explicit offline state + live local player + game focused).

Why not the Player mod's Infinite ammo / No reload: both crash on the last bullet on the
27 Nov game build. Both write game memory through hard-coded layouts: No reload casts
`m_aReferencedControllers[6]` to the inventory with no type check, and Infinite ammo spawns and
writes a `ZHM5CrippleBox`. The game files were updated 27 Nov 05:16 and the SDK is from 26 Nov.
The loader log reports `Could not find pattern ... Game might have been updated`. Turn both
Player options off while using this.

Safety, every 0.5 s while enabled:
1. Inventory found with `ZEntityRef::QueryInterface<ZCharacterSubcontrollerInventory>()` (game
   type registry), not a fixed index.
2. Layout self-check: the game's own `Functions::ZCharacterSubcontrollerInventory_GetAmmoInPocketForType`
   must equal the SDK struct's decoded `m_nAmmoInPocket[i]` for **all 17** ammo types.
3. Write with the SDK's `TCheatProtect::Set`, then read back through the game getter. It must match.
4. The whole probe runs under an SEH guard (`__try/__except`).
Any mismatch, failed read-back or exception blocks the feature until restart (status + toast +
log line). Nothing is written. Only raises reserve ammo, never lowers it.

## Safety gates (all verified against SDK source)

| Gate | SDK source it relies on |
|---|---|
| Offline only (fails closed if unknown) | `!Globals::OnlineManager->IsConnected()` (QuickSave mod) **and** `m_eState` is `eOFFLINE`(2) or `eDISCONNECTED`(4) from `ZOnlineManager_EState` |
| Live local player required | `SDK()->GetLocalPlayer()`, also called in the bundled Noclip mod's frame update |
| Restore 100% on mission exit/reload | `Hooks::ZEntitySceneContext_ClearScene`, used by the bundled Player and Noclip mods |
| Shot detection, 47 only | `Hooks::ZHM5ItemWeapon_FireProjectiles` + `ZHM5ItemWeapon::m_pOwner`, used by the bundled Player mod |
| Game speed | `Globals::GameTimeManager->m_fGameTimeMultiplier`, used by the bundled World mod |
| Paused while SDK menu open / game unfocused | `OnDrawUI(p_HasFocus)`, `GetForegroundWindow` |

## v0.2.1 review (ChatGPT) - merged

Accepted as-is: local-player gate, stale latch cleared on disable/re-enable, a fire request
on the first aim frame is rejected (hip-fire filter), "fire request" wording, invalid
activation mode fails closed, extended tests.

Answered from SDK source:
- Hook lifecycle: `ModLoader::UnloadMod` calls `HookRegistry::ClearDetoursWithContext(plugin)`
  before deleting the plugin (ModLoader.cpp:345). No manual removal needed.
- `IsConnected()` is only true for eCONNECTED / eRETRY_SAVE_EVENTS / eREAUTHENTICATE, so it's
  false during the whole connecting flow. Added an explicit offline-state whitelist. The menu
  shows the raw state code, so a false lock-out can be diagnosed in game.
- Request vs confirmed shot: correct. The detour runs before the original. Observing the result
  would mean calling `CallOriginal` and returning, which stops other mods' detours (Player). So
  this stays a request signal on purpose.

## v0.1.1 review (ChatGPT)

Kept: time-scale ownership/conflict checks, trigger hysteresis, wait-for-release, no false
D-pad edges on reconnect, settings range validation, mutex, guarded unregister,
System32-only XInput loading, the logic tests.

Changed:
- Manual "arm offline session" checkbox **replaced** with the real SDK online-state check, so
  it's automatic and can't be forgotten or ticked while online.
- Raw "aim AND right trigger" mode **replaced** with the verified 47-fired-a-shot mode.
- The README's open lifecycle question (play-mode callbacks stopping while slowed) is now
  covered by the ClearScene hook.

## Known limits (not guessed around)

- The SDK exposes no "is aiming a weapon" flag. *Hold aim* mode can still trigger on cameras
  or binoculars; use the after-shot mode if that bothers you.
- Shot detection may miss shots when the Player mod's **Super accuracy** or **Rapid fire** is
  on, because those detours return early and stop the hook chain.
- Independent camera speed: the SDK has no camera-update hook (`ZHM5MainCamera` is only
  forward-declared). Test first whether the camera even slows. If it does, this needs
  reverse-engineering of the game executable.
- Don't run the bundled **World** mod's time multiplier at the same time. Trainer47 detects the
  conflict and backs off.

## Build

Add `Trainer47` to the `MODS` list in the root `CMakeLists.txt` (already done in the drop-in),
then either push to a fork (GitHub Actions builds `ZHMModSDK-Release`) or run
`cmake --preset x64-Release` + `cmake --build` in a VS 2022 x64 Native Tools prompt.

Logic tests (run from Mods/Trainer47):

    g++ -std=c++23 -Wall -Wextra -Werror -pedantic -I Src tests/logic_tests.cpp -o logic-tests && ./logic-tests

## In-game acceptance checklist (offline story mission)

- [ ] Build succeeds; note the SDK commit and game version
- [ ] Mod loads; menu shows "Offline - local player detected"
- [ ] Hold LT → 25%, release → exactly 100%
- [ ] After-shot mode: aiming alone = no slow, first shot → slow, release → 100%
- [ ] Camera / binoculars in after-shot mode never slow
- [ ] Exit or restart the mission while slowed → next scene at 100%
- [ ] Alt-tab and open the `~` menu while slowed → back to 100%
- [ ] Shortcuts work and don't clash with inventory (change the hold button if they do)
- [ ] **Does the camera/reticle slow down too?** Record 10 s at 25%
