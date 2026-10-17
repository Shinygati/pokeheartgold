#include "choose_starter.h"

#include "constants/balls.h"
#include "constants/items.h"
#include "constants/species.h"

#include "field_system.h"
#include "launch_application.h"
#include "map_header.h"
#include "math_util.h"
#include "pokedex.h"
#include "screen_fade.h"
#include "task.h"
#include "update_dex_received.h"

// Custom shiny odds for the starters, same idea as wild encounters: each starter rolls once whether it is meant to be
// shiny (1 in STARTER_SHINY_ODDS), then the RNG is moved forward to a spot whose PID has that shininess. CreateMon
// then rolls the PID and IVs from that spot exactly as it always does, so the starter keeps an unbroken PID -> IV
// RNG chain and stays legal. Shininess is judged against the player's full 32-bit trainer ID.
#define STARTER_SHINY_ODDS         4096
#define STARTER_SHINY_SEARCH_LIMIT 400000 // RNG steps searched before giving up (about 8192 are needed on average)

struct ChooseStarterTaskData {
    int state;
    struct ChooseStarterArgs *args;
};

static BOOL CreateStarter(TaskManager *taskManager);
static u32 StarterNextSeed(u32 seed);
static void SeekStarterPersonality(u32 otId, BOOL wantShiny);

void LaunchStarterChoiceScene(FieldSystem *fieldSystem) {
    struct ChooseStarterTaskData *env = Heap_AllocAtEnd(HEAP_ID_FIELD2, sizeof(struct ChooseStarterTaskData));
    env->state = 0;
    TaskManager_Call(fieldSystem->taskman, CreateStarter, env);
}

// One step of the game's LCRNG (same constants as LCRandom).
static u32 StarterNextSeed(u32 seed) {
    return seed * 1103515245 + 24691;
}

// Moves the RNG to the first spot where the next two outputs (low half, then high half) make a PID whose shininess
// matches wantShiny. Nothing is generated here, the RNG just ends up where an unmodified game could have been.
static void SeekStarterPersonality(u32 otId, BOOL wantShiny) {
    u32 seed = GetLCRNGSeed();

    for (u32 n = 0; n < STARTER_SHINY_SEARCH_LIMIT; ++n) {
        u32 seed1 = StarterNextSeed(seed);
        u32 seed2 = StarterNextSeed(seed1);
        u32 personality = (seed1 >> 16) | (seed2 & 0xFFFF0000);

        if ((CalcShininessByOtIdAndPersonality(otId, personality) != 0) == wantShiny) {
            break;
        }
        seed = seed1;
    }

    SetLCRNGSeed(seed);
}

static BOOL CreateStarter(TaskManager *taskManager) {
    FieldSystem *fieldSystem = TaskManager_GetFieldSystem(taskManager);
    struct ChooseStarterTaskData *env = TaskManager_GetEnvironment(taskManager);
    int i;
    u32 mapsec;
    Party *party;

    switch (env->state) {
    case 0:
        BeginNormalPaletteFade(FADE_BOTH_SCREENS, FADE_TYPE_BRIGHTNESS_OUT, FADE_TYPE_BRIGHTNESS_OUT, RGB_BLACK, 6, 1, HEAP_ID_FIELD1);
        env->state = 1;
        break;
    case 1:
        if (!IsPaletteFadeFinished()) {
            break;
        }
        {
            const int species[] = {
                SPECIES_CHIKORITA,
                SPECIES_CYNDAQUIL,
                SPECIES_TOTODILE,
            };
            mapsec = MapHeader_GetMapSec(fieldSystem->location->mapId); // sp14

            env->args = Heap_AllocAtEnd(HEAP_ID_FIELD2, sizeof(struct ChooseStarterArgs));
            env->args->cursorPos = 0;
            env->args->options = Save_PlayerData_GetOptionsAddr(fieldSystem->saveData);
            for (i = 0; i < (int)NELEMS(species); i++) {
                Pokemon *mon = &env->args->starters[i];
                PlayerProfile *profile = Save_PlayerData_GetProfile(fieldSystem->saveData);
                BOOL wantShiny = LCRandRange(STARTER_SHINY_ODDS) == 0;
                ZeroMonData(mon);
                SeekStarterPersonality(PlayerProfile_GetTrainerID(profile), wantShiny);
                CreateMon(mon, species[i], 5, 32, FALSE, 0, OT_ID_PLAYER_ID, 0);
                sub_020720FC(mon, profile, BALL_POKE, mapsec, 12, HEAP_ID_FIELD2);
                {
                    int item = ITEM_NONE;
                    SetMonData(mon, MON_DATA_HELD_ITEM, &item);
                }
            }
        }
        ChooseStarter_LaunchApp(fieldSystem, env->args);
        sub_0203E30C();
        env->state = 2;
        break;
    case 2:
        if (FieldSystem_ApplicationIsRunning(fieldSystem)) {
            break;
        }
        env->state = 3;
        break;
    case 3: {
        Pokedex *pokedex = Save_Pokedex_Get(fieldSystem->saveData);
        party = SaveArray_Party_Get(fieldSystem->saveData);
        Pokemon *myChoice = &env->args->starters[env->args->cursorPos];
        if (Party_AddMon(party, myChoice)) {
            UpdatePokedexWithReceivedSpecies(fieldSystem->saveData, myChoice);
        }
        Pokedex_SetMonCaughtFlag(pokedex, Party_GetMonByIndex(party, 0));
        env->state = 4;
        FieldSystem_LoadFieldOverlay(fieldSystem);
        break;
    }
    case 4:
        if (!sub_020505C8(fieldSystem)) {
            break;
        }
        BeginNormalPaletteFade(FADE_BOTH_SCREENS, FADE_TYPE_BRIGHTNESS_IN, FADE_TYPE_BRIGHTNESS_IN, RGB_BLACK, 6, 1, HEAP_ID_FIELD1);
        env->state = 5;
        break;
    case 5:
        if (!IsPaletteFadeFinished()) {
            break;
        }
        Heap_Free(env->args);
        Heap_Free(env);
        return TRUE;
    }

    return FALSE;
}