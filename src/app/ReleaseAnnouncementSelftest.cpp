#include "ReleaseAnnouncementSelftest.h"
#include "ReleaseAnnouncements.h"
#include "ReleaseAnnouncementWorker.h"
#include "Db.h"
#include "PacketOps.h"
#include "WinUtil.h"

#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace devhub {
namespace {
using json = nlohmann::json;
namespace fs = std::filesystem;
const std::string kWebhook = "https://discord.com/api/webhooks/123456789012345678/abcdefghijklmnopqrstuvwxyz0123456789";

json fixtureEvent(long long project, long long id = 200, const std::string& notes = "- Added release announcements.") {
    const std::string repo = "example/Fixture";
    json release = {{"id", id}, {"tag", "v1.0/preview"},
        {"url", "https://github.com/example/Fixture/releases/tag/v1.0/preview"},
        {"body", "New release notes"}, {"commit", std::string(40, 'a')},
        {"draft", false}, {"prerelease", false}, {"published_at", "2026-11-10T00:00:00Z"},
        {"assets", json::array({{{"id", 300}, {"name", "latest.zip"}, {"size", 256},
            {"sha256", std::string(64, 'a')},
            {"url", "https://github.com/example/Fixture/releases/download/v1.0/preview/latest.zip"}}})}};
    json previous = {{"id", 199}, {"tag", "v0.9"},
        {"url", "https://github.com/example/Fixture/releases/tag/v0.9"}, {"body", "Old release notes"}};
    return {{"schema", "xa.devhub.release-handoff/v1"}, {"project_id", project}, {"repository", repo},
        {"previous_release", previous}, {"release", release}, {"generated_notes", notes},
        {"notes_source", {{"old_body_sha256", sha256Hex("Old release notes").hex},
            {"new_body_sha256", sha256Hex("New release notes").hex}, {"skill_sha256", std::string(64, 'b')}}}};
}
long long addProject(Db& db, const fs::path& root) {
    auto guard = db.guard();
    SQLite::Statement insert(db.raw(guard.token()),
        "INSERT INTO projects(name,slug,path,github_url,created_at,updated_at) VALUES(?,?,?,?,?,?)");
    insert.bind(1, "Release fixture"); insert.bind(2, "release-fixture"); insert.bind(3, narrow(root.native()));
    insert.bind(4, "https://github.com/example/Fixture.git"); insert.bind(5, "2026-11-10T00:00:00Z");
    insert.bind(6, "2026-11-10T00:00:00Z"); insert.exec();
    return db.raw(guard.token()).getLastInsertRowid();
}
json eventById(Db& db, long long project, const std::string& id) {
    const auto history = listReleaseAnnouncements(&db, project, 100);
    if (history.value("ok", false))
        for (const auto& event : history.at("events"))
            if (event.value("release_id", "") == id) return event;
    return json();
}
} // namespace

