/*
===========================================================================
sof2_compat.h — JK2 compatibility stubs for jagamex86.dll build
===========================================================================
Included ONLY when SP_GAME is defined (building jagamex86.dll).
Provides stub structs/enums so JK2 game code compiles against SOF2 engine.
Lightsaber/force-power functionality is never active in SOF2 maps.
===========================================================================
*/
#pragma once

// -----------------------------------------------------------------------
// Constants that JK2 game code references
// -----------------------------------------------------------------------
#ifndef MAX_POWERUPS
#define MAX_POWERUPS            16
#endif
#ifndef MAX_AMMO
#define MAX_AMMO                10
#endif
#ifndef MAX_INVENTORY
#define MAX_INVENTORY           15
#endif
#ifndef MAX_SABERS
#define MAX_SABERS              2
#endif
#ifndef MAX_BLADES
#define MAX_BLADES              8
#endif
#define MAX_SECURITY_KEYS           5
#define MAX_SECURITY_KEY_MESSSAGE   24

// -----------------------------------------------------------------------
// Force power enum (JK2/JKA)
// -----------------------------------------------------------------------
typedef enum {
    FP_HEAL = 0,
    FP_LEVITATION,
    FP_SPEED,
    FP_PUSH,
    FP_PULL,
    FP_TELEPATHY,
    FP_GRIP,
    FP_LIGHTNING,
    FP_RAGE,
    FP_PROTECT,
    FP_ABSORB,
    FP_TEAM_HEAL,
    FP_TEAM_FORCE,
    FP_DRAIN,
    FP_SEE,
    FP_SABER_OFFENSE,
    FP_SABER_DEFENSE,
    FP_SABERTHROW,
    NUM_FORCE_POWERS
} forcePowers_t;

// -----------------------------------------------------------------------
// Saber type enum
// -----------------------------------------------------------------------
typedef enum {
    SABER_NONE = 0,
    SABER_SINGLE,
    SABER_STAFF,
    SABER_DAGGER,
    SABER_BROAD,
    SABER_PRONG,
    SABER_ARC,
    SABER_SAI,
    SABER_CLAW,
    SABER_LANCE,
    SABER_STAR,
    SABER_TRIDENT,
    SABER_SITH_SWORD,
    NUM_SABERS
} saberType_t;

// -----------------------------------------------------------------------
// Saber style enum
// -----------------------------------------------------------------------
typedef enum {
    SS_NONE = 0,
    SS_FAST,
    SS_MEDIUM,
    SS_STRONG,
    SS_DESANN,
    SS_TAVION,
    SS_DUAL,
    SS_STAFF,
    SS_NUM_SABER_STYLES
} saber_styles_t;

// -----------------------------------------------------------------------
// Water height level enum
// -----------------------------------------------------------------------
typedef enum {
    WHL_NONE,
    WHL_ANKLES,
    WHL_KNEES,
    WHL_WAIST,
    WHL_TORSO,
    WHL_SHOULDERS,
    WHL_HEAD,
    WHL_UNDER
} waterHeightLevel_t;

// -----------------------------------------------------------------------
// Blade info stub — minimal fields accessed by game code
// -----------------------------------------------------------------------
struct bladeInfo_t {
    qboolean    active;
    int         color;
    float       radius;
    float       length;
    float       lengthMax;
    float       lengthOld;
    vec3_t      muzzlePoint;
    vec3_t      muzzlePointOld;
    vec3_t      muzzleDir;
    vec3_t      muzzleDirOld;
    // trail stub (not needed for SOF2)

    qboolean    Active()  const { return active; }
    float       Length()  const { return length; }
    float       LengthMax() const { return lengthMax; }
    void        SetLength(float l) { length = l; }
    void        Activate()   { active = qtrue; }
    void        Deactivate() { active = qfalse; }
    void        ActivateTrail(float)   {}
    void        DeactivateTrail(float) {}
    void        BladeActivate(int /*iBlade*/, qboolean bActive = qtrue) { active = bActive; }
};

// -----------------------------------------------------------------------
// Saber info stub — all fields accessed by game code
// -----------------------------------------------------------------------
struct saberInfo_t {
    char           *name;
    char           *fullName;
    saberType_t     type;
    char           *model;
    char           *skin;
    int             soundOn;
    int             soundLoop;
    int             soundOff;
    int             numBlades;
    bladeInfo_t     blade[MAX_BLADES];
    int             stylesLearned;
    int             stylesForbidden;
    int             maxChain;
    int             forceRestrictions;
    int             lockBonus;
    int             parryBonus;
    int             breakParryBonus;
    int             breakParryBonus2;
    int             disarmBonus;
    int             disarmBonus2;
    saber_styles_t  singleBladeStyle;
    char           *brokenSaber1;
    char           *brokenSaber2;
    int             saberFlags;
    int             saberFlags2;
    int             spinSound;
    int             swingSound[3];
    int             fallSound[3];
    float           moveSpeedScale;
    float           animSpeedScale;
    int             bladeStyle2Start;
    int             hitOtherEffect;
    int             hitOtherEffect2;

    qboolean    Active() const {
        for (int i = 0; i < numBlades && i < MAX_BLADES; i++) {
            if (blade[i].active) return qtrue;
        }
        return qfalse;
    }
    float   Length(int bladeNum = 0) const {
        if (bladeNum >= 0 && bladeNum < MAX_BLADES) return blade[bladeNum].length;
        return 0.0f;
    }
    float   LengthMax(int bladeNum = 0) const {
        if (bladeNum >= 0 && bladeNum < MAX_BLADES) return blade[bladeNum].lengthMax;
        return 0.0f;
    }
    void    SetLength(float l) {
        for (int i = 0; i < MAX_BLADES; i++) blade[i].length = l;
    }
    void    Activate() {
        for (int i = 0; i < MAX_BLADES; i++) blade[i].active = qtrue;
    }
    void    Deactivate() {
        for (int i = 0; i < MAX_BLADES; i++) blade[i].active = qfalse;
    }
    void    ActivateTrail(float d)   { for (int i = 0; i < MAX_BLADES; i++) blade[i].ActivateTrail(d); }
    void    DeactivateTrail(float d) { for (int i = 0; i < MAX_BLADES; i++) blade[i].DeactivateTrail(d); }
    void    BladeActivate(int iBlade, qboolean bActive = qtrue) {
        if (iBlade >= 0 && iBlade < MAX_BLADES) blade[iBlade].active = bActive;
    }
};
