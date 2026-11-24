#include "sub0fabric/pure/codec.hpp"

#include "json_util.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <vector>

namespace sub0fabric {

namespace {

constexpr std::array<std::string_view, 4> kJournalFields{"seq", "ts", "type", "d"};
constexpr std::array<std::string_view, 3> kJournalRequired{"seq", "type", "d"};
constexpr std::array<std::string_view, 6> kCanvasFields{"kind", "writer", "lamport", "subject", "time", "payload"};
constexpr std::array<std::string_view, 3> kCanvasRequired{"kind", "writer", "lamport"};

/// journal-1, byte for byte as Sub0Squad's `Journal` wrote it: `{"seq":N,"ts":T,"type":"...","d":{...}}`. Members this version does not know are ignored on
/// read, as the Squad reader ignores them (the journal is Squad's own file, never a carrier of foreign members).
class Journal1Codec final : public RecordCodec
{
public:
    std::string_view shape() const noexcept override { return "journal-1"; }
    std::string_view defaultOrder() const noexcept override { return "seq"; }
    bool carriesSeq() const noexcept override { return true; }
    bool carriesWriter() const noexcept override { return false; }
    std::span<const std::string_view> fields() const noexcept override { return kJournalFields; }
    std::span<const std::string_view> requiredFields() const noexcept override { return kJournalRequired; }

    void encode(const Record& r, std::string& out) const override
    {
        json::Writer w;
        w.beginObject().key("seq").num(static_cast<std::int64_t>(r.seq)).key("ts").num(r.ts).kv("type", r.kind).kvRaw("d", r.payload.empty() ? "{}" : r.payload).endObject();
        out.append(w.view());
    }

    std::expected<void, std::string> decode(std::string_view text, Record& out) const override
    {
        out.kind.clear();
        out.payload.clear();
        out.seq = 0;
        out.ts = 0.0;
        bool haveSeq = false, haveType = false, haveBody = false;
        try {
            json::ParseSession session;
            auto doc = session.iterate(text);
            simdjson::ondemand::object obj = doc.get_object();
            for (auto field : obj) {
                const std::string_view key = field.unescaped_key();
                auto value = field.value();
                if (key == "seq") { out.seq = value.get_uint64(); haveSeq = true; }
                else if (key == "ts") { out.ts = value.get_double(); }
                else if (key == "type") { std::string_view s = value.get_string(); out.kind.assign(s); haveType = true; }
                else if (key == "d") { out.payload.assign(json::trimRaw(value.raw_json())); haveBody = true; }
            }
        } catch (const simdjson::simdjson_error& e) {
            return std::unexpected(std::format("not a journal-1 record: {}", e.what()));
        }
        if (!haveSeq) return std::unexpected("journal-1 record has no seq");
        if (!haveType) return std::unexpected("journal-1 record has no type");
        if (!haveBody) return std::unexpected("journal-1 record has no d");
        return {};
    }
};

/// canvas-event-1: `{"kind":..,"writer":{"namespace":..,"writer":..},"lamport":N,"subject":..,"time":..,"payload":{..}}` then unknown members, as Sub0Canvas's
/// `eventJson` writes it. No `seq` or `type` (A22): a record's position is the segment's.
class CanvasEventCodec final : public RecordCodec
{
public:
    std::string_view shape() const noexcept override { return "canvas-event-1"; }
    std::string_view defaultOrder() const noexcept override { return "lamport"; }
    bool carriesSeq() const noexcept override { return false; }
    bool carriesWriter() const noexcept override { return true; }
    std::span<const std::string_view> fields() const noexcept override { return kCanvasFields; }
    std::span<const std::string_view> requiredFields() const noexcept override { return kCanvasRequired; }

    void encode(const Record& r, std::string& out) const override
    {
        json::Writer w;
        w.beginObject().kv("kind", r.kind);
        w.key("writer").beginObject().kv("namespace", r.writer.ns).kv("writer", r.writer.writer).endObject();
        w.kv("lamport", static_cast<std::int64_t>(r.lamport)).kv("subject", r.subject).kv("time", r.time);
        w.kvRaw("payload", r.payload.empty() ? std::string_view{"{}"} : std::string_view{r.payload});
        if (!r.extra.empty()) w.raw(r.extra);   // the writer places the comma; the text is members, kept as they were read
        w.endObject();
        out.append(w.view());
    }

