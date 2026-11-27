#include "ReleaseAnnouncements.h"
#include "Db.h"
#include "PacketOps.h"
#include "devhub/Util.h"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <vector>

namespace devhub {
namespace {
using json = nlohmann::json;
constexpr size_t kMaxDocument = 256 * 1024;
constexpr size_t kMaxNotes = 16000;
constexpr size_t kMaxTemplate = 1000;
constexpr const char* kMarker = "[automated message]";

json failure(const char* text, const char* code = "invalid") {
    return {{"ok", false}, {"error", text}, {"code", code}};
}
bool exactKeys(const json& value, const std::set<std::string>& allowed) {
    if (!value.is_object()) return false;
    for (auto it = value.begin(); it != value.end(); ++it)
        if (!allowed.count(it.key())) return false;
    return true;
}
bool textField(const json& value, const char* key, size_t limit,
               std::string& result, bool required = true) {
    const auto it = value.find(key);
    if (it == value.end()) { result.clear(); return !required; }
    if (!it->is_string()) return false;
    result = it->get<std::string>();
    return result.size() <= limit && result.find('\0') == std::string::npos;
}
bool snowflake(const std::string& value) {
    if (value.empty() || value.size() > 20 || value[0] == '0') return false;
    std::uint64_t number = 0;
    for (char ch : value) {
        if (ch < '0' || ch > '9' ||
            number > ((std::numeric_limits<std::uint64_t>::max)() -
                      static_cast<unsigned>(ch - '0')) / 10) return false;
        number = number * 10 + static_cast<unsigned>(ch - '0');
    }
    return number != 0;
}
bool hex(const std::string& value, size_t length) {
    return value.size() == length &&
        std::all_of(value.begin(), value.end(), [](char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        });
}
bool positiveId(const json& value, const char* key, long long& result) {
    const auto it = value.find(key);
    if (it == value.end() || !it->is_number_integer()) return false;
    if (it->is_number_unsigned() && it->get<std::uint64_t>() >
        static_cast<std::uint64_t>((std::numeric_limits<long long>::max)())) return false;
    result = it->get<long long>();
    return result > 0;
}
bool revisionField(const json& value, const char* key, std::uint64_t& result) {
    const auto it = value.find(key);
    if (it == value.end() || !it->is_number_integer() ||
        (it->is_number_integer() && !it->is_number_unsigned() && it->get<long long>() < 0)) return false;
    result = it->get<std::uint64_t>();
    return result < (std::numeric_limits<std::uint64_t>::max)();
}
std::string configKey(long long id) {
    return "release_announcement_webhook_config_" + std::to_string(id);
}
std::string eventKey(long long project, long long release) {
    return "release_announcement_webhook_event_" + std::to_string(project) + "_" + std::to_string(release);
}
std::string pendingKey(long long project, long long release) {
    return "release_announcement_pending_" + std::to_string(project) + "_" + std::to_string(release);
}
json defaults(long long id) {
    return {{"schema", 1}, {"project_id", id}, {"revision", 0},
        {"enabled", false}, {"public_release", true},
        {"public_channel_id", ""}, {"private_channel_id", ""},
        {"public_webhook_url", ""}, {"private_webhook_url", ""},
        {"message_template", kMarker}, {"format", "plain"}};
}
bool validConfig(const json& value, long long id) {
    if (!exactKeys(value, {"schema", "project_id", "revision", "enabled", "public_release",
            "public_channel_id", "private_channel_id", "public_webhook_url", "private_webhook_url",
            "message_template", "format"}) || value.value("schema", 0) != 1 ||
        value.value("project_id", 0LL) != id || !value.at("enabled").is_boolean() ||
        !value.at("public_release").is_boolean()) return false;
    std::uint64_t revision = 0;
    if (!revisionField(value, "revision", revision)) return false;
    std::string pattern, format;
    if (!textField(value, "message_template", kMaxTemplate, pattern) ||
        !textField(value, "format", 16, format) ||
        (format != "plain" && format != "embed" && format != "codeblock")) return false;
    const size_t marker = pattern.find(kMarker);
    if (marker == std::string::npos || pattern.find(kMarker, marker + 1) != std::string::npos) return false;
    for (const char* route : {"public", "private"}) {
        std::string channel, url, webhookId, path;
        if (!textField(value, (std::string(route) + "_channel_id").c_str(), 20, channel) ||
            !textField(value, (std::string(route) + "_webhook_url").c_str(), 512, url) ||
            (!channel.empty() && !snowflake(channel)) ||
            (!url.empty() && !parseReleaseWebhookUrl(url, webhookId, path))) return false;
    }
    if (value.at("enabled").get<bool>()) {
        const std::string route = value.at("public_release").get<bool>() ? "public" : "private";
        if (value.at(route + "_channel_id").get<std::string>().empty() ||
            value.at(route + "_webhook_url").get<std::string>().empty()) return false;
    }
    return true;
}
json loadConfig(Db* db, Db::Held held, long long id) {
    const std::string raw = db->getSetting(held, configKey(id));
    if (raw.empty()) return defaults(id);
    if (raw.size() > 4096) throw std::runtime_error("invalid announcement configuration");
    json value = json::parse(raw);
    if (!validConfig(value, id)) throw std::runtime_error("invalid announcement configuration");
    return value;
}
json publicConfig(json value) {
    for (const char* route : {"public", "private"}) {
        const std::string prefix(route);
        const std::string url = value.at(prefix + "_webhook_url").get<std::string>();
        std::string id, path;
        if (!url.empty()) parseReleaseWebhookUrl(url, id, path);
        value.erase(prefix + "_webhook_url");
        value["has_" + prefix + "_webhook"] = !url.empty();
        value[prefix + "_webhook_id"] = id;
    }
    value["ok"] = true;
    return value;
}
json projectLocked(Db::Held held, Db* db, long long id) {
    SQLite::Statement query(db->raw(held), "SELECT id,name,path,github_url FROM projects WHERE id=?");
    query.bind(1, id);
    if (!query.executeStep()) return json();
    return Db::rowToJson(query);
}
std::string repository(const std::string& url) {
    std::string value = url;
    if (startsWith(value, "https://github.com/")) value.erase(0, 19);
    else if (startsWith(value, "git@github.com:")) value.erase(0, 15);
    if (endsWith(value, ".git")) value.resize(value.size() - 4);
    if (value.size() > 200 || std::count(value.begin(), value.end(), '/') != 1) return {};
    const size_t slash = value.find('/');
    if (slash == 0 || slash + 1 == value.size()) return {};
    if (!std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '/';
    })) return {};
    for (const auto& part : {value.substr(0, slash), value.substr(slash + 1)})
        if (part == "." || part == "..") return {};
    return toLower(value);
}
std::string urlEncode(const std::string& value) {
    static const char digits[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char ch : value) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~') result += static_cast<char>(ch);
        else { result += '%'; result += digits[ch >> 4]; result += digits[ch & 15]; }
    }
    return result;
}
bool releaseIdentity(const json& release, const std::string& repo, bool current) {
    long long id = 0;
    std::string tag, url, body, commit;
    if (!release.is_object() || !positiveId(release, "id", id) ||
        !textField(release, "tag", 256, tag) || tag.empty() ||
        !textField(release, "url", 1024, url) ||
        !textField(release, "body", 65536, body)) return false;
    const std::string prefix = "https://github.com/" + repo + "/releases/tag/";
    if (!startsWith(toLower(url.substr(0, prefix.size())), prefix) ||
        (url.substr(prefix.size()) != urlEncode(tag) &&
         url.substr(prefix.size()) != replaceAll(urlEncode(tag), "%2F", "/"))) return false;
    if (!current) return true;
    if (!textField(release, "commit", 64, commit, false)) return false;
    if (commit.empty() && !textField(release, "target_commitish", 64, commit)) return false;
    if (!hex(commit, 40) && !hex(commit, 64)) return false;
    std::string published;
    if (!release.contains("draft") || !release.at("draft").is_boolean() || release.at("draft").get<bool>() ||
        !textField(release, "published_at", 64, published) || published.empty()) return false;
    const auto assets = release.find("assets");
    if (assets == release.end() || !assets->is_array() || assets->empty() || assets->size() > 32) return false;
    std::set<std::string> names;
    for (const auto& asset : *assets) {
        long long assetId = 0, size = 0;
        std::string name, digest, assetUrl;
        if (!asset.is_object() || !positiveId(asset, "id", assetId) || !positiveId(asset, "size", size) ||
            !textField(asset, "name", 255, name) || name.empty() ||
            name.find_first_of("/\\\r\n") != std::string::npos || !names.insert(name).second ||
            !textField(asset, "sha256", 64, digest) || !hex(digest, 64) ||
            !textField(asset, "url", 2048, assetUrl)) return false;
        const std::string assetPrefix = "https://github.com/" + repo + "/releases/download/";
        if (toLower(assetUrl.substr(0, assetPrefix.size())) != assetPrefix ||
            (assetUrl.substr(assetPrefix.size()) != urlEncode(tag) + "/" + urlEncode(name) &&
             assetUrl.substr(assetPrefix.size()) != replaceAll(urlEncode(tag), "%2F", "/") + "/" + urlEncode(name))) return false;
    }
    return true;
}
json loadEvent(Db* db, Db::Held held, const std::string& key) {
    const std::string raw = db->getSetting(held, key);
    if (raw.empty()) return json();
    if (raw.size() > kMaxDocument) throw std::runtime_error("invalid announcement record");
    json event = json::parse(raw);
    long long project = 0, release = 0;
    std::uint64_t revision = 0;
    if (!event.is_object() || event.value("schema", 0) != 1 ||
        !positiveId(event, "project_id", project) || !positiveId(event, "release_id", release) ||
        key != eventKey(project, release) || !revisionField(event, "revision", revision) || revision == 0 ||
        !event.at("request").is_object() || !validConfig(event.at("config"), project) ||
        !event.at("message_ids").is_array() || event.at("message_ids").size() > 20 ||
        !event.at("cursor").is_number_unsigned()) throw std::runtime_error("invalid announcement record");
    static const std::set<std::string> statuses = {"pending", "sending", "delivered", "failed", "uncertain"};
    if (!statuses.count(event.value("status", "")) ||
        event.at("cursor").get<size_t>() != event.at("message_ids").size()) throw std::runtime_error("invalid announcement state");
    for (const auto& id : event.at("message_ids"))
        if (!id.is_string() || !snowflake(id.get<std::string>())) throw std::runtime_error("invalid announcement message");
    return event;
}
json publicEvent(const json& event) {
    const auto& messages = event.at("message_ids");
    const std::string route = event.at("config").at("public_release").get<bool>() ? "public" : "private";
    std::string webhookId, requestPath;
    parseReleaseWebhookUrl(event.at("config").at(route + "_webhook_url").get<std::string>(), webhookId, requestPath);
    return {{"project_id", event.at("project_id")}, {"release_id", std::to_string(event.at("release_id").get<long long>())},
        {"id", std::to_string(event.at("release_id").get<long long>())}, {"revision", event.at("revision")},
        {"status", event.at("status")}, {"tag", event.at("request").at("release").at("tag")},
        {"message_id", messages.empty() ? json("") : messages.back()}, {"message_ids", messages},
        {"channel_id", event.at("config").at(route + "_channel_id")}, {"webhook_id", webhookId},
        {"error", event.value("error", "")}, {"created_at", event.at("created_at")},
        {"updated_at", event.at("updated_at")}, {"public_release", event.at("config").at("public_release")}};
}
void saveEvent(Db* db, Db::Held held, const json& event) {
    const std::string text = event.dump();
    if (text.size() > kMaxDocument) throw std::runtime_error("announcement record too large");
    db->setSetting(held, eventKey(event.at("project_id").get<long long>(),
                                event.at("release_id").get<long long>()), text);
}
void removePending(Db* db, Db::Held held, long long project, long long release) {
    SQLite::Statement remove(db->raw(held), "DELETE FROM settings WHERE key=?");
    remove.bind(1, pendingKey(project, release)); remove.exec();
}
std::vector<std::string> chunks(std::string value, size_t capacity) {
    std::vector<std::string> result;
    while (!value.empty()) {
        size_t count = (std::min)(capacity, value.size());
        if (count < value.size()) {
            const size_t newline = value.rfind('\n', count - 1);
            if (newline != std::string::npos && newline > count / 2) count = newline + 1;
            else {
                while (count > 0 && (static_cast<unsigned char>(value[count]) & 0xc0) == 0x80) --count;
            }
        }
        result.push_back(value.substr(0, count));
        value.erase(0, count);
    }
    if (result.empty()) result.emplace_back();
    return result;
}
json allowedMentions(const std::string& text) {
    json roles = json::array();
    std::set<std::string> seen;
    for (size_t pos = 0; (pos = text.find("<@&", pos)) != std::string::npos; ++pos) {
        const size_t end = text.find('>', pos + 3);
        if (end == std::string::npos) break;
        const std::string id = text.substr(pos + 3, end - pos - 3);
        if (snowflake(id) && seen.insert(id).second) roles.push_back(id);
    }
    return {{"parse", json::array()}, {"roles", roles}, {"users", json::array()}, {"replied_user", false}};
}
} // namespace

