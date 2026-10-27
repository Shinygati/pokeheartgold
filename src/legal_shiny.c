#include "legal_shiny.h"

// PKHeX-legal shiny generation for HeartGold/SoulSilver (Method 1 / Method K).
//
// Nothing here edits a PID or IVs after the fact. A shiny PID is built directly from the trainer's
// TID/SID (for a PID low half `lo`, every high half (TSV ^ lo) & ~7 | w, w = 0..7, is shiny), the
// LCRNG seed that outputs exactly that PID is solved for, and the seed is only kept if the
// unmodified calls after it (IVs) and before it (activation / slot / level / Synchronize / nature,
// as PKHeX's MethodK.cs walks them) already are what we want. The mon is then created by pointing
// the LCRNG at that seed, so the game's own code produces the IVs, held item and Unown form calls.
//
// Shininess is always judged against the full 32-bit OT ID (TID + SID) stored on the mon, so the
// mon is shiny for everyone it is traded to, in Pokemon Bank/HOME, and in PKHeX.

#ifndef LEGAL_SHINY_HOST
#include "math_util.h"
#define LS_MTRandom()        MTRandom()
#define LS_LCRandom()        LCRandom()
#define LS_GetSeed()         GetLCRNGSeed()
#define LS_SetSeed(seed)     SetLCRNGSeed(seed)
#else
#define LS_MTRandom()        MTRNG_Next()
#define LS_LCRandom()        LCRNG_Next()
#define LS_GetSeed()         LCRNG_GetSeed()
#define LS_SetSeed(seed)     LCRNG_SetSeed(seed)
#endif

#define LS_MULT  0x41C64E6Du
#define LS_ADD   0x00006073u
#define LS_RMULT 0xEEB9EB65u
#define LS_RADD  0x0A3561A1u

#define LS_WINDOW_MAX      100 // rejected-PID pairs to walk back through
#define LS_PID_LOOP_MAX    400 // forward PID loop cap for non-shiny frames
#define LS_NORMAL_SKIP_MAX 30000 // forward frame candidates for non-shiny frames
#define LS_REROLL_TRIES    4 // Safari Zone / Bug Contest: nature..IVs rolled up to 4 times for a 31 IV

enum {
    LS_LEAD_NONE,
    LS_LEAD_SYNC,
};

static inline u32 LS_Next(u32 seed) {
    return seed * LS_MULT + LS_ADD;
}

static inline u32 LS_Prev(u32 seed) {
    return seed * LS_RMULT + LS_RADD;
}

BOOL LegalShiny_Roll(u32 odds) {
    if (odds <= 1) {
        return TRUE;
    }

    return (LS_MTRandom() % odds) == 0;
}

BOOL LegalShiny_IsShiny(u32 otId, u32 personality) {
    return ((otId >> 16) ^ (otId & 0xFFFF) ^ (personality >> 16) ^ (personality & 0xFFFF)) < 8;
}

void LegalShiny_ApplyFrame(const LegalFrame *frame) {
    LS_SetSeed(frame->pidSeed);
}

// Port of PKHeX LCRNGReversal.GetSeeds: every origin seed whose next two outputs are (first, second).
// Lattice bounds from StarfBerry's LCG_Recovery. At most 3 results, about 1 on average.
static int LS_SeedsFromPair(u32 first16, u32 second16, u32 *out) {
    u32 first = first16 << 16;
    u32 second = second16 << 16;
    u32 tmp = ((second * LS_RMULT - first) >> 16) * 0x7ED7u;
    u32 lo = (tmp + 0x50F5A0Bu) >> 16;
    u32 up = (tmp + 0x50F40B4u) >> 16;
    int count = 0;

    if (lo != up) {
        return 0;
    }

    u32 low = (lo * 0xD33u) % 0x7ED7u;

    do {
        u32 seed = LS_Prev(second | low);

        if ((seed & 0xFFFF0000) == first) {
            out[count++] = LS_Prev(seed);
        }
    } while ((low += 0x7ED7u) < 0x10000);

    return count;
}

static int LS_CountIVsAtLeast(u32 ivWord1, u32 ivWord2, u32 minIV) {
    int count = 0;
    int i;

    for (i = 0; i < 3; i++) {
        if (((ivWord1 >> (i * 5)) & 0x1F) >= minIV) {
            count++;
        }

        if (((ivWord2 >> (i * 5)) & 0x1F) >= minIV) {
            count++;
        }
    }

    return count;
}

