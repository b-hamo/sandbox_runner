#include "artifact/output_snapshot.h"
#include "artifact_export_session.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs=std::filesystem;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
bool starts_with(const std::string& text,const std::string& prefix){return text.rfind(prefix,0)==0;}
// Writes a whole stream, e.g. "file.txt:Zone.Identifier" as a browser does for MOTW.
void write_stream(const std::wstring& path,const std::string& text) {
    const auto h=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);
    check(h!=INVALID_HANDLE_VALUE,"stream test setup failed");
    DWORD written=0;
    const bool ok=WriteFile(h,text.data(),static_cast<DWORD>(text.size()),&written,nullptr)!=FALSE;
    CloseHandle(h);
    check(ok && written==text.size(),"stream test write failed");
}
// A file with a Zone.Identifier stream still refuses the whole capture, but the
// failure names that file and stream. Removing the stream makes capture succeed.
void motw_test(const fs::path& root) {
    const auto motw=root/L"minobox_export_test.txt";
    write_stream(motw.wstring(),"exported by browser");
    write_stream(motw.wstring()+L":Zone.Identifier","[ZoneTransfer]\r\nZoneId=3\r\n");
    {
        artifact::OutputSnapshot snapshot(root.wstring());
        check(snapshot.capture(1024,2048)==ERROR_NOT_SUPPORTED,"MOTW stream accepted");
        check(snapshot.files().empty(),"MOTW refusal exposed a partial manifest");
        const auto& failure=snapshot.failure();
        check(failure.reason && std::string(failure.reason)=="STREAMS_NOT_SUPPORTED","MOTW reason missing");
        check(failure.relative_path==L"minobox_export_test.txt","MOTW path missing");
        check(failure.stream==L"Zone.Identifier","MOTW stream name missing");
        const auto problem=artifact::capture_problem(failure);
        check(starts_with(problem,"OUTPUT_UNAVAILABLE: STREAMS_NOT_SUPPORTED path=\"minobox_export_test.txt\" "
            "stream=\"Zone.Identifier\" win32=50 hint=\""),"MOTW problem text incorrect");
        const auto writer=CreateFileW((root/L"sub"/L"first.bin").c_str(),GENERIC_WRITE,
            FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        check(writer!=INVALID_HANDLE_VALUE,"refused capture retained other file locks");
        CloseHandle(writer);
    }
    check(DeleteFileW((motw.wstring()+L":Zone.Identifier").c_str())!=FALSE,"MOTW stream removal failed");
    {
        artifact::OutputSnapshot snapshot(root.wstring());
        check(snapshot.capture(1024,2048)==ERROR_SUCCESS,"unblocked file still refused");
        check(snapshot.files().size()==3 && snapshot.failure().reason==nullptr,"unblocked manifest incorrect");
    }
    fs::remove(motw);
}
void failure_detail_test(const fs::path& root) {
    const auto first=root/L"sub"/L"first.bin";
    {
        // A writer that denies sharing, like an open ETL trace.
        const auto busy=CreateFileW(first.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
        check(busy!=INVALID_HANDLE_VALUE,"busy file setup failed");
        artifact::OutputSnapshot snapshot(root.wstring());
        const auto error=snapshot.capture(1024,2048);
        CloseHandle(busy);
        check(error==ERROR_SHARING_VIOLATION,"busy file accepted");
        check(std::string(snapshot.failure().reason)=="FILE_IN_USE" &&
              snapshot.failure().relative_path==L"sub\\first.bin","busy file not named");
    }
    {
        artifact::OutputSnapshot snapshot(root.wstring());
        check(snapshot.capture(4,2048)==ERROR_FILE_TOO_LARGE,"per-file limit not enforced");
        check(starts_with(artifact::capture_problem(snapshot.failure()),
            "OUTPUT_TOO_LARGE: FILE_TOO_LARGE path=\"sub\\first.bin\" "),"per-file limit not named");
        check(snapshot.capture(1024,7)==ERROR_FILE_TOO_LARGE,"total limit not enforced");
        check(std::string(snapshot.failure().reason)=="TOTAL_TOO_LARGE" &&
              snapshot.failure().relative_path==L"sub\\first.bin","total limit not named");
    }
    {
        // Overlong names are clipped without splitting a surrogate pair.
        artifact::CaptureFailure failure;
        failure.error=ERROR_NOT_SUPPORTED;failure.reason="STREAMS_NOT_SUPPORTED";
        for(int i=0;i<2000;++i)failure.relative_path+=L"\U0001F600";
        failure.stream=std::wstring(500,L's');
        const auto problem=artifact::capture_problem(failure);
        check(problem.size()<=900 && problem.find("...")!=std::string::npos,"overlong problem not bounded");
        check(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,problem.data(),
            static_cast<int>(problem.size()),nullptr,0)>0,"bounded problem is not valid UTF-8");
    }
}

void protocol_test(const fs::path& root) {
    const scrp::SessionContext identity{"SES-OUTPUT-TEST","RT-1",1};
    const auto connection=scrp::uuid_v4();
    runner::ArtifactExportSession session(root.wstring(),identity,connection,
        [](const scrp::ArtifactUploadRequest&,artifact::VerifiedFile&,const std::atomic<bool>&)
            ->scrp::ArtifactTransferResult {throw std::runtime_error("unexpected upload");},
        [](const std::wstring&,artifact::CandidateStatus&){return false;});
    auto request=[&](const char* operation,const std::string& id) {
        Json::Value payload(Json::objectValue);
        payload["operation"]=operation;payload["snapshot_id"]=id;
        payload["max_file_bytes"]=1024;payload["max_total_bytes"]=2048;
        return scrp::make_envelope(identity,connection,1,"OUTPUT_SNAPSHOT_REQUEST",payload);
    };
    auto exchange=[&](const scrp::Envelope& req) {
        auto replies=session.snapshot(req);
        if(!replies.messages.empty())return replies.messages.front();
        check(replies.deferred,"snapshot did not reserve deferred response");
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        do {
            auto completed=session.poll();
            if(!completed.empty()) {
                check(completed.size()==1 && completed[0].correlation_id==req.message_id,
                      "snapshot response correlation mismatch");
                return completed[0].reply;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while(std::chrono::steady_clock::now()<deadline);
        throw std::runtime_error("snapshot worker did not finish");
    };
    const auto id=scrp::uuid_v4();
    for(int variant=0;variant<3;++variant) {
        auto bad=request("CAPTURE",id);
        if(variant==0)bad.connection_id=scrp::uuid_v4();
        if(variant==1)++bad.session.generation;
        if(variant==2)bad.payload["unexpected"]=true;
        bool rejected=false;
        try {session.snapshot(bad);}catch(const std::invalid_argument&){rejected=true;}
        check(rejected,"wrong connection/generation/payload accepted");
    }
    check(exchange(request("RELEASE",id)).payload["complete"].asBool(),"empty release failed");
    const auto captured=exchange(request("CAPTURE",id));
    check(captured.payload["complete"].asBool() && captured.payload["files"].size()==2,
          "worker manifest incomplete");
    check(!exchange(request("CAPTURE",scrp::uuid_v4())).payload["complete"].asBool(),
          "active snapshot replaced");
    check(!exchange(request("RELEASE",scrp::uuid_v4())).payload["complete"].asBool(),
          "wrong snapshot released");
    check(exchange(request("VERIFY",id)).payload["complete"].asBool(),"matching verify failed");
    std::ofstream(root/L"late.txt")<<"late";
    check(!exchange(request("VERIFY",id)).payload["complete"].asBool(),"changed manifest accepted");
    check(exchange(request("RELEASE",id)).payload["complete"].asBool(),"release failed");
    check(exchange(request("RELEASE",id)).payload["complete"].asBool(),"repeated release failed");
    auto too_large=request("CAPTURE",scrp::uuid_v4());too_large.payload["max_file_bytes"]=1;
    auto rejected=exchange(too_large);
    check(!rejected.payload["complete"].asBool() && rejected.payload["files"].empty(),
          "failed capture returned partial manifest");
    check(starts_with(rejected.payload["problem"].asString(),"OUTPUT_TOO_LARGE: FILE_TOO_LARGE path="),
          "size problem lost its code or path");
    const auto motw=(root/L"browser.txt").wstring();
    write_stream(motw,"saved");
    write_stream(motw+L":Zone.Identifier","[ZoneTransfer]\r\nZoneId=3\r\n");
    rejected=exchange(request("CAPTURE",scrp::uuid_v4()));
    check(!rejected.payload["complete"].asBool() && rejected.payload["files"].empty() &&
          starts_with(rejected.payload["problem"].asString(),
              "OUTPUT_UNAVAILABLE: STREAMS_NOT_SUPPORTED path=\"browser.txt\" stream=\"Zone.Identifier\""),
          "MOTW refusal did not name the file and stream");
    fs::remove(motw);
    fs::remove(root/L"late.txt");
    check(exchange(request("CAPTURE",id)).payload["complete"].asBool(),"capture after release failed");
    session.stop();
    check(!exchange(request("CAPTURE",id)).payload["complete"].asBool(),"closed session accepted capture");
    const auto writer=CreateFileW((root/L"sub"/L"first.bin").c_str(),GENERIC_WRITE,
        FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    check(writer!=INVALID_HANDLE_VALUE,"termination retained snapshot locks");
    CloseHandle(writer);
}
int main(int argc,char** argv) {
    try {
        check(argc==2,"Pass a new, empty test root inside Windows Sandbox");
        const auto root=fs::absolute(argv[1]);
        check(!fs::exists(root),"Test root must not already exist");
        fs::create_directories(root/L"sub");
        const auto first=root/L"sub"/L"first.bin";
        std::ofstream(first,std::ios::binary)<<"first";
        {
            artifact::OutputSnapshot snapshot(root.wstring());
            check(snapshot.capture(1024,2048)==ERROR_SUCCESS,"nested file capture failed");
            check(snapshot.files().size()==1 && snapshot.files()[0].source.size()==5,"manifest incorrect");
            check(snapshot.files()[0].sha256=="a7937b64b8caa58f03721bb6bacf5c78cb235febe0e70b1b84cd99541461a08e", "manifest hash incorrect");
            const auto writer=CreateFileW(first.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
            if(writer!=INVALID_HANDLE_VALUE)CloseHandle(writer);
            check(writer==INVALID_HANDLE_VALUE,"snapshot allowed a concurrent writer");
            check(snapshot.validate()==ERROR_SUCCESS,"unchanged manifest rejected");
            std::ofstream(root/L"new.txt")<<"new";
            check(snapshot.validate()!=ERROR_SUCCESS,"new file during transfer was silently omitted");
        }
        {
            artifact::OutputSnapshot snapshot(root.wstring());
            check(snapshot.capture(4,2048)==ERROR_FILE_TOO_LARGE,"per-file limit not enforced");
            check(snapshot.files().empty(),"partial failed manifest exposed");
            check(snapshot.capture(1024,7)==ERROR_FILE_TOO_LARGE,"total limit not enforced");
            check(snapshot.capture(1024,2048)==ERROR_SUCCESS,"recapture failed");
            check(snapshot.files().size()==2,"recapture missed added file");
        }
        protocol_test(root);
        motw_test(root);
        failure_detail_test(root);
        check(CreateHardLinkW((root/L"alias.bin").c_str(),first.c_str(),nullptr)!=FALSE,"hardlink test setup failed");
        {
            artifact::OutputSnapshot snapshot(root.wstring());
            check(snapshot.capture(1024,2048)!=ERROR_SUCCESS,"hardlink alias accepted");
            check(snapshot.files().empty(),"rejected manifest retained locks/files");
            check(std::string(snapshot.failure().reason)=="HARDLINK" &&
                  snapshot.failure().relative_path==L"alias.bin","hardlink refusal not named");
        }
        fs::remove(root/L"alias.bin");fs::remove(first);fs::remove(root/L"new.txt");
        fs::remove(root/L"sub");fs::remove(root);
        std::cout<<"Output snapshot and worker protocol verification passed\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
