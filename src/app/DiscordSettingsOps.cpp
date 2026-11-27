#include "Ingest.h"
#include "Db.h"
#include "devhub/Util.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <string>
#include <vector>

namespace devhub {

bool parseDiscordAdminUserIds(const std::string& value,
                              std::vector<std::string>& userIds,
                              std::string* error) {
    userIds.clear();
    if (error) error->clear();

    std::string token;
    auto flush = [&]() {
        if (token.empty()) return true;
        std::uint64_t parsed = 0;
        const char* begin = token.data();
        const char* end = begin + token.size();
        const auto result = std::from_chars(begin, end, parsed, 10);
        if (result.ec != std::errc{} || result.ptr != end || parsed == 0)
            return false;
        std::string canonical = std::to_string(parsed);
        if (std::find(userIds.begin(), userIds.end(), canonical) == userIds.end())
            userIds.push_back(std::move(canonical));
        token.clear();
        return true;
    };

    for (unsigned char ch : value) {
        if (ch == ',' || ch == ';' || ch <= ' ') {
            if (!flush()) {
                userIds.clear();
                if (error)
                    *error = "use positive decimal Discord user IDs only";
                return false;
            }
        } else {
            token.push_back(static_cast<char>(ch));
        }
    }
    if (!flush() || (!trim(value).empty() && userIds.empty())) {
        userIds.clear();
        if (error)
            *error = "use positive decimal Discord user IDs separated by commas, semicolons, or spaces";
        return false;
    }
    return true;
}

bool discordAdminUserAllowed(Db* db, const std::string& userId) {
    if (!db || userId.empty()) return false;
    std::string configured;
    {
        auto lk = db->guard();
        configured = db->getSetting(lk.token(), "discord_admin_user_ids");
    }
    std::vector<std::string> userIds;
    if (!parseDiscordAdminUserIds(configured, userIds)) return false;
    return std::find(userIds.begin(), userIds.end(), userId) != userIds.end();
}

bool discordGuildEnabled(Db* db, const std::string& guildId) {
    if (guildId.empty()) return false;
    auto lk = db->guard();
    SQLite::Statement q(db->raw(lk.token()),
        "SELECT enabled FROM discord_guilds WHERE guild_id=?");
    q.bind(1, guildId);
    return q.executeStep() && q.getColumn(0).getInt() != 0;
}

bool discordChannelEnabled(Db* db, const std::string& channelId) {
    if (channelId.empty()) return false;
    auto lk = db->guard();
    SQLite::Statement q(db->raw(lk.token()),
        "SELECT enabled FROM discord_channels WHERE channel_id=?");
    q.bind(1, channelId);
    return q.executeStep() && q.getColumn(0).getInt() != 0;
}

bool discordWatch(Db* db, const std::string& guildId,
                  const std::string& channelId) {
    if (channelId.empty()) return false;
    auto lk = db->guard();
    SQLite::Statement channel(db->raw(lk.token()),
        "SELECT enabled FROM discord_channels WHERE channel_id=?");
    channel.bind(1, channelId);
    // Explicit channel choices always win, including disabled channels.
    if (channel.executeStep()) return channel.getColumn(0).getInt() != 0;
    if (db->getSetting(lk.token(), "discord_auto_add_channels") == "0" ||
        guildId.empty()) return false;
    const std::string layout = db->getSetting(lk.token(), "discord_layout_" + channelId);
    if (!layout.empty()) {
        try {
            const auto entry = nlohmann::json::parse(layout);
            const int type = entry.at("type").get<int>();
            const auto parentId = entry.at("parent").get<std::string>();
            if (type >= 10 && type <= 12 && !parentId.empty() &&
                entry.at("guild").get<std::string>() == guildId) {
                SQLite::Statement parent(db->raw(lk.token()),
                    "SELECT enabled FROM discord_channels WHERE channel_id=? "
                    "AND (guild_id=? OR guild_id='')");
                parent.bind(1, parentId);
                parent.bind(2, guildId);
                // An explicit parent choice overrides server-wide discovery,
                // but never overrides an explicit child choice checked above.
                if (parent.executeStep()) return parent.getColumn(0).getInt() != 0;
            }
        } catch (const nlohmann::json::exception&) {
            // Missing/bad display metadata retains the existing guild policy.
        }
    }
    SQLite::Statement guild(db->raw(lk.token()),
        "SELECT enabled FROM discord_guilds WHERE guild_id=?");
    guild.bind(1, guildId);
    return guild.executeStep() && guild.getColumn(0).getInt() != 0;
}

bool discordDiscoverThread(Db* db, const std::string& channelId,
    const std::string& guildId, const std::string& guildName,
    const std::string& name, const std::string& parentId) {
    if (channelId.empty() || guildId.empty() || parentId.empty() || channelId == parentId)
        return false;
    auto lk = db->guard();
    // The lock covers policy and insert; an operator's disabled row cannot be
    // overwritten by a delayed gateway event or a concurrent discovery pass.
    SQLite::Statement existing(db->raw(lk.token()),
        "SELECT enabled FROM discord_channels WHERE channel_id=?");
    existing.bind(1, channelId);
    if (existing.executeStep()) return existing.getColumn(0).getInt() != 0;
    if (!discordWatch(db, guildId, channelId)) return false;
    SQLite::Statement add(db->raw(lk.token()),
        "INSERT INTO discord_channels(channel_id,guild_id,guild_name,channel_name,"
        "project_id,created_at) VALUES(?,?,?,?,"
        "(SELECT project_id FROM discord_channels WHERE channel_id=? "
        "AND (guild_id=? OR guild_id='')),?) "
        "ON CONFLICT(channel_id) DO NOTHING");
    add.bind(1, channelId);
    add.bind(2, guildId);
    add.bind(3, guildName);
    add.bind(4, name);
    add.bind(5, parentId);
    add.bind(6, guildId);
    add.bind(7, nowIsoUtc());
    add.exec();
    return discordChannelEnabled(db, channelId);
}

void discordSetChannelMetadata(Db* db, const std::string& channelId,
                               const std::string& name,
                               const std::string& guildId,
                               const std::string& guildName) {
    if (channelId.empty() || name.empty()) return;
    auto lk = db->guard();
    // Metadata refresh never adds or enables a monitor.
    SQLite::Statement up(db->raw(lk.token()),
        "UPDATE discord_channels SET channel_name=?,"
        "guild_id=CASE WHEN ?='' THEN guild_id ELSE ? END,"
        "guild_name=CASE WHEN ?='' THEN guild_name ELSE ? END "
        "WHERE channel_id=?");
    up.bind(1, name);
    up.bind(2, guildId);
    up.bind(3, guildId);
    up.bind(4, guildName);
    up.bind(5, guildName);
    up.bind(6, channelId);
    up.exec();
}

void discordSetChannelLayout(Db* db, const std::string& channelId,
    const std::string& guildId, const std::string& name,
    const std::string& parentId, int position, int type) {
    if (channelId.empty() || guildId.empty()) return;
    const std::string value = nlohmann::json({{"id", channelId}, {"guild", guildId},
        {"name", name}, {"parent", parentId}, {"position", position}, {"type", type}}).dump();
    auto lk = db->guard();
    const std::string key = "discord_layout_" + channelId;
    if (db->getSetting(lk.token(), key) != value)
        db->setSetting(lk.token(), key, value);
}

void discordPruneChannelLayout(Db* db, const std::string& guildId,
    const std::vector<std::string>& currentChannelIds) {
    auto lk = db->guard();
    std::vector<std::string> stale;
    SQLite::Statement rows(db->raw(lk.token()),
        "SELECT key,value FROM settings WHERE key GLOB 'discord_layout_[0-9]*'");
    while (rows.executeStep()) {
        try {
            const auto entry = nlohmann::json::parse(rows.getColumn(1).getString());
            const int type = entry.at("type").get<int>();
            // Guild channel snapshots omit threads, including archived forum posts.
            if (entry.at("guild").get<std::string>() != guildId || (type >= 10 && type <= 12)) continue;
            const auto id = entry.at("id").get<std::string>();
            if (std::find(currentChannelIds.begin(), currentChannelIds.end(), id) == currentChannelIds.end())
                stale.push_back(rows.getColumn(0).getString());
        } catch (const nlohmann::json::exception&) {
            // Leave malformed entries for the next metadata refresh to replace.
        }
    }
    for (const auto& key : stale) {
        SQLite::Statement remove(db->raw(lk.token()), "DELETE FROM settings WHERE key=?");
        remove.bind(1, key);
        remove.exec();
    }
}

void discordRemoveChannelLayout(Db* db, const std::string& channelId) {
    auto lk = db->guard();
    SQLite::Statement remove(db->raw(lk.token()), "DELETE FROM settings WHERE key=?");
    remove.bind(1, "discord_layout_" + channelId);
    remove.exec();
}

bool discordChannelCheckMetadata(Db* db, const std::string& channelId,
                                 std::string& guildId, std::string& channelName,
                                 std::string& guildName) {
    guildId.clear();
    channelName.clear();
    guildName.clear();
    if (channelId.empty()) return false;
    auto lk = db->guard();
    SQLite::Statement channel(db->raw(lk.token()),
        "SELECT guild_id,channel_name,guild_name FROM discord_channels WHERE channel_id=?");
    channel.bind(1, channelId);
    if (channel.executeStep()) {
        guildId = channel.getColumn(0).getString();
        channelName = channel.getColumn(1).getString();
        guildName = channel.getColumn(2).getString();
    }
    const auto layout = nlohmann::json::parse(db->getSetting(lk.token(), "discord_layout_" + channelId),
                                    nullptr, false);
    if (layout.is_object()) {
        if (guildId.empty() && layout.contains("guild") && layout["guild"].is_string())
            guildId = layout["guild"].get<std::string>();
        if (channelName.empty() && layout.contains("name") && layout["name"].is_string())
            channelName = layout["name"].get<std::string>();
    }
    if (guildName.empty() && !guildId.empty()) {
        SQLite::Statement guild(db->raw(lk.token()),
            "SELECT guild_name FROM discord_guilds WHERE guild_id=?");
        guild.bind(1, guildId);
        if (guild.executeStep()) guildName = guild.getColumn(0).getString();
    }
    return !guildId.empty();
}

bool discordMarkChannelCheckedOnce(Db* db, const std::string& channelId,
                                  std::string* error) {
    if (error) error->clear();
    if (channelId.empty()) {
        if (error) *error = "channel ID is empty";
        return false;
    }
    try {
        auto lk = db->guard();
        SQLite::Statement stamp(db->raw(lk.token()),
            "INSERT INTO discord_channels(channel_id,enabled,created_at,last_scan_at) "
            "VALUES(?,0,?,?) ON CONFLICT(channel_id) DO UPDATE SET last_scan_at=excluded.last_scan_at");
        const std::string now = nowIsoUtc();
        stamp.bind(1, channelId);
        stamp.bind(2, now);
        stamp.bind(3, now);
        stamp.exec();
        return true;
    } catch (const std::exception& e) {
        if (error) *error = redactHttpUrls(e.what());
        return false;
    }
}

std::string discordChannelLastRead(Db* db, const std::string& channelId) {
    auto lk = db->guard();
    SQLite::Statement q(db->raw(lk.token()),
        "SELECT last_read_ts FROM discord_channels WHERE channel_id=?");
    q.bind(1, channelId);
    if (q.executeStep()) return q.getColumn(0).getString();
    return {};
}

std::string discordNextUnnamedChannel(Db* db, long long& afterRowId) {
    auto lk = db->guard();
    SQLite::Statement q(db->raw(lk.token()),
        "SELECT id,channel_id FROM discord_channels WHERE id>? "
        "AND (channel_name='' OR channel_name=channel_id OR NOT EXISTS "
        "(SELECT 1 FROM settings WHERE key='discord_layout_' || channel_id)) "
        "ORDER BY id LIMIT 1");
    q.bind(1, afterRowId);
    if (!q.executeStep()) return {};
    afterRowId = q.getColumn(0).getInt64();
    return q.getColumn(1).getString();
}

void discordSetGuildName(Db* db, const std::string& guildId,
                         const std::string& name) {
    auto lk = db->guard();
    SQLite::Statement up(db->raw(lk.token()),
        "UPDATE discord_guilds SET guild_name=? WHERE guild_id=?");
    up.bind(1, name);
    up.bind(2, guildId);
    up.exec();
}

void discordMarkChannelChecked(Db* db, const std::string& channelId) {
    if (channelId.empty()) return;
    auto lk = db->guard();
    SQLite::Statement up(db->raw(lk.token()),
        "UPDATE discord_channels SET last_scan_at=? WHERE channel_id=?");
    up.bind(1, nowIsoUtc());
    up.bind(2, channelId);
    up.exec();
}

} // namespace devhub