bool parseReleaseWebhookUrl(const std::string& url, std::string& id,
                            std::string& requestPath) {
    id.clear(); requestPath.clear();
    constexpr const char* prefix = "https://discord.com/api/webhooks/";
    if (!startsWith(url, prefix) || url.size() > 512) return false;
    const size_t begin = std::string(prefix).size();
    const size_t slash = url.find('/', begin);
    if (slash == std::string::npos) return false;
    const std::string candidate = url.substr(begin, slash - begin);
    const std::string token = url.substr(slash + 1);
    if (!snowflake(candidate) || token.size() < 20 || token.size() > 200 ||
        !std::all_of(token.begin(), token.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '-';
        })) return false;
    id = candidate;
    requestPath = "/api/webhooks/" + candidate + "/" + token + "?wait=true";
    return true;
}

json releaseAnnouncementConfig(Db* db, long long projectId) {
    try {
        auto guard = db->guard();
        if (projectId <= 0 || projectLocked(guard.token(), db, projectId).is_null()) return failure("Project not found", "not_found");
        return publicConfig(loadConfig(db, guard.token(), projectId));
    } catch (...) { return failure("Stored release announcement configuration could not be read"); }
}

json saveReleaseAnnouncementConfig(Db* db, long long projectId, const json& body) {
    if (!exactKeys(body, {"expected_revision", "enabled", "public_release", "public_channel_id", "private_channel_id",
        "public_webhook_url", "private_webhook_url", "clear_public_webhook", "clear_private_webhook", "message_template", "format"}))
        return failure("Unexpected release announcement setting");
    try {
        std::uint64_t expected = 0;
        if (!revisionField(body, "expected_revision", expected)) return failure("expected_revision is required");
        auto guard = db->guard();
        if (projectId <= 0 || projectLocked(guard.token(), db, projectId).is_null()) return failure("Project not found", "not_found");
        json config = loadConfig(db, guard.token(), projectId);
        if (config.at("revision").get<std::uint64_t>() != expected) return failure("Configuration changed; reload before saving", "conflict");
        for (const char* key : {"enabled", "public_release", "public_channel_id", "private_channel_id", "message_template", "format"})
            if (body.contains(key)) config[key] = body.at(key);
        for (const char* route : {"public", "private"}) {
            const std::string key = std::string(route) + "_webhook_url";
            const std::string clear = "clear_" + std::string(route) + "_webhook";
            if (body.contains(clear) && !body.at(clear).is_boolean()) return failure("Webhook clear flag must be boolean");
            if (body.value(clear, false)) config[key] = "";
            if (body.contains(key)) {
                if (!body.at(key).is_string()) return failure("Webhook URL must be text");
                if (!body.at(key).get<std::string>().empty()) {
                    if (body.value(clear, false)) return failure("Cannot clear and replace a webhook together");
                    config[key] = body.at(key);
                }
            }
        }
        config["revision"] = expected + 1;
        if (!validConfig(config, projectId)) return failure("Invalid route, webhook, format, or template; use exactly one [automated message]");
        db->setSetting(guard.token(), configKey(projectId), config.dump());
        return publicConfig(config);
    } catch (...) { return failure("Release announcement configuration was not changed"); }
}

