#include "GuiInternal.h"
#include "ReleaseAnnouncements.h"

namespace devhub {
namespace {
constexpr const char* formats[] = {"plain", "embed", "codeblock"};

nlohmann::json editedConfig(const ReleaseAnnouncementUi& ui) {
    return {{"enabled", ui.enabled}, {"public_release", ui.publicRelease},
        {"expected_revision", ui.revision},
        {"public_channel_id", trim(ui.publicChannel)},
        {"private_channel_id", trim(ui.privateChannel)},
        {"public_webhook_url", trim(ui.publicWebhook)},
        {"private_webhook_url", trim(ui.privateWebhook)},
        {"clear_public_webhook", ui.clearPublic},
        {"clear_private_webhook", ui.clearPrivate},
        {"message_template", std::string(ui.messageTemplate)},
        {"format", formats[ui.format]}};
}

void loadConfig(App& a) {
    auto& ui = a.releaseAnnouncements;
    ui.config = releaseAnnouncementConfig(a.db, ui.projectId);
    ui.loaded = true;
    ui.error = ui.config.value("error", "");
    if (!ui.config.value("ok", false)) return;
    ui.revision = ui.config.value("revision", std::uint64_t{0});
    ui.enabled = ui.config.value("enabled", false);
    ui.publicRelease = ui.config.value("public_release", true);
    ui.clearPublic = ui.clearPrivate = false;
    ui.publicWebhook[0] = ui.privateWebhook[0] = '\0';
    copyBuf(ui.publicChannel, sizeof(ui.publicChannel), ui.config.value("public_channel_id", ""));
    copyBuf(ui.privateChannel, sizeof(ui.privateChannel), ui.config.value("private_channel_id", ""));
    copyBuf(ui.messageTemplate, sizeof(ui.messageTemplate), ui.config.value("message_template", "[automated message]"));
    const std::string format = ui.config.value("format", "plain");
    ui.format = format == "embed" ? 1 : format == "codeblock" ? 2 : 0;
    ui.databaseChanges = -1;
}

void displayPayload(const nlohmann::json& payload) {
    const std::string content = payload.value("content", "");
    if (!content.empty()) ImGui::TextWrapped("%s", content.c_str());
    if (payload.contains("embeds") && payload["embeds"].is_array()) {
        for (const auto& embed : payload["embeds"]) {
            ImGui::TextColored(C_PURPLE, "%s", embed.value("title", "Release update").c_str());
            ImGui::TextWrapped("%s", embed.value("description", "").c_str());
        }
    }
}

void channelPicker(App& a, const char* label, char* buffer, size_t capacity) {
    std::string selected = buffer[0] ? buffer : "Choose channel";
    for (const auto& channel : a.channels)
        if (channel.channelId == buffer) selected = channel.name.empty() ? channel.channelId : channel.name;
    if (ImGui::BeginCombo(label, selected.c_str())) {
        for (const auto& channel : a.channels) {
            const std::string name = (channel.name.empty() ? channel.channelId : channel.name) +
                (channel.projectId == a.releaseAnnouncements.projectId ? " (assigned to project)" : "") +
                "##" + channel.channelId;
            if (ImGui::Selectable(name.c_str(), channel.channelId == buffer))
                copyBuf(buffer, capacity, channel.channelId);
        }
        ImGui::EndCombo();
    }
}
}

void drawReleaseAnnouncements(App& a) {
    if (!sectionHeader(a, "Release announcements", "release_announcements", false)) return;
    auto& ui = a.releaseAnnouncements;
    ImGui::TextWrapped("Keep using your Build, Prepare, and Push scripts. A connected Push script hands a verified GitHub release to DevHub for delivery here.");
    if (projectIdCombo(a, "Project##releaseannounce", &ui.projectId)) {
        ui.loaded = false;
        ui.preview = nullptr;
        ui.history = nullptr;
        ui.pendingRetry = nullptr;
    }
    if (ui.projectId <= 0) return;
    if (!ui.loaded) loadConfig(a);
    ImGui::TextColored(C_DIM, "Project ID for step 3: %lld", ui.projectId);
    if (ImGui::SmallButton("Copy step 3 connection settings")) {
        const std::string settings = nlohmann::json{{"project_id", ui.projectId}}.dump(2);
        ImGui::SetClipboardText(settings.c_str());
        toast(a, "save these settings as devhub-release.json next to the connected Push script");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("For a Push script with DevHub integration: save the copied JSON as devhub-release.json in its project folder. Existing scripts need the integration added once.");
    if (!ui.error.empty()) ImGui::TextWrapped("%s", ui.error.c_str());
    if (!ui.config.value("ok", false)) {
        if (ImGui::Button("Reload announcement settings")) loadConfig(a);
        return;
    }
    ImGui::Checkbox("Enable release announcements", &ui.enabled);
    ImGui::Checkbox("Public plugin / project", &ui.publicRelease);
    ImGui::TextWrapped("Public projects post an announcement. Private projects use the private webhook and attach the verified release ZIP for Access Management.");

    channelPicker(a, "Public channel", ui.publicChannel, sizeof(ui.publicChannel));
    ImGui::InputText("Public channel ID", ui.publicChannel, sizeof(ui.publicChannel));
    ImGui::InputTextWithHint("Public webhook", ui.config.value("has_public_webhook", false) ? "Saved; leave blank to keep" : "https://discord.com/api/webhooks/...", ui.publicWebhook, sizeof(ui.publicWebhook), ImGuiInputTextFlags_Password);
    ImGui::Checkbox("Clear saved public webhook", &ui.clearPublic);
    channelPicker(a, "Private channel", ui.privateChannel, sizeof(ui.privateChannel));
    ImGui::InputText("Private channel ID", ui.privateChannel, sizeof(ui.privateChannel));
    ImGui::InputTextWithHint("Private webhook", ui.config.value("has_private_webhook", false) ? "Saved; leave blank to keep" : "https://discord.com/api/webhooks/...", ui.privateWebhook, sizeof(ui.privateWebhook), ImGuiInputTextFlags_Password);
    ImGui::Checkbox("Clear saved private webhook", &ui.clearPrivate);
    ImGui::TextUnformatted("Message template");
    ImGui::InputTextMultiline("##release-template", ui.messageTemplate, sizeof(ui.messageTemplate), ImVec2(-1, 105));
    ImGui::TextWrapped("Include [automated message] once for the generated notes. Add your own role mention, link, and other text around it. Only role mentions written in this template can notify a role.");
    const char* formatLabels[] = {"Plain message", "Embed", "Code block"};
    ImGui::Combo("Update format", &ui.format, formatLabels, 3);
    if (ImGui::Button("Save announcement settings")) {
        auto result = saveReleaseAnnouncementConfig(a.db, ui.projectId, editedConfig(ui));
        if (result.value("ok", false)) {
            loadConfig(a);
            toast(a, "release announcement settings saved");
        } else toast(a, result.value("error", "could not save settings"), true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload##releaseannounce")) loadConfig(a);
    ImGui::SameLine();
    if (ImGui::Button("Preview format"))
        ui.preview = previewReleaseAnnouncement(editedConfig(ui), "- **Updated Release:** Your generated release notes appear here.", "Preview", "");
    if (ui.preview.is_object()) {
        if (!ui.preview.value("ok", false)) ImGui::TextWrapped("%s", ui.preview.value("error", "Preview unavailable").c_str());
        else if (ui.preview.contains("messages")) {
            ImGui::SeparatorText("Preview");
            for (const auto& message : ui.preview["messages"]) displayPayload(message);
        }
    }

    if (!ui.publicRelease && ui.config.value("has_private_webhook", false)) {
        if (ImGui::Button("Allow saved private webhook in Access Management")) {
            const std::string channel = ui.config.value("private_channel_id", "");
            const std::string webhook = ui.config.value("private_webhook_id", "");
            std::string error;
            const auto rules = discordAccessRules(a.db, &error);
            const auto rule = std::find_if(rules.begin(), rules.end(), [&](const auto& entry) { return entry.channelId == channel; });
            if (!error.empty()) toast(a, error, true);
            else if (rule == rules.end()) toast(a, "add this private channel and its ZIP filename prefix in Access Management first", true);
            else if (saveDiscordAccessRule(a.db, channel, rule->filenamePrefix, rule->enabled, rule->revision, &error, webhook))
                toast(a, "saved private webhook allowed for this Access Management channel");
            else toast(a, error, true);
        }
        ImGui::TextWrapped("Use an enabled Access Management rule for the private channel and matching ZIP prefix. The bot needs View Channel, Read Message History, and Pin Messages.");
    }

    // No history/settings reload per frame. Check the shared connection's
    // in-memory change counter at most once per second while this is visible.
    if (ImGui::GetTime() >= ui.nextRefresh) {
        ui.nextRefresh = ImGui::GetTime() + 1.0;
        int changes = 0;
        { auto guard = a.db->guard(); changes = a.db->raw(guard.token()).getTotalChanges(); }
        if (changes != ui.databaseChanges) {
            ui.history = listReleaseAnnouncements(a.db, ui.projectId, 30);
            ui.databaseChanges = changes;
        }
    }
    ImGui::SeparatorText("Delivery history");
    if (ui.history.is_object() && !ui.history.value("ok", false))
        ImGui::TextWrapped("%s", ui.history.value("error", "History unavailable").c_str());
    if (ui.history.is_object() && ui.history.contains("events")) {
        for (const auto& event : ui.history["events"]) {
            const std::string releaseId = event.value("release_id", "");
            const std::string status = event.value("status", "");
            ImGui::PushID(releaseId.c_str());
            ImGui::Text("%s: %s", event.value("tag", "Release").c_str(), status.c_str());
            const std::string error = event.value("error", "");
            if (!error.empty()) ImGui::TextWrapped("%s", error.c_str());
            if (status == "failed" || status == "uncertain") {
                if (ImGui::SmallButton(status == "uncertain" ? "Review retry..." : "Retry Discord delivery")) {
                    if (status == "uncertain") ui.pendingRetry = event;
                    else {
                        auto result = retryReleaseAnnouncement(a.db, ui.projectId, releaseId, event.value("revision", std::uint64_t{0}));
                        toast(a, result.value("ok", false) ? "Discord delivery queued" : result.value("error", "Retry failed"), !result.value("ok", false));
                        ui.databaseChanges = -1;
                    }
                }
            }
            ImGui::PopID();
        }
    }
    if (ui.pendingRetry.is_object() && !ImGui::IsPopupOpen("Retry uncertain delivery?")) ImGui::OpenPopup("Retry uncertain delivery?");
    if (ImGui::BeginPopupModal("Retry uncertain delivery?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Discord may already have accepted this update. Check the destination channel before retrying; another post could duplicate it. This retries Discord only.");
        if (ImGui::Button("I checked; retry Discord")) {
            auto result = retryReleaseAnnouncement(a.db, ui.projectId, ui.pendingRetry.value("release_id", ""), ui.pendingRetry.value("revision", std::uint64_t{0}), true);
            toast(a, result.value("ok", false) ? "Discord delivery queued" : result.value("error", "Retry failed"), !result.value("ok", false));
            ui.pendingRetry = nullptr;
            ui.databaseChanges = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel##uncertain")) { ui.pendingRetry = nullptr; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}
}
