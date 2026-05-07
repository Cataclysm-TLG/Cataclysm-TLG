#pragma once
#ifndef CATA_SRC_CRAFTING_H
#define CATA_SRC_CRAFTING_H

#include <list>
#include <optional>
#include <vector>

class Character;
class item;
class recipe;
struct attention_plan;
template <typename E> struct enum_traits;

enum class craft_flags : int {
    none = 0,
    start_only = 1, // Only require 5% (plus remainder) of tool charges
};

template<>
struct enum_traits<craft_flags> {
    static constexpr bool is_flag_enum = true;
};

// removes any (removable) ammo from the item and stores it in the
// players inventory.
void remove_ammo( item &dis_item, Character &p );
// same as above but for each item in the list
void remove_ammo( std::list<item> &dis_items, Character &p );

void drop_or_handle( const item &newit, Character &p );

// Asks the avatar what to do at each attention step.  Returns nullopt if the
// avatar cancelled.  `existing` pre-fills choices on resume.
std::optional<std::vector<attention_plan>> show_craft_planning_modal(
        const recipe &rec, const Character &crafter, int batch,
        const std::vector<attention_plan> &existing );

#endif // CATA_SRC_CRAFTING_H
