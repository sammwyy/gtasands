#pragma once

#include <nds/ndstypes.h>

#include "game/text/gxt.h"

// The slice of CPlayerInfo / CPlayerPedData / CWanted / CClock / CWeapon the
// game actually runs today. A new game (CPlayerInfo::Clear + CStats::Init +
// CClock::Initialise): Grove Street, $0, 100 health, no armour, no wanted,
// fists. Nothing here drifts on its own. Stars, cash, fat and weapons move
// only when something is earned or spent. eWeaponType ids match the original.
namespace player {

enum WeaponId : int {
	WEAPON_UNARMED = 0,
	WEAPON_BRASSKNUCKLE = 1,
	WEAPON_GOLFCLUB = 2,
	WEAPON_NIGHTSTICK = 3,
	WEAPON_KNIFE = 4,
	WEAPON_BASEBALLBAT = 5,
	WEAPON_SHOVEL = 6,
	WEAPON_POOLCUE = 7,
	WEAPON_KATANA = 8,
	WEAPON_CHAINSAW = 9,
	WEAPON_DILDO1 = 10,
	WEAPON_DILDO2 = 11,
	WEAPON_VIBE1 = 12,
	WEAPON_VIBE2 = 13,
	WEAPON_FLOWERS = 14,
	WEAPON_CANE = 15,
	WEAPON_GRENADE = 16,
	WEAPON_TEARGAS = 17,
	WEAPON_MOLOTOV = 18,
	WEAPON_PISTOL = 22,
	WEAPON_PISTOL_SILENCED = 23,
	WEAPON_DESERT_EAGLE = 24,
	WEAPON_SHOTGUN = 25,
	WEAPON_SAWNOFF = 26,
	WEAPON_SPAS12 = 27,
	WEAPON_MICRO_UZI = 28,
	WEAPON_MP5 = 29,
	WEAPON_AK47 = 30,
	WEAPON_M4 = 31,
	WEAPON_TEC9 = 32,
	WEAPON_COUNTRYRIFLE = 33,
	WEAPON_SNIPERRIFLE = 34,
	WEAPON_RLAUNCHER = 35,
	WEAPON_RLAUNCHER_HS = 36,
	WEAPON_FLAMETHROWER = 37,
	WEAPON_MINIGUN = 38,
	WEAPON_SATCHEL = 39,
	WEAPON_DETONATOR = 40,
	WEAPON_SPRAYCAN = 41,
	WEAPON_EXTINGUISHER = 42,
	WEAPON_CAMERA = 43,
	WEAPON_PARACHUTE = 46,
};

// What the HUD draws for the weapon in hand. Fists: txd and icon are null.
struct Weapon {
	const char* txd;
	const char* icon;
	int clip, total;
};

struct State {
	float x, y, z;
	float heading;        // radians, 0 = north, clockwise
	float speed;          // world units/sec -> radar zoom
	float health;         // 0..maxHealth
	float maxHealth;
	float armour;         // 0..maxArmour
	float maxArmour;
	float breath;         // 0..maxBreath; hidden on the HUD while full
	float maxBreath;
	int money;
	int wantedLevel;      // 0..6, CWanted. Stays 0 until a crime is reported.
	Weapon weapon;
	int weaponType;       // WeaponId of the equipped slot
	const GxtChar* zoneName;
	const GxtChar* vehicleName; // nullptr until a vehicle is entered
	float fat;            // STAT_FAT, 0..1000
	float muscle;         // STAT_MUSCLE, 0..1000
	float sprint;         // sprint energy left, 0..100 (m_fTimeCanRun / max)
	bool aiming;          // R held: free-aim / fight stance
	bool inVehicle;       // no vehicle system yet; Y is the enter/exit edge
	bool vehicleRequest;  // true on the frame Y was pressed
};

extern State g;

// CClock. 1 game minute = 1 real second (CClock::Initialise(1000)).
// Month is 1..12, day is 1..daysInMonth, weekday is 1..7 (a new game is 4).
extern unsigned g_clockH, g_clockM, g_clockDay, g_clockMonth;
extern int g_weekday;
unsigned clockSeconds();    // CClock::ms_nGameClockSeconds, 0..59
float minutesToday();       // CClock::GetMinutesToday (fractional, 0..1440)
void setClock(unsigned h, unsigned m);   // CClock::SetGameClock (seconds reset)

void init();
void update();            // once per frame while INGAME and the menu is down
float camYaw();           // chase camera heading, radians (0 = north, clockwise)
float camPitch();         // radians above the horizon; positive looks down
// Render / camera reference height: CJ's feet z (g.z), smoothed so stairs and
// jumps don't jerk the camera. The GL origin of the frame sits here.
float viewZ();
float camDist();          // chase distance after the camera collision (<= dbg::opts.camDist)
constexpr float CAM_LOOK_H = 0.85f;   // chase camera target above viewZ() on foot
float camLookH();         // the current one (higher behind a car)

// Vehicles (game/vehicle): CTaskComplexEnterCarAsDriver / CTaskSimpleCarDrive /
// CTaskComplexLeaveCar, reduced. Y near a car walks CJ to the driver door and
// plays CAR_open / getin / closedoor; Y while driving brakes, then getout /
// close. Driving: d-pad left/right steer, up or A accelerate, down or B
// brake / reverse, R handbrake, L horn.
int vehicleId();          // car CJ is entering / driving / leaving, -1 = on foot
bool driving();           // sitting in it (CTaskSimpleCarDrive)
bool hiddenInVehicle();   // RC toy driven from outside: CJ is not drawn
void enterVehicleNow(int id);   // debug: straight into the driver seat
void leaveVehicleNow();         // the car is gone: CJ stands at its door
// CJ's entity matrix while attached to a car (player-local GL, origin = the
// renderer's GL origin). False when on foot.
bool attachedMatrix(float ox, float oy, float oz, s32 out[12]);
void vehicleExploded(int id);   // inside a car that blew up
void explosionDamage(float x, float y, float z, float damage);
void hitByVehicle(float vx, float vy, float damage);   // m/s of the car
// Puts CJ on the floor under him (collision probe around g.z, else from
// 60 m above). The loading screen calls it once the collision is in.
void placeOnGround();
#ifdef GTASADS_DEBUG
void debugSetView(float yaw, float pitch);   // headless tests: face + camera heading
#endif

// CPed::GiveWeapon. Same type in a slot adds ammo; a new type replaces it
// and is selected. Melee has no ammo count.
void giveWeapon(int type, int ammo);

// Debug quick actions (hud/debugui GAME tab).
void clearWeapons();               // back to fists only
void giveAllWeapons();             // one weapon per slot, generous ammo
int weaponTypeAt(int index);       // weapon.dat table walk (-1 past the end)
void setBodyStats(float fat, float muscle);  // STAT_FAT / STAT_MUSCLE (0..1000)
void refillSprint();
void respawn();                    // back to the Grove Street spawn
// One-line locomotion state for the debug UI (blend, move state, phase, walk key).
void locoDebug(char* buf, int n);

}
