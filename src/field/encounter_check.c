#include "field/encounter_check.h"

#include "global.h"

#include "constants/abilities.h"
#include "constants/maps.h"
#include "constants/weather.h"

#include "battle/battle_setup.h"
#include "overlay_2/overlay_02_gear_phone.h"

#include "alph_puzzle.h"
#include "bug_contest.h"
#include "encounter.h"
#include "field_roamer.h"
#include "follow_mon.h"
#include "gf_rtc.h"
#include "map_events.h"
#include "math_util.h"
#include "metatile_behavior.h"
#include "overlay_01.h"
#include "overlay_02.h"
#include "poke_overlay.h"
#include "roamer.h"
#include "safari_zone.h"
#include "script_pokemon_util.h"
#include "sys_flags.h"
#include "sys_vars.h"
#include "unk_02054648.h"
#include "unk_0205CB48.h"
#include "unk_0206D494.h"
#include "unk_02097268.h"
#include "unk_02097F6C.h"
#include "wild_encounter.h"

#define ENCOUNTER_TYPE_LAND       0
#define ENCOUNTER_TYPE_SURFING    1
#define ENCOUNTER_TYPE_FISHING    2
#define ENCOUNTER_TYPE_ROCK_SMASH 3
#define ENCOUNTER_TYPE_HEADBUTT   4

#define ENCOUNTER_CONTEXT_REGULAR     0
#define ENCOUNTER_CONTEXT_SAFARI      1
#define ENCOUNTER_CONTEXT_BUG_CONTEST 2

// Custom shiny odds. Every wild mon rolls once, up front, whether it is meant to be shiny (1 in WILD_SHINY_ODDS).
// A shiny mon is then found by searching the RNG for a slot -> level -> nature -> PID chain that an unmodified
// encounter could have rolled and that ends on a shiny PID (see searchShinyAttempt). A mon that is not meant to be
// shiny re-rolls the whole attempt in the rare case its PID is shiny. IVs are only ever rolled for the accepted
// attempt, so the mon still has an unbroken slot -> level -> nature -> PID -> IV RNG chain, like an unmodified one.
#define WILD_SHINY_ODDS          256
#define WILD_SHINY_MAX_ATTEMPTS  120000
#define WILD_SHINY_SEARCH_WINDOW 128     // RNG states searched behind each shiny PID for an encounter start
#define WILD_SHINY_SEARCH_LIMIT  3000000 // RNG steps searched before giving up (about 8192 are needed on average, 50000 for Safari and Bug Contest, 400000 with Synchronize)

enum {
    WILD_SHINY_REQ_NONE,
    WILD_SHINY_REQ_SHINY,
    WILD_SHINY_REQ_NOT_SHINY,
};

FS_EXTERN_OVERLAY(bug_contest);

typedef struct UnlockedUnown {
    int num;
    const u8 *letters;
} UnlockedUnown;

typedef struct EncounterGenState {
    u32 playerId;
    BOOL isRepel;
    BOOL isSweetScent;
    u8 level;
    u8 isEgg;
    u8 ability;
    u8 unk_0F;
    u8 unk_10;
    u8 isSinjohMap;
    u8 unownPuzzleFlags[ALPH_PUZZLE_MAX];
    Pokedex *pokedex;
} EncounterGenState;

enum {
    WILD_ATTEMPT_REGULAR,
    WILD_ATTEMPT_SAFARI,
    WILD_ATTEMPT_BUG_CONTEST,
    WILD_ATTEMPT_SINGLE,
};

enum {
    WILD_ROLL_BLOCKED,     // suppressed (Repel, Keen Eye/Intimidate, Unown): an unmodified game makes no encounter
    WILD_ROLL_PERSONALITY, // slot, level, nature and PID are rolled, IVs are not
    WILD_ROLL_CUTE_CHARM,  // fixed-form PID, built by CreateMonWithGenderNatureLetter
    WILD_ROLL_RETRY,       // Synchronize's roll missed: the encounter is rolled again from the slot roll
};

typedef struct WildAttempt {
    u8 kind;
    u8 encType;
    int rodType;
    FieldSystem *fieldSystem;
    Pokemon *leadMon;
    EncounterGenState *encounterGen;
    EncounterSlot *encSlots;
    u16 species;
    u8 level;
    u32 personality;
} WildAttempt;

static void EncSlotArray_Init_Land(const EncounterData *encData, EncounterSlot *encSlots);
static void EncSlotArray_Update_NightFishing(const EncounterData *encData, u8 rodType, EncounterSlot *encSlots);
static void EncSlotArray_Update_HoennSinnohSounds(FieldSystem *fieldSystem, const EncounterData *encData, EncounterSlot *encSlots);
static void EncSlots_Update_LandSwarm(FieldSystem *fieldSystem, const EncounterData *encData, EncounterSlot *encSlotA, EncounterSlot *encSlotB);
static void EncSlots_Update_SurfingSwarm(FieldSystem *fieldSystem, const EncounterData *encData, EncounterSlot *encSlot);
static void EncSlots_Update_FishingSwarm(FieldSystem *fieldSystem, u8 rodType, const EncounterData *encData, EncounterSlot *encSlots);
static BOOL FieldSystem_GenerateLandRegularEncounter(FieldSystem *fieldSystem, Pokemon *leadMon, BattleSetup *battleSetup, const EncounterData *encData, EncounterSlot *encSlots, EncounterGenState *encounterGen);
static BOOL FieldSystem_GenerateLandSafariEncounter(FieldSystem *fieldSystem, Pokemon *leadMon, BattleSetup *battleSetup, const EncounterData *encData, EncounterSlot *encSlots, EncounterGenState *encounterGen);
static BOOL FieldSystem_GenerateBugContestEncounter(FieldSystem *fieldSystem, Pokemon *leadMon, BattleSetup *battleSetup, const EncounterData *encData, EncounterSlot *encSlots, EncounterGenState *encounterGen);
static BOOL FieldSystem_GenerateDoubleEncounter(FieldSystem *fieldSystem, Pokemon *leadMon, BattleSetup *battleSetup, EncounterSlot *encSlots, EncounterGenState *encounterGen);
static BOOL FieldSystem_GenerateSurfingEncounter(FieldSystem *fieldSystem, Pokemon *leadMon, BattleSetup *battleSetup, EncounterSlot *encSlots, EncounterGenState *encounterGen, BOOL isSafari);
static BOOL FieldSystem_GenerateFishingEncounter(FieldSystem *fieldSystem, Pokemon *leadMon, BattleSetup *battleSetup, EncounterSlot *encSlots, EncounterGenState *encounterGen, int rodType, BOOL isSafari);
static BOOL FieldSystem_GenerateRockSmashEncounter(FieldSystem *fieldSystem, Pokemon *leadMon, BattleSetup *battleSetup, EncounterSlot *encSlots, EncounterGenState *encounterGen);
static BOOL FieldSystem_EncounterRateRoll(FieldSystem *fieldSystem, u8 encounterRate, u8 metatileBehavior);
static u8 getStepCountEncounterRateBoost(u16 stepCount);
static u8 FieldSystem_GetTileEncounterTypeAndRate(FieldSystem *fieldSystem, u8 metatileBehavior, u8 *pEncType);
static BOOL FieldSystem_SecondEncounterRoll(FieldSystem *fieldSystem, u16 encounterRate);
static u8 EncounterSlot_WildMonSlotRoll_Land(void);
static u8 EncounterSlot_WildMonSlotRoll_Surfing(void);
static u8 EncounterSlot_WildMonSlotRoll_Fishing(int rodType);
static u8 EncounterSlot_WildMonSlotRoll_RockSmash(void);
static u8 EncounterSlot_WildMonSlotRoll_Headbutt(void);
static void ApplyLeadMonHeldItemEffectToEncounterRate(Pokemon *leadMon, u8 *pEncounterRate);
static void ApplyFluteEffectToEncounterRate(FieldSystem *fieldSystem, u8 *pEncounterRate);
static u8 getWildMonNature(Pokemon *pokemon, EncounterGenState *encounterGen);
static BOOL rollWildNature(Pokemon *pokemon, EncounterGenState *encounterGen, u8 *nature);
static u8 EncounterSlot_WildMonLevelRoll(EncounterSlot *encSlot, EncounterGenState *encounterGen);
static void generateWildShinyAndAddToParty(u16 species, u8 level, int battler, u32 otid, EncounterGenState *encounterGen, Pokemon *leadMon, BattleSetup *battleSetup);
static BOOL rollWildShiny(void);
static u32 findWildPersonality(u8 nature);
static BOOL wildPersonalityMeetsShinyRequirement(u32 otId, u32 personality, int shinyRequirement);
static BOOL FieldSystem_GenerateRegularEncounter(Pokemon *leadMon, int rodType, EncounterGenState *encounterGen, EncounterSlot *encSlots, u8 encType, int battler, BattleSetup *battleSetup);
static BOOL FieldSystem_GenerateSafariEncounter(FieldSystem *fieldSystem, Pokemon *leadMon, int rodType, EncounterGenState *encounterGen, u8 encType, int battler, BattleSetup *battleSetup);
static BOOL FieldSystem_GenerateBugContestEncounter_Internal(FieldSystem *fieldSystem, Pokemon *leadMon, int rodType, EncounterGenState *encounterGen, u8 encType, int battler, BattleSetup *battleSetup);
static int FieldSystem_GetLandEncounterRate(FieldSystem *fieldSystem);
static int FieldSystem_GetSurfingEncounterRate(FieldSystem *fieldSystem);
static u8 FieldSystem_GetFishingEncounterRate(FieldSystem *fieldSystem, u8 rodType);
static u8 getFriendshipBoostToFishingBiteRate(u8 friendship);
static int FieldSystem_GetRockSmashEncounterRate(FieldSystem *fieldSystem);
static BOOL chooseAbilityCoercedSlot(EncounterSlot *encSlots, u8 numEncSlots, u8 type, u8 *slot);
static BOOL EncounterSlot_AbilityInfluenceOnSlotChoiceCheck(Pokemon *leadMon, EncounterGenState *encounterGen, EncounterSlot *encSlots, u8 numSlots, u8 type, u8 ability, u8 *slotNum);
static u8 ApplyAbilityEffectToEncounterRate(BOOL isFishing, u8 encounterRate, EncounterGenState *encounterGen, u16 weatherType, Pokemon *leadMon);
static BOOL DoesAbilitySuppressEncounter(EncounterGenState *encounterGen, Pokemon *leadMon, u8 level);
static void FieldSystem_CreateBattleSetupForWildBattle(FieldSystem *fieldSystem, u8 encounterCtx, BattleSetup **pBattleSetup);
static BOOL EncounterGen_DoesRepelSuppressEncounter(u8 roamerLevel, EncounterGenState *encounterGen);
static BOOL EncounterGen_CanGenerateUnownEncounter(EncounterGenState *encounterGen);
static void initRoamingWildmon(u32 otId, Roamer *roamer, BattleSetup *battleSetup);
static BOOL getRandomActiveRoamerInCurrMap(FieldSystem *fieldSystem, Roamer **pRoamer);
static BOOL hasCaughtUnownForm(Pokedex *pokedex, u8 numForms, u8 targetForm);
static u8 EncounterGen_ChooseUnownForm(EncounterGenState *encounterGen);
static BOOL addGeneratedMonToBattleSetupParty(int battler, EncounterGenState *encounterGen, Pokemon *pokemon, BattleSetup *battleSetup);
static u8 ApplyAbilityEffectToSlotLevel(EncounterSlot *encSlots, int numEncSlots, EncounterGenState *encounterGen, u8 chosenSlot);
static void EncounterGenState_Init(FieldSystem *fieldSystem, Pokemon *pokemon, const EncounterData *encData, EncounterGenState *encounterGen);
static void FieldSystem_UpdateTurnFrameCounter(FieldSystem *fieldSystem);

