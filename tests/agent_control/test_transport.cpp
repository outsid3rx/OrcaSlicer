#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include "slic3r/AgentControl/RpcServer.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/system/error_code.hpp>
#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace asio = boost::asio;
namespace http = boost::beast::http;
using tcp = asio::ip::tcp;
using Json = nlohmann::json;
using Slic3r::AgentControl::RpcServer;

void require(bool condition, const char *message)
{
    INFO(message);
    REQUIRE(condition);
}
std::string host(const Json &descriptor)
{
    const auto endpoint = descriptor.at("endpoint").get<std::string>();
    return endpoint.substr(7, endpoint.find('/', 7) - 7);
}
void connect(tcp::socket &socket, const Json &descriptor)
{
    const auto target = host(descriptor);
    socket.connect({asio::ip::make_address("127.0.0.1"),
                    static_cast<unsigned short>(std::stoi(target.substr(target.find(':') + 1)))});
}
http::response<http::string_body> request(const Json &descriptor, const std::string &token,
                                         const std::string &host_header, bool origin = false,
                                         const std::string &body = R"({"jsonrpc":"2.0","id":1,"method":"system.describe"})",
                                         const std::string &content_type = "application/json")
{
    asio::io_context io;
    tcp::socket socket(io);
    connect(socket, descriptor);
    http::request<http::string_body> input(http::verb::post, "/api/v1/rpc", 11);
    input.set(http::field::host, host_header);
    input.set(http::field::authorization, "Bearer " + token);
    input.set(http::field::content_type, content_type);
    if (origin) input.set(http::field::origin, "https://example.org");
    input.body() = body;
    input.prepare_payload();
    http::write(socket, input);
    boost::beast::flat_buffer buffer;
    http::response<http::string_body> output;
    http::read(socket, buffer, output);
    return output;
}
TEST_CASE("Local control authenticates requests and isolates window lifetimes", "[AgentControlTransport]")
{
    const auto root = std::filesystem::temp_directory_path() /
        ("orca-agent-transport-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(root);
    try {
        std::atomic<int> calls{0};
        auto handler = [&calls](const Json &input) {
            ++calls;
            return Json{{"jsonrpc", "2.0"}, {"id", input.at("id")}, {"result", "ok"}};
        };
        RpcServer first(handler), second(handler);
        const std::string session = "12345678-1234-4234-8234-123456789abc";
        const std::string window1 = "12345678-1234-4234-8234-123456789ab1";
        const std::string window2 = "12345678-1234-4234-8234-123456789ab2";
        require(!first.start(root.string(), "../bad", window1), "Unsafe identifier accepted");
        require(first.start(root.string(), session, window1), "First server failed to start");
        require(!first.start(root.string(), session, window1), "Double start accepted");
        require(second.start(root.string(), session, window2), "Second window failed to start");
        const auto one = first.descriptor();
        const auto two = second.descriptor();
        require(one.at("endpoint") != two.at("endpoint"), "Window endpoints collide");
        require(one.at("auth_token") != two.at("auth_token"), "Window tokens collide");
        const auto file1 = root / "agent-control" / (session + "-" + window1 + ".json");
        const auto file2 = root / "agent-control" / (session + "-" + window2 + ".json");
        require(std::filesystem::exists(file1), "First descriptor missing");
        require(std::filesystem::exists(file2), "Second descriptor missing");
#ifndef _WIN32
        struct stat state{};
        require(::stat(file1.c_str(), &state) == 0, "Cannot stat descriptor");
        require((state.st_mode & 0077) == 0, "Descriptor exposes credentials");
#endif
        RpcServer collision(handler);
        require(!collision.start(root.string(), session, window1), "Existing descriptor replaced");
        require(std::filesystem::exists(file1), "Failed start removed another server descriptor");
#ifndef _WIN32
        const auto insecure = root / "insecure";
        std::filesystem::create_directory(insecure);
        std::filesystem::create_directory(insecure / "agent-control");
        ::chmod((insecure / "agent-control").c_str(), 0755);
        RpcServer exposed(handler);
        require(!exposed.start(insecure.string(), session, window1), "Unsafe discovery directory accepted");
#endif
        const auto token = one.at("auth_token").get<std::string>();
        {
            asio::io_context io;
            tcp::socket unauthenticated(io);
            connect(unauthenticated, one);
            const std::string headers = "POST /api/v1/rpc HTTP/1.1\r\nHost: " + host(one)
                + "\r\nAuthorization: Bearer wrong\r\nContent-Length: 100\r\n\r\n";
            asio::write(unauthenticated, asio::buffer(headers));
            boost::beast::flat_buffer buffer;
            http::response<http::string_body> rejected;
            http::read(unauthenticated, buffer, rejected);
            require(rejected.result() == http::status::unauthorized, "Authentication waits for request body");
        }
        require(request(one, "wrong", host(one)).result() == http::status::unauthorized, "Wrong token accepted");
        require(request(one, token, "localhost").result() == http::status::forbidden, "Host bypass accepted");
        require(request(one, token, host(one), true).result() == http::status::forbidden, "Origin accepted");
        require(request(one, token, host(one), false, "{}", "text/plain").result() == http::status::unsupported_media_type,
                "Unexpected content type accepted");
        require(calls == 0, "Rejected request reached handler");
        const auto response = request(one, token, host(one));
        require(response.result() == http::status::ok, "Authenticated request failed");
        require(Json::parse(response.body()).at("result") == "ok", "Handler response changed");
        require(calls == 1, "Handler not invoked exactly once");
        const auto malformed = request(one, token, host(one), false, "{");
        require(Json::parse(malformed.body()).at("error").at("code") == -32700, "Invalid JSON response missing");
        require(calls == 1, "Invalid JSON reached handler");
        const auto duplicate = request(one, token, host(one), false, R"({"id":1,"id":2})");
        require(Json::parse(duplicate.body()).contains("error"), "Duplicate keys accepted");
        require(calls == 1, "Duplicate keys reached handler");

        const auto nested = request(one, token, host(one), false, std::string(100, '[') + "0" + std::string(100, ']'));
        require(Json::parse(nested.body()).contains("error"), "Excessive JSON nesting accepted");
        require(calls == 1, "Deep JSON reached handler");

        require(request(two, token, host(two)).result() == http::status::unauthorized, "One window token authorizes another");
        require(request(two, two.at("auth_token").get<std::string>(), host(two)).result() == http::status::ok,
                "Second window is unreachable");
        require(calls == 2, "Second window request was not dispatched");
        {
            asio::io_context io;
            tcp::socket oversized(io);
            connect(oversized, one);
            const std::string headers = "POST /api/v1/rpc HTTP/1.1\r\nHost: " + host(one)
                + "\r\nAuthorization: Bearer " + token + "\r\nContent-Type: application/json"
                + "\r\nContent-Length: 1048577\r\n\r\n";
            asio::write(oversized, asio::buffer(headers));
            char byte;
            boost::system::error_code ec;
            oversized.read_some(asio::buffer(&byte, 1), ec);
            require(bool(ec), "Oversized request was not closed at headers");
            require(calls == 2, "Oversized request reached handler");
        }

        {
            asio::io_context io;
            std::vector<std::unique_ptr<tcp::socket>> clients;
            for (int index = 0; index < 9; ++index) {
                clients.emplace_back(std::make_unique<tcp::socket>(io));
                connect(*clients.back(), one);
            }
            const auto began = std::chrono::steady_clock::now();
            char byte;
            boost::system::error_code ec;
            clients.back()->read_some(asio::buffer(&byte, 1), ec);
            require(bool(ec), "Concurrent connection limit exceeded");
            require(std::chrono::steady_clock::now() - began < std::chrono::seconds(1), "Excess connection waits for timeout");
        }
        {
            asio::io_context io;
            tcp::socket slow(io);
            connect(slow, two);
            const auto began = std::chrono::steady_clock::now();
            char byte;
            boost::system::error_code ec;
            slow.read_some(asio::buffer(&byte, 1), ec);
            require(bool(ec), "Header deadline did not close client");
            const auto elapsed = std::chrono::steady_clock::now() - began;
            require(elapsed >= std::chrono::seconds(4), "Header deadline closes a normal idle connection too early");
            require(elapsed < std::chrono::seconds(7), "Header deadline is unbounded");
        }

        // A client that never completes its HTTP header must not delay shutdown.
        asio::io_context io;
        tcp::socket stalled(io);
        connect(stalled, one);
        asio::write(stalled, asio::buffer("POST /api/v1/rpc HTTP/1.1\r\n", 27));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto start = std::chrono::steady_clock::now();
        first.stop();
        require(std::chrono::steady_clock::now() - start < std::chrono::seconds(1), "Shutdown waits on stalled client");
        require(!std::filesystem::exists(file1), "Own descriptor survived shutdown");
        require(std::filesystem::exists(file2), "Another window's descriptor removed");
        first.stop();
        require(first.descriptor().is_null(), "Stopped server exposes old credentials");
        require(first.start(root.string(), session, window1), "Restart failed");
        const auto restarted = first.descriptor();
        require(restarted.at("auth_token") != token, "Restart retained old token");
        require(request(restarted, token, host(restarted)).result() == http::status::unauthorized, "Old token authorizes restart");
        first.stop();
        second.stop();
        std::filesystem::remove_all(root);
        std::cout << "Transport socket checks passed\n";
        return;
    } catch (const std::exception &error) {
        std::filesystem::remove_all(root);
        FAIL(error.what());
    }
}
