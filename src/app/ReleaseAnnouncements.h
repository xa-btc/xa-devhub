#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>

namespace devhub {
class Db;

// Webhook URLs are write-only. Empty URL fields preserve the existing secret;
// clear_public_webhook / clear_private_webhook explicitly remove it.
nlohmann::json releaseAnnouncementConfig(Db* db, long long projectId);
nlohmann::json saveReleaseAnnouncementConfig(
    Db* db, long long projectId, const nlohmann::json& body);
nlohmann::json submitReleaseAnnouncement(Db* db, const nlohmann::json& body);
nlohmann::json listReleaseAnnouncements(Db* db, long long projectId, int limit = 30);
nlohmann::json retryReleaseAnnouncement(
    Db* db, long long projectId, const std::string& releaseId,
    std::uint64_t expectedRevision, bool acknowledgeUncertain = false);

// Pure preview uses only public configuration; no network or persistence.
nlohmann::json previewReleaseAnnouncement(
    const nlohmann::json& config, const std::string& notes,
    const std::string& tag = "Preview", const std::string& releaseUrl = "");
bool parseReleaseWebhookUrl(const std::string& url, std::string& id,
                            std::string& requestPath);

// Worker-only values; secrets never leave the in-process worker boundary.
struct ReleaseAnnouncementWork {
    long long projectId = 0;
    long long releaseId = 0;
    std::uint64_t revision = 0;
    nlohmann::json record;
};
bool claimReleaseAnnouncement(Db* db, ReleaseAnnouncementWork& work);
bool releaseAnnouncementWorkCurrent(Db* db, const ReleaseAnnouncementWork& work);
void finishReleaseAnnouncement(Db* db, const ReleaseAnnouncementWork& work,
                               const std::string& status,
                               const std::string& messageId,
                               const std::string& error);
void recoverInterruptedReleaseAnnouncements(Db* db);

} // namespace devhub
