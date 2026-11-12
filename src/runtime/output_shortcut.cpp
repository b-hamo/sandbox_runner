#include "output_shortcut.h"
#include "../protocol/scrp/envelope.h"
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdexcept>

namespace runner::runtime {
namespace {
void require(HRESULT result, const char* message) {
    if (FAILED(result)) throw std::runtime_error(message);
}
template<class T> struct ComPtr {
    T* value=nullptr;
    ~ComPtr() { if(value)value->Release(); }
};
struct ComScope {
    HRESULT result=CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ComScope() { if(FAILED(result) && result!=RPC_E_CHANGED_MODE)throw std::runtime_error("Output shortcut COM initialization failed"); }
    ~ComScope() { if(SUCCEEDED(result))CoUninitialize(); }
};
constexpr auto description=L"SecureCUA session Output";
}
std::filesystem::path create_output_shortcut(const std::filesystem::path& output) {
    namespace fs=std::filesystem;
    if(!output.is_absolute() || output!=output.lexically_normal() || output.filename()!=L"output")
        throw std::runtime_error("Output shortcut target must be the session Output directory");
    const auto attributes=GetFileAttributesW(output.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES || !(attributes&FILE_ATTRIBUTE_DIRECTORY) || (attributes&FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("Output shortcut target unavailable");
    ComScope apartment;
    PWSTR desktop=nullptr;
    require(SHGetKnownFolderPath(FOLDERID_Desktop,0,nullptr,&desktop),"Cannot find Guest desktop");
    const fs::path link=fs::path(desktop)/L"Output.lnk";
    CoTaskMemFree(desktop);
    ComPtr<IShellLinkW> shell;
    require(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&shell.value)),"Cannot create Output shortcut");
    ComPtr<IPersistFile> persist;
    require(shell.value->QueryInterface(IID_PPV_ARGS(&persist.value)),"Cannot persist Output shortcut");
    const auto existing=GetFileAttributesW(link.c_str());
    if(existing!=INVALID_FILE_ATTRIBUTES) {
        if(existing&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("Output shortcut path is occupied");
        require(persist.value->Load(link.c_str(),STGM_READ),"Existing Output shortcut is not readable");
        wchar_t label[128]{};
        require(shell.value->GetDescription(label,128),"Existing Output shortcut has no owner marker");
        if(std::wstring(label)!=description)throw std::runtime_error("Refusing to overwrite a user Output shortcut");
    } else if(GetLastError()!=ERROR_FILE_NOT_FOUND) {
        throw std::runtime_error("Cannot inspect Guest Output shortcut");
    }
    require(shell.value->SetPath(output.c_str()),"Cannot set Output shortcut target");
    require(shell.value->SetArguments(L""),"Cannot clear Output shortcut arguments");
    require(shell.value->SetWorkingDirectory(output.c_str()),"Cannot set Output shortcut working directory");
    require(shell.value->SetDescription(description),"Cannot identify Output shortcut");
    const fs::path temporary=link.parent_path()/(L"Output-"+fs::path(scrp::uuid_v4()).wstring()+L".lnk");
    try {
        require(persist.value->Save(temporary.c_str(),TRUE),"Cannot save Output shortcut");
        if(!MoveFileExW(temporary.c_str(),link.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot publish Output shortcut");
    } catch(...) { DeleteFileW(temporary.c_str()); throw; }
    return link;
}
}
