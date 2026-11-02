#include "artifact/output_snapshot.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs=std::filesystem;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
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
        check(CreateHardLinkW((root/L"alias.bin").c_str(),first.c_str(),nullptr)!=FALSE,"hardlink test setup failed");
        {
            artifact::OutputSnapshot snapshot(root.wstring());
            check(snapshot.capture(1024,2048)!=ERROR_SUCCESS,"hardlink alias accepted");
            check(snapshot.files().empty(),"rejected manifest retained locks/files");
        }
        fs::remove(root/L"alias.bin");fs::remove(first);fs::remove(root/L"new.txt");
        fs::remove(root/L"sub");fs::remove(root);
        std::cout<<"Output snapshot verification passed\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