json previewReleaseAnnouncement(const json& config, const std::string& notes,
                                const std::string& tag, const std::string& releaseUrl) {
    try {
        std::string pattern, format;
        if (!textField(config, "message_template", kMaxTemplate, pattern) ||
            !textField(config, "format", 16, format) || notes.empty() || notes.size() > kMaxNotes ||
            normalizeUtf8(notes) != notes || notes.find('\0') != std::string::npos ||
            (format != "plain" && format != "embed" && format != "codeblock")) return failure("Invalid announcement preview input");
        const size_t marker = pattern.find(kMarker);
        if (marker == std::string::npos || pattern.find(kMarker, marker + 1) != std::string::npos) return failure("Template must contain exactly one [automated message]");
        const std::string before = pattern.substr(0, marker);
        const std::string after = pattern.substr(marker + std::string(kMarker).size());
        json messages = json::array();
        if (format == "embed") {
            auto parts = chunks(notes, 3900);
            for (size_t index = 0; index < parts.size(); ++index) {
                json embed = {{"title", chunks(tag, 200).front()}, {"description", parts[index]}};
                if (!releaseUrl.empty()) embed["url"] = releaseUrl;
                messages.push_back({{"content", index == 0 ? before + after : ""}, {"embeds", json::array({embed})}});
            }
        } else if (format == "codeblock") {
            if (notes.find("```") != std::string::npos) return failure("Code-block notes must be unfenced; use plain or embed format for existing code fences");
            const auto parts = chunks(notes, 950);
            for (size_t index = 0; index < parts.size(); ++index)
                messages.push_back({{"content", (index == 0 ? before : "") + "```\n" + parts[index] + "\n```" +
                    (index + 1 == parts.size() ? after : "")}});
        } else {
            for (const auto& part : chunks(before + notes + after, 1950)) messages.push_back({{"content", part}});
        }
        if (messages.size() > 20) return failure("Announcement needs too many messages");
        const json mentions = allowedMentions(pattern);
        for (auto& message : messages) message["allowed_mentions"] = mentions;
        return {{"ok", true}, {"messages", messages}};
    } catch (...) { return failure("Announcement preview could not be formatted"); }
}

