#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>

namespace devhub {
struct App;
struct ReleaseAnnouncementUi {
    long long projectId = 0;
    std::uint64_t revision = 0;
    bool loaded = false, enabled = false, publicRelease = true;
    bool clearPublic = false, clearPrivate = false;
    int format = 0, databaseChanges = -1;
    double nextRefresh = 0;
    char publicChannel[32]{}, privateChannel[32]{};
    char publicWebhook[512]{}, privateWebhook[512]{};
    char messageTemplate[4096] = "[automated message]";
    nlohmann::json config, history, preview, pendingRetry;
    std::string error;
};
void drawReleaseAnnouncements(App& app);
}
