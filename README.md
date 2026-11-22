# wow-tofuworld-bot-duels

Tofu Worldview module for the mod-playerbots fork of AzerothCore (3.3.5a). Written against core 32fd347 and mod-playerbots ecc4a53. Requires mod-playerbots in the same build.

## What it does

- Random bots challenge each other to duels near real players, so players see duels happen in the open world. Bots accept and fight duels with mod-playerbots' own `duel` strategy; this module only sends the challenge (spell 7266).
- Now and then a bot challenges a real player (`BotDuels.ChallengePlayers.Chance`, then a cooldown per player). The player accepts or declines as with any duel.
- Why a module: stock mod-playerbots never starts a duel. Its `start duel` strategy has no triggers, and the `rpg duel` trigger needs an rpg target that only the old `rpg` strategy chooses, while random bots run `new rpg`.
- Runs only around real players: each real player's own update scans the bots on its map within `BotDuels.Radius` every `BotDuels.IntervalSeconds`, and starts at most one duel per scan, up to `BotDuels.MaxActivePerPlayer` duels around that player.
- Takes part: random bots that are not in a group, at least `MinLevel`, healthy, idle, within `MaxLevelDifference` and `PartnerRadius` of the other side, in line of sight, in the open world where the area allows duels and outside `Playerbots.PvpProhibitedZoneIds` and `AreaIds`. Altbots, addclass bots, SelfBots, and grouped bots never duel through this module.

## Files

- `src/bot_duels.cpp`, `src/bot_duels_loader.cpp`: the module. Its folder in the deploy set must be named `mod-tofu-bot-duels`, because the core calls `Addmod_tofu_bot_duelsScripts()`.
- `conf/bot_duels.conf.dist`: every setting. The module is off until `BotDuels.Enable = 1`.

## Install

1. Clone into the deploy set: `C:\Users\chris\Desktop\WoW Projects\modules\mod-tofu-bot-duels`, branch `main`.
2. Update Source (builds RelWithDebInfo).
3. Copy `configs\modules\bot_duels.conf.dist` to `bot_duels.conf` and set the values you want.
4. Start the servers.

## Checks

- Worldserver log at startup: `BotDuels: enabled. Duel spell range ... yards, partners within ..., player challenge chance ...% per scan.`
- With `BotDuels.Log = 1`: lines `BotDuels: <bot> challenged <bot> (sent).` while a real player stands among idle level 10+ random bots in an area that allows duels.
- In game: bots near you duel each other (the duel flag appears between them); with a non-zero chance, a bot eventually challenges you.
