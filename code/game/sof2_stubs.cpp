// sof2_stubs.cpp — stub implementations for JK2-only functions not built into jagamex86.dll.
// All stubs return safe defaults (NULL, 0, qfalse) or are no-ops.
// Global variables are zero-initialized stub definitions to satisfy the linker.

#include "common_headers.h"     // q_shared.h, cg_local.h, g_shared.h, g_vehicles.h, etc.
#include "../cgame/cg_media.h"  // cgs_t
#include "wp_saber.h"           // sabersLockMode_t, swingType_t, evasionType_t

// ============================================================
// Global variables — define to match exact extern declarations
// ============================================================

// cg_local.h: extern cg_t cg;
cg_t  cg;

// cg_media.h: extern cgs_t cgs;
cgs_t cgs;

// cg_camera.h: extern camera_s client_camera;
camera_s client_camera;

// cg_local.h: extern centity_t cg_entities[MAX_GENTITIES];
centity_t cg_entities[MAX_GENTITIES];

// cg_local.h: extern weaponInfo_t cg_weapons[MAX_WEAPONS];
weaponInfo_t cg_weapons[MAX_WEAPONS];

// g_vehicles.h: extern vehicleInfo_t g_vehicleInfo[MAX_VEHICLES];
vehicleInfo_t g_vehicleInfo[MAX_VEHICLES];

// g_vehicles.h: extern vehWeaponInfo_t g_vehWeaponInfo[MAX_VEH_WEAPONS];
vehWeaponInfo_t g_vehWeaponInfo[MAX_VEH_WEAPONS];

// g_local.h: extern stringID_table_t animTable[MAX_ANIMATIONS+1];
stringID_table_t animTable[MAX_ANIMATIONS+1];

// g_cmds.cpp: extern stringID_table_t SaberStyleTable[];
// wp_saber.cpp defines it with SS_NUM_SABER_STYLES+1 entries; null-terminate one stub entry.
stringID_table_t SaberStyleTable[SS_NUM_SABER_STYLES + 1];

// wp_saber.h: extern float forceJumpHeight[];
float forceJumpHeight[NUM_FORCE_POWER_LEVELS];
float forceJumpHeightMax[NUM_FORCE_POWER_LEVELS];
float forceJumpStrength[NUM_FORCE_POWER_LEVELS];

// wp_saber.h: extern const char *saberColorStringForColor[];
const char *saberColorStringForColor[SABER_PURPLE + 2]; // null-terminated

// wp_saber.h / AI_Jedi.cpp: extern int parryDebounce[];
int parryDebounce[NUM_FORCE_POWER_LEVELS];

// cg_local.h: extern int showPowers[MAX_SHOWPOWERS];
int showPowers[MAX_SHOWPOWERS];

// Scalars
bool     in_camera         = false;
float    cg_zoomFov        = 0.0f;
qboolean cg_usingInFrontOf = qfalse;
qboolean MatrixMode        = qfalse;
qboolean NPCsPrecached     = qfalse;
int      numVehicles       = 0;
int      statusTextIndex   = 0;

// cgame cvars (vmCvar_t is trivially zero-initialised)
vmCvar_t cg_debugAnim;
vmCvar_t cg_debugAnimTarget;
vmCvar_t cg_debugSaber;
vmCvar_t cg_gunAutoFirst;
vmCvar_t cg_thirdPersonAlpha;
vmCvar_t cg_thirdPersonAngle;
vmCvar_t cg_thirdPersonAutoAlpha;
vmCvar_t cg_thirdPersonPitchOffset;
vmCvar_t cg_thirdPersonRange;
vmCvar_t cg_thirdPersonVertOffset;
vmCvar_t cg_updatedDataPadForcePower1;
vmCvar_t cg_updatedDataPadForcePower2;
vmCvar_t cg_updatedDataPadForcePower3;

// ============================================================
// CGCam — camera control (from cgame/cg_camera.cpp)
// ============================================================
void CGCam_Enable  (void) {}
void CGCam_Disable (void) {}
void CGCam_Move    (vec3_t /*origin*/, float /*duration*/) {}
void CGCam_Pan     (vec3_t /*angles*/, vec3_t /*dir*/, float /*duration*/) {}
void CGCam_Zoom    (float /*fov*/, float /*duration*/) {}
void CGCam_Roll    (float /*angle*/, float /*duration*/) {}
void CGCam_Follow  (const char * /*name*/, float /*speed*/, float /*initLerp*/) {}
void CGCam_Track   (const char * /*name*/, float /*speed*/, float /*initLerp*/) {}
void CGCam_Distance(float /*dist*/, float /*initLerp*/) {}
void CGCam_Fade    (vec3_t /*src*/, vec3_t /*dst*/, float /*duration*/) {}
void CGCam_StartRoff(char * /*name*/) {}
void CGCam_Shake   (float /*intensity*/, int /*duration*/) {}