// pidSeed >> 16 is the PID's high half; the two calls after it are the IVs.
static BOOL LS_FrameHas31(u32 pidSeed) {
    u32 iv1State = LS_Next(pidSeed);
    u32 iv2State = LS_Next(iv1State);

    return LS_CountIVsAtLeast(iv1State >> 16, iv2State >> 16, 31) > 0;
}

static BOOL LS_IsLevelAccepted(const LegalFrameSpec *spec, u32 level, BOOL relaxLevel) {
    if (relaxLevel) {
        return level >= spec->minLevel;
    }

    return level == spec->level;
}

// Given the state right after the slot call (slotState >> 16 is the slot rand), checks the slot,
// its level and the encounter activation call before it.
static BOOL LS_IsSlotAndActivationValid(const LegalFrameSpec *spec, u32 slotState, u32 levelRand16, BOOL relaxLevel, int lead, u8 *outLevel) {
    u8 slot = spec->slotForRoll[(slotState >> 16) % spec->slotRollMod];
    u32 level;

    if (slot >= spec->numSlots || spec->slotSpecies[slot] != spec->species) {
        return FALSE;
    }

    if (spec->levelRand) {
        u32 range = (u32)spec->slotMaxLevel[slot] - spec->slotMinLevel[slot] + 1;
        level = (levelRand16 % range) + spec->slotMinLevel[slot];
    } else {
        level = spec->slotMaxLevel[slot];
    }

    if (!LS_IsLevelAccepted(spec, level, relaxLevel)) {
        return FALSE;
    }

    if (spec->activation == LEGAL_ACTIVATION_RATE || (spec->activation == LEGAL_ACTIVATION_BUG_CONTEST && lead == LS_LEAD_SYNC)) {
        u32 activation = LS_Prev(slotState) >> 16; // call before the slot

        if (activation % 100 >= spec->activationRate) {
            return FALSE;
        }
    }

    *outLevel = (u8)level;
    return TRUE;
}

static BOOL LS_IsSlotFromLeadValid(const LegalFrameSpec *spec, u32 leadState, BOOL relaxLevel, int lead, u8 *outLevel) {
    u32 s1 = LS_Prev(leadState);

    if (!spec->levelRand) {
        return LS_IsSlotAndActivationValid(spec, s1, 0, relaxLevel, lead, outLevel);
    }

    return LS_IsSlotAndActivationValid(spec, LS_Prev(s1), s1 >> 16, relaxLevel, lead, outLevel);
}

// leadState >> 16 is the rand right before the PID loop (Synchronize proc or nature call).
// Method K: a nature-call match is tried first (no lead), then a passing Synchronize proc.
static BOOL LS_IsLeadPositionValid(const LegalFrameSpec *spec, u32 leadState, u32 nature, BOOL syncLead, BOOL relaxLevel, u8 *outLevel) {
    u32 p0 = leadState >> 16;

    if (p0 % 25 == nature && LS_IsSlotFromLeadValid(spec, leadState, relaxLevel, LS_LEAD_NONE, outLevel)) {
        return TRUE;
    }

    if (syncLead && (p0 & 1) == 0 && LS_IsSlotFromLeadValid(spec, leadState, relaxLevel, LS_LEAD_SYNC, outLevel)) {
        return TRUE;
    }

    return FALSE;
}

// Same walk as PKHeX MethodK.GetOriginSeed: try the lead/nature call right before the PID, then
// step back one rejected PID pair at a time until a pair that would itself have been accepted.
static BOOL LS_IsMethodKFrameValid(const LegalFrameSpec *spec, u32 originSeed, u32 nature, BOOL syncLead, BOOL relaxLevel, u8 *outLevel) {
    u32 state = originSeed;
    int i;

    for (i = 0; i < LS_WINDOW_MAX; i++) {
        if (LS_IsLeadPositionValid(spec, state, nature, syncLead, relaxLevel, outLevel)) {
            return TRUE;
        }

        u32 hi = state >> 16;
        state = LS_Prev(state);
        u32 lo = state >> 16;

        if (((hi << 16) | lo) % 25 == nature) {
            return FALSE;
        }

        state = LS_Prev(state);
    }

    return FALSE;
}

