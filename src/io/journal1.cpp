#include "sub0fabric/io/journal1.hpp"

#include "sub0fabric/io/store.hpp"
#include "sub0fabric/pure/layout.hpp"

#include <chrono>
#include <fstream>

namespace sub0fabric::io {

namespace fs = std::filesystem;

namespace {

double wallNow()
{
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

WriterOptions journalOptions()
{
    WriterOptions o;
    o.shape = "journal-1";
    o.framing = Framing::lines;
    o.layout = Layout::shared;
    o.lock = false;
    return o;
}

}  // namespace

Journal::Journal() = default;
Journal::Journal(Journal&&) noexcept = default;
Journal& Journal::operator=(Journal&&) noexcept = default;
Journal::~Journal() = default;

std::expected<JournalReplay, std::string> Journal::replay(const fs::path& dir)
{
    JournalReplay out;
    const fs::path path = dir / std::string{kJournalFileName};
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::error_code ec;
        if (!fs::exists(path, ec)) return out;  // no journal yet
        return std::unexpected("cannot read " + path.string());
    }
    const RecordCodec& codec = *findCodec("journal-1");
    SegmentScanner scanner(Framing::lines, codec, 0, 0);
    std::string chunk(64u << 10, '\0');
    bool atEof = false;
    for (;;) {
        Record r;
        const Step step = scanner.next(&r);
        if (step == Step::ok) {
            out.records.push_back(JournalRecord{r.seq, r.ts, std::move(r.kind), std::move(r.payload)});
            continue;
        }
        if (step == Step::needMore) {
            if (atEof) { scanner.finish(); continue; }
            in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            const auto got = static_cast<std::size_t>(in.gcount());
            if (got == 0) atEof = true;
            else scanner.feed(std::string_view{chunk.data(), got});
            continue;
        }
        if (step == Step::corrupt)   // the scanner's offset of the last good record is where the bad line starts
            return std::unexpected("journal corrupt before its end: line starting at byte " + std::to_string(scanner.validOffset()) + " in " + path.string());
        out.tornTail = step == Step::torn;
        break;
    }
    out.validBytes = scanner.validOffset();
    return out;
}

std::expected<Journal, std::string> Journal::open(const fs::path& dir)
{
    auto replayed = replay(dir);
    if (!replayed) return std::unexpected(replayed.error());
    auto writer = StoreWriter::open(dir, WriterId{"", "journal"}, journalOptions());
    if (!writer) return std::unexpected(writer.error());
    Journal j;
    j.writer_ = std::make_unique<StoreWriter>(std::move(*writer));
    j.records_ = std::move(replayed->records);
    j.nextSeq_ = j.writer_->nextSeq();
    return j;
}

std::expected<std::uint64_t, std::string> Journal::append(std::string_view type, std::string_view bodyJson, bool durable)
{
    if (!writer_) return std::unexpected("journal not open");
    Record r;
    r.kind = std::string{type};
    r.payload = std::string{bodyJson};
    r.ts = wallNow();
    auto seq = writer_->append(r);
    if (!seq) return std::unexpected("journal write failed: " + seq.error());
    if (durable)
        if (auto s = writer_->sync(); !s) return std::unexpected("journal fdatasync failed: " + s.error());
    records_.push_back(JournalRecord{*seq, r.ts, std::move(r.kind), std::move(r.payload)});
    nextSeq_ = *seq + 1;
    return *seq;
}

}  // namespace sub0fabric::io