// ============================================================
// cgi_* — cgame import wrappers (from cgame/cg_syscalls.cpp)
// ============================================================
void cgi_S_StartSound       (const float */*org*/, int /*entityNum*/, int /*entchannel*/, int /*sfx*/) {}
void cgi_S_StopSounds       (void) {}
void cgi_S_UpdateEntityPosition(int /*entityNum*/, const float */*origin*/) {}
void cgi_FS_FCloseFile      (int /*f*/) {}
void cgi_R_GetBModelVerts   (int /*bmodelIndex*/, vec3_t */*verts*/, vec3_t /*normal*/) {}
void cgi_R_GetLighting      (const vec3_t /*point*/, vec3_t /*ambientLight*/, vec3_t /*directedLight*/, vec3_t /*lightDir*/) {}

int  cgi_FS_FOpenFile       (const char */*qpath*/, fileHandle_t */*f*/, fsMode_t /*mode*/) { return 0; }
int  cgi_FS_Read            (void */*buffer*/, int /*len*/, int /*f*/) { return 0; }
int  cgi_R_RegisterShader   (const char */*name*/) { return 0; }
int  cgi_S_RegisterSound    (const char */*sample*/) { return 0; }
int  cgi_SP_GetStringTextString(const char */*reference*/, char */*buffer*/, int /*bufferLength*/) { return 0; }

// ============================================================
// CG_* — cgame functions called from game code
// ============================================================
void CG_Printf              (const char */*fmt*/, ...) {}
void CG_CenterPrint         (const char */*str*/, int /*y*/) {}
void CG_ChangeWeapon        (int /*num*/) {}
void CG_OutOfAmmoChange     (void) {}
void CG_ItemPickup          (int /*itemNum*/, qboolean /*bHaveIt*/) {}
void CG_LandingEffect       (vec3_t /*origin*/, vec3_t /*normal*/, int /*material*/) {}
void CG_ReadTheEvilCGHackStuff  (void) {}
void CG_WriteTheEvilCGHackStuff (void) {}
void CG_RegisterClientModels(int /*clientNum*/) {}
void CG_RegisterItemVisuals (int /*itemNum*/) {}
void CG_RegisterItemSounds  (int /*itemNum*/) {}
void CG_RegisterClientRenderInfo(clientInfo_t */*ci*/, renderInfo_t */*ri*/) {}
void CG_RegisterNPCCustomSounds(clientInfo_t */*ci*/) {}
void CG_SetClientViewAngles (vec3_t /*angles*/, qboolean /*instant*/) {}
void CG_PlayerLockedWeaponSpeech(int /*jumping*/) {}
void CG_DLightThink         (centity_t */*cent*/) {}
void CG_Limb                (centity_t */*cent*/) {}
void CG_MatrixEffect        (centity_t */*cent*/) {}
void CG_CreateMiscEntFromGent(gentity_t */*gent*/, const vec3_t /*origin*/, float /*rotation*/) {}
void CG_Cube                (vec3_t /*mins*/, vec3_t /*maxs*/, vec3_t /*color*/, float /*alpha*/) {}
void CG_TestLine            (vec3_t /*start*/, vec3_t /*end*/, int /*time*/, unsigned /*color*/, int /*radius*/) {}
void CG_DrawEdge            (vec3_t /*start*/, vec3_t /*end*/, int /*type*/) {}
void CG_DrawNode            (vec3_t /*origin*/, int /*type*/) {}
void CG_DrawRadius          (vec3_t /*origin*/, unsigned /*radius*/, int /*type*/) {}
void CG_DrawCombatPoint     (vec3_t /*origin*/, int /*flags*/) {}
void CG_DoGlass             (vec3_t /*verts*/[4], vec3_t /*normal*/, vec3_t /*dmgPt*/, vec3_t /*dmgDir*/, float /*dmgRadius*/) {}
void CG_MiscModelExplosion  (vec3_t /*mins*/, vec3_t /*maxs*/, int /*size*/, material_t /*chunkType*/) {}
void CG_Chunks              (int /*owner*/, vec3_t /*origin*/, const vec3_t /*normal*/, const vec3_t /*mins*/, const vec3_t /*maxs*/, float /*speed*/, int /*numChunks*/, material_t /*chunkType*/, int /*health*/, float /*scale*/, int /*weaponNum*/) {}

