#include <libultraship/bridge/consolevariablebridge.h>
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "variables.h"
}

#define CVAR_NAME "gCheats.HumanSwordBeams"
#define CVAR CVarGetInteger(CVAR_NAME, 0)

// Lets human Link fire sword beams the same way Fierce Deity does: swing the sword while Z-targeting. Like Fierce
// Deity, each beam costs magic, so Link must have the magic meter and some magic left.
void RegisterHumanSwordBeams() {
    COND_VB_SHOULD(VB_FIRE_SWORD_BEAM, CVAR, {
        Player* player = va_arg(args, Player*);
        s32 isZTargeting = va_arg(args, s32);

        // EnMThunder derives its sword type from (heldItemAction - PLAYER_IA_SWORD_KOKIRI), so only allow real
        // swords (Kokiri, Razor, Gilded, Great Fairy's Sword) and never Deku Sticks or other held items.
        if ((player->transformation == PLAYER_FORM_HUMAN) && isZTargeting &&
            (player->heldItemAction >= PLAYER_IA_SWORD_KOKIRI) &&
            (player->heldItemAction <= PLAYER_IA_SWORD_TWO_HANDED)) {
            *should = true;
        }
    });
}

static RegisterShipInitFunc initFunc(RegisterHumanSwordBeams, { CVAR_NAME });
