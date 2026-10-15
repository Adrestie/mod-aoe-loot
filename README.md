# mod-aoe-loot

Area loot for AzerothCore 3.3.5a, as Retail has it since Mists of Pandaria:
when several monsters died close together, clicking one corpse you may loot
opens a single window that also holds the loot of the other corpses around it
that you are entitled to.

**Work in progress: written, built against AzerothCore, not yet tried in
game.** See [What to check in game](#what-to-check-in-game).

## How it behaves

- **Spatial.** The corpses need not have died together nor belong to the same
  pack: what counts is that they lie near you (`AoeLoot.Range`) and still have
  loot. A corpse farther away waits for another click.
- **Only your corpses.** A monster tagged by a player outside your group or
  raid is left out, as the game already does when you click it.
- **Only your loot.** The window shows what the game would show you on each
  corpse, no more: your quest items, the items free for you under your group's
  loot method, rolls in progress, nothing held for someone else. The rules of
  who gets what are the core's, untouched. In a group, each member loots their
  own share with their own click.
- **Quest items, not more than needed.** Each corpse alone only knows what your
  bags hold; the window also counts what the other corpses offer, and stops
  offering a quest item once your quests have what they ask for.
- **What you leave stays.** Nothing moves between corpses: each row of the
  window points at the corpse it came from, and taking it takes it from there.
  A row you leave, or one your full bags refuse, stays on its corpse.
- **Rolls once.** As in the core, the group's roll starts the first time
  anyone opens a corpse; a corpse opened through the window counts, and the
  roll does not start again for the next player.
- **Money** of all the corpses is taken in one click, and shared in a group as
  the core shares a corpse's.
- **Solo too.** Being in a group is not needed.
- **Auto loot** is the client's: with it, one click takes everything that fits.

Only monster corpses: chests, skinning, pickpocketing, players' insignia and
items keep the core's loot. With a single corpse around, the module leaves the
core alone.

## How it works

The clicked corpse is opened by the core (`Player::SendLoot`), so its rights,
rolls and round robin are the core's own. Its answer is held back, the rows
the other corpses would show you are added to it, and the merged window goes
out in its place, every row numbered again from 0. The module then answers the
window's packets itself -- taking a row, the money, the master looter giving a
row, closing -- by sending each to the corpse the row belongs to. Closing the
window closes every corpse it drew from, so the round robin, the lootable flag
and the corpse's decay follow as if each had been opened by hand.

One thing the merged window does not see: an item that leaves one of its
corpses by someone else's hand while it is open -- another player takes it, a
roll hands it out -- stays shown; clicking it says it is already looted.
Opening the window again shows it gone.

## Requirements

| | |
|---|---|
| [AzerothCore](https://github.com/azerothcore/azerothcore-wotlk) | 3.3.5a, built with the module in `modules/` |

No database change, nothing on the client side.

## Installing

Stop the world server, then run `installer.exe`, the WoW-mods installer
([WoW-mods-installer releases](https://github.com/Adrestie/WoW-mods-installer/releases)),
and give it this folder. It copies the module into `modules/` and the
configuration into `configs/modules/`. Then rebuild the core.

By hand: copy the module into `modules/`, copy `conf/mod-aoe-loot.conf.dist`
to `configs/modules/mod-aoe-loot.conf`, rebuild the core.

## Configuration

`mod-aoe-loot.conf`:

| Setting | Default | |
|---|---|---|
| `AoeLoot.Enable` | 1 | the module on or off |
| `AoeLoot.Range` | 30 | how far the other corpses may lie, in yards |
| `AoeLoot.MaxCorpses` | 15 | corpses per window, the clicked one included, nearest first |
| `AoeLoot.MaxItems` | 16 | rows per window; the rest wait on their corpses |
| `AoeLoot.LineOfSight` | 1 | leave out the corpses behind a wall or a floor |

## What to check in game

- Alone, several corpses: one window; leave an item, close, and find it again
  on the corpse it dropped from.
- Full bags: what does not fit stays on its corpse.
- A monster tagged by a player outside the group, among yours: not in the
  window.
- In a group: your click does not take a teammate's quest items or held items;
  they still loot their own.
- Group loot or need before greed with green or better items on several
  corpses: one roll per item, never twice, nothing lost, nothing doubled.
- Master loot: the master looter gives a row of another corpse; the others see
  it locked.
- Round robin: the items under the threshold go to the round robin player
  first, then to everyone once they close the window.
- A quest item on several corpses with fewer needed: the window offers only
  what the quest still asks for.
- Money from several corpses: one line in the chat, shared in a group.
- More rows than `AoeLoot.MaxItems`: the client shows them all, and the rest
  waits on the corpses. Try raising the value to find the client's real limit.
- The client's auto loot: one click empties what fits.
- Corpse decay and skinning once a corpse of the window is emptied.

## Licence

GNU General Public License, version 2 or later, like AzerothCore.