qboolean CG_TryPlayCustomSound(vec3_t /*origin*/, int /*entityNum*/, soundChannel_t /*channel*/, const char */*soundSet*/, int /*customSoundIndex*/) { return qfalse; }

// ============================================================
// Force powers — from wp_saber.cpp
// ============================================================
void ForceAbsorb   (gentity_t */*self*/) {}
void ForceGrip     (gentity_t */*self*/) {}
void ForceHeal     (gentity_t */*self*/) {}
void ForceJump     (gentity_t */*self*/, usercmd_t */*ucmd*/) {}
void ForceLightning(gentity_t */*self*/) {}
void ForceProtect  (gentity_t */*self*/) {}
void ForceRage     (gentity_t */*self*/) {}
void ForceSeeing   (gentity_t */*self*/) {}
void ForceSpeed    (gentity_t */*self*/, int /*level*/) {}
void ForceTelepathy(gentity_t */*self*/) {}
void ForceThrow    (gentity_t */*self*/, qboolean /*pull*/, qboolean /*fake*/) {}

qboolean ForceDrain2(gentity_t */*self*/) { return qfalse; }

void WP_ForcePowerStart     (gentity_t */*self*/, forcePowers_t /*fp*/, int /*cost*/) {}
void WP_ForcePowerStop      (gentity_t */*self*/, forcePowers_t /*fp*/) {}
void WP_ForcePowerDrain     (gentity_t */*self*/, forcePowers_t /*fp*/, int /*overrideCost*/) {}
void WP_ForcePowersUpdate   (gentity_t */*self*/, usercmd_t */*ucmd*/) {}
void WP_InitForcePowers     (gentity_t */*self*/) {}
void WP_StopForceHealEffects(gentity_t */*self*/) {}
void WP_ResistForcePush     (gentity_t */*self*/, gentity_t */*pusher*/, qboolean /*fake*/) {}
void WP_DeactivateSaber     (gentity_t */*self*/, qboolean /*clearLength*/) {}
void WP_DropWeapon          (gentity_t */*dropper*/, vec3_t /*velocity*/) {}
void WP_SaberFreeStrings    (saberInfo_t &/*saber*/) {}
void WP_SaberLoadParms      (void) {}
void WP_SaberAddG2SaberModels(gentity_t */*ent*/, int /*saberNum*/) {}
void WP_SaberUpdate         (gentity_t */*self*/, usercmd_t */*ucmd*/) {}
void WP_SabersDamageTrace   (gentity_t */*ent*/, qboolean /*noGhoul2*/) {}
void WP_SaberSetDefaults    (saberInfo_t */*saber*/, qboolean /*keepStyle*/) {}
void WP_SaberStartMissileBlockCheck(gentity_t */*self*/, usercmd_t */*ucmd*/) {}
void WP_SaberSwingSound     (gentity_t */*self*/, int /*saberNum*/, swingType_t /*swingType*/) {}
void WP_SaberFallSound      (gentity_t */*self*/, gentity_t */*saber*/) {}
void WP_SaberUpdateOldBladeData(gentity_t */*ent*/) {}
void WP_SetSaber            (gentity_t */*ent*/, int /*saberNum*/, const char */*saberName*/) {}
void WP_SetSaberOrigin      (gentity_t */*ent*/, vec3_t /*origin*/) {}
void WP_SaberSetColor       (gentity_t */*ent*/, int /*saberNum*/, int /*bladeNum*/, char */*colorName*/) {}
void WP_RemoveSaber         (gentity_t */*ent*/, int /*saberNum*/) {}

qboolean WP_ForcePowerAvailable         (gentity_t */*self*/, forcePowers_t /*fp*/, int /*overrideAmt*/) { return qfalse; }
qboolean WP_ForceThrowable              (gentity_t */*self*/, gentity_t */*ent*/, gentity_t */*forceRet*/, qboolean /*pull*/, float /*dist*/, float /*ndot*/, vec3_t /*dir*/) { return qfalse; }
qboolean WP_SaberBladeUseSecondBladeStyle(saberInfo_t */*saber*/, int /*bladeNum*/) { return qfalse; }
qboolean WP_SaberCanTurnOffSomeBlades  (saberInfo_t */*saber*/) { return qfalse; }
qboolean WP_SaberLose                  (gentity_t */*self*/, vec3_t /*saberDir*/) { return qfalse; }
qboolean WP_SaberParseParms            (const char */*saberName*/, saberInfo_t *saber, qboolean /*setColors*/) { if (saber) memset(saber, 0, sizeof(*saber)); return qfalse; }
qboolean WP_SabersCheckLock2           (gentity_t */*attacker*/, gentity_t */*defender*/, sabersLockMode_t /*lockMode*/) { return qfalse; }
qboolean WP_SaberStyleValidForSaber    (gentity_t */*ent*/, int /*style*/) { return qfalse; }
qboolean WP_UseFirstValidSaberStyle    (gentity_t */*ent*/, int */*saberAnimLevel*/) { return qfalse; }
qboolean WP_BreakSaber                 (gentity_t */*ent*/, const char */*saberName*/, saberType_t /*saberType*/) { return qfalse; }
bool     WP_DoingMoronicForcedAnimationForForcePowers(gentity_t */*self*/) { return false; }

