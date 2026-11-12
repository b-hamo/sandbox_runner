#define NOMINMAX
#include <windows.h>
#include "control/action_scheduler.h"
#include "runtime/sandbox_runtime.h"
#include "transport/control/control_receiver.h"
#include "protocol/scrp/artifact_export.h"
#include <future>
#include <iostream>
#include <stdexcept>
#include <fstream>
using namespace runner::control;
struct Backend : WorkerBackend {
    unsigned executions=0;
    CaptureResult capture(const CaptureLimits&) override { return {true,"",1,1,{1}}; }
    void screen_size(int& w,int& h) override { w=h=100; }
    ExecOutcome input(const ActionRequest&,std::chrono::steady_clock::time_point,const std::atomic<bool>&) override {
        ++executions;
        return {ActionStatus::Success,ErrorCode::None,"one execution",1};
    }
};
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
void output_storage_recovery_test() {
    using namespace runner::runtime;
    const auto base=std::filesystem::temp_directory_path()/("scrp-output-recovery-"+scrp::uuid_v4());
    std::filesystem::create_directory(base);
    std::filesystem::path output;
    {
        SessionWorkspace old;
        output=old.create(base,"SES-storage","RT-storage",1).output;
        std::ofstream(output/"preserved.txt")<<"application state";
    }
    SessionWorkspace recovered;
    const auto& paths=recovered.create(base,"SES-storage","RT-storage",2,1);
    check(paths.output==output,"surviving app Output path changed");
    check(paths.root.filename()==L"generation-2","action workspace reused old generation");
    std::ifstream input(output/"preserved.txt");std::string contents;std::getline(input,contents);
    check(contents=="application state","old Output file lost");
    check(!MoveFileW(output.c_str(),(output.parent_path()/"moved").c_str()),"retained Output ancestor not pinned");
    bool duplicate=false,future=false;
    try { SessionWorkspace other;other.create(base,"SES-storage","RT-storage",2,1); } catch(const std::exception&) {duplicate=true;}
    try { SessionWorkspace other;other.create(base,"SES-storage","RT-storage",3,4); } catch(const std::exception&) {future=true;}
    check(duplicate && future,"unsafe Output/action generation reuse accepted");
}
void runtime_binding_test() {
    using namespace runner::runtime;
    auto backend=std::make_shared<Backend>();
    SandboxRuntime::Config config;
    config.binding={"SES-reconnect-test","RT-test",1};
    config.guest_workspace_base=std::filesystem::temp_directory_path()/
        ("scrp-reconnect-"+scrp::uuid_v4());
    std::filesystem::create_directory(config.guest_workspace_base);
    config.worker.backend=backend;
    SandboxRuntime runtime(config);
    HostGrant grant;
    grant.context={config.binding,"CONN-first"};
    grant.policy_version="POL-test";
    grant.gui_input=true;grant.gui_observe=true;
    grant.startup_verified=grant.network_policy_verified=grant.required_monitoring_verified=true;
    grant.lease=std::chrono::seconds(15);
    check(runtime.startup_probe().ok,"initial probe");runtime.activate(grant);
    ActionRequest request;request.action_id="ACT-runtime-once";request.task_id="TASK-proof";
    request.policy_version=grant.policy_version;request.operation=Operation::KeyboardType;
    request.text_utf8="once";
    std::promise<RuntimeResult> finished;auto future=finished.get_future();
    check(runtime.submit(grant.context,request,[&](const RuntimeResult& r){finished.set_value(r);}).accepted,"initial runtime admission");
    check(future.wait_for(std::chrono::seconds(5))==std::future_status::ready,"runtime completion");
    check(future.get().action.status==ActionStatus::Success,"runtime execution");
    runtime.prepare_reconnect();
    auto next=grant;next.context.connection_id="CONN-second";
    check(runtime.startup_probe().ok,"fresh reconnect probe");
    runtime.activate(next);
    check(runtime.submit(grant.context,request,{}).error==ErrorCode::ProtocolDenied,"old connection admitted");
    check(runtime.submit(next.context,request,{}).duplicate,"runtime ledger lost");
    check(backend->executions==1,"runtime input replayed");
    runtime.stop();
}
struct TestSchema : ::control::Schema {
    Json::Value hello(const scrp::SessionContext&) const override {
        Json::Value payload(Json::objectValue); payload["capabilities"]=Json::Value(Json::arrayValue); return payload;
    }
    std::string hello_ack(const scrp::Envelope&) const override { return "CONN-test"; }
    void validate_request(const scrp::Envelope&) const override {}
};
void artifact_reconnect_contract_test() {
    const scrp::SessionContext session{"SES-contract-test","RT-test",1};
    for(bool opt_in:{false,true}) {
        scrp::ArtifactExportSchema schema(std::make_shared<TestSchema>(),opt_in);
        const auto hello=schema.hello(session);
        bool advertised=false,reset_advertised=false;
        for(const auto& cap:hello["capabilities"])if(cap=="artifact.reconnect.v1")advertised=true;
        for(const auto& cap:hello["capabilities"])if(cap=="artifact.reconnect.reset.v1")reset_advertised=true;
        check(advertised==opt_in,"reconnect advertised without owning shutdown lifecycle");
        check(reset_advertised==opt_in,"file reset advertised without owning shutdown lifecycle");
        for(int scenario=0;scenario<6;++scenario) {
            schema.hello(session);
            scrp::Envelope ack;ack.type="HELLO_ACK";ack.sequence_number=1;ack.session=session;
            ack.correlation_id=scrp::uuid_v4();
            auto& caps=ack.payload["allowed_capabilities"];caps=Json::Value(Json::arrayValue);
            if(scenario!=1)caps.append("artifact.export.v1");
            if(scenario!=2)caps.append("artifact.output.v1");
            if(scenario!=4)caps.append("artifact.reconnect.v1");
            caps.append("artifact.reconnect.reset.v1");
            if(scenario==3)caps.append("artifact.reconnect.v1");
            if(scenario==5)caps.append("artifact.reconnect.reset.v1");
            ack.payload["channel_credentials"]["telemetry"]["token"]=std::string(48,'a');
            ack.payload["channel_credentials"]["telemetry"]["expires_at"]="2099-01-01T00:00:00Z";
            bool accepted=false;
            try{schema.hello_ack(ack);accepted=true;}catch(const std::exception&){}
            check(accepted==(opt_in && scenario==0),"invalid artifact reconnect grant accepted");
        }
    }
}
void failure_classification_test() {
    for(bool transport:{false,true}) {
        ::control::Context context;
        context.session={"SES-test","RT-test",1};context.connection.endpoint=L"wss://localhost/scrp/v1/control";
        context.schema=std::make_shared<TestSchema>();
        context.credential=[] { return telemetry::ChannelCredential{L"Authorization",L"Bearer test",
            std::chrono::system_clock::now()+std::chrono::minutes(1)}; };
        ::control::Handlers handlers{{"OBSERVE",[](const scrp::Envelope&) {}}};
        ::control::Receiver receiver(context,handlers,{},[transport]()->std::unique_ptr<telemetry::WebSocket>{
            if(transport)throw telemetry::SocketError("test transport failure");
            throw std::runtime_error("test protocol failure");
        });
        std::atomic<bool> stop{false};receiver.run(stop);
        check(receiver.snapshot().state==::control::State::failed,"failure state");
        check(receiver.snapshot().transport_failure==transport,"protocol failure classified as reconnectable");
    }
}
int main() {
  try {
    auto backend=std::make_shared<Backend>();
    ActionScheduler::Config config;config.backend=backend;
    ActionScheduler worker(config);
    ActionRequest r;r.action_id="ACT-once";r.task_id="TASK-proof";r.policy_version="POL-test";
    r.operation=Operation::KeyboardType;r.text_utf8="once";r.timeout_ms=1000;
    worker.start();
    std::promise<ActionResult> complete;auto result=complete.get_future();
    check(worker.submit(r,[&](const ActionResult& value){complete.set_value(value);}).accepted,"initial admission");
    check(result.wait_for(std::chrono::seconds(5))==std::future_status::ready,"completion deadline");
    check(result.get().status==ActionStatus::Success,"input result");
    worker.stop();
    worker.restart_after_disconnect();
    auto duplicate=worker.submit(r,{});
    check(!duplicate.accepted && duplicate.duplicate,"dedup ledger lost on reconnect");
    r.text_utf8="conflicting";
    check(worker.submit(r,{}).error==ErrorCode::DuplicateConflict,"conflicting replay accepted");
    check(backend->executions==1,"input replayed");
    worker.stop();
    runtime_binding_test();
    output_storage_recovery_test();
    failure_classification_test();
    artifact_reconnect_contract_test();
    std::cout<<"PASS: reconnect retains execution ledger; duplicate and conflicting input not replayed\n";
    return 0;
  }catch(const std::exception& error){std::cout<<error.what()<<'\n';return 1;}
}
