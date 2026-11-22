/*
 * wow-tofuworld-bot-duels (Tofu Worldview)
 *
 * Random bots challenge each other to duels near real players, so players see duels happen, and now and then a bot challenges a real player.
 * - Stock mod-playerbots never starts a duel: its "start duel" strategy has no triggers, and the "rpg duel" trigger needs an rpg target that only the old "rpg" strategy chooses (random bots use "new rpg"). Bots do accept and fight duels through the stock "duel" strategy, so this module only sends the challenge (spell 7266).
 * - Runs only around real players: each real player's own update (map thread) scans the bots on its map within BotDuels.Radius every BotDuels.IntervalSeconds. Nothing runs where no real player is.
 * - Only random bots duel. Altbots, addclass bots, SelfBots, and any bot in a group are left alone.
 *
 * Written against mod-playerbots/azerothcore-wotlk 32fd347 and mod-playerbots ecc4a53.
 */

#include "Config.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "SocialMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldSession.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr uint32 SpellDuel = 7266;

    struct Settings
    {
        bool enable = false;
        uint32 intervalMs = 10000;
        float radius = 60.0f;
        float partnerRadius = 10.0f;
        uint32 minLevel = 10;
        uint32 maxLevelDifference = 3;
        float minHealthPct = 90.0f;
        uint32 botCooldownSeconds = 120;
        uint32 maxActivePerPlayer = 2;
        uint32 challengePlayersChance = 0;
        uint32 challengePlayersCooldownSeconds = 1800;
        bool log = false;
    };

    Settings settings;
    float duelSpellRange = 0.0f; // max range of spell 7266, read at startup; 0 until then

    // Shared by all map threads (each real player's update runs in its map's thread).
    std::mutex stateLock;
    std::unordered_map<uint32, uint32> scanTimers;     // real player GUID -> ms since last scan
    std::unordered_map<uint32, int64> botReadyAt;      // bot GUID -> game time when it may duel again
    std::unordered_map<uint32, int64> playerReadyAt;   // real player GUID -> game time when a bot may challenge them again

    void LoadSettings()
    {
        Settings s;
        s.enable = sConfigMgr->GetOption<bool>("BotDuels.Enable", false);
        s.intervalMs = std::max<uint32>(sConfigMgr->GetOption<uint32>("BotDuels.IntervalSeconds", 10), 1) * 1000;
        s.radius = sConfigMgr->GetOption<float>("BotDuels.Radius", 60.0f);
        s.partnerRadius = sConfigMgr->GetOption<float>("BotDuels.PartnerRadius", 10.0f);
        s.minLevel = sConfigMgr->GetOption<uint32>("BotDuels.MinLevel", 10);
        s.maxLevelDifference = sConfigMgr->GetOption<uint32>("BotDuels.MaxLevelDifference", 3);
        s.minHealthPct = float(std::min<uint32>(sConfigMgr->GetOption<uint32>("BotDuels.MinHealthPct", 90), 100));
        s.botCooldownSeconds = sConfigMgr->GetOption<uint32>("BotDuels.BotCooldownSeconds", 120);
        s.maxActivePerPlayer = sConfigMgr->GetOption<uint32>("BotDuels.MaxActivePerPlayer", 2);
        s.challengePlayersChance = std::min<uint32>(sConfigMgr->GetOption<uint32>("BotDuels.ChallengePlayers.Chance", 0), 100);
        s.challengePlayersCooldownSeconds = sConfigMgr->GetOption<uint32>("BotDuels.ChallengePlayers.CooldownSeconds", 1800);
        s.log = sConfigMgr->GetOption<bool>("BotDuels.Log", false);
        settings = s;
    }

    // The duel spell fails out of range, so partners must be within its range.
    float PartnerRange()
    {
        if (duelSpellRange <= 0.0f)
            return settings.partnerRadius;
        return std::min(settings.partnerRadius, duelSpellRange - 1.0f);
    }

    int64 Now()
    {
        return GameTime::GetGameTime().count();
    }

    bool AreaAllowsDuels(Player* player)
    {
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(player->GetAreaId());
        return !area || (area->flags & AREA_FLAG_ALLOW_DUELS);
    }

    // Free of anything that would make a duel fail or look wrong.
    bool IsIdle(Player* p)
    {
        return p->IsInWorld() && !p->IsBeingTeleported() && p->IsAlive() && !p->IsInCombat() && !p->duel && !p->IsInFlight() && !p->GetVehicle() && p->GetHealthPct() >= settings.minHealthPct && AreaAllowsDuels(p) && !sPlayerbotAIConfig.IsPvpProhibited(p->GetZoneId(), p->GetAreaId());
    }

    bool IsDuelBot(Player* p)
    {
        return p && sRandomPlayerbotMgr.IsRandomBot(p) && !p->GetGroup();
    }

    // Caller holds stateLock.
    bool Ready(std::unordered_map<uint32, int64> const& readyAt, Player* p, int64 now)
    {
        auto it = readyAt.find(p->GetGUID().GetCounter());
        return it == readyAt.end() || it->second <= now;
    }

    bool LevelsMatch(Player* a, Player* b)
    {
        uint32 la = a->GetLevel();
        uint32 lb = b->GetLevel();
        return (la > lb ? la - lb : lb - la) <= settings.maxLevelDifference;
    }

    bool CanChallenge(Player* challenger, Player* target)
    {
        if (challenger->GetExactDist(target) > PartnerRange() || !LevelsMatch(challenger, target))
            return false;
        if (!target->GetSocial() || target->GetSocial()->HasIgnore(challenger->GetGUID()))
            return false;
        return challenger->IsWithinLOSInMap(target);
    }

    bool SendChallenge(Player* challenger, Player* target)
    {
        if (challenger->IsMounted())
            challenger->Dismount();
        challenger->SetSelection(target->GetGUID());
        SpellCastResult result = challenger->CastSpell(target, SpellDuel, false);
        if (settings.log)
            LOG_INFO("module", "BotDuels: {} challenged {} ({}).", challenger->GetName(), target->GetName(), result == SPELL_CAST_OK ? "sent" : "cast failed " + std::to_string(uint32(result)));
        return result == SPELL_CAST_OK;
    }

    void Scan(Player* player)
    {
        Map* map = player->FindMap();
        if (!map || map->Instanceable())
            return;

        int64 now = Now();
        std::vector<Player*> idleBots;
        uint32 activeDuels = 0;

        {
            std::lock_guard<std::mutex> guard(stateLock);
            for (auto const& ref : map->GetPlayers())
            {
                Player* bot = ref.GetSource();
                if (!bot || bot == player || !IsDuelBot(bot) || !player->InSamePhase(bot) || bot->GetExactDist(player) > settings.radius)
                    continue;
                if (bot->duel)
                {
                    ++activeDuels;
                    continue;
                }
                if (bot->GetLevel() >= settings.minLevel && IsIdle(bot) && Ready(botReadyAt, bot, now))
                    idleBots.push_back(bot);
            }
        }

        if (idleBots.empty())
            return;

        // Rarely, a bot challenges this real player. The cooldown starts whether or not the player accepts.
        if (settings.challengePlayersChance && !player->IsGameMaster() && !player->isAFK() && IsIdle(player) && urand(1, 100) <= settings.challengePlayersChance)
        {
            bool ready = false;
            {
                std::lock_guard<std::mutex> guard(stateLock);
                ready = Ready(playerReadyAt, player, now);
            }

            if (ready)
            {
                Player* challenger = nullptr;
                float best = 0.0f;
                for (Player* bot : idleBots)
                {
                    if (!CanChallenge(bot, player))
                        continue;
                    float dist = bot->GetExactDist(player);
                    if (!challenger || dist < best)
                    {
                        challenger = bot;
                        best = dist;
                    }
                }

                if (challenger && SendChallenge(challenger, player))
                {
                    std::lock_guard<std::mutex> guard(stateLock);
                    playerReadyAt[player->GetGUID().GetCounter()] = now + settings.challengePlayersCooldownSeconds;
                    botReadyAt[challenger->GetGUID().GetCounter()] = now + settings.botCooldownSeconds;
                    return;
                }
            }
        }

        // Bot versus bot: at most one new duel per scan, up to MaxActivePerPlayer around this player.
        if (activeDuels >= settings.maxActivePerPlayer || idleBots.size() < 2)
            return;

        // Try challengers in random order; pair each with its nearest valid partner.
        for (std::size_t i = idleBots.size() - 1; i > 0; --i)
            std::swap(idleBots[i], idleBots[urand(0, uint32(i))]);

        for (Player* challenger : idleBots)
        {
            Player* partner = nullptr;
            float best = 0.0f;
            for (Player* other : idleBots)
            {
                if (other == challenger || !CanChallenge(challenger, other))
                    continue;
                float dist = challenger->GetExactDist(other);
                if (!partner || dist < best)
                {
                    partner = other;
                    best = dist;
                }
            }

            if (partner && SendChallenge(challenger, partner))
            {
                std::lock_guard<std::mutex> guard(stateLock);
                botReadyAt[challenger->GetGUID().GetCounter()] = now + settings.botCooldownSeconds;
                botReadyAt[partner->GetGUID().GetCounter()] = now + settings.botCooldownSeconds;
                return;
            }
        }
    }
}