static void EncSlotArray_Init_Land(const EncounterData *encData, EncounterSlot *encSlots) {
    TIMEOFDAY timeOfDay = GF_RTC_GetTimeOfDay();
    const u16 *slotSpecies;

    if (timeOfDay == RTC_TIMEOFDAY_MORN) {
        slotSpecies = encData->landSlots.species_morn;
    } else if (timeOfDay == RTC_TIMEOFDAY_DAY || timeOfDay == RTC_TIMEOFDAY_EVE) {
        slotSpecies = encData->landSlots.species_day;
    } else if (timeOfDay == RTC_TIMEOFDAY_NITE || timeOfDay == RTC_TIMEOFDAY_LATE) {
        slotSpecies = encData->landSlots.species_nite;
    } else {
        GF_ASSERT(FALSE);
        slotSpecies = encData->landSlots.species_morn;
    }
    for (int i = 0; i < NUM_ENCOUNTERS_LAND; ++i) {
        encSlots[i].species = slotSpecies[i];
        encSlots[i].maxLevel = encData->landSlots.levels[i];
        encSlots[i].minLevel = encData->landSlots.levels[i];
    }
}

static void EncSlotArray_Update_NightFishing(const EncounterData *encData, u8 rodType, EncounterSlot *encSlots) {
    TIMEOFDAY timeOfDay = GF_RTC_GetTimeOfDay();

    if (timeOfDay == RTC_TIMEOFDAY_NITE || timeOfDay == RTC_TIMEOFDAY_LATE) {
        if (rodType == ROD_TYPE_GOOD) {
            encSlots[3].species = encData->nightFish;
        } else if (rodType == ROD_TYPE_SUPER) {
            encSlots[1].species = encData->nightFish;
        }
    }
}

static void EncSlotArray_Update_HoennSinnohSounds(FieldSystem *fieldSystem, const EncounterData *encData, EncounterSlot *encSlots) {
    if (GetRadioMusicPlayingSeq() == RADIO_MUSIC_SEQ_HOENN_SOUNDS) {
        encSlots[2].species = encData->hoennSoundsSpecies[0];
        encSlots[3].species = encData->hoennSoundsSpecies[0];
        encSlots[4].species = encData->hoennSoundsSpecies[1];
        encSlots[5].species = encData->hoennSoundsSpecies[1];
    } else if (GetRadioMusicPlayingSeq() == RADIO_MUSIC_SEQ_SINNOH_SOUNDS) {
        encSlots[2].species = encData->sinnohSoundsSpecies[0];
        encSlots[3].species = encData->sinnohSoundsSpecies[0];
        encSlots[4].species = encData->sinnohSoundsSpecies[1];
        encSlots[5].species = encData->sinnohSoundsSpecies[1];
    }
}

static void EncSlots_Update_LandSwarm(FieldSystem *fieldSystem, const EncounterData *encData, EncounterSlot *encSlotA, EncounterSlot *encSlotB) {
    RoamerSaveData *roamerSave = Save_Roamers_Get(fieldSystem->saveData);
    if (RoamerSave_OutbreakActive(roamerSave) && sub_02097F6C(Roamers_GetRand(roamerSave, 2), fieldSystem->location->mapId, 0)) {
        encSlotA->species = encData->landSwarm;
        encSlotB->species = encData->landSwarm;
    }
}

static void EncSlots_Update_SurfingSwarm(FieldSystem *fieldSystem, const EncounterData *encData, EncounterSlot *encSlot) {
    RoamerSaveData *roamerSave = Save_Roamers_Get(fieldSystem->saveData);
    if (RoamerSave_OutbreakActive(roamerSave) && sub_02097F6C(Roamers_GetRand(roamerSave, 2), fieldSystem->location->mapId, 1)) {
        encSlot->species = encData->surfSwarm;
    }
}

static void EncSlots_Update_FishingSwarm(FieldSystem *fieldSystem, u8 rodType, const EncounterData *encData, EncounterSlot *encSlots) {
    RoamerSaveData *roamerSave = Save_Roamers_Get(fieldSystem->saveData);
    if (RoamerSave_OutbreakActive(roamerSave) && sub_02097F6C(Roamers_GetRand(roamerSave, 2), fieldSystem->location->mapId, 2)) {
        int sOldRodSlotIdxs[1] = { 2 };
        int sGoodRodSlotIdxs[3] = { 0, 2, 3 };
        int sSuperRodSlotIdxs[5] = { 0, 1, 2, 3, 4 };

        const int *slots;
        u32 numSlots;

        switch (rodType) {
        case ROD_TYPE_OLD:
            numSlots = NELEMS(sOldRodSlotIdxs);
            slots = sOldRodSlotIdxs;
            break;
        case ROD_TYPE_GOOD:
            numSlots = NELEMS(sGoodRodSlotIdxs);
            slots = sGoodRodSlotIdxs;
            break;
        case ROD_TYPE_SUPER:
            numSlots = NELEMS(sSuperRodSlotIdxs);
            slots = sSuperRodSlotIdxs;
            break;
        default:
            GF_ASSERT(FALSE);
            return;
        }

        for (u8 i = 0; i < numSlots; ++i) {
            encSlots[slots[i]].species = encData->fishSwarm;
        }
    }
}

BOOL FieldSystem_PerformLandOrSurfEncounterCheck(FieldSystem *fieldSystem) {
    if (fieldSystem->encounterInhibitSteps <= 3) {
        return FALSE;
    }

    EncounterSlot encSlots[NUM_ENCOUNTERS_LAND];
    EncounterGenState encounterGen;
    BattleSetup *battleSetup;
    Roamer *roamer;
    u8 encounterType;
    u8 encounterRate;

    u32 x = PlayerAvatar_GetXCoord(fieldSystem->playerAvatar);
    u32 z = PlayerAvatar_GetZCoord(fieldSystem->playerAvatar);
    u8 metatileBehavior = GetMetatileBehavior(fieldSystem, x, z);
    encounterRate = FieldSystem_GetTileEncounterTypeAndRate(fieldSystem, metatileBehavior, &encounterType);
    if (encounterRate == 0) {
        return FALSE;
    }
    FieldSystem_UpdateTurnFrameCounter(fieldSystem);
    Party *party = SaveArray_Party_Get(fieldSystem->saveData);
    EncounterData *encData = MapEvents_GetLoadedEncTable(fieldSystem);
    Pokemon *leadMon = Party_GetMonByIndex(party, 0);
    EncounterGenState_Init(fieldSystem, leadMon, encData, &encounterGen);
    if (!RoamerSave_RepelNotInUse(Save_Roamers_Get(fieldSystem->saveData))) {
        Pokemon *mon = GetFirstAliveMonInParty_CrashIfNone(party);
        encounterGen.isRepel = TRUE;
        encounterGen.level = GetMonData(mon, MON_DATA_LEVEL, NULL);
    }
    encounterRate = ApplyAbilityEffectToEncounterRate(FALSE, encounterRate, &encounterGen, LocalFieldData_GetWeatherType(Save_LocalFieldData_Get(fieldSystem->saveData)), leadMon);
    ApplyFluteEffectToEncounterRate(fieldSystem, &encounterRate);
    ApplyLeadMonHeldItemEffectToEncounterRate(leadMon, &encounterRate);
    BOOL ret = FieldSystem_EncounterRateRoll(fieldSystem, encounterRate, metatileBehavior) ? TRUE : FALSE;
    if (!ret) {
        return FALSE;
    }

    // followerFlag is a holdover from the Sinnoh games.
    // It indicates whether you have Cheryl, Marley, Mira, Riley, or Buck with you.
    // If so, wild battles are double battles with you fighting along the given companion.
    // This never happens in HGSS, so it is always FALSE.
    BOOL followerFlag = Save_VarsFlags_CheckHaveFollower(Save_VarsFlags_Get(fieldSystem->saveData)) ? TRUE : FALSE;
    if (!followerFlag && getRandomActiveRoamerInCurrMap(fieldSystem, &roamer)) {
        if (!EncounterGen_DoesRepelSuppressEncounter(GetRoamerData(roamer, ROAMER_DATA_LEVEL), &encounterGen)) {
            battleSetup = BattleSetup_New(HEAP_ID_FIELD2, BATTLE_TYPE_ROAMER);
            BattleSetup_InitFromFieldSystem(battleSetup, fieldSystem);
            initRoamingWildmon(encounterGen.playerId, roamer, battleSetup);
            sub_02050B08(fieldSystem, battleSetup);
            return TRUE;
        }
        return FALSE;
    }

    SaveVarsFlags *saveVarsFlags = Save_VarsFlags_Get(fieldSystem->saveData);
    BOOL safariFlag = Save_VarsFlags_CheckSafariSysFlag(saveVarsFlags);
    BOOL bugContestFlag = Save_VarsFlags_CheckBugContestFlag(saveVarsFlags);

    if (!followerFlag) {
        u8 encounterCtx = ENCOUNTER_CONTEXT_REGULAR;
        if (safariFlag) {
            encounterCtx = ENCOUNTER_CONTEXT_SAFARI;
        } else if (bugContestFlag) {
            encounterCtx = ENCOUNTER_CONTEXT_BUG_CONTEST;
        }
        FieldSystem_CreateBattleSetupForWildBattle(fieldSystem, encounterCtx, &battleSetup);
    } else {
        battleSetup = BattleSetup_New(HEAP_ID_FIELD2, BATTLE_TYPE_DOUBLES | BATTLE_TYPE_MULTI | BATTLE_TYPE_AI);
    }
    BattleSetup_InitFromFieldSystem(battleSetup, fieldSystem);
    BOOL found;
    if (encounterType == ENCOUNTER_TYPE_LAND) {
        EncSlotArray_Init_Land(encData, encSlots);
        EncSlots_Update_LandSwarm(fieldSystem, encData, &encSlots[0], &encSlots[1]);
        EncSlotArray_Update_HoennSinnohSounds(fieldSystem, encData, encSlots);
        if (!followerFlag) {
            if (safariFlag) {
                found = FieldSystem_GenerateLandSafariEncounter(fieldSystem, leadMon, battleSetup, encData, encSlots, &encounterGen);
            } else if (bugContestFlag) {
                found = FieldSystem_GenerateBugContestEncounter(fieldSystem, leadMon, battleSetup, encData, encSlots, &encounterGen);
            } else {
                found = FieldSystem_GenerateLandRegularEncounter(fieldSystem, leadMon, battleSetup, encData, encSlots, &encounterGen);
            }
        } else {
            battleSetup->trainerId[BATTLER_PLAYER2] = Save_VarsFlags_GetFollowerTrainerNum(Save_VarsFlags_Get(fieldSystem->saveData));
            EnemyTrainerSet_Init(battleSetup, fieldSystem->saveData, HEAP_ID_FIELD2);
            found = FieldSystem_GenerateDoubleEncounter(fieldSystem, leadMon, battleSetup, encSlots, &encounterGen);
        }
    } else if (encounterType == ENCOUNTER_TYPE_SURFING) {
        for (int i = 0; i < NUM_ENCOUNTERS_SURF; ++i) {
            encSlots[i].species = encData->surfSlots[i].species;
            encSlots[i].maxLevel = encData->surfSlots[i].level_max;
            encSlots[i].minLevel = encData->surfSlots[i].level_min;
        }
        EncSlots_Update_SurfingSwarm(fieldSystem, encData, encSlots);
        found = FieldSystem_GenerateSurfingEncounter(fieldSystem, leadMon, battleSetup, encSlots, &encounterGen, safariFlag);
    } else {
        GF_ASSERT(FALSE);
        BattleSetup_Delete(battleSetup);
        return FALSE;
    }
    if (found) {
        sub_02050B08(fieldSystem, battleSetup);
        fieldSystem->encounterInhibitSteps = 0;
        fieldSystem->reverseTurnFrameSteps = 0;
    } else {
        ret = FALSE;
    }
    GF_ASSERT(GF_heap_c_dummy_return_true(HEAP_ID_FIELD1));
    GF_ASSERT(GF_heap_c_dummy_return_true(HEAP_ID_FIELD2));
    if (!ret) {
        BattleSetup_Delete(battleSetup);
    }
    return ret;
}

