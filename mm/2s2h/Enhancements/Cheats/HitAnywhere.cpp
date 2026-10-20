#include <libultraship/bridge/consolevariablebridge.h>
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

#include <unordered_map>
#include <unordered_set>

extern "C" {
#include "variables.h"
#include "functions.h"
}

// 0 = Off, 1 = Z-Targeted Enemy, 2 = All Enemies on the Map
#define CVAR_NAME "gCheats.HitAnywhere"
#define CVAR CVarGetInteger(CVAR_NAME, 0)

typedef enum {
    HIT_ANYWHERE_OFF,
    HIT_ANYWHERE_TARGETED,
    HIT_ANYWHERE_ALL_ENEMIES,
} HitAnywhereMode;

// How many targets can be struck by a single swing. The collision system holds 50 AT colliders per frame and other
// actors (including Link's own sword) need some of those, so stay well under that.
#define HIT_ANYWHERE_MAX_TARGETS 24

// How long (in frames) a culled, far-away enemy is kept updating after being struck so it can react, flinch or die.
#define HIT_ANYWHERE_FORCED_UPDATE_FRAMES 60

/*
 * How it works:
 * While Link's melee attack is active (sword swing, Goron punch, Zora fins, etc.) an invisible attack cylinder owned
 * by Link is placed on top of each target. The game's normal collision system then treats it exactly like Link's own
 * weapon touching the enemy, so damage tables, hit effects, sounds, shields and invincibility all behave as normal.
 * Each target can only be struck once per swing.
 *
 * Enemies far from Link are usually "culled" (they stop updating) and could never register a hit, so targets are
 * temporarily allowed to update while they are being attacked, then returned to normal shortly after.
 */

typedef struct {
    ColliderCylinder collider;
    Actor* target;
} HitAnywhereSlot;

static ColliderCylinderInit sHitAnywhereCylinderInit = {
    {
        COL_MATERIAL_NONE,
        AT_ON | AT_TYPE_PLAYER,
        AC_NONE,
        OC1_NONE,
        OC2_TYPE_PLAYER,
        COLSHAPE_CYLINDER,
    },
    {
        ELEM_MATERIAL_UNK2,
        { 0x00000000, 0x00, 0x01 },
        { 0xF7CFFFFF, 0x00, 0x00 },
        ATELEM_ON | ATELEM_SFX_NORMAL,
        ACELEM_NONE,
        OCELEM_NONE,
    },
    { 80, 160, -40, { 0, 0, 0 } },
};

static HitAnywhereSlot sSlots[HIT_ANYWHERE_MAX_TARGETS];
static bool sSlotsInitialized = false;

// Actors already struck during the current swing
static std::unordered_set<Actor*> sHitThisSwing;
// Actors we turned update culling off for, and how many frames until it is restored
static std::unordered_map<Actor*, s32> sForcedUpdateActors;

static s8 sPrevMeleeWeaponState = PLAYER_MELEE_WEAPON_STATE_0;
static s32 sPrevMeleeWeaponAnimation = -1;

static bool IsActorLoaded(Actor* target) {
    if (gPlayState == nullptr || target == nullptr) {
        return false;
    }

    for (s32 category = 0; category < ACTORCAT_MAX; category++) {
        for (Actor* actor = gPlayState->actorCtx.actorLists[category].first; actor != nullptr; actor = actor->next) {
            if (actor == target) {
                return true;
            }
        }
    }

    return false;
}

static void RestoreForcedUpdateActors() {
    for (auto& [actor, framesLeft] : sForcedUpdateActors) {
        if (IsActorLoaded(actor)) {
            actor->flags &= ~ACTOR_FLAG_UPDATE_CULLING_DISABLED;
        }
    }
    sForcedUpdateActors.clear();
}

static void ResetHitAnywhereState() {
    for (auto& slot : sSlots) {
        slot.target = nullptr;
    }
    sSlotsInitialized = false;
    sHitThisSwing.clear();
    sForcedUpdateActors.clear();
    sPrevMeleeWeaponState = PLAYER_MELEE_WEAPON_STATE_0;
    sPrevMeleeWeaponAnimation = -1;
}

static void InitSlots(Player* player) {
    for (auto& slot : sSlots) {
        Collider_InitCylinder(gPlayState, &slot.collider);
        Collider_SetCylinder(gPlayState, &slot.collider, &player->actor, &sHitAnywhereCylinderInit);
        slot.target = nullptr;
    }
    sSlotsInitialized = true;
}

static void KeepActorUpdating(Actor* actor) {
    auto it = sForcedUpdateActors.find(actor);
    if (it != sForcedUpdateActors.end()) {
        it->second = HIT_ANYWHERE_FORCED_UPDATE_FRAMES;
    } else if (!(actor->flags & ACTOR_FLAG_UPDATE_CULLING_DISABLED)) {
        // Only track actors we changed, so we never strip the flag from an actor that set it itself
        actor->flags |= ACTOR_FLAG_UPDATE_CULLING_DISABLED;
        sForcedUpdateActors[actor] = HIT_ANYWHERE_FORCED_UPDATE_FRAMES;
    }
}

static void TickForcedUpdateActors() {
    for (auto it = sForcedUpdateActors.begin(); it != sForcedUpdateActors.end();) {
        if (!IsActorLoaded(it->first)) {
            it = sForcedUpdateActors.erase(it);
        } else if (--it->second <= 0) {
            it->first->flags &= ~ACTOR_FLAG_UPDATE_CULLING_DISABLED;
            it = sForcedUpdateActors.erase(it);
        } else {
            ++it;
        }
    }
}

