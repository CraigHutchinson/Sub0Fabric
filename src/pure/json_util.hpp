#pragma once
// Internal JSON helpers (simdjson stays out of public headers). Writes use simdjson's string_builder with automatic commas; reads use the on-demand
// forward parser through a pool, with a reusable padded buffer so a steady-state decode copies the line once and allocates nothing for the parse.
// The writer and the pool follow Sub0Canvas's lib/src/json_util.hpp (Fabric depends on no other Sub0 project, so the idiom is repeated here, not shared).
#include <simdjson.h>

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace sub0fabric::json {

/// Comma-managing writer over simdjson::builder::string_builder.
class Writer
{
public:
    Writer() : buffer_(256) {}
    Writer& beginObject() { sep(); buffer_.append('{'); first_[depth_++] = true; return *this; }
    Writer& endObject() { buffer_.append('}'); --depth_; return *this; }
    /// Open an array; its elements are separated by the same automatic commas as an object's members.
    Writer& beginArray() { sep(); buffer_.append('['); first_[depth_++] = true; return *this; }
    Writer& endArray() { buffer_.append(']'); --depth_; return *this; }
    Writer& key(std::string_view k) { sep(); buffer_.escape_and_append_with_quotes(k); buffer_.append(':'); pendingKey_ = true; return *this; }
    Writer& str(std::string_view v) { sep(); buffer_.escape_and_append_with_quotes(v); return *this; }
    Writer& num(std::int64_t v) { sep(); buffer_.append(v); return *this; }
    /// An unsigned number: a `seq` above INT64_MAX must never pass through a signed cast.
    Writer& num(std::uint64_t v) { sep(); buffer_.append(v); return *this; }
    Writer& num(double v) { sep(); buffer_.append(v); return *this; }
    Writer& raw(std::string_view v) { sep(); buffer_.append_raw(v); return *this; }
    Writer& kv(std::string_view k, std::string_view v) { return key(k).str(v); }
    Writer& kv(std::string_view k, std::int64_t v) { return key(k).num(v); }
    Writer& kv(std::string_view k, std::uint64_t v) { return key(k).num(v); }
    Writer& kvRaw(std::string_view k, std::string_view v) { return key(k).raw(v); }

    /// The text so far (valid until the next write).
    [[nodiscard]] std::string_view view() { std::string_view v; if (buffer_.view().get(v)) return {}; return v; }

private:
    void sep()
    {
        if (pendingKey_) { pendingKey_ = false; return; }
        if (depth_ == 0) return;
        if (first_[depth_ - 1]) first_[depth_ - 1] = false; else buffer_.append(',');
    }
    simdjson::builder::string_builder buffer_;
    std::array<bool, 8> first_{};   ///< per open object or array: nothing written yet. Containers nest at most 8 deep, objects and arrays together (the protocol's responses use 4)
    std::size_t depth_ = 0;
    bool pendingKey_ = false;
};

/// A parser and its padded copy buffer, borrowed from a process-wide pool and returned when the session ends. Not thread_local: MinGW-w64 runs
/// thread_local destructors after the thread's TLS storage is freed, which crashed threads that had parsed JSON (the same note as Sub0Canvas).
class ParseSession
{
public:
    ParseSession()
    {
        std::lock_guard lock{pool().mutex};
        if (!pool().free.empty()) { slot_ = std::move(pool().free.back()); pool().free.pop_back(); }
        else slot_ = std::make_unique<Slot>();
    }
    ~ParseSession()
    {
        std::lock_guard lock{pool().mutex};
        pool().free.push_back(std::move(slot_));
    }
    ParseSession(const ParseSession&) = delete;
    ParseSession& operator=(const ParseSession&) = delete;

    /// Parse `text` (copied once into the reusable padded buffer). Throws simdjson::simdjson_error on malformed input.
    simdjson::ondemand::document iterate(std::string_view text)
    {
        slot_->buffer.assign(text);
        slot_->buffer.append(simdjson::SIMDJSON_PADDING, '\0');
        return slot_->parser.iterate(slot_->buffer.data(), text.size(), slot_->buffer.size());
    }

private:
    struct Slot
    {
        simdjson::ondemand::parser parser;
        std::string buffer;
    };
    struct Pool
    {
        std::mutex mutex;
        std::vector<std::unique_ptr<Slot>> free;   ///< at most the peak number of parses at once
    };
    static Pool& pool() { static Pool p; return p; }
    std::unique_ptr<Slot> slot_;
};

/// A raw value's text without the whitespace simdjson leaves after it.
inline std::string_view trimRaw(std::string_view raw)
{
    while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\n' || raw.back() == '\t' || raw.back() == '\r')) raw.remove_suffix(1);
    return raw;
}

template <class V>
std::string toString(V&& v) { std::string_view sv = v.get_string(); return std::string{sv}; }

}  // namespace sub0fabric::json
