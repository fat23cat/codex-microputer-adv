#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include "rpc_framer.h"
namespace {
int failures = 0;
void check(bool ok, const char* n)
{
    if (!ok) {
        ++failures;
        std::cerr << "FAIL rpc_framer: " << n << '\n';
    }
}
std::vector<std::array<uint8_t, 63>> reports(const std::string& wire)
{
    std::vector<std::array<uint8_t, 63>> out;
    for (size_t off = 0; off < wire.size(); off += rpc_framer::kPayload) {
        std::array<uint8_t, 63> r{};
        size_t n = std::min(rpc_framer::kPayload, wire.size() - off);
        r[0] = rpc_framer::kChannel;
        r[1] = static_cast<uint8_t>(n);
        std::copy_n(wire.data() + off, n, r.data() + 2);
        out.push_back(r);
    }
    return out;
}
void boundary()
{
    std::string json = R"({"method":"v.oai.thstatus","params":[],"pad":")";
    while ((json.size() + 2) % rpc_framer::kPayload != 0)
        json.push_back('x');
    json += R"("})";
    check(json.size() % rpc_framer::kPayload == 0, "fixture boundary");
    rpc_framer::Assembler a;
    std::vector<std::string> m;
    for (auto& r : reports(json))
        a.feed(r.data(), r.size(), [&](const std::string& s) { m.push_back(s); });
    check(m.size() == 1 && m[0] == json, "boundary emits exact");
    check(a.buffered() == 0, "boundary clears");
}
void newline()
{
    std::string x = R"({"method":"v.oai.rgbcfg"})", y = R"({"method":"device.status"})";
    rpc_framer::Assembler a;
    std::vector<std::string> m;
    for (auto& r : reports(x + "\n" + y + "\n"))
        a.feed(r.data(), r.size(), [&](const std::string& s) { m.push_back(s); });
    check(m.size() == 2 && m[0] == x && m[1] == y, "newline order");
}
void reset()
{
    rpc_framer::Assembler a;
    std::array<uint8_t, 63> r{};
    r[0] = rpc_framer::kChannel;
    r[1] = rpc_framer::kPayload;
    std::fill(r.begin() + 2, r.end(), 'x');
    a.feed(r.data(), r.size(), [](const std::string&) {});
    check(a.buffered() == rpc_framer::kPayload, "partial buffered");
    a.reset();
    check(a.buffered() == 0, "reset clears");
    std::string msg = R"({"method":"device.status"})";
    std::vector<std::string> m;
    for (auto& q : reports(msg))
        a.feed(q.data(), q.size(), [&](const std::string& s) { m.push_back(s); });
    check(m.size() == 1 && m[0] == msg, "new session clean");
}
} // namespace
int main()
{
    boundary();
    newline();
    reset();
    if (failures)
        return EXIT_FAILURE;
    std::cout << "PASS rpc_framer (3 scenarios)\n";
}