BOOL FieldSystem_PerformFishEncounterCheck(FieldSystem *fieldSystem, u8 rodType, BattleSetup **pBattleSetup) {
    EncounterSlot encSlots[NUM_ENCOUNTERS_LAND];
    EncounterGenState encounterGen;

    u8 encounterRate = FieldSystem_GetFishingEncounterRate(fieldSystem, rodType);
    if (encounterRate == 0) {
        return FALSE;
    }

    Pokemon *leadMon = Party_GetMonByIndex(SaveArray_Party_Get(fieldSystem->saveData), 0);
    EncounterGenState_Init(fieldSystem, leadMon, NULL, &encounterGen);
    encounterRate = ApplyAbilityEffectToEncounterRate(TRUE, encounterRate, &encounterGen, LocalFieldData_GetWeatherType(Save_LocalFieldData_Get(fieldSystem->saveData)), leadMon);
    if (LCRandRange(100) >= encounterRate) {
        return FALSE;
    }
    BOOL safariFlag = Save_VarsFlags_CheckSafariSysFlag(Save_VarsFlags_Get(fieldSystem->saveData));
    if (safariFlag) {
        FieldSystem_CreateBattleSetupForWildBattle(fieldSystem, ENCOUNTER_CONTEXT_SAFARI, pBattleSetup);
    } else {
        FieldSystem_CreateBattleSetupForWildBattle(fieldSystem, ENCOUNTER_CONTEXT_REGULAR, pBattleSetup);
    }
    BattleSetup_InitFromFieldSystem(*pBattleSetup, fieldSystem);
    sub_02052544(*pBattleSetup);
    const EncounterData *encData = MapEvents_GetLoadedEncTable(fieldSystem);
    const EncounterDataSlot *rodSlots;
    switch (rodType) {
    case ROD_TYPE_OLD:
        rodSlots = encData->oldRodSlots;
        break;
    case ROD_TYPE_GOOD:
        rodSlots = encData->goodRodSlots;
        break;
    case ROD_TYPE_SUPER:
        rodSlots = encData->superRodSlots;
        break;
    }
    // potential UB: rodSlots is uninitialized on invalid rod type
    for (u8 i = 0; i < NUM_ENCOUNTERS_FISH; ++i) {
        encSlots[i].species = rodSlots[i].species;
        encSlots[i].maxLevel = rodSlots[i].level_max;
        encSlots[i].minLevel = rodSlots[i].level_min;
    }
    EncSlotArray_Update_NightFishing(encData, rodType, encSlots);
    EncSlots_Update_FishingSwarm(fieldSystem, rodType, encData, encSlots);
    return FieldSystem_GenerateFishingEncounter(fieldSystem, leadMon, *pBattleSetup, encSlots, &encounterGen, rodType, safariFlag) ? TRUE : FALSE;
}

BOOL FieldSystem_PerformRockSmashEncounterCheck(FieldSystem *fieldSystem, BattleSetup **pBattleSetup) {
    EncounterSlot encSlots[NUM_ENCOUNTERS_LAND];
    EncounterGenState encounterGen;

    u8 encounterRate = FieldSystem_GetRockSmashEncounterRate(fieldSystem);
    if (encounterRate == 0) {
        return FALSE;
    }

    Pokemon *leadMon = Party_GetMonByIndex(SaveArray_Party_Get(fieldSystem->saveData), 0);
    EncounterGenState_Init(fieldSystem, leadMon, NULL, &encounterGen);
    encounterRate = ApplyAbilityEffectToEncounterRate(FALSE, encounterRate, &encounterGen, LocalFieldData_GetWeatherType(Save_LocalFieldData_Get(fieldSystem->saveData)), leadMon);
    if ((LCRandom() % 100) >= encounterRate) {
        return FALSE;
    }

    FieldSystem_CreateBattleSetupForWildBattle(fieldSystem, ENCOUNTER_CONTEXT_REGULAR, pBattleSetup);
    BattleSetup_InitFromFieldSystem(*pBattleSetup, fieldSystem);
    const EncounterData *encData = MapEvents_GetLoadedEncTable(fieldSystem);
    const EncounterDataSlot *encDataSlots = encData->rockSmashSlots;
    for (u8 i = 0; i < NUM_ENCOUNTERS_ROCKSMASH; ++i) {
        encSlots[i].species = encDataSlots[i].species;
        encSlots[i].maxLevel = encDataSlots[i].level_max;
        encSlots[i].minLevel = encDataSlots[i].level_min;
    }
    if (!FieldSystem_GenerateRockSmashEncounter(fieldSystem, leadMon, *pBattleSetup, encSlots, &encounterGen)) {
        BattleSetup_Delete(*pBattleSetup);
        return FALSE;
    }

    return TRUE;
}

BOOL FieldSystem_PerformSweetScentEncounterCheck(FieldSystem *fieldSystem, TaskManager *taskManager) {
    EncounterSlot encSlots[NUM_ENCOUNTERS_LAND];
    EncounterGenState encounterGen;
    BattleSetup *battleSetup;
    Roamer *roamer;
    u8 encType;
    SaveVarsFlags *saveVarsFlags = Save_VarsFlags_Get(fieldSystem->saveData);
    u32 x = PlayerAvatar_GetXCoord(fieldSystem->playerAvatar);
    u32 z = PlayerAvatar_GetZCoord(fieldSystem->playerAvatar);
    u8 metatileBehavior = GetMetatileBehavior(fieldSystem, x, z);

    if (!FieldSystem_GetTileEncounterTypeAndRate(fieldSystem, metatileBehavior, &encType)) {
        return FALSE;
    }

    Party *party = SaveArray_Party_Get(fieldSystem->saveData);
    const EncounterData *encData = MapEvents_GetLoadedEncTable(fieldSystem);
    Pokemon *leadMon = Party_GetMonByIndex(party, 0);
    EncounterGenState_Init(fieldSystem, leadMon, encData, &encounterGen);
    encounterGen.isSweetScent = TRUE;
    BOOL followerFlag = Save_VarsFlags_CheckHaveFollower(saveVarsFlags);
    if (!followerFlag && getRandomActiveRoamerInCurrMap(fieldSystem, &roamer)) {
        battleSetup = BattleSetup_New(HEAP_ID_FIELD2, BATTLE_TYPE_ROAMER);
        BattleSetup_InitFromFieldSystem(battleSetup, fieldSystem);
        initRoamingWildmon(encounterGen.playerId, roamer, battleSetup);
        FieldSystem_StartForcedWildBattle(fieldSystem, taskManager, battleSetup);
        return TRUE;
    }

    BOOL safariFlag = Save_VarsFlags_CheckSafariSysFlag(saveVarsFlags);
    BOOL bugContestFlag = Save_VarsFlags_CheckBugContestFlag(saveVarsFlags);
    if (!followerFlag) {
        u8 encounterCtx = ENCOUNTER_CONTEXT_REGULAR;
        if (safariFlag) {
            encounterCtx = ENCOUNTER_CONTEXT_SAFARI;
        } else if (bugContestFlag) {
            encounterCtx = ENCOUNTER_CONTEXT_BUG_CONTEST;
        }
        FieldSystem_CreateBattleSetupForWildBattle(fieldSystem, encounterCtx, &battleSetup);
    } else {
        battleSetup = BattleSetup_New(HEAP_ID_FIELD2, BATTLE_TYPE_DOUBLES | BATTLE_TYPE_MULTI | BATTLE_TYPE_AI);
    }
    BattleSetup_InitFromFieldSystem(battleSetup, fieldSystem);
    BOOL found;
    if (encType == ENCOUNTER_TYPE_LAND) {
        EncSlotArray_Init_Land(encData, encSlots);
        EncSlots_Update_LandSwarm(fieldSystem, encData, &encSlots[0], &encSlots[1]);
        EncSlotArray_Update_HoennSinnohSounds(fieldSystem, encData, encSlots);
        if (!followerFlag) {
            if (safariFlag) {
                found = FieldSystem_GenerateLandSafariEncounter(fieldSystem, leadMon, battleSetup, encData, encSlots, &encounterGen);
            } else if (bugContestFlag) {
                found = FieldSystem_GenerateBugContestEncounter(fieldSystem, leadMon, battleSetup, encData, encSlots, &encounterGen);
            } else {
                found = FieldSystem_GenerateLandRegularEncounter(fieldSystem, leadMon, battleSetup, encData, encSlots, &encounterGen);
            }
        } else {
            battleSetup->trainerId[BATTLER_PLAYER2] = Save_VarsFlags_GetFollowerTrainerNum(Save_VarsFlags_Get(fieldSystem->saveData));
            EnemyTrainerSet_Init(battleSetup, fieldSystem->saveData, HEAP_ID_FIELD2);
            found = FieldSystem_GenerateDoubleEncounter(fieldSystem, leadMon, battleSetup, encSlots, &encounterGen);
        }
    } else if (encType == ENCOUNTER_TYPE_SURFING) {
        for (int i = 0; i < NUM_ENCOUNTERS_SURF; ++i) {
            encSlots[i].species = encData->surfSlots[i].species;
            encSlots[i].maxLevel = encData->surfSlots[i].level_max;
            encSlots[i].minLevel = encData->surfSlots[i].level_min;
        }
        EncSlots_Update_SurfingSwarm(fieldSystem, encData, encSlots);
        found = FieldSystem_GenerateSurfingEncounter(fieldSystem, leadMon, battleSetup, encSlots, &encounterGen, safariFlag);
    } else {
        GF_ASSERT(FALSE);
        return FALSE;
    }
    if (found) {
        FieldSystem_StartForcedWildBattle(fieldSystem, taskManager, battleSetup);
    } else {
        GF_ASSERT(FALSE);
    }
    fieldSystem->encounterInhibitSteps = 0;
    fieldSystem->reverseTurnFrameSteps = 0;
    return TRUE;
}

