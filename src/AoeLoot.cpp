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

#include "CellImpl.h"
#include "Config.h"
#include "Creature.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "WorldSession.h"

#include <algorithm>
#include <unordered_map>

namespace
{
    constexpr char const* STATE_KEY = "mod-aoe-loot";

    // How far past the search range a corpse may be when a row is taken: the
    // player may have stepped while the window was open.
    constexpr float TAKE_MARGIN = 5.0f;

    // One row as the core writes it in SMSG_LOOT_RESPONSE.
    struct WireRow
    {
        uint8 slot = 0;
        uint32 itemId = 0;
        uint32 count = 0;
        uint32 displayId = 0;
        uint32 randomSuffix = 0;
        int32 randomPropertyId = 0;
        uint8 slotType = 0;
    };

    struct WireLoot
    {
        uint32 gold = 0;
        std::vector<WireRow> rows;
    };

    // Reads what follows the loot type in SMSG_LOOT_RESPONSE: what
    // operator<<(ByteBuffer&, LootView const&) wrote.
    WireLoot ReadLootView(ByteBuffer& buffer)
    {
        WireLoot loot;
        loot.gold = buffer.read<uint32>();
        uint8 count = buffer.read<uint8>();
        loot.rows.reserve(count);
        for (uint8 i = 0; i < count; ++i)
        {
            WireRow row;
            row.slot = buffer.read<uint8>();
            row.itemId = buffer.read<uint32>();
            row.count = buffer.read<uint32>();
            row.displayId = buffer.read<uint32>();
            row.randomSuffix = buffer.read<uint32>();
            row.randomPropertyId = buffer.read<int32>();
            row.slotType = buffer.read<uint8>();
            loot.rows.push_back(row);
        }
        return loot;
    }

    // The rights a player has on a creature's loot: what Player::SendLoot
    // works out for a corpse.
    PermissionTypes PermissionFor(Player* player, Creature* creature)
    {
        if (Group* recipientGroup = creature->GetLootRecipientGroup())
        {
            if (player->GetGroup() != recipientGroup)
                return NONE_PERMISSION;

            switch (recipientGroup->GetLootMethod())
            {
                case MASTER_LOOT:
                    return recipientGroup->GetMasterLooterGuid() == player->GetGUID() ? MASTER_PERMISSION : RESTRICTED_PERMISSION;
                case FREE_FOR_ALL:
                    return ALL_PERMISSION;
                case ROUND_ROBIN:
                    return ROUND_ROBIN_PERMISSION;
                default:
                    return GROUP_PERMISSION;
            }
        }

        return creature->GetLootRecipientGUID() == player->GetGUID() ? OWNER_PERMISSION : NONE_PERMISSION;
    }

    // A corpse the window may draw from: dead, still lootable, a corpse loot
    // (not skinning, not pickpocketing), and the player's to loot -- killed by
    // the player, or by the player's group or raid.
    bool IsLootableCorpse(Player* player, Creature* creature)
    {
        if (creature->IsAlive() || !creature->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE))
            return false;

        LootType lootType = creature->loot.loot_type;
        if (lootType != LOOT_NONE && lootType != LOOT_CORPSE)
            return false;

        if (creature->loot.isLooted())
            return false;

        if (PermissionFor(player, creature) == NONE_PERMISSION)
            return false;