json submitReleaseAnnouncement(Db* db, const json& body) {
    try {
        if (!exactKeys(body, {"schema", "project_id", "repository", "previous_release", "release", "generated_notes", "notes_source", "private_attachment"}) ||
            body.value("schema", "") != "xa.devhub.release-handoff/v1" || body.dump().size() > 192 * 1024) return failure("Invalid release handoff schema or size");
        long long projectId = 0, releaseId = 0;
        std::string repo, notes;
        if (!positiveId(body, "project_id", projectId) || !textField(body, "repository", 200, repo) ||
            repository(repo).empty() || !textField(body, "generated_notes", kMaxNotes, notes) || notes.empty() ||
            !body.contains("release") || !releaseIdentity(body.at("release"), repository(repo), true) ||
            !positiveId(body.at("release"), "id", releaseId) || !body.contains("previous_release")) return failure("Invalid published release identity, assets, or notes");
        repo = repository(repo);
        const auto& previous = body.at("previous_release");
        if (!previous.is_null() && (!releaseIdentity(previous, repo, false) || previous.at("id") == body.at("release").at("id"))) return failure("Invalid previous release baseline");
        const auto& provenance = body.at("notes_source");
        for (const char* key : {"old_body_sha256", "new_body_sha256", "skill_sha256"}) {
            std::string digest;
            if (!textField(provenance, key, 64, digest) || !hex(digest, 64)) return failure("Missing release-note provenance");
        }
        const auto oldHash = sha256Hex(previous.is_null() ? "" : previous.at("body").get<std::string>());
        const auto newHash = sha256Hex(body.at("release").at("body").get<std::string>());
        if (!oldHash.ok || !newHash.ok || provenance.at("old_body_sha256") != oldHash.hex ||
            provenance.at("new_body_sha256") != newHash.hex) return failure("Release-note provenance does not match the supplied releases");
        const auto identity = sha256Hex(body.dump());
        if (!identity.ok) return failure("Could not hash the release handoff");
        auto guard = db->guard();
        const json project = projectLocked(guard.token(), db, projectId);
        if (project.is_null()) return failure("Project not found", "not_found");
        if (repository(project.value("github_url", "")) != repo) return failure("Release repository does not match this project");
        const json existing = loadEvent(db, guard.token(), eventKey(projectId, releaseId));
        if (!existing.is_null()) {
            if (existing.value("source_hash", "") != identity.hex) return failure("This release already has a different handoff", "conflict");
            json result = publicEvent(existing); result["ok"] = true; result["duplicate"] = true; return result;
        }
        const json config = loadConfig(db, guard.token(), projectId);
        if (!config.at("enabled").get<bool>()) return failure("Release announcements are disabled for this project", "disabled");
        const bool isPublic = config.at("public_release").get<bool>();
        const bool attachment = body.contains("private_attachment") && !body.at("private_attachment").is_null();
        if (attachment == isPublic) return failure(isPublic ? "Public announcements cannot upload private artifacts" : "Private announcements require an explicit verified ZIP");
        if (attachment) {
            const auto& file = body.at("private_attachment");
            std::string path, filename, digest; long long size = 0;
            if (!exactKeys(file, {"path", "filename", "size", "sha256"}) ||
                !textField(file, "path", 32700, path) || path.empty() ||
                !textField(file, "filename", 255, filename) || filename.empty() ||
                filename.find_first_of("/\\\r\n\";") != std::string::npos ||
                std::any_of(filename.begin(), filename.end(), [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; }) ||
                !endsWith(toLower(filename), ".zip") ||
                !textField(file, "sha256", 64, digest) || !hex(digest, 64) ||
                !positiveId(file, "size", size) || size > 24 * 1024 * 1024) return failure("Invalid private ZIP identity or size (maximum 24 MiB)");
            if (project.value("path", "").empty()) return failure("Private delivery requires a project directory");
        }
        const json preview = previewReleaseAnnouncement(config, notes, body.at("release").at("tag").get<std::string>(), body.at("release").at("url").get<std::string>());
        if (!preview.value("ok", false)) return preview;
        SQLite::Statement pending(db->raw(guard.token()), "SELECT COUNT(*) FROM settings WHERE key GLOB ?");
        pending.bind(1, "release_announcement_pending_" + std::to_string(projectId) + "_*");
        pending.executeStep();
        if (pending.getColumn(0).getInt() >= 32) return failure("This project already has 32 pending announcements");
        const std::string now = nowIsoUtc();
        json event = {{"schema", 1}, {"project_id", projectId}, {"release_id", releaseId}, {"revision", 1},
            {"source_hash", identity.hex}, {"request", body}, {"config", config}, {"project_path", project.value("path", "")},
            {"status", "pending"}, {"cursor", size_t(0)}, {"message_ids", json::array()}, {"error", ""},
            {"created_at", now}, {"updated_at", now}};
        SQLite::Transaction transaction(db->raw(guard.token()));
        saveEvent(db, guard.token(), event);
        db->setSetting(guard.token(), pendingKey(projectId, releaseId), eventKey(projectId, releaseId));
        transaction.commit();
        json result = publicEvent(event); result["ok"] = true; result["duplicate"] = false; return result;
    } catch (...) { return failure("Release handoff was not accepted; verify its fields and stored configuration"); }
}

