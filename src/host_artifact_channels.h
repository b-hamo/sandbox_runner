#pragma once
#include "artifact_export_session.h"
#include "artifact_candidate_adapter.h"
#include "protocol/scrp/artifact_export.h"
#include "transport/artifact/https_uploader.h"

namespace runner {
// The product owner starts Telemetry after the authenticated Control handshake.
// Watch notifications exist for GUI startup coverage; candidate production is
// attached only after CHANNEL_ACK. No initial scan or pre-ready replay is implied.
class HostArtifactChannels {
public:
    using Attach = std::function<void(std::function<void(const artifact::OutputChange&)>)>;
    HostArtifactChannels(scrp::SessionContext, telemetry::ConnectionSettings,
        artifact_transfer::Destination, std::wstring output,
        std::shared_ptr<scrp::ArtifactExportSchema>, Attach, std::function<bool()> healthy);
    ~HostArtifactChannels();
    void connected(const scrp::Envelope&);
    ::control::Replies handle(const scrp::Envelope&);
    std::vector<::control::DeferredReply> poll(bool gui_active);
    void block() noexcept;
    void stop(); // owner or termination thread, never the Receiver callback
    bool stopped() const { return stopped_; }
private:
    scrp::SessionContext session_;
    telemetry::ConnectionSettings settings_;
    artifact_transfer::HttpsUploader uploader_;
    std::wstring output_;
    std::string connection_;
    std::shared_ptr<scrp::ArtifactExportSchema> schema_;
    Attach attach_;
    std::function<bool()> healthy_;
    std::shared_ptr<ArtifactCandidateAdapter> adapter_;
    std::unique_ptr<telemetry::TelemetryClient> telemetry_;
    std::unique_ptr<ArtifactExportSession> exports_;
    std::unique_ptr<CandidateTelemetry> bridge_;
    std::unique_ptr<artifact::CandidateDetector> detector_;
    std::chrono::steady_clock::time_point deadline_{};
    std::atomic<bool> blocked_{false}, degraded_{false}, stopped_{false};
};
}
