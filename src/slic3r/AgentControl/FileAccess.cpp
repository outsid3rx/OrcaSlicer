#include "FileAccess.hpp"
#include "Domain.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <ios>
#include <fstream>
#include <openssl/evp.h>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#include <sddl.h>
#include <openssl/rand.h>
#endif
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace Slic3r::AgentControl {
namespace {
namespace fs=std::filesystem;
std::filesystem::path checked_path(const Json& value, bool output)
{
    ensure(value.is_string(), "invalid_path", "Supply an absolute local path");
    const std::filesystem::path supplied=std::filesystem::u8path(value.get<std::string>());
    ensure(supplied.is_absolute() && value.get_ref<const std::string&>().find('\0') == std::string::npos,
           "invalid_path", "Supply an absolute local path without NUL bytes");
    const char* env = std::getenv(output ? "ORCA_AGENT_OUTPUT_ROOTS" : "ORCA_AGENT_INPUT_ROOTS");
    ensure(env && *env, "files_disabled", "Configure explicit JSON-array ORCA_AGENT_INPUT_ROOTS / ORCA_AGENT_OUTPUT_ROOTS when launching Orca");
    Json roots;
    try { roots = Json::parse(env); } catch (...) { throw DomainError("invalid_roots", "File access roots must be a JSON array of absolute directories"); }
    ensure(roots.is_array() && !roots.empty(), "invalid_roots", "File access roots must be a nonempty JSON array");
    std::error_code error;
    const auto resolved = output ? std::filesystem::canonical(supplied.parent_path(), error) / supplied.filename() : std::filesystem::canonical(supplied, error);
    ensure(!error, "invalid_path", "The input or output parent directory does not exist");
    if(output)ensure(!supplied.filename().empty()&&supplied.filename()!="."&&supplied.filename()!="..","invalid_path","Output must name a file");
    if (output && std::filesystem::is_symlink(supplied, error))
        throw DomainError("invalid_path", "Export destinations cannot be symlinks");
    bool allowed = false;
    for (const auto& root : roots) {
        ensure(root.is_string() && std::filesystem::u8path(root.get<std::string>()).is_absolute(), "invalid_roots", "Every file root must be an absolute directory");
        const auto canonical_root = std::filesystem::canonical(std::filesystem::u8path(root.get<std::string>()), error);
        ensure(!error && std::filesystem::is_directory(canonical_root), "invalid_roots", "Every file root must be an existing directory");
        auto at = resolved.begin();
        bool match = true;
        for (const auto& component : canonical_root) {
            if (at == resolved.end() || *at++ != component) { match = false; break; }
        }
        allowed |= match;
    }
    ensure(allowed, "path_forbidden", "The path is outside the explicitly configured file roots");
    return resolved;
}
#ifdef _WIN32
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    Handle()=default;
    explicit Handle(HANDLE handle):value(handle){}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
    Handle(Handle&& other)noexcept:value(std::exchange(other.value,INVALID_HANDLE_VALUE)){}
    Handle& operator=(Handle&& other)noexcept {if(value!=INVALID_HANDLE_VALUE)::CloseHandle(value);value=std::exchange(other.value,INVALID_HANDLE_VALUE);return *this;}
    ~Handle(){if(value!=INVALID_HANDLE_VALUE)::CloseHandle(value);}
};
void win_checked(bool condition,const char* kind,const char* message){if(!condition)throw DomainError(kind,std::string(message)+" (Windows error "+std::to_string(::GetLastError())+")");}
fs::path actual_path(HANDLE handle) {
    std::vector<wchar_t> buffer(32768);
    DWORD length=::GetFinalPathNameByHandleW(handle,buffer.data(),static_cast<DWORD>(buffer.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    win_checked(length>0&&length<buffer.size(),"file_error","Cannot resolve opened file");
    std::wstring path(buffer.data(),length);
    if(path.rfind(L"\\\\?\\UNC\\",0)==0)path=L"\\\\"+path.substr(8);
    else if(path.rfind(L"\\\\?\\",0)==0)path=path.substr(4);
    return fs::path(path);
}
struct PrivateDirectory {
    fs::path path;Handle descriptor;
    PrivateDirectory() {
        HANDLE token_raw=nullptr;win_checked(::OpenProcessToken(::GetCurrentProcess(),TOKEN_QUERY,&token_raw)!=0,"file_error","Cannot inspect current user");Handle token(token_raw);
        DWORD length=0;::GetTokenInformation(token.value,TokenUser,nullptr,0,&length);
        std::vector<unsigned char> token_data(length);win_checked(::GetTokenInformation(token.value,TokenUser,token_data.data(),length,&length)!=0,"file_error","Cannot read current user identity");
        LPWSTR sid_text=nullptr;win_checked(::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(token_data.data())->User.Sid,&sid_text)!=0,"file_error","Cannot encode user identity");
        const std::wstring acl=L"D:P(A;OICI;FA;;;"+std::wstring(sid_text)+L")";::LocalFree(sid_text);
        PSECURITY_DESCRIPTOR raw_sd=nullptr;win_checked(::ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(),SDDL_REVISION_1,&raw_sd,nullptr)!=0,"file_error","Cannot create private staging ACL");
        std::unique_ptr<void,decltype(&::LocalFree)> sd(raw_sd,&::LocalFree);
        SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES),sd.get(),FALSE};
        std::vector<wchar_t> temp(32768);DWORD temp_length=::GetTempPathW(static_cast<DWORD>(temp.size()),temp.data());win_checked(temp_length>0&&temp_length<temp.size(),"file_error","Cannot locate temporary directory");
        std::array<unsigned char,16> random{};ensure(RAND_bytes(random.data(),static_cast<int>(random.size()))==1,"file_error","Cannot create staging identity");
        static const wchar_t hex[]=L"0123456789abcdef";std::wstring name=L"orca-agent-";for(auto byte:random){name+=hex[byte>>4];name+=hex[byte&15];}
        path=fs::path(std::wstring(temp.data(),temp_length))/name;
        win_checked(::CreateDirectoryW(path.c_str(),&attributes)!=0,"file_error","Cannot create exclusive private staging directory");
        descriptor=Handle(::CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES|FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        win_checked(descriptor.value!=INVALID_HANDLE_VALUE,"file_error","Cannot pin private staging directory");
        FILE_ATTRIBUTE_TAG_INFO info{};win_checked(::GetFileInformationByHandleEx(descriptor.value,FileAttributeTagInfo,&info,sizeof(info))!=0,"file_error","Cannot inspect staging directory");
        ensure((info.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)==0,"file_error","Staging directory cannot be a reparse point");path=actual_path(descriptor.value);
    }
    ~PrivateDirectory(){std::error_code error;fs::remove_all(path,error);}
};
FILE_BASIC_INFO regular_info(HANDLE file,std::uintmax_t limit,std::uintmax_t& size) {
    FILE_ATTRIBUTE_TAG_INFO tags{};win_checked(::GetFileInformationByHandleEx(file,FileAttributeTagInfo,&tags,sizeof(tags))!=0,"file_error","Cannot inspect opened file");
    ensure((tags.FileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))==0&&::GetFileType(file)==FILE_TYPE_DISK,"invalid_path","Expected an opened regular file without reparse points");
    LARGE_INTEGER bytes{};win_checked(::GetFileSizeEx(file,&bytes)!=0,"file_error","Cannot inspect file size");ensure(bytes.QuadPart>=0&&static_cast<std::uintmax_t>(bytes.QuadPart)<=limit,"limit_exceeded","File exceeds permitted size");size=static_cast<std::uintmax_t>(bytes.QuadPart);
    FILE_BASIC_INFO info{};win_checked(::GetFileInformationByHandleEx(file,FileBasicInfo,&info,sizeof(info))!=0,"file_error","Cannot inspect opened file timestamps");return info;
}
void copy_bytes(HANDLE input,HANDLE output,std::uintmax_t limit) {
    std::array<char,65536> bytes{};std::uintmax_t total=0;
    for(;;){DWORD read=0;win_checked(::ReadFile(input,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)!=0,"file_error","Cannot read opened input");if(read==0)break;total+=read;ensure(total<=limit,"limit_exceeded","File exceeds permitted size");DWORD written=0;while(written<read){DWORD count=0;win_checked(::WriteFile(output,bytes.data()+written,read-written,&count,nullptr)!=0&&count>0,"file_error","Cannot write staged artifact");written+=count;}}
}
Handle exclusive_relative_file(HANDLE parent,const std::wstring& name) {
    using Create=NTSTATUS(NTAPI*)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,PIO_STATUS_BLOCK,PLARGE_INTEGER,ULONG,ULONG,ULONG,ULONG,PVOID,ULONG);
    auto create=reinterpret_cast<Create>(::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"),"NtCreateFile"));ensure(create!=nullptr,"unsupported_platform","Windows relative file creation is unavailable");
    ensure(name.size()<32760,"invalid_path","Output filename is too long");UNICODE_STRING text{};text.Buffer=const_cast<wchar_t*>(name.data());text.Length=static_cast<USHORT>(name.size()*sizeof(wchar_t));text.MaximumLength=text.Length;
    OBJECT_ATTRIBUTES attributes{};attributes.Length=sizeof(attributes);attributes.RootDirectory=parent;attributes.ObjectName=&text;attributes.Attributes=OBJ_CASE_INSENSITIVE;
    IO_STATUS_BLOCK status{};HANDLE raw=INVALID_HANDLE_VALUE;
    const NTSTATUS result=create(&raw,GENERIC_READ|GENERIC_WRITE|DELETE|SYNCHRONIZE,&attributes,&status,nullptr,FILE_ATTRIBUTE_NORMAL,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,2,0x20|0x40|0x00200000,nullptr,0);
    ensure(result>=0,"file_error","Cannot create an exclusive output file in the pinned directory");return Handle(raw);
}
#else
struct Descriptor {
    int value=-1;
    Descriptor()=default;
    explicit Descriptor(int descriptor):value(descriptor){}
    Descriptor(const Descriptor&)=delete;
    Descriptor& operator=(const Descriptor&)=delete;
    Descriptor(Descriptor&& other) noexcept:value(std::exchange(other.value,-1)){}
    Descriptor& operator=(Descriptor&& other) noexcept {if(value>=0)::close(value);value=std::exchange(other.value,-1);return *this;}
    ~Descriptor(){if(value>=0)::close(value);}
};
std::string os_error(){return std::error_code(errno,std::generic_category()).message();}
void checked(bool condition,const char* kind,const char* message) {if(!condition)throw DomainError(kind,std::string(message)+": "+os_error());}
fs::path actual_path(int descriptor) {
#ifdef __APPLE__
    std::array<char,4096> path{};
    checked(::fcntl(descriptor,F_GETPATH,path.data())==0,"file_error","Cannot resolve opened file");
    return fs::path(path.data());
#elif defined(__linux__)
    std::array<char,65536> path{};
    auto count=::readlink(("/proc/self/fd/"+std::to_string(descriptor)).c_str(),path.data(),path.size()-1);
    checked(count>0&&static_cast<std::size_t>(count)<path.size()-1,"file_error","Cannot resolve opened file");
    return fs::path(std::string(path.data(),static_cast<std::size_t>(count)));
#else
    (void)descriptor;
    throw DomainError("unsupported_platform","Secure file access requires an actual-open path resolver on this platform");
#endif
}
struct PrivateDirectory {
    fs::path path;
    Descriptor descriptor;
    PrivateDirectory() {
        std::array<char,64> pattern{};
        const std::string text="/tmp/orca-agent-XXXXXX";
        std::copy(text.begin(),text.end(),pattern.begin());
        char* created=::mkdtemp(pattern.data());
        checked(created!=nullptr,"file_error","Cannot create private staging directory");
        path=fs::canonical(created);
        descriptor=Descriptor(::open(path.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
        checked(descriptor.value>=0,"file_error","Cannot open private staging directory");
        struct stat st{};
        checked(::fstat(descriptor.value,&st)==0,"file_error","Cannot inspect private staging directory");
        ensure(S_ISDIR(st.st_mode)&&st.st_uid==::geteuid()&&(st.st_mode&0777)==0700,"file_error","Staging directory must be private to the current user");
    }
    ~PrivateDirectory(){std::error_code error;fs::remove_all(path,error);}
};
void copy_bytes(int input,int output,std::uintmax_t limit) {
    std::array<char,65536> bytes{};std::uintmax_t total=0;
    for(;;) {
        auto read_count=::read(input,bytes.data(),bytes.size());
        if(read_count<0&&errno==EINTR)continue;
        checked(read_count>=0,"file_error","Cannot read input");
        if(read_count==0)break;
        total+=static_cast<std::uintmax_t>(read_count);ensure(total<=limit,"limit_exceeded","File exceeds permitted size");
        ssize_t written=0;
        while(written<read_count){auto count=::write(output,bytes.data()+written,static_cast<std::size_t>(read_count-written));if(count<0&&errno==EINTR)continue;checked(count>0,"file_error","Cannot write staged artifact");written+=count;}
    }
}
bool unchanged(const struct stat& before,const struct stat& after) {
#ifdef __APPLE__
    const bool times=before.st_mtimespec.tv_sec==after.st_mtimespec.tv_sec&&before.st_mtimespec.tv_nsec==after.st_mtimespec.tv_nsec&&before.st_ctimespec.tv_sec==after.st_ctimespec.tv_sec&&before.st_ctimespec.tv_nsec==after.st_ctimespec.tv_nsec;
#else
    const bool times=before.st_mtim.tv_sec==after.st_mtim.tv_sec&&before.st_mtim.tv_nsec==after.st_mtim.tv_nsec&&before.st_ctim.tv_sec==after.st_ctim.tv_sec&&before.st_ctim.tv_nsec==after.st_ctim.tv_nsec;
#endif
    return times&&before.st_dev==after.st_dev&&before.st_ino==after.st_ino&&before.st_size==after.st_size;
}
#endif
}

std::filesystem::path input_path(const Json& value)
{
    auto path = checked_path(value, false);
    std::error_code error;
    ensure(std::filesystem::is_regular_file(path, error) && !error, "invalid_path", "Input must be a regular local file");
    ensure(std::filesystem::file_size(path, error) <= 512ULL * 1024 * 1024 && !error, "file_too_large", "Input files are limited to 512 MiB");
    return path;
}

std::filesystem::path output_path(const Json& value, bool overwrite)
{
    auto path = checked_path(value, true);
    ensure(overwrite || !std::filesystem::exists(path), "file_exists", "Destination exists; explicit overwrite is required");
    return path;
}

std::string file_sha256(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    ensure(bool(input), "file_error", "Cannot open artifact");
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    ensure(digest && EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) == 1, "internal_error", "Cannot initialize artifact digest");
    std::array<char, 65536> buffer{};
    while (input) {
        input.read(buffer.data(), buffer.size());
        ensure(EVP_DigestUpdate(digest.get(), buffer.data(), static_cast<std::size_t>(input.gcount())) == 1, "internal_error", "Cannot hash artifact");
    }
    ensure(input.eof(), "file_error", "Cannot read artifact");
    unsigned char bytes[EVP_MAX_MD_SIZE]; unsigned length = 0;
    ensure(EVP_DigestFinal_ex(digest.get(), bytes, &length) == 1, "internal_error", "Cannot finish artifact digest");
    static const char digits[] = "0123456789abcdef";
    std::string result;
    for (unsigned i = 0; i < length; ++i) { result += digits[bytes[i] >> 4]; result += digits[bytes[i] & 15]; }
    return result;
}