int WP_SaberInitBladeData(gentity_t */*ent*/) { return 0; }

// ============================================================
// Jedi AI — from AI_Jedi.cpp
// ============================================================
void NPC_BSJedi_Default       (void) {}
void NPC_BSJedi_FollowLeader  (void) {}
void NPC_EvasionSaber         (void) {}
void NPC_Jedi_Pain            (gentity_t */*self*/, gentity_t */*inflictor*/, gentity_t */*attacker*/, const vec3_t /*point*/, int /*damage*/, int /*mod*/, int /*hitLoc*/) {}
void NPC_Jedi_RateNewEnemy    (gentity_t */*self*/, gentity_t */*enemy*/) {}
void NPC_CultistDestroyer_Precache(void) {}
void NPC_ShadowTrooper_Precache(void) {}
void NPC_TavionScepter_Precache(void) {}
void NPC_TavionSithSword_Precache(void) {}
void NPC_Rosh_Dark_Precache   (void) {}
void Jedi_Ambush              (gentity_t */*self*/) {}
void Jedi_ClearTimers         (gentity_t */*ent*/) {}
void Jedi_Cloak               (gentity_t */*self*/) {}
void RT_CheckJump             (void) {}

qboolean Jedi_CultistDestroyer(gentity_t */*self*/) { return qfalse; }
qboolean Jedi_DodgeEvasion    (gentity_t */*self*/, gentity_t */*shooter*/, trace_t */*tr*/, int /*hitLoc*/) { return qfalse; }
qboolean Jedi_StopKnockdown   (gentity_t */*self*/, gentity_t */*pusher*/, const vec3_t /*pushDir*/) { return qfalse; }
qboolean Jedi_WaitingAmbush   (gentity_t */*self*/) { return qfalse; }
qboolean Rosh_BeingHealed     (gentity_t */*self*/) { return qfalse; }
qboolean Rosh_TwinPresent     (gentity_t */*self*/) { return qfalse; }
gentity_t *Jedi_FindEnemyInCone(gentity_t */*self*/, gentity_t */*fallback*/, float /*minDot*/) { return NULL; }

int Jedi_ReCalcParryTime(gentity_t */*self*/, evasionType_t /*evasionType*/) { return 0; }

// ============================================================
// Vehicle functions — from g_vehicles.cpp / FighterNPC.cpp etc.
// ============================================================
Vehicle_t *G_IsRidingVehicle  (gentity_t */*pEnt*/) { return NULL; }
void G_VehicleSpawn           (gentity_t */*ent*/) {}
void G_CreateFighterNPC       (Vehicle_t **/*pVeh*/, const char */*strType*/) {}
void G_CreateSpeederNPC       (Vehicle_t **/*pVeh*/, const char */*strType*/) {}
void G_CreateWalkerNPC        (Vehicle_t **/*pVeh*/, const char */*strType*/) {}
void G_DrivableATSTDie        (gentity_t */*self*/) {}
void G_DriveATST              (gentity_t */*self*/, gentity_t */*atst*/) {}
void Vehicle_SetAnim          (gentity_t */*ent*/, int /*setflags*/, int /*anim*/, int /*priority*/, int /*flags*/) {}
void G_CreateG2AttachedWeaponModel(gentity_t */*ent*/, const char */*modelName*/, int /*boltNum*/, int /*weaponNum*/) {}
bool VEH_StartStrafeRam       (Vehicle_t */*pVeh*/, bool /*alt_fire*/) { return false; }

int  BG_VehicleGetIndex       (const char */*name*/) { return 0; }

// ============================================================
// Game utility stubs
// ============================================================
void G_Throw                  (gentity_t */*ent*/, const vec3_t /*newDir*/, float /*speed*/) {}
void G_DrainPowerForSpecialMove(gentity_t */*self*/, forcePowers_t /*fp*/, int /*cost*/, qboolean /*isMeditation*/) {}

