#include "RpcServer.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/system/error_code.hpp>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Slic3r::AgentControl {
namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using Json = nlohmann::json;
constexpr std::size_t max_body = 1024 * 1024;

std::string random_token()
{
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        throw std::runtime_error("Cannot generate agent control token");
    const char *hex = "0123456789abcdef";
    std::string result;
    for (const auto byte : bytes) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}

bool valid_id(const std::string &id)
{
    if (id.size() != 36) return false;
    for (std::size_t i = 0; i < id.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (id[i] != '-') return false;
        } else if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}

#ifdef _WIN32
// Explicit current-user ACL, protected from inherited directory permissions.
class PrivateSecurity {
public:
    PrivateSecurity()
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return;
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<unsigned char> buffer(size);
        bool ok = GetTokenInformation(token, TokenUser, buffer.data(), size, &size) != 0;
        CloseHandle(token);
        if (!ok) return;
        LPWSTR sid = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, &sid)) return;
        std::wstring sddl = L"D:P(A;;FA;;;" + std::wstring(sid) + L")";
        LocalFree(sid);
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &m_sd, nullptr)) {
            attributes = {sizeof(SECURITY_ATTRIBUTES), m_sd, FALSE};
        }
    }
    ~PrivateSecurity() { if (m_sd) LocalFree(m_sd); }
    bool valid() const { return m_sd != nullptr; }
    SECURITY_ATTRIBUTES attributes{};
private:
    PSECURITY_DESCRIPTOR m_sd = nullptr;
};
#endif

bool publish_descriptor(const std::filesystem::path &directory,
                        const std::filesystem::path &path, const std::string &data)
{
#ifdef _WIN32
    PrivateSecurity security;
    if (!security.valid()) return false;
    // Each process creates its own file with an explicit ACL. The directory can
    // be shared by this user's sessions; its contents never grant token access.
    if (!CreateDirectoryW(directory.c_str(), &security.attributes) && GetLastError() != ERROR_ALREADY_EXISTS)
        return false;
    const auto temp = std::filesystem::path(path.wstring() + L".tmp");
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, &security.attributes,
                             CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr)
                    && written == data.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
#else
    if (::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) return false;
    struct stat state{};
    if (::lstat(directory.c_str(), &state) != 0 || !S_ISDIR(state.st_mode)
        || state.st_uid != ::geteuid() || (state.st_mode & 0077) != 0) return false;
    const std::string temp = path.string() + ".tmp";
    const int file = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (file < 0) return false;
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto count = ::write(file, data.data() + offset, data.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        offset += static_cast<std::size_t>(count);
    }
    bool ok = offset == data.size() && ::fsync(file) == 0;
    if (::close(file) != 0) ok = false;
    // link() atomically publishes without replacing an existing owner's file.
    if (ok) ok = ::link(temp.c_str(), path.c_str()) == 0;
    ::unlink(temp.c_str());
    return ok;
#endif
}

unsigned long process_id()
{
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(::getpid());
#endif
}
} // namespace

struct RpcServer::Impl {
    struct Connection;
    explicit Impl(Handler callback) : handler(std::move(callback)), acceptor(io) {}
    Handler handler;
    asio::io_context io;
    tcp::acceptor acceptor;
    std::thread worker;
    std::set<std::shared_ptr<Connection>> connections;
    std::string authorization;
    std::string host;
    std::filesystem::path descriptor_path;
    mutable std::mutex descriptor_mutex;
    Json published;
    bool stopping = false; // Accessed only on worker, except before launch/after join.

    struct Connection : std::enable_shared_from_this<Connection> {
        Connection(Impl &owner, tcp::socket socket) : owner(owner), stream(std::move(socket)) {}
        Impl &owner;
        beast::tcp_stream stream;
        beast::flat_buffer buffer;
        http::request_parser<http::string_body> parser;
        http::response<http::string_body> response;

