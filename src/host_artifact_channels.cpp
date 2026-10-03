#include "host_artifact_channels.h"
#include <iostream>

namespace runner {
HostArtifactChannels::HostArtifactChannels(scrp::SessionContext session,
    telemetry::ConnectionSettings settings, artifact_transfer::Destination destination,
    std::wstring output, std::shared_ptr<scrp::ArtifactExportSchema> schema,
    Attach attach, std::function<bool()> healthy)
    : session_(std::move(session)), settings_(std::move(settings)), uploader_(std::move(destination)),
      output_(std::move(output)), schema_(std::move(schema)), attach_(std::move(attach)), healthy_(std::move(healthy)) {
    if (!schema_ || !attach_ || !healthy_) throw std::invalid_argument("Missing Artifact product services");
}
HostArtifactChannels::~HostArtifactChannels() { stop(); }
void HostArtifactChannels::connected(const scrp::Envelope& ack) {
    if (!schema_->artifact_allowed()) return;
    connection_ = ack.connection_id.asString();
    auto credential = schema_->telemetry_credential();
    adapter_ = std::make_shared<ArtifactCandidateAdapter>(std::make_shared<scrp::ExportTelemetrySchema>());
    telemetry::Context context{session_, settings_, [credential] { return credential; }, adapter_};
    telemetry::ClientOptions options;
    options.max_reconnects = 0; // A broken channel disables this export session.
    telemetry_ = std::make_unique<telemetry::TelemetryClient>(std::move(context), options);
    deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    telemetry_->start(); // No blocking handshake on the Control receive thread.
}
::control::Replies HostArtifactChannels::handle(const scrp::Envelope& request) {
    // Host must wait for Telemetry registration before requesting a reported
    // candidate. Invalid startup ordering is a protocol failure, never a grant
    // that can later be retried implicitly on the same upload ID.
    if (!exports_) throw std::runtime_error("Artifact channel is not ready");
    return exports_->handle(request);
}
std::vector<::control::DeferredReply> HostArtifactChannels::poll(bool gui_active) {
    if (blocked_) return exports_ ? exports_->poll() : std::vector<::control::DeferredReply>{};
    if (!gui_active) { block(); return exports_ ? exports_->poll() : std::vector<::control::DeferredReply>{}; }
    if (!telemetry_) return {};
    const auto status = telemetry_->snapshot();
    if (status.errors || status.expired_events || status.rejected_events || !healthy_()) degraded_ = true;
    if (exports_ && status.state != telemetry::ConnectionState::ready) degraded_ = true;
    if (degraded_) {
        if (!exports_) throw std::runtime_error("Artifact Telemetry startup failed");
        if (exports_) exports_->set_healthy(false);
        if (detector_) detector_->invalidate(ERROR_CONNECTION_ABORTED);
        return exports_ ? exports_->poll() : std::vector<::control::DeferredReply>{};
    }
    if (!exports_ && status.state == telemetry::ConnectionState::ready) {
        exports_ = std::make_unique<ArtifactExportSession>(output_,session_,connection_,
            [this](const scrp::ArtifactUploadRequest& request, artifact::VerifiedFile& source, const std::atomic<bool>& cancelled) {
                return uploader_.upload(request,source,cancelled);
            }, [this](const std::wstring& path, artifact::CandidateStatus& current) {
                return detector_ && detector_->get_status(path,current);
            });
        bridge_ = std::make_unique<CandidateTelemetry>(*telemetry_,adapter_,
            [this](const char* message) {
                std::cerr << message << '\n';
                // This diagnostic also covers the intentionally local-only
                // invalidation wire contract. Registry invalidation is separate.
            },4096,[this](const CandidateObservation& observation,const artifact::CandidateStatus& current,bool valid) {
                exports_->observe(observation,current,valid);
            });
        detector_ = std::make_unique<artifact::CandidateDetector>(output_,[this](const artifact::CandidateStatus& current) {
            bridge_->submit(current);
        });
        attach_([this](const artifact::OutputChange& event) { detector_->submit(event); });
        std::cout << "Artifact Telemetry READY; candidate reporting enabled\n" << std::flush;
    }
    if (!exports_ && std::chrono::steady_clock::now() >= deadline_)
        throw std::runtime_error("Artifact Telemetry startup timed out");
    return exports_ ? exports_->poll() : std::vector<::control::DeferredReply>{};
}
void HostArtifactChannels::block() noexcept {
    blocked_ = true;
    if (exports_) exports_->block();
}
void HostArtifactChannels::stop() {
    if (stopped_) return;
    block();
    attach_({}); // Synchronizes with notification delivery before producer join.
    if (detector_) detector_->stop();
    if (exports_) exports_->stop();
    if (telemetry_) telemetry_->stop();
    stopped_ = true;
}
}