int runReleaseAnnouncementSelftest() {
    int failures = 0;
    const auto check = [&](bool condition, const char* name) {
        std::printf("  [%s] release announcements: %s\n", condition ? "PASS" : "FAIL", name);
        if (!condition) ++failures;
    };
    const fs::path temporary = fs::temp_directory_path() /
        ("devhub-release-selftest-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    bool ownedCreated = false;
    try {
        ownedCreated = fs::create_directory(temporary);
        if (!ownedCreated) throw std::runtime_error("disposable fixture directory already exists");
        {
            Db db(narrow((temporary / "fixture.db").native()));
            const long long project = addProject(db, temporary);
            json config = releaseAnnouncementConfig(&db, project);
            check(config.value("ok", false) && !config.value("enabled", true) && !config.value("has_public_webhook", true), "new projects are opt-in");
            config = saveReleaseAnnouncementConfig(&db, project, {
                {"expected_revision", 0}, {"enabled", true}, {"public_release", true},
                {"public_webhook_url", kWebhook}, {"public_channel_id", "223456789012345678"},
                {"message_template", "<@&423456789012345678> Updates: [automated message]"}, {"format", "plain"}});
            check(config.value("ok", false) && config.value("has_public_webhook", false) &&
                !config.contains("public_webhook_url") && config.value("public_webhook_id", "") == "123456789012345678", "configuration redacts secret URLs and returns exact webhook identity");
            {
                auto guard = db.guard();
                SQLite::Statement raw(db.raw(guard.token()), "SELECT value FROM settings WHERE key=?");
                raw.bind(1, "release_announcement_webhook_config_" + std::to_string(project));
                if (!raw.executeStep()) throw std::runtime_error("sealed configuration fixture row is missing");
                const std::string stored = raw.getColumn(0).getString();
                check(stored.rfind("dpapi:v1:", 0) == 0 && stored.find("abcdefghijklmnopqrstuvwxyz") == std::string::npos, "webhook configuration is DPAPI sealed on disk");
            }
            const auto stale = saveReleaseAnnouncementConfig(&db, project, {{"expected_revision", 0}, {"enabled", false}});
            check(!stale.value("ok", true) && stale.value("code", "") == "conflict", "stale configuration edits are rejected");
            config = saveReleaseAnnouncementConfig(&db, project, {{"expected_revision", config.at("revision")}, {"public_webhook_url", ""}});
            check(config.value("ok", false) && config.value("has_public_webhook", false), "blank secrets preserve the configured webhook");
            std::string webhookId, requestPath;
            check(!parseReleaseWebhookUrl(kWebhook + "?thread_id=1", webhookId, requestPath) &&
                !parseReleaseWebhookUrl("https://discord.com.evil.invalid/api/webhooks/123456789012345678/abcdefghijklmnopqrstuvwxyz", webhookId, requestPath), "webhook URLs reject queries and lookalike destinations");
            const auto preview = previewReleaseAnnouncement(config, std::string(5000, 'x'), "v1.0");
            bool bounded = preview.value("ok", false) && preview.at("messages").size() == 3;
            for (const auto& message : preview.at("messages")) bounded = bounded && message.at("content").get<std::string>().size() <= 2000 && message.at("allowed_mentions").at("parse").empty();
            check(bounded && preview.at("messages")[0].at("allowed_mentions").at("roles") == json::array({"423456789012345678"}), "long messages split without unrestricted mentions");
            json codeConfig = config; codeConfig["format"] = "codeblock";
            check(!previewReleaseAnnouncement(codeConfig, "```powershell\ncommand\n```").value("ok", true), "code-block formatting refuses existing fences without altering commands");
            json embedConfig = config; embedConfig["format"] = "embed";
            const auto embeds = previewReleaseAnnouncement(embedConfig, std::string(9000, 'x'));
            check(embeds.value("ok", false) && embeds.at("messages").size() == 3 &&
                embeds.at("messages")[0].at("embeds")[0].at("description").get<std::string>().size() <= 4096, "embeds preserve all notes within per-message limits");
            const auto unicodeTitle = previewReleaseAnnouncement(embedConfig, "Notes", std::string(199, 'a') + "\xE2\x82\xAC" + "z");
            check(unicodeTitle.value("ok", false) && unicodeTitle.at("messages")[0].at("embeds")[0].at("title") == std::string(199, 'a'),
                "embed title truncation preserves UTF-8 boundaries");

            json source = fixtureEvent(project);
            auto submitted = submitReleaseAnnouncement(&db, source);
            check(submitted.value("ok", false) && submitted.value("release_id", "") == "200", "verified mixed-case repository and slash-tag release handoff is accepted");
            auto duplicate = submitReleaseAnnouncement(&db, source);
            check(duplicate.value("ok", false) && duplicate.value("duplicate", false), "identical handoff is idempotent");
            json changed = source; changed["generated_notes"] = "Different notes";
            check(!submitReleaseAnnouncement(&db, changed).value("ok", true), "changed payload cannot reuse the same release identity");
            json wrongRepo = fixtureEvent(project, 201); wrongRepo["repository"] = "other/repo";
            check(!submitReleaseAnnouncement(&db, wrongRepo).value("ok", true), "unassigned repository is rejected");
            json wrongHash = fixtureEvent(project, 201); wrongHash["notes_source"]["new_body_sha256"] = std::string(64, 'c');
            check(!submitReleaseAnnouncement(&db, wrongHash).value("ok", true), "notes provenance must bind the exact release bodies");
            json publicArtifact = fixtureEvent(project, 201); publicArtifact["private_attachment"] = {{"path", "private.zip"}};
            check(!submitReleaseAnnouncement(&db, publicArtifact).value("ok", true), "public routes cannot accept a private artifact");
            ReleaseAnnouncementWork work;
            check(claimReleaseAnnouncement(&db, work) && work.releaseId == 200, "claim persists sending before transport");
            ReleaseAnnouncementDeliveryHooks hooks;
            int calls = 0;
            hooks.send = [&](const std::string&, const std::string&, const json&, const std::string&, const std::string&, bool& attempted) {
                ++calls; attempted = true; return ReleaseAnnouncementSendResult{"sent", "523456789012345678", ""};
            };
            hooks.beforeReceiptSave = [] { throw std::runtime_error("injected persistence failure"); };
            processReleaseAnnouncement(&db, work, hooks);
            auto current = eventById(db, project, "200");
            check(calls == 1 && current.value("status", "") == "uncertain", "accepted POST followed by persistence failure requires uncertain recovery");
            check(!retryReleaseAnnouncement(&db, project, "200", current.at("revision").get<std::uint64_t>()).value("ok", true), "uncertain delivery requires explicit duplicate-risk acknowledgement");
            auto retry = retryReleaseAnnouncement(&db, project, "200", current.at("revision").get<std::uint64_t>(), true);
            check(retry.value("ok", false) && claimReleaseAnnouncement(&db, work), "explicit uncertain retry requeues only Discord delivery");
            recoverInterruptedReleaseAnnouncements(&db);
            current = eventById(db, project, "200");
            check(current.value("status", "") == "uncertain" && !claimReleaseAnnouncement(&db, work), "restart recovery never blindly repeats an interrupted POST");
            retry = retryReleaseAnnouncement(&db, project, "200", current.at("revision").get<std::uint64_t>(), true);
            check(retry.value("ok", false) && claimReleaseAnnouncement(&db, work), "recovered uncertainty remains explicitly retryable");
            hooks.beforeReceiptSave = {};
            processReleaseAnnouncement(&db, work, hooks);
            check(eventById(db, project, "200").value("status", "") == "delivered", "confirmed message receipt finishes the announcement");

            const auto multipart = submitReleaseAnnouncement(&db, fixtureEvent(project, 202, std::string(5000, 'z')));
            check(multipart.value("ok", false) && claimReleaseAnnouncement(&db, work), "multipart release enters the delivery lane");
            finishReleaseAnnouncement(&db, work, "sent", "623456789012345678", "");
            config = saveReleaseAnnouncementConfig(&db, project, {{"expected_revision", config.at("revision")}, {"public_webhook_url", ""}});
            const bool claimedAfterConfig = claimReleaseAnnouncement(&db, work);
            current = eventById(db, project, "202");
            check(!claimedAfterConfig && current.value("status", "") == "failed", "queued parts stop on configuration revision changes");
            retry = retryReleaseAnnouncement(&db, project, "202", current.at("revision").get<std::uint64_t>());
            check(retry.value("ok", false), "restored effective configuration permits partial delivery recovery despite newer revision");
            while (claimReleaseAnnouncement(&db, work)) finishReleaseAnnouncement(&db, work, "sent", "723456789012345678", "");
            current = eventById(db, project, "202");
            check(current.value("status", "") == "delivered" && current.at("message_ids").size() == 3, "partial retry resumes at the first unconfirmed message");
            long long before = 0, after = 0;
            { auto guard = db.guard(); before = db.raw(guard.token()).getTotalChanges(); }
            bool idle = true;
            for (int pass = 0; pass < 20; ++pass) idle = !claimReleaseAnnouncement(&db, work) && idle;
            { auto guard = db.guard(); after = db.raw(guard.token()).getTotalChanges(); }
            check(idle && before == after, "twenty idle claim passes produce zero database writes");

            const fs::path privateRoot = temporary / "private-project";
            fs::create_directory(privateRoot);
            {
                auto guard = db.guard();
                SQLite::Statement update(db.raw(guard.token()), "UPDATE projects SET path=? WHERE id=?");
                update.bind(1, narrow(privateRoot.native())); update.bind(2, project); update.exec();
            }
            std::string zip("PK\x05\x06", 4); zip.append(18, '\0');
            const fs::path privateZip = privateRoot / "private.zip";
            { std::ofstream output(privateZip, std::ios::binary); output.write(zip.data(), static_cast<std::streamsize>(zip.size())); }
            config = saveReleaseAnnouncementConfig(&db, project, {{"expected_revision", config.at("revision")},
                {"public_release", false}, {"private_webhook_url", kWebhook}, {"private_channel_id", "823456789012345678"}});
            check(config.value("ok", false), "private route is separately configured");
            const auto privateSource = [&](long long releaseId) {
                json value = fixtureEvent(project, releaseId);
                value["private_attachment"] = {{"path", narrow(privateZip.native())}, {"filename", "private.zip"},
                    {"size", zip.size()}, {"sha256", sha256Hex(zip).hex}};
                return value;
            };
            hooks.send = [&](const std::string&, const std::string& channel, const json&, const std::string& filename, const std::string& bytes, bool& attempted) {
                ++calls; attempted = true;
                check(channel == "823456789012345678" && filename == "private.zip" && bytes == zip,
                    "private sender receives only the exact verified ZIP and private destination");
                return ReleaseAnnouncementSendResult{"sent", "923456789012345678", ""};
            };
            check(submitReleaseAnnouncement(&db, privateSource(203)).value("ok", false) && claimReleaseAnnouncement(&db, work), "private receipt is accepted and claimed");
            processReleaseAnnouncement(&db, work, hooks);
            check(eventById(db, project, "203").value("status", "") == "delivered", "private attachment completes only with a confirmed message receipt");
            json wrongZip = privateSource(204); wrongZip["private_attachment"]["sha256"] = std::string(64, 'd');
            const int beforeWrongZip = calls;
            check(submitReleaseAnnouncement(&db, wrongZip).value("ok", false) && claimReleaseAnnouncement(&db, work), "changed-ZIP fixture is queued for preflight");
            processReleaseAnnouncement(&db, work, hooks);
            check(calls == beforeWrongZip && eventById(db, project, "204").value("status", "") == "failed", "changed private ZIP is rejected before invoking transport");
            json outsideZip = privateSource(205);
            outsideZip["private_attachment"]["path"] = narrow((temporary / "private.zip").native());
            { std::ofstream output(temporary / "private.zip", std::ios::binary); output.write(zip.data(), static_cast<std::streamsize>(zip.size())); }
            check(submitReleaseAnnouncement(&db, outsideZip).value("ok", false) && claimReleaseAnnouncement(&db, work), "outside-project fixture is queued for preflight");
            processReleaseAnnouncement(&db, work, hooks);
            check(calls == beforeWrongZip && eventById(db, project, "205").value("status", "") == "failed", "outside-project private bytes never reach transport");
            const auto clear = saveReleaseAnnouncementConfig(&db, project, {{"expected_revision", config.at("revision")},
                {"enabled", false}, {"clear_private_webhook", true}});
            check(clear.value("ok", false) && !clear.value("has_private_webhook", true), "explicit secret clearing removes only the selected webhook");
            {
                auto guard = db.guard();
                db.setSetting(guard.token(), "release_announcement_webhook_config_" + std::to_string(project), "{malformed");
                before = db.raw(guard.token()).getTotalChanges();
            }
            check(!releaseAnnouncementConfig(&db, project).value("ok", true) &&
                !saveReleaseAnnouncementConfig(&db, project, {{"expected_revision", 0}, {"enabled", false}}).value("ok", true),
                "malformed stored configuration fails closed on reads and edits");
            {
                auto guard = db.guard();
                after = db.raw(guard.token()).getTotalChanges();
                check(before == after && db.getSetting(guard.token(), "release_announcement_webhook_config_" + std::to_string(project)) == "{malformed",
                    "malformed configuration is preserved without writes");
            }
        }
    } catch (const std::exception& error) {
        ++failures;
        std::printf("  [FAIL] release announcement fixture: %s\n", error.what());
    } catch (...) {
        ++failures; std::printf("  [FAIL] release announcement fixture: unknown failure\n");
    }
    std::error_code cleanup;
    // Only the uniquely created disposable fixture directory is removed.
    if (ownedCreated) fs::remove_all(temporary, cleanup);
    return failures;
}
} // namespace devhub
