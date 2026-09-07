// package_tests.cpp — the container. Half of this is a round trip; the other
// half hands the reader files a hostile writer might send, because this is the
// one part of the engine that parses somebody else's bytes.

#include "ls_test.h"

#include <livesprite/livesprite.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace ls;

namespace {

struct Doc {
    DocumentId doc;
    SpriteId sprite;
};

Doc makeDoc(LSContext& ctx) {
    Doc built;
    built.doc = ctx.createDocument({"packaged", 32, 32}).value;
    built.sprite = ctx.createSprite(built.doc).value;
    const LayerId layer = ctx.createLayer(built.sprite, {"main"}).value;
    const GeometryId rect = ctx.createRect(built.doc, {{8.f, 8.f}, 10.f, 10.f, 0.f}).value;
    const RegionId region = ctx.createRegionFromGeometry(rect).value;
    FillSolidOp fill;
    fill.targetRegion = region;
    fill.fallbackColor = Color::white();
    ctx.addOperation(layer, fill);
    return built;
}

PackageEntry makeEntry(const std::string& name, const std::string& body,
                       const std::string& type = "application/octet-stream") {
    PackageEntry entry;
    entry.name = name;
    entry.contentType = type;
    entry.data.assign(body.begin(), body.end());
    return entry;
}

std::string bodyOf(const PackageEntry& entry) {
    return std::string(entry.data.begin(), entry.data.end());
}

// --- round trip ------------------------------------------------------------

void testPackageRoundTrip() {
    auto ctx = LSContext::create();
    const Doc built = makeDoc(*ctx);

    std::vector<PackageEntry> entries = {
        makeEntry("fast/thumbnail.png", "not really a png, but bytes are bytes", "image/png"),
        makeEntry("fast/session.json", "{\"zoom\":4}", "application/json"),
        makeEntry("pract/arranger/layout.bin", std::string(1024, '\x7f')),
    };

    auto package = ctx->writePackage(built.doc, entries);
    LS_REQUIRE(package.ok());
    LS_CHECK(package.value.formatTag == "livesprite/package");
    LS_CHECK(package.value.bytes.size() > 100);

    // It is a ZIP, so anything can open it.
    LS_CHECK(package.value.bytes[0] == 'P' && package.value.bytes[1] == 'K');

    // Writing the same state twice gives the same bytes: no timestamps, no
    // ordering drift.
    auto again = ctx->writePackage(built.doc, entries);
    LS_CHECK(again.ok() && again.value.bytes == package.value.bytes);

    auto contents = ctx->readPackage(package.value);
    LS_REQUIRE(contents.ok());
    LS_CHECK(contents.value.entries.size() == 3);
    LS_CHECK(!contents.value.document.bytes.empty());

    // Entries come back in name order, whole.
    LS_CHECK(contents.value.entries[0].name == "fast/session.json");
    LS_CHECK(bodyOf(contents.value.entries[0]) == "{\"zoom\":4}");
    LS_CHECK(contents.value.entries[2].name == "pract/arranger/layout.bin");
    LS_CHECK(contents.value.entries[2].data.size() == 1024);

    // The document inside loads and compiles to the same picture.
    CompileProfile profile;
    profile.type = CompileProfileType::Export;
    profile.outputWidth = 32;
    profile.outputHeight = 32;
    auto before = ctx->compileSprite(built.sprite, profile);
    LS_REQUIRE(before.ok());

    auto loaded = LSContext::create();
    std::vector<PackageEntry> received;
    auto restoredDoc = loaded->loadPackage(package.value, &received);
    LS_REQUIRE(restoredDoc.ok());
    LS_CHECK(received.size() == 3);

    SpriteId restored;
    for (uint64_t candidate = 1; candidate < 4096; ++candidate) {
        if (loaded->getSpriteInfo(SpriteId{candidate}).ok()) {
            restored = SpriteId{candidate};
            break;
        }
    }
    LS_REQUIRE(restored.valid());
    auto after = loaded->compileSprite(restored, profile);
    LS_REQUIRE(after.ok());
    LS_CHECK(after.value.raster.pixels == before.value.raster.pixels);
}

void testForeignEntriesSurvive() {
    auto ctx = LSContext::create();
    const Doc built = makeDoc(*ctx);

    // A package written by another app, carrying entries this one knows nothing
    // about.
    auto original = ctx->writePackage(built.doc, {
        makeEntry("pract/references/pose.png", "someone else's data"),
        makeEntry("someotherapp/notes.txt", "keep me"),
    });
    LS_REQUIRE(original.ok());

    // This app opens it, edits the document, and writes it back, handing the
    // entries through untouched.
    auto reader = LSContext::create();
    std::vector<PackageEntry> carried;
    auto doc = reader->loadPackage(original.value, &carried);
    LS_REQUIRE(doc.ok());
    LS_CHECK(reader->setCanvasSize(doc.value, 48, 48).ok());
    carried.push_back(makeEntry("fast/thumbnail.png", "mine"));

    auto rewritten = reader->writePackage(doc.value, carried);
    LS_REQUIRE(rewritten.ok());

    auto final = ctx->readPackage(rewritten.value);
    LS_REQUIRE(final.ok());
    LS_CHECK(final.value.entries.size() == 3);
    bool foundForeign = false;
    for (const PackageEntry& entry : final.value.entries) {
        if (entry.name == "someotherapp/notes.txt") {
            foundForeign = bodyOf(entry) == "keep me";
        }
    }
    LS_CHECK(foundForeign);
}

// --- names -----------------------------------------------------------------

void testEntryNameRules() {
    // A name is an identifier inside a namespace, never a path out of one.
    LS_CHECK(LSContext::isValidPackageEntryName("fast/thumbnail.png"));
    LS_CHECK(LSContext::isValidPackageEntryName("pract/arranger/cell-3_2.bin"));

    // Traversal, in the shapes an extractor would act on.
    LS_CHECK(!LSContext::isValidPackageEntryName("../secret"));
    LS_CHECK(!LSContext::isValidPackageEntryName("fast/../../etc/passwd"));
    LS_CHECK(!LSContext::isValidPackageEntryName("/etc/passwd"));
    LS_CHECK(!LSContext::isValidPackageEntryName("fast/.."));
    LS_CHECK(!LSContext::isValidPackageEntryName("fast/./x"));

    // Windows shapes: drive letters, UNC paths, backslash separators.
    LS_CHECK(!LSContext::isValidPackageEntryName("C:/Windows/System32/x.dll"));
    LS_CHECK(!LSContext::isValidPackageEntryName("fast\\thumbnail.png"));
    LS_CHECK(!LSContext::isValidPackageEntryName("\\\\server\\share\\x"));

    // Structural nonsense.
    LS_CHECK(!LSContext::isValidPackageEntryName(""));
    LS_CHECK(!LSContext::isValidPackageEntryName("fast//thumbnail.png"));
    LS_CHECK(!LSContext::isValidPackageEntryName("fast/"));
    LS_CHECK(!LSContext::isValidPackageEntryName(std::string(300, 'a') + "/x"));
    LS_CHECK(!LSContext::isValidPackageEntryName(std::string("fast/nul\0byte", 13)));

    // Unnamespaced names are refused: the top level is shared ground.
    LS_CHECK(!LSContext::isValidPackageEntryName("thumbnail.png"));

    // The engine owns its own two names.
    LS_CHECK(!LSContext::isValidPackageEntryName("livesprite.json"));
    LS_CHECK(!LSContext::isValidPackageEntryName("manifest.json"));

    // And the writer refuses them rather than repairing them, so a bad name
    // cannot be rewritten into a good one that collides with something real.
    auto ctx = LSContext::create();
    const Doc built = makeDoc(*ctx);
    LS_CHECK(ctx->writePackage(built.doc, {makeEntry("../escape", "x")}).error ==
             LSError::PackageEntryRejected);
    LS_CHECK(ctx->writePackage(built.doc, {makeEntry("fast/a", "x"), makeEntry("fast/a", "y")})
                 .error == LSError::PackageEntryRejected);
}

// --- limits ----------------------------------------------------------------

void testLimits() {
    auto ctx = LSContext::create();
    const Doc built = makeDoc(*ctx);

    PackageEntry huge;
    huge.name = "fast/huge.bin";
    huge.data.assign(kPackageMaxEntrySize + 1, 0);
    LS_CHECK(ctx->writePackage(built.doc, {huge}).error == LSError::PackageLimitExceeded);

    std::vector<PackageEntry> many;
    for (size_t i = 0; i <= kPackageMaxEntries; ++i) {
        many.push_back(makeEntry("fast/e" + std::to_string(i), "x"));
    }
    LS_CHECK(ctx->writePackage(built.doc, many).error == LSError::PackageLimitExceeded);
}

// --- hostile input ---------------------------------------------------------

void testMalformedPackagesAreRefused() {
    auto ctx = LSContext::create();
    const Doc built = makeDoc(*ctx);
    auto package = ctx->writePackage(built.doc, {makeEntry("fast/a.bin", "hello")});
    LS_REQUIRE(package.ok());

    // Nothing at all.
    SerializedData empty;
    LS_CHECK(ctx->readPackage(empty).fail());

    // Not a package.
    SerializedData garbage;
    garbage.bytes.assign(200, 0x41);
    LS_CHECK(ctx->readPackage(garbage).error == LSError::PackageMalformed);

    // Truncated part way through.
    SerializedData truncated = package.value;
    truncated.bytes.resize(truncated.bytes.size() / 2);
    LS_CHECK(ctx->readPackage(truncated).fail());

    // Truncated to just the tail, so the directory points outside the buffer.
    SerializedData headless = package.value;
    headless.bytes.erase(headless.bytes.begin(),
                         headless.bytes.begin() + static_cast<ptrdiff_t>(headless.bytes.size() / 2));
    LS_CHECK(ctx->readPackage(headless).fail());

    // A flipped byte in the payload: the checksum catches it rather than
    // handing corruption on.
    SerializedData corrupted = package.value;
    for (size_t i = 40; i < corrupted.bytes.size(); ++i) {
        if (corrupted.bytes[i] == 'h') {         // inside "hello"
            corrupted.bytes[i] = 'j';
            break;
        }
    }
    LS_CHECK(ctx->readPackage(corrupted).error == LSError::PackageMalformed);

    // A package with no document in it is not a LiveSprite package, however
    // well formed the ZIP is.
    auto noDocument = package.value;
    const std::string documentName = "livesprite.json";
    for (size_t i = 0; i + documentName.size() < noDocument.bytes.size(); ++i) {
        if (std::equal(documentName.begin(), documentName.end(), noDocument.bytes.begin() + static_cast<ptrdiff_t>(i))) {
            noDocument.bytes[i] = 'x';           // rename it out of the way
        }
    }
    LS_CHECK(ctx->readPackage(noDocument).fail());
}

void testCompressedEntriesAreRefused() {
    auto ctx = LSContext::create();
    const Doc built = makeDoc(*ctx);
    auto package = ctx->writePackage(built.doc, {makeEntry("fast/a.bin", "hello")});
    LS_REQUIRE(package.ok());

    // Claim deflate in the central directory. A reader that inflated whatever
    // it was told to would be one crafted file away from a decompression bomb;
    // this one refuses the whole package.
    SerializedData claimsDeflate = package.value;
    bool patched = false;
    for (size_t i = 0; i + 4 < claimsDeflate.bytes.size() && !patched; ++i) {
        const bool centralHeader = claimsDeflate.bytes[i] == 0x50 &&
                                   claimsDeflate.bytes[i + 1] == 0x4b &&
                                   claimsDeflate.bytes[i + 2] == 0x01 &&
                                   claimsDeflate.bytes[i + 3] == 0x02;
        if (centralHeader && i + 11 < claimsDeflate.bytes.size()) {
            claimsDeflate.bytes[i + 10] = 8;     // method 8: deflate
            patched = true;
        }
    }
    LS_CHECK(patched);
    LS_CHECK(ctx->readPackage(claimsDeflate).error == LSError::PackageMalformed);
}

void testHostileEntryNamesAreRefusedOnRead() {
    auto ctx = LSContext::create();
    const Doc built = makeDoc(*ctx);
    auto package = ctx->writePackage(built.doc, {makeEntry("fast/aaaaaaaaa", "x")});
    LS_REQUIRE(package.ok());

    // Rewrite the stored name to a traversal of the same length, in both the
    // local header and the directory, exactly as a hostile writer would.
    SerializedData attack = package.value;
    const std::string from = "fast/aaaaaaaaa";
    const std::string to   = "../../../etc/x";
    LS_REQUIRE(from.size() == to.size());
    // The name occurs three times: the local header, the manifest listing, and
    // the central directory. The reader takes names from the directory, so only
    // that copy is rewritten; everything else stays valid, which makes this an
    // attack on the name rather than a corrupt file the checksum would catch.
    size_t lastOccurrence = 0;
    bool found = false;
    for (size_t i = 0; i + from.size() <= attack.bytes.size(); ++i) {
        if (std::equal(from.begin(), from.end(), attack.bytes.begin() + static_cast<ptrdiff_t>(i))) {
            lastOccurrence = i;
            found = true;
        }
    }
    LS_REQUIRE(found);
    std::copy(to.begin(), to.end(), attack.bytes.begin() + static_cast<ptrdiff_t>(lastOccurrence));

    // The reader refuses the entry rather than handing an app a name it might
    // write to disk.
    LS_CHECK(ctx->readPackage(attack).error == LSError::PackageEntryRejected);
}

void testDocumentVersionComesFromTheDocument() {
    auto ctx = LSContext::create();
    const Doc built = makeDoc(*ctx);
    auto package = ctx->writePackage(built.doc, {});
    LS_REQUIRE(package.ok());

    // The wrapper is the part an editor is most likely to have rewritten, so a
    // lie there must not decide how the document is read.
    SerializedData lying = package.value;
    lying.engineVersion = LS_ENGINE_VERSION + (1u << 16);
    auto contents = ctx->readPackage(lying);
    LS_REQUIRE(contents.ok());
    LS_CHECK(contents.value.document.engineVersion == LS_ENGINE_VERSION);

    auto loaded = LSContext::create();
    LS_CHECK(loaded->loadPackage(lying).ok());
}

} // namespace

int main() {
    testPackageRoundTrip();
    testForeignEntriesSurvive();
    testEntryNameRules();
    testLimits();
    testMalformedPackagesAreRefused();
    testCompressedEntriesAreRefused();
    testHostileEntryNamesAreRefusedOnRead();
    testDocumentVersionComesFromTheDocument();
    return lstest::report("package");
}