        void close()
        {
            boost::system::error_code ignored;
            stream.socket().cancel(ignored);
            stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
            stream.socket().close(ignored);
            owner.connections.erase(shared_from_this());
        }
        void read()
        {
            parser.header_limit(16 * 1024);
            parser.body_limit(max_body);
            stream.expires_after(std::chrono::seconds(5));
            http::async_read_header(stream, buffer, parser,
                [self = shared_from_this()](boost::system::error_code ec, std::size_t) {
                    if (ec) { self->close(); return; }
                    self->check_headers();
                });
        }
        void reply(http::status status, std::string body)
        {
            response = {status, 11};
            response.set(http::field::content_type, "application/json");
            response.set(http::field::cache_control, "no-store");
            response.keep_alive(false);
            response.body() = std::move(body);
            response.prepare_payload();
            stream.expires_after(std::chrono::seconds(5));
            http::async_write(stream, response,
                [self = shared_from_this()](boost::system::error_code, std::size_t) { self->close(); });
        }
        void check_headers()
        {
            const auto &request = parser.get();
            const auto auth = request[http::field::authorization];
            if (request.count(http::field::authorization) != 1 || auth.size() != owner.authorization.size()
                || CRYPTO_memcmp(auth.data(), owner.authorization.data(), auth.size()) != 0) {
                reply(http::status::unauthorized, R"({"error":"unauthorized"})"); return;
            }
            if (request.count(http::field::host) != 1 || request[http::field::host] != owner.host
                || request.count(http::field::origin) != 0) {
                reply(http::status::forbidden, R"({"error":"forbidden"})"); return;
            }
            if (request.method() != http::verb::post || request.target() != "/api/v1/rpc") {
                reply(http::status::not_found, R"({"error":"not_found"})"); return;
            }
            const auto content_type = request[http::field::content_type];
            if (request.count(http::field::content_type) != 1
                || (content_type != "application/json" && content_type != "application/json; charset=utf-8")) {
                reply(http::status::unsupported_media_type, R"({"error":"content_type"})"); return;
            }
            http::async_read(stream, buffer, parser,
                [self = shared_from_this()](boost::system::error_code ec, std::size_t) {
                    if (ec) { self->close(); return; }
                    self->dispatch();
                });
        }
        void dispatch()
        {
            bool duplicate_key = false;
            std::vector<std::set<std::string>> object_keys;
            Json request;
            try {
                request = Json::parse(parser.get().body(),
                [&duplicate_key, &object_keys](int depth, Json::parse_event_t event, Json &value) {
                    if (depth > 64) throw std::runtime_error("JSON nesting limit");
                    if (event == Json::parse_event_t::object_start) object_keys.emplace_back();
                    else if (event == Json::parse_event_t::object_end) object_keys.pop_back();
                    else if (event == Json::parse_event_t::key
                             && !object_keys.back().insert(value.get<std::string>()).second)
                        duplicate_key = true;
                    return true;
                }, false);
            } catch (...) {
                request = Json(Json::value_t::discarded);
            }
            if (request.is_discarded() || duplicate_key) {
                reply(http::status::ok, Json{{"jsonrpc", "2.0"}, {"id", nullptr},
                    {"error", {{"code", -32700}, {"message", "Parse error"}}}}.dump()); return;
            }
            try {
                auto result = owner.handler(request).dump();
                if (result.size() > 4 * max_body) throw std::runtime_error("Response too large");
                reply(http::status::ok, std::move(result));
            } catch (...) {
                reply(http::status::ok, Json{{"jsonrpc", "2.0"}, {"id", nullptr},
                    {"error", {{"code", -32603}, {"message", "Internal error"}}}}.dump());
            }
        }
    };

    void accept()
    {
        acceptor.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
            if (stopping) return;
            if (!ec && connections.size() < 8) {
                auto connection = std::make_shared<Connection>(*this, std::move(socket));
                connections.insert(connection);
                connection->read();
            }
            if (acceptor.is_open()) accept();
        });
    }
};

RpcServer::RpcServer(Handler handler) : m_impl(std::make_unique<Impl>(std::move(handler))) {}
RpcServer::~RpcServer() { stop(); }

bool RpcServer::start(const std::string &runtime_dir, const std::string &session_id,
                      const std::string &window_id)
{
    auto &impl = *m_impl;
    if (impl.worker.joinable() || !valid_id(session_id) || !valid_id(window_id) || !impl.handler)
        return false;
    try {
        impl.io.restart();
        impl.stopping = false;
        impl.authorization = "Bearer " + random_token();
        impl.acceptor.open(tcp::v4());
        impl.acceptor.bind({asio::ip::make_address_v4("127.0.0.1"), 0});
        impl.acceptor.listen(8);
        impl.host = "127.0.0.1:" + std::to_string(impl.acceptor.local_endpoint().port());
        Json descriptor{{"session_id", session_id}, {"window_id", window_id}, {"protocol_version", 1},
                        {"endpoint", "http://" + impl.host + "/api/v1/rpc"},
                        {"auth_token", impl.authorization.substr(7)}, {"pid", process_id()}};
        const auto directory = std::filesystem::u8path(runtime_dir) / "agent-control";
        const auto path = directory / (session_id + "-" + window_id + ".json");
        if (!publish_descriptor(directory, path, descriptor.dump())) throw std::runtime_error("Discovery unavailable");
        impl.descriptor_path = path;
        {
            std::lock_guard<std::mutex> lock(impl.descriptor_mutex);
            impl.published = std::move(descriptor);
        }
        impl.accept();
        impl.worker = std::thread([&impl] { impl.io.run(); });
        return true;
    } catch (...) {
        stop();
        return false;
    }
}

void RpcServer::stop()
{
    auto &impl = *m_impl;
    const auto close = [&impl] {
        impl.stopping = true;
        boost::system::error_code ignored;
        impl.acceptor.close(ignored);
        const auto connections = impl.connections;
        for (const auto &connection : connections) connection->close();
    };
    if (impl.worker.joinable()) {
        asio::post(impl.io, close);
        impl.worker.join();
    } else {
        close();
        impl.io.poll();
    }
    if (!impl.descriptor_path.empty()) {
        std::error_code ignored;
        std::filesystem::remove(impl.descriptor_path, ignored);
        impl.descriptor_path.clear();
    }
    std::lock_guard<std::mutex> lock(impl.descriptor_mutex);
    impl.published = nullptr;
    impl.authorization.clear();
}

nlohmann::json RpcServer::descriptor() const
{
    std::lock_guard<std::mutex> lock(m_impl->descriptor_mutex);
    return m_impl->published;
}

} // namespace Slic3r::AgentControl
