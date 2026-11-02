#include "output_snapshot.h"
#include "../protocol/scrp/envelope.h"
#include <algorithm>
#include <filesystem>
#include <bcrypt.h>
#include <array>

namespace artifact {
namespace {
DWORD hash_locked(const VerifiedFile& file, std::string& result) {
    struct Hash {
        BCRYPT_ALG_HANDLE algorithm=nullptr;
        BCRYPT_HASH_HANDLE value=nullptr;
        ~Hash(){if(value)BCryptDestroyHash(value);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);}
    } hash;
    if(BCryptOpenAlgorithmProvider(&hash.algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0 ||
       BCryptCreateHash(hash.algorithm,&hash.value,nullptr,0,nullptr,0,0)<0)return ERROR_GEN_FAILURE;
    std::array<unsigned char,65536> buffer{};
    std::uint64_t total=0;
    for(;;) {
        DWORD count=0;
        if(!ReadFile(file.handle(),buffer.data(),static_cast<DWORD>(buffer.size()),&count,nullptr))return GetLastError();
        if(!count)break;
        total+=count;
        if(total>file.size())return ERROR_FILE_INVALID;
        if(BCryptHashData(hash.value,buffer.data(),count,0)<0)return ERROR_GEN_FAILURE;
    }
    unsigned char digest[32];
    if(total!=file.size() || file.validate_unchanged()!=ERROR_SUCCESS)return ERROR_FILE_INVALID;
    if(BCryptFinishHash(hash.value,digest,sizeof(digest),0)<0)return ERROR_GEN_FAILURE;
    LARGE_INTEGER zero{};
    if(!SetFilePointerEx(file.handle(),zero,nullptr,FILE_BEGIN))return GetLastError();
    constexpr char hex[]="0123456789abcdef";
    for(auto byte:digest){result+=hex[byte>>4];result+=hex[byte&15];}
    return ERROR_SUCCESS;
}
struct Handles {
    std::vector<HANDLE> values;
    ~Handles(){for(auto i=values.rbegin();i!=values.rend();++i)CloseHandle(*i);}
};
DWORD pin(const std::filesystem::path& path, Handles& handles) {
    HANDLE h=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES|FILE_LIST_DIRECTORY,
        FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(h==INVALID_HANDLE_VALUE)return GetLastError();
    handles.values.push_back(h);
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(h,&info))return GetLastError();
    if(!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))return ERROR_ACCESS_DENIED;
    return ERROR_SUCCESS;
}
DWORD enumerate(const std::wstring& root, Handles& handles, std::vector<std::wstring>& names) {
    namespace fs=std::filesystem;
    const fs::path base(root);
    if(!base.is_absolute() || base!=base.lexically_normal() || base.root_name().wstring().size()!=2)return ERROR_BAD_PATHNAME;
    auto ancestor=base.root_path();
    auto error=pin(ancestor,handles);if(error)return error;
    for(const auto& part:base.relative_path()) { ancestor/=part;error=pin(ancestor,handles);if(error)return error; }
    std::vector<fs::path> pending{base};
    std::size_t directories=0;
    while(!pending.empty()) {
        auto directory=pending.back();pending.pop_back();
        if(++directories>4096)return ERROR_TOO_MANY_NAMES;
        WIN32_FIND_DATAW entry{};
        HANDLE search=FindFirstFileW((directory/L"*").c_str(),&entry);
        if(search==INVALID_HANDLE_VALUE) {
            if(GetLastError()==ERROR_FILE_NOT_FOUND)continue;
            return GetLastError();
        }
        do {
            const std::wstring name=entry.cFileName;
            if(name==L"." || name==L"..")continue;
            const auto path=directory/name;
            const auto relative=path.lexically_relative(base).wstring();
            if(!valid_relative_file(relative) || (entry.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) {error=ERROR_ACCESS_DENIED;break;}
            if(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) {
                error=pin(path,handles);if(error)break;
                pending.push_back(path);
            } else {
                if(names.size()>=4096){error=ERROR_TOO_MANY_NAMES;break;}
                names.push_back(relative);
            }
        } while(FindNextFileW(search,&entry));
        if(!error && GetLastError()!=ERROR_NO_MORE_FILES)error=GetLastError();
        FindClose(search);
        if(error)return error;
    }
    std::sort(names.begin(),names.end());
    return ERROR_SUCCESS;
}
}
OutputSnapshot::OutputSnapshot(std::wstring root):root_(std::move(root)){}
OutputSnapshot::~OutputSnapshot(){clear();}
void OutputSnapshot::clear() noexcept {
    files_.clear();
    for(auto i=directories_.rbegin();i!=directories_.rend();++i)CloseHandle(*i);
    directories_.clear();
}
DWORD OutputSnapshot::capture(std::uint64_t max_file_bytes,std::uint64_t max_total_bytes) {
    clear();
    Handles handles;std::vector<std::wstring> names;
    auto error=enumerate(root_,handles,names);if(error)return error;
    std::uint64_t total=0;
    for(const auto& name:names) {
        FileObservation observation{};
        error=observe_file(root_,name,observation);if(error){clear();return error;}
        SnapshotFile entry;entry.relative_path=name;
        error=open_verified_file(root_,name,observation,entry.source);if(error){clear();return error;}
        if(entry.source.size()>max_file_bytes || entry.source.size()>max_total_bytes-total){clear();return ERROR_FILE_TOO_LARGE;}
        total+=entry.source.size();
        error=hash_locked(entry.source,entry.sha256);if(error){clear();return error;}
        entry.event_id=scrp::uuid_v4();
        files_.push_back(std::move(entry));
    }
    directories_.swap(handles.values);
    error=validate();if(error)clear();
    return error;
}
DWORD OutputSnapshot::validate() const {
    Handles handles;std::vector<std::wstring> names;
    auto error=enumerate(root_,handles,names);if(error)return error;
    if(names.size()!=files_.size())return ERROR_FILE_INVALID;
    for(std::size_t i=0;i<names.size();++i) {
        if(names[i]!=files_[i].relative_path)return ERROR_FILE_INVALID;
        error=files_[i].source.validate_unchanged();if(error)return error;
    }
    return ERROR_SUCCESS;
}
}