json listReleaseAnnouncements(Db* db, long long projectId, int limit) {
    try {
        auto guard = db->guard();
        if (projectId <= 0 || projectLocked(guard.token(), db, projectId).is_null()) return failure("Project not found", "not_found");
        SQLite::Statement query(db->raw(guard.token()), "SELECT key FROM settings WHERE key GLOB ? ORDER BY rowid DESC LIMIT ?");
        query.bind(1, "release_announcement_webhook_event_" + std::to_string(projectId) + "_*");
        query.bind(2, (std::max)(1, (std::min)(limit, 100)));
        json events = json::array();
        while (query.executeStep()) events.push_back(publicEvent(loadEvent(db, guard.token(), query.getColumn(0).getString())));
        return {{"ok", true}, {"events", events}};
    } catch (...) { return failure("Stored release announcement history could not be read"); }
}

json retryReleaseAnnouncement(Db* db, long long projectId, const std::string& releaseIdText,
                               std::uint64_t expectedRevision, bool acknowledgeUncertain) {
    try {
        if (!snowflake(releaseIdText)) return failure("Invalid release ID");
        const auto unsignedId = std::stoull(releaseIdText);
        if (unsignedId > static_cast<std::uint64_t>((std::numeric_limits<long long>::max)())) return failure("Invalid release ID");
        const long long releaseId = static_cast<long long>(unsignedId);
        auto guard = db->guard();
        json event = loadEvent(db, guard.token(), eventKey(projectId, releaseId));
        if (event.is_null()) return failure("Announcement not found", "not_found");
        if (event.at("revision").get<std::uint64_t>() != expectedRevision) return failure("Announcement changed; reload before retrying", "conflict");
        const std::string status = event.at("status").get<std::string>();
        if (status != "failed" && status != "uncertain") return failure("Only failed or uncertain deliveries can be retried");
        if (status == "uncertain" && !acknowledgeUncertain) return failure("Delivery may already exist; inspect Discord and acknowledge duplicate risk before retrying", "conflict");
        const json config = loadConfig(db, guard.token(), projectId);
        if (!config.at("enabled").get<bool>()) return failure("Release announcements are disabled");
        if (config.at("public_release") != event.at("config").at("public_release")) return failure("A retry cannot switch between public and private destinations", "conflict");
        json effectiveConfig = config, originalConfig = event.at("config");
        effectiveConfig.erase("revision"); originalConfig.erase("revision");
        if (!event.at("message_ids").empty() && effectiveConfig != originalConfig) return failure("Part of this announcement was delivered; restore its original configuration before retrying", "conflict");
        event["config"] = config;
        event["status"] = "pending"; event["error"] = "";
        event["revision"] = expectedRevision + 1; event["updated_at"] = nowIsoUtc();
        SQLite::Transaction transaction(db->raw(guard.token()));
        saveEvent(db, guard.token(), event);
        db->setSetting(guard.token(), pendingKey(projectId, releaseId), eventKey(projectId, releaseId));
        transaction.commit();
        json result = publicEvent(event); result["ok"] = true; return result;
    } catch (...) { return failure("Announcement retry was not saved"); }
}

