#include <filesystem>
#include <string>
#include <ios>
#include <cstdio>
#include "PrintJournal.hpp"
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Slic3r::AgentControl {
namespace {
using Json=nlohmann::json;
constexpr std::size_t max_record_bytes=64*1024;
std::atomic<unsigned long long> next_temporary{0};
void require(bool condition,const char* message) { if(!condition)throw std::runtime_error(message); }
#ifdef _WIN32
class PrivateSecurity {
public:
    PrivateSecurity() {
        HANDLE token=nullptr;require(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token),"Cannot inspect current user for print journal ACL");
        DWORD size=0;GetTokenInformation(token,TokenUser,nullptr,0,&size);std::vector<unsigned char> buffer(size);
        const bool found=GetTokenInformation(token,TokenUser,buffer.data(),size,&size)!=0;CloseHandle(token);require(found,"Cannot inspect print journal user identity");
        LPWSTR sid=nullptr;require(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid,&sid),"Cannot encode print journal user identity");
        const std::wstring sddl=L"D:P(A;OICI;FA;;;"+std::wstring(sid)+L")";LocalFree(sid);
        require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr),"Cannot create private print journal ACL");
        attributes={sizeof(SECURITY_ATTRIBUTES),descriptor,FALSE};
    }
    ~PrivateSecurity(){if(descriptor)LocalFree(descriptor);}
    PSECURITY_DESCRIPTOR descriptor=nullptr;
    SECURITY_ATTRIBUTES attributes{};
};
bool write_exclusive(const std::filesystem::path& path,const std::string& bytes) {
    PrivateSecurity security;
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,&security.attributes,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);
    if(file==INVALID_HANDLE_VALUE) {
        if(GetLastError()==ERROR_FILE_EXISTS||GetLastError()==ERROR_ALREADY_EXISTS)return false;
        throw std::runtime_error("Cannot exclusively create print journal record");
    }
    DWORD count=0;
    bool ok=WriteFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr)&&count==bytes.size()&&FlushFileBuffers(file);
    CloseHandle(file);
    require(ok,"Cannot durably write print journal record");return true;
}
#else
bool write_exclusive(const std::filesystem::path& path,const std::string& bytes) {
    int file=::open(path.c_str(),O_CREAT|O_EXCL|O_WRONLY|O_NOFOLLOW|O_CLOEXEC,0600);
    if(file<0) {
        if(errno==EEXIST)return false;
        throw std::runtime_error("Cannot exclusively create print journal record");
    }
    std::size_t offset=0;
    while(offset<bytes.size()) {
        const ssize_t written=::write(file,bytes.data()+offset,bytes.size()-offset);
        if(written<0&&errno==EINTR)continue;
        if(written<=0){::close(file);throw std::runtime_error("Cannot write print journal record");}
        offset+=static_cast<std::size_t>(written);
    }
    const int synced=::fsync(file);const int closed=::close(file);
    require(synced==0&&closed==0,"Cannot durably write print journal record");return true;
}
#endif
Json read_record(const std::filesystem::path& path) {
    std::error_code error;
    const auto status=std::filesystem::symlink_status(path,error);
    if(error==std::errc::no_such_file_or_directory||(!error&&status.type()==std::filesystem::file_type::not_found))return nullptr;
    require(!error,"Cannot inspect print journal record");
    require(std::filesystem::is_regular_file(status),"Print journal record must be a regular file");
    require(std::filesystem::file_size(path)<=max_record_bytes,"Print journal record is too large");
    std::ifstream stream(path,std::ios::binary);require(stream.good(),"Cannot read print journal record");
    try { return Json::parse(stream); } catch(...) { throw std::runtime_error("Print journal record is incomplete; its claim remains consumed"); }
}
}
PrintJournal::PrintJournal(std::filesystem::path directory):m_directory(std::move(directory)) {
    require(m_directory.is_absolute(),"Print journal directory must be absolute");
    std::error_code error;
    const bool created=std::filesystem::create_directories(m_directory,error);
    require(!error,"Cannot create print journal directory");
    const auto status=std::filesystem::symlink_status(m_directory,error);
    require(!error&&std::filesystem::is_directory(status),"Print journal directory cannot be a symlink");
    // macOS temporary roots may contain the system /var -> /private/var link.
    // Reject a redirected journal leaf above, then pin its physical ancestors.
    m_directory=std::filesystem::canonical(m_directory,error);
    require(!error,"Cannot resolve the physical print journal directory");
#ifdef _WIN32
    PrivateSecurity security;
    const DWORD attributes=GetFileAttributesW(m_directory.c_str());
    require(attributes!=INVALID_FILE_ATTRIBUTES&&!(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"Print journal directory cannot redirect through a reparse point");
    require(SetFileSecurityW(m_directory.c_str(),DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,security.descriptor),"Cannot make print journal directory private");
    (void)created;
#else
    struct stat info{};
    require(::lstat(m_directory.c_str(),&info)==0&&info.st_uid==::geteuid(),"Print journal directory must belong to the current user");
    require(::chmod(m_directory.c_str(),0700)==0,"Cannot make print journal directory private");
    if(created) {
        // Persist newly created directory entries as well as the records they
        // will contain; otherwise a power loss could lose the whole journal.
        for(auto parent=m_directory;!parent.empty();parent=parent.parent_path()) {
            int directory=::open(parent.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            require(directory>=0,"Cannot open a print journal ancestor for durability");
            const int synced=::fsync(directory);::close(directory);
            require(synced==0,"Cannot synchronize a print journal directory entry");
            if(parent==parent.parent_path())break;
        }
    }
#endif
}
std::filesystem::path PrintJournal::path(const std::string& id,const char* suffix) const {
    require(!id.empty()&&id.size()<=128,"Invalid print operation identity");
    for(unsigned char c:id)require((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-',"Invalid print operation identity");
    return m_directory/(id+suffix);
}
void PrintJournal::sync_directory() const {
#ifndef _WIN32
    int directory=::open(m_directory.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    require(directory>=0,"Cannot open print journal directory for durability");
    const int synced=::fsync(directory);::close(directory);
    require(synced==0,"Cannot durably synchronize print journal directory");
#endif
}
bool PrintJournal::claim(const std::string& id,const Json& intent) const {
    const std::string bytes=intent.dump();require(bytes.size()<=max_record_bytes,"Print intent is too large");
    if(!write_exclusive(path(id,".claim.json"),bytes))return false;
    sync_directory();return true;
}
void PrintJournal::record_outcome(const std::string& id,const Json& outcome) const {
    require(std::filesystem::is_regular_file(std::filesystem::symlink_status(path(id,".claim.json"))),"Cannot record an outcome without a consumed claim");
    const std::string bytes=outcome.dump();require(bytes.size()<=max_record_bytes,"Print outcome is too large");
#ifdef _WIN32
    const auto process=GetCurrentProcessId();
#else
    const auto process=::getpid();
#endif
    const auto temporary=path(id,(".tmp-"+std::to_string(process)+"-"+std::to_string(next_temporary.fetch_add(1,std::memory_order_relaxed))).c_str());
    require(write_exclusive(temporary,bytes),"Cannot allocate print outcome temporary file");
    const auto destination=path(id,".outcome.json");
#ifdef _WIN32
    const bool published=MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
    require(published,"Cannot publish print outcome");
#else
    require(::rename(temporary.c_str(),destination.c_str())==0,"Cannot publish print outcome");
#endif
    sync_directory();
}
nlohmann::json PrintJournal::read(const std::string& id) const {
    Json claim=read_record(path(id,".claim.json"));
    if(claim.is_null())return nullptr;
    Json outcome=read_record(path(id,".outcome.json"));
    return {{"intent",claim},{"outcome",outcome.is_null()?Json{{"state","outcome_unknown"},{"reason","Claim consumed; no durable outcome was recorded"}}:outcome}};
}
}
