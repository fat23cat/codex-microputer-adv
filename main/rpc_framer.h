#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
namespace rpc_framer {
constexpr uint8_t kChannel = 2;
constexpr size_t kPayload = 61, kMaxJson = 4096;
enum class FeedResult : uint8_t { Accepted, Invalid, Oversize };
inline bool complete_json(const std::string& text)
{
    int depth = 0;
    bool started = false, in_string = false, escaped = false;
    for (char ch : text) {
        if (in_string) {
            if (escaped)
                escaped = false;
            else if (ch == '\\')
                escaped = true;
            else if (ch == '"')
                in_string = false;
            continue;
        }
        if (ch == '"')
            in_string = true;
        else if (ch == '{' || ch == '[') {
            ++depth;
            started = true;
        } else if (ch == '}' || ch == ']') {
            if (--depth < 0)
                return false;
        } else if (started && depth == 0 && ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
            return false;
    }
    return started && depth == 0 && !in_string;
}
class Assembler {
  public:
    Assembler()
    {
        input_.reserve(512);
    }
    template <typename Handler>
    FeedResult feed(const uint8_t* body, size_t length, Handler&& handler)
    {
        if (!body || length < 2 || body[0] != kChannel)
            return FeedResult::Invalid;
        size_t count = body[1];
        if (count > kPayload || count + 2 > length) {
            reset();
            return FeedResult::Invalid;
        }
        input_.append(reinterpret_cast<const char*>(body + 2), count);
        if (input_.size() > kMaxJson) {
            reset();
            return FeedResult::Oversize;
        }
        size_t nl = 0;
        while ((nl = input_.find('\n')) != std::string::npos) {
            std::string line = input_.substr(0, nl);
            input_.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (!line.empty())
                handler(line);
        }
        if (!input_.empty() && (count < kPayload || complete_json(input_))) {
            std::string message = std::move(input_);
            reset();
            handler(message);
        }
        return FeedResult::Accepted;
    }
    void reset()
    {
        input_.clear();
    }
    size_t buffered() const
    {
        return input_.size();
    }

  private:
    std::string input_;
};
} // namespace rpc_framer
