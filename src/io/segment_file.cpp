#include "segment_file.hpp"

#include "sub0fabric/pure/layout.hpp"

#include <algorithm>
#include <fstream>

namespace sub0fabric::io::detail {

namespace fs = std::filesystem;

std::expected<FileScan, std::string> scanFile(const fs::path& path, Framing framing, const RecordCodec& codec, std::uint64_t firstPosition, std::uint64_t startOffset,
                                              SparseIndex* index, std::size_t chunkBytes)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::unexpected("cannot read " + path.string());
    in.seekg(0, std::ios::end);
    FileScan out;
    out.fileSize = static_cast<std::uint64_t>(in.tellg());
    in.seekg(static_cast<std::streamoff>(startOffset), std::ios::beg);
    SegmentScanner scanner(framing, codec, firstPosition, startOffset);
    std::string chunk(chunkBytes, '\0');
    bool atEof = false;
    for (;;) {
        if (index != nullptr) index->note(scanner.position(), scanner.offset());
        const Step step = scanner.next(nullptr);
        if (step == Step::ok) continue;
        if (step == Step::needMore) {
            if (atEof) {
                scanner.finish();
                continue;
            }
            in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            const auto got = static_cast<std::size_t>(in.gcount());
            if (got == 0) atEof = true;   // a short read ends the file; the next needMore calls finish()
            else {
                scanner.feed(std::string_view{chunk.data(), got});
                out.bytesRead += got;
            }
            continue;
        }
        out.torn = step == Step::torn;
        if (step == Step::corrupt) out.corrupt = scanner.error();
        break;
    }
    out.validOffset = scanner.validOffset();
    out.position = scanner.position();
    out.records = scanner.position() - firstPosition;
    return out;
}

std::expected<FileHeader, std::string> readHeader(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::unexpected("cannot read " + path.string());
    in.seekg(0, std::ios::end);
    FileHeader h;
    h.fileSize = static_cast<std::uint64_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    std::string head(std::min<std::uint64_t>(h.fileSize, (64u << 10) + 32), '\0');
    in.read(head.data(), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(in.gcount()));
    h.parse = parseHeader(head, true);
    return h;
}

std::expected<std::string, std::string> readFirstLine(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::unexpected("cannot read " + path.string());
    std::string head(64u << 10, '\0');
    in.read(head.data(), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(in.gcount()));
    const auto nl = head.find('\n');
    if (nl == std::string::npos) return std::string{};
    head.resize(nl);
    return head;
}

std::vector<std::pair<std::uint64_t, fs::path>> listSegments(const fs::path& dir)
{
    std::vector<std::pair<std::uint64_t, fs::path>> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (const auto n = parseSegmentFileName(it->path().filename().string())) out.emplace_back(*n, it->path());
    }
    std::ranges::sort(out);
    return out;
}

}  // namespace sub0fabric::io::detail