typedef struct ShinyStage {
    u8 minIV;
    u8 count;
    u8 relaxLevel;
    u16 budget; // random PID low halves to try
} ShinyStage;

// Worst case (all stages exhausted) is ~15000 low halves; in practice stage 0 or 1 hits almost always.
static const ShinyStage sShinyStages[] = {
    { 31, 2, FALSE, 2500 }, // 2x31, the level the game rolled
    { 31, 2, TRUE, 4000 }, // 2x31, any level this species' slots can roll (rare wide-range slots)
    { 25, 2, TRUE, 2500 }, // 2x25+
    { 0, 0, TRUE, 6000 }, // shiny + legal, any IVs
};

BOOL LegalShiny_FindShinyFrame(const LegalFrameSpec *spec, u32 otId, int nature, BOOL syncLead, LegalFrame *out) {
    u32 tsv = (otId >> 16) ^ (otId & 0xFFFF);
    BOOL wild = spec != NULL && spec->type != LEGAL_FRAME_METHOD1;
    BOOL need31 = wild && spec->rerollMin31;
    u32 rng = LS_MTRandom();
    u32 seeds[4];
    int stage;

    if (!wild) {
        syncLead = FALSE;
    }

    for (stage = 0; stage < (int)(sizeof(sShinyStages) / sizeof(sShinyStages[0])); stage++) {
        const ShinyStage *st = &sShinyStages[stage];
        u32 iter;

        for (iter = 0; iter < st->budget; iter++) {
            rng = LS_Next(rng);
            u32 lo = rng >> 16;
            u32 base = (tsv ^ lo) & 0xFFF8;
            u32 w, wEnd;

            if (nature >= 0) {
                // pid % 25 == (hi * 11 + lo) % 25 because 65536 % 25 == 11, and 11 * 16 == 1 (mod 25)
                u32 n0 = (base * 11 + lo) % 25;
                w = (((u32)nature + 25 - n0) * 16) % 25;

                if (w > 7) {
                    continue;
                }

                wEnd = w + 1;
            } else {
                w = 0;
                wEnd = 8;
            }

            for (; w < wEnd; w++) {
                u32 hi = base | w;
                u32 pid = (hi << 16) | lo;
                u32 pidNature = pid % 25;
                int n = LS_SeedsFromPair(lo, hi, seeds);
                int i;

                for (i = 0; i < n; i++) {
                    u32 pidSeed = LS_Next(LS_Next(seeds[i]));
                    u32 iv1State = LS_Next(pidSeed);
                    u32 iv2State = LS_Next(iv1State);

                    if (LS_CountIVsAtLeast(iv1State >> 16, iv2State >> 16, st->minIV) < st->count) {
                        continue;
                    }

                    // Safari / Bug Contest: a direct (no re-roll) frame must contain a 31 IV.
                    if (need31 && !LS_FrameHas31(pidSeed)) {
                        continue;
                    }

                    u8 level = spec != NULL ? spec->level : 0;

                    if (wild && !LS_IsMethodKFrameValid(spec, seeds[i], pidNature, syncLead, st->relaxLevel, &level)) {
                        continue;
                    }

                    out->personality = pid;
                    out->pidSeed = pidSeed;
                    out->level = level;
                    return TRUE;
                }
            }
        }
    }

    return FALSE;
}

