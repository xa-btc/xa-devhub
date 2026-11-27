#include "GuiInternal.h"
#include "DiscordCommandHelp.h"

namespace devhub {

static void refreshInboxReviewCaches(App& a) {
    a.needInbox = a.needChannels = a.needNotifyCounters = true;
    a.needProjects = a.needItems = a.needSources = a.needWork = true;
    a.needCal = true;
}

static std::string inboxReviewSummary(const InboxReviewBatch& batch) {
    std::string result = std::to_string(batch.succeeded) +
        (batch.approve ? " approved" : " dismissed") + ", " +
        std::to_string(batch.skipped) + " skipped, " +
        std::to_string(batch.failed) + " failed";
    if (batch.attachmentsPending)
        result += "; " + std::to_string(batch.attachmentsPending) + " attachments queued";
    if (batch.attachmentsFailed)
        result += "; " + std::to_string(batch.attachmentsFailed) + " attachments failed";
    return result;
}

static void startInboxReview(App& a, bool approve) {
    if (a.inboxReview.active()) return;
    if (!ImGui::GetIO().KeyCtrl) {
        toast(a, "hold Ctrl while clicking to " +
            std::string(approve ? "approve assigned inbox requests" : "dismiss all inbox requests"));
        return;
    }
    try {
        std::vector<InboxReviewTarget> selections;
        for (const auto& m : a.inbox)
            selections.push_back({m.id, m.selProjectId,
                kPromoteTypeNames[std::clamp(m.selType, 0, 2)]});
        auto targets = snapshotInboxReviewTargets(a.db, approve, selections);
        if (targets.empty()) {
            toast(a, approve ? "no inbox requests with an active assigned project"
                             : "no pending inbox requests");
            a.needInbox = true;
            return;
        }
        a.inboxReview = {};
        a.inboxReview.approve = approve;
        a.inboxReview.targets = std::move(targets);
    } catch (const std::exception& e) {
        toast(a, "could not start inbox review: " + redactHttpUrls(e.what()), true);
    }
}

// Run from the main frame loop so changing pages cannot strand a batch.
// One request per frame keeps bulk review from blocking on the entire inbox.
void processDiscordInboxReview(App& a) {
    auto& batch = a.inboxReview;
    if (!batch.active()) return;
    const auto target = batch.targets[batch.next++];
    try {
        const auto result = reviewPendingInboxTarget(a.db, a.discord, target, batch.approve);
        if (result.value("skipped", false) || result.value("duplicate", false)) {
            ++batch.skipped;
        } else if (result.value("ok", false)) {
            ++batch.succeeded;
            batch.attachmentsFailed += result.value("attachments_failed", 0);
            batch.attachmentsPending += result.value("attachments_pending", 0);
        } else {
            ++batch.failed;
            if (batch.firstError.empty())
                batch.firstError = result.value("error", "request failed");
        }
    } catch (const std::exception& e) {
        ++batch.failed;
        if (batch.firstError.empty()) batch.firstError = redactHttpUrls(e.what());
    }
    const double now = ImGui::GetTime();
    if (!batch.active() || now >= batch.nextRefresh) {
        refreshInboxReviewCaches(a);
        batch.nextRefresh = now + 0.25;
    }
    if (!batch.active())
        toast(a, inboxReviewSummary(batch), batch.failed > 0 || batch.attachmentsFailed > 0);
}

static void dismissInboxRequest(App& a, long long id) {
    try {
        const auto result = dismissSuggestion(a.db, a.discord, id);
        toast(a, result.value("ok", false) ? "dismissed" :
            result.value("error", "dismiss failed"), !result.value("ok", false));
    } catch (const std::exception& e) {
        toast(a, "dismiss failed: " + redactHttpUrls(e.what()), true);
    }
    a.needInbox = a.needChannels = a.needNotifyCounters = true;
}

static void drawDiscordBotSection(App& a, const DiscordBot::Status& bs) {
    if (bs.state == "connected") {
        ImGui::TextColored(C_GREEN, "connected");
        ImGui::SameLine();
        ImGui::Text("as %s", bs.botUser.c_str());
        ImGui::SameLine();
        ImGui::TextColored(C_DIM, "- sees %d server(s)", bs.guilds);
    } else if (bs.state == "connecting") {
        ImGui::TextColored(C_ORANGE, "connecting...");
    } else {
        ImGui::TextColored(C_DIM, "offline");
    }
    ImGui::SameLine(0, 20);
    if (bs.state == "offline") {
        if (ImGui::SmallButton("Connect")) {
            std::string token;
            {
                auto lk = a.db->guard();
                token = a.db->getSetting(lk.token(), "discord_bot_token");
            }
            if (trim(token).empty()) toast(a, "save a bot token first", true);
            else { a.discord->start(token); toast(a, "connecting..."); }
        }
    } else {
        if (ImGui::SmallButton("Disconnect")) a.discord->stop();
        ImGui::SameLine();
        if (ImGui::SmallButton("Reconnect")) {
            std::string token;
            {
                auto lk = a.db->guard();
                token = a.db->getSetting(lk.token(), "discord_bot_token");
            }
            a.discord->start(token);
        }
    }
    if (!bs.error.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(C_RED, "%s", bs.error.c_str());
        ImGui::PopTextWrapPos();
        if (bs.error.find("4014") != std::string::npos ||
            bs.error.find("Disallowed") != std::string::npos) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(C_ORANGE,
                "Fix: discord.com/developers -> your application -> Bot -> "
                "Privileged Gateway Intents -> enable MESSAGE CONTENT INTENT "
                "-> Save. The bot retries automatically and will connect "
                "within seconds of the toggle being saved.");
            ImGui::PopTextWrapPos();
        }
        if (bs.error.find("4004") != std::string::npos ||
            bs.error.find("Authentication") != std::string::npos) {
            ImGui::TextColored(C_ORANGE,
                "Fix: the token is wrong or was reset - copy a fresh one from "
                "the developer portal and Save token + connect.");
        }
    }
    // Transient connection hiccups (dropped socket, rate limit) retry on
    // their own and this notice expires by itself - orange, not red.
    if (!bs.warn.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(C_ORANGE, "%s", bs.warn.c_str());
        ImGui::PopTextWrapPos();
        ImGui::TextColored(C_DIM,
            "transient connection hiccup during a history sweep - retried "
            "automatically, nothing to do");
    }
    if (bs.state != "connected" && bs.error.empty()) {
        ImGui::TextColored(C_DIM,
            "Checklist: bot invited to the server, and the MESSAGE CONTENT "
            "INTENT toggle enabled under Bot settings in the Discord developer "
            "portal (required - the gateway refuses the connection without it).");
    }

    bool autoAddChannels;
    {
        auto lk = a.db->guard();
        autoAddChannels =
            a.db->getSetting(lk.token(), "discord_auto_add_channels") != "0";
    }
    if (ImGui::Checkbox("Auto Add Channels", &autoAddChannels)) {
        auto lk = a.db->guard();
        a.db->setSetting(lk.token(), "discord_auto_add_channels",
                         autoAddChannels ? "1" : "0");
    }
    ImGui::TextWrapped(
        "Automatically log newly discovered channels and threads in monitored "
        "servers. When off, only existing enabled channel monitors are logged. "
        "Changes apply immediately; existing monitor choices are preserved.");

    ImGui::SetNextItemWidth(420);
    ImGui::InputText("bot token", a.dcToken, sizeof(a.dcToken),
                     a.dcShowToken ? 0 : ImGuiInputTextFlags_Password);
    ImGui::SameLine();
    ImGui::Checkbox("show", &a.dcShowToken);
    ImGui::SameLine();
    if (ImGui::Button("Save token + connect")) {
        {
            auto lk = a.db->guard();
            a.db->setSetting(lk.token(), "discord_bot_token", a.dcToken);
        }
        a.discord->start(a.dcToken);
        toast(a, "token saved - connecting");
    }

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(C_DIM,
        "Enter Discord user IDs, not usernames. Separate multiple IDs with "
        "commas. This global whitelist can approve/capture messages in watched "
        "channels; approve or reject pending notification cards; reply with an "
        "approval note; and use !tickets, !xatickets, !xahelp, !leaderboard, and menu "
        "reactions in every server the bot can read. Access Management uses this "
        "whitelist for matching upload pins. Clear and save to disable "
        "these privileged actions; changes apply without reconnecting.");
    ImGui::PopTextWrapPos();
    ImGui::SetNextItemWidth(420);
    if (a.dcAdminIdsValue.truncated) {
        ImGui::TextColored(
            C_ORANGE,
            "The saved whitelist is %zu bytes and cannot fit in this editor. "
            "It will be preserved unchanged.",
            a.dcAdminIdsValue.original.size());
        ImGui::BeginDisabled();
    }
    ImGui::InputTextWithHint("admin/developer whitelist",
                             "Discord user IDs, comma-separated",
                             a.dcAdminIds, sizeof(a.dcAdminIds));
    if (a.dcAdminIdsValue.truncated) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Save whitelist")) {
        std::vector<std::string> ids;
        std::string error;
        const std::string whitelist =
            preservedText(a.dcAdminIds, a.dcAdminIdsValue);
        if (!parseDiscordAdminUserIds(whitelist, ids, &error)) {
            toast(a, "whitelist not saved - " + error, true);
        } else {
            std::ostringstream normalized;
            for (size_t i = 0; i < ids.size(); ++i) {
                if (i) normalized << ", ";
                normalized << ids[i];
            }
            const std::string saved = normalized.str();
            {
                auto lk = a.db->guard();
                a.db->setSetting(lk.token(), "discord_admin_user_ids", saved);
            }
            loadPreservedText(a.dcAdminIds, sizeof(a.dcAdminIds), saved,
                              a.dcAdminIdsValue);
            toast(a, ids.empty()
                ? "whitelist cleared - privileged Discord actions disabled"
                : "admin/developer whitelist saved");
        }
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(C_DIM,
        "In a watched channel, a listed admin/developer can @mention the bot "
        "with a suggestion, or reply to another message and @mention it. New "
        "suggestions enter the normal approval inbox. If that exact replied-to "
        "message already belongs to a promoted ticket, its follow-up note and "
        "only new target/reply attachments are appended to the existing ticket. "
        "Files are never saved before the first Promote approval.");
    ImGui::PopTextWrapPos();
}