        return sScriptMgr->OnAllowedToLootContainerCheck(player, creature->GetGUID());
    }

    class NearbyCorpseCheck
    {
    public:
        NearbyCorpseCheck(Player* player, Creature* clicked, float range)
            : _player(player), _clicked(clicked), _range(range) { }

        bool operator()(Creature* creature) const
        {
            if (creature == _clicked || !_player->IsWithinDistInMap(creature, _range))
                return false;

            if (!IsLootableCorpse(_player, creature))
                return false;

            return !sAoeLootConfig().lineOfSight || _player->IsWithinLOSInMap(creature);
        }

    private:
        Player* _player;
        Creature* _clicked;
        float _range;
    };

    std::vector<Creature*> FindOtherCorpses(Player* player, Creature* clicked)
    {
        AoeLootConfig const& config = sAoeLootConfig();

        std::list<Creature*> found;
        NearbyCorpseCheck check(player, clicked, config.range);
        Acore::CreatureListSearcher<NearbyCorpseCheck> searcher(player, found, check);
        Cell::VisitObjects(player, searcher, config.range);

        std::vector<Creature*> corpses(found.begin(), found.end());
        std::sort(corpses.begin(), corpses.end(), [player](Creature* a, Creature* b)
        {
            return player->GetExactDist2dSq(a) < player->GetExactDist2dSq(b);
        });

        // the clicked corpse counts in the limit
        if (config.maxCorpses > 0 && corpses.size() > config.maxCorpses - 1)
            corpses.resize(config.maxCorpses - 1);

        return corpses;
    }

    // What Player::SendLoot does to a creature's loot the first time anyone
    // opens it: the group's rolls start, or the master looter gets the list.
    // Done once per corpse, whoever opens it first.
    void OpenCorpse(Player* player, Creature* creature)
    {
        Loot& loot = creature->loot;

        if (loot.loot_type == LOOT_NONE)
        {
            if (Group* recipientGroup = creature->GetLootRecipientGroup())
            {
                switch (recipientGroup->GetLootMethod())
                {
                    case GROUP_LOOT:
                        recipientGroup->GroupLoot(&loot, creature);
                        break;
                    case NEED_BEFORE_GREED:
                        recipientGroup->NeedBeforeGreed(&loot, creature);
                        break;
                    case MASTER_LOOT:
                        recipientGroup->MasterLoot(&loot, creature);
                        break;
                    default:
                        break;
                }
            }
        }

        loot.loot_type = LOOT_CORPSE;

        sScriptMgr->OnPlayerBeforeSendLoot(player, creature->GetGUID(), &loot);
    }

    // How many more of an item the player's quests ask for, or -1 when no
    // quest of the log asks for it (no limit then).
    int64 QuestNeed(Player* player, uint32 itemId)
    {
        bool asked = false;
        int64 total = 0;

        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 questId = player->GetQuestSlotQuestId(slot);
            if (!questId || player->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
                continue;

            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;

            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            {
                if (quest->RequiredItemId[i] == itemId)
                {
                    asked = true;
                    total += quest->RequiredItemCount[i];
                }
            }
        }

        if (!asked)
            return -1;

        return std::max<int64>(0, total - int64(player->GetItemCount(itemId, true)));
    }

    // Keeps the merged window from offering more of a quest item than the
    // player's quests still ask for: each corpse, alone, only knows what the
    // bags hold, not what the other corpses of the window offer.
    class QuestLimit
    {
    public:
        explicit QuestLimit(Player* player) : _player(player) { }

        bool Allows(WireRow const& row)
        {
            // rows the player cannot take hold nothing back
            if (row.slotType != LOOT_SLOT_TYPE_ALLOW_LOOT && row.slotType != LOOT_SLOT_TYPE_OWNER)
                return true;

            auto itr = _need.find(row.itemId);
            if (itr == _need.end())
                itr = _need.emplace(row.itemId, QuestNeed(_player, row.itemId)).first;

            int64& need = itr->second;
            if (need < 0)
                return true;

            if (need == 0)
                return false;

            need -= std::min<int64>(need, row.count);
            return true;
        }

    private:
        Player* _player;
        std::unordered_map<uint32, int64> _need;
    };

    // The creature a row comes from, while its loot may still be taken.
    Creature* SourceCorpse(Player* player, AoeLootState const& state, ObjectGuid guid)
    {
        if (!player->IsInWorld())
            return nullptr;

        Creature* creature = player->GetMap()->GetCreature(guid);
        if (!creature || creature->IsAlive())
            return nullptr;

        float reach = guid == state.primary ? INTERACTION_DISTANCE : sAoeLootConfig().range + TAKE_MARGIN;
        if (!creature->IsWithinDistInMap(player, reach))
            return nullptr;

        return creature;
    }

    // Whether a corpse's share of a row has left it for this player.
    bool IsPartGone(Player* player, Loot& loot, AoeLootPart const& part, uint32 itemId)
    {
        QuestItem* questItem = nullptr;
        QuestItem* ffaItem = nullptr;
        QuestItem* conditionalItem = nullptr;
        LootItem* item = loot.LootItemInSlot(part.sourceSlot, player, &questItem, &ffaItem, &conditionalItem);
        if (!item || item->is_looted || item->itemid != itemId)
            return true;

        return (questItem && questItem->is_looted) || (ffaItem && ffaItem->is_looted) || (conditionalItem && conditionalItem->is_looted);
    }

    // Rows that may share a line: the same item, with the same random
    // property, both free for the player to take. Rolls, master loot and
    // locked rows keep one line per corpse, as each is decided on its own.
    bool CanStack(WireRow const& a, WireRow const& b)
    {
        if (a.itemId != b.itemId || a.randomSuffix != b.randomSuffix || a.randomPropertyId != b.randomPropertyId)
            return false;

        if (a.slotType != b.slotType || (a.slotType != LOOT_SLOT_TYPE_ALLOW_LOOT && a.slotType != LOOT_SLOT_TYPE_OWNER))
            return false;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(a.itemId);
        return proto && a.count + b.count <= proto->GetMaxStackSize();
    }

    // Draws the window again from every corpse it covers, as each would show
    // itself to the player now, and sends it.
    void SendWindow(Player* player, AoeLootState& state)
    {
        state.rows.clear();
        state.moneySources.clear();

        std::vector<WireRow> shown;
        QuestLimit questLimit(player);
        uint32 gold = 0;
        uint32 maxItems = sAoeLootConfig().maxItems;

        std::vector<ObjectGuid> sources;
        sources.push_back(state.primary);
        sources.insert(sources.end(), state.secondaries.begin(), state.secondaries.end());

        for (ObjectGuid const& guid : sources)
        {
            Creature* creature = player->GetMap()->GetCreature(guid);
            if (!creature || creature->IsAlive())
                continue;

            PermissionTypes permission = PermissionFor(player, creature);
            if (permission == NONE_PERMISSION)
                continue;

            ByteBuffer view;
            view << LootView(creature->loot, player, permission);
            WireLoot corpseLoot = ReadLootView(view);

            if (corpseLoot.gold)
            {
                gold += corpseLoot.gold;
                state.moneySources.push_back(guid);
            }

            for (WireRow const& row : corpseLoot.rows)
            {
                if (!questLimit.Allows(row))
                    continue;

                AoeLootPart part{ guid, row.slot, false };

                auto stack = std::find_if(shown.begin(), shown.end(), [&row](WireRow const& other) { return CanStack(other, row); });
                if (stack != shown.end())
                {
                    stack->count += row.count;
                    state.rows[stack - shown.begin()].parts.push_back(part);
                    continue;
                }

                // a full window: the row waits on its corpse
                if (shown.size() >= maxItems)
                    continue;

                shown.push_back(row);
                state.rows.push_back({ row.itemId, { part }, false });
            }
        }

        WorldPacket data(SMSG_LOOT_RESPONSE, 8 + 1 + 4 + 1 + shown.size() * 22);
        data << state.primary;
        data << uint8(state.lootType);
        data << uint32(gold);
        data << uint8(shown.size());
        for (std::size_t i = 0; i < shown.size(); ++i)
        {
            WireRow const& row = shown[i];
            data << uint8(i);
            data << uint32(row.itemId);
            data << uint32(row.count);
            data << uint32(row.displayId);
            data << uint32(row.randomSuffix);
            data << int32(row.randomPropertyId);
            data << uint8(row.slotType);
        }

        player->SendDirectMessage(&data);
    }

    // What WorldSession::DoLootRelease does to a creature's loot, without its
    // distance check: these corpses were never the one the player stood at.
    void CloseCorpse(Player* player, Creature* creature)
    {
        Loot& loot = creature->loot;

        if (loot.isLooted())
        {
            creature->AllLootRemovedFromCorpse();
            creature->RemoveDynamicFlag(UNIT_DYNFLAG_LOOTABLE);
            loot.clear();
            return;
        }

        // if the round robin player releases, everyone may loot
        if (player->GetGUID() == loot.roundRobinPlayer)
        {
            loot.roundRobinPlayer.Clear();

            if (Group* group = player->GetGroup())
                group->SendLooter(creature, nullptr);
        }

        creature->ForceValuesUpdateAtIndex(UNIT_DYNAMIC_FLAGS);
    }

    bool IsOpen(Player* player, AoeLootState const* state)
    {
        return state && state->active && player->GetLootGUID() == state->primary;
    }
}