BOOL FieldSystem_PerformHeadbuttEncounterCheck(FieldSystem *fieldSystem, BattleSetup **pBattleSetup, const HeadbuttSlot *headbuttSlots) {
    EncounterSlot encSlots[NUM_ENCOUNTERS_LAND];
    EncounterGenState encounterGen;
    EncounterDataSlot encDataSlots[NUM_ENCOUNTERS_HEADBUTT];

    Pokemon *leadMon = Party_GetMonByIndex(SaveArray_Party_Get(fieldSystem->saveData), 0);
    EncounterGenState_Init(fieldSystem, leadMon, NULL, &encounterGen);
    FieldSystem_CreateBattleSetupForWildBattle(fieldSystem, ENCOUNTER_CONTEXT_REGULAR, pBattleSetup);
    BattleSetup_InitFromFieldSystem(*pBattleSetup, fieldSystem);
    for (u8 i = 0; i < NUM_ENCOUNTERS_HEADBUTT; ++i) {
        encDataSlots[i].species = headbuttSlots[i].species;
        encDataSlots[i].level_max = headbuttSlots[i].maxLevel;
        encDataSlots[i].level_min = headbuttSlots[i].minLevel;
    }
    for (u8 i = 0; i < NUM_ENCOUNTERS_HEADBUTT; ++i) {
        encSlots[i].species = encDataSlots[i].species;
        encSlots[i].maxLevel = encDataSlots[i].level_max;
        encSlots[i].minLevel = encDataSlots[i].level_min;
    }
    if (!FieldSystem_GenerateRegularEncounter(leadMon, ROD_TYPE_NONE, &encounterGen, encSlots, ENCOUNTER_TYPE_HEADBUTT, BATTLER_ENEMY, *pBattleSetup)) {
        BattleSetup_Delete(*pBattleSetup);
        return FALSE;
    }

    return TRUE;
}

static BOOL FieldSystem_GenerateLandRegularEncounter(FieldSystem *fieldSystem, Pokemon *leadMon, BattleSetup *battleSetup, const EncounterData *encData, EncounterSlot *encSlots, EncounterGenState *encounterGen) {
    return FieldSystem_GenerateRegularEncounter(leadMon, ROD_TYPE_NONE, encounterGen, encSlots, ENCOUNTER_TYPE_LAND, BATTLER_ENEMY, battleSetup);
}

static BOOL FieldSystem_GenerateLandSafariEncounter(FieldSystem *fieldSystem, Pokemon *leadMon, BattleSetup *battleSetup, const EncounterData *encData, EncounterSlot *encSlots, EncounterGenState *encounterGen) {
    return FieldSystem_GenerateSafariEncounter(fieldSystem, leadMon, ROD_TYPE_NONE, encounterGen, ENCOUNTER_TYPE_LAND, BATTLER_ENEMY, battleSetup);
}

static BOOL FieldSystem_GenerateBugContestEncounter(FieldSystem *fieldSystem, Pokemon *leadMon, BattleSetup *battleSetup, const EncounterData *encData, EncounterSlot *encSlots, EncounterGenState *encounterGen) {
    FSOverlayID ovyId = FS_OVERLAY_ID(bug_contest);

    HandleLoadOverlay(ovyId, OVY_LOAD_ASYNC);
    BOOL ret = FieldSystem_GenerateBugContestEncounter_Internal(fieldSystem, leadMon, ROD_TYPE_NONE, encounterGen, ENCOUNTER_TYPE_LAND, BATTLER_ENEMY, battleSetup);
    UnloadOverlayByID(ovyId);
    return ret;
}

static BOOL FieldSystem_GenerateDoubleEncounter(FieldSystem *fieldSystem, Pokemon *leadMon, BattleSetup *battleSetup, EncounterSlot *encSlots, EncounterGenState *encounterGen) {
    if (FieldSystem_GenerateRegularEncounter(leadMon, ROD_TYPE_NONE, encounterGen, encSlots, ENCOUNTER_TYPE_LAND, BATTLER_ENEMY, battleSetup) == FALSE) {
        return FALSE;
    } else {
        return FieldSystem_GenerateRegularEncounter(leadMon, ROD_TYPE_NONE, encounterGen, encSlots, ENCOUNTER_TYPE_LAND, BATTLER_ENEMY2, battleSetup);
    }
}

static BOOL FieldSystem_GenerateSurfingEncounter(FieldSystem *fieldSystem, Pokemon *leadMon, BattleSetup *battleSetup, EncounterSlot *encSlots, EncounterGenState *encounterGen, BOOL isSafari) {
    if (isSafari) {
        return FieldSystem_GenerateSafariEncounter(fieldSystem, leadMon, ROD_TYPE_NONE, encounterGen, ENCOUNTER_TYPE_SURFING, BATTLER_ENEMY, battleSetup);
    } else {
        return FieldSystem_GenerateRegularEncounter(leadMon, ROD_TYPE_NONE, encounterGen, encSlots, ENCOUNTER_TYPE_SURFING, BATTLER_ENEMY, battleSetup);
    }
}

static BOOL FieldSystem_GenerateFishingEncounter(FieldSystem *fieldSystem, Pokemon *leadMon, BattleSetup *battleSetup, EncounterSlot *encSlots, EncounterGenState *encounterGen, int rodType, BOOL isSafari) {
    if (isSafari) {
        return FieldSystem_GenerateSafariEncounter(fieldSystem, leadMon, rodType, encounterGen, ENCOUNTER_TYPE_FISHING, BATTLER_ENEMY, battleSetup);
    } else {
        return FieldSystem_GenerateRegularEncounter(leadMon, rodType, encounterGen, encSlots, ENCOUNTER_TYPE_FISHING, BATTLER_ENEMY, battleSetup);
    }
}

static BOOL FieldSystem_GenerateRockSmashEncounter(FieldSystem *fieldSystem, Pokemon *leadMon, BattleSetup *battleSetup, EncounterSlot *encSlots, EncounterGenState *encounterGen) {
    return FieldSystem_GenerateRegularEncounter(leadMon, ROD_TYPE_NONE, encounterGen, encSlots, ENCOUNTER_TYPE_ROCK_SMASH, BATTLER_ENEMY, battleSetup);
}

static BOOL FieldSystem_EncounterRateRoll(FieldSystem *fieldSystem, u8 encounterRate, u8 metatileBehavior) {
    if (encounterRate > 100) {
        encounterRate = 100;
    }
    int avatarState = PlayerAvatar_GetState(fieldSystem->playerAvatar);
    u8 modifiedRate = 40;
    if (avatarState != PLAYER_STATE_CYCLING && avatarState != PLAYER_STATE_SURFING && !sub_0205DE98(fieldSystem->playerAvatar)) {
        modifiedRate -= 20;
    }
    if (MetatileBehavior_IsVeryTallGrass(metatileBehavior)) {
        modifiedRate += 40;
    } else if (avatarState == PLAYER_STATE_CYCLING) {
        modifiedRate += 30;
    }
    modifiedRate += getStepCountEncounterRateBoost(fieldSystem->reverseTurnFrameSteps);
    if (GetRadioMusicPlayingSeq() == RADIO_MUSIC_SEQ_MARCH) {
        modifiedRate += 25;
    } else if (GetRadioMusicPlayingSeq() == RADIO_MUSIC_SEQ_LULLABY) {
        modifiedRate -= 25;
    }
    if (modifiedRate > 100) {
        modifiedRate = 100;
    }
    if (LCRandRange(100) < modifiedRate && FieldSystem_SecondEncounterRoll(fieldSystem, encounterRate)) {
        return TRUE;
    }

    return FALSE;
}

static u8 getStepCountEncounterRateBoost(u16 stepCount) {
    u8 result = 0;

    if (stepCount >= 4) {
        result = 60;
    } else if (stepCount >= 3) {
        result = 40;
    } else if (stepCount >= 2) {
        result = 30;
    }

    return result;
}

static u8 FieldSystem_GetTileEncounterTypeAndRate(FieldSystem *fieldSystem, u8 metatileBehavior, u8 *pEncType) {
    if (MetatileBehavior_CanGenerateWalkingEncounters(metatileBehavior)) {
        if (MetatileBehavior_IsSurfableWater(metatileBehavior)) {
            *pEncType = ENCOUNTER_TYPE_SURFING;
            return FieldSystem_GetSurfingEncounterRate(fieldSystem);
        } else {
            *pEncType = ENCOUNTER_TYPE_LAND;
            return FieldSystem_GetLandEncounterRate(fieldSystem);
        }
    }

    return 0;
}

static BOOL FieldSystem_SecondEncounterRoll(FieldSystem *fieldSystem, u16 encounterRate) {
    // This may have been used for some additional logic, but it's essentially
    // just an encounter roll against the unmodified rate.
    if (LCRandRange(100) < encounterRate) {
        return TRUE;
    }
    return FALSE;
}

static u8 EncounterSlot_WildMonSlotRoll_Land(void) {
    u8 rnd = LCRandRange(100);

    if (rnd < 20) {
        return 0;
    } else if (rnd >= 20 && rnd < 40) {
        return 1;
    } else if (rnd >= 40 && rnd < 50) {
        return 2;
    } else if (rnd >= 50 && rnd < 60) {
        return 3;
    } else if (rnd >= 60 && rnd < 70) {
        return 4;
    } else if (rnd >= 70 && rnd < 80) {
        return 5;
    } else if (rnd >= 80 && rnd < 85) {
        return 6;
    } else if (rnd >= 85 && rnd < 90) {
        return 7;
    } else if (rnd >= 90 && rnd < 94) {
        return 8;
    } else if (rnd >= 94 && rnd < 98) {
        return 9;
    } else if (rnd == 98) {
        return 10;
    } else {
        return 11;
    }
}

static u8 EncounterSlot_WildMonSlotRoll_Surfing(void) {
    u8 rnd = LCRandRange(100);

    if (rnd < 60) {
        return 0;
    } else if (rnd >= 60 && rnd < 90) {
        return 1;
    } else if (rnd >= 90 && rnd < 95) {
        return 2;
    } else if (rnd >= 95 && rnd < 99) {
        return 3;
    } else {
        return 4;
    }
}

static u8 EncounterSlot_WildMonSlotRoll_Fishing(int rodType) {
    u8 rnd = LCRandRange(100);

    if (rnd < 40) {
        return 0;
    } else if (rnd < 70) {
        return 1;
    } else if (rnd < 85) {
        return 2;
    } else if (rnd < 95) {
        return 3;
    } else {
        return 4;
    }
}

static u8 EncounterSlot_WildMonSlotRoll_RockSmash(void) {
    u8 rnd = LCRandRange(100);

    return rnd >= 80 ? 1 : 0;
}

static u8 EncounterSlot_WildMonSlotRoll_Headbutt(void) {
    u8 rnd = LCRandRange(100);

    if (rnd < 50) {
        return 0;
    } else if (rnd < 65) {
        return 1;
    } else if (rnd < 80) {
        return 2;
    } else if (rnd < 90) {
        return 3;
    } else if (rnd < 95) {
        return 4;
    } else {
        return 5;
    }
}

static void ApplyLeadMonHeldItemEffectToEncounterRate(Pokemon *leadMon, u8 *pEncounterRate) {
    u16 heldItem = GetMonData(leadMon, MON_DATA_HELD_ITEM, NULL);
    if (heldItem == ITEM_CLEANSE_TAG || heldItem == ITEM_PURE_INCENSE) {
        *pEncounterRate = *pEncounterRate * 2 / 3;
    }
}

static void ApplyFluteEffectToEncounterRate(FieldSystem *fieldSystem, u8 *pEncounterRate) {
    u8 flute = RoamerSave_GetFlute(Save_Roamers_Get(fieldSystem->saveData));
    if (flute == 1) { // black flute
        *pEncounterRate /= 2;
    } else if (flute == 2) { // white flute
        *pEncounterRate += *pEncounterRate / 2;
    }
}

