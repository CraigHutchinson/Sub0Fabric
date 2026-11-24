#include "sub0fabric/io/store.hpp"

#include "segment_file.hpp"

#include "sub0fabric/pure/layout.hpp"

#include <algorithm>
#include <format>
#include <fstream>

namespace sub0fabric::io {

namespace fs = std::filesystem;

namespace {

/// The writer of a `lines` stream that sits at the store root: a journal-1 directory names no writer.
const WriterId kSharedWriter{"", "journal"};

struct SegInfo
{
    fs::path path;
    std::uint64_t number = 0;
    std::uint64_t first = 0;         ///< position of the first record
    std::uint64_t headerEnd = 0;     ///< offset of the first record (0 for lines)
    std::uint64_t scannedOffset = 0; ///< just after the last good record found
    std::uint64_t records = 0;
    std::uint64_t fileSize = 0;      ///< the file's size at the last scan: an unchanged file is not scanned again
    bool torn = false;
    std::string damage;
    SparseIndex index;
};

struct Stream
{
    WriterId writer;
    const RecordCodec* codec = nullptr;
    std::string order;
    Framing framing = Framing::frames;
    fs::path dir;
    std::vector<SegInfo> segments;
};

/// One segment of a stream, as it was when a read began: the read depends on nothing the reader may change afterwards.
struct Plan
{
    fs::path path;
    std::uint64_t startPosition = 0;   ///< the index entry the read starts from
    std::uint64_t startOffset = 0;
    std::uint64_t wantFrom = 0;        ///< the first position to return
    std::uint64_t end = 0;             ///< one past the last position known when the read began
    std::string damage;
};

/// One stream's records from a position, in stream order, across segments, holding one chunk.
class StreamSource final : public RecordSource
{
public:
    StreamSource(std::vector<Plan> plans, Framing framing, const RecordCodec& codec, WriterId writer, std::size_t chunk, std::shared_ptr<ReadStats> stats)
        : plans_(std::move(plans)), codec_(&codec), writer_(std::move(writer)), scanner_(framing, codec, 0, 0), chunk_(chunk, '\0'), stats_(std::move(stats))
    {
    }

    std::expected<bool, std::string> next(Record& out) override
    {
        for (;;) {
            if (!open_) {
                if (current_ >= plans_.size()) return false;
                const Plan& plan = plans_[current_];
                in_.close();
                in_.clear();
                in_.open(plan.path, std::ios::binary);
                if (!in_) return std::unexpected("cannot read " + plan.path.string());
                in_.seekg(static_cast<std::streamoff>(plan.startOffset), std::ios::beg);
                scanner_.reset(plan.startPosition, plan.startOffset);
                atEof_ = false;
                open_ = true;
            }
            const Plan& plan = plans_[current_];
            if (scanner_.position() >= plan.end) {
                if (!plan.damage.empty()) return std::unexpected(std::format("{} in {}", plan.damage, plan.path.string()));
                open_ = false;
                ++current_;
                continue;
            }
            const bool skipping = scanner_.position() < plan.wantFrom;
            const Step step = scanner_.next(skipping ? nullptr : &out);
            switch (step) {
            case Step::ok:
                if (skipping) { ++stats_->recordsSkipped; continue; }
                if (!codec_->carriesWriter()) out.writer = writer_;
                ++stats_->recordsReturned;
                return true;
            case Step::needMore: {
                if (atEof_) { scanner_.finish(); continue; }
                in_.read(chunk_.data(), static_cast<std::streamsize>(chunk_.size()));
                const auto got = static_cast<std::size_t>(in_.gcount());
                if (got == 0) atEof_ = true;
                else {
                    scanner_.feed(std::string_view{chunk_.data(), got});
                    stats_->bytesRead += got;
                }
                continue;
            }
            case Step::corrupt: return std::unexpected(std::format("{} in {}", scanner_.error(), plan.path.string()));
            case Step::end:
            case Step::torn: return std::unexpected(std::format("{} ends before the {} records found when it was scanned", plan.path.string(), plan.end - plan.startPosition));
            }
        }
    }

private:
    std::vector<Plan> plans_;
    const RecordCodec* codec_;
    WriterId writer_;
    SegmentScanner scanner_;
    std::string chunk_;
    std::shared_ptr<ReadStats> stats_;
    std::ifstream in_;
    std::size_t current_ = 0;
    bool open_ = false;
    bool atEof_ = false;
};

}  // namespace

struct StoreReader::Impl
{
    fs::path root;
    ReaderOptions options;
    std::map<WriterId, Stream> streams;
    std::map<fs::path, WriterId> dirs;   ///< directories already known to hold a stream
    std::shared_ptr<ReadStats> stats;