void AoeLootConfig::Load()
{
    enabled = sConfigMgr->GetOption<bool>("AoeLoot.Enable", true);
    range = std::clamp(sConfigMgr->GetOption<float>("AoeLoot.Range", 30.0f), INTERACTION_DISTANCE, 100.0f);
    maxCorpses = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("AoeLoot.MaxCorpses", 15), 1, 50);
    maxItems = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("AoeLoot.MaxItems", 16), 1, 255);
    lineOfSight = sConfigMgr->GetOption<bool>("AoeLoot.LineOfSight", true);
}

AoeLootConfig& sAoeLootConfig()
{
    static AoeLootConfig config;
    return config;
}

AoeLootState* AoeLoot::GetState(Player* player)
{
    return player->CustomData.GetDefault<AoeLootState>(STATE_KEY);
}

bool AoeLoot::HandleLoot(Player* player, ObjectGuid guid)
{
    AoeLootConfig const& config = sAoeLootConfig();
    if (!config.enabled)
        return false;

    AoeLootState* state = GetState(player);

    // a window still open closes before the next one opens; the core
    // closes the clicked corpse of the old one in Player::SendLoot
    if (state->active)
        Release(player);

    // what WorldSession::HandleLootOpcode checks: the core answers these
    if (!player->IsAlive() || !guid.IsCreatureOrVehicle() || player->HasPlayerFlag(PLAYER_FLAGS_NO_PLAY_TIME))
        return false;

    Creature* clicked = player->GetMap()->GetCreature(guid);
    if (!clicked || !clicked->IsWithinDistInMap(player, INTERACTION_DISTANCE) || !IsLootableCorpse(player, clicked))
        return false;

    std::vector<Creature*> others = FindOtherCorpses(player, clicked);
    if (others.empty())
        return false;

    // from here the module opens the clicked corpse in the core's place
    if (player->IsNonMeleeSpellCast(false))
        player->InterruptNonMeleeSpells(false);

    state->Reset();
    state->capturing = true;
    player->SendLoot(guid, LOOT_CORPSE);
    state->capturing = false;

    // nothing went out (a session without socket): the core's work is done
    if (!state->captured)
        return true;

    WorldPacket answer = std::move(*state->captured);
    state->captured.reset();

    answer.rpos(0);
    answer.read_skip<uint64>();
    uint8 lootType = answer.read<uint8>();

    // the core refused the corpse: its answer goes out as it is
    if (lootType == LOOT_NONE || player->GetLootGUID() != guid)
    {
        player->SendDirectMessage(&answer);
        return true;
    }

    // the player looks at the merged window, not at the clicked corpse's:
    // the core's notices about it carry its own slot numbers
    clicked->loot.RemoveLooter(player->GetGUID());

    state->primary = guid;
    state->lootType = lootType;
    state->active = true;

    for (Creature* creature : others)
    {
        OpenCorpse(player, creature);
        state->secondaries.push_back(creature->GetGUID());
    }

    SendWindow(player, *state);
    return true;
}

