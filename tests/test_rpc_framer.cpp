#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "rpc_framer.h"

namespace {
int failures = 0;
void check(bool condition, const char* name)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL rpc_framer: " << name << '\n';
}

std::vector<std::array<uint8_t, 63>> reports(const std::string& wire)
{
    std::vector<std::array<uint8_t, 63>> out;
    for (size_t offset = 0; offset < wire.size(); offset += rpc_framer::kPayload) {
        std::array<uint8_t, 63> report{};
        const size_t count = std::min(rpc_framer::kPayload, wire.size() - offset);
        report[0] = rpc_framer::kChannel;
        report[1] = static_cast<uint8_t>(count);
        std::copy_n(wire.data() + offset, count, report.data() + 2);
        out.push_back(report);
    }
    return out;
}

void exact_report_boundary_completes()
{
    std::string json = R"({"method":"v.oai.thstatus","params":[],"pad":")";
    while ((json.size() + 2) % rpc_framer::kPayload != 0) json.push_back('x');
    json += R"("})";
    check(json.size() % rpc_framer::kPayload == 0, "fixture ends at 61-byte boundary");

    rpc_framer::Assembler assembler;
    std::vector<std::string> messages;
    for (const auto& report : reports(json))
        assembler.feed(report.data(), report.size(),
                       [&](const std::string& message) { messages.push_back(message); });
    check(messages.size() == 1, "balanced full final packet emits message");
    check(!messages.empty() && messages[0] == json, "boundary message is byte exact");
    check(assembler.buffered() == 0, "boundary message leaves no buffered bytes");
}

void newline_and_short_packet_complete()
{
    const std::string first = R"({"method":"v.oai.rgbcfg"})";
    const std::string second = R"({"method":"device.status"})";
    rpc_framer::Assembler assembler;
    std::vector<std::string> messages;
    for (const auto& report : reports(first + "\n" + second + "\n"))
        assembler.feed(report.data(), report.size(),
                       [&](const std::string& message) { messages.push_back(message); });
    check(messages.size() == 2, "newline stream emits both messages");
    check(messages[0] == first && messages[1] == second, "newline order is stable");
}

void invalid_and_oversize_reset_buffer()
{
    rpc_framer::Assembler assembler;
    std::array<uint8_t, 63> invalid{};
    invalid[0] = rpc_framer::kChannel;
    invalid[1] = 62;
    auto result = assembler.feed(invalid.data(), invalid.size(), [](const std::string&) {});
    check(result == rpc_framer::FeedResult::Invalid, "oversized report count is invalid");

    std::array<uint8_t, 63> full{};
    full[0] = rpc_framer::kChannel;
    full[1] = rpc_framer::kPayload;
    std::fill(full.begin() + 2, full.end(), 'x');
    result = rpc_framer::FeedResult::Accepted;
    for (size_t i = 0; i <= rpc_framer::kMaxJson / rpc_framer::kPayload; ++i)
        result = assembler.feed(full.data(), full.size(), [](const std::string&) {});
    check(result == rpc_framer::FeedResult::Oversize, "oversize JSON is rejected");
    check(assembler.buffered() == 0, "oversize rejection clears buffer");
}
}  // namespace

int main()
{
    exact_report_boundary_completes();
    newline_and_short_packet_complete();
    invalid_and_oversize_reset_buffer();
    if (failures) return EXIT_FAILURE;
    std::cout << "PASS rpc_framer (3 scenarios)\n";
    return EXIT_SUCCESS;
}
