#pragma once
#ifndef CATA_SRC_MONDEATH_H
#define CATA_SRC_MONDEATH_H

#include "item.h"

class map;
class monster;

namespace mdeath
{
// Drop a body
item_location normal( map *here, monster &z );
// Overkill splatter (also part of normal under conditions)
item_location splatter( map *here, monster &z );

void scatter_chunks( map *here, const itype_id &chunk_name, int chunk_amt,
                     const mtype &z, const tripoint_bub_ms &pos,
                     int distance, int pile_size = 1 );

// Hallucination disappears
void disappear( monster &z );
// Broken robot drop
void broken( map *here, monster &z );
} //namespace mdeath

item_location make_mon_corpse( map *here, monster &z, int damageLvl );

#endif // CATA_SRC_MONDEATH_H