static u8 getWildMonNature(Pokemon *pokemon, EncounterGenState *encounterGen) {
    if (!encounterGen->isEgg && encounterGen->ability == ABILITY_SYNCHRONIZE && LCRandRange(2) == 0) {
        return GetMonData(pokemon, MON_DATA_PERSONALITY, NULL) % 25;
    } else {
        return LCRandRange(25);
    }
}

// Synchronize always works: it still makes the game's 1 in 2 roll, but a miss throws the whole attempt away
// (the caller rolls again from the slot roll) instead of falling back to a random nature. Every kept mon therefore
// comes from a roll that a Synchronize hit in an unmodified game, so it stays legal.
static BOOL rollWildNature(Pokemon *pokemon, EncounterGenState *encounterGen, u8 *nature) {
    if (!encounterGen->isEgg && encounterGen->ability == ABILITY_SYNCHRONIZE) {
        if (LCRandRange(2) != 0) {
            return FALSE;
        }
        *nature = GetMonData(pokemon, MON_DATA_PERSONALITY, NULL) % 25;
        return TRUE;
    }
    *nature = LCRandRange(25);
    return TRUE;
}

static u8 EncounterSlot_WildMonLevelRoll(EncounterSlot *encSlot, EncounterGenState *encounterGen) {
    u8 hi;
    u8 lo;

    if (encSlot->maxLevel >= encSlot->minLevel) {
        lo = encSlot->minLevel;
        hi = encSlot->maxLevel;
    } else {
        lo = encSlot->maxLevel;
        hi = encSlot->minLevel;
    }

    u8 range = hi - lo + 1;
    u8 lvl = LCRandom() % range;
    if (!encounterGen->isEgg && (encounterGen->ability == ABILITY_HUSTLE || encounterGen->ability == ABILITY_VITAL_SPIRIT || encounterGen->ability == ABILITY_PRESSURE)) {
        if (LCRandRange(2) == 0) {
            return lo + lvl;
        }
        return hi;
    }

    return lo + lvl;
}

static void generateWildShinyAndAddToParty(u16 species, u8 level, int battler, u32 otid, EncounterGenState *encounterGen, Pokemon *leadMon, BattleSetup *battleSetup) {
    u8 monGender;
    u8 monNature;
    Pokemon *wildMon = AllocMonZeroed(HEAP_ID_FIELD2);
    BOOL overrideGenderOrNature;
    u32 personality;

    ZeroMonData(wildMon);

    overrideGenderOrNature = FALSE;
    if (!encounterGen->isEgg) {
        if (encounterGen->ability == ABILITY_CUTE_CHARM) {
            switch (GetMonBaseStat(species, BASE_GENDER_RATIO)) {
            case MON_RATIO_MALE:
            case MON_RATIO_FEMALE:
            case MON_RATIO_UNKNOWN:
                break;
            default:
                if (LCRandRange(3) != 0) {
                    monGender = GetMonData(leadMon, MON_DATA_GENDER, NULL);
                    overrideGenderOrNature = TRUE;
                }
                break;
            }
        } else if (encounterGen->ability == ABILITY_SYNCHRONIZE) {
            if (LCRandRange(2) == 0) {
                monNature = GetMonNature(leadMon);
                overrideGenderOrNature = TRUE;
            }
        }
    }
    personality = GenerateShinyPersonality(otid);
    if (overrideGenderOrNature) {
        do {
            if (encounterGen->ability == ABILITY_CUTE_CHARM) {
                u8 generatedGender = GetGenderBySpeciesAndPersonality(species, personality);
                GF_ASSERT(generatedGender != MON_GENDERLESS);
                if (generatedGender != monGender) {
                    break;
                }
                personality = GenerateShinyPersonality(otid);
            } else if (encounterGen->ability == ABILITY_SYNCHRONIZE) {
                u8 generatedNature = GetNatureFromPersonality(personality);
                if (generatedNature == monNature) {
                    break;
                }
                personality = GenerateShinyPersonality(otid);
            }
        } while (TRUE);
    }

    CreateMon(wildMon, species, level, 0x20, TRUE, personality, OT_ID_PRESET, encounterGen->playerId);
    GF_ASSERT(addGeneratedMonToBattleSetupParty(battler, encounterGen, wildMon, battleSetup));
    Heap_Free(wildMon);
}

static BOOL rollWildShiny(void) {
    return LCRandRange(WILD_SHINY_ODDS) == 0;
}

static u32 findWildPersonality(u8 nature) {
    u32 personality;

    do {
        u32 low = LCRandom();
        u32 high = LCRandom();
        personality = low | (high << 16);
    } while (nature != GetNatureFromPersonality(personality));

    return personality;
}

static BOOL wildPersonalityMeetsShinyRequirement(u32 otId, u32 personality, int shinyRequirement) {
    BOOL isShiny = CalcShininessByOtIdAndPersonality(otId, personality) != 0;

    if (shinyRequirement == WILD_SHINY_REQ_SHINY) {
        return isShiny;
    }
    if (shinyRequirement == WILD_SHINY_REQ_NOT_SHINY) {
        return !isShiny;
    }
    return TRUE;
}

// One step of the game's LCRNG (same constants as LCRandom in math_util.c).
static u32 wildNextSeed(u32 seed) {
    return seed * 1103515245 + 24691;
}

static void initWildAttempt(WildAttempt *attempt, u8 kind, FieldSystem *fieldSystem, Pokemon *leadMon, EncounterGenState *encounterGen, EncounterSlot *encSlots, u8 encType, int rodType, u16 species, u8 level) {
    attempt->kind = kind;
    attempt->encType = encType;
    attempt->rodType = rodType;
    attempt->fieldSystem = fieldSystem;
    attempt->leadMon = leadMon;
    attempt->encounterGen = encounterGen;
    attempt->encSlots = encSlots;
    attempt->species = species;
    attempt->level = level;
    attempt->personality = 0;
}

static BOOL rollRegularSlotAndLevel(WildAttempt *attempt) {
    Pokemon *leadMon = attempt->leadMon;
    EncounterGenState *encounterGen = attempt->encounterGen;
    EncounterSlot *encSlots = attempt->encSlots;
    u8 slot = 0;
    u8 level = 0;

    switch (attempt->encType) {
    case ENCOUNTER_TYPE_LAND:
        if (!EncounterSlot_AbilityInfluenceOnSlotChoiceCheck(leadMon, encounterGen, encSlots, NUM_ENCOUNTERS_LAND, TYPE_STEEL, ABILITY_MAGNET_PULL, &slot) && !EncounterSlot_AbilityInfluenceOnSlotChoiceCheck(leadMon, encounterGen, encSlots, NUM_ENCOUNTERS_LAND, TYPE_ELECTRIC, ABILITY_STATIC, &slot)) {
            slot = EncounterSlot_WildMonSlotRoll_Land();
        }
        slot = ApplyAbilityEffectToSlotLevel(encSlots, NUM_ENCOUNTERS_LAND, encounterGen, slot);
        level = encSlots[slot].maxLevel;
        break;
    case ENCOUNTER_TYPE_ROCK_SMASH:
        if (!EncounterSlot_AbilityInfluenceOnSlotChoiceCheck(leadMon, encounterGen, encSlots, NUM_ENCOUNTERS_ROCKSMASH, TYPE_STEEL, ABILITY_MAGNET_PULL, &slot) && !EncounterSlot_AbilityInfluenceOnSlotChoiceCheck(leadMon, encounterGen, encSlots, NUM_ENCOUNTERS_ROCKSMASH, TYPE_ELECTRIC, ABILITY_STATIC, &slot)) {
            slot = EncounterSlot_WildMonSlotRoll_RockSmash();
        }
        level = EncounterSlot_WildMonLevelRoll(&encSlots[slot], encounterGen);
        break;
    case ENCOUNTER_TYPE_SURFING:
        if (!EncounterSlot_AbilityInfluenceOnSlotChoiceCheck(leadMon, encounterGen, encSlots, NUM_ENCOUNTERS_SURF, TYPE_STEEL, ABILITY_MAGNET_PULL, &slot) && !EncounterSlot_AbilityInfluenceOnSlotChoiceCheck(leadMon, encounterGen, encSlots, NUM_ENCOUNTERS_SURF, TYPE_ELECTRIC, ABILITY_STATIC, &slot)) {
            slot = EncounterSlot_WildMonSlotRoll_Surfing();
        }
        level = EncounterSlot_WildMonLevelRoll(&encSlots[slot], encounterGen);
        break;
    case ENCOUNTER_TYPE_FISHING:
        if (!EncounterSlot_AbilityInfluenceOnSlotChoiceCheck(leadMon, encounterGen, encSlots, NUM_ENCOUNTERS_FISH, TYPE_STEEL, ABILITY_MAGNET_PULL, &slot) && !EncounterSlot_AbilityInfluenceOnSlotChoiceCheck(leadMon, encounterGen, encSlots, NUM_ENCOUNTERS_FISH, TYPE_ELECTRIC, ABILITY_STATIC, &slot)) {
            slot = EncounterSlot_WildMonSlotRoll_Fishing(attempt->rodType);
        }
        level = EncounterSlot_WildMonLevelRoll(&encSlots[slot], encounterGen);
        break;
    case ENCOUNTER_TYPE_HEADBUTT:
        if (!EncounterSlot_AbilityInfluenceOnSlotChoiceCheck(leadMon, encounterGen, encSlots, NUM_ENCOUNTERS_HEADBUTT, TYPE_STEEL, ABILITY_MAGNET_PULL, &slot) && !EncounterSlot_AbilityInfluenceOnSlotChoiceCheck(leadMon, encounterGen, encSlots, NUM_ENCOUNTERS_HEADBUTT, TYPE_ELECTRIC, ABILITY_STATIC, &slot)) {
            slot = EncounterSlot_WildMonSlotRoll_Headbutt();
        }
        level = EncounterSlot_WildMonLevelRoll(&encSlots[slot], encounterGen);
        break;
    default:
        GF_ASSERT(FALSE);
    }

    if (encSlots[slot].species == SPECIES_UNOWN && !EncounterGen_CanGenerateUnownEncounter(encounterGen)) {
        return FALSE;
    }
    if (DoesAbilitySuppressEncounter(encounterGen, leadMon, level)) {
        return FALSE;
    }
    if (EncounterGen_DoesRepelSuppressEncounter(level, encounterGen) == TRUE) {
        return FALSE;
    }

    attempt->species = encSlots[slot].species;
    attempt->level = level;
    return TRUE;
}

static BOOL rollSafariSlotAndLevel(WildAttempt *attempt) {
    Pokemon *leadMon = attempt->leadMon;
    EncounterGenState *encounterGen = attempt->encounterGen;
    EncounterSlot *encSlots = attempt->encSlots;
    u8 slot = 0;

    BOOL encSlotModified = EncounterSlot_AbilityInfluenceOnSlotChoiceCheck(leadMon, encounterGen, encSlots, NUM_ENCOUNTERS_SAFARI, TYPE_STEEL, ABILITY_MAGNET_PULL, &slot);
    if (!encSlotModified) {
        encSlotModified = EncounterSlot_AbilityInfluenceOnSlotChoiceCheck(leadMon, encounterGen, encSlots, NUM_ENCOUNTERS_SAFARI, TYPE_ELECTRIC, ABILITY_STATIC, &slot);
    }
    if (!encSlotModified) {
        slot = LCRandom() % NUM_ENCOUNTERS_SAFARI;
    }
    if (attempt->encType == ENCOUNTER_TYPE_LAND) {
        slot = ApplyAbilityEffectToSlotLevel(encSlots, NUM_ENCOUNTERS_SAFARI, encounterGen, slot);
    }

    u8 level = encSlots[slot].maxLevel;
    if (DoesAbilitySuppressEncounter(encounterGen, leadMon, level) || EncounterGen_DoesRepelSuppressEncounter(level, encounterGen) == TRUE) {
        return FALSE;
    }

    attempt->species = encSlots[slot].species;
    attempt->level = level;
    return TRUE;
}