static bool IsValidTarget(Player* player, Actor* actor) {
    return (actor != nullptr) && (actor != &player->actor) && (actor->update != nullptr) &&
           !sHitThisSwing.contains(actor);
}

static void PlaceAttackOnTarget(Player* player, HitAnywhereSlot* slot, Actor* target) {
    ColliderCylinder* cyl = &slot->collider;

    // Use the exact same damage properties as Link's current attack (sword type, spin, jump attack, etc.)
    cyl->elem.atDmgInfo = player->meleeWeaponQuads[0].elem.atDmgInfo;
    cyl->elem.atElemFlags = ATELEM_ON | ATELEM_SFX_NORMAL;
    cyl->base.atFlags = AT_ON | AT_TYPE_PLAYER;

    // Cover the enemy from its feet up past its focus point (usually the head) so the cylinder overlaps its hurtbox
    f32 bottom = MIN(target->world.pos.y, target->focus.pos.y) - 40.0f;
    f32 top = MAX(target->world.pos.y, target->focus.pos.y) + 60.0f;
    f32 height = CLAMP(top - bottom, 100.0f, 1000.0f);

    cyl->dim.radius = 80;
    cyl->dim.height = (s16)height;
    cyl->dim.yShift = 0;
    cyl->dim.pos.x = (s16)target->world.pos.x;
    cyl->dim.pos.y = (s16)bottom;
    cyl->dim.pos.z = (s16)target->world.pos.z;

    if (CollisionCheck_SetAT(gPlayState, &gPlayState->colChkCtx, &cyl->base) >= 0) {
        slot->target = target;
        KeepActorUpdating(target);
    }
}

static void OnPlayerUpdate(Actor* actor) {
    Player* player = (Player*)actor;
    s32 mode = CVAR;

    if (gPlayState == nullptr || mode == HIT_ANYWHERE_OFF) {
        return;
    }

    if (!sSlotsInitialized) {
        InitSlots(player);
    }

    // Collision for last frame's attacks has now been processed, record which enemies were actually struck so they
    // are not hit again during the same swing.
    for (auto& slot : sSlots) {
        if (slot.target != nullptr && (slot.collider.base.atFlags & AT_HIT)) {
            sHitThisSwing.insert(slot.target);
            if (slot.collider.base.at != nullptr) {
                sHitThisSwing.insert(slot.collider.base.at);
            }
        }
        slot.target = nullptr;
    }

    TickForcedUpdateActors();

    s8 meleeWeaponState = player->meleeWeaponState;
    s32 meleeWeaponAnimation = player->meleeWeaponAnimation;

    // No active attack: forget who was hit so the next swing can hit them again
    if (meleeWeaponState < PLAYER_MELEE_WEAPON_STATE_1) {
        sHitThisSwing.clear();
        sPrevMeleeWeaponState = meleeWeaponState;
        sPrevMeleeWeaponAnimation = meleeWeaponAnimation;
        return;
    }

    // A new swing (or the next hit of a combo) starts fresh
    if (sPrevMeleeWeaponState < PLAYER_MELEE_WEAPON_STATE_1 || sPrevMeleeWeaponAnimation != meleeWeaponAnimation) {
        sHitThisSwing.clear();
    }
    sPrevMeleeWeaponState = meleeWeaponState;
    sPrevMeleeWeaponAnimation = meleeWeaponAnimation;

    s32 slotIndex = 0;

    if (mode == HIT_ANYWHERE_TARGETED) {
        Actor* target = player->focusActor;
        if (IsValidTarget(player, target)) {
            PlaceAttackOnTarget(player, &sSlots[slotIndex++], target);
        }
    } else if (mode == HIT_ANYWHERE_ALL_ENEMIES) {
        static const s32 sTargetCategories[] = { ACTORCAT_ENEMY, ACTORCAT_BOSS };

        for (s32 category : sTargetCategories) {
            for (Actor* target = gPlayState->actorCtx.actorLists[category].first;
                 target != nullptr && slotIndex < HIT_ANYWHERE_MAX_TARGETS; target = target->next) {
                if (IsValidTarget(player, target)) {
                    PlaceAttackOnTarget(player, &sSlots[slotIndex++], target);
                }
            }
        }
    }
}

void RegisterHitAnywhere() {
    if (!CVAR) {
        // Turning the cheat off: put any far-away enemies we woke up back to normal
        RestoreForcedUpdateActors();
        sHitThisSwing.clear();
    }

    COND_ID_HOOK(OnActorUpdate, ACTOR_PLAYER, CVAR, OnPlayerUpdate);

    // Forget actors as they are destroyed so a stale pointer can never touch a new actor reusing the same memory
    COND_HOOK(OnActorDestroy, CVAR, [](Actor* actor) {
        sForcedUpdateActors.erase(actor);
        sHitThisSwing.erase(actor);
        for (auto& slot : sSlots) {
            if (slot.target == actor) {
                slot.target = nullptr;
            }
        }
    });

    // Colliders and actor pointers belong to the current PlayState, so drop everything when it is destroyed
    COND_HOOK(OnPlayDestroy, CVAR, []() { ResetHitAnywhereState(); });
}

static RegisterShipInitFunc initFunc(RegisterHitAnywhere, { CVAR_NAME });