bool claimReleaseAnnouncement(Db* db, ReleaseAnnouncementWork& work) {
    auto guard = db->guard();
    SQLite::Statement pending(db->raw(guard.token()), "SELECT value FROM settings WHERE key GLOB 'release_announcement_pending_*' ORDER BY rowid LIMIT 32");
    while (pending.executeStep()) {
        const std::string key = pending.getColumn(0).getString();
        json event = loadEvent(db, guard.token(), key);
        if (event.is_null() || event.value("status", "") != "pending") continue;
        const long long project = event.at("project_id").get<long long>();
        const long long release = event.at("release_id").get<long long>();
        const json config = loadConfig(db, guard.token(), project);
        const auto revision = event.at("revision").get<std::uint64_t>();
        if (!config.at("enabled").get<bool>() || config != event.at("config")) {
            event["status"] = "failed"; event["error"] = "Configuration changed before delivery; review and retry";
            event["revision"] = revision + 1; event["updated_at"] = nowIsoUtc();
            SQLite::Transaction transaction(db->raw(guard.token()));
            saveEvent(db, guard.token(), event); removePending(db, guard.token(), project, release);
            transaction.commit();
            continue;
        }
        event["status"] = "sending"; event["revision"] = revision + 1; event["updated_at"] = nowIsoUtc();
        saveEvent(db, guard.token(), event);
        work = {project, release, revision + 1, event};
        return true;
    }
    return false;
}