static BOOL rollBugContestSlotAndLevel(WildAttempt *attempt) {
    EncounterSlot *encSlot = BugContest_GetEncounterSlot(FieldSystem_BugContest_Get(attempt->fieldSystem), HEAP_ID_FIELD1);
    u16 species = encSlot->species;
    u8 level = encSlot->maxLevel;
    Heap_Free(encSlot);

    if (EncounterGen_DoesRepelSuppressEncounter(level, attempt->encounterGen) == TRUE) {
        return FALSE;
    }

    attempt->species = species;
    attempt->level = level;
    return TRUE;
}

// Everything an unmodified encounter rolls up to and including the PID. No IVs, and nothing is created, so an
// attempt can be repeated from any RNG state. Cute Charm builds a fixed-form PID, so it ignores the shiny requirement.
static int rollWildAttempt(WildAttempt *attempt) {
    BOOL notSuppressed;
    EncounterGenState *encounterGen = attempt->encounterGen;

    switch (attempt->kind) {
    case WILD_ATTEMPT_REGULAR:
        notSuppressed = rollRegularSlotAndLevel(attempt);
        break;
    case WILD_ATTEMPT_SAFARI:
        notSuppressed = rollSafariSlotAndLevel(attempt);
        break;
    case WILD_ATTEMPT_BUG_CONTEST:
        notSuppressed = rollBugContestSlotAndLevel(attempt);
        break;
    default:
        notSuppressed = TRUE; // WILD_ATTEMPT_SINGLE: species and level are fixed by the caller
        break;
    }
    if (!notSuppressed) {
        return WILD_ROLL_BLOCKED;
    }

    u8 canCoerceGender = TRUE;
    switch (GetMonBaseStat(attempt->species, BASE_GENDER_RATIO)) {
    case MON_RATIO_MALE:
    case MON_RATIO_FEMALE:
    case MON_RATIO_UNKNOWN:
        canCoerceGender = FALSE;
    default:
        break;
    }
    if (canCoerceGender && !encounterGen->isEgg && encounterGen->ability == ABILITY_CUTE_CHARM && LCRandRange(3) != 0) {
        return WILD_ROLL_CUTE_CHARM;
    }

    u8 nature;
    if (!rollWildNature(attempt->leadMon, encounterGen, &nature)) {
        return WILD_ROLL_RETRY;
    }
    attempt->personality = findWildPersonality(nature);
    return WILD_ROLL_PERSONALITY;
}

// True if the two RNG outputs after seed (what CreateMon rolls for IVs) contain a 31.
static BOOL wildIvRollHasPerfectIV(u32 seed) {
    seed = wildNextSeed(seed);
    u32 iv1 = seed >> 16;
    seed = wildNextSeed(seed);
    u32 iv2 = seed >> 16;

    for (int i = 0; i < 3; ++i) {
        if (((iv1 >> (5 * i)) & 31) == 31 || ((iv2 >> (5 * i)) & 31) == 31) {
            return TRUE;
        }
    }
    return FALSE;
}

// Finds an attempt whose PID is shiny without running attempts one after another (that took ~400,000 RNG calls).
// Walk the RNG forward looking at every PID-sized pair of outputs; the PIDs that are shiny are rare, but checking
// one is a single compare. For each shiny pair, look back through the last WILD_SHINY_SEARCH_WINDOW RNG states for
// a state an unmodified encounter could have started from and that ends exactly on that pair (slot, level, nature,
// then the PID loop stopping on it). Each candidate runs the real attempt code, so abilities, Repel and the like
// are honoured, and the RNG is left exactly where an unmodified encounter would be after rolling the PID.
static BOOL searchShinyAttempt(WildAttempt *attempt, BOOL forceOnePerfectIV) {
    u32 history[WILD_SHINY_SEARCH_WINDOW]; // history[n % WINDOW] = RNG state before output n
    u32 startSeed = GetLCRNGSeed();
    u32 otId = attempt->encounterGen->playerId;
    u32 seed0 = startSeed;
    u32 seed1 = wildNextSeed(seed0);
    u32 seed2 = wildNextSeed(seed1);

    // Synchronize fixes the nature, so only PIDs of that nature can come out of an attempt.
    BOOL synchronized = !attempt->encounterGen->isEgg && attempt->encounterGen->ability == ABILITY_SYNCHRONIZE;
    u32 syncNature = synchronized ? GetMonData(attempt->leadMon, MON_DATA_PERSONALITY, NULL) % 25 : 0;

    for (u32 n = 0; n < WILD_SHINY_SEARCH_LIMIT; ++n) {
        history[n % WILD_SHINY_SEARCH_WINDOW] = seed0;

        u32 personality = (seed1 >> 16) | (seed2 & 0xFFFF0000); // outputs n and n + 1, low half first
        if (CalcShininessByOtIdAndPersonality(otId, personality) && (!synchronized || personality % 25 == syncNature)) {
            u32 oldest = n + 1 > WILD_SHINY_SEARCH_WINDOW ? n + 1 - WILD_SHINY_SEARCH_WINDOW : 0;
            for (u32 start = n; start-- > oldest;) {
                SetLCRNGSeed(history[start % WILD_SHINY_SEARCH_WINDOW]);
                if (rollWildAttempt(attempt) == WILD_ROLL_PERSONALITY && GetLCRNGSeed() == seed2) {
                    if (!forceOnePerfectIV || wildIvRollHasPerfectIV(seed2)) {
                        return TRUE;
                    }
                    // Safari and Bug Contest keep re-rolling until a 31 IV turns up, so a legal shiny needs one on its first try.
                }
            }
        }

        seed0 = seed1;
        seed1 = seed2;
        seed2 = wildNextSeed(seed2);
    }

    SetLCRNGSeed(startSeed);
    return FALSE;
}

// Builds the mon from a finished roll. Returns FALSE, having generated nothing, if a later PID (31-IV retries)
// does not satisfy shinyRequirement. The caller then re-rolls the whole encounter from the slot roll onwards.
static BOOL addWildMonToParty(WildAttempt *attempt, int rollResult, int battler, BOOL forceOnePerfectIV, int shinyRequirement, BattleSetup *battleSetup) {
    EncounterGenState *encounterGen = attempt->encounterGen;
    Pokemon *leadMon = attempt->leadMon;
    u16 species = attempt->species;
    u8 level = attempt->level;
    Pokemon *wildMon = AllocMonZeroed(HEAP_ID_FIELD2);
    ZeroMonData(wildMon);

    if (rollResult == WILD_ROLL_CUTE_CHARM) {
        u8 monGender = GetMonData(leadMon, MON_DATA_GENDER, NULL);
        if (monGender == MON_FEMALE) {
            monGender = MON_MALE;
        } else if (monGender == MON_MALE) {
            monGender = MON_FEMALE;
        } else {
            GF_ASSERT(FALSE); // can't put cute charm on a genderless pokemon
        }
        CreateMonWithGenderNatureLetter(wildMon, species, level, 0x20, monGender, getWildMonNature(leadMon, encounterGen), 0);
        SetMonData(wildMon, MON_DATA_OT_ID, &encounterGen->playerId);
        GF_ASSERT(addGeneratedMonToBattleSetupParty(battler, encounterGen, wildMon, battleSetup));
        Heap_Free(wildMon);
        return TRUE;
    }

    // Each try is exactly what CreateMonWithNature does: nature roll, PID loop, then CreateMon rolls the IVs.
    // The first try's nature and PID were rolled as part of the attempt.
    u32 personality = attempt->personality;
    if (forceOnePerfectIV) {
        int i;
        BOOL success = FALSE;
        for (i = 0; i < 4; ++i) {
            if (i != 0) {
                u8 nature;
                if (!rollWildNature(leadMon, encounterGen, &nature)) {
                    Heap_Free(wildMon);
                    return FALSE;
                }
                personality = findWildPersonality(nature);
                if (!wildPersonalityMeetsShinyRequirement(encounterGen->playerId, personality, shinyRequirement)) {
                    Heap_Free(wildMon);
                    return FALSE;
                }
            }
            CreateMon(wildMon, species, level, 0x20, TRUE, personality, OT_ID_PLAYER_ID, 0);
            for (int j = 0; j < 6; ++j) {
                if (GetMonData(wildMon, MON_DATA_HP_IV + j, NULL) == 31) {
                    success = TRUE;
                    break;
                }
            }
            if (success) {
                break;
            }
        }
    } else {
        CreateMon(wildMon, species, level, 0x20, TRUE, personality, OT_ID_PLAYER_ID, 0);
    }

    SetMonData(wildMon, MON_DATA_OT_ID, &encounterGen->playerId);
    GF_ASSERT(addGeneratedMonToBattleSetupParty(battler, encounterGen, wildMon, battleSetup));
    Heap_Free(wildMon);
    return TRUE;
}

static BOOL generateWildEncounter(WildAttempt *attempt, BOOL forceOnePerfectIV, int battler, BattleSetup *battleSetup) {
    EncounterGenState *encounterGen = attempt->encounterGen;

    // Rolled once, before any slot/level/nature calls, so the accepted attempt's own RNG chain is untouched.
    BOOL wantShiny = rollWildShiny();
    int shinyRequirement = wantShiny ? WILD_SHINY_REQ_SHINY : WILD_SHINY_REQ_NOT_SHINY;

    // With a Cute Charm lead most attempts end in the fixed-form PID, so the plain retry loop below is quick there.
    if (wantShiny && !(encounterGen->ability == ABILITY_CUTE_CHARM && !encounterGen->isEgg)) {
        if (searchShinyAttempt(attempt, forceOnePerfectIV)) {
            return addWildMonToParty(attempt, WILD_ROLL_PERSONALITY, battler, forceOnePerfectIV, WILD_SHINY_REQ_SHINY, battleSetup);
        }
        shinyRequirement = WILD_SHINY_REQ_NONE; // nothing found in time, accept whatever comes
    }

    for (int tries = 0;; ++tries) {
        if (tries >= WILD_SHINY_MAX_ATTEMPTS) {
            shinyRequirement = WILD_SHINY_REQ_NONE; // safety valve, accept whatever comes
        }

        int rollResult = rollWildAttempt(attempt);
        if (rollResult == WILD_ROLL_RETRY) {
            continue;
        }
        if (rollResult == WILD_ROLL_BLOCKED) {
            if (wantShiny && shinyRequirement != WILD_SHINY_REQ_NONE) {
                continue; // keep searching for a shiny attempt that isn't blocked
            }
            return FALSE;
        }
        if (rollResult == WILD_ROLL_PERSONALITY && !wildPersonalityMeetsShinyRequirement(encounterGen->playerId, attempt->personality, shinyRequirement)) {
            continue;
        }
        if (addWildMonToParty(attempt, rollResult, battler, forceOnePerfectIV, shinyRequirement, battleSetup)) {
            return TRUE;
        }
    }
}

