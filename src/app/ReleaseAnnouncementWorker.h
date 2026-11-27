#pragma once

#include "ReleaseAnnouncements.h"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace devhub {
class Db;

struct ReleaseAnnouncementSendResult {
    std::string state = "failed";
    std::string id;
    std::string error;
};
struct ReleaseAnnouncementDeliveryHooks {
    // Inert unless explicitly supplied by a disposable native fixture.
    std::function<ReleaseAnnouncementSendResult(
        const std::string&, const std::string&, const nlohmann::json&,
        const std::string&, const std::string&, bool&)> send;
    std::function<void()> beforeReceiptSave;
    std::function<bool()> cancelled;
};
void processReleaseAnnouncement(
    Db* db, const ReleaseAnnouncementWork& work,
    const ReleaseAnnouncementDeliveryHooks& hooks = {});

// One owned, joined delivery lane. Idle iterations only read the bounded pending
// index; no history rewrites, blind POST retries, or GitHub publication occurs.
class ReleaseAnnouncementWorker {
public:
    explicit ReleaseAnnouncementWorker(Db* db) : db_(db) {}
    ~ReleaseAnnouncementWorker() { stop(); }
    ReleaseAnnouncementWorker(const ReleaseAnnouncementWorker&) = delete;
    ReleaseAnnouncementWorker& operator=(const ReleaseAnnouncementWorker&) = delete;
    void start();
    void stop();
    void wake();

private:
    void run();
    Db* db_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopping_ = false;
    bool signaled_ = false;
    std::thread thread_;
};
} // namespace devhub
