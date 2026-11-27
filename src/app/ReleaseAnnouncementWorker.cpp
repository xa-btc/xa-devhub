#include "ReleaseAnnouncementWorker.h"
#include "ReleaseAnnouncements.h"
#include "Db.h"
#include "PacketOps.h"
#include "WinUtil.h"
#include "devhub/Util.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <vector>

namespace devhub {
namespace {
using json = nlohmann::json;
namespace fs = std::filesystem;
constexpr size_t kMaxZip = 24 * 1024 * 1024;
constexpr size_t kMaxResponse = 64 * 1024;

struct FileHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit FileHandle(HANDLE handle) : value(handle) {}
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    FileHandle(FileHandle&& other) noexcept : value(other.value) { other.value = INVALID_HANDLE_VALUE; }
    ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct HttpHandle {
    HINTERNET value = nullptr;
    explicit HttpHandle(HINTERNET handle) : value(handle) {}
    HttpHandle(const HttpHandle&) = delete;
    HttpHandle& operator=(const HttpHandle&) = delete;
    ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
};
bool equalPathPart(const fs::path& a, const fs::path& b) {
    const auto aa = a.native(), bb = b.native();
    return CompareStringOrdinal(aa.data(), static_cast<int>(aa.size()),
                                bb.data(), static_cast<int>(bb.size()), TRUE) == CSTR_EQUAL;
}
bool contained(const fs::path& root, const fs::path& file) {
    auto a = root.begin(), b = file.begin();
    for (; a != root.end(); ++a, ++b)
        if (b == file.end() || !equalPathPart(*a, *b)) return false;
    return b != file.end();
}
bool validComponents(const fs::path& path) {
    if (!path.is_absolute() || path.native().size() > 32700 ||
        path.native().rfind(L"\\\\", 0) == 0) return false;
    for (const auto& part : path.relative_path()) {
        const std::wstring value = part.native();
        if (value.empty() || value == L"." || value == L".." ||
            value.back() == L'.' || value.back() == L' ' ||
            value.find_first_of(L":*?\"<>|") != std::wstring::npos) return false;
    }
    return true;
}
std::string readPrivateZip(const ReleaseAnnouncementWork& work) {
    const auto& file = work.record.at("request").at("private_attachment");
    const fs::path root(widen(work.record.at("project_path").get<std::string>()));
    const fs::path path(widen(file.at("path").get<std::string>()));
    if (!validComponents(root) || !validComponents(path) || !contained(root, path) ||
        narrow(path.filename().native()) != file.at("filename").get<std::string>())
        throw std::runtime_error("Private ZIP must be an exact file inside the configured project directory");
    // Lock every traversed directory, including the drive and project root,
    // without FILE_SHARE_DELETE. Reject junctions instead of following them.
    std::vector<FileHandle> directories;
    fs::path current = path.root_path();
    const auto lockDirectory = [&](const fs::path& directory) {
        FileHandle handle(CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        FILE_ATTRIBUTE_TAG_INFO info{};
        if (handle.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(handle.value,
                FileAttributeTagInfo, &info, sizeof(info)) ||
            !(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Private ZIP directory could not be locked or is redirected");
        directories.push_back(std::move(handle));
    };
    lockDirectory(current);
    for (const auto& part : path.parent_path().relative_path()) {
        current /= part;
        lockDirectory(current);
    }
    FileHandle handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    FILE_ATTRIBUTE_TAG_INFO info{};
    LARGE_INTEGER length{};
    if (handle.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(handle.value,
            FileAttributeTagInfo, &info, sizeof(info)) ||
        (info.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        GetFileType(handle.value) != FILE_TYPE_DISK || !GetFileSizeEx(handle.value, &length) ||
        length.QuadPart < 22 || length.QuadPart > static_cast<long long>(kMaxZip) ||
        length.QuadPart != file.at("size").get<long long>())
        throw std::runtime_error("Private ZIP is unavailable, redirected, or has changed size");
    std::string bytes(static_cast<size_t>(length.QuadPart), '\0');
    size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD count = 0;
        if (!ReadFile(handle.value, bytes.data() + offset,
            static_cast<DWORD>((std::min)(size_t(64 * 1024), bytes.size() - offset)), &count, nullptr) || count == 0)
            throw std::runtime_error("Private ZIP could not be completely read");
        offset += count;
    }
    const auto digest = sha256Hex(bytes);
    if (!digest.ok || digest.hex != file.at("sha256").get<std::string>() ||
        (bytes.compare(0, 4, "PK\x03\x04", 4) != 0 && bytes.compare(0, 4, "PK\x05\x06", 4) != 0))
        throw std::runtime_error("Private ZIP bytes do not match the verified release artifact");
    return bytes;
}
bool expired(const std::chrono::steady_clock::time_point& deadline,
              const std::function<bool()>& cancelled) {
    return std::chrono::steady_clock::now() >= deadline || (cancelled && cancelled());
}
bool verifyWebhookTarget(HINTERNET connection, const std::string& path,
                          const std::string& expectedWebhook, const std::string& expectedChannel,
                          const std::chrono::steady_clock::time_point& deadline,
                          const std::function<bool()>& cancelled) {
    const std::string target = path.substr(0, path.find('?'));
    HttpHandle request(WinHttpOpenRequest(connection, L"GET", widen(target).c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request.value) return false;
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy))) return false;
    policy = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_AUTOLOGON_POLICY, &policy, sizeof(policy))) return false;
    if (expired(deadline, cancelled) || !WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || expired(deadline, cancelled) ||
            !WinHttpReceiveResponse(request.value, nullptr)) return false;
    DWORD status = 0, length = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX) || status != 200) return false;
    std::string response;
    for (;;) {
        if (expired(deadline, cancelled)) return false;
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.value, &available)) return false;
        if (!available) break;
        if (available > kMaxResponse - response.size()) return false;
        std::string part(available, '\0'); DWORD received = 0;
        if (!WinHttpReadData(request.value, part.data(), available, &received) || !received) return false;
        response.append(part.data(), received);
    }
    try {
        const json metadata = json::parse(response);
        return metadata.value("id", "") == expectedWebhook && metadata.value("channel_id", "") == expectedChannel;
    } catch (...) { return false; }
}
ReleaseAnnouncementSendResult postWebhook(const std::string& url, const std::string& expectedChannel,
                       json payload, const std::string& filename, const std::string& bytes, bool& attempted,
                       const std::function<bool()>& cancelled) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(40);
    std::string expectedWebhook, path;
    if (!parseReleaseWebhookUrl(url, expectedWebhook, path)) return {"failed", "", "Stored webhook URL is invalid"};
    std::string body;
    std::wstring contentType = L"Content-Type: application/json\r\n";
    if (bytes.empty()) body = payload.dump();
    else {
        payload["attachments"] = json::array({{{"id", 0}, {"filename", filename}}});
        const auto hash = sha256Hex(bytes + payload.dump());
        if (!hash.ok) return {"failed", "", "Could not prepare the private attachment"};
        std::string boundary = "devhub-release-" + hash.hex;
        while (bytes.find(boundary) != std::string::npos) boundary += 'x';
        contentType = L"Content-Type: multipart/form-data; boundary=" + widen(boundary) + L"\r\n";
        body = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"payload_json\"\r\nContent-Type: application/json\r\n\r\n" + payload.dump() +
            "\r\n--" + boundary + "\r\nContent-Disposition: form-data; name=\"files[0]\"; filename=\"" + filename +
            "\"\r\nContent-Type: application/zip\r\n\r\n" + bytes + "\r\n--" + boundary + "--\r\n";
    }
    HttpHandle session(WinHttpOpen(L"XADevHub/ReleaseAnnouncements", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.value) return {"failed", "", "Could not open the Discord HTTP session"};
    if (!WinHttpSetTimeouts(session.value, 10000, 10000, 10000, 10000))
        return {"failed", "", "Could not bound the Discord HTTP timeouts"};
    HttpHandle connection(WinHttpConnect(session.value, L"discord.com", INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection.value) return {"failed", "", "Could not open the Discord connection"};
    if (!verifyWebhookTarget(connection.value, path, expectedWebhook, expectedChannel, deadline, cancelled))
        return {"failed", "", "Webhook metadata did not verify the configured channel and identity; no message was sent"};
    HttpHandle request(WinHttpOpenRequest(connection.value, L"POST", widen(path).c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request.value) return {"failed", "", "Could not create the Discord request"};
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy)))
        return {"failed", "", "Could not disable webhook redirects"};
    policy = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_AUTOLOGON_POLICY, &policy, sizeof(policy)))
        return {"failed", "", "Could not isolate webhook authentication"};
    // From this point a missing receipt is ambiguous: never auto-repeat POST.
    if (expired(deadline, cancelled)) return {"failed", "", "Announcement stopped before sending"};
    attempted = true;
    if (!WinHttpSendRequest(request.value, contentType.c_str(), static_cast<DWORD>(contentType.size()),
            body.data(), static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) || expired(deadline, cancelled) ||
        !WinHttpReceiveResponse(request.value, nullptr))
        return {"uncertain", "", "Discord delivery receipt was lost; inspect the channel before retrying"};
    DWORD status = 0, length = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX))
        return {"uncertain", "", "Discord returned no readable HTTP status; inspect the channel"};
    if (status < 200 || status >= 300) {
        const bool rejected = status >= 400 && status < 500;
        return {rejected ? "failed" : "uncertain", "", "Discord returned HTTP " + std::to_string(status) +
            (rejected ? "; correct the webhook or permissions, then retry" : "; inspect the channel before retrying")};
    }
    std::string response;
    for (;;) {
        if (expired(deadline, cancelled)) return {"uncertain", "", "Discord receipt deadline expired; inspect the channel"};
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.value, &available)) return {"uncertain", "", "Discord receipt was incomplete"};
        if (!available) break;
        if (available > kMaxResponse - response.size()) return {"uncertain", "", "Discord receipt exceeded the response limit"};
        std::string part(available, '\0'); DWORD received = 0;
        if (!WinHttpReadData(request.value, part.data(), available, &received) || !received)
            return {"uncertain", "", "Discord receipt was incomplete"};
        response.append(part.data(), received);
    }
    try {
        const json message = json::parse(response);
        std::string id = message.value("id", "");
        std::string checkedId, unused;
        if (!parseReleaseWebhookUrl("https://discord.com/api/webhooks/" + id + "/abcdefghijklmnopqrst", checkedId, unused) ||
            message.value("webhook_id", "") != expectedWebhook || message.value("channel_id", "") != expectedChannel)
            return {"uncertain", "", "Discord receipt identity differs from the configured channel/webhook"};
        if (!bytes.empty()) {
            const auto& attachments = message.at("attachments");
            if (!attachments.is_array() || attachments.size() != 1 ||
                attachments[0].value("filename", "") != filename ||
                attachments[0].value("size", size_t(0)) != bytes.size())
                return {"uncertain", "", "Discord receipt does not confirm the exact private ZIP"};
        }
        return {"sent", id, ""};
    } catch (...) { return {"uncertain", "", "Discord receipt could not be verified; inspect the channel"}; }
}
void deliver(Db* db, const ReleaseAnnouncementWork& work,
             const ReleaseAnnouncementDeliveryHooks& hooks, bool& attempted) {
    const auto& record = work.record;
    const auto& config = record.at("config");
    const auto& source = record.at("request");
    const auto& release = source.at("release");
    const auto preview = previewReleaseAnnouncement(config, source.at("generated_notes").get<std::string>(),
        release.at("tag").get<std::string>(), release.at("url").get<std::string>());
    const size_t cursor = record.at("cursor").get<size_t>();
    if (!preview.value("ok", false) || cursor >= preview.at("messages").size()) {
        finishReleaseAnnouncement(db, work, "failed", "", "Stored announcement could not be formatted"); return;
    }
    const bool isPublic = config.at("public_release").get<bool>();
    std::string bytes, filename;
    if (!isPublic) {
        // Verify before starting and again immediately before attaching the
        // exact bytes; middle text parts do not reread the ZIP unnecessarily.
        if (cursor == 0 || cursor + 1 == preview.at("messages").size()) bytes = readPrivateZip(work);
        if (cursor + 1 == preview.at("messages").size()) filename = source.at("private_attachment").at("filename").get<std::string>();
        else bytes.clear();
    }
    if (!releaseAnnouncementWorkCurrent(db, work)) {
        finishReleaseAnnouncement(db, work, "failed", "", "Project or announcement configuration changed before delivery"); return;
    }
    const std::string route = isPublic ? "public" : "private";
    const auto result = hooks.send
        ? hooks.send(config.at(route + "_webhook_url").get<std::string>(), config.at(route + "_channel_id").get<std::string>(),
                     preview.at("messages")[cursor], filename, bytes, attempted)
        : postWebhook(config.at(route + "_webhook_url").get<std::string>(), config.at(route + "_channel_id").get<std::string>(),
                      preview.at("messages")[cursor], filename, bytes, attempted, hooks.cancelled);
    if (hooks.beforeReceiptSave) hooks.beforeReceiptSave();
    finishReleaseAnnouncement(db, work, result.state, result.id, result.error);
}
} // namespace