static BOOL FieldSystem_GenerateRegularEncounter(Pokemon *leadMon, int rodType, EncounterGenState *encounterGen, EncounterSlot *encSlots, u8 encType, int battler, BattleSetup *battleSetup) {
    WildAttempt attempt;
    initWildAttempt(&attempt, WILD_ATTEMPT_REGULAR, NULL, leadMon, encounterGen, encSlots, encType, rodType, 0, 0);
    return generateWildEncounter(&attempt, FALSE, battler, battleSetup);
}

static BOOL FieldSystem_GenerateSafariEncounter(FieldSystem *fieldSystem, Pokemon *leadMon, int rodType, EncounterGenState *encounterGen, u8 encType, int battler, BattleSetup *battleSetup) {
    SafariZoneAreaSet *areaSet = SafariZone_GetAreaSet(Save_SafariZone_Get(fieldSystem->saveData), 3);
    EncounterSlot *encSlots;
    int area = ov02_0224E340(fieldSystem);
    TimeOfDayWildParam timeOfDay = GF_RTC_GetTimeOfDayWildParam();

    switch (encType) {
    case ENCOUNTER_TYPE_LAND:
        encSlots = SafariZoneAreaSet_LoadAreaEncounters(areaSet, area, SAFARI_ENCOUNTER_SLOTS_LAND, timeOfDay, HEAP_ID_FIELD1);
        break;
    case ENCOUNTER_TYPE_SURFING:
        encSlots = SafariZoneAreaSet_LoadAreaEncounters(areaSet, area, SAFARI_ENCOUNTER_SLOTS_SURFING, timeOfDay, HEAP_ID_FIELD1);
        break;
    case ENCOUNTER_TYPE_FISHING:
        encSlots = SafariZoneAreaSet_LoadAreaEncounters(areaSet, area, SAFARI_ENCOUNTER_SLOTS_ROD(rodType), timeOfDay, HEAP_ID_FIELD1);
        break;
    default: // other encounter types invalid in safari
        GF_ASSERT(FALSE);
    }

    WildAttempt attempt;
    initWildAttempt(&attempt, WILD_ATTEMPT_SAFARI, fieldSystem, leadMon, encounterGen, encSlots, encType, rodType, 0, 0);
    BOOL created = generateWildEncounter(&attempt, TRUE, battler, battleSetup);
    Heap_Free(encSlots);
    return created;
}

static BOOL FieldSystem_GenerateBugContestEncounter_Internal(FieldSystem *fieldSystem, Pokemon *leadMon, int rodType, EncounterGenState *encounterGen, u8 encType, int battler, BattleSetup *battleSetup) {
    WildAttempt attempt;
    initWildAttempt(&attempt, WILD_ATTEMPT_BUG_CONTEST, fieldSystem, leadMon, encounterGen, NULL, encType, rodType, 0, 0);
    return generateWildEncounter(&attempt, TRUE, battler, battleSetup);
}

void FieldSystem_GenerateSingleWildPokemon(FieldSystem *fieldSystem, u16 species, u8 level, BOOL shiny, BattleSetup *battleSetup) {
    EncounterGenState encounterGen;
    Pokemon *leadMon = Party_GetMonByIndex(SaveArray_Party_Get(fieldSystem->saveData), 0);
    EncounterGenState_Init(fieldSystem, leadMon, NULL, &encounterGen);
    if (shiny) {
        u32 otid = PlayerProfile_GetTrainerID(Save_PlayerData_GetProfile(fieldSystem->saveData));
        generateWildShinyAndAddToParty(species, level, BATTLER_ENEMY, otid, &encounterGen, leadMon, battleSetup);
    } else {
        WildAttempt attempt;
        initWildAttempt(&attempt, WILD_ATTEMPT_SINGLE, fieldSystem, leadMon, &encounterGen, NULL, 0, 0, species, level);
        generateWildEncounter(&attempt, FALSE, BATTLER_ENEMY, battleSetup);
    }
}

BOOL FieldSystem_CanGenerateStepEncounter(FieldSystem *fieldSystem, u8 metatileBehavior) {
    int encounterRate = 0;

    if (MetatileBehavior_CanGenerateWalkingEncounters(metatileBehavior)) {
        if (MetatileBehavior_IsSurfableWater(metatileBehavior)) {
            encounterRate = FieldSystem_GetSurfingEncounterRate(fieldSystem);
        } else {
            encounterRate = FieldSystem_GetLandEncounterRate(fieldSystem);
        }
    }

    if (encounterRate != 0) {
        if (fieldSystem->location->mapId == MAP_RUINS_OF_ALPH_UNDERGROUND_HALL
            || fieldSystem->location->mapId == MAP_RUINS_OF_ALPH_UNDERGROUND_HALL_SINJOH_EVENT
            || fieldSystem->location->mapId == MAP_RUINS_OF_ALPH_HALL_ENTRANCE_SINJOH_EVENT
            || fieldSystem->location->mapId == MAP_RUINS_OF_ALPH_UNDERGROUND_HALL_SINJOH_EVENT_2) {
            SaveVarsFlags *varsFlags = Save_VarsFlags_Get(fieldSystem->saveData);
            for (u8 i = 0; i < 4; ++i) {
                if (Save_VarsFlags_CheckAlphPuzzleFlag(varsFlags, i)) {
                    return TRUE;
                }
            }
            return FALSE;
        }
        return TRUE;
    }

    return FALSE;
}

static int FieldSystem_GetLandEncounterRate(FieldSystem *fieldSystem) {
    const EncounterData *encData = MapEvents_GetLoadedEncTable(fieldSystem);
    return encData->encounterRate_walking;
}

static int FieldSystem_GetSurfingEncounterRate(FieldSystem *fieldSystem) {
    const EncounterData *encData = MapEvents_GetLoadedEncTable(fieldSystem);
    return encData->encounterRate_surfing;
}

static u8 FieldSystem_GetFishingEncounterRate(FieldSystem *fieldSystem, u8 rodType) {
    const EncounterData *encData = MapEvents_GetLoadedEncTable(fieldSystem);
    u8 ret;

    switch (rodType) {
    case ROD_TYPE_OLD:
        ret = encData->encounterRate_oldRod;
        break;
    case ROD_TYPE_GOOD:
        ret = encData->encounterRate_goodRod;
        break;
    case ROD_TYPE_SUPER:
        ret = encData->encounterRate_superRod;
        break;
    default:
        GF_ASSERT(FALSE);
        return 0;
    }
    if (ret == 0) {
        return 0;
    }
    if (FollowMon_IsActive(fieldSystem)) {
        ret += getFriendshipBoostToFishingBiteRate(GetMonData(GetFirstAliveMonInParty_CrashIfNone(SaveArray_Party_Get(fieldSystem->saveData)), MON_DATA_FRIENDSHIP, NULL));
    }
    return ret;
}

static u8 getFriendshipBoostToFishingBiteRate(u8 friendship) {
    if (friendship <= 99) {
        return 0;
    }
    if (friendship <= 149) {
        return 20;
    }
    if (friendship <= 199) {
        return 30;
    }
    if (friendship <= 249) {
        return 40;
    }
    return 50;
}

static int FieldSystem_GetRockSmashEncounterRate(FieldSystem *fieldSystem) {
    const EncounterData *encData = MapEvents_GetLoadedEncTable(fieldSystem);
    return encData->encounterRate_rockSmash;
}

static BOOL chooseAbilityCoercedSlot(EncounterSlot *encSlots, u8 numEncSlots, u8 type, u8 *slot) {
    u8 foundSlots[NUM_ENCOUNTERS_LAND];
    u8 i;
    u8 numFoundSlots;

    for (i = 0; i < NUM_ENCOUNTERS_LAND; ++i) {
        foundSlots[i] = 0;
    }
    numFoundSlots = 0;
    for (i = 0; i < numEncSlots; ++i) {
        u8 type1 = GetMonBaseStat(encSlots[i].species, BASE_TYPE1);
        u8 type2 = GetMonBaseStat(encSlots[i].species, BASE_TYPE2);
        if (type1 == type || type2 == type) {
            foundSlots[numFoundSlots++] = i;
        }
    }
    if (numFoundSlots == 0 || numFoundSlots == numEncSlots) {
        return FALSE;
    }
    *slot = foundSlots[LCRandom() % numFoundSlots];
    return TRUE;
}

static BOOL EncounterSlot_AbilityInfluenceOnSlotChoiceCheck(Pokemon *leadMon, EncounterGenState *encounterGen, EncounterSlot *encSlots, u8 numSlots, u8 type, u8 ability, u8 *slotNum) {
    if (!encounterGen->isEgg && encounterGen->ability == ability && LCRandRange(2) == 0) {
        return chooseAbilityCoercedSlot(encSlots, numSlots, type, slotNum);
    }

    return FALSE;
}

static u8 ApplyAbilityEffectToEncounterRate(BOOL isFishing, u8 encounterRate, EncounterGenState *encounterGen, u16 weatherType, Pokemon *leadMon) {
    int ret = encounterRate;

    if (!encounterGen->isEgg) {
        if (isFishing) {
            if (encounterGen->ability == ABILITY_STICKY_HOLD || encounterGen->ability == ABILITY_SUCTION_CUPS) {
                ret *= 2;
            }
        } else if (encounterGen->ability == ABILITY_ARENA_TRAP || encounterGen->ability == ABILITY_NO_GUARD || encounterGen->ability == ABILITY_ILLUMINATE) {
            ret *= 2;
        } else if (encounterGen->ability == ABILITY_SNOW_CLOAK) {
            if (weatherType == WEATHER_SNOW) {
                ret /= 2;
            }
        } else if (encounterGen->ability == ABILITY_WHITE_SMOKE || encounterGen->ability == ABILITY_QUICK_FEET || encounterGen->ability == ABILITY_STENCH) {
            ret /= 2;
        }
        if (ret > 100) {
            ret = 100;
        }
    }

    return ret;
}

static BOOL DoesAbilitySuppressEncounter(EncounterGenState *encounterGen, Pokemon *leadMon, u8 level) {
    if (encounterGen->isSweetScent) {
        return FALSE;
    }
    if (!encounterGen->isEgg) {
        if (encounterGen->ability == ABILITY_KEEN_EYE || encounterGen->ability == ABILITY_INTIMIDATE) {
            u8 leadMonLevel = GetMonData(leadMon, MON_DATA_LEVEL, NULL);
            if (leadMonLevel <= 5) {
                return FALSE;
            } else if (level <= leadMonLevel - 5 && LCRandRange(2) == 0) {
                return TRUE;
            }
        }
    }

    return FALSE;
}

static void FieldSystem_CreateBattleSetupForWildBattle(FieldSystem *fieldSystem, u8 encounterCtx, BattleSetup **pBattleSetup) {
    switch (encounterCtx) {
    case ENCOUNTER_CONTEXT_SAFARI: {
        LocalFieldData *localFieldData = Save_LocalFieldData_Get(fieldSystem->saveData);
        u16 *pSafariBalls = LocalFieldData_GetSafariBallsCounter(localFieldData);
        *pBattleSetup = BattleSetup_New_SafariZone(HEAP_ID_FIELD2, *pSafariBalls);
    } break;
    case ENCOUNTER_CONTEXT_BUG_CONTEST: {
        BugContest *bugContest = FieldSystem_BugContest_Get(fieldSystem);
        u16 *pSportBalls = BugContest_GetSportBallsAddr(bugContest);
        *pBattleSetup = BattleSetup_New_BugContest(HEAP_ID_FIELD2, *pSportBalls, bugContest->mon);
    } break;
    default:
        *pBattleSetup = BattleSetup_New(HEAP_ID_FIELD2, BATTLE_TYPE_NONE);
        break;
    }
}

