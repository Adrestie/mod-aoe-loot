# mod-aoe-loot

**WORK IN PROGRESS**

Area loot for AzerothCore 3.3.5a, as Retail has it since Mists of Pandaria: looting one corpse opens a single window that also holds the loot of the corpses around it.

## Key features

- Every nearby corpse you may loot, in one window.
- The core's loot rules untouched: your quest items, your share under the group's loot method, rolls started once.
- The same item from several corpses stacked on one row; what you leave stays on its corpse.
- The money of all the corpses in one click, shared in a group.
- Solo or in a group, with the client's auto loot. Nothing on the client side.

## Dependencies

| Dependency | For |
|---|---|
| [AzerothCore](https://github.com/azerothcore/azerothcore-wotlk) (WotLK 3.3.5a) | the server |

## How to install

1. Download `installer.exe` from the [WoW-mods-installer releases](https://github.com/Adrestie/WoW-mods-installer/releases).
2. Stop the worldserver.
3. Run `installer.exe`, give it this folder and the folders it asks for, click **Check**, then **Install**.
4. Rebuild the worldserver.

## How to update

Same steps with the new version's folder, choosing **Update** instead of **Install**. Your `mod-aoe-loot.conf` settings are kept. Rebuild the worldserver.

## How to uninstall

Same steps, choosing **Remove**. Rebuild the worldserver.

## Configuration

`mod-aoe-loot.conf` sets:

- the module on or off;
- how far the other corpses may lie;
- how many corpses and rows one window holds;
- whether corpses behind a wall or a floor are left out.