class BotDuelsWorldScript : public WorldScript
{
public:
    BotDuelsWorldScript() : WorldScript("BotDuelsWorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LoadSettings();
    }

    void OnStartup() override
    {
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(SpellDuel))
            duelSpellRange = info->GetMaxRange(info->IsPositive());
        LOG_INFO("module", "BotDuels: {}. Duel spell range {:.1f} yards, partners within {:.1f}, player challenge chance {}% per scan.", settings.enable ? "enabled" : "disabled", duelSpellRange, PartnerRange(), settings.challengePlayersChance);
    }
};

class BotDuelsPlayerScript : public PlayerScript
{
public:
    BotDuelsPlayerScript() : PlayerScript("BotDuelsPlayerScript", { PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_LOGOUT }) { }

    // Runs for every character, bots included, so bots leave at the first check.
    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        if (!settings.enable || !player || !player->GetSession() || player->GetSession()->IsHeadless() || !IsRealPlayer(player))
            return;

        {
            std::lock_guard<std::mutex> guard(stateLock);
            uint32& elapsed = scanTimers[player->GetGUID().GetCounter()];
            elapsed += diff;
            if (elapsed < settings.intervalMs)
                return;
            elapsed = 0;
        }

        Scan(player);
    }

    void OnPlayerLogout(Player* player) override
    {
        if (!player)
            return;
        std::lock_guard<std::mutex> guard(stateLock);
        uint32 guid = player->GetGUID().GetCounter();
        scanTimers.erase(guid);
        botReadyAt.erase(guid);
    }
};

void AddSC_wow_tofuworld_bot_duels()
{
    new BotDuelsWorldScript();
    new BotDuelsPlayerScript();
}
