/*
 * This file is part of mod-aoe-loot.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef MOD_AOE_LOOT_H
#define MOD_AOE_LOOT_H

#include "DataMap.h"
#include "LootMgr.h"
#include "ObjectGuid.h"
#include "WorldPacket.h"

#include <optional>
#include <vector>

class Creature;
class Player;
class WorldSession;

/*
 * Area loot, as Retail has it since Mists of Pandaria.
 *
 * Clicking a corpse opens one window that also holds the loot of the other
 * corpses around it the player may loot. Nothing moves between corpses: each
 * row of the window points at the corpse it comes from and the slot it has
 * there, and taking a row takes it from that corpse. What is left stays where
 * it dropped.
 *
 * The clicked corpse is opened by the core itself (Player::SendLoot), so its
 * rights, its rolls and its round robin are the core's. Its answer is held
 * back, the rows of the other corpses are added to it, and the merged window
 * is sent in its place. Every row is numbered again from 0, so the client
 * never sees a slot number above what one corpse could have.
 */

struct AoeLootConfig
{
    bool  enabled = true;
    float range = 30.0f;
    uint32 maxCorpses = 15;
    uint32 maxItems = 16;
    bool  lineOfSight = true;

    void Load();
};

AoeLootConfig& sAoeLootConfig();

// One row of the merged window: where it comes from.
struct AoeLootRow
{
    ObjectGuid source;
    uint8 sourceSlot = 0;
    uint32 itemId = 0;
    bool taken = false;
};

// What a player has open, kept on the player (Player::CustomData).
struct AoeLootState : public DataMap::Base
{
    // the corpse the player clicked, whose loot the core opened
    ObjectGuid primary;
    // the other corpses the window draws from
    std::vector<ObjectGuid> secondaries;
    // the corpses whose money the window shows, the clicked one included
    std::vector<ObjectGuid> moneySources;
    // the rows, by slot of the merged window
    std::vector<AoeLootRow> rows;
    bool active = false;

    // set while the core opens the clicked corpse: its answer is held back
    bool capturing = false;
    std::optional<WorldPacket> captured;

    // set while a row is taken from its corpse: the core's notices carry
    // the corpse's own slot numbers, which mean nothing in the merged window
    bool muteRemovals = false;

    void Reset()
    {
        primary.Clear();
        secondaries.clear();
        moneySources.clear();
        rows.clear();
        active = false;
        capturing = false;
        captured.reset();
        muteRemovals = false;
    }
};

namespace AoeLoot
{
    AoeLootState* GetState(Player* player);

    // CMSG_LOOT. True when the module answered it, false to let the core do.
    bool HandleLoot(Player* player, ObjectGuid guid);
    // CMSG_AUTOSTORE_LOOT_ITEM
    bool HandleTakeItem(Player* player, uint8 slot);
    // CMSG_LOOT_MONEY
    bool HandleTakeMoney(Player* player);
    // CMSG_LOOT_MASTER_GIVE
    bool HandleMasterGive(Player* player, ObjectGuid lootGuid, uint8 slot, ObjectGuid targetGuid);
    // CMSG_LOOT_RELEASE: closes the other corpses; the core closes the clicked one
    void HandleRelease(Player* player, ObjectGuid guid);

    // Closes the other corpses of a window the player no longer has open.
    void ReleaseIfClosed(Player* player);
    void Release(Player* player);

    // Packets the server sends: true to let them go.
    bool FilterOutgoing(WorldSession* session, WorldPacket const& packet);
}

#endif