static BOOL EncounterGen_DoesRepelSuppressEncounter(u8 roamerLevel, EncounterGenState *encounterGen) {
    if (encounterGen->isRepel && encounterGen->level > roamerLevel) {
        return TRUE;
    }

    return FALSE;
}

static BOOL EncounterGen_CanGenerateUnownEncounter(EncounterGenState *encounterGen) {
    for (int i = 0; i < ALPH_PUZZLE_MAX; ++i) {
        if (encounterGen->unownPuzzleFlags[i]) {
            return TRUE;
        }
    }

    return FALSE;
}

static void initRoamingWildmon(u32 otId, Roamer *roamer, BattleSetup *battleSetup) {
    Pokemon *pokemon = AllocMonZeroed(HEAP_ID_FIELD1);
    u32 species = GetRoamerData(roamer, ROAMER_DATA_SPECIES);
    u8 level = GetRoamerData(roamer, ROAMER_DATA_LEVEL);
    u32 ivs = GetRoamerData(roamer, ROAMER_DATA_IVS);
    u32 pid = GetRoamerData(roamer, ROAMER_DATA_PERSONALITY);
    u32 status = GetRoamerData(roamer, ROAMER_DATA_STATUS);
    u16 hp = GetRoamerData(roamer, ROAMER_DATA_HP);

    CreateMonWithFixedIVs(pokemon, species, level, ivs, pid);
    SetMonData(pokemon, MON_DATA_OT_ID, &otId);
    SetMonData(pokemon, MON_DATA_STATUS, &status);
    SetMonData(pokemon, MON_DATA_HP, &hp);
    GF_ASSERT(Party_AddMon(battleSetup->party[BATTLER_ENEMY], pokemon));
    Heap_Free(pokemon);
}

static BOOL getRandomActiveRoamerInCurrMap(FieldSystem *fieldSystem, Roamer **pRoamer) {
    Roamer *foundRoamers[ROAMER_MAX];
    u8 nRoamers = 0;
    RoamerSaveData *saveRoamers = Save_Roamers_Get(fieldSystem->saveData);

    for (u8 i = 0; i < ROAMER_MAX; ++i) {
        u32 mapId = GetRoamMapByLocationIdx(Roamer_GetLocation(saveRoamers, i));
        if (GetRoamerIsActiveByIndex(saveRoamers, i) && mapId == fieldSystem->location->mapId) {
            foundRoamers[nRoamers] = Roamers_GetRoamMonStats(saveRoamers, i);
            ++nRoamers;
        }
    }
    if (nRoamers == 0) {
        return FALSE;
    }
    if (LCRandRange(2) == 0) {
        return FALSE;
    }
    if (nRoamers > 1) {
        u16 chosenRoamer = LCRandRange(nRoamers);
        *pRoamer = foundRoamers[chosenRoamer];
    } else {
        *pRoamer = foundRoamers[0];
    }
    return TRUE;
}

static BOOL hasCaughtUnownForm(Pokedex *pokedex, u8 numForms, u8 targetForm) {
    for (int i = 0; i < numForms; ++i) {
        if (targetForm == Pokedex_GetSeenFormByIdx_Unown(pokedex, i, TRUE)) {
            return TRUE;
        }
    }

    return FALSE;
}

static const u8 sUnlockedUnown_Kabuto[] = {
    UNOWN_A,
    UNOWN_B,
    UNOWN_C,
    UNOWN_D,
    UNOWN_E,
    UNOWN_F,
    UNOWN_G,
    UNOWN_H,
    UNOWN_I,
    UNOWN_J,
};
static const u8 sUnlockedUnown_Aerodactyl[] = {
    UNOWN_R,
    UNOWN_S,
    UNOWN_T,
    UNOWN_U,
    UNOWN_V,
};
static const u8 sUnlockedUnown_Omanyte[] = {
    UNOWN_K,
    UNOWN_L,
    UNOWN_M,
    UNOWN_N,
    UNOWN_O,
    UNOWN_P,
    UNOWN_Q,
};
static const u8 sUnlockedUnown_HoOh[] = {
    UNOWN_W,
    UNOWN_X,
    UNOWN_Y,
    UNOWN_Z,
};
static const u8 sUnlockedUnown_Sinjoh[] = {
    UNOWN_EXCLAMATION_MARK,
    UNOWN_QUESTION_MARK,
};

static const UnlockedUnown sUnlockedUnown[] = {
    { NELEMS(sUnlockedUnown_Kabuto),     sUnlockedUnown_Kabuto     },
    { NELEMS(sUnlockedUnown_Aerodactyl), sUnlockedUnown_Aerodactyl },
    { NELEMS(sUnlockedUnown_Omanyte),    sUnlockedUnown_Omanyte    },
    { NELEMS(sUnlockedUnown_HoOh),       sUnlockedUnown_HoOh       },
    { NELEMS(sUnlockedUnown_Sinjoh),     sUnlockedUnown_Sinjoh     },
};

static u8 EncounterGen_ChooseUnownForm(EncounterGenState *encounterGen) {
    u8 ret;
    int i;
    int j;
    int numUncaughtUnown;
    int numAvailableUnown;
    int numSeenUnown;
    BOOL isUnownSounds = FALSE;
    u8 *availableUnown;
    u8 *availableUncaughtUnown;
    const UnlockedUnown *r4;

    if (encounterGen->isSinjohMap) {
        return sUnlockedUnown[4].letters[LCRandom() % 2];
    }

    availableUnown = Heap_AllocAtEnd(HEAP_ID_FIELD1, UNOWN_FORM_MAX);
    availableUncaughtUnown = Heap_AllocAtEnd(HEAP_ID_FIELD1, UNOWN_FORM_MAX);
    MI_CpuClear8(availableUnown, UNOWN_FORM_MAX);
    MI_CpuClear8(availableUncaughtUnown, UNOWN_FORM_MAX);
    if (GetRadioMusicPlayingSeq() == RADIO_MUSIC_SEQ_UNOWN) {
        isUnownSounds = TRUE;
    }
    numUncaughtUnown = 0;
    numAvailableUnown = 0;
    numSeenUnown = Pokedex_GetSeenFormNum_Unown(encounterGen->pokedex, TRUE);
    for (i = 0; i < 4; ++i) {
        if (!encounterGen->unownPuzzleFlags[i]) {
            continue;
        }
        r4 = &sUnlockedUnown[i];
        for (j = 0; j < r4->num; ++j) {
            if (!hasCaughtUnownForm(encounterGen->pokedex, numSeenUnown, r4->letters[j])) {
                availableUncaughtUnown[numUncaughtUnown] = r4->letters[j];
                ++numUncaughtUnown;
            }
            availableUnown[numAvailableUnown] = r4->letters[j];
            ++numAvailableUnown;
        }
    }
    if (isUnownSounds && numUncaughtUnown > 0 && (LCRandom() % 100) < 50) {
        ret = availableUncaughtUnown[LCRandom() % numUncaughtUnown];
    } else {
        ret = availableUnown[LCRandom() % numAvailableUnown];
    }
    Heap_Free(availableUncaughtUnown);
    Heap_Free(availableUnown);
    return ret;
}

static BOOL addGeneratedMonToBattleSetupParty(int battler, EncounterGenState *encounterGen, Pokemon *pokemon, BattleSetup *battleSetup) {
    WildMonSetRandomHeldItem(pokemon, battleSetup->battleType, !encounterGen->isEgg && encounterGen->ability == ABILITY_COMPOUNDEYES ? 1 : 0);
    if (GetMonData(pokemon, MON_DATA_SPECIES, NULL) == SPECIES_UNOWN) {
        u8 form = EncounterGen_ChooseUnownForm(encounterGen);
        SetMonData(pokemon, MON_DATA_FORM, &form);
    }
    return Party_AddMon(battleSetup->party[battler], pokemon);
}

static u8 ApplyAbilityEffectToSlotLevel(EncounterSlot *encSlots, int numEncSlots, EncounterGenState *encounterGen, u8 chosenSlot) {
    if (!encounterGen->isEgg && (encounterGen->ability == ABILITY_VITAL_SPIRIT || encounterGen->ability == ABILITY_HUSTLE || encounterGen->ability == ABILITY_PRESSURE)) {
        if (LCRandRange(2) == 0) {
            return chosenSlot;
        }
        for (u8 i = 0; i < numEncSlots; ++i) {
            if (encSlots[i].species == encSlots[chosenSlot].species && encSlots[i].maxLevel > encSlots[chosenSlot].maxLevel) {
                chosenSlot = i;
            }
        }
        return chosenSlot;
    }

    return chosenSlot;
}

static void EncounterGenState_Init(FieldSystem *fieldSystem, Pokemon *pokemon, const EncounterData *encData, EncounterGenState *encounterGen) {
    if (!GetMonData(pokemon, MON_DATA_IS_EGG, NULL)) {
        encounterGen->isEgg = FALSE;
        encounterGen->ability = GetMonData(pokemon, MON_DATA_ABILITY, NULL);
    } else {
        encounterGen->isEgg = TRUE;
        encounterGen->ability = NUM_ABILITIES;
    }
    encounterGen->level = 0;
    encounterGen->isRepel = FALSE;
    encounterGen->isSweetScent = FALSE;
    encounterGen->unk_0F = 0;
    encounterGen->unk_10 = 0;
    SaveVarsFlags *saveVarsFlags = Save_VarsFlags_Get(fieldSystem->saveData);
    encounterGen->isSinjohMap = fieldSystem->location->mapId == MAP_RUINS_OF_ALPH_HALL_ENTRANCE_SINJOH_EVENT;
    for (int i = 0; i < 4; ++i) {
        encounterGen->unownPuzzleFlags[i] = Save_VarsFlags_CheckAlphPuzzleFlag(saveVarsFlags, i);
    }
    encounterGen->pokedex = Save_Pokedex_Get(fieldSystem->saveData);
    encounterGen->playerId = PlayerProfile_GetTrainerID(Save_PlayerData_GetProfile(fieldSystem->saveData));
}

static void FieldSystem_UpdateTurnFrameCounter(FieldSystem *fieldSystem) {
    u8 facingDirection = PlayerAvatar_GetFacingDirection(fieldSystem->playerAvatar);
    BOOL isTurnFrame = FALSE;
    switch (facingDirection) {
    case DIR_NORTH:
        if (fieldSystem->lastFacingDirection == DIR_SOUTH) {
            isTurnFrame = TRUE;
        }
        break;
    case DIR_SOUTH:
        if (fieldSystem->lastFacingDirection == DIR_NORTH) {
            isTurnFrame = TRUE;
        }
        break;
    case DIR_WEST:
        if (fieldSystem->lastFacingDirection == DIR_EAST) {
            isTurnFrame = TRUE;
        }
        break;
    case DIR_EAST:
        if (fieldSystem->lastFacingDirection == DIR_WEST) {
            isTurnFrame = TRUE;
        }
        break;
    default:
        GF_ASSERT(FALSE);
        return;
    }
    if (isTurnFrame && fieldSystem->reverseTurnFrameSteps < 0xFFFF) {
        ++fieldSystem->reverseTurnFrameSteps;
    }
    fieldSystem->lastFacingDirection = facingDirection;
}