bool AoeLoot::HandleTakeItem(Player* player, uint8 slot)
{
    AoeLootState* state = player->CustomData.Get<AoeLootState>(STATE_KEY);
    if (!IsOpen(player, state))
        return false;

    if (player->HasPlayerFlag(PLAYER_FLAGS_NO_PLAY_TIME))
    {
        player->SendLootError(state->primary, LOOT_ERROR_PLAY_TIME_EXCEEDED);
        return true;
    }

    if (slot >= state->rows.size() || state->rows[slot].taken)
    {
        player->SendEquipError(EQUIP_ERR_ALREADY_LOOTED, nullptr, nullptr);
        return true;
    }

    // each corpse's share is taken from its corpse, until the bags are full
    AoeLootRow& row = state->rows[slot];
    bool tookSome = false;
    bool reached = false;

    for (AoeLootPart& part : row.parts)
    {
        if (part.taken)
            continue;

        Creature* source = SourceCorpse(player, *state, part.source);
        if (!source)
            continue;

        reached = true;
        sScriptMgr->OnPlayerAfterCreatureLoot(player);

        InventoryResult result;
        state->muteRemovals = true;
        player->StoreLootItem(part.sourceSlot, &source->loot, result);
        state->muteRemovals = false;

        if (!IsPartGone(player, source->loot, part, row.itemId))
            break;

        part.taken = true;
        tookSome = true;
    }

    if (std::all_of(row.parts.begin(), row.parts.end(), [](AoeLootPart const& part) { return part.taken; }))
    {
        row.taken = true;
        player->SendNotifyLootItemRemoved(slot);
    }
    // part of the stack went: the window shows what is left of it
    else if (tookSome)
        SendWindow(player, *state);
    else if (!reached)
        player->SendLootError(state->primary, LOOT_ERROR_TOO_FAR);

    return true;
}

