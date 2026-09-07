// ls_package.cpp — the LiveSprite container.
//
// A package is a ZIP holding the engine document plus whatever entries an app
// keeps beside it. Two decisions shape everything here.
//
// Portability: plain ZIP with stored entries and no extra fields, so any tool
// and any language can open it, and the bytes are the same on every platform.
// Nothing here reads or writes a struct directly: every field is assembled from
// bytes in little-endian order by hand, so alignment and endianness cannot
// change the file.
//
// Safety: this is the only code in the engine that parses a file somebody else
// wrote, so it assumes the file is hostile. Nothing is compressed, so a
// decompression bomb has nowhere to live; sizes are checked against limits
// before anything is allocated; every offset is bounds checked against the
// buffer rather than trusted; every entry name is validated rather than
// sanitised, so a name that tries to climb out of its namespace is refused
// instead of quietly rewritten into something else.

#include "ls_internal.h"
#include "ls_json.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace ls {
namespace {

constexpr const char* kDocumentEntryName = "livesprite.json";
constexpr const char* kManifestEntryName = "manifest.json";

constexpr uint32_t kLocalHeaderSignature   = 0x04034b50u;
constexpr uint32_t kCentralHeaderSignature = 0x02014b50u;
constexpr uint32_t kEndOfDirectorySignature = 0x06054b50u;
constexpr size_t   kLocalHeaderSize   = 30;
constexpr size_t   kCentralHeaderSize = 46;
constexpr size_t   kEndOfDirectorySize = 22;

// --- checksums -------------------------------------------------------------

uint32_t crc32Of(const uint8_t* data, size_t length) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int bit = 0; bit < 8; ++bit) {
                c = (c & 1u) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        ready = true;
    }
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < length; ++i) {
        crc = table[(crc ^ data[i]) & 0xffu] ^ (crc >> 8);
    }
    return crc ^ 0xffffffffu;
}

// --- little-endian field access, written out rather than cast --------------

void push16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
}

void push32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xffu));
}

// Every read is bounds checked: a truncated or lying package must fail rather
// than walk off the end of the buffer.
bool read16(const std::vector<uint8_t>& data, size_t offset, uint16_t& out) {
    if (offset + 2 > data.size()) {
        return false;
    }
    out = static_cast<uint16_t>(data[offset]) |
          static_cast<uint16_t>(static_cast<uint16_t>(data[offset + 1]) << 8);
    return true;
}

bool read32(const std::vector<uint8_t>& data, size_t offset, uint32_t& out) {
    if (offset + 4 > data.size()) {
        return false;
    }
    out = static_cast<uint32_t>(data[offset]) |
          (static_cast<uint32_t>(data[offset + 1]) << 8) |
          (static_cast<uint32_t>(data[offset + 2]) << 16) |
          (static_cast<uint32_t>(data[offset + 3]) << 24);
    return true;
}

// --- name rules ------------------------------------------------------------

bool isNameCharacter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

} // namespace

