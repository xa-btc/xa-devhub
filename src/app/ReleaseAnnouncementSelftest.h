#pragma once
namespace devhub {
// Disposable databases and injected HTTP only; this never starts a worker or
// opens a live webhook. Returns the number of failed native assertions.
int runReleaseAnnouncementSelftest();
}