bool AoeLoot::HandleTakeMoney(Player* player)
{
    AoeLootState* state = player->CustomData.Get<AoeLootState>(STATE_KEY);
    if (!IsOpen(player, state))
        return false;

    // the money would be zeroed whoever is paid: a restricted looter keeps
    // it on the corpses
    if (player->HasPlayerFlag(PLAYER_FLAGS_NO_PLAY_TIME))
    {
        player->SendLootError(state->primary, LOOT_ERROR_PLAY_TIME_EXCEEDED);
        return true;
    }

    std::vector<Loot*> loots;
    for (ObjectGuid const& guid : state->moneySources)
        if (Creature* creature = SourceCorpse(player, *state, guid))
            if (creature->loot.gold)
                loots.push_back(&creature->loot);

    state->moneySources.clear();

    uint32 total = 0;
    for (Loot* loot : loots)
    {
        sScriptMgr->OnPlayerBeforeLootMoney(player, loot);
        loot->NotifyMoneyRemoved();
        total += loot->gold;
    }

    // what WorldSession::HandleLootMoneyOpcode pays for a corpse, once for
    // the sum: shared among the group members near the player
    if (total && player->GetGroup())
    {
        std::vector<Player*> playersNear;
        for (GroupReference* itr = player->GetGroup()->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (member && player->IsAtLootRewardDistance(member))
                playersNear.push_back(member);
        }

        uint32 goldPerPlayer = playersNear.empty() ? 0 : total / playersNear.size();

        for (Player* member : playersNear)
        {
            uint32 finalGold = goldPerPlayer;

            if (member->HasPlayerFlag(PLAYER_FLAGS_NO_PLAY_TIME))
                continue;

            if (member->HasPlayerFlag(PLAYER_FLAGS_PARTIAL_PLAY_TIME))
            {
                finalGold /= 2;
                if (!finalGold)
                    continue;
            }

            member->ModifyMoney(finalGold);
            member->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_MONEY, finalGold);

            WorldPacket data(SMSG_LOOT_MONEY_NOTIFY, 4 + 1);
            data << uint32(finalGold);
            data << uint8(playersNear.size() > 1 ? 0 : 1);     // 0: "Your share is...", 1: "You loot..."
            member->SendDirectMessage(&data);
        }
    }
    else if (total)
    {
        uint32 finalGold = total;
        if (player->HasPlayerFlag(PLAYER_FLAGS_PARTIAL_PLAY_TIME))
            finalGold /= 2;

        sScriptMgr->OnPlayerAfterCreatureLootMoney(player);

        if (finalGold)
        {
            player->ModifyMoney(finalGold);
            player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_MONEY, finalGold);

            WorldPacket data(SMSG_LOOT_MONEY_NOTIFY, 4 + 1);
            data << uint32(finalGold);
            data << uint8(1);                                   // "You loot..."
            player->SendDirectMessage(&data);
        }
    }

    for (Loot* loot : loots)
    {
        sScriptMgr->OnLootMoney(player, loot->gold);
        loot->gold = 0;
    }

    player->SendNotifyLootMoneyRemoved();
    return true;
}