qboolean G_CheckEnemyPresence     (gentity_t */*ent*/, int /*dir*/, float /*range*/, float /*tolerance*/) { return qfalse; }
qboolean G_CheckIncrementLockAnim (int /*anim*/, int /*value*/) { return qfalse; }
qboolean G_EnoughPowerForSpecialMove(int /*power*/, int /*cost*/, qboolean /*drainIt*/) { return qfalse; }
qboolean G_InCinematicSaberAnim   (gentity_t */*ent*/) { return qfalse; }
qboolean G_TryingCartwheel        (gentity_t */*self*/, usercmd_t */*ucmd*/) { return qfalse; }
qboolean G_TryingJumpForwardAttack(gentity_t */*self*/, usercmd_t */*ucmd*/) { return qfalse; }
qboolean G_TryingKataAttack       (gentity_t */*self*/, usercmd_t */*ucmd*/) { return qfalse; }
qboolean G_TryingLungeAttack      (gentity_t */*self*/, usercmd_t */*ucmd*/) { return qfalse; }
qboolean G_TryingPullAttack       (gentity_t */*self*/, usercmd_t */*ucmd*/, qboolean /*checkForward*/) { return qfalse; }
qboolean G_TryingSpecial          (gentity_t */*self*/, usercmd_t */*ucmd*/) { return qfalse; }

int G_CostForSpecialMove(int /*power*/, qboolean /*forceDrained*/) { return 0; }

// ============================================================
// FX projectile think functions — from cgame/cg_weaponevents.cpp
// ============================================================
void FX_BlasterProjectileThink     (centity_t */*cent*/, const weaponInfo_s */*weapon*/) {}
void FX_BlasterAltFireThink        (centity_t */*cent*/, const weaponInfo_s */*weapon*/) {}
void FX_BryarProjectileThink       (centity_t */*cent*/, const weaponInfo_s */*weapon*/) {}
void FX_BryarAltProjectileThink    (centity_t */*cent*/, const weaponInfo_s */*weapon*/) {}
void FX_BowcasterProjectileThink   (centity_t */*cent*/, const weaponInfo_s */*weapon*/) {}
void FX_RepeaterProjectileThink    (centity_t */*cent*/, const weaponInfo_s */*weapon*/) {}
void FX_RepeaterAltProjectileThink (centity_t */*cent*/, const weaponInfo_s */*weapon*/) {}
void FX_DEMP2_ProjectileThink      (centity_t */*cent*/, const weaponInfo_s */*weapon*/) {}
void FX_DEMP2_AltProjectileThink   (centity_t */*cent*/, const weaponInfo_s */*weapon*/) {}
void FX_FlechetteProjectileThink   (centity_t */*cent*/, const weaponInfo_s */*weapon*/) {}
void FX_FlechetteAltProjectileThink(centity_t */*cent*/, const weaponInfo_s */*weapon*/) {}
void FX_RocketProjectileThink      (centity_t */*cent*/, const weaponInfo_s */*weapon*/) {}
void FX_RocketAltProjectileThink   (centity_t */*cent*/, const weaponInfo_s */*weapon*/) {}
void FX_ConcProjectileThink        (centity_t */*cent*/, const weaponInfo_s */*weapon*/) {}
void FX_EmplacedProjectileThink    (centity_t */*cent*/, const weaponInfo_s */*weapon*/) {}
void FX_ATSTMainProjectileThink    (centity_t */*cent*/, const weaponInfo_s */*weapon*/) {}
void FX_ATSTSideMainProjectileThink(centity_t */*cent*/, const weaponInfo_s */*weapon*/) {}
void FX_ATSTSideAltProjectileThink (centity_t */*cent*/, const weaponInfo_s */*weapon*/) {}
void FX_TurretProjectileThink      (centity_t */*cent*/, const weaponInfo_s */*weapon*/) {}
void FX_TuskenShotProjectileThink  (centity_t */*cent*/, const weaponInfo_s */*weapon*/) {}
void FX_NoghriShotProjectileThink  (centity_t */*cent*/, const weaponInfo_s */*weapon*/) {}
void FX_Read                       (void) {}
void FX_Write                      (void) {}

// ============================================================
// Misc stubs
// ============================================================
qboolean ValidAnimFileIndex(int /*index*/) { return qfalse; }
int      CAS_GetBModelSound(const char */*name*/, int /*index*/) { return 0; }

// saberInfoRetail_t::sg_export(saberInfo_t&) is implemented in g_savegame.cpp.
