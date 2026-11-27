#include "Trainer47Logic.h"
#include <cassert>
#include <limits>
#include <iostream>

int main() {
    using namespace Trainer47Logic;
    assert(Near(Bounded(std::numeric_limits<float>::quiet_NaN(), .25f, .05f, .9f), .25f));
    assert(Near(Bounded(std::numeric_limits<float>::infinity(), .12f, 0.f, 1.f), .12f));
    assert(Near(Bounded(-12.f, .25f, .05f, .9f), .05f));
    assert(Near(Bounded(12.f, .25f, .05f, .9f), .9f));
    assert(!TriggerHeld(.29f, .30f, false));
    assert(TriggerHeld(.30f, .30f, false));
    assert(TriggerHeld(.27f, .30f, true));
    assert(!TriggerHeld(.24f, .30f, true));
    assert(!TriggerHeld(0.f, .05f, true));
    assert(Near(NextPreset(.5f), .25f));
    assert(Near(NextPreset(.25f), .1f));
    assert(Near(NextPreset(.1f), .05f));
    assert(Near(NextPreset(.05f), .5f));
    assert(Near(NextPreset(.8f), .5f));
    assert(Near(NextPreset(.4f), .25f));
    for (int fps : {30, 60, 144}) {
        float speed=1.f;
        for (int i=0; i<fps; ++i) speed=Advance(speed,.25f,.25f,.12f,1.f/fps);
        assert(Near(speed,.25f));
        for (int i=0; i<fps; ++i) speed=Advance(speed,1.f,.25f,.30f,1.f/fps);
        assert(speed==1.f);
    }
    assert(Advance(1.f,.25f,.25f,0.f,0.f)==.25f);
    assert(Advance(.25f,1.f,.25f,0.f,0.f)==1.f);
    assert(Advance(1.f,.25f,.25f,.12f,-1.f)==1.f);
    // v0.2: activation modes
    assert(!WantSlow(Activation_HoldAim, false, false));
    assert(WantSlow(Activation_HoldAim, true, false));
    assert(!WantSlow(Activation_AimAfterShot, true, false));
    assert(WantSlow(Activation_AimAfterShot, true, true));
    assert(!WantSlow(Activation_AimAfterShot, false, true));
    // Latch: reject a queued hip-fire request on a fresh aim;
    // accept fire requests after aim has been observed in both updates.
    bool latch = false;
    latch = UpdateShotLatch(false, true, latch, true); assert(!latch);
    latch = UpdateShotLatch(true, true, latch, false); assert(!latch);
    latch = UpdateShotLatch(true, true, latch, true); assert(latch);
    latch = UpdateShotLatch(true, true, latch, false); assert(latch);
    latch = UpdateShotLatch(true, false, latch, true); assert(!latch);
    latch = UpdateShotLatch(false, false, latch, true); assert(!latch);
    assert(!WantSlow(-1, true, true));
    assert(!WantSlow(99, true, true));
    // gates fail closed
    assert(GatesOpen(true, true, false, true));
    assert(!GatesOpen(false, true, false, true));
    assert(!GatesOpen(true, false, false, true));
    assert(!GatesOpen(true, true, true, true));
    assert(!GatesOpen(true, true, false, false));
    // offline state whitelist
    assert(IsOfflineState(2));
    assert(IsOfflineState(4));
    for (int st : {0, 1, 3, 5, 6, 15, 20, 24, 30, 31, 32, -1}) assert(!IsOfflineState(st));
    // v0.2.2 infinite pistol ammo
    assert(IsPistolAmmo(0) && IsPistolAmmo(10));
    for (int t : {1, 2, 3, 5, 8, 9, 11, 15, 16, 17, -1}) assert(!IsPistolAmmo(t));
    assert(AmmoTarget(48) == 48u);
    assert(AmmoTarget(0) == 60u);
    assert(AmmoTarget(501) == 60u);
    assert(AmmoTarget(0xFFFFFFF0u) == 60u); // garbage max from a bad layout
    assert(NeedsTopUp(0, 48) && NeedsTopUp(47, 48));
    assert(!NeedsTopUp(48, 48) && !NeedsTopUp(200, 48)); // never lowers
    {
        uint32_t g[c_AmmoTypeCount] = {}, o[c_AmmoTypeCount] = {};
        g[0] = o[0] = 24; g[10] = o[10] = 12;
        assert(LayoutMatches(g, o, c_AmmoTypeCount));
        o[16] = 1; assert(!LayoutMatches(g, o, c_AmmoTypeCount)); // any mismatch blocks writes
        assert(!LayoutMatches(nullptr, o, c_AmmoTypeCount));
        assert(!LayoutMatches(g, o, 0));
    }
    std::cout << "All Trainer47 logic tests passed\n";
}