bool AoeLoot::HandleMasterGive(Player* player, ObjectGuid lootGuid, uint8 slot, ObjectGuid targetGuid)
{
    AoeLootState* state = player->CustomData.Get<AoeLootState>(STATE_KEY);
    if (!IsOpen(player, state) || lootGuid != state->primary)
        return false;

    // what WorldSession::HandleLootMasterGiveOpcode checks
    Group* group = player->GetGroup();
    if (!group || group->GetMasterLooterGuid() != player->GetGUID() || group->GetLootMethod() != MASTER_LOOT)
    {
        player->SendLootError(lootGuid, LOOT_ERROR_DIDNT_KILL);
        return true;
    }

    Player* target = ObjectAccessor::GetPlayer(*player, targetGuid);
    if (!target)
    {
        player->SendLootError(lootGuid, LOOT_ERROR_PLAYER_NOT_FOUND);
        return true;
    }

    if (!player->IsInRaidWith(target) || target->HasPlayerFlag(PLAYER_FLAGS_NO_PLAY_TIME))
    {
        player->SendLootError(lootGuid, LOOT_ERROR_MASTER_OTHER);
        return true;
    }

    // master loot rows never stack: one row, one corpse
    if (slot >= state->rows.size() || state->rows[slot].taken || state->rows[slot].parts.size() != 1)
        return true;

    AoeLootRow& row = state->rows[slot];
    AoeLootPart& part = row.parts.front();
    Creature* source = SourceCorpse(player, *state, part.source);
    if (!source)
    {
        player->SendLootError(lootGuid, LOOT_ERROR_TOO_FAR);
        return true;
    }

    Loot* loot = &source->loot;
    if (part.sourceSlot >= loot->items.size() + loot->quest_items.size())
        return true;

    LootItem& item = part.sourceSlot >= loot->items.size() ? loot->quest_items[part.sourceSlot - loot->items.size()] : loot->items[part.sourceSlot];
    if (item.is_looted || item.itemid != row.itemId)
        return true;

    ItemPosCountVec dest;
    InventoryResult msg = target->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, item.itemid, item.count);
    if (!item.AllowedForPlayer(target, loot->sourceWorldObjectGUID))
        msg = EQUIP_ERR_YOU_CAN_NEVER_USE_THAT_ITEM;
    if (msg != EQUIP_ERR_OK)
    {
        if (msg == EQUIP_ERR_CANT_CARRY_MORE_OF_THIS)
            player->SendLootError(lootGuid, LOOT_ERROR_MASTER_UNIQUE_ITEM);
        else if (msg == EQUIP_ERR_INVENTORY_FULL)
            player->SendLootError(lootGuid, LOOT_ERROR_MASTER_INV_FULL);
        else
            player->SendLootError(lootGuid, LOOT_ERROR_MASTER_OTHER);

        return true;
    }

    // list of players allowed to receive this item in trade
    AllowedLooterSet looters = item.GetAllowedLooters();

    Item* newItem = target->StoreNewItem(dest, item.itemid, true, item.randomPropertyId, looters);
    target->SendNewItem(newItem, uint32(item.count), false, false, true);
    target->UpdateLootAchievements(&item, loot);

    item.count = 0;
    item.is_looted = true;

    // the others looking at that corpse, with its own slot; the master
    // looter, with the window's
    loot->NotifyItemRemoved(part.sourceSlot);
    --loot->unlootedCount;

    part.taken = true;
    row.taken = true;
    player->SendNotifyLootItemRemoved(slot);
    return true;
}

void AoeLoot::HandleRelease(Player* player, ObjectGuid guid)
{
    AoeLootState* state = player->CustomData.Get<AoeLootState>(STATE_KEY);
    if (IsOpen(player, state) && guid == state->primary)
        Release(player);
}

void AoeLoot::ReleaseIfClosed(Player* player)
{
    AoeLootState* state = player->CustomData.Get<AoeLootState>(STATE_KEY);
    if (state && state->active && player->GetLootGUID() != state->primary)
        Release(player);
}

void AoeLoot::Release(Player* player)
{
    AoeLootState* state = player->CustomData.Get<AoeLootState>(STATE_KEY);
    if (!state || !state->active)
        return;

    if (player->IsInWorld())
        for (ObjectGuid const& guid : state->secondaries)
            if (Creature* creature = player->GetMap()->GetCreature(guid))
                if (!creature->IsAlive())
                    CloseCorpse(player, creature);

    state->Reset();
}

bool AoeLoot::FilterOutgoing(WorldSession* session, WorldPacket const& packet)
{
    uint16 opcode = packet.GetOpcode();
    if (opcode != SMSG_LOOT_RESPONSE && opcode != SMSG_LOOT_REMOVED)
        return true;

    Player* player = session->GetPlayer();
    if (!player)
        return true;

    AoeLootState* state = player->CustomData.Get<AoeLootState>(STATE_KEY);
    if (!state)
        return true;

    if (opcode == SMSG_LOOT_RESPONSE && state->capturing && !state->captured)
    {
        state->captured.emplace(packet);
        return false;
    }

    if (opcode == SMSG_LOOT_REMOVED && state->muteRemovals)
        return false;

    return true;
}
