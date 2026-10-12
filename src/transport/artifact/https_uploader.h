#pragma once
#include "artifact/file_stability.h"
#include "protocol/scrp/artifact_export.h"
#include <mutex>
#include <set>

namespace runner::artifact_transfer {
struct Destination {
    // Trusted bootstrap origin, with its independently configured Artifact port.
    // No path, credentials, query or fragment. The PUT path is fixed by v1.
    std::wstring origin;
    telemetry::TlsTrust trust;
};
class HttpsUploader {
public:
    explicit HttpsUploader(Destination);
    // Serial worker use. Holds no worker and never reopens a pathname. The
    // caller retains VerifiedFile (including directory handles) until return.
    // Each upload_id is consumed once, including a failed HTTP attempt.
    scrp::ArtifactTransferResult upload(const scrp::ArtifactUploadRequest&,
        artifact::VerifiedFile&, const std::atomic<bool>& cancelled) noexcept;
private:
    Destination destination_;
    std::wstring host_;
    unsigned short port_ = 0;
    std::mutex mutex_;
    std::set<std::string> consumed_;
};
} // namespace runner::artifact_transfer