    /// Scan `seg` from where the last scan stopped, adding to its index and count.
    std::expected<void, std::string> extend(const Stream& stream, SegInfo& seg)
    {
        std::error_code ec;
        const std::uint64_t size = fs::file_size(seg.path, ec);
        if (ec) return std::unexpected("cannot stat " + seg.path.string() + ": " + ec.message());
        if (size == seg.fileSize || !seg.damage.empty()) return {};
        auto scan = detail::scanFile(seg.path, stream.framing, *stream.codec, seg.first + seg.records, seg.scannedOffset, &seg.index, options.chunkBytes);
        if (!scan) return std::unexpected(scan.error());
        stats->bytesRead += scan->bytesRead;
        stats->recordsScanned += scan->records;
        seg.records += scan->records;
        seg.scannedOffset = scan->validOffset;
        seg.fileSize = size;
        seg.torn = scan->torn;
        seg.damage = scan->corrupt;
        return {};
    }

    /// Learn the segments of the stream in `dir`, given which framing it is.
    std::expected<void, std::string> refreshFrames(const fs::path& dir)
    {
        const auto files = detail::listSegments(dir);
        if (files.empty()) return {};
        Stream* stream = nullptr;
        if (const auto d = dirs.find(dir); d != dirs.end()) stream = &streams.at(d->second);   // std::map: the pointer stays valid
        for (const auto& [number, path] : files) {
            if (stream != nullptr) {
                const auto it = std::ranges::find(stream->segments, number, &SegInfo::number);
                if (it != stream->segments.end()) {
                    if (auto ok = extend(*stream, *it); !ok) return ok;
                    continue;
                }
            }
            auto head = detail::readHeader(path);
            if (!head) return std::unexpected(head.error());
            if (head->parse.step == Step::torn) continue;   // a writer is creating it: seen at the next refresh
            if (head->parse.step != Step::ok) return std::unexpected(std::format("{}: {}", path.string(), head->parse.error));
            const SegmentHeader& h = head->parse.header;
            const RecordCodec* codec = findCodec(h.shape);
            if (codec == nullptr) return std::unexpected(std::format("{}: unknown record shape '{}'", path.string(), h.shape));
            if (stream == nullptr) {
                auto it = streams.find(h.writer);
                if (it == streams.end()) {
                    Stream fresh;
                    fresh.writer = h.writer;
                    fresh.codec = codec;
                    fresh.order = h.order;
                    fresh.framing = Framing::frames;
                    fresh.dir = dir;
                    it = streams.emplace(h.writer, std::move(fresh)).first;
                    dirs[dir] = h.writer;
                }
                stream = &it->second;
            }
            if (h.writer != stream->writer || h.shape != stream->codec->shape())
                return std::unexpected(std::format("{}: header disagrees with the stream's earlier segments", path.string()));
            SegInfo seg;
            seg.path = path;
            seg.number = number;
            seg.first = h.first;
            seg.headerEnd = seg.scannedOffset = head->parse.size;
            seg.index = SparseIndex(options.indexEvery);
            stream->segments.push_back(std::move(seg));
            if (auto ok = extend(*stream, stream->segments.back()); !ok) return ok;
        }
        if (stream != nullptr) std::ranges::sort(stream->segments, {}, &SegInfo::number);   // a segment found late is placed by its number
        return {};
    }

    std::expected<void, std::string> refreshLines(const fs::path& dir, const WriterId& writer)
    {
        const fs::path path = dir / std::string{kJournalFileName};
        std::error_code ec;
        if (!fs::exists(path, ec)) return {};
        auto it = streams.find(writer);
        if (it == streams.end()) {
            auto line = detail::readFirstLine(path);
            if (!line) return std::unexpected(line.error());
            if (line->empty()) return {};   // nothing readable yet
            auto shape = detectShape(*line);
            if (!shape) return std::unexpected(std::format("{}: {}", path.string(), shape.error()));
            Stream s;
            s.writer = writer;
            s.codec = findCodec(*shape);
            s.order = std::string{s.codec->defaultOrder()};
            s.framing = Framing::lines;
            s.dir = dir;
            SegInfo seg;
            seg.path = path;
            seg.number = 1;
            seg.index = SparseIndex(options.indexEvery);
            s.segments.push_back(std::move(seg));
            it = streams.emplace(writer, std::move(s)).first;
            dirs[dir] = writer;
        }
        return extend(it->second, it->second.segments.front());
    }