    std::expected<void, std::string> decode(std::string_view text, Record& out) const override
    {
        out.kind.clear();
        out.writer.ns.clear();
        out.writer.writer.clear();
        out.lamport = 0;
        out.subject.clear();
        out.time.clear();
        out.payload.clear();
        out.extra.clear();
        bool haveKind = false, haveWriter = false, haveLamport = false;
        std::optional<json::Writer> extra;   // collects unknown members as an object, whose braces are stripped below; made only when there are some
        bool anyExtra = false;
        try {
            json::ParseSession session;
            auto doc = session.iterate(text);
            simdjson::ondemand::object obj = doc.get_object();
            for (auto field : obj) {
                const std::string_view key = field.unescaped_key();
                auto value = field.value();
                if (key == "kind") { std::string_view s = value.get_string(); out.kind.assign(s); haveKind = true; }
                else if (key == "lamport") { out.lamport = value.get_uint64(); haveLamport = true; }
                else if (key == "subject") { std::string_view s = value.get_string(); out.subject.assign(s); }
                else if (key == "time") { std::string_view s = value.get_string(); out.time.assign(s); }
                else if (key == "payload") { out.payload.assign(json::trimRaw(value.raw_json())); }
                else if (key == "writer") {
                    simdjson::ondemand::object w = value.get_object();
                    for (auto wf : w) {
                        const std::string_view wk = wf.unescaped_key();
                        auto wv = wf.value();
                        if (wk == "namespace") { std::string_view s = wv.get_string(); out.writer.ns.assign(s); }
                        else if (wk == "writer") { std::string_view s = wv.get_string(); out.writer.writer.assign(s); }
                    }
                    haveWriter = true;
                }
                else {
                    if (!extra) extra.emplace().beginObject();
                    extra->key(key).raw(json::trimRaw(value.raw_json()));
                    anyExtra = true;
                }
            }
        } catch (const simdjson::simdjson_error& e) {
            return std::unexpected(std::format("not a canvas-event-1 record: {}", e.what()));
        }
        if (!haveKind || out.kind.empty()) return std::unexpected("canvas-event-1 record has no kind");
        if (!haveWriter) return std::unexpected("canvas-event-1 record has no writer");
        if (!haveLamport) return std::unexpected("canvas-event-1 record has no lamport");
        if (out.payload.empty()) out.payload = "{}";
        if (anyExtra) {
            extra->endObject();
            const std::string_view whole = extra->view();
            out.extra.assign(whole.substr(1, whole.size() - 2));
        }
        return {};
    }
};

const Journal1Codec kJournal1;
const CanvasEventCodec kCanvasEvent;
const std::array<const RecordCodec*, 2> kCodecs{&kJournal1, &kCanvasEvent};

bool contains(std::span<const std::string_view> set, std::string_view name) { return std::ranges::find(set, name) != set.end(); }

}  // namespace

const RecordCodec* findCodec(std::string_view name) noexcept
{
    for (const RecordCodec* c : kCodecs)
        if (c->shape() == name) return c;
    return nullptr;
}

std::span<const RecordCodec* const> codecs() noexcept { return kCodecs; }

std::expected<std::string_view, std::string> detectShape(std::string_view text)
{
    std::vector<std::string> keys;
    try {
        json::ParseSession session;
        auto doc = session.iterate(text);
        simdjson::ondemand::object obj = doc.get_object();
        for (auto field : obj) {
            std::string_view key = field.unescaped_key();
            keys.emplace_back(key);
        }
    } catch (const simdjson::simdjson_error& e) {
        return std::unexpected(std::format("not a JSON object: {}", e.what()));
    }
    const RecordCodec* found = nullptr;
    for (const RecordCodec* c : kCodecs) {
        const bool all = std::ranges::all_of(c->requiredFields(), [&](std::string_view f) { return std::ranges::find(keys, f) != keys.end(); });
        if (!all) continue;
        if (found != nullptr) return std::unexpected(std::format("ambiguous: both {} and {} match", found->shape(), c->shape()));
        found = c;
    }
    if (found == nullptr) return std::unexpected("no known record shape has all its required fields here");
    return found->shape();
}

bool fieldSetsDisjoint() noexcept
{
    for (std::size_t i = 0; i < kCodecs.size(); ++i)
        for (std::size_t j = i + 1; j < kCodecs.size(); ++j)
            for (std::string_view f : kCodecs[i]->fields())
                if (contains(kCodecs[j]->fields(), f)) return false;
    return true;
}

}  // namespace sub0fabric
