#include "script_pokemon_util.h"

#include "global.h"

#include "constants/battle.h"
#include "constants/items.h"

#include "get_egg.h"
#include "math_util.h"
#include "party.h"
#include "player_data.h"
#include "pokemon_mood.h"
#include "unk_0206979C.h"
#include "update_dex_received.h"

// Custom shiny odds for gift Pokemon, same idea as wild encounters: the gift rolls once whether it is meant to be shiny
// (1 in GIFT_SHINY_ODDS), then the RNG is moved forward to a spot whose PID has that shininess. CreateMon then rolls
// the PID and IVs from that spot exactly as it always does, so the gift keeps an unbroken PID -> IV RNG chain and
// stays legal. Shininess is judged against the player's full 32-bit trainer ID.
#define GIFT_SHINY_ODDS         4096
#define GIFT_SHINY_SEARCH_LIMIT 400000 // RNG steps searched before giving up (about 8192 are needed on average)

static u32 GiftNextSeed(u32 seed);
static void SeekGiftPersonality(u32 otId, BOOL wantShiny);

// One step of the game's LCRNG (same constants as LCRandom).
static u32 GiftNextSeed(u32 seed) {
    return seed * 1103515245 + 24691;
}

// Moves the RNG to the first spot where the next two outputs (low half, then high half) make a PID whose shininess
// matches wantShiny. Nothing is generated here, the RNG just ends up where an unmodified game could have been.
static void SeekGiftPersonality(u32 otId, BOOL wantShiny) {
    u32 seed = GetLCRNGSeed();

    for (u32 n = 0; n < GIFT_SHINY_SEARCH_LIMIT; ++n) {
        u32 seed1 = GiftNextSeed(seed);
        u32 seed2 = GiftNextSeed(seed1);
        u32 personality = (seed1 >> 16) | (seed2 & 0xFFFF0000);

        if ((CalcShininessByOtIdAndPersonality(otId, personality) != 0) == wantShiny) {
            break;
        }
        seed = seed1;
    }

    SetLCRNGSeed(seed);
}

static BOOL MonNotFaintedOrEgg(Pokemon *mon) {
    if (GetMonData(mon, MON_DATA_HP, NULL) == 0) {
        return FALSE;
    }
    return !GetMonData(mon, MON_DATA_IS_EGG, NULL);
}

BOOL GiveMon(enum HeapID heapID, SaveData *saveData, int species, int level, int form, u8 ability, u16 heldItem, int ball, int encounterType) {
    Party *party;
    Pokemon *mon;
    PlayerProfile *profile;
    u32 sp1C;
    BOOL result;

    profile = Save_PlayerData_GetProfile(saveData);
    party = SaveArray_Party_Get(saveData);
    {
        mon = AllocMonZeroed(heapID);
        ZeroMonData(mon);
        BOOL wantShiny = LCRandRange(GIFT_SHINY_ODDS) == 0;
        SeekGiftPersonality(PlayerProfile_GetTrainerID(profile), wantShiny);
        CreateMon(mon, species, level, 32, FALSE, 0, 0, 0);
        sub_020720FC(mon, profile, ITEM_POKE_BALL, ball, encounterType, heapID);
        sp1C = heldItem;
        SetMonData(mon, MON_DATA_HELD_ITEM, &sp1C);
        SetMonData(mon, MON_DATA_FORM, &form);
        if (ability != 0) {
            SetMonData(mon, MON_DATA_ABILITY, &ability);
        }
        result = Party_AddMon(party, mon);
        if (result) {
            UpdatePokedexWithReceivedSpecies(saveData, mon);
        }
        Heap_Free(mon);
    }
    return result;
}

BOOL GiveEgg(enum HeapID heapID, SaveData *saveData, int species, u8 metLocation, MapsecType mapsecType, int maploc) {
#pragma unused(heapID)
    PlayerProfile *profile;
    Party *party;
    Pokemon *mon;
    BOOL result;

    profile = Save_PlayerData_GetProfile(saveData);
    party = SaveArray_Party_Get(saveData);
    mon = AllocMonZeroed(HEAP_ID_FIELD3);
    ZeroMonData(mon);
    SetEggStats(mon, species, metLocation, profile, 4, sub_02017FE4(mapsecType, maploc));
    result = Party_AddMon(party, mon);
    Heap_Free(mon);
    return result;
}