// Body of the collapsible "Monitors" section: add-monitor inputs plus the
// per-server channel groups (each group's open state persists too).
static void drawDiscordMonitors(App& a) {
    ImGui::TextColored(C_DIM,
        "Server ID alone = discover channels when Auto Add Channels is on. "
        "Add a channel or thread ID to monitor it individually. "
        "Use each channel's on checkbox to control monitoring. "
        "Discovered channels stay visible while off.");
    ImGui::SetNextItemWidth(190);
    ImGui::InputTextWithHint("##msrv", "server id", a.dcServerId, sizeof(a.dcServerId));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(190);
    ImGui::InputTextWithHint("##mchan", "channel id (optional)", a.dcChanId,
                             sizeof(a.dcChanId));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    ImGui::InputTextWithHint("##mname", "name (optional)", a.dcMonName,
                             sizeof(a.dcMonName));
    ImGui::SameLine();
    if (ImGui::Button("Add monitor")) {
        std::string srv = trim(a.dcServerId), chan = trim(a.dcChanId);
        if (!chan.empty()) {
            auto lk = a.db->guard();
            SQLite::Statement ins(a.db->raw(lk.token()),
                "INSERT INTO discord_channels(channel_id,guild_id,guild_name,channel_name,"
                "created_at) VALUES(?,?,?,?,?) ON CONFLICT(channel_id) DO NOTHING");
            ins.bind(1, chan);
            ins.bind(2, srv);
            ins.bind(3, "");
            ins.bind(4, a.dcMonName);
            ins.bind(5, nowIsoUtc());
            ins.exec();
            lk.unlock();
            a.needChannels = true;
            toast(a, "channel monitor added");
        } else if (!srv.empty()) {
            auto lk = a.db->guard();
            SQLite::Statement ins(a.db->raw(lk.token()),
                "INSERT INTO discord_guilds(guild_id,guild_name,created_at) "
                "VALUES(?,?,?) ON CONFLICT(guild_id) DO NOTHING");
            ins.bind(1, srv);
            ins.bind(2, a.dcMonName);
            ins.bind(3, nowIsoUtc());
            ins.exec();
            lk.unlock();
            a.needGuilds = true;
            toast(a, "server monitor added - discovery follows Auto Add Channels");
        } else {
            toast(a, "enter a server id (and optionally a channel id)", true);
        }
        a.dcServerId[0] = a.dcChanId[0] = a.dcMonName[0] = 0;
    }

    int listing = 0;
    {
        auto lk = a.db->guard();
        listing = a.db->getSetting(lk.token(), "discord_monitor_listing") == "discord" ? 1 : 0;
    }
    ImGui::SetNextItemWidth(180);
    if (ImGui::Combo("Channel listing", &listing, "A-Z\0Discord listing\0")) {
        auto lk = a.db->guard();
        a.db->setSetting(lk.token(), "discord_monitor_listing", listing ? "discord" : "az");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Discord listing groups known channels under categories in server order. "
                          "Layout refreshes when the bot connects and receives channel updates.");

    const bool connected = a.discord->status().state == "connected";
    auto canCheckChannel = [](const Channel& channel) {
        // Forum/category containers have no message history; check their thread rows.
        return !channel.layoutKnown || channel.type == 0 || channel.type == 2 ||
            channel.type == 5 || channel.type == 13 || channel.isThread;
    };
    ImGui::BeginDisabled(!connected);
    if (ImGui::Button("Check All (Ctrl+Click)")) {
        if (!ImGui::GetIO().KeyCtrl) {
            toast(a, "Hold Ctrl while clicking to check the latest 100 messages per channel.");
        } else {
            std::set<std::string> recent;
            {
                auto lk = a.db->guard();
                SQLite::Statement checked(a.db->raw(lk.token()),
                    "SELECT channel_id FROM discord_channels "
                    "WHERE julianday(last_scan_at) >= julianday('now','-5 minutes')");
                while (checked.executeStep()) recent.insert(checked.getColumn(0).getString());
            }
            std::set<std::string> seen;
            int queued = 0, skipped = 0;
            for (const auto* channels : {&a.channels, &a.discoveredChannels}) {
                for (const auto& channel : *channels) {
                    if (!canCheckChannel(channel) || !seen.insert(channel.channelId).second) continue;
                    if (recent.count(channel.channelId) ||
                        a.discord->channelCheckStatus(channel.channelId).pending) {
                        ++skipped;
                        continue;
                    }
                    if (a.discord->checkChannelNow(channel.channelId)) ++queued;
                    else ++skipped;
                }
            }
            toast(a, "Queued " + std::to_string(queued) + " channel checks; " +
                     std::to_string(skipped) + " recent, pending, or unavailable channels skipped.");
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Hold Ctrl and click to collect the latest 100 messages from each known message channel\nnot checked in the last 5 minutes, including disabled channels. Monitoring settings stay unchanged.\nForum posts are checked individually; forum containers have no message history.");
    ImGui::SameLine();
    ImGui::TextColored(C_DIM, "Latest 100 each; skips checks within 5 minutes");

    auto drawChannelTable = [&](std::vector<Channel*>& list) {
        if (list.empty()) {
            ImGui::TextColored(C_DIM,
                "No channels discovered yet. Use Check All Channels while connected.");
            return;
        }
        if (!ImGui::BeginTable("channels", 8,
                               ImGuiTableFlags_RowBg |
                                   ImGuiTableFlags_BordersInnerH |
                                   ImGuiTableFlags_SizingStretchProp))
            return;
        ImGui::TableSetupColumn("channel", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("project", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("msgs", ImGuiTableColumnFlags_WidthStretch, 0.5f);
        ImGui::TableSetupColumn("pending", ImGuiTableColumnFlags_WidthStretch, 0.6f);
        // last read = newest message the bot has ingested;
        // last checked = the last time the bot actually looked at the channel.
        ImGui::TableSetupColumn("last read", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("last checked", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("on", ImGuiTableColumnFlags_WidthStretch, 0.4f);
        ImGui::TableSetupColumn("actions", ImGuiTableColumnFlags_WidthFixed, 160.0f);
        ImGui::TableHeadersRow();
        for (Channel* pc : list) {
            Channel& c = *pc;
            ImGui::PushID(c.channelId.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const char* prefix = c.isThread ? "  > " : c.type == 15 ? "[forum] " :
                c.type == 16 ? "[media] " : c.type == 2 || c.type == 13 ? "[voice] " : "#";
            ImGui::TextColored(c.enabled ? C_TEXT : C_DIM, "%s%s", prefix,
                               c.name.empty() ? c.channelId.c_str() : c.name.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("Category: %s", c.categoryName.c_str());
                if (!c.parentName.empty()) ImGui::Text("Parent channel / forum: %s", c.parentName.c_str());
                ImGui::Text("ID: %s", c.channelId.c_str());
                ImGui::TextUnformatted(c.enabled ? "Monitoring enabled" : "Not monitored");
                if (c.type == 15 || c.type == 16)
                    ImGui::TextUnformatted("With Auto Add Channels on, monitoring this forum adds new threads.\nWith Auto Add Channels off, new threads remain visible without monitoring.");
                if (!c.layoutKnown) ImGui::TextUnformatted("Layout not received from Discord yet.");
                ImGui::EndTooltip();
            }
            ImGui::TableNextColumn();
            int idx = -1;
            for (int i = 0; i < (int)a.projects.size(); ++i)
                if (a.projects[i].id == c.projectId) idx = i;
            ImGui::BeginDisabled(c.id == 0);
            if (projectCombo(a, "##chp", &idx, -1)) {
                auto lk = a.db->guard();
                SQLite::Statement up(a.db->raw(lk.token()),
                    "UPDATE discord_channels SET project_id=? WHERE id=?");
                up.bind(1, a.projects[idx].id);
                up.bind(2, c.id);
                up.exec();
                lk.unlock();
                a.needChannels = true;
                a.needInbox = true;
            }
            ImGui::EndDisabled();
            ImGui::TableNextColumn();
            ImGui::Text("%lld", c.msgs);
            ImGui::TableNextColumn();
            if (c.pending > 0) ImGui::TextColored(C_ORANGE, "%lld", c.pending);
            else ImGui::TextColored(C_DIM, "0");
            ImGui::TableNextColumn();
            ImGui::TextColored(C_DIM, "%s",
                               c.lastRead.empty() ? "never" : shortTs(c.lastRead).c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("timestamp of the newest message ingested");
            ImGui::TableNextColumn();
            ImGui::TextColored(C_DIM, "%s",
                               c.lastScan.empty() ? "never" : shortTs(c.lastScan).c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("last time the bot checked this channel "
                                  "(connect backfill, live event, or the "
                                  "5-minute re-scan)");
            ImGui::TableNextColumn();
            bool on = c.enabled != 0;
            const bool supportsMonitoring = c.id != 0 || !c.layoutKnown || c.type == 0 ||
                c.type == 5 || c.type == 15 || c.type == 16 || c.isThread;
            ImGui::BeginDisabled(!supportsMonitoring);
            if (ImGui::Checkbox("##on", &on)) {
                auto lk = a.db->guard();
                SQLite::Statement up(a.db->raw(lk.token()),
                    "INSERT INTO discord_channels(channel_id,guild_id,guild_name,channel_name,enabled,created_at) "
                    "VALUES(?,?,?,?,?,?) ON CONFLICT(channel_id) DO UPDATE SET enabled=excluded.enabled");
                up.bind(1, c.channelId);
                up.bind(2, c.guildId);
                up.bind(3, c.guild);
                up.bind(4, c.name);
                up.bind(5, on ? 1 : 0);
                up.bind(6, nowIsoUtc());
                up.exec();
                lk.unlock();
                a.needChannels = true;
            }
            ImGui::EndDisabled();
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(c.id == 0);
            if (ImGui::SmallButton("del")) {
                bool deleted = false;
                bool hasNotifyCard = false;
                bool hasDeletedSourceMarker = false;
                auto lk = a.db->guard();
                SQLite::Statement lineage(a.db->raw(lk.token()),
                    "SELECT EXISTS(SELECT 1 FROM discord_notify_cards n "
                    "JOIN discord_messages m ON m.id=n.discord_message_row_id "
                    "WHERE m.channel_row_id=?),"
                    "EXISTS(SELECT 1 FROM discord_messages m "
                    "WHERE m.channel_row_id=? AND m.state='deleted')");
                lineage.bind(1, c.id);
                lineage.bind(2, c.id);
                lineage.executeStep();
                hasNotifyCard = lineage.getColumn(0).getInt() != 0;
                hasDeletedSourceMarker = lineage.getColumn(1).getInt() != 0;
                if (!hasNotifyCard && !hasDeletedSourceMarker) {
                    SQLite::Statement del(a.db->raw(lk.token()),
                        "DELETE FROM discord_channels WHERE id=?");
                    del.bind(1, c.id);
                    deleted = del.exec() == 1;
                }
                lk.unlock();
                if (hasDeletedSourceMarker) {
                    toast(a,
                        "channel contains retained Discord deletion markers; disable it instead",
                        true);
                } else if (hasNotifyCard) {
                    toast(a,
                        "channel owns Discord notification cards; disable it instead",
                        true);
                } else if (deleted) {
                    a.needChannels = true;
                    a.needInbox = true;
                } else {
                    toast(a, "channel monitor was not deleted", true);
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            const auto check = a.discord->channelCheckStatus(c.channelId);
            ImGui::BeginDisabled(!connected || !canCheckChannel(c) || check.pending);
            if (ImGui::SmallButton(check.pending ? "Checking..." : "Check Now")) {
                if (!a.discord->checkChannelNow(c.channelId))
                    toast(a, "Channel check could not start. Check the bot connection.", true);
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::BeginTooltip();
                if (!canCheckChannel(c))
                    ImGui::TextUnformatted("Check the individual threads inside this forum.");
                else if (!connected) ImGui::TextUnformatted("Connect the bot to check messages.");
                else ImGui::TextUnformatted("Collect the latest 100 messages once, even if monitoring is off.\nMonitoring settings and the background history position stay unchanged.");
                if (!check.message.empty()) ImGui::TextUnformatted(check.message.c_str());
                ImGui::EndTooltip();
            }
            if (check.failed) {
                ImGui::SameLine();
                ImGui::TextColored(C_ORANGE, "!");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", check.message.c_str());
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    };

    auto snowflakeLess = [](const std::string& x, const std::string& y) {
        return x.size() != y.size() ? x.size() < y.size() : x < y;
    };
    auto drawListing = [&](const std::string& guildId, std::vector<Channel*>& list) {
        if (!listing) {
            std::sort(list.begin(), list.end(), [&](const Channel* x, const Channel* y) {
                const auto xn = toLower(x->name.empty() ? x->channelId : x->name);
                const auto yn = toLower(y->name.empty() ? y->channelId : y->name);
                return xn != yn ? xn < yn : snowflakeLess(x->channelId, y->channelId);
            });
            drawChannelTable(list);
            return;
        }
        std::sort(list.begin(), list.end(), [&](const Channel* x, const Channel* y) {
            // Discord sorts voice/stage after text-like channels, then position and ID.
            const bool xv = x->orderType == 2 || x->orderType == 13;
            const bool yv = y->orderType == 2 || y->orderType == 13;
            if (xv != yv) return xv < yv;
            if (x->position != y->position) return x->position < y->position;
            if (x->orderId != y->orderId) return snowflakeLess(x->orderId, y->orderId);
            if (x->isThread != y->isThread) return !x->isThread;
            return snowflakeLess(x->channelId, y->channelId);
        });
        std::vector<const DiscordChannelLayout*> categories;
        for (const auto& [id, meta] : a.discordLayout)
            if (meta.guildId == guildId && meta.type == 4) categories.push_back(&meta);
        std::sort(categories.begin(), categories.end(), [&](const auto* x, const auto* y) {
            return x->position != y->position ? x->position < y->position : snowflakeLess(x->id, y->id);
        });
        auto drawCategory = [&](const std::string& id, const std::string& name, bool showEmpty) {
            std::vector<Channel*> children;
            for (auto* channel : list)
                if (channel->categoryId == id) children.push_back(channel);
            if (children.empty() && !showEmpty) return;
            ImGui::PushID(id.c_str());
            const std::string label = name + " (" + std::to_string(children.size()) + ")###category";
            if (sectionHeader(a, label.c_str(), "discord_category_" + guildId + "_" + id, true)) {
                if (children.empty()) ImGui::TextColored(C_DIM, "No known channels in this category.");
                else drawChannelTable(children);
            }
            ImGui::PopID();
        };
        drawCategory("", "Uncategorized", false);
        for (const auto* category : categories) drawCategory(category->id, category->name, true);
        drawCategory("?", "Unknown category", false);
        if (categories.empty() && list.empty()) drawChannelTable(list);
    };

    auto drawRefresh = [&](const std::string& guildId) {
        if (guildId.empty()) return;
        const auto refresh = a.discord->guildChannelRefreshStatus(guildId);
        const bool connected = a.discord->status().state == "connected";
        ImGui::BeginDisabled(!connected || refresh.pending);
        if (ImGui::Button(refresh.pending ? "Checking channels..." : "Check All Channels")) {
            if (!a.discord->refreshGuildChannels(guildId))
                toast(a, "Could not start channel refresh. Check the bot connection.", true);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Refresh this server's channels, forums, and active threads.\nExisting monitoring choices stay unchanged; discovered channels remain off.");
        ImGui::SameLine();
        ImGui::TextColored(C_DIM, "Refresh listing only");
        if (!connected) ImGui::TextColored(C_DIM, "Connect the bot to refresh channels.");
        if (!refresh.message.empty())
            ImGui::TextColored(refresh.failed ? C_ORANGE : C_DIM, "%s", refresh.message.c_str());
    };

    std::map<std::string, std::vector<Channel*>> byGuild;
    for (auto& c : a.channels) byGuild[c.guildId].push_back(&c);
    for (auto& c : a.discoveredChannels) byGuild[c.guildId].push_back(&c);
    std::set<std::string> shownGuilds;

    for (auto& g : a.guilds) {
        ImGui::PushID((int)(g.id + 100000));
        auto& chans = byGuild[g.guildId];
        shownGuilds.insert(g.guildId);
        char hdr[320];
        std::snprintf(hdr, sizeof(hdr), "%s  -  %s  (%d channel%s)###guildhdr",
                      g.name.empty() ? "(server - name appears on connect)"
                                     : g.name.c_str(),
                      g.guildId.c_str(), (int)chans.size(),
                      chans.size() == 1 ? "" : "s");
        bool open = sectionHeader(a, hdr, "guild_" + g.guildId, true,
                                  ImGuiTreeNodeFlags_AllowOverlap);
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 110);
        bool on = g.enabled != 0;
        if (ImGui::Checkbox("on##gon", &on)) {
            auto lk = a.db->guard();
            SQLite::Statement up(a.db->raw(lk.token()),
                "UPDATE discord_guilds SET enabled=? WHERE id=?");
            up.bind(1, on ? 1 : 0);
            up.bind(2, g.id);
            up.exec();
            lk.unlock();
            a.needGuilds = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("del")) {
            auto lk = a.db->guard();
            SQLite::Statement del(a.db->raw(lk.token()),
                "DELETE FROM discord_guilds WHERE id=?");
            del.bind(1, g.id);
            del.exec();
            lk.unlock();
            a.needGuilds = true;
        }
        if (open) {
            drawRefresh(g.guildId);
            drawListing(g.guildId, chans);
        }
        ImGui::PopID();
    }

    // Channel monitors that live outside any server-wide monitor.
    for (auto& [guildId, list] : byGuild) {
        if (shownGuilds.count(guildId)) continue;
        const std::string gname = list.empty() ? guildId : list.front()->guild;
        ImGui::PushID(guildId.empty() ? "#nogld" : guildId.c_str());
        char hdr[320];
        std::snprintf(hdr, sizeof(hdr), "%s  (%d channel%s)###chanhdr",
                      gname.empty() ? "Individual channel monitors"
                                    : (gname + "  -  channel monitors").c_str(),
                      (int)list.size(), list.size() == 1 ? "" : "s");
        if (sectionHeader(a, hdr,
                          "changrp_" + (guildId.empty() ? std::string("none") : guildId),
                          true)) {
            drawRefresh(guildId);
            drawListing(guildId, list);
        }
        ImGui::PopID();
    }
}

static std::string discordAccessChannelLabel(const Channel& channel) {
    std::string label = channel.guild.empty() ? channel.guildId : channel.guild;
    if (!label.empty()) label += " / ";
    label += channel.categoryName + " / ";
    if (!channel.parentName.empty()) label += channel.parentName + " / ";
    label += "#" + (channel.name.empty() ? channel.channelId : channel.name);
    return label + " (" + channel.channelId + ")";
}

static std::string discordAccessChannelLabel(const App& a,
                                             const std::string& channelId) {
    for (const auto* list : {&a.channels, &a.discoveredChannels})
        for (const auto& channel : *list)
            if (channel.channelId == channelId) return discordAccessChannelLabel(channel);
    return channelId;
}

static void drawDiscordAccessChannelPicker(App& a) {
    using CategoryChannels = std::map<std::string, std::vector<const Channel*>>;
    std::map<std::string, CategoryChannels> servers;
    std::map<std::string, std::string> serverNames;
    std::set<std::string> seen;
    for (const auto* list : {&a.channels, &a.discoveredChannels}) {
        for (const auto& channel : *list) {
            std::uint64_t parsed = 0;
            if (channel.type == 4 || !parseDiscordAccessSnowflake(channel.channelId, parsed) ||
                !seen.insert(channel.channelId).second) continue;
            servers[channel.guildId][channel.categoryId].push_back(&channel);
            if (!channel.guild.empty()) serverNames[channel.guildId] = channel.guild;
        }
    }
    for (const auto& guild : a.guilds)
        if (!guild.name.empty()) serverNames[guild.guildId] = guild.name;
    if (servers.empty()) ImGui::TextDisabled("No known channels yet. Use Check All Channels in Monitors.");
    const bool appearing = ImGui::IsWindowAppearing();
    auto idLess = [](const std::string& x, const std::string& y) {
        return x.size() != y.size() ? x.size() < y.size() : x < y;
    };
    for (auto& [guildId, categories] : servers) {
        ImGui::PushID(guildId.c_str());
        const std::string serverName = serverNames.count(guildId) ? serverNames.at(guildId) :
            guildId.empty() ? "Unknown server" : guildId;
        ImGui::SeparatorText(serverName.c_str());
        std::vector<std::string> categoryIds;
        for (const auto& [id, channels] : categories) categoryIds.push_back(id);
        std::sort(categoryIds.begin(), categoryIds.end(), [&](const auto& x, const auto& y) {
            // Uncategorised channels precede Discord categories; unknown metadata follows them.
            if (x.empty() != y.empty()) return x.empty();
            if ((x == "?") != (y == "?")) return y == "?";
            const auto xm = a.discordLayout.find(x), ym = a.discordLayout.find(y);
            const int xp = xm == a.discordLayout.end() ? 0 : xm->second.position;
            const int yp = ym == a.discordLayout.end() ? 0 : ym->second.position;
            return xp != yp ? xp < yp : idLess(x, y);
        });
        for (const auto& categoryId : categoryIds) {
            auto& channels = categories.at(categoryId);
            std::sort(channels.begin(), channels.end(), [&](const Channel* x, const Channel* y) {
                const bool xv = x->orderType == 2 || x->orderType == 13;
                const bool yv = y->orderType == 2 || y->orderType == 13;
                if (xv != yv) return xv < yv;
                if (x->position != y->position) return x->position < y->position;
                if (x->orderId != y->orderId) return idLess(x->orderId, y->orderId);
                if (x->isThread != y->isThread) return !x->isThread;
                return idLess(x->channelId, y->channelId);
            });
            ImGui::PushID(categoryId.c_str());
            if (appearing && std::any_of(channels.begin(), channels.end(), [&](const Channel* channel) {
                    return channel->channelId == a.dcAccessChannel;
                })) ImGui::SetNextItemOpen(true);
            const std::string header = channels.front()->categoryName + "###category";
            if (ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                for (const auto* channel : channels) {
                    ImGui::PushID(channel->channelId.c_str());
                    const bool forum = channel->type == 15 || channel->type == 16;
                    const std::string name = channel->name.empty() ? channel->channelId : channel->name;
                    if (forum) {
                        ImGui::TextColored(C_DIM, "[forum] %s", name.c_str());
                    } else {
                        const std::string label = channel->isThread
                            ? "    " + channel->parentName + " / " + name
                            : (channel->type == 2 || channel->type == 13 ? "[voice] " : "#") + name;
                        const bool selected = channel->channelId == a.dcAccessChannel;
                        if (ImGui::Selectable((label + "###channel").c_str(), selected))
                            copyBuf(a.dcAccessChannel, sizeof(a.dcAccessChannel), channel->channelId);
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted(discordAccessChannelLabel(*channel).c_str());
                        if (forum) ImGui::TextUnformatted("Select a thread inside this forum for an access rule.");
                        ImGui::EndTooltip();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::PopID();
        }
        ImGui::PopID();
    }
}

static void clearDiscordAccessEditor(App& a) {
    a.dcAccessChannel[0] = a.dcAccessPrefix[0] = a.dcAccessWebhook[0] = 0;
    a.dcAccessPrefixValue = {};
    a.dcAccessEditRevision = 0;
    a.dcAccessEnabled = false;
}

static void drawDiscordAccessManagement(App& a) {
    const bool interacting = ImGui::IsAnyItemActive() ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                    ImGuiPopupFlags_AnyPopupLevel);
    const double now = ImGui::GetTime();
    if (!a.dcAccessLoaded || (!interacting && now >= a.dcAccessRefreshAt)) {
        a.dcAccessRules = discordAccessRules(a.db, &a.dcAccessLoadError);
        a.dcAccessLoaded = true;
        a.dcAccessRefreshAt = now + 5.0;
    }

    ImGui::TextWrapped(
        "Keep the latest matching ZIP upload pinned in each selected channel. "
        "Uploads by the Bot section's admin/developer whitelist qualify. "
        "Optionally trust one exact webhook ID for that channel's release uploads. "
        "The new message is pinned before older matching upload pins are removed; "
        "other pins are kept.");
    ImGui::TextWrapped(
        "Monitoring begins when a rule is enabled and catches up on uploads "
        "missed while DevHub is offline. Changing the prefix or trusted webhook, or re-enabling a "
        "rule starts a new watch period. Earlier uploads do not trigger updates.");
    ImGui::TextWrapped(
        "Bot permissions: View Channel, Read Message History, Pin Messages.");
    if (a.discord->status().state != "connected")
        ImGui::TextColored(C_ORANGE,
            "Pin updates will resume when the bot reconnects.");

    std::string configuredAdmins;
    {
        auto lk = a.db->guard();
        configuredAdmins = a.db->getSetting(lk.token(), "discord_admin_user_ids");
    }
    std::vector<std::string> admins;
    if (!parseDiscordAdminUserIds(configuredAdmins, admins) || admins.empty()) {
        ImGui::TextColored(C_ORANGE,
            "Human uploads need the Bot admin/developer whitelist; webhooks need an exact trusted ID below.");
    }
    if (!a.dcAccessLoadError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, C_RED);
        ImGui::TextWrapped("Access rules could not be loaded: %s",
                           a.dcAccessLoadError.c_str());
        ImGui::PopStyleColor();
    }

    const bool editing = a.dcAccessEditRevision != 0;
    ImGui::SeparatorText(editing ? "Edit access rule" : "Add access rule");
    ImGui::BeginDisabled(!a.dcAccessLoadError.empty());
    ImGui::BeginDisabled(editing);
    const std::string selectedChannel = a.dcAccessChannel[0]
        ? discordAccessChannelLabel(a, a.dcAccessChannel)
        : "Choose a known channel";
    ImGui::SetNextItemWidth(480);
    if (ImGui::BeginCombo("channel##access", selectedChannel.c_str(), ImGuiComboFlags_HeightLarge)) {
        drawDiscordAccessChannelPicker(a);
        ImGui::EndCombo();
    }
    if (a.dcAccessChannel[0] && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", selectedChannel.c_str());
    ImGui::SetNextItemWidth(260);
    ImGui::InputTextWithHint("channel ID##access", "or enter an exact channel ID",
                            a.dcAccessChannel, sizeof(a.dcAccessChannel));
    ImGui::EndDisabled();
    if (a.dcAccessPrefixValue.truncated) {
        ImGui::TextColored(C_ORANGE,
            "This saved prefix is too long for the editor and will be preserved.");
    }
    ImGui::BeginDisabled(a.dcAccessPrefixValue.truncated);
    ImGui::SetNextItemWidth(360);
    ImGui::InputTextWithHint("filename prefix##access", "XA-Zod-v",
                            a.dcAccessPrefix, sizeof(a.dcAccessPrefix));
    ImGui::EndDisabled();
    ImGui::TextColored(C_DIM,
        "Literal prefix, ignoring letter case; ZIP attachments only. One rule per channel.");
    ImGui::SetNextItemWidth(260);
    ImGui::InputTextWithHint("trusted webhook ID##access", "optional; ID only, not its URL",
                            a.dcAccessWebhook, sizeof(a.dcAccessWebhook));
    ImGui::TextWrapped(
        "Leave blank for human admins only. Only this exact webhook in this channel "
        "can qualify; its name does not grant access. Changing it starts a new watch period.");
    ImGui::Checkbox("Enabled##access_editor", &a.dcAccessEnabled);
    if (ImGui::Button(editing ? "Save rule##access" : "Add rule##access")) {
        std::string error;
        if (saveDiscordAccessRule(a.db, trim(a.dcAccessChannel),
                preservedText(a.dcAccessPrefix, a.dcAccessPrefixValue),
                a.dcAccessEnabled, a.dcAccessEditRevision, &error,
                trim(a.dcAccessWebhook))) {
            clearDiscordAccessEditor(a);
            a.dcAccessLoaded = false;
            toast(a, editing ? "access rule saved" : "access rule added");
        } else {
            toast(a, "access rule not saved - " + error, true);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(editing ? "Cancel edit##access" : "Clear##access"))
        clearDiscordAccessEditor(a);
    ImGui::EndDisabled();

    ImGui::SeparatorText("Channel rules");
    if (a.dcAccessRules.empty()) {
        ImGui::TextColored(C_DIM, "No access rules configured.");
        return;
    }
    for (const auto& rule : a.dcAccessRules) {
        ImGui::PushID(rule.channelId.c_str());
        ImGui::BeginChild("access_rule", ImVec2(0, 0),
                         ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
        const std::string channelLabel = discordAccessChannelLabel(a, rule.channelId);
        ImGui::TextWrapped("%s", channelLabel.c_str());
        ImGui::TextWrapped("ZIP prefix: %s", rule.filenamePrefix.c_str());
        if (!rule.trustedWebhookId.empty())
            ImGui::Text("Trusted webhook ID: %s", rule.trustedWebhookId.c_str());
        const bool isBeingEdited = editing && rule.channelId == a.dcAccessChannel;
        bool enabled = rule.enabled;
        ImGui::BeginDisabled(isBeingEdited || !a.dcAccessLoadError.empty());
        if (ImGui::Checkbox("Enabled", &enabled)) {
            std::string error;
            if (saveDiscordAccessRule(a.db, rule.channelId, rule.filenamePrefix,
                                      enabled, rule.revision, &error,
                                      rule.trustedWebhookId)) {
                a.dcAccessLoaded = false;
                toast(a, enabled ? "access monitoring enabled"
                                 : "access monitoring disabled - pins retained");
            } else {
                toast(a, "access rule not changed - " + error, true);
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("edit")) {
            copyBuf(a.dcAccessChannel, sizeof(a.dcAccessChannel), rule.channelId);
            loadPreservedText(a.dcAccessPrefix, sizeof(a.dcAccessPrefix),
                              rule.filenamePrefix, a.dcAccessPrefixValue);
            copyBuf(a.dcAccessWebhook, sizeof(a.dcAccessWebhook), rule.trustedWebhookId);
            a.dcAccessEnabled = rule.enabled;
            a.dcAccessEditRevision = rule.revision;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!rule.enabled ||
            (rule.status != "failed" && rule.status != "retry"));
        if (ImGui::SmallButton("retry")) {
            std::string error;
            if (retryDiscordAccessRule(a.db, rule.channelId, rule.revision, &error)) {
                a.dcAccessLoaded = false;
                toast(a, "access pin update queued for retry");
            } else {
                toast(a, "access retry not queued - " + error, true);
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("remove")) {
            std::string error;
            if (removeDiscordAccessRule(a.db, rule.channelId, rule.revision, &error)) {
                a.dcAccessLoaded = false;
                toast(a, "access rule removed - pins retained");
            } else {
                toast(a, "access rule not removed - " + error, true);
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Stop watching this channel and keep its current pins.");
        ImGui::EndDisabled();

        const char* state = !rule.enabled ? "disabled"
            : !rule.lastScanError.empty() ? "upload check needs attention"
            : rule.status == "synced" ? "up to date"
            : rule.status == "pending" ? "pin update pending"
            : rule.status == "retry" ? "retry scheduled"
            : rule.status == "failed" ? "needs attention"
            : "waiting for a matching upload";
        const ImVec4 color = !rule.enabled ? C_DIM
            : !rule.lastScanError.empty() ? C_ORANGE
            : rule.status == "synced" ? C_GREEN
            : rule.status == "failed" ? C_RED
            : rule.status == "retry" ? C_ORANGE : C_DIM;
        ImGui::TextColored(color, "%s", state);
        if (!rule.latestFilename.empty())
            ImGui::TextWrapped("Latest match: %s", rule.latestFilename.c_str());
        if (!rule.lastSuccessAt.empty())
            ImGui::TextColored(C_DIM, "Last pin update: %s",
                               shortTs(rule.lastSuccessAt).c_str());
        if (!rule.nextRetryAt.empty() && rule.enabled && rule.status == "retry")
            ImGui::TextColored(C_DIM, "Next retry: %s",
                               shortTs(rule.nextRetryAt).c_str());
        if (!rule.lastError.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, C_ORANGE);
            ImGui::TextWrapped("%s", rule.lastError.c_str());
            ImGui::PopStyleColor();
        }
        if (!rule.lastScanError.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, C_ORANGE);
            ImGui::TextWrapped("Upload check: %s", rule.lastScanError.c_str());
            ImGui::PopStyleColor();
        }
        if (!rule.latestGuildId.empty() && !rule.latestMessageId.empty()) {
            if (ImGui::SmallButton("open latest upload"))
                openUrl("https://discord.com/channels/" + rule.latestGuildId +
                        "/" + rule.channelId + "/" + rule.latestMessageId);
        }
        ImGui::EndChild();
        ImGui::PopID();
    }
}

void drawDiscord(App& a) {
    drawReleaseAnnouncements(a);
    // one-time buffer loads
    if (!a.dcLoaded) {
        auto lk = a.db->guard();
        copyBuf(a.dcToken, sizeof(a.dcToken),
                a.db->getSetting(lk.token(), "discord_bot_token"));
        loadPreservedText(a.dcAdminIds, sizeof(a.dcAdminIds),
                          a.db->getSetting(lk.token(), "discord_admin_user_ids"),
                          a.dcAdminIdsValue);
        a.dcLoaded = true;
    }
    if (!a.dcPatternsLoaded) {
        SuggestionDetector d;
        {
            auto lk = a.db->guard();
            d = makeDetectorLocked(lk.token(), a.db);
        }
        copyBuf(a.dcPatterns, sizeof(a.dcPatterns),
                SuggestionDetector::patternsToJson(d.patterns()).dump(1));
        a.dcPatternsLoaded = true;
    }

    // Bot: collapsible; the connection summary stays visible in the header.
    DiscordBot::Status bs = a.discord->status();
    std::string botHdr = "Bot  -  ";
    if (bs.state == "connected")
        botHdr += "connected as " + bs.botUser + " (" +
                  std::to_string(bs.guilds) +
                  (bs.guilds == 1 ? " server)" : " servers)");
    else
        botHdr += bs.state;
    if (sectionHeader(a, (botHdr + "###secbot").c_str(), "discord_bot", true))
        drawDiscordBotSection(a, bs);

    // Commands are collapsed initially; sectionHeader persists the choice.
    if (sectionHeader(a, "Commands", "discord_commands", false)) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(kDiscordCommandHelpAccess);
        for (const auto& entry : kDiscordCommandHelp) {
            ImGui::Spacing();
            ImGui::TextColored(C_GREEN, "%s", entry.command);
            ImGui::TextUnformatted(entry.description);
        }
        ImGui::Spacing();
        ImGui::TextUnformatted(kDiscordCommandHelpMention);
        ImGui::PopTextWrapPos();
    }

    // ---- bot notifications -------------------------------------------------
    if (sectionHeader(a, "Bot notifications", "discord_notify", false)) {
        if (!a.dcNotifyLoaded) {
            auto lk = a.db->guard();
            copyBuf(a.dcNotifyGuild, sizeof(a.dcNotifyGuild),
                    a.db->getSetting(lk.token(), "notify_guild_id"));
            copyBuf(a.dcNotifyChan, sizeof(a.dcNotifyChan),
                    a.db->getSetting(lk.token(), "notify_channel_id"));
            a.dcNotifyLoaded = true;
        }
        ImGui::TextColored(C_DIM,
            "The bot posts a card into this channel for every detected "
            "suggestion/bug, and keeps editing the SAME card as you handle "
            "it: promoted (project + type), completed, dismissed, or deleted. "
            "Whitelisted admins can react with the check mark to approve, react "
            "with X to reject, or reply with a non-empty note to approve and "
            "carry that note into the ticket. Approval waits for a mapped or "
            "inferred project; controls are restored after reconnect. "
            "It only ever edits its own cards - users' messages are never "
            "touched. Leave the channel id blank to disable.");
        ImGui::SetNextItemWidth(190);
        ImGui::InputTextWithHint("##ng", "server id", a.dcNotifyGuild,
                                 sizeof(a.dcNotifyGuild));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(190);
        ImGui::InputTextWithHint("##nc", "channel id", a.dcNotifyChan,
                                 sizeof(a.dcNotifyChan));
        ImGui::SameLine();
        auto saveNotify = [&]() {
            auto lk = a.db->guard();
            a.db->setSetting(lk.token(), "notify_guild_id", trim(a.dcNotifyGuild));
            a.db->setSetting(lk.token(), "notify_channel_id", trim(a.dcNotifyChan));
        };
        if (ImGui::Button("Save##notify")) {
            saveNotify();
            toast(a, trim(a.dcNotifyChan).empty()
                          ? "bot notifications disabled"
                          : "notify channel saved");
        }
        ImGui::SameLine();
        if (ImGui::Button("Save + send test")) {
            saveNotify();
            if (trim(a.dcNotifyChan).empty()) {
                toast(a, "enter the channel id first", true);
            } else if (a.discord->status().state != "connected") {
                toast(a, "bot is not connected - connect it first", true);
            } else {
                sendNotifyTest(a.db, a.discord);
                toast(a, "test card sent - check the channel");
            }
        }

        bool reloadNotifyCounters = a.needNotifyCounters;
        int notifyDatabaseChanges = 0;
        int notifyDatabaseVersion = 0;
        if (reloadNotifyCounters) {
            auto lk = a.db->guard();
            // The bot/API share this connection: data_version alone would
            // miss their writes. total_changes is an in-memory counter;
            // data_version additionally catches commits on other connections.
            notifyDatabaseChanges = a.db->raw(lk.token()).getTotalChanges();
            SQLite::Statement version(a.db->raw(lk.token()), "PRAGMA data_version");
            version.executeStep();
            notifyDatabaseVersion = version.getColumn(0).getInt();
            reloadNotifyCounters = !a.notifyFailures.loaded ||
                notifyDatabaseChanges != a.notifyFailures.databaseChanges ||
                notifyDatabaseVersion != a.notifyFailures.databaseVersion;
            if (!reloadNotifyCounters) a.needNotifyCounters = false;
        }
        if (reloadNotifyCounters) {
            a.notifyFailures = {};
            auto lk = a.db->guard();
            SQLite::Statement total(a.db->raw(lk.token()), R"sql(
WITH failures AS (
 SELECT id,'create' AS operation,post_revision AS revision
   FROM discord_notify_cards WHERE post_state='failed'
 UNION ALL
 SELECT id,'edit',edit_revision
   FROM discord_notify_cards WHERE edit_state='failed'
)
SELECT COUNT(*) FROM failures f
 WHERE NOT EXISTS(
   SELECT 1 FROM discord_notify_failure_dismissals d
    WHERE d.card_row_id=f.id AND d.operation=f.operation
      AND d.revision=f.revision))sql");
            total.executeStep();
            a.notifyFailures.activeTotal = total.getColumn(0).getInt();

            SQLite::Statement dismissedTotal(a.db->raw(lk.token()), R"sql(
WITH failures AS (
 SELECT id,'create' AS operation,post_revision AS revision
   FROM discord_notify_cards WHERE post_state='failed'
 UNION ALL
 SELECT id,'edit',edit_revision
   FROM discord_notify_cards WHERE edit_state='failed'
)
SELECT COUNT(*) FROM failures f
 WHERE EXISTS(
   SELECT 1 FROM discord_notify_failure_dismissals d
    WHERE d.card_row_id=f.id AND d.operation=f.operation
      AND d.revision=f.revision
      AND d.reason NOT LIKE 'cleared:%'))sql");
            dismissedTotal.executeStep();
            a.notifyFailures.dismissedTotal =
                dismissedTotal.getColumn(0).getInt();

            // Snapshot every acknowledged current failure, not just the 20
            // displayed below. Confirmation retains these exact identities.
            SQLite::Statement acknowledgedTargets(a.db->raw(lk.token()), R"sql(
SELECT d.card_row_id,d.revision,d.operation
  FROM discord_notify_failure_dismissals d
  JOIN discord_notify_cards n ON n.id=d.card_row_id
 WHERE d.reason NOT LIKE 'cleared:%'
   AND ((d.operation='create' AND n.post_state='failed'
         AND n.post_revision=d.revision)
     OR (d.operation='edit' AND n.edit_state='failed'
         AND n.edit_revision=d.revision))
 ORDER BY d.card_row_id,d.operation)sql");
            while (acknowledgedTargets.executeStep()) {
                a.notifyFailures.acknowledgedTargets.push_back({
                    acknowledgedTargets.getColumn(0).getInt64(),
                    acknowledgedTargets.getColumn(1).getInt(),
                    acknowledgedTargets.getColumn(2).getString()});
            }

            SQLite::Statement oldMessageTotal(a.db->raw(lk.token()), R"sql(
SELECT COUNT(*) FROM discord_notify_cards n
 WHERE n.edit_state='failed'
   AND (instr(n.edit_last_error,
       'Maximum number of edits to messages older than 1 hour reached')>0
     OR instr(n.edit_last_error,'30046')>0)
   AND NOT EXISTS(
     SELECT 1 FROM discord_notify_failure_dismissals d
      WHERE d.card_row_id=n.id AND d.operation='edit'
        AND d.revision=n.edit_revision))sql");
            oldMessageTotal.executeStep();
            a.notifyFailures.oldMessageTotal =
                oldMessageTotal.getColumn(0).getInt();
            SQLite::Statement oldMessageTargets(a.db->raw(lk.token()), R"sql(
SELECT n.id,n.edit_revision
  FROM discord_notify_cards n
 WHERE n.edit_state='failed'
   AND (instr(n.edit_last_error,
       'Maximum number of edits to messages older than 1 hour reached')>0
     OR instr(n.edit_last_error,'30046')>0)
   AND NOT EXISTS(
     SELECT 1 FROM discord_notify_failure_dismissals d
      WHERE d.card_row_id=n.id AND d.operation='edit'
        AND d.revision=n.edit_revision)
 ORDER BY n.updated_at,n.id
 LIMIT ?)sql");
            oldMessageTargets.bind(
                1, static_cast<int>(kMaxNotifyFailureDismissBatch));
            while (oldMessageTargets.executeStep()) {
                a.notifyFailures.oldMessageTargets.push_back({
                    oldMessageTargets.getColumn(0).getInt64(),
                    oldMessageTargets.getColumn(1).getInt()});
            }

            auto readFailureRows = [](SQLite::Statement& statement,
                                      std::vector<NotifyFailureRow>& rows) {
                while (statement.executeStep()) {
                    NotifyFailureRow row;
                    row.id = statement.getColumn(0).getInt64();
                    row.itemId = statement.getColumn(1).getInt64();
                    row.messageRowId = statement.getColumn(2).getInt64();
                    row.operation = statement.getColumn(3).getString();
                    row.attempts = statement.getColumn(4).getInt();
                    row.revision = statement.getColumn(5).getInt();
                    row.error =
                        redactHttpUrls(statement.getColumn(6).getString());
                    row.channelId = statement.getColumn(7).getString();
                    row.messageId = statement.getColumn(8).getString();
                    rows.push_back(std::move(row));
                }
            };
            SQLite::Statement failed(a.db->raw(lk.token()), R"sql(
WITH failures AS (
 SELECT id,COALESCE(item_id,0) AS item_id,
        COALESCE(discord_message_row_id,0) AS message_row_id,
        'create' AS operation,post_attempts AS attempts,
        post_revision AS revision,post_last_error AS error,
        notify_channel_id,notify_message_id,updated_at
   FROM discord_notify_cards WHERE post_state='failed'
 UNION ALL
 SELECT id,COALESCE(item_id,0),
        COALESCE(discord_message_row_id,0),
        'edit',edit_attempts,edit_revision,edit_last_error,
        notify_channel_id,notify_message_id,updated_at
   FROM discord_notify_cards WHERE edit_state='failed'
)
SELECT id,item_id,message_row_id,operation,attempts,revision,error,
       notify_channel_id,notify_message_id
  FROM failures f
 WHERE NOT EXISTS(
   SELECT 1 FROM discord_notify_failure_dismissals d
    WHERE d.card_row_id=f.id AND d.operation=f.operation
      AND d.revision=f.revision)
 ORDER BY updated_at DESC,id DESC,operation LIMIT 20)sql");
            readFailureRows(failed, a.notifyFailures.active);

            SQLite::Statement dismissed(a.db->raw(lk.token()), R"sql(
WITH failures AS (
 SELECT id,COALESCE(item_id,0) AS item_id,
        COALESCE(discord_message_row_id,0) AS message_row_id,
        'create' AS operation,post_attempts AS attempts,
        post_revision AS revision,post_last_error AS error,
        notify_channel_id,notify_message_id,updated_at
   FROM discord_notify_cards WHERE post_state='failed'
 UNION ALL
 SELECT id,COALESCE(item_id,0),
        COALESCE(discord_message_row_id,0),
        'edit',edit_attempts,edit_revision,edit_last_error,
        notify_channel_id,notify_message_id,updated_at
   FROM discord_notify_cards WHERE edit_state='failed'
)
SELECT id,item_id,message_row_id,operation,attempts,revision,error,
       notify_channel_id,notify_message_id
  FROM failures f
 WHERE EXISTS(
   SELECT 1 FROM discord_notify_failure_dismissals d
    WHERE d.card_row_id=f.id AND d.operation=f.operation
      AND d.revision=f.revision
      AND d.reason NOT LIKE 'cleared:%')
 ORDER BY updated_at DESC,id DESC,operation LIMIT 20)sql");
            readFailureRows(dismissed, a.notifyFailures.dismissed);
        }
        const auto& notifyFailures = a.notifyFailures.active;
        const auto& dismissedNotifyFailures = a.notifyFailures.dismissed;
        const int notifyFailureTotal = a.notifyFailures.activeTotal;
        const int dismissedNotifyFailureTotal =
            a.notifyFailures.dismissedTotal;
        const int oldMessageNotifyFailureTotal =
            a.notifyFailures.oldMessageTotal;
        const auto& oldMessageNotifyFailureTargets =
            a.notifyFailures.oldMessageTargets;
        if (notifyFailureTotal > 0 || dismissedNotifyFailureTotal > 0) {
            ImGui::SeparatorText(notifyFailureTotal > 0
                ? "Delivery failures" : "Delivery history");
        }
        if (!notifyFailures.empty()) {
            ImGui::TextColored(C_ORANGE,
                "%d active notification delivery failure(s)",
                notifyFailureTotal);
            ImGui::TextColored(C_DIM,
                "Retry transient failures, or recreate a card when Discord "
                "cannot edit its stored message. Dismiss hides only this "
                "failed revision locally; it does not delete the card or "
                "contact Discord.");
            if (oldMessageNotifyFailureTotal > 0) {
                const int batchCount = static_cast<int>(
                    oldMessageNotifyFailureTargets.size());
                const std::string bulkLabel =
                    "dismiss old-message alerts (" +
                    std::to_string(batchCount) +
                    (batchCount < oldMessageNotifyFailureTotal
                        ? " of " +
                          std::to_string(oldMessageNotifyFailureTotal)
                        : "") + ")";
                if (batchCount > 0 &&
                    ImGui::SmallButton(bulkLabel.c_str())) {
                    a.dismissOldNotifyAlertTargets =
                        oldMessageNotifyFailureTargets;
                    a.dismissOldNotifyAlertTotal =
                        oldMessageNotifyFailureTotal;
                }
                ImGui::SameLine();
                ImGui::TextColored(C_DIM,
                    "exact Discord edit-limit failures only");
            }
            for (const NotifyFailureRow& row : notifyFailures) {
                ImGui::PushID(static_cast<int>(row.id));
                ImGui::PushID(row.operation.c_str());
                const std::string owner = row.itemId > 0
                    ? "item I" + std::to_string(row.itemId)
                    : "source D" + std::to_string(row.messageRowId);
                ImGui::TextColored(C_RED,
                    "card N%lld - %s %s failed (%d attempts, revision %d)",
                    row.id, owner.c_str(), row.operation.c_str(),
                    row.attempts, row.revision);
                if (!row.error.empty())
                    ImGui::TextWrapped("%s", row.error.c_str());
                ImGui::TextColored(C_DIM, "stored target: channel %s%s%s",
                    row.channelId.empty() ? "(none)" : row.channelId.c_str(),
                    row.messageId.empty() ? "" : ", message ",
                    row.messageId.empty() ? "" : row.messageId.c_str());
                const bool oldMessageLimit =
                    row.operation == "edit" &&
                    isDiscordOldMessageEditLimitText(row.error);
                if (oldMessageLimit) {
                    ImGui::TextColored(C_ORANGE,
                        "Discord will not accept another edit for this older "
                        "message. Recreate it or dismiss this alert.");
                } else {
                    if (ImGui::SmallButton("retry delivery")) {
                        if (retryFailedNotifyCard(a.db, a.discord, row.id))
                            toast(a, "notification card N" +
                                      std::to_string(row.id) +
                                      " queued for retry");
                        else
                            toast(a, "notification retry was not applied",
                                  true);
                        a.needNotifyCounters = true;
                    }
                    ImGui::SameLine();
                }
                if (ImGui::SmallButton("recreate in saved channel")) {
                    a.recreateNotifyCardId = row.id;
                    a.recreateNotifyCardOperation = row.operation;
                    a.recreateNotifyCardRevision = row.revision;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("dismiss alert")) {
                    if (dismissFailedNotifyCardAlert(
                            a.db, row.id, row.operation, row.revision))
                        toast(a, "notification alert dismissed; delivery "
                                  "history retained");
                    else
                        toast(a, "alert changed before it could be dismissed",
                              true);
                    a.needNotifyCounters = true;
                }
                ImGui::PopID();
                ImGui::PopID();
            }
            if (notifyFailureTotal > static_cast<int>(notifyFailures.size()))
                ImGui::TextColored(C_DIM, "%d older active failure(s) omitted",
                    notifyFailureTotal -
                    static_cast<int>(notifyFailures.size()));
        }
        if (!dismissedNotifyFailures.empty()) {
            const std::string acknowledgedLabel =
                "Acknowledged current failures (" +
                std::to_string(dismissedNotifyFailureTotal) +
                ")###acknowledged_notify_failures";
            if (ImGui::TreeNode(acknowledgedLabel.c_str())) {
                ImGui::TextColored(C_DIM,
                    "These failed revisions remain recorded but no longer "
                    "raise an alert. Restore one to make it active again.");
                const std::string clearLabel =
                    "clear all acknowledged failures (" +
                    std::to_string(a.notifyFailures.acknowledgedTargets.size()) +
                    ")";
                if (ImGui::SmallButton(clearLabel.c_str()))
                    a.clearAcknowledgedNotifyTargets =
                        a.notifyFailures.acknowledgedTargets;
                for (const NotifyFailureRow& row :
                     dismissedNotifyFailures) {
                    ImGui::PushID(static_cast<int>(row.id));
                    ImGui::PushID(row.operation.c_str());
                    const std::string owner = row.itemId > 0
                        ? "item I" + std::to_string(row.itemId)
                        : "source D" + std::to_string(row.messageRowId);
                    ImGui::TextColored(C_DIM,
                        "card N%lld - %s %s revision %d (%d attempts)",
                        row.id, owner.c_str(), row.operation.c_str(),
                        row.revision, row.attempts);
                    if (!row.error.empty())
                        ImGui::TextWrapped("%s", row.error.c_str());
                    if (ImGui::SmallButton("restore alert")) {
                        if (restoreFailedNotifyCardAlert(
                                a.db, row.id, row.operation, row.revision))
                            toast(a, "notification alert restored");
                        else
                            toast(a, "dismissed revision is no longer current",
                                  true);
                        a.needNotifyCounters = true;
                    }
                    ImGui::SameLine();
                    const bool oldMessageLimit =
                        row.operation == "edit" &&
                        isDiscordOldMessageEditLimitText(row.error);
                    if (!oldMessageLimit) {
                        if (ImGui::SmallButton("retry delivery")) {
                            if (retryFailedNotifyCard(
                                    a.db, a.discord, row.id))
                                toast(a, "notification card N" +
                                          std::to_string(row.id) +
                                          " queued for retry");
                            else
                                toast(a,
                                    "notification retry was not applied",
                                    true);
                            a.needNotifyCounters = true;
                        }
                        ImGui::SameLine();
                    }
                    if (ImGui::SmallButton("recreate in saved channel")) {
                        a.recreateNotifyCardId = row.id;
                        a.recreateNotifyCardOperation = row.operation;
                        a.recreateNotifyCardRevision = row.revision;
                    }
                    ImGui::PopID();
                    ImGui::PopID();
                }
                if (dismissedNotifyFailureTotal >
                    static_cast<int>(dismissedNotifyFailures.size()))
                    ImGui::TextColored(C_DIM,
                        "%d older acknowledged failure(s) omitted",
                        dismissedNotifyFailureTotal -
                        static_cast<int>(dismissedNotifyFailures.size()));
                ImGui::TreePop();
            }
        }

        if (reloadNotifyCounters) {
            auto lk = a.db->guard();
            SQLite::Statement total(a.db->raw(lk.token()),
                "SELECT COUNT(*) FROM discord_notify_orphans "
                "WHERE cleanup_state='failed'");
            total.executeStep();
            a.notifyFailures.orphanTotal = total.getColumn(0).getInt();
            SQLite::Statement failed(a.db->raw(lk.token()),
                "SELECT id,card_row_id,notify_channel_id,notify_message_id,"
                "cleanup_attempts,cleanup_last_error "
                "FROM discord_notify_orphans WHERE cleanup_state='failed' "
                "ORDER BY updated_at DESC,id DESC LIMIT 20");
            while (failed.executeStep()) {
                OrphanFailureRow row;
                row.id = failed.getColumn(0).getInt64();
                row.cardRowId = failed.getColumn(1).getInt64();
                row.channelId = failed.getColumn(2).getString();
                row.messageId = failed.getColumn(3).getString();
                row.attempts = failed.getColumn(4).getInt();
                row.error = redactHttpUrls(failed.getColumn(5).getString());
                a.notifyFailures.orphans.push_back(std::move(row));
            }
        }
        const auto& orphanFailures = a.notifyFailures.orphans;
        const int orphanFailureTotal = a.notifyFailures.orphanTotal;
        if (!orphanFailures.empty()) {
            ImGui::SeparatorText("Orphan cleanup failures");
            ImGui::TextColored(C_RED,
                "%d exact callback-generated card cleanup failure(s)",
                orphanFailureTotal);
            ImGui::TextColored(C_DIM,
                "These IDs came only from this bot's create callbacks. Correct "
                "connectivity/permissions, then retry cleanup.");
            ImGui::PushID("orphan_cleanup");
            for (const OrphanFailureRow& row : orphanFailures) {
                ImGui::PushID(static_cast<int>(row.id));
                ImGui::TextColored(C_RED,
                    "cleanup O%lld from card N%lld failed (%d attempts)",
                    row.id, row.cardRowId, row.attempts);
                ImGui::TextColored(C_DIM, "exact target: channel %s, message %s",
                    row.channelId.c_str(), row.messageId.c_str());
                if (!row.error.empty())
                    ImGui::TextWrapped("%s", row.error.c_str());
                if (ImGui::SmallButton("retry orphan cleanup")) {
                    if (retryFailedNotifyOrphan(a.db, a.discord, row.id))
                        toast(a, "orphan cleanup O" + std::to_string(row.id) +
                                  " queued for retry");
                    else
                        toast(a, "orphan cleanup retry was not applied", true);
                    a.needNotifyCounters = true;
                }
                ImGui::PopID();
            }
            ImGui::PopID();
            if (orphanFailureTotal > static_cast<int>(orphanFailures.size()))
                ImGui::TextColored(C_DIM, "%d older cleanup failure(s) omitted",
                    orphanFailureTotal -
                    static_cast<int>(orphanFailures.size()));
        }

        if (reloadNotifyCounters) {
            auto lk = a.db->guard();
            SQLite::Statement total(a.db->raw(lk.token()),
                "SELECT COUNT(*) FROM discord_review_control_cleanups "
                "WHERE cleanup_state='failed' "
                "AND cleanup_last_error NOT LIKE 'cleared:%'");
            total.executeStep();
            a.notifyFailures.reviewCleanupTotal =
                total.getColumn(0).getInt();
            SQLite::Statement reviewCleanupTargets(a.db->raw(lk.token()), R"sql(
SELECT id,cleanup_revision,cleanup_last_error LIKE 'cleared:%'
  FROM discord_review_control_cleanups
 WHERE cleanup_state='failed' ORDER BY id)sql");
            while (reviewCleanupTargets.executeStep()) {
                const ReviewCleanupFailureTarget target{
                    reviewCleanupTargets.getColumn(0).getInt64(),
                    reviewCleanupTargets.getColumn(1).getInt()};
                if (reviewCleanupTargets.getColumn(2).getInt() != 0)
                    a.notifyFailures.clearedReviewCleanupTargets.push_back(target);
                else
                    a.notifyFailures.reviewCleanupTargets.push_back(target);
            }
            a.notifyFailures.clearedReviewCleanupTotal = static_cast<int>(
                a.notifyFailures.clearedReviewCleanupTargets.size());
            SQLite::Statement failed(a.db->raw(lk.token()),
                "SELECT id,card_row_id,notify_channel_id,notify_message_id,"
                "cleanup_attempts,cleanup_last_error "
                "FROM discord_review_control_cleanups "
                "WHERE cleanup_state='failed' "
                "AND cleanup_last_error NOT LIKE 'cleared:%' "
                "ORDER BY updated_at DESC,id DESC LIMIT 20");
            while (failed.executeStep()) {
                ReviewCleanupFailureRow row;
                row.id = failed.getColumn(0).getInt64();
                row.cardRowId = failed.getColumn(1).getInt64();
                row.channelId = failed.getColumn(2).getString();
                row.messageId = failed.getColumn(3).getString();
                row.attempts = failed.getColumn(4).getInt();
                row.error = redactHttpUrls(failed.getColumn(5).getString());
                a.notifyFailures.reviewCleanups.push_back(std::move(row));
            }
            // Store the stamp taken BEFORE reading the cache. A write between
            // cache sections must trigger the next refresh rather than being
            // mistaken for data already included in this snapshot.
            a.notifyFailures.databaseChanges = notifyDatabaseChanges;
            a.notifyFailures.databaseVersion = notifyDatabaseVersion;
            a.notifyFailures.loaded = true;
            a.needNotifyCounters = false;
        }
        const auto& reviewCleanupFailures =
            a.notifyFailures.reviewCleanups;
        const int reviewCleanupFailureTotal =
            a.notifyFailures.reviewCleanupTotal;
        if (!reviewCleanupFailures.empty()) {
            ImGui::SeparatorText("Review-reaction cleanup failures");
            ImGui::TextColored(C_RED,
                "%d all-reaction cleanup failure(s)",
                reviewCleanupFailureTotal);
            ImGui::TextColored(C_DIM,
                "Every reaction on the exact terminal bot card is targeted; "
                "the card, source, and item are retained. Requires Manage "
                "Messages.");
            ImGui::TextWrapped(
                "For Missing Permissions, allow the bot Manage Messages in the "
                "target channel and check category/channel overrides, then "
                "retry. Clearing only hides local alerts; reactions remain.");
            ImGui::PushID("review_control_cleanup");
            if (ImGui::SmallButton("retry all reaction cleanups")) {
                a.reviewCleanupActionTargets =
                    a.notifyFailures.reviewCleanupTargets;
                a.retryReviewCleanupAction = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("clear all reaction cleanup alerts")) {
                a.reviewCleanupActionTargets =
                    a.notifyFailures.reviewCleanupTargets;
                a.retryReviewCleanupAction = false;
            }
            for (const ReviewCleanupFailureRow& row :
                 reviewCleanupFailures) {
                ImGui::PushID(static_cast<int>(row.id));
                ImGui::TextColored(C_RED,
                    "cleanup C%lld from card N%lld failed (%d attempts)",
                    row.id, row.cardRowId, row.attempts);
                ImGui::TextColored(C_DIM,
                    "exact target: channel %s, message %s",
                    row.channelId.c_str(), row.messageId.c_str());
                if (!row.error.empty())
                    ImGui::TextWrapped("%s", row.error.c_str());
                if (ImGui::SmallButton("retry reaction cleanup")) {
                    if (retryFailedReviewControlCleanup(
                            a.db, a.discord, row.id))
                        toast(a, "review-reaction cleanup C" +
                                  std::to_string(row.id) +
                                  " queued for retry");
                    else
                        toast(a, "review-reaction cleanup retry was not applied",
                              true);
                    a.needNotifyCounters = true;
                }
                ImGui::PopID();
            }
            ImGui::PopID();
            if (reviewCleanupFailureTotal >
                static_cast<int>(reviewCleanupFailures.size()))
                ImGui::TextColored(C_DIM,
                    "%d older reaction cleanup failure(s) omitted",
                    reviewCleanupFailureTotal -
                    static_cast<int>(reviewCleanupFailures.size()));
        }
        if (a.notifyFailures.clearedReviewCleanupTotal > 0) {
            ImGui::TextColored(C_DIM,
                "%d reaction cleanup alert(s) cleared locally; reactions may remain",
                a.notifyFailures.clearedReviewCleanupTotal);
            if (ImGui::SmallButton("retry cleared reaction cleanups")) {
                a.reviewCleanupActionTargets =
                    a.notifyFailures.clearedReviewCleanupTargets;
                a.retryReviewCleanupAction = true;
            }
        }
    }

    if (sectionHeader(a, "Monitors", "discord_monitors", true))
        drawDiscordMonitors(a);

    if (sectionHeader(a, "Access Management", "discord_access", false))
        drawDiscordAccessManagement(a);

    ImGui::SeparatorText("Inbox");
    ImGui::BeginDisabled(a.inboxReview.active());
    if (ImGui::Button("Dismiss All (Ctrl+Click)")) startInboxReview(a, false);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Hold Ctrl and click to dismiss every current inbox request, including older requests beyond the displayed 200.");
    ImGui::SameLine();
    if (ImGui::Button("Approve All Assigned (Ctrl+Click)")) startInboxReview(a, true);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Hold Ctrl and click to promote all current inbox requests with an active assigned project. Uses your selected project and ticket type; unassigned requests stay in the inbox.");
    ImGui::EndDisabled();
    if (!a.inboxReview.targets.empty()) {
        const auto& batch = a.inboxReview;
        const bool reviewingInbox = batch.active();
        ImGui::TextColored(reviewingInbox ? C_ACCENT : C_DIM,
            "%s: %zu / %zu - %s", reviewingInbox ? "Reviewing" : "Finished",
            batch.next, batch.targets.size(), inboxReviewSummary(batch).c_str());
        if (!batch.firstError.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(C_RED, "First error: %s", batch.firstError.c_str());
            ImGui::PopTextWrapPos();
        }
    }
    ImGui::TextColored(C_DIM,
        "Manual capture (admin/developer whitelist only): directly mention "
        "the bot with a suggestion, or mention it while replying to a message. "
        "The submitted message lands here at 100%%; reply text after the "
        "mention becomes the admin note. Replies to an exact source that is "
        "already promoted enrich that existing ticket instead of returning "
        "to this inbox.");
    if (a.inbox.empty())
        ImGui::TextColored(C_DIM, "inbox zero - nothing detected");
    for (auto& m : a.inbox) {
        ImGui::PushID((int)m.id);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, C_BG2);
        ImGui::BeginChild("msg", ImVec2(0, 0),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
        ImGui::BeginDisabled(a.inboxReview.active());
        bool dismiss = false;
        if (ImGui::BeginTable("request_header", 2,
                ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("details", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("dismiss", ImGuiTableColumnFlags_WidthFixed,
                ImGui::CalcTextSize("X").x + ImGui::GetStyle().FramePadding.x * 2.0f);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            dismiss = ImGui::SmallButton("X##dismiss_request");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Dismiss this inbox request");
            ImGui::TableSetColumnIndex(0);
            chip((m.kind + " " + std::to_string((int)(m.score * 100)) + "%").c_str(),
                 m.kind == "bug" ? C_RED : C_ACCENT);
            ImGui::SameLine();
            ImGui::TextUnformatted(m.author.c_str());
            ImGui::SameLine();
            ImGui::TextColored(C_DIM, "#%s - %s", m.channelName.c_str(),
                               shortTs(m.posted).c_str());
            if (!m.guildId.empty() && !m.messageId.empty()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("open in Discord"))
                    openUrl("https://discord.com/channels/" + m.guildId + "/" +
                            m.channelId + "/" + m.messageId);
            }
            // Project attribution: channel mapping first, then name/alias
            // inference, otherwise a general flag the user assigns by hand.
            if (m.selProjectId <= 0) {
                long long want = m.channelProjectId > 0 ? m.channelProjectId
                                                        : m.inferredProjectId;
                m.selProjectId = want;
                m.selProject = projectIndexForId(a, want);
                if (m.selProject < 0) m.selProjectId = 0;
            }
            ImGui::SameLine();
            if (m.inferredProjectId > 0 && m.channelProjectId <= 0) {
                Project* ip = findProject(a, m.inferredProjectId);
                if (ip) chip(("auto: " + ip->name).c_str(), C_GREEN);
            } else if (m.channelProjectId <= 0 && m.inferredProjectId <= 0) {
                chip("general - pick a project", C_DIM);
            }
            ImGui::EndTable();
        }
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(m.content.c_str());
        if (!m.adminNote.empty()) {
            ImGui::TextColored(C_GREEN, "admin note:");
            ImGui::SameLine();
            ImGui::TextUnformatted(m.adminNote.c_str());
        }
        if (m.attachmentCount > 0) {
            ImGui::TextColored(C_ORANGE,
                "%d file%s attached - files are saved only when you Promote",
                m.attachmentCount, m.attachmentCount == 1 ? "" : "s");
        }
        ImGui::PopTextWrapPos();
        ImGui::TextColored(C_DIM, "matched: %s", m.matched.c_str());

        projectCombo(a, "##pm", &m.selProject);
        m.selProjectId =
            (m.selProject >= 0 &&
             m.selProject < static_cast<int>(a.projects.size()))
                ? a.projects[m.selProject].id : 0;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        ImGui::Combo("##pt", &m.selType, kPromoteTypeNames, 3);
        ImGui::SameLine();
        if (ImGui::Button("Edit pending"))
            openEditPendingSuggestion(a, m);
        ImGui::SameLine();
        bool noProject = m.selProjectId <= 0;
        if (noProject) ImGui::BeginDisabled();
        if (ImGui::Button("Promote to item")) {
            nlohmann::json r = promoteSuggestion(a.db, a.discord, m.id,
                                                 m.selProjectId,
                                                 kPromoteTypeNames[m.selType], "");
            if (r.value("ok", false)) {
                std::string result = "promoted with credit to " + m.author;
                const int saved = r.value("attachments_saved", 0);
                const int failed = r.value("attachments_failed", 0);
                const int pendingAttachments =
                    r.value("attachments_pending", 0);
                if (saved || failed || pendingAttachments) {
                    result += " - attachments: " + std::to_string(saved) +
                              " saved";
                    if (failed) result += ", " + std::to_string(failed) + " failed";
                    if (pendingAttachments)
                        result += ", " +
                                  std::to_string(pendingAttachments) +
                                  " queued for save";
                }
                toast(a, result, failed > 0);
                a.needInbox = true;
                a.needProjects = true;
                a.needItems = true;
                a.needSources = true;
                a.needWork = true;
            } else {
                toast(a, r.value("error", "promote failed"), true);
            }
        }
        if (noProject) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Dismiss")) dismiss = true;
        if (dismiss) dismissInboxRequest(a, m.id);
        ImGui::EndDisabled();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopID();
    }

    ImGui::Spacing();
    if (sectionHeader(a, "Detection templates", "discord_templates", false)) {
        ImGui::TextColored(C_DIM,
            "One entry per pattern: {\"phrase\",\"kind\":suggestion|bug,\"weight\"}. "
            "Phrases starting with ^ anchor to the message start. Score >= 0.5 lands "
            "in the inbox.");
        ImGui::InputTextMultiline("##pat", a.dcPatterns, sizeof(a.dcPatterns),
                                  ImVec2(-1, 220));
        auto savePatterns = [&]() -> bool {
            nlohmann::json j = nlohmann::json::parse(a.dcPatterns, nullptr, false);
            if (j.is_discarded()) { toast(a, "invalid JSON", true); return false; }
            auto pats = SuggestionDetector::patternsFromJson(j);
            if (pats.empty()) { toast(a, "no valid patterns", true); return false; }
            auto lk = a.db->guard();
            a.db->setSetting(lk.token(), "detection_patterns",
                             SuggestionDetector::patternsToJson(pats).dump());
            return true;
        };
        if (ImGui::Button("Save patterns")) {
            if (savePatterns()) toast(a, "patterns saved");
        }
        ImGui::SameLine();
        if (ImGui::Button("Save + rescan inbox")) {
            if (savePatterns()) {
                nlohmann::json r = rescanNewMessages(a.db);
                toast(a, "rescanned " + std::to_string(r.value("rescanned", 0)) + ", " +
                             std::to_string(r.value("candidates", 0)) + " candidates");
                a.needInbox = true;
                a.needChannels = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset to defaults")) {
            {
                auto lk = a.db->guard();
                a.db->setSetting(lk.token(), "detection_patterns", "");
            }
            a.dcPatternsLoaded = false;
            toast(a, "patterns reset");
        }
    }
}


} // namespace devhub
