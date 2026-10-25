#include <cstddef>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include "slic3r/AgentControl/Commands.hpp"

#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

using Json = nlohmann::json;
using Slic3r::AgentControl::Commands;

void require(bool condition, const char *message)
{
    INFO(message);
    REQUIRE(condition);
}
Json request(const Commands &commands, int id, const std::string &method = "scene.describe")
{
    return {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", {{"target", commands.target()}}}};
}
Json status(Commands &commands, const std::string &job)
{
    auto input = request(commands, -1, "jobs.get");
    input["params"]["job_id"] = job;
    return commands.receive(input).at("result");
}
TEST_CASE("Queued commands preserve targets, retries and terminal outcomes", "[AgentControlCommands]")
{
    try {
        Commands commands("session", "window");
        for (const auto &invalid : {Json(), Json::array(), Json{{"jsonrpc", "2.0"}},
                                  Json{{"jsonrpc", "2.0"}, {"id", true}, {"method", "system.describe"}}})
            require(commands.receive(invalid).at("error").at("code") == -32600, "Invalid envelope admitted");
        auto stale = request(commands, 1);
        stale["params"]["target"]["window_id"] = "other";
        require(commands.receive(stale).at("error").at("data").at("kind") == "stale_target", "Wrong window admitted");
        auto missing = request(commands, 1);
        missing["params"].erase("target");
        require(commands.receive(missing).contains("error"), "Implicit target admitted");
        require(commands.receive(request(commands, 1, "shell.execute")).at("error").at("code") == -32601,
                "Unknown method admitted");
        auto describe = request(commands, 1, "system.describe");
        describe.erase("params");
        const auto description = commands.receive(describe);
        require(description.contains("result"), "Discovery requires an already known target");
        const auto first_job = description.at("result").at("job_id").get<std::string>();
        require(commands.receive(describe) == description, "Retry creates a second job");
        auto conflict = describe;
        conflict["params"] = Json::object();
        require(commands.receive(conflict).at("error").at("data").at("kind") == "request_conflict",
                "Reused request id changes payload");
        Commands::Work work;
        require(commands.take(work), "Queued work missing");
        require(work.job_id == first_job, "Retry duplicated queued work");
        require(status(commands, first_job).at("state") == "running", "Dequeued work is not running");
        require(!commands.take(work), "Deduplicated request executes twice");
        commands.finish(first_job, {{"actual", 4}});
        require(status(commands, first_job).at("result").at("actual") == 4, "Completion result lost");
        commands.fail(first_job, "late_failure", "ignored");
        require(status(commands, first_job).at("state") == "succeeded", "Terminal outcome overwritten");
        const auto cancelled = commands.receive(request(commands, 2)).at("result").at("job_id").get<std::string>();
        require(commands.cancel_queued(cancelled), "Queued cancellation failed");
        require(!commands.take(work), "Cancelled queued command executed");
        require(status(commands, cancelled).at("state") == "cancelled", "Cancellation state missing");
        const auto pending = commands.receive(request(commands, 3)).at("result").at("job_id").get<std::string>();
        require(commands.take(work), "Pending command missing");
        require(!commands.cancel_queued(pending), "Running command treated as queued cancellation");
        commands.waiting_for_user(pending, true);
        require(status(commands, pending).at("waiting_for_user") == true, "Modal wait is not visible to the client");
        commands.stop();
        commands.finish(pending, {{"too_late", true}});
        require(!commands.take(work), "Work dequeued after stop");
        require(commands.receive(request(commands, 4)).at("error").at("data").at("kind") == "closing",
                "Closing window admits new work");
        commands.stop();

        Commands bounded("session", "bounded");
        std::string completed;
        for (int id = 0; id < 256; ++id) {
            const auto admitted = bounded.receive(request(bounded, id));
            require(admitted.contains("result"), "Queue fills below its documented bound");
            if (id == 0) completed = admitted.at("result").at("job_id");
        }
        require(bounded.receive(request(bounded, 256)).at("error").at("data").at("kind") == "busy", "Queue limit exceeded");
        bounded.finish(completed, Json::object());
        require(bounded.receive(request(bounded, 256)).contains("result"), "Terminal history cannot be evicted");
        auto evicted = request(bounded, -1, "jobs.get");
        evicted["params"]["job_id"] = completed;
        require(bounded.receive(evicted).at("error").at("data").at("kind") == "unknown_job", "Evicted history still returned");

        Commands timed("session", "timed");
        const auto expired = timed.receive(request(timed, 1)).at("result").at("job_id").get<std::string>();
        require(!timed.take(work, std::chrono::steady_clock::now() + std::chrono::seconds(16)),
                "Expired queued command executed");
        require(status(timed, expired).at("error").at("kind") == "queue_timeout", "Expired outcome missing");

        Commands priority("session", "priority");
        const auto target_job = priority.receive(request(priority, 1)).at("result").at("job_id").get<std::string>();
        auto cancel = request(priority, 2, "jobs.cancel");
        cancel["params"]["job_id"] = target_job;
        priority.receive(cancel);
        require(priority.take(work), "Cancellation request missing");
        require(work.method == "jobs.cancel", "Cancellation runs after its queued target");
        require(priority.cancel_queued(target_job), "Cancellation failed before the target runs");
        priority.finish(work.job_id, {{"cancelled", true}});
        require(!priority.take(work), "Cancelled target ran after acknowledgement");

        Commands churn("session", "churn");
        for (int i = 0; i < 2000; ++i) {
            const auto admitted = churn.receive(request(churn, i));
            require(admitted.contains("result"), "Cancelled history blocks admission");
            require(churn.cancel_queued(admitted.at("result").at("job_id")), "Churn cancellation failed");
        }
        require(!churn.take(work), "Evicted cancelled work ran later");
        std::cout << "Command contract checks passed\n";
        return;
    } catch (const std::exception &error) {
        FAIL(error.what());
    }
}

TEST_CASE("Oversized requests do not reserve work or request IDs", "[AgentControlCommands]")
{
    Commands commands("session", "sizes");
    auto oversized = request(commands, 1);
    oversized["params"]["payload"] = std::string(64 * 1024, 'x');
    REQUIRE(commands.receive(oversized).at("error").at("data").at("kind") == "request_too_large");
    Commands::Work work;
    REQUIRE_FALSE(commands.take(work));
    const auto accepted = commands.receive(request(commands, 1));
    REQUIRE(accepted.contains("result"));
    REQUIRE(commands.take(work));
    REQUIRE(work.job_id == accepted.at("result").at("job_id"));
}

TEST_CASE("Request memory pressure preserves live jobs and evicts only completed history", "[AgentControlCommands]")
{
    Commands commands("session", "memory");
    auto large = request(commands, 0);
    large["params"]["payload"] = std::string(60 * 1024, 'x');
    const auto original = commands.receive(large);
    REQUIRE(original.contains("result"));
    const auto first_job = original.at("result").at("job_id").get<std::string>();
    std::string last_job;
    int rejected_id = 0;
    for (int id = 1; id < 256; ++id) {
        auto next = large;
        next["id"] = id;
        const auto response = commands.receive(next);
        if (response.contains("error")) {
            REQUIRE(response.at("error").at("data").at("kind") == "busy");
            rejected_id = id;
            break;
        }
        last_job = response.at("result").at("job_id");
    }
    REQUIRE(rejected_id > 0);
    REQUIRE(rejected_id < 256); // The byte budget, rather than the job count, is enforced.
    REQUIRE(commands.receive(large) == original); // A retry consumes no additional budget.
    REQUIRE(status(commands, first_job).at("state") == "queued");
    REQUIRE(status(commands, last_job).at("state") == "queued");

    Commands::Work work;
    REQUIRE(commands.take(work));
    REQUIRE(work.job_id == first_job);
    auto blocked = large;
    blocked["id"] = rejected_id;
    REQUIRE(commands.receive(blocked).contains("error")); // Running work cannot be evicted.
    commands.finish(first_job, Json::object());
    REQUIRE(commands.receive(blocked).contains("result"));
    auto missing = request(commands, -1, "jobs.get");
    missing["params"]["job_id"] = first_job;
    REQUIRE(commands.receive(missing).at("error").at("data").at("kind") == "unknown_job");
    REQUIRE(status(commands, last_job).at("state") == "queued");
}

TEST_CASE("Evicted cancelled payloads release their admission budget", "[AgentControlCommands]")
{
    Commands commands("session", "memory-churn");
    auto large = request(commands, 0);
    large["params"]["payload"] = std::string(60 * 1024, 'x');
    for (int id = 0; id < 300; ++id) {
        large["id"] = id;
        const auto admitted = commands.receive(large);
        REQUIRE(admitted.contains("result"));
        REQUIRE(commands.cancel_queued(admitted.at("result").at("job_id")));
    }
    Commands::Work work;
    REQUIRE_FALSE(commands.take(work));
}

TEST_CASE("Large terminal results evict old history without disturbing active work", "[AgentControlCommands]")
{
    Commands commands("session", "result-memory");
    const auto active_request = request(commands, 1);
    const auto active = commands.receive(active_request);
    const std::string active_job = active.at("result").at("job_id");
    Commands::Work work;
    REQUIRE(commands.take(work));
    REQUIRE(work.job_id == active_job);
    commands.progress(active_job, 7, "active");
    commands.waiting_for_user(active_job, true);

    const Json image {{"data", std::string(2 * 1024 * 1024, 'x')}};
    std::vector<std::string> completed;
    for (int id = 2; id < 14; ++id) {
        const auto admitted = commands.receive(request(commands, id));
        REQUIRE(admitted.contains("result"));
        const std::string job = admitted.at("result").at("job_id");
        REQUIRE(commands.take(work));
        REQUIRE(work.job_id == job);
        commands.finish(job, image);
        completed.push_back(job);
    }
    CHECK(commands.receive(active_request) == active);
    CHECK(status(commands, active_job).at("state") == "running");
    CHECK(status(commands, active_job).at("waiting_for_user") == true);
    auto probe = request(commands, -1, "jobs.get");
    probe["params"]["job_id"] = completed.front();
    CHECK(commands.receive(probe).at("error").at("data").at("kind") == "unknown_job");
    CHECK(status(commands, completed.back()).at("result") == image);

    std::size_t retained_bytes = 0;
    for (const auto& job : completed) {
        probe["params"]["job_id"] = job;
        const auto reply = commands.receive(probe);
        if (reply.contains("result")) retained_bytes += reply.at("result").at("result").dump().size();
    }
    CHECK(retained_bytes <= 16 * 1024 * 1024);
    // Eviction drops the dedup entry as it did under count/request pressure;
    // mutations still require their original state token after a replay.
    const auto retry = commands.receive(request(commands, 2));
    REQUIRE(retry.contains("result"));
    CHECK(retry.at("result").at("job_id") != completed.front());
}

TEST_CASE("Status eviction releases request and result bytes through sustained churn", "[AgentControlCommands]")
{
    Commands commands("session", "mixed-accounting");
    Json input = request(commands, 1);
    input["params"]["payload"] = std::string(60 * 1024, 'r');
    const Json payload {{"data", std::string(2 * 1024 * 1024, 's')}};
    Commands::Work work;
    std::string last;
    for (int id = 0; id < 48; ++id) {
        input["id"] = id;
        const auto admitted = commands.receive(input);
        REQUIRE(admitted.contains("result"));
        last = admitted.at("result").at("job_id");
        REQUIRE(commands.take(work));
        commands.progress(last, 50, "running");
        commands.waiting_for_user(last, true);
        if (id % 2) commands.finish(last, payload);
        else commands.fail(last, "partial_batch", "Inspect child receipts", payload);
    }
    CHECK(status(commands, last).at("result") == payload);
    commands.stop();
    CHECK(commands.receive(request(commands, 100)).at("error").at("data").at("kind") == "closing");
}

TEST_CASE("Oversized completion keeps a small receipt without inviting mutation replay", "[AgentControlCommands]")
{
    Commands commands("session", "oversized-result");
    const auto input = request(commands, 1, "settings.patch");
    const auto admitted = commands.receive(input);
    const std::string job = admitted.at("result").at("job_id");
    Commands::Work work;
    REQUIRE(commands.take(work));
    commands.finish(job, {{"data", std::string(3 * 1024 * 1024 + 1, 'x')}});
    const auto receipt = status(commands, job);
    CHECK(receipt.at("state") == "failed");
    CHECK(receipt.at("error").at("kind") == "result_too_large");
    CHECK(receipt.at("operation_may_have_completed") == true);
    CHECK_FALSE(receipt.contains("result"));
    CHECK(commands.receive(input) == admitted);
    CHECK_FALSE(commands.take(work));
}

TEST_CASE("Progress remains bounded without truncating UTF-8 code points", "[AgentControlCommands]")
{
    Commands commands("session", "bounded-progress");
    const std::string job = commands.receive(request(commands, 1)).at("result").at("job_id");
    Commands::Work work;
    REQUIRE(commands.take(work));
    std::string message(4095, 'x');
    message += "\xe2\x82\xac";
    commands.progress(job, 10, message);
    const auto progress = status(commands, job);
    CHECK(progress.at("message").get<std::string>().size() <= 4096);
    CHECK_NOTHROW(progress.dump());
    CHECK(progress.at("state") == "running");
}

TEST_CASE("Failure details retain child receipts on the original job", "[AgentControlCommands]")
{
    Commands commands("session", "batch-receipts");
    const std::string job = commands.receive(request(commands, 1)).at("result").at("job_id");
    Commands::Work work;
    REQUIRE(commands.take(work));
    const Json details {{"children", Json::array({{{"job_id", "child:1"}, {"state", "succeeded"}}})}};
    commands.fail(job, "partial_batch", "A later plate failed", details);
    CHECK(status(commands, job).at("error").at("details") == details);
}
