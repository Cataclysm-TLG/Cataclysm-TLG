#pragma once
#ifndef CATA_SRC_CAUTION_H
#define CATA_SRC_CAUTION_H

class Character;

// Caution mode: optional warnings and confirmations that interrupt the player
// when they keep acting while impaired.  See the "Caution Mode Options" group
// in the options menu; everything is toggleable there.
namespace caution
{

// Run the per-turn checks (speed, stamina, bleeding, bites, surrounded, pain,
// critical limbs, prolonged danger).  Call once per game turn for the avatar.
void check_turn( const Character &you );

// Ask for confirmation before a melee attack while impaired.  Returns false
// if the player backs out of the attack.
bool confirm_attack( const Character &you );

// Warn after the avatar takes damage while impaired.
void on_avatar_hurt( const Character &you );

} // namespace caution

#endif // CATA_SRC_CAUTION_H