BOOL LegalShiny_FindNormalFrame(const LegalFrameSpec *spec, u32 preSlotSeed, u32 otId, int syncNature, LegalFrame *out) {
    u32 candidate = preSlotSeed;
    int lead = syncNature >= 0 ? LS_LEAD_SYNC : LS_LEAD_NONE;
    int tries = spec->rerollMin31 ? LS_REROLL_TRIES : 1;
    int skip;

    for (skip = 0; skip < LS_NORMAL_SKIP_MAX; skip++, candidate = LS_Next(candidate)) {
        u32 slotState = LS_Next(candidate);
        u32 state = slotState;
        u8 level;
        u32 pid = 0;
        BOOL ok = TRUE;
        int t;

        if (spec->levelRand) {
            state = LS_Next(slotState);
        }

        if (!LS_IsSlotAndActivationValid(spec, slotState, state >> 16, FALSE, lead, &level)) {
            continue;
        }

        for (t = 0; t < tries && ok; t++) {
            u32 nature;
            int pairs;

            state = LS_Next(state); // Synchronize proc, or the nature call

            if (syncNature >= 0) {
                if (((state >> 16) & 1) != 0) {
                    ok = FALSE; // proc failed here; a later frame will pass
                    break;
                }

                nature = (u32)syncNature;
            } else {
                nature = (state >> 16) % 25;
            }

            for (pairs = 0; pairs < LS_PID_LOOP_MAX; pairs++) {
                state = LS_Next(state);
                u32 lo = state >> 16;
                state = LS_Next(state);
                pid = ((state >> 16) << 16) | lo;

                if (pid % 25 == nature) {
                    break;
                }
            }

            if (pairs == LS_PID_LOOP_MAX) {
                ok = FALSE;
                break;
            }

            if (t == tries - 1 || LS_FrameHas31(state)) {
                break; // this try is kept
            }

            state = LS_Next(LS_Next(state)); // this try's IVs had no 31: the game rolls again
        }

        if (!ok || LegalShiny_IsShiny(otId, pid)) {
            continue; // never a shiny outside the shiny roll
        }

        out->personality = pid;
        out->pidSeed = state;
        out->level = level;
        return TRUE;
    }

    return FALSE;
}

void LegalShiny_RollNormalMethod1(u32 otId, int nature, LegalFrame *out) {
    u32 pid;

    while (TRUE) {
        u32 lo = LS_LCRandom();
        u32 hi = LS_LCRandom();

        pid = lo | (hi << 16);

        if (nature >= 0 && pid % 25 != (u32)nature) {
            continue;
        }

        if (!LegalShiny_IsShiny(otId, pid)) {
            break;
        }
    }

    out->personality = pid;
    out->pidSeed = LS_GetSeed();
    out->level = 0;
}

u32 LegalShiny_AdjustEggPersonality(u32 personality, u32 otId, BOOL shiny) {
    u32 nature = personality % 25;
    u32 hi = personality >> 16;

    if (!shiny) {
        if (!LegalShiny_IsShiny(otId, personality)) {
            return personality;
        }

        // hi +/- 25 keeps pid % 25, and two values 25 apart always differ above bit 2.
        hi = hi >= 25 ? hi - 25 : hi + 25;
        return (hi << 16) | (personality & 0xFFFF);
    }

    if (LegalShiny_IsShiny(otId, personality)) {
        return personality;
    }

    u32 tsv = (otId >> 16) ^ (otId & 0xFFFF);
    u32 keep = personality & 0x80FF; // gender byte, ability bit, EGG_GENDER_MALE
    u32 start = (personality >> 8) & 0x7F;
    u32 i;

    // Bits 8-14 of the low half don't affect anything stored on a Gen 4 egg; try them until one
    // of the 8 shiny high halves lands on the same nature.
    for (i = 0; i < 128; i++) {
        u32 lo = keep | (((start + i) & 0x7F) << 8);
        u32 base = (tsv ^ lo) & 0xFFF8;
        u32 n0 = (base * 11 + lo) % 25;
        u32 w = ((nature + 25 - n0) * 16) % 25;

        if (w <= 7) {
            return ((base | w) << 16) | lo;
        }
    }

    // Unreachable in practice; keep the egg's low half and accept a new nature.
    return (((tsv ^ (personality & 0xFFFF)) & 0xFFF8) << 16) | (personality & 0xFFFF);
}

#ifndef LEGAL_SHINY_HOST
void CreateMonWithLegalShinyOdds(Pokemon *mon, int species, int level, int otIdType, int fixedOtId, u32 shinyOtId, u32 odds, int nature) {
    LegalFrame frame;

    if (!LegalShiny_Roll(odds) || !LegalShiny_FindShinyFrame(NULL, shinyOtId, nature, FALSE, &frame)) {
        LegalShiny_RollNormalMethod1(shinyOtId, nature, &frame);
    }

    LegalShiny_ApplyFrame(&frame);
    CreateMon(mon, species, level, 32, TRUE, frame.personality, otIdType, fixedOtId);
}
#endif
