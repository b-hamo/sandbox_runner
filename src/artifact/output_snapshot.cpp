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
struct EntryFailure {
    std::wstring path;          // relative to Output; empty for Output itself
    const char* reason=nullptr; // null: classify from the Win32 error
};
DWORD enumerate(const std::wstring& root, Handles& handles, std::vector<std::wstring>& names,
                EntryFailure* failed=nullptr) {
    auto at=[&](const std::wstring& path,const char* reason=nullptr) {
        if(failed){failed->path=path;failed->reason=reason;}
    };
    namespace fs=std::filesystem;
    const fs::path base(root);
    if(!base.is_absolute() || base!=base.lexically_normal() || base.root_name().wstring().size()!=2)return ERROR_BAD_PATHNAME;
    auto ancestor=base.root_path();
    auto error=pin(ancestor,handles);if(error){at({},"OUTPUT_ROOT_UNAVAILABLE");return error;}
    for(const auto& part:base.relative_path()) {
        ancestor/=part;error=pin(ancestor,handles);
        if(error){at({},"OUTPUT_ROOT_UNAVAILABLE");return error;}
    }
    std::vector<fs::path> pending{base};
    std::size_t directories=0;
    while(!pending.empty()) {
        auto directory=pending.back();pending.pop_back();
        const auto directory_relative=directory==base ? std::wstring() : directory.lexically_relative(base).wstring();
        if(++directories>4096)return ERROR_TOO_MANY_NAMES;
        WIN32_FIND_DATAW entry{};
        HANDLE search=FindFirstFileW((directory/L"*").c_str(),&entry);
        if(search==INVALID_HANDLE_VALUE) {
            const DWORD find_error=GetLastError();
            if(find_error==ERROR_FILE_NOT_FOUND)continue;
            at(directory_relative);
            return find_error;
        }
        do {
            const std::wstring name=entry.cFileName;
            if(name==L"." || name==L"..")continue;
            const auto path=directory/name;
            const auto relative=path.lexically_relative(base).wstring();
            if(!valid_relative_file(relative)) {at(relative,"INVALID_NAME");error=ERROR_ACCESS_DENIED;break;}
            if(entry.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) {at(relative,"REPARSE_POINT");error=ERROR_ACCESS_DENIED;break;}
            if(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) {
                error=pin(path,handles);if(error){at(relative);break;}
                pending.push_back(path);
            } else {
                if(names.size()>=4096){error=ERROR_TOO_MANY_NAMES;break;}
                names.push_back(relative);
            }
        } while(FindNextFileW(search,&entry));
        if(!error && GetLastError()!=ERROR_NO_MORE_FILES){error=GetLastError();at(directory_relative);}
        FindClose(search);
        if(error)return error;
    }
    std::sort(names.begin(),names.end());
    return ERROR_SUCCESS;
}
const char* classify(DWORD error) {
    switch(error) {
    case ERROR_NOT_SUPPORTED: case ERROR_MORE_DATA: return "STREAMS_NOT_SUPPORTED";
    case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: return "FILE_IN_USE";
    case ERROR_ACCESS_DENIED: return "ACCESS_DENIED";
    case ERROR_FILE_INVALID: return "FILE_CHANGED";
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return "FILE_REMOVED";
    case ERROR_TOO_MANY_NAMES: return "TOO_MANY_FILES";
    case ERROR_DIRECTORY: return "NOT_REGULAR_FILE";
    case ERROR_BAD_PATHNAME: case ERROR_FILENAME_EXCED_RANGE: return "INVALID_NAME";
    default: return "OS_ERROR";
    }
}
// Diagnostic re-inspection after a refusal. It never changes the verdict; it
// only names what the locked checks already rejected.
HANDLE open_metadata(const std::wstring& root,const std::wstring& relative) {
    std::wstring path=root;
    std::replace(path.begin(),path.end(),L'/',L'\\');
    while(!path.empty() && path.back()==L'\\')path.pop_back();
    path=L"\\\\?\\"+path+L"\\"+relative;
    return CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
}
std::wstring first_named_stream(HANDLE file) {
    std::vector<unsigned char> buffer(64*1024);
    if(!GetFileInformationByHandleEx(file,FileStreamInfo,buffer.data(),static_cast<DWORD>(buffer.size())))return {};
    const std::wstring unnamed=L"::$DATA",suffix=L":$DATA";
    for(std::size_t offset=0;offset+sizeof(FILE_STREAM_INFO)<=buffer.size();) {
        const auto* info=reinterpret_cast<const FILE_STREAM_INFO*>(buffer.data()+offset);
        std::wstring name(info->StreamName,info->StreamNameLength/sizeof(wchar_t));
        if(name!=unnamed) {
            if(!name.empty() && name.front()==L':')name.erase(0,1);
            if(name.size()>suffix.size() && name.compare(name.size()-suffix.size(),suffix.size(),suffix)==0)
                name.resize(name.size()-suffix.size());
            return name;
        }
        if(!info->NextEntryOffset)break;
        offset+=info->NextEntryOffset;
    }
    return {};
}
const char* diagnose(const std::wstring& root,const std::wstring& relative,DWORD error,std::wstring& stream) {
    const char* reason=classify(error);
    const bool streams=error==ERROR_NOT_SUPPORTED || error==ERROR_MORE_DATA;
    if(relative.empty() || (error!=ERROR_ACCESS_DENIED && !streams))return reason;
    HANDLE file=open_metadata(root,relative);
    if(file==INVALID_HANDLE_VALUE)return reason;
    if(streams) stream=first_named_stream(file);
    else {
        BY_HANDLE_FILE_INFORMATION info{};
        if(GetFileInformationByHandle(file,&info)) {
            if(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)reason="REPARSE_POINT";
            else if(!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) && info.nNumberOfLinks!=1)reason="HARDLINK";
        }
    }
    CloseHandle(file);
    return reason;
}
const char* hint(const std::string& reason) {
    if(reason=="STREAMS_NOT_SUPPORTED")return "remove alternate data streams such as Zone.Identifier (e.g. Unblock-File), then retry";
    if(reason=="FILE_IN_USE")return "close the program using this file, then retry";
    if(reason=="HARDLINK")return "replace the hardlink with an independent copy";
    if(reason=="REPARSE_POINT")return "replace the link with a regular file or folder";
    if(reason=="INVALID_NAME")return "rename it to a plain Windows file name";
    if(reason=="FILE_TOO_LARGE")return "shrink or remove this file";
    if(reason=="TOTAL_TOO_LARGE")return "reduce the total size of Output";
    if(reason=="TOO_MANY_FILES")return "reduce the number of files and folders in Output";
    if(reason=="FILE_CHANGED" || reason=="OUTPUT_CHANGED")return "wait until writing finishes, then retry";
    return nullptr;
}
// Keeps the head and tail of an overlong name without splitting a surrogate pair.
std::wstring clip(const std::wstring& text,std::size_t limit) {
    if(text.size()<=limit)return text;
    std::size_t head=(limit-3)/2,tail=text.size()-(limit-3-head);
    if(head && IS_HIGH_SURROGATE(text[head-1]))--head;
    if(tail<text.size() && IS_LOW_SURROGATE(text[tail]))++tail;
    return text.substr(0,head)+L"..."+text.substr(tail);
}
std::string utf8(const std::wstring& text) {
    if(text.empty())return {};
    // Flags 0 replaces unpaired surrogates, so the result is always valid UTF-8.
    const int size=WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    if(size<=0)return {};
    std::string result(static_cast<std::size_t>(size),'\0');
    WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),&result[0],size,nullptr,nullptr);
    return result;
}
}
std::string capture_problem(const CaptureFailure& failure) {
    constexpr std::size_t max_bytes=900; // stays below the Host's 1 KiB error message
    const std::string reason=failure.reason ? failure.reason : classify(failure.error);
    std::string text=failure.error==ERROR_FILE_TOO_LARGE ? "OUTPUT_TOO_LARGE" : "OUTPUT_UNAVAILABLE";
    text+=": "+reason;
    if(!failure.relative_path.empty())text+=" path=\""+utf8(clip(failure.relative_path,200))+"\"";
    if(!failure.stream.empty())text+=" stream=\""+utf8(clip(failure.stream,40))+"\"";
    text+=" win32="+std::to_string(failure.error);
    if(const char* advice=hint(reason))text+=" hint=\""+std::string(advice)+"\"";
    if(text.size()>max_bytes) {
        std::size_t end=max_bytes;
        while(end && (static_cast<unsigned char>(text[end])&0xC0)==0x80)--end;
        text.resize(end);
    }
    return text;
}
OutputSnapshot::OutputSnapshot(std::wstring root):root_(std::move(root)){}
OutputSnapshot::~OutputSnapshot(){clear();}
void OutputSnapshot::clear() noexcept {
    files_.clear();
    for(auto i=directories_.rbegin();i!=directories_.rend();++i)CloseHandle(*i);
    directories_.clear();
}
DWORD OutputSnapshot::fail(DWORD error,const std::wstring& relative_path,const char* reason) {
    clear();
    failure_.error=error;
    failure_.relative_path=relative_path;
    failure_.stream.clear();
    failure_.reason=reason ? reason : diagnose(root_,relative_path,error,failure_.stream);
    return error;
}
DWORD OutputSnapshot::capture(std::uint64_t max_file_bytes,std::uint64_t max_total_bytes) {
    clear();
    failure_={};
    Handles handles;std::vector<std::wstring> names;EntryFailure failed;
    auto error=enumerate(root_,handles,names,&failed);if(error)return fail(error,failed.path,failed.reason);
    std::uint64_t total=0;
    for(const auto& name:names) {
        FileObservation observation{};
        error=observe_file(root_,name,observation);if(error)return fail(error,name);
        SnapshotFile entry;entry.relative_path=name;
        error=open_verified_file(root_,name,observation,entry.source);if(error)return fail(error,name);
        if(entry.source.size()>max_file_bytes)return fail(ERROR_FILE_TOO_LARGE,name,"FILE_TOO_LARGE");
        if(entry.source.size()>max_total_bytes-total)return fail(ERROR_FILE_TOO_LARGE,name,"TOTAL_TOO_LARGE");
        total+=entry.source.size();
        error=hash_locked(entry.source,entry.sha256);if(error)return fail(error,name);
        entry.event_id=scrp::uuid_v4();
        files_.push_back(std::move(entry));
    }
    directories_.swap(handles.values);
    error=validate();if(error)return fail(error,{},"OUTPUT_CHANGED");
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