bool LSContext::isValidPackageEntryName(std::string_view name) {
    if (name.empty() || name.size() > kPackageMaxNameLength) {
        return false;
    }
    // The engine owns these two names inside every package.
    if (name == kDocumentEntryName || name == kManifestEntryName) {
        return false;
    }
    // A name is an identifier, not a path: nothing that an extractor could read
    // as absolute, as a drive, or as a step upwards.
    if (name.front() == '/' || name.back() == '/') {
        return false;
    }
    if (name.find('\\') != std::string_view::npos ||
        name.find(':') != std::string_view::npos ||
        name.find('\0') != std::string_view::npos) {
        return false;
    }

    size_t segments = 0;
    size_t start = 0;
    while (start <= name.size()) {
        const size_t slash = name.find('/', start);
        const std::string_view segment = slash == std::string_view::npos
            ? name.substr(start)
            : name.substr(start, slash - start);
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        for (char c : segment) {
            if (!isNameCharacter(c)) {
                return false;
            }
        }
        ++segments;
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }

    // The first segment names the owner, so two apps cannot collide.
    return segments >= 2;
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

Result<SerializedData> LSContext::writePackage(DocumentId doc,
                                               const std::vector<PackageEntry>& entries) const {
    auto document = serializeDocument(doc);
    if (document.fail()) {
        return Result<SerializedData>::err(document.error);
    }

    if (entries.size() > kPackageMaxEntries) {
        return Result<SerializedData>::err(LSError::PackageLimitExceeded);
    }

    // Ordered by name, so a package written twice from the same state is byte
    // identical, like everything else the engine emits.
    std::map<std::string, const PackageEntry*> ordered;
    size_t total = document.value.bytes.size();
    for (const PackageEntry& entry : entries) {
        if (!isValidPackageEntryName(entry.name)) {
            return Result<SerializedData>::err(LSError::PackageEntryRejected);
        }
        if (entry.data.size() > kPackageMaxEntrySize) {
            return Result<SerializedData>::err(LSError::PackageLimitExceeded);
        }
        if (!ordered.emplace(entry.name, &entry).second) {
            return Result<SerializedData>::err(LSError::PackageEntryRejected);   // duplicate
        }
        total += entry.data.size();
        if (total > kPackageMaxTotalSize) {
            return Result<SerializedData>::err(LSError::PackageLimitExceeded);
        }
    }

    // The manifest records what the writer claimed about each entry. It carries
    // no authority: a reader hands these claims back untouched.
    std::string manifest = "{\"format\":\"livesprite/package\",\"engineVersion\":" +
                           std::to_string(LS_ENGINE_VERSION) + ",\"entries\":[";
    bool first = true;
    for (const auto& [name, entry] : ordered) {
        if (!first) {
            manifest += ",";
        }
        first = false;
        manifest += "{\"name\":\"" + name + "\",\"contentType\":\"";
        for (char c : entry->contentType) {
            // The claim is text in a JSON string, so it is escaped rather than
            // trusted to be well behaved.
            if (c == '"' || c == '\\') {
                manifest += '\\';
            }
            if (static_cast<unsigned char>(c) >= 0x20) {
                manifest += c;
            }
        }
        manifest += "\",\"size\":" + std::to_string(entry->data.size()) + "}";
    }
    manifest += "]}";

    struct Pending {
        std::string name;
        const std::vector<uint8_t>* data;
    };
    const std::vector<uint8_t> manifestBytes(manifest.begin(), manifest.end());

    std::vector<Pending> plan;
    plan.push_back({ kDocumentEntryName, &document.value.bytes });
    plan.push_back({ kManifestEntryName, &manifestBytes });
    for (const auto& [name, entry] : ordered) {
        plan.push_back({ name, &entry->data });
    }

    struct Placed {
        std::string name;
        uint32_t crc = 0;
        uint32_t size = 0;
        uint32_t offset = 0;
    };

    std::vector<uint8_t> out;
    std::vector<Placed> placed;
    for (const Pending& item : plan) {
        Placed record;
        record.name = item.name;
        record.crc = crc32Of(item.data->data(), item.data->size());
        record.size = static_cast<uint32_t>(item.data->size());
        record.offset = static_cast<uint32_t>(out.size());

        push32(out, kLocalHeaderSignature);
        push16(out, 20);      // version needed: 2.0, stored
        push16(out, 0);       // no flags: no encryption, no data descriptor
        push16(out, 0);       // method 0: stored, so there is nothing to inflate
        push16(out, 0);       // time and date fixed, so output is deterministic
        push16(out, 0);
        push32(out, record.crc);
        push32(out, record.size);
        push32(out, record.size);
        push16(out, static_cast<uint16_t>(record.name.size()));
        push16(out, 0);       // no extra field
        out.insert(out.end(), record.name.begin(), record.name.end());
        out.insert(out.end(), item.data->begin(), item.data->end());
        placed.push_back(std::move(record));
    }

    const uint32_t directoryOffset = static_cast<uint32_t>(out.size());
    for (const Placed& record : placed) {
        push32(out, kCentralHeaderSignature);
        push16(out, 20);      // version made by
        push16(out, 20);      // version needed
        push16(out, 0);
        push16(out, 0);
        push16(out, 0);
        push16(out, 0);
        push32(out, record.crc);
        push32(out, record.size);
        push32(out, record.size);
        push16(out, static_cast<uint16_t>(record.name.size()));
        push16(out, 0);       // extra
        push16(out, 0);       // comment
        push16(out, 0);       // disk number
        push16(out, 0);       // internal attributes
        push32(out, 0);       // external attributes: no permissions carried
        push32(out, record.offset);
        out.insert(out.end(), record.name.begin(), record.name.end());
    }
    const uint32_t directorySize = static_cast<uint32_t>(out.size()) - directoryOffset;

    push32(out, kEndOfDirectorySignature);
    push16(out, 0);
    push16(out, 0);
    push16(out, static_cast<uint16_t>(placed.size()));
    push16(out, static_cast<uint16_t>(placed.size()));
    push32(out, directorySize);
    push32(out, directoryOffset);
    push16(out, 0);           // no archive comment

    SerializedData package;
    package.bytes = std::move(out);
    package.engineVersion = LS_ENGINE_VERSION;
    package.formatTag = "livesprite/package";
    return Result<SerializedData>::ok(std::move(package));
}

// ---------------------------------------------------------------------------
// Reading. Everything below treats the buffer as hostile.
// ---------------------------------------------------------------------------

Result<PackageContents> LSContext::readPackage(const SerializedData& package) const {
    const std::vector<uint8_t>& data = package.bytes;
    if (data.size() < kEndOfDirectorySize || data.size() > kPackageMaxTotalSize) {
        return Result<PackageContents>::err(LSError::PackageMalformed);
    }

    // Find the end of central directory by scanning back from the end. The
    // comment field can be up to 64 KB, so the search is bounded by that rather
    // than running over the whole file.
    size_t endOffset = 0;
    bool foundEnd = false;
    const size_t searchLimit = std::min<size_t>(data.size(), kEndOfDirectorySize + 65535);
    for (size_t back = kEndOfDirectorySize; back <= searchLimit; ++back) {
        const size_t candidate = data.size() - back;
        uint32_t signature = 0;
        if (read32(data, candidate, signature) && signature == kEndOfDirectorySignature) {
            endOffset = candidate;
            foundEnd = true;
            break;
        }
    }
    if (!foundEnd) {
        return Result<PackageContents>::err(LSError::PackageMalformed);
    }

    uint16_t entryCount = 0;
    uint32_t directorySize = 0;
    uint32_t directoryOffset = 0;
    if (!read16(data, endOffset + 10, entryCount) ||
        !read32(data, endOffset + 12, directorySize) ||
        !read32(data, endOffset + 16, directoryOffset)) {
        return Result<PackageContents>::err(LSError::PackageMalformed);
    }
    if (entryCount > kPackageMaxEntries + 2) {
        return Result<PackageContents>::err(LSError::PackageLimitExceeded);
    }
    if (static_cast<size_t>(directoryOffset) + directorySize > data.size()) {
        return Result<PackageContents>::err(LSError::PackageMalformed);
    }

    PackageContents contents;
    bool foundDocument = false;
    size_t totalBytes = 0;
    size_t cursor = directoryOffset;

    for (uint16_t i = 0; i < entryCount; ++i) {
        if (cursor + kCentralHeaderSize > data.size()) {
            return Result<PackageContents>::err(LSError::PackageMalformed);
        }
        uint32_t signature = 0;
        uint16_t method = 0;
        uint32_t crc = 0;
        uint32_t compressedSize = 0;
        uint32_t uncompressedSize = 0;
        uint16_t nameLength = 0;
        uint16_t extraLength = 0;
        uint16_t commentLength = 0;
        uint32_t localOffset = 0;
        if (!read32(data, cursor, signature) || signature != kCentralHeaderSignature ||
            !read16(data, cursor + 10, method) ||
            !read32(data, cursor + 16, crc) ||
            !read32(data, cursor + 20, compressedSize) ||
            !read32(data, cursor + 24, uncompressedSize) ||
            !read16(data, cursor + 28, nameLength) ||
            !read16(data, cursor + 30, extraLength) ||
            !read16(data, cursor + 32, commentLength) ||
            !read32(data, cursor + 42, localOffset)) {
            return Result<PackageContents>::err(LSError::PackageMalformed);
        }

        // Stored only. Refusing compression is what makes a decompression bomb
        // impossible rather than merely bounded.
        if (method != 0 || compressedSize != uncompressedSize) {
            return Result<PackageContents>::err(LSError::PackageMalformed);
        }
        if (uncompressedSize > kPackageMaxEntrySize) {
            return Result<PackageContents>::err(LSError::PackageLimitExceeded);
        }
        totalBytes += uncompressedSize;
        if (totalBytes > kPackageMaxTotalSize) {
            return Result<PackageContents>::err(LSError::PackageLimitExceeded);
        }

        if (cursor + kCentralHeaderSize + nameLength > data.size()) {
            return Result<PackageContents>::err(LSError::PackageMalformed);
        }
        const std::string name(reinterpret_cast<const char*>(data.data() + cursor + kCentralHeaderSize),
                               nameLength);
        cursor += kCentralHeaderSize + nameLength + extraLength + commentLength;

        // The local header is checked too: the central directory is a claim
        // about where the data is, and the two have to agree.
        uint32_t localSignature = 0;
        uint16_t localNameLength = 0;
        uint16_t localExtraLength = 0;
        if (!read32(data, localOffset, localSignature) ||
            localSignature != kLocalHeaderSignature ||
            !read16(data, localOffset + 26, localNameLength) ||
            !read16(data, localOffset + 28, localExtraLength)) {
            return Result<PackageContents>::err(LSError::PackageMalformed);
        }
        const size_t payload = static_cast<size_t>(localOffset) + kLocalHeaderSize +
                               localNameLength + localExtraLength;
        if (payload + uncompressedSize > data.size()) {
            return Result<PackageContents>::err(LSError::PackageMalformed);
        }

        std::vector<uint8_t> bytes(data.begin() + static_cast<ptrdiff_t>(payload),
                                   data.begin() + static_cast<ptrdiff_t>(payload + uncompressedSize));
        if (crc32Of(bytes.data(), bytes.size()) != crc) {
            // Corruption stops here rather than being handed on.
            return Result<PackageContents>::err(LSError::PackageMalformed);
        }

        if (name == kDocumentEntryName) {
            contents.document.bytes = std::move(bytes);
            contents.document.formatTag = "livesprite/document";
            // The version comes from inside the document, not from the wrapper
            // around it: the wrapper is the part an editor is most likely to
            // have rewritten.
            json::Value probe;
            contents.document.engineVersion = LS_ENGINE_VERSION;
            if (json::parse(std::string(contents.document.bytes.begin(),
                                        contents.document.bytes.end()), probe)) {
                if (const json::Value* version = probe.find("engineVersion")) {
                    contents.document.engineVersion =
                        static_cast<uint32_t>(version->asNumber(LS_ENGINE_VERSION));
                }
            }
            foundDocument = true;
            continue;
        }
        if (name == kManifestEntryName) {
            continue;   // read for its claims below, never for authority
        }

        // An entry whose name is not acceptable is refused, not repaired: a
        // sanitised name could collide with a legitimate one.
        if (!isValidPackageEntryName(name)) {
            return Result<PackageContents>::err(LSError::PackageEntryRejected);
        }
        for (const PackageEntry& existing : contents.entries) {
            if (existing.name == name) {
                return Result<PackageContents>::err(LSError::PackageEntryRejected);
            }
        }

        PackageEntry entry;
        entry.name = name;
        entry.data = std::move(bytes);
        contents.entries.push_back(std::move(entry));
    }

    if (!foundDocument) {
        return Result<PackageContents>::err(LSError::PackageMalformed);
    }

    std::sort(contents.entries.begin(), contents.entries.end(),
              [](const PackageEntry& a, const PackageEntry& b) { return a.name < b.name; });
    return Result<PackageContents>::ok(std::move(contents));
}

Result<DocumentId> LSContext::loadPackage(const SerializedData& package,
                                          std::vector<PackageEntry>* entries) {
    auto contents = readPackage(package);
    if (contents.fail()) {
        return Result<DocumentId>::err(contents.error);
    }
    auto document = deserializeDocument(contents.value.document);
    if (document.fail()) {
        return Result<DocumentId>::err(document.error);
    }
    if (entries != nullptr) {
        *entries = std::move(contents.value.entries);
    }
    return document;
}

} // namespace ls
