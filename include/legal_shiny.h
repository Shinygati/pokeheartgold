#ifndef POKEHEARTGOLD_LEGAL_SHINY_H
#define POKEHEARTGOLD_LEGAL_SHINY_H

#ifdef LEGAL_SHINY_HOST
#include "legal_shiny_host.h"
#else
#include "global.h"
#endif

// Shiny odds (1 in N). Change these to retune the whole game.
#define SHINY_ODDS_BASE    4096 // wild (all encounter types), static/legendary encounters
#define SHINY_ODDS_STARTER 4096 // Chikorita / Cyndaquil / Totodile
#define SHINY_ODDS_GIFT    512  // script gifts (GiveMon) and gift eggs
#define SHINY_ODDS_EGG     512  // daycare eggs, regardless of parent languages
#define SHINY_ODDS_ROAMER  100  // Raikou, Entei, Latios, Latias

#define LEGAL_SHINY_NATURE_ANY (-1)

// How PKHeX (MethodK.cs) checks the RNG calls that come before the PID.
enum LegalFrameType {
    LEGAL_FRAME_METHOD1 = 0, // statics, gifts, starters, roamers: only PID -> IVs must be consecutive
    LEGAL_FRAME_WILD, // Method K: [activation] -> slot -> [level] -> [lead] -> nature -> PID -> IVs
};

// The rand right before the slot rand that PKHeX checks for some encounter types.
enum LegalActivation {
    LEGAL_ACTIVATION_NONE = 0, // grass, surf, headbutt, Safari grass/surf (Sweet Scent can trigger them)
    LEGAL_ACTIVATION_RATE, // fishing hook / Rock Smash: rand % 100 < activationRate
    LEGAL_ACTIVATION_BUG_CONTEST, // only with a Synchronize lead (can't Sweet Scent): rand % 100 < activationRate
};

#define LEGAL_FRAME_MAX_SLOTS 12

typedef struct LegalFrameSpec {
    u8 type; // enum LegalFrameType
    u8 level; // final level of the mon (== met level)
    u8 minLevel; // lowest level allowed if a shiny search has to re-roll the level (Repel), else 1
    u8 numSlots;
    u8 slotRollMod; // slot = slotForRoll[rand % slotRollMod]: 100, or 10 in the Safari Zone
    u8 levelRand; // a level rand follows the slot rand (everything but grass and the Safari Zone)
    u8 activation; // enum LegalActivation
    u8 activationRate;
    u8 rerollMin31; // Safari Zone / Bug Contest re-roll nature..IVs up to 4 times until a 31 IV appears
    u16 species;
    u8 slotForRoll[100];
    u16 slotSpecies[LEGAL_FRAME_MAX_SLOTS];
    u8 slotMinLevel[LEGAL_FRAME_MAX_SLOTS];
    u8 slotMaxLevel[LEGAL_FRAME_MAX_SLOTS];
} LegalFrameSpec;

typedef struct LegalFrame {
    u32 personality;
    u32 pidSeed; // LCRNG state right after the PID's second call: the next two calls are the IVs
    u8 level; // level the frame's level/slot call produced (== spec level unless relaxed)
} LegalFrame;

// MT-based roll so it never touches the LCRNG calls PKHeX checks.
BOOL LegalShiny_Roll(u32 odds);
BOOL LegalShiny_IsShiny(u32 otId, u32 personality);

// Builds a shiny PID directly (TSV xor), reverse-solves the LCRNG seed that outputs it, and keeps
// the seed only if its untouched IVs give 2x31 and, for wild frames, the calls before the PID form a
// valid Method K frame for this species + level. If the budget runs out it next allows any level a
// slot of this species can roll (out->level), then 2x25+ IVs, then any IVs (always at least one 31
// in the Safari Zone / Bug Contest, which PKHeX requires there).
// nature: LEGAL_SHINY_NATURE_ANY or a fixed nature (Synchronize). syncLead selects the Synchronize
// frame layout for wild frames. Returns FALSE only if every budget ran out (practically never).
BOOL LegalShiny_FindShinyFrame(const LegalFrameSpec *spec, u32 otId, int nature, BOOL syncLead, LegalFrame *out);

// Non-shiny wild frame with a 100% Synchronize proc. Walks forward from preSlotSeed (LCRNG state
// right before the slot call, i.e. right after the hook / Rock Smash / encounter-rate call) one call
// at a time and returns the first frame that is a normal vanilla frame for the same species + level,
// with every Synchronize check passing (if syncNature >= 0) and a non-shiny PID. Safari / Bug
// Contest frames include the vanilla 31-IV re-rolls. The first candidate is the vanilla frame.
BOOL LegalShiny_FindNormalFrame(const LegalFrameSpec *spec, u32 preSlotSeed, u32 otId, int syncNature, LegalFrame *out);

// Non-shiny Method 1 from the live LCRNG: PID pairs until the nature matches and the PID is not
// shiny; the IVs come from the very next two calls.
void LegalShiny_RollNormalMethod1(u32 otId, int nature, LegalFrame *out);

// Points the LCRNG at the frame's IV calls; call CreateMon(..., 32, TRUE, frame.personality, ...)
// right after so the IVs (and later held item / Unown form calls) line up.
void LegalShiny_ApplyFrame(const LegalFrame *frame);

// Eggs have no PID/IV correlation in Gen 4. Returns a PID with the same nature, gender byte,
// ability bit and EGG_GENDER_MALE bit that is shiny (or guaranteed not shiny) for otId.
u32 LegalShiny_AdjustEggPersonality(u32 personality, u32 otId, BOOL shiny);

#ifndef LEGAL_SHINY_HOST
#include "pokemon.h"

// Replacement for CreateMon(mon, species, level, 32, FALSE, 0, otIdType, fixedOtId) for gifts and
// starters: rolls 1/odds against shinyOtId (the full 32-bit ID the mon will end up with); a shiny
// gets a legal Method 1 PID with 2x31 IVs, otherwise a normal Method 1 PID that is never shiny.
// nature: LEGAL_SHINY_NATURE_ANY or a fixed nature.
void CreateMonWithLegalShinyOdds(Pokemon *mon, int species, int level, int otIdType, int fixedOtId, u32 shinyOtId, u32 odds, int nature);
#endif

#endif // POKEHEARTGOLD_LEGAL_SHINY_H
