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

#include "AoeLoot.h"

#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldSession.h"

class AoeLootWorldScript : public WorldScript
{
public:
    AoeLootWorldScript() : WorldScript("AoeLootWorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        sAoeLootConfig().Load();
    }
};

// The loot packets of the client: those of a merged window are answered here.
class AoeLootServerScript : public ServerScript
{
public:
    AoeLootServerScript() : ServerScript("AoeLootServerScript", { SERVERHOOK_CAN_PACKET_RECEIVE, SERVERHOOK_CAN_PACKET_SEND }) { }

    bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
    {
        uint16 opcode = packet.GetOpcode();
        if (opcode != CMSG_LOOT && opcode != CMSG_AUTOSTORE_LOOT_ITEM && opcode != CMSG_LOOT_MONEY
            && opcode != CMSG_LOOT_RELEASE && opcode != CMSG_LOOT_MASTER_GIVE)
            return true;

        Player* player = session ? session->GetPlayer() : nullptr;
        if (!player || !player->IsInWorld())
            return true;

        WorldPacket data(packet);
        data.rpos(0);

        switch (opcode)
        {
            case CMSG_LOOT:
            {
                ObjectGuid guid;
                data >> guid;
                return !AoeLoot::HandleLoot(player, guid);
            }
            case CMSG_AUTOSTORE_LOOT_ITEM:
            {
                uint8 slot = data.read<uint8>();
                return !AoeLoot::HandleTakeItem(player, slot);
            }
            case CMSG_LOOT_MONEY:
                return !AoeLoot::HandleTakeMoney(player);
            case CMSG_LOOT_MASTER_GIVE:
            {
                ObjectGuid lootGuid, targetGuid;
                uint8 slot;
                data >> lootGuid >> slot >> targetGuid;
                return !AoeLoot::HandleMasterGive(player, lootGuid, slot, targetGuid);
            }
            case CMSG_LOOT_RELEASE:
            {
                // the other corpses close here, the clicked one in the core
                ObjectGuid guid;
                data >> guid;
                AoeLoot::HandleRelease(player, guid);
                return true;
            }
            default:
                return true;
        }
    }

    bool CanPacketSend(WorldSession* session, WorldPacket const& packet) override
    {
        if (!session)
            return true;

        return AoeLoot::FilterOutgoing(session, packet);
    }
};

class AoeLootPlayerScript : public PlayerScript
{
public:
    AoeLootPlayerScript() : PlayerScript("AoeLootPlayerScript", { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_UPDATE }) { }

    // The state is made at login, so that the threads which send packets
    // to the player only ever read it.
    void OnPlayerLogin(Player* player) override
    {
        AoeLoot::GetState(player);
    }

    void OnPlayerLogout(Player* player) override
    {
        AoeLoot::Release(player);
    }

    // A window the core closed by itself -- another loot opened, the player
    // died or left -- closes its other corpses too.
    void OnPlayerUpdate(Player* player, uint32 /*diff*/) override
    {
        AoeLoot::ReleaseIfClosed(player);
    }
};

void AddSC_aoe_loot()
{
    new AoeLootWorldScript();
    new AoeLootServerScript();
    new AoeLootPlayerScript();
}
