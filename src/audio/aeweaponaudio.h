#pragma once

// CAEWeaponAudioEntity (Audio/Entities/AEWeaponAudioEntity.cpp) for the
// player's guns: WeaponFire -> PlayGunSounds (dry click, low thump, the
// stereo shot pair front-end + positional, the stereo tail), WeaponReload.
namespace aeweapon {

void prepare(int weaponType);   // equip: queue the weapon's sounds (EnsureSoundBankIsLoaded)
void fire(int weaponType, float x, float y, float z);
void reload(int weaponType, bool second, float x, float y, float z);   // AE_WEAPON_RELOAD_A / _B

}