struct InputSnapshot::Impl {
    fs::path original,path;
    std::string hash;
    std::uintmax_t bytes=0;
    PrivateDirectory staging;
};
InputSnapshot::InputSnapshot(const Json& source,std::uintmax_t max_bytes):m_impl(std::make_unique<Impl>()) {
#ifdef _WIN32
    const auto requested=input_path(source);
    Handle input(::CreateFileW(requested.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
    win_checked(input.value!=INVALID_HANDLE_VALUE,"invalid_path","Cannot open input");
    std::uintmax_t size=0;const auto before=regular_info(input.value,max_bytes,size);
    m_impl->original=input_path(actual_path(input.value).u8string());m_impl->path=m_impl->staging.path/("input"+requested.extension().string());
    Handle output(::CreateFileW(m_impl->path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));win_checked(output.value!=INVALID_HANDLE_VALUE,"file_error","Cannot create input snapshot");copy_bytes(input.value,output.value,max_bytes);win_checked(::FlushFileBuffers(output.value)!=0,"file_error","Cannot finish input snapshot");
    std::uintmax_t final_size=0;const auto after=regular_info(input.value,max_bytes,final_size);
    ensure(size==final_size&&before.LastWriteTime.QuadPart==after.LastWriteTime.QuadPart&&before.ChangeTime.QuadPart==after.ChangeTime.QuadPart,"conflict","Input changed while creating snapshot");
    output=Handle();m_impl->bytes=final_size;m_impl->hash=file_sha256(m_impl->path);
#else
    const auto requested=input_path(source);
    Descriptor input(::open(requested.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK));
    checked(input.value>=0,"invalid_path","Cannot open input without following a final symlink");
    struct stat before{};checked(::fstat(input.value,&before)==0,"file_error","Cannot inspect opened input");
    ensure(S_ISREG(before.st_mode)&&before.st_size>=0,"invalid_path","Input must be an opened regular file");
    ensure(static_cast<std::uintmax_t>(before.st_size)<=max_bytes,"limit_exceeded","Input exceeds permitted size");
    // Validate the actual opened vnode, not just the path that was checked before open().
    m_impl->original=input_path(actual_path(input.value).u8string());
    m_impl->path=m_impl->staging.path/("input"+requested.extension().string());
    Descriptor output(::openat(m_impl->staging.descriptor.value,m_impl->path.filename().c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600));
    checked(output.value>=0,"file_error","Cannot create private input snapshot");
    copy_bytes(input.value,output.value,max_bytes);
    checked(::fsync(output.value)==0,"file_error","Cannot finish input snapshot");
    struct stat after{};checked(::fstat(input.value,&after)==0,"file_error","Cannot recheck opened input");
    ensure(unchanged(before,after),"conflict","Input changed while creating its snapshot");
    m_impl->bytes=static_cast<std::uintmax_t>(after.st_size);
    m_impl->hash=file_sha256(m_impl->path);
#endif
}
InputSnapshot::~InputSnapshot()=default;
const fs::path& InputSnapshot::path()const{return m_impl->path;}
const fs::path& InputSnapshot::original()const{return m_impl->original;}
const std::string& InputSnapshot::sha256()const{return m_impl->hash;}
std::uintmax_t InputSnapshot::size()const{return m_impl->bytes;}

struct OutputArtifact::Impl {
    fs::path destination,path;
    bool overwrite=false;
    PrivateDirectory staging;
#ifdef _WIN32
    Handle parent,publication;
    bool publication_committed=false;
    ~Impl(){if(publication.value!=INVALID_HANDLE_VALUE&&!publication_committed){FILE_DISPOSITION_INFO info{TRUE};::SetFileInformationByHandle(publication.value,FileDispositionInfo,&info,sizeof(info));}}
#else
    Descriptor parent;
    std::string publication_name;
    ~Impl(){if(parent.value>=0&&!publication_name.empty())::unlinkat(parent.value,publication_name.c_str(),0);}
#endif
};
OutputArtifact::OutputArtifact(const Json& destination,bool overwrite):m_impl(std::make_unique<Impl>()) {
#ifdef _WIN32
    m_impl->overwrite=overwrite;m_impl->destination=output_path(destination,overwrite);
    m_impl->parent=Handle(::CreateFileW(m_impl->destination.parent_path().c_str(),FILE_READ_ATTRIBUTES|FILE_LIST_DIRECTORY|FILE_ADD_FILE|FILE_DELETE_CHILD|SYNCHRONIZE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    win_checked(m_impl->parent.value!=INVALID_HANDLE_VALUE,"invalid_path","Cannot pin output directory");
    FILE_ATTRIBUTE_TAG_INFO tags{};win_checked(::GetFileInformationByHandleEx(m_impl->parent.value,FileAttributeTagInfo,&tags,sizeof(tags))!=0,"file_error","Cannot inspect output directory");ensure((tags.FileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0&&(tags.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)==0,"invalid_path","Output parent must be a directory without reparse points");
    const auto actual=actual_path(m_impl->parent.value);m_impl->destination=output_path((actual/m_impl->destination.filename()).u8string(),overwrite);m_impl->path=m_impl->staging.path/("artifact"+m_impl->destination.extension().string());
#else
    m_impl->overwrite=overwrite;m_impl->destination=output_path(destination,overwrite);
    m_impl->parent=Descriptor(::open(m_impl->destination.parent_path().c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
    checked(m_impl->parent.value>=0,"invalid_path","Cannot pin output directory");
    const auto actual=actual_path(m_impl->parent.value);
    m_impl->destination=output_path((actual/m_impl->destination.filename()).u8string(),overwrite);
    m_impl->path=m_impl->staging.path/("artifact"+m_impl->destination.extension().string());
#endif
}
OutputArtifact::~OutputArtifact()=default;
const fs::path& OutputArtifact::path()const{return m_impl->path;}
const fs::path& OutputArtifact::destination()const{return m_impl->destination;}
Json OutputArtifact::publish() {
#ifdef _WIN32
    const auto actual=actual_path(m_impl->parent.value);ensure(actual==m_impl->destination.parent_path(),"conflict","Output directory moved during export");output_path((actual/m_impl->destination.filename()).u8string(),m_impl->overwrite);
    Handle source(::CreateFileW(m_impl->path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr));win_checked(source.value!=INVALID_HANDLE_VALUE,"export_failed","Native export did not produce an artifact");
    constexpr std::uintmax_t limit=2ULL*1024*1024*1024;std::uintmax_t size=0;const auto before=regular_info(source.value,limit,size);
    m_impl->publication=exclusive_relative_file(m_impl->parent.value,L"."+m_impl->staging.path.filename().wstring()+L".publish");copy_bytes(source.value,m_impl->publication.value,limit);win_checked(::FlushFileBuffers(m_impl->publication.value)!=0,"file_error","Cannot finish staged artifact");
    std::uintmax_t final_size=0;const auto after=regular_info(source.value,limit,final_size);ensure(size==final_size&&before.LastWriteTime.QuadPart==after.LastWriteTime.QuadPart&&before.ChangeTime.QuadPart==after.ChangeTime.QuadPart,"conflict","Export changed during publication");const auto hash=file_sha256(m_impl->path);
    const auto name=m_impl->destination.filename().wstring();const DWORD bytes=static_cast<DWORD>(sizeof(FILE_RENAME_INFO)+name.size()*sizeof(wchar_t));std::vector<unsigned char> buffer(bytes,0);auto* rename=reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());rename->ReplaceIfExists=m_impl->overwrite?TRUE:FALSE;rename->RootDirectory=m_impl->parent.value;rename->FileNameLength=static_cast<DWORD>(name.size()*sizeof(wchar_t));std::copy(name.begin(),name.end(),rename->FileName);
    const BOOL renamed=::SetFileInformationByHandle(m_impl->publication.value,FileRenameInfo,rename,bytes);const DWORD error=::GetLastError();win_checked(renamed!=0,error==ERROR_ALREADY_EXISTS||error==ERROR_FILE_EXISTS?"already_exists":"file_error","Atomic output publication failed");m_impl->publication_committed=true;
    return {{"path",m_impl->destination.u8string()},{"sha256",hash},{"size",final_size}};
#else
    const auto parent_path=actual_path(m_impl->parent.value);
    ensure(parent_path==m_impl->destination.parent_path(),"conflict","Output directory moved during export");
    output_path((parent_path/m_impl->destination.filename()).u8string(),m_impl->overwrite);
    Descriptor source(::openat(m_impl->staging.descriptor.value,m_impl->path.filename().c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK));
    checked(source.value>=0,"export_failed","Native export did not produce a regular artifact");
    struct stat before{};checked(::fstat(source.value,&before)==0,"file_error","Cannot inspect staged artifact");
    ensure(S_ISREG(before.st_mode)&&before.st_size>=0,"export_failed","Export must produce a regular file");
    constexpr std::uintmax_t limit=2ULL*1024*1024*1024;
    ensure(static_cast<std::uintmax_t>(before.st_size)<=limit,"limit_exceeded","Export exceeds 2 GiB publication limit");
    const std::string publication_name="."+m_impl->staging.path.filename().string()+".publish";
    Descriptor output(::openat(m_impl->parent.value,publication_name.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600));
    checked(output.value>=0,"file_error","Cannot create exclusive staged output");
    m_impl->publication_name=publication_name;
    copy_bytes(source.value,output.value,limit);
    checked(::fsync(output.value)==0,"file_error","Cannot finish output artifact");
    struct stat after{};checked(::fstat(source.value,&after)==0,"file_error","Cannot recheck staged artifact");
    ensure(unchanged(before,after),"conflict","Export artifact changed during publication");
    const auto hash=file_sha256(m_impl->path);
    const std::string name=m_impl->destination.filename().string();
    if(m_impl->overwrite)checked(::renameat(m_impl->parent.value,m_impl->publication_name.c_str(),m_impl->parent.value,name.c_str())==0,"file_error","Atomic output replacement failed");
    else {
        const int linked=::linkat(m_impl->parent.value,m_impl->publication_name.c_str(),m_impl->parent.value,name.c_str(),0);
        checked(linked==0,errno==EEXIST?"already_exists":"file_error","Atomic no-replace publication failed");
        // Publication has succeeded. A cleanup failure must not turn a known
        // successful commit into an error that invites a duplicate export.
        if(::unlinkat(m_impl->parent.value,m_impl->publication_name.c_str(),0)!=0)
            return {{"path",m_impl->destination.u8string()},{"sha256",hash},{"size",static_cast<std::uintmax_t>(after.st_size)}};
    }
    m_impl->publication_name.clear();
    // The file's contents have been synced; directory sync is best effort on filesystems that permit it.
    (void)::fsync(m_impl->parent.value);
    return {{"path",m_impl->destination.u8string()},{"sha256",hash},{"size",static_cast<std::uintmax_t>(after.st_size)}};
#endif
}
} // namespace Slic3r::AgentControl