    std::expected<void, std::string> refresh()
    {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) return {};
        // The root itself (Layout::shared), then <ns>/<writer> (Layout::perWriter).
        if (auto ok = refreshFrames(root); !ok) return ok;
        if (auto ok = refreshLines(root, kSharedWriter); !ok) return ok;
        for (fs::directory_iterator nsIt(root, ec), end; !ec && nsIt != end; nsIt.increment(ec)) {
            if (!nsIt->is_directory()) continue;
            auto ns = decodeName(nsIt->path().filename().string());
            if (!ns) continue;
            std::error_code ec2;
            for (fs::directory_iterator wIt(nsIt->path(), ec2), end2; !ec2 && wIt != end2; wIt.increment(ec2)) {
                if (!wIt->is_directory()) continue;
                auto writer = decodeName(wIt->path().filename().string());
                if (!writer) continue;
                if (auto ok = refreshFrames(wIt->path()); !ok) return ok;
                if (auto ok = refreshLines(wIt->path(), WriterId{*ns, *writer}); !ok) return ok;
            }
        }
        return {};
    }
};

StoreReader::StoreReader(std::unique_ptr<Impl> impl, std::shared_ptr<ReadStats> stats) : impl_(std::move(impl)), stats_(std::move(stats)) {}
StoreReader::StoreReader(StoreReader&&) noexcept = default;
StoreReader& StoreReader::operator=(StoreReader&&) noexcept = default;
StoreReader::~StoreReader() = default;

std::expected<StoreReader, std::string> StoreReader::open(const fs::path& root, ReaderOptions options)
{
    auto stats = std::make_shared<ReadStats>();
    auto impl = std::make_unique<Impl>();
    impl->root = root;
    impl->options = options;
    impl->stats = stats;
    if (auto ok = impl->refresh(); !ok) return std::unexpected(ok.error());
    return StoreReader(std::move(impl), std::move(stats));
}

std::expected<void, std::string> StoreReader::refresh() { return impl_->refresh(); }

std::vector<StreamInfo> StoreReader::streams() const
{
    std::vector<StreamInfo> out;
    for (const auto& [writer, s] : impl_->streams) {
        StreamInfo info;
        info.writer = writer;
        info.shape = std::string{s.codec->shape()};
        info.order = s.order;
        info.framing = s.framing;
        info.segments = s.segments.size();
        for (const SegInfo& seg : s.segments) {
            info.records += seg.records;
            info.bytes += seg.fileSize;
            info.tornTail = seg.torn;
            if (!seg.damage.empty() && info.damage.empty()) info.damage = std::format("{} in {}", seg.damage, seg.path.string());
        }
        out.push_back(std::move(info));
    }
    return out;
}

std::uint64_t StoreReader::count(const WriterId& writer) const
{
    const auto it = impl_->streams.find(writer);
    if (it == impl_->streams.end()) return 0;
    std::uint64_t n = 0;
    for (const SegInfo& seg : it->second.segments) n += seg.records;
    return n;
}

std::expected<std::unique_ptr<Merger>, std::string> StoreReader::since(const Cursor& cursor, std::string_view orderOverride) const
{
    std::string order{orderOverride};
    for (const auto& [writer, s] : impl_->streams) {
        if (order.empty()) order = s.order;
        else if (orderOverride.empty() && s.order != order)
            return std::unexpected(std::format("streams are read in different orders ('{}' and '{}'): name one", order, s.order));
    }
    const OrderPolicy* policy = findOrderPolicy(order.empty() ? std::string_view{"lamport"} : std::string_view{order});
    if (policy == nullptr) return std::unexpected(std::format("unknown order policy '{}'", order));

    std::vector<std::unique_ptr<RecordSource>> sources;
    for (const auto& [writer, s] : impl_->streams) {
        const std::uint64_t from = cursor.position(writer);   // records consumed = the 0-based position of the next one
        std::vector<Plan> plans;
        for (const SegInfo& seg : s.segments) {
            const std::uint64_t end = seg.first + seg.records;
            if (end <= from && seg.damage.empty()) continue;   // wholly consumed
            if (seg.records == 0 && seg.damage.empty()) continue;
            Plan plan;
            plan.path = seg.path;
            plan.end = end;
            plan.damage = seg.damage;
            plan.wantFrom = std::max(from, seg.first);
            if (from > seg.first && seg.records > 0) {
                const SparseIndex::Entry e = seg.index.lookup(from);
                plan.startPosition = e.position;
                plan.startOffset = e.offset;
            } else {
                plan.startPosition = seg.first;
                plan.startOffset = seg.headerEnd;
            }
            plans.push_back(std::move(plan));
        }
        if (plans.empty()) continue;
        sources.push_back(std::make_unique<StreamSource>(std::move(plans), s.framing, *s.codec, writer, impl_->options.chunkBytes, stats_));
    }
    return std::make_unique<Merger>(std::move(sources), *policy);
}

}  // namespace sub0fabric::io