bool releaseAnnouncementWorkCurrent(Db* db, const ReleaseAnnouncementWork& work) {
    auto guard = db->guard();
    const json event = loadEvent(db, guard.token(), eventKey(work.projectId, work.releaseId));
    if (event.is_null() || event.value("status", "") != "sending" ||
        event.at("revision").get<std::uint64_t>() != work.revision ||
        loadConfig(db, guard.token(), work.projectId) != event.at("config")) return false;
    const json project = projectLocked(guard.token(), db, work.projectId);
    return !project.is_null() && repository(project.value("github_url", "")) ==
        repository(event.at("request").at("repository").get<std::string>()) &&
        project.value("path", "") == event.value("project_path", "");
}

void finishReleaseAnnouncement(Db* db, const ReleaseAnnouncementWork& work,
                               const std::string& status, const std::string& messageId,
                               const std::string& error) {
    auto guard = db->guard();
    json event = loadEvent(db, guard.token(), eventKey(work.projectId, work.releaseId));
    if (event.is_null() || event.value("status", "") != "sending" ||
        event.at("revision").get<std::uint64_t>() != work.revision) return;
    if (status == "sent" && snowflake(messageId)) {
        const auto& request = event.at("request");
        const json preview = previewReleaseAnnouncement(event.at("config"), request.at("generated_notes").get<std::string>(),
            request.at("release").at("tag").get<std::string>(), request.at("release").at("url").get<std::string>());
        if (!preview.value("ok", false)) throw std::runtime_error("invalid stored announcement format");
        event["message_ids"].push_back(messageId);
        const size_t cursor = event.at("cursor").get<size_t>() + 1;
        event["cursor"] = cursor;
        event["status"] = cursor == preview.at("messages").size() ? "delivered" : "pending";
        event["error"] = "";
    } else {
        event["status"] = status == "failed" ? "failed" : "uncertain";
        event["error"] = logSafe(redactHttpUrls(error), 512);
    }
    event["revision"] = work.revision + 1; event["updated_at"] = nowIsoUtc();
    SQLite::Transaction transaction(db->raw(guard.token()));
    saveEvent(db, guard.token(), event);
    if (event.at("status") != "pending") removePending(db, guard.token(), work.projectId, work.releaseId);
    transaction.commit();
}

void recoverInterruptedReleaseAnnouncements(Db* db) {
    auto guard = db->guard();
    SQLite::Statement pending(db->raw(guard.token()), "SELECT value FROM settings WHERE key GLOB 'release_announcement_pending_*'");
    std::vector<std::string> keys;
    while (pending.executeStep()) keys.push_back(pending.getColumn(0).getString());
    for (const auto& key : keys) {
        json event = loadEvent(db, guard.token(), key);
        if (event.is_null() || event.value("status", "") != "sending") continue;
        event["status"] = "uncertain";
        event["error"] = "DevHub stopped during delivery; inspect Discord before retrying";
        event["revision"] = event.at("revision").get<std::uint64_t>() + 1;
        event["updated_at"] = nowIsoUtc();
        SQLite::Transaction transaction(db->raw(guard.token()));
        saveEvent(db, guard.token(), event);
        removePending(db, guard.token(), event.at("project_id").get<long long>(), event.at("release_id").get<long long>());
        transaction.commit();
    }
}
} // namespace devhub