void processReleaseAnnouncement(Db* db, const ReleaseAnnouncementWork& work,
                                const ReleaseAnnouncementDeliveryHooks& hooks) {
    bool attempted = false;
    try { deliver(db, work, hooks, attempted); }
    catch (...) {
        // A local preparation failure is safe to retry. Once POST can have
        // reached Discord, even a database write failure requires inspection.
        finishReleaseAnnouncement(db, work, attempted ? "uncertain" : "failed", "",
            attempted ? "Delivery may exist but its receipt could not be saved; inspect Discord before retrying" :
                        "Announcement preparation failed; verify the artifact, project, and configuration");
    }
}

void ReleaseAnnouncementWorker::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (thread_.joinable()) return;
    stopping_ = false; signaled_ = true;
    thread_ = std::thread([this] { run(); });
}
void ReleaseAnnouncementWorker::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true; signaled_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}
void ReleaseAnnouncementWorker::wake() {
    { std::lock_guard<std::mutex> lock(mutex_); signaled_ = true; }
    cv_.notify_one();
}
void ReleaseAnnouncementWorker::run() {
    std::string lastError;
    try { recoverInterruptedReleaseAnnouncements(db_); }
    catch (...) { appLog("[release] Interrupted announcement recovery needs inspection"); }
    long long seenChanges = -1, seenVersion = -1;
    for (;;) {
        bool forced = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::seconds(1), [this] { return stopping_ || signaled_; });
            if (stopping_) return;
            forced = signaled_; signaled_ = false;
        }
        ReleaseAnnouncementWork work;
        try {
            long long changes = 0, version = 0;
            {
                auto guard = db_->guard();
                changes = db_->raw(guard.token()).getTotalChanges();
                SQLite::Statement query(db_->raw(guard.token()), "PRAGMA data_version");
                if (!query.executeStep()) throw std::runtime_error("cannot read database revision");
                version = query.getColumn(0).getInt64();
            }
            if (!forced && changes == seenChanges && version == seenVersion) continue;
            seenChanges = changes; seenVersion = version;
            if (!claimReleaseAnnouncement(db_, work)) continue;
            ReleaseAnnouncementDeliveryHooks hooks;
            hooks.cancelled = [this] { std::lock_guard<std::mutex> lock(mutex_); return stopping_; };
            processReleaseAnnouncement(db_, work, hooks);
            lastError.clear();
            // Each successful part is durable before the next part can begin.
            wake();
        } catch (const std::exception& error) {
            const std::string safe = logSafe(redactHttpUrls(error.what()), 256);
            if (work.projectId != 0) {
                try { finishReleaseAnnouncement(db_, work, "uncertain", "", "Delivery state could not be saved; inspect Discord before retrying"); }
                catch (...) { /* Persisted sending state becomes uncertain after restart. */ }
            }
            if (safe != lastError) { appLog("[release] " + safe); lastError = safe; }
        } catch (...) {
            if (lastError != "unknown") appLog("[release] Announcement delivery needs inspection");
            lastError = "unknown";
        }
    }
}
} // namespace devhub
