#include "host_gui.h"
#include "gui_session.h"
#include "artifact/output_watcher.h"
#include "artifact_export_session.h"
#include "artifact_candidate_adapter.h"
#include "protocol/scrp/artifact_export.h"
#include "transport/artifact/https_uploader.h"
#include "host_artifact_channels.h"
#include "transport/observation/https_uploader.h"
#include "control/control_util.h"
#include <wincrypt.h>
#include <iostream>
#include <thread>

namespace runner {
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::invalid_argument(message); }
std::string text(const Json::Value& v) {
    require(v.isString() && !v.asString().empty() && v.asString().size() <= 16384 &&
            v.asString().find('\0') == std::string::npos, "Invalid Host bootstrap text");
    return v.asString();
}
std::chrono::system_clock::time_point utc(const std::string& s) {
    scrp::validate_utc_timestamp(s);
    SYSTEMTIME st{};st.wYear=std::stoi(s.substr(0,4));st.wMonth=std::stoi(s.substr(5,2));st.wDay=std::stoi(s.substr(8,2));
    st.wHour=std::stoi(s.substr(11,2));st.wMinute=std::stoi(s.substr(14,2));st.wSecond=std::stoi(s.substr(17,2));
    if(s[19]=='.') {auto fraction=s.substr(20,s.size()-21);fraction.append(3,'0');st.wMilliseconds=std::stoi(fraction.substr(0,3));}
    FILETIME ft{};require(SystemTimeToFileTime(&st,&ft), "Invalid Host bootstrap expiry");
    const auto ticks=(std::uint64_t(ft.dwHighDateTime)<<32)|ft.dwLowDateTime;
    require(ticks>=116444736000000000ULL, "Invalid Host bootstrap expiry");
    const auto ms=(ticks-116444736000000000ULL)/10000;
    require(ms < static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::duration::max()).count()), "Host timestamp out of range");
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}
Json::Value bootstrap(const std::wstring& path) {
    std::vector<char> buffer(65537);
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(file!=INVALID_HANDLE_VALUE,"Cannot open Host bootstrap");
    DWORD n=0;
    const bool ok=ReadFile(file,buffer.data(),static_cast<DWORD>(buffer.size()),&n,nullptr);CloseHandle(file);
    require(ok && n<=65536,"Cannot read Host bootstrap or size exceeded");
    auto root=scrp::parse_json(std::string(buffer.data(),n),65536);
    const std::vector<std::string> fields={"bootstrap_version","session_id","runtime_id","generation","host","port","path","token","token_expires_at","host_certificate_pem","observation_upload"};
    require(root.isObject(),"Unsupported Host bootstrap fields");
    const bool exporting=root.isMember("control_contract") || root.isMember("artifact_upload");
    require(!exporting || (root["control_contract"]=="artifact-export-v1" && root.isMember("artifact_upload")),
            "Artifact profile and upload settings must be selected together");
    const auto expected_fields=fields.size()+(root.isMember("host_certificate_sha256") ? 1u : 0u)+(exporting ? 2u : 0u);
    require(root.size()==expected_fields,"Unsupported Host bootstrap fields");
    for(const auto& f:fields)require(root.isMember(f),"Missing Host bootstrap field");
    require(root["bootstrap_version"]=="1.0" && root["path"]=="/scrp/v1/control", "Unsupported Host bootstrap version/path");
    return root;
}
unsigned port(const Json::Value& v) {
    require((v.type()==Json::intValue || v.type()==Json::uintValue) && v.isUInt() && v.asUInt()>0 && v.asUInt()<=65535,"Invalid Host port");
    return v.asUInt();
}
std::wstring ipv4(const std::wstring& address) {
    require(!address.empty() && address.size()<=15,"An explicit Host IPv4 address is required");
    unsigned parts=0;std::size_t start=0;
    for(std::size_t i=0;i<=address.size();++i) {
        if(i==address.size() || address[i]==L'.') {
            require(i>start && i-start<=3,"Invalid Host IPv4 address");
            auto part=address.substr(start,i-start);
            require(part.find_first_not_of(L"0123456789")==std::wstring::npos && std::stoi(part)<=255 &&
                    (part.size()==1 || part[0]!=L'0'),"Invalid Host IPv4 address");
            ++parts;start=i+1;
        }
    }
    require(parts==4 && address!=L"0.0.0.0","Invalid Host IPv4 address");return address;
}
struct Monitor {
    HANDLE stop=CreateEventW(nullptr,TRUE,FALSE,nullptr), ready=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::atomic<bool> healthy{false};std::atomic<DWORD> error{ERROR_SUCCESS};std::atomic<unsigned> changes{0};std::thread worker;
    std::mutex sink_mutex;
    std::function<void(const artifact::OutputChange&)> sink;
    void attach(std::function<void(const artifact::OutputChange&)> value) {
        std::lock_guard<std::mutex> lock(sink_mutex); sink=std::move(value);
    }
    void finish(){if(stop)SetEvent(stop);if(worker.joinable())worker.join();}
    ~Monitor(){finish();if(ready)CloseHandle(ready);if(stop)CloseHandle(stop);}
    void start(const std::wstring& output,HANDLE external_stop) {
        require(stop && ready,"Cannot create Output monitoring events");
        worker=std::thread([&,output,external_stop]{
            DWORD result=ERROR_GEN_FAILURE;
            try { result=artifact::watch_output(output,stop,[&](const artifact::OutputChange& e){
                if(e.kind==artifact::ChangeKind::ready){healthy=true;SetEvent(ready);}else ++changes;
                std::lock_guard<std::mutex> lock(sink_mutex);
                if(sink)sink(e);
            }); } catch(...) {}
            healthy=false;error=result;
            if(WaitForSingleObject(stop,0)!=WAIT_OBJECT_0)SetEvent(external_stop);
        });
        HANDLE waits[]={external_stop,ready};
        require(WaitForMultipleObjects(2,waits,FALSE,5000)==WAIT_OBJECT_0+1 && healthy,"Output monitor did not become ready");
    }
};
}
DWORD run_host_gui(const std::wstring& file,const std::wstring& override_address,HANDLE stop) {
    require(stop && WaitForSingleObject(stop,0)==WAIT_TIMEOUT,"Host GUI startup cancelled");
    const auto b=bootstrap(file);
    std::wstring address=override_address;
    if(!b["host"].isNull()) {
        auto host=control::util::utf8_to_wide(text(b["host"]));require(bool(host),"Invalid bootstrap Host address");
        require(address.empty() || address==*host,"Host address disagrees with bootstrap");address=*host;
    }
    address=ipv4(address);
    require((b["generation"].type()==Json::uintValue || b["generation"].type()==Json::intValue) &&
            b["generation"].isUInt64() && b["generation"].asUInt64()>0,"Invalid Host generation");
    ::control::Context ctx;ctx.session={text(b["session_id"]),text(b["runtime_id"]),b["generation"].asUInt64()};
    scrp::validate_session(ctx.session);
    ctx.connection.endpoint=L"wss://"+address+L":"+std::to_wstring(port(b["port"]))+L"/scrp/v1/control";
    const auto pem=text(b["host_certificate_pem"]);DWORD size=0;
    require(CryptStringToBinaryA(pem.c_str(),static_cast<DWORD>(pem.size()),CRYPT_STRING_BASE64HEADER,nullptr,&size,nullptr,nullptr) && size>0 && size<=16384,"Invalid bootstrap certificate");
    std::vector<std::uint8_t> der(size);
    require(CryptStringToBinaryA(pem.c_str(),static_cast<DWORD>(pem.size()),CRYPT_STRING_BASE64HEADER,der.data(),&size,nullptr,nullptr),"Invalid bootstrap certificate");
    ctx.connection.trust.leaf_sha256=control::util::sha256_hex(der.data(),size);
    require(ctx.connection.trust.leaf_sha256.size()==64,"Cannot hash bootstrap certificate");
    // Older Host bootstrap v1.0 omits this field. If supplied, cross-check
    // against the certificate itself; never replace the locally derived pin.
    if(b.isMember("host_certificate_sha256")) {
        const auto& supplied=b["host_certificate_sha256"];
        require(supplied.isString(),"Invalid bootstrap certificate SHA-256");
        const auto fingerprint=supplied.asString();
        require(fingerprint.size()==64 && fingerprint.find_first_not_of("0123456789abcdef")==std::string::npos,
                "Invalid bootstrap certificate SHA-256");
        require(fingerprint==ctx.connection.trust.leaf_sha256,"Bootstrap certificate SHA-256 mismatch");
    }
    auto token=control::util::utf8_to_wide(text(b["token"]));require(bool(token),"Invalid bootstrap token");
    telemetry::ChannelCredential credential{L"Authorization",L"Bearer "+*token,utc(text(b["token_expires_at"]))};
    telemetry::validate_credential(credential);telemetry::validate_connection_settings(ctx.connection);
    ctx.credential=[credential]{return credential;};
    const auto& upload=b["observation_upload"];
    require(upload.isObject() && upload.size()==2 && upload["path"]=="/scrp/v1/observations/","Unsupported observation upload settings");
    observation::Destination destination{L"https://"+address+L":"+std::to_wstring(port(upload["port"])),201,
        std::chrono::milliseconds(5000),observation::AuthorizationMode::Host6055UploadId};
    destination.trust=ctx.connection.trust;
    observation::HttpsUploader uploader(destination);
    const bool exporting=b.isMember("control_contract");
    artifact_transfer::Destination artifacts;
    if(exporting) {
        const auto& settings=b["artifact_upload"];
        require(settings.isObject() && settings.size()==2 && settings["path"]=="/scrp/v1/artifacts/",
                "Unsupported Artifact upload settings");
        artifacts.origin=L"https://"+address+L":"+std::to_wstring(port(settings["port"]));
        artifacts.trust=ctx.connection.trust;
    }
    const std::wstring base=L"C:\\RunnerWorkspace\\Sessions";
    require(artifact::prepare_output_directory(base)==ERROR_SUCCESS,"Cannot prepare Guest workspace");
    Monitor monitor;
    runtime::SandboxRuntime::Config config;
    config.binding={ctx.session.session_id,ctx.session.runtime_id,ctx.session.generation};config.guest_workspace_base=base;
    config.startup_authority=runtime::SandboxRuntime::StartupAuthority::Host6055;
    config.output_monitor_healthy=[&]{return monitor.healthy.load();};
    GuiSession gui(config,[&](const scrp::Envelope& e){
        runtime::HostGrant grant;grant.context={config.binding,e.connection_id.asString()};
        grant.policy_version="POL-0.1.0";grant.gui_observe=true;grant.gui_input=true;grant.lease=std::chrono::seconds(15);
        return grant; // Host Broker owns startup/input admission; no invented verification bools.
    },[&](const scrp::Envelope& e,const control::Observation& png,const std::atomic<bool>& cancelled){
        observation::UploadGrant grant;grant.session=e.session;grant.connection_id=e.connection_id.asString();
        grant.task_id=e.task_id.asString();grant.action_id=e.action_id.asString();grant.upload_id=e.payload["upload_id"].asString();
        grant.expires_at=std::min(utc(e.timestamp)+std::chrono::seconds(30),std::chrono::system_clock::now()+std::chrono::seconds(30));
        grant.max_bytes=8u*1024u*1024u;grant.max_pixels=16000000;
        return uploader.upload(grant,e,png,cancelled);
    });
    monitor.start(gui.workspace().output.wstring(),stop);
    std::cout<<"Host GUI profile 6055cc6: Output monitor ready; Host owns startup/input admission\n"<<std::flush;
    DWORD result=ERROR_SUCCESS;
    if(!exporting) result=run_control_session(std::move(ctx),stop,&gui);
    else {
        auto schema=std::make_shared<scrp::ArtifactExportSchema>(gui.schema());
        auto settings=ctx.connection;
        settings.endpoint=L"wss://"+address+L":"+std::to_wstring(port(b["port"]))+L"/scrp/v1/telemetry";
        HostArtifactChannels channels(ctx.session,std::move(settings),std::move(artifacts),
            gui.workspace().output.wstring(),schema,
            [&](std::function<void(const artifact::OutputChange&)> sink){monitor.attach(std::move(sink));},
            [&]{return monitor.healthy.load();});
        ctx.schema=schema;
        auto handlers=gui.handlers();
        const auto terminate=handlers.at("TERMINATE");
        std::thread artifact_stopper;
        bool terminating=false;
        handlers["ARTIFACT_REQUEST"]=[&](const scrp::Envelope& e){return channels.handle(e);};
        handlers["TERMINATE"]=[&](const scrp::Envelope& e){
            if(!terminating) {
                terminating=true;channels.block();
                artifact_stopper=std::thread([&]{channels.stop();});
            }
            return terminate(e);
        };
        auto gui_hooks=gui.hooks();
        std::optional<::control::DeferredReply> held_terminate;
        ::control::Hooks hooks;
        hooks.connected=[&](const scrp::Envelope& e){gui_hooks.connected(e);channels.connected(e);};
        hooks.poll=[&]{
            auto pending=gui_hooks.poll();
            std::vector<::control::DeferredReply> ready;
            for(auto& reply:pending) {
                if(reply.finish)held_terminate=std::move(reply);
                else ready.push_back(std::move(reply));
            }
            auto uploaded=channels.poll(gui.active());
            for(auto& reply:uploaded)ready.push_back(std::move(reply));
            // TERMINATE_RESULT is emitted only after Artifact cancellation,
            // producer/uploader joins and their final results have been drained.
            if(held_terminate && channels.stopped()) {
                ready.push_back(std::move(*held_terminate));held_terminate.reset();
            }
            return ready;
        };
        hooks.disconnected=[&]{gui_hooks.disconnected();channels.block();};
        ::control::Receiver receiver(std::move(ctx),std::move(handlers),{},telemetry::make_winhttp_websocket,std::move(hooks));
        std::atomic<bool> cancelled{false},done{false};
        std::thread control_worker([&]{receiver.run(cancelled);done=true;});
        while(!done) {
            const auto wait=WaitForSingleObject(stop,20);
            if(wait==WAIT_OBJECT_0){gui.block();cancelled=true;break;}
            if(wait==WAIT_FAILED){result=GetLastError();gui.block();cancelled=true;break;}
        }
        control_worker.join();
        if(artifact_stopper.joinable())artifact_stopper.join();
        channels.stop();gui.stop();
        if(receiver.snapshot().state==::control::State::failed) {
            std::cerr<<receiver.snapshot().diagnostic<<'\n';result=ERROR_GEN_FAILURE;
        }
    }
    monitor.finish();
    std::cout<<"Output monitor notifications="<<monitor.changes<<'\n';
    return monitor.error!=ERROR_SUCCESS?monitor.error.load():result;
}
}