void PartyMonSetMoveInSlot(Party *party, int mon_slot, int move_slot, u16 moveId) {
    MonSetMoveInSlot_ResetPpUp(Party_GetMonByIndex(party, mon_slot), moveId, move_slot);
}

int GetIdxOfFirstPartyMonWithMove(Party *party, u16 move) {
    int i;
    int n;
    Pokemon *mon;

    n = Party_GetCount(party);
    for (i = 0; i < n; i++) {
        mon = Party_GetMonByIndex(party, i);
        if (GetMonData(mon, MON_DATA_IS_EGG, NULL)) {
            continue;
        }
        if (GetMonData(mon, MON_DATA_MOVE1, NULL) == move
            || GetMonData(mon, MON_DATA_MOVE2, NULL) == move
            || GetMonData(mon, MON_DATA_MOVE3, NULL) == move
            || GetMonData(mon, MON_DATA_MOVE4, NULL) == move) {
            return i;
        }
    }
    return 0xFF;
}

int CountAlivePokemon(Party *party) {
    int i;
    int n;
    int c;
    Pokemon *mon;

    n = Party_GetCount(party);
    c = 0;
    for (i = 0; i < n; i++) {
        mon = Party_GetMonByIndex(party, i);
        if (MonNotFaintedOrEgg(mon)) {
            c++;
        }
    }
    return c;
}

Pokemon *GetFirstAliveMonInParty_CrashIfNone(Party *party) {
    int i;
    int n;
    Pokemon *mon;

    n = Party_GetCount(party);
    for (i = 0; i < n; i++) {
        mon = Party_GetMonByIndex(party, i);
        if (MonNotFaintedOrEgg(mon)) {
            return mon;
        }
    }
    GF_ASSERT(FALSE);
    return NULL;
}

u8 GetIdxOfFirstAliveMonInParty_CrashIfNone(Party *party) {
    int i;
    int n;
    Pokemon *mon;

    n = Party_GetCount(party);
    for (i = 0; i < n; i++) {
        mon = Party_GetMonByIndex(party, i);
        if (MonNotFaintedOrEgg(mon)) {
            return i;
        }
    }
    GF_ASSERT(FALSE);
    return 0;
}

Pokemon *GetFirstNonEggInParty(Party *party) {
    u16 i;
    u16 n;
    Pokemon *mon;

    n = Party_GetCount(party);
    for (i = 0; i < n; i++) {
        mon = Party_GetMonByIndex(party, i);
        if (!GetMonData(mon, MON_DATA_IS_EGG, NULL)) {
            return mon;
        }
    }
    return NULL;
}

BOOL HasEnoughAlivePokemonForDoubleBattle(Party *party) {
    return CountAlivePokemon(party) >= 2;
}

BOOL ApplyPoisonStep(Party *party, u16 location) {
    int n;
    int i;
    int n_poisoned;
    int n_fainted;
    u32 hp;
    Pokemon *mon;

    n_poisoned = 0;
    n_fainted = 0;
    n = Party_GetCount(party);
    for (i = 0; i < n; i++) {
        mon = Party_GetMonByIndex(party, i);
        if (!MonNotFaintedOrEgg(mon)) {
            continue;
        }
        if (!(GetMonData(mon, MON_DATA_STATUS, NULL) & (STATUS_POISON | STATUS_BAD_POISON))) {
            continue;
        }
        hp = GetMonData(mon, MON_DATA_HP, NULL);
        if (hp > 1) {
            hp--;
        }
        SetMonData(mon, MON_DATA_HP, &hp);
        if (hp == 1) {
            n_fainted++;
            MonApplyFriendshipMod(mon, FRIENDSHIP_EVENT_HEAL_FIELD_PSN, location);
            ApplyMonMoodModifier(mon, MON_MOOD_MODIFIER_SURVIVED_PSN);
        }
        n_poisoned++;
    }
    if (n_fainted != 0) {
        return 2;
    } else if (n_poisoned != 0) {
        return 1;
    } else {
        return 0;
    }
}

BOOL SurvivePoisoning(Pokemon *mon) {
    u32 status;
    if ((GetMonData(mon, MON_DATA_STATUS, NULL) & (STATUS_POISON | STATUS_BAD_POISON)) && GetMonData(mon, MON_DATA_HP, NULL) == 1) {
        status = 0;
        SetMonData(mon, MON_DATA_STATUS, &status);
        return TRUE;
    } else {
        return FALSE;
    }
}