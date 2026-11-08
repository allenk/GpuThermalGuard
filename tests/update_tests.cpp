// Tests for the updater. Built only with GTG_FEATURE_UPDATE, which the public
// tree does not turn on, so they live apart from test_main.cpp.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <aclapi.h>
#include <sddl.h>

#include "peer/link.hpp"
#include "peer/pairing_code.hpp"
#include "update/decision.hpp"
#include "update/digest.hpp"
#include "update/files.hpp"
#include "update/handoff.hpp"
#include "update/hosts_trigger.hpp"
#include "update/hosts_updates.hpp"
#include "update/hosts_wire.hpp"
#include "update/http.hpp"
#include "update/plan.hpp"
#include "update/json.hpp"
#include "update/manifest.hpp"
#include "update/preconditions.hpp"
#include "update/preference.hpp"
#include "update/safe_moment.hpp"
#include "update/source.hpp"
#include "update/tray_badge.hpp"
#include "update/trust.hpp"
#include "update/work_folder.hpp"

namespace {

using namespace gtg::update;

void Require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

// A backslash, spelled so that no editor or tool turns an escape in this file
// into the character it stands for.
const std::string kBackslash(1, '\x5c');

std::string Hex64(const char c) {
    return std::string(64, c);
}

// A manifest that passes every rule, built from parts a test can change.
struct ManifestText {
    std::string format{"1"};
    std::string version{"\"1.2.1\""};
    std::string published{"\"2026-11-05T08:00:00.1234567Z\""};
    std::string notes{"\"Two lines.\\nSecond.\""};
    std::string entry{"\"GpuThermalGuard.exe\""};
    std::string exe_sha{Hex64('A')};
    std::string dll_sha{Hex64('b')};
    std::string build{Hex64('A')};
    std::string dir{"\"builds/0123456789ABCDEF/overlay\""};
    std::string exe_size{"2340120"};
    std::string dll_size{"879896"};
    std::string extra_variant;
    std::string release;   // computed when empty

    std::string Variants() const {
        std::string files = std::format(
            "[{{\"name\":\"GpuThermalGuard.exe\",\"size\":{},\"sha256\":\"{}\"}},"
            "{{\"name\":\"gtg_overlay.dll\",\"size\":{},\"sha256\":\"{}\"}}]",
            exe_size, exe_sha, dll_size, dll_sha);
        std::string text = std::format("{{\"overlay\":{{\"build\":\"{}\",\"dir\":{},\"files\":{}}}",
                                       build, dir, files);
        if (!extra_variant.empty()) text += "," + extra_variant;
        return text + "}";
    }

    std::string Text(const Sha256& sha) const {
        std::string identity = release;
        if (identity.empty()) {
            std::vector<ManifestVariant> variants(1);
            variants[0].name = "overlay";
            variants[0].build = ParseDigest(build).value_or(Digest{});
            identity = ToHex(*ReleaseOf(variants, sha));
        }
        return std::format(
            "{{\"format\":{},\"version\":{},\"release\":\"{}\",\"published\":{},\"notes\":{},"
            "\"entry\":{},\"variants\":{}}}",
            format, version, identity, published, notes, entry, Variants());
    }
};

void TestDigestHex() {
    const auto digest = ParseDigest(Hex64('f'));
    Require(digest && (*digest)[0] == 0xFF && (*digest)[31] == 0xFF, "lower-case hex parses");
    Require(ToHex(*digest) == Hex64('F'), "hex is written upper-case");
    Require(!ParseDigest(Hex64('A').substr(1)), "63 digits refused");
    Require(!ParseDigest(Hex64('A') + "0"), "65 digits refused");
    Require(!ParseDigest(std::string(63, 'A') + "g"), "a non-hex digit refused");
    Sha256 sha;
    Require(sha.Available(), "CNG SHA-256 loads from System32");
    // FIPS 180-2 test vector.
    Require(ToHex(*sha.Of(std::string_view("abc"))) ==
                "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD",
            "SHA-256 of abc");
    Require(ToHex(*sha.Of(std::string_view())) ==
                "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855",
            "SHA-256 of nothing");
}

void TestJsonAccepts() {
    const auto parsed =
        json::Parse(R"( {"a":[1,-2,0,3.5,1e3,true,false,null],"b":{"c":"x\"y\\z\/\n"},"e":""} )");
    Require(parsed.value.has_value(), "valid JSON parses: " + parsed.error);
    const json::Value& root = *parsed.value;
    const json::Value* a = root.Find("a");
    Require(a && a->IsArray() && a->items.size() == 8, "array members");
    Require(a->items[0].integer == 1 && a->items[1].integer == -2 && a->items[2].integer == 0,
            "integers");
    Require(!a->items[3].integer && !a->items[4].integer, "fractions and exponents are no integer");
    Require(
        a->items[5].boolean && !a->items[6].boolean && a->items[7].kind == json::Value::Kind::Null,
        "literals");
    Require(root.Find("b")->Find("c")->text == "x\"y\\z/\n", "escapes unescaped");
    Require(root.Find("missing") == nullptr, "absent key");
    // A u escape, a surrogate pair and raw UTF-8.
    const std::string escaped = "{\"s\":\"" + kBackslash + "u00e9" + kBackslash + "ud83d" +
                                kBackslash + "ude00\xE4\xB8\xAD\"}";
    const auto unicode = json::Parse(escaped);
    Require(
        unicode.value && unicode.value->Find("s")->text == "\xC3\xA9\xF0\x9F\x98\x80\xE4\xB8\xAD",
        "escapes and UTF-8 decode to UTF-8");
    const auto big = json::Parse("{\"n\":99999999999999999999}");
    Require(big.value && !big.value->Find("n")->integer, "an integer that does not fit has none");
}

void TestJsonRefuses() {
    const std::vector<std::pair<std::string, std::string>> cases{
        {"\xEF\xBB\xBF{}", "byte order mark"},
        {"{} {}", "data after the value"},
        {"{\"a\":1,}", "trailing comma"},
        {"[1,]", "trailing comma in an array"},
        {"{\"a\":1,\"a\":2}", "duplicate key"},
        {"{'a':1}", "single quotes"},
        {"{\"a\":01}", "leading zero"},
        {"{\"a\":1.}", "bare decimal point"},
        {"{\"a\":.5}", "missing integer part"},
        {"{\"a\":+1}", "plus sign"},
        {"{\"a\":tru}", "broken literal"},
        {"{\"a\":\"x\ny\"}", "raw control character"},
        {"{\"a\":\"" + kBackslash + "x\"}", "unknown escape"},
        {"{\"a\":\"" + kBackslash + "ud800\"}", "lone high surrogate"},
        {"{\"a\":\"" + kBackslash + "udc00\"}", "lone low surrogate"},
        {"{\"a\":\"" + kBackslash + "u0000\"}", "NUL"},
        {"{\"a\":\"\xC0\xAF\"}", "overlong UTF-8"},
        {"{\"a\":\"\xED\xA0\x80\"}", "UTF-8 surrogate"},
        {"{\"a\":\"\xF4\x90\x80\x80\"}", "beyond U+10FFFF"},
        {"{\"a\":\"\xE4\xB8\"}", "truncated UTF-8"},
        {"{\"a\":\"abc", "unterminated string"},
        {"{\"a\" 1}", "missing colon"},
        {"", "empty"},
        {"/*c*/{}", "comment"},
    };
    for (const auto& [text, what] : cases) {
        Require(!json::Parse(text).value, "refused: " + what);
    }
    json::Limits limits;
    limits.max_depth = 3;
    // The scalar inside counts as a level.
    Require(json::Parse("[[1]]", limits).value.has_value(), "depth 3 allowed");
    Require(!json::Parse("[[[1]]]", limits).value, "depth 4 refused");
    limits = {};
    limits.max_string_bytes = 4;
    Require(json::Parse("\"abcd\"", limits).value.has_value(), "string at the limit");
    Require(!json::Parse("\"abcde\"", limits).value, "string over the limit");
    limits = {};
    limits.max_bytes = 8;
    Require(!json::Parse("[1,2,3,4,5]", limits).value, "text over the byte limit");
    limits = {};
    limits.max_values = 3;
    Require(!json::Parse("[1,2,3]", limits).value, "too many values");
}

void TestJsonQuote() {
    Require(json::Quote("a\"b") == "\"a" + kBackslash + "\"b\"", "quote escaped");
    Require(json::Quote(kBackslash) == "\"" + kBackslash + kBackslash + "\"", "backslash escaped");
    Require(json::Quote(std::string(1, '\x01')) == "\"" + kBackslash + "u0001\"",
            "control character escaped");
    const std::string path = "C:" + kBackslash + "Program Files" + kBackslash + "\xE4\xB8\xAD";
    const auto round = json::Parse("{\"p\":" + json::Quote(path) + "}");
    Require(round.value && round.value->Find("p")->text == path, "a path round-trips");
}

void TestNamesAndDirs() {
    for (const char* name : {"GpuThermalGuard.exe", "gtg_overlay.dll", "a", "9-x_y.z", "overlay"}) {
        Require(IsSafeName(name), std::string("safe: ") + name);
    }
    for (const char* name : {"", ".x", "-x", "_x", "a b", "a/b", "a\\b", "a:b", "x.", "NUL",
                             "nul.txt", "COM1", "lpt9.dll", "con", "Aux.json", "..", "a*"}) {
        Require(!IsSafeName(name), std::string("unsafe: ") + name);
    }
    Require(IsSafeName(std::string(64, 'a')) && !IsSafeName(std::string(65, 'a')), "64 max");
    Require(IsSafeName("COM10") && IsSafeName("console"), "only exact device names are reserved");
    Require(IsSafeDir("builds/0123456789ABCDEF/overlay") && IsSafeDir("x"), "safe dirs");
    for (const char* dir :
         {"", "/x", "x/", "x//y", "x/../y", "x/./y", "..", "a\\b", "a/b/c/d/e/f/g/h/i"}) {
        Require(!IsSafeDir(dir), std::string("unsafe dir: ") + dir);
    }
}

void TestVersionAndTime() {
    Require(ParseVersion("1.2.0") == Version{1, 2, 0}, "version parses");
    Require(ParseVersion("65535.0.10") == Version{65535, 0, 10}, "large components");
    for (const char* text : {"1.2", "1.2.0.0", "01.2.0", "1.2.x", "1..0", "-1.2.0", "1.2.0 ",
                             "65536.0.0", "1.2.0-beta"}) {
        Require(!ParseVersion(text), std::string("refused version: ") + text);
    }
    Require(Version{1, 10, 0} > Version{1, 9, 9}, "versions compare numerically");
    Require(FormatVersion({1, 2, 3}) == "1.2.3", "version text");
    Require(ParseUtcMs("1970-01-01T00:00:00Z") == 0, "epoch");
    Require(ParseUtcMs("2026-11-05T08:00:00.1234567Z") == 1'793'865'600'123LL,
            "seven fraction digits keep milliseconds");
    Require(ParseUtcMs("2024-02-29T23:59:59.9Z") == 1'709'251'199'900LL, "leap day");
    for (const char* text :
         {"2023-02-29T00:00:00Z", "2026-11-05T24:00:00Z", "2026-11-05T08:00:00",
          "2026-11-05 08:00:00Z", "2026-11-05T08:00:00.Z", "2026-11-05T08:00:00.12345678Z",
          "2026-11-05T08:00:00+08:00", "2026-13-01T00:00:00Z", "2026-11-05T08:60:00Z"}) {
        Require(!ParseUtcMs(text), std::string("refused time: ") + text);
    }
}

void TestManifestAccepts() {
    Sha256 sha;
    const ManifestText text;
    const ManifestResult result = ParseManifest(text.Text(sha), sha);
    Require(result.manifest.has_value(), "manifest parses: " + result.error);
    const Manifest& manifest = *result.manifest;
    Require(manifest.version == Version{1, 2, 1}, "version");
    Require(manifest.entry == "GpuThermalGuard.exe", "entry");
    Require(manifest.notes == L"Two lines.\nSecond.", "notes keep line breaks");
    const ManifestVariant* overlay = manifest.Find("overlay");
    Require(overlay && overlay->files.size() == 2 && overlay->dir.size() == 31, "variant");
    Require(overlay->Find("gputhermalguard.EXE") != nullptr, "file names ignore case");
    Require(overlay->files[0].size == 2340120, "size");
    Require(manifest.Find("no-overlay") == nullptr, "absent variant");

    ManifestText flat;
    flat.dir = "\"\"";
    Require(ParseManifest(flat.Text(sha), sha).manifest.has_value(), "empty dir is the root");
    ManifestText unknown = text;
    unknown.notes = "\"x\",\"future\":{\"anything\":[1,2]}";
    Require(ParseManifest(unknown.Text(sha), sha).manifest.has_value(), "unknown keys ignored");

    // Two variants: the release covers both, sorted by name whatever the order written.
    ManifestText two;
    two.extra_variant = std::format(
        "\"no-overlay\":{{\"build\":\"{}\",\"files\":[{{\"name\":\"GpuThermalGuard.exe\","
        "\"size\":1,\"sha256\":\"{}\"}}]}}",
        Hex64('C'), Hex64('C'));
    std::vector<ManifestVariant> variants(2);
    variants[0].name = "overlay";
    variants[0].build = *ParseDigest(Hex64('A'));
    variants[1].name = "no-overlay";
    variants[1].build = *ParseDigest(Hex64('C'));
    two.release = ToHex(*ReleaseOf(variants, sha));
    const auto expected = sha.Of("no-overlay=" + Hex64('C') + "\noverlay=" + Hex64('A'));
    Require(*ReleaseOf(variants, sha) == *expected, "release is over sorted variant=BUILD lines");
    const ManifestResult both = ParseManifest(two.Text(sha), sha);
    Require(both.manifest && both.manifest->variants.size() == 2 &&
                both.manifest->variants[0].name == "no-overlay",
            "two variants, sorted: " + both.error);
}

void TestManifestRefuses() {
    Sha256 sha;
    std::vector<std::pair<std::string, ManifestText>> cases;
    auto add = [&](const std::string& what, auto change) {
        ManifestText text;
        change(text);
        cases.emplace_back(what, text);
    };
    add("format 2", [](ManifestText& t) { t.format = "2"; });
    add("format as text", [](ManifestText& t) { t.format = "\"1\""; });
    add("version with four parts", [](ManifestText& t) { t.version = "\"1.2.1.0\""; });
    add("published without Z", [](ManifestText& t) { t.published = "\"2026-11-05T08:00:00\""; });
    add("notes not text", [](ManifestText& t) { t.notes = "3"; });
    add("entry with a path", [](ManifestText& t) { t.entry = "\"../GpuThermalGuard.exe\""; });
    add("entry not among the files", [](ManifestText& t) { t.entry = "\"Other.exe\""; });
    add("build not the entry's hash", [](ManifestText& t) { t.build = Hex64('D'); });
    add("short hash", [](ManifestText& t) { t.exe_sha = Hex64('A').substr(2); });
    add("dir climbing out", [](ManifestText& t) { t.dir = "\"builds/../../x\""; });
    add("absolute dir", [](ManifestText& t) { t.dir = "\"/builds\""; });
    add("dir with a backslash", [](ManifestText& t) { t.dir = "\"builds\\\\x\""; });
    add("size zero", [](ManifestText& t) { t.exe_size = "0"; });
    add("size over 64 MiB", [](ManifestText& t) { t.exe_size = "67108865"; });
    add("negative size", [](ManifestText& t) { t.exe_size = "-1"; });
    add("fractional size", [](ManifestText& t) { t.exe_size = "1.5"; });
    add("release of something else", [](ManifestText& t) { t.release = Hex64('E'); });
    add("bad variant name", [](ManifestText& t) {
        t.extra_variant = "\"../x\":{\"build\":\"" + Hex64('A') + "\",\"files\":[]}";
    });
    for (const auto& [what, text] : cases) {
        const ManifestResult result = ParseManifest(text.Text(sha), sha);
        Require(!result.manifest && !result.error.empty(), "manifest refused: " + what);
    }
    // A file listed twice under names Windows treats as one.
    const std::string twice = ManifestText{}.Text(sha);
    std::string doubled = twice;
    const std::string dll = "\"gtg_overlay.dll\"";
    doubled.replace(doubled.find(dll), dll.size(), "\"GPUTHERMALGUARD.exe\"");
    Require(!ParseManifest(doubled, sha).manifest, "a name listed twice, by case");
    Require(!ParseManifest(std::string(kMaxManifestBytes + 1, ' '), sha).manifest, "over 64 KiB");
    Require(!ParseManifest("[]", sha).manifest, "not an object");
}

void TestDecision() {
    Sha256 sha;
    const Manifest manifest = *ParseManifest(ManifestText{}.Text(sha), sha).manifest;
    const ManifestVariant& variant = manifest.variants[0];
    Running running;
    running.version = {1, 2, 0};
    running.build = *ParseDigest(Hex64('9'));
    running.linked_unix_ms = manifest.published_unix_ms - 1;
    Require(OffersUpdate(Decide(running, manifest, variant)), "a newer version is an update");

    running.version = {1, 2, 1};
    Require(Decide(running, manifest, variant).compare == Compare::Newer,
            "same version, published after this build was linked: newer");
    running.linked_unix_ms = manifest.published_unix_ms;
    Require(Decide(running, manifest, variant).compare == Compare::Older,
            "same version, published no later than this build: not newer");
    running.version = {1, 3, 0};
    const Decision older = Decide(running, manifest, variant);
    Require(older.compare == Compare::Older && !OffersUpdate(older) && OffersReinstall(older),
            "an older release is offered only as a reinstall");
    running.build = variant.build;
    const Decision same = Decide(running, manifest, variant);
    Require(same.compare == Compare::SameBuild && !OffersUpdate(same) && OffersReinstall(same),
            "the same build: reinstall only");

    Manifest old = manifest;
    old.version = {1, 1, 9};
    running.build = *ParseDigest(Hex64('9'));
    const Decision floor = Decide(running, old, variant);
    Require(floor.below_floor && !OffersUpdate(floor) && !OffersReinstall(floor),
            "below the floor: nothing, forced or not");
    old.version = kFloor;
    Require(OffersReinstall(Decide(running, old, variant)), "the floor itself may be installed");
}

void TestSafeMoment() {
    SafeInputs in;
    in.protection_idle = true;
    in.temperature_c = 70.0;
    in.trigger_c = 85;
    Require(SafeMoment(in) == Wait::None, "idle, 15 C under the trigger, no overlay: now");
    in.temperature_c = 70.1;
    Require(SafeMoment(in) == Wait::Temperature, "less than 15 C under the trigger: wait");
    in.temperature_c.reset();
    Require(SafeMoment(in) == Wait::Temperature, "no fresh reading: wait");
    in.temperature_c = 40.0;
    in.overlay_sessions = 1;
    Require(SafeMoment(in) == Wait::Overlay, "a game draws the overlay: wait");
    in.protection_idle = false;
    Require(SafeMoment(in) == Wait::Protection, "protection busy comes first");

    // No NVIDIA GPU: no protection to interrupt and no temperature to wait for.
    SafeInputs none;
    none.no_gpu = true;
    Require(SafeMoment(none) == Wait::None, "no GPU, no overlay: now");
    none.overlay_sessions = 1;
    Require(SafeMoment(none) == Wait::Overlay, "no GPU, a game draws the overlay: wait");
}

std::string Narrow(const std::wstring& text) {
    std::string out;
    for (const wchar_t c : text)
        out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    return out;
}

void Put(const std::filesystem::path& path, const std::string_view bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    Require(static_cast<bool>(out), "write " + path.string());
}

std::string Get(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Gives a folder this test made an access list it can delete again. As the
// folder's owner the test may always rewrite the list.
void Unlock(const std::filesystem::path& path) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;OICI;FA;;;WD)", SDDL_REVISION_1, &descriptor, nullptr)) {
        return;
    }
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL dacl = nullptr;
    if (GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted)) {
        (void)SetNamedSecurityInfoW(
            const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr,
            dacl, nullptr);
    }
    LocalFree(descriptor);
}

class TempFolder {
public:
    explicit TempFolder(const std::wstring& tag)
        : path_(std::filesystem::temp_directory_path() /
                std::format(L"gtg-update-tests-{}-{}", GetCurrentProcessId(), tag)) {
        Clean();
        std::filesystem::create_directories(path_);
    }

    ~TempFolder() { Clean(); }

    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;

    [[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }

private:
    void Clean() {
        std::error_code error;
        if (!std::filesystem::exists(path_, error)) return;
        for (auto it = std::filesystem::recursive_directory_iterator(path_, error);
             !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
            if (it->is_directory()) Unlock(it->path());
        }
        Unlock(path_);
        std::filesystem::remove_all(path_, error);
    }

    std::filesystem::path path_;
};

void TestSourcePath() {
    const std::vector<std::wstring> valid{L"\\\\fileserver\\Tools\\GTG_Updates", L"\\\\host\\share",
                                          L"\\\\host\\share\\", L"\\\\10.0.0.2\\s\\a folder"};
    for (std::size_t i = 0; i < valid.size(); ++i) {
        Require(IsValidSource(valid[i]), std::format("valid source {}", i));
    }
    const std::vector<std::wstring> invalid{
        L"",
        L"K:\\tools\\GTG",
        L"\\\\host",
        L"\\\\host\\",
        L"\\\\?\\C:\\x",
        L"\\\\.\\pipe\\x",
        L"\\\\host\\share\\..\\x",
        L"\\\\host\\share\\.\\x",
        L"\\\\host\\\\share",
        L"\\\\host\\share\\a*b",
        L"\\\\host\\share\\a:b",
        L"//host/share",
        L"\\\\host\\share\\\\",
        L"relative\\x",
        L"\\\\h\\s\\" + std::wstring(260, L'a'),
    };
    for (std::size_t i = 0; i < invalid.size(); ++i) {
        Require(!IsValidSource(invalid[i]), std::format("invalid source {}", i));
    }
}

void TestReadBounded() {
    TempFolder temp(L"read");
    const auto file = temp.Path() / "ten";
    Put(file, "0123456789");
    const ReadResult whole = ReadBounded(file, 10);
    Require(whole.status == ReadResult::Status::Ok && whole.bytes.size() == 10,
            "read at the bound");
    Require(ReadBounded(file, 9).status == ReadResult::Status::TooLarge, "over the bound");
    Require(ReadBounded(temp.Path() / "none", 9).status == ReadResult::Status::Missing, "missing");
}

void TestLoadAndStage() {
    Sha256 sha;
    TempFolder temp(L"stage");
    const auto source = temp.Path() / "source";
    const auto release_dir = source / "builds" / "0123456789ABCDEF" / "overlay";
    std::filesystem::create_directories(release_dir);
    const std::string exe = "MZ stand-in executable";
    const std::string dll(70'000, 'd');
    Put(release_dir / "GpuThermalGuard.exe", exe);
    Put(release_dir / "gtg_overlay.dll", dll);
    ManifestText text;
    text.exe_sha = ToHex(*sha.Of(exe));
    text.build = text.exe_sha;
    text.dll_sha = ToHex(*sha.Of(dll));
    text.exe_size = std::to_string(exe.size());
    text.dll_size = std::to_string(dll.size());
    Put(source / "gtg-update.json", text.Text(sha));

    const SourceManifest loaded = LoadManifest(source, sha);
    Require(loaded.manifest.has_value(), "manifest loads: " + Narrow(loaded.problem));
    const UpdateSource folder = FolderSource(source.wstring());
    Require(!ReadManifest(folder, sha).problem.empty(), "a local folder is refused as a source");
    const Manifest& manifest = *loaded.manifest;
    const ManifestVariant& variant = *manifest.Find("overlay");

    const auto work = temp.Path() / "work";
    const std::wstring release = ReleaseFolderName(manifest.release);
    Require(release.size() == 16, "release folder name");
    std::filesystem::create_directories(work / "staged" / "OLDRELEASE000000");
    std::filesystem::create_directories(work / "staged" / release);
    Put(work / "staged" / release / "junk.txt", "x");
    // The source file carries a mark of the web; staging writes new files.
    Put(release_dir / "GpuThermalGuard.exe:Zone.Identifier", "[ZoneTransfer]\r\nZoneId=3\r\n");

    const StageResult staged = Stage(folder, manifest, variant, work, sha);
    Require(staged.Ok(), "staged: " + Narrow(staged.problem));
    Require(staged.files.size() == 2 && Get(staged.files[0].path) == exe &&
                Get(staged.files[1].path) == dll,
            "staged bytes are the source's");
    Require(staged.files[0].sha256 == variant.build, "staged entry is the build");
    Require(!std::filesystem::exists(staged.folder / "junk.txt"), "staging folder emptied first");
    const auto stream = staged.files[0].path.wstring() + L":Zone.Identifier";
    Require(GetFileAttributesW(stream.c_str()) == INVALID_FILE_ATTRIBUTES,
            "no Zone.Identifier comes along");
    RemoveStagedExcept(work, release);
    Require(!std::filesystem::exists(work / "staged" / "OLDRELEASE000000") &&
                std::filesystem::exists(staged.folder),
            "older staged releases removed, this one kept");

    Put(release_dir / "gtg_overlay.dll", std::string(dll.size(), 'e'));
    Require(!Stage(folder, manifest, variant, work, sha).Ok(),
            "a changed file of the listed size is refused");
    Put(release_dir / "gtg_overlay.dll", dll + "d");
    Require(!Stage(folder, manifest, variant, work, sha).Ok(), "a longer file is refused");
    std::filesystem::remove(release_dir / "gtg_overlay.dll");
    Require(!Stage(folder, manifest, variant, work, sha).Ok(), "a missing file");

    std::filesystem::remove(source / "gtg-update.json");
    Require(!LoadManifest(source, sha).problem.empty(), "no manifest");
    Put(source / "gtg-update.json", std::string(kMaxManifestBytes + 1, ' '));
    Require(!LoadManifest(source, sha).problem.empty(), "manifest over 64 KiB");
    Put(source / "gtg-update.json", "{}");
    const SourceManifest invalid = LoadManifest(source, sha);
    Require(!invalid.problem.empty() && !invalid.detail.empty(), "invalid manifest says why");
}

// A plain HTTP server on 127.0.0.1 for the release reader's tests: one route
// per path, each a status, an optional Location and a body. Every response
// closes its connection.
class LoopbackHttp {
public:
    struct Route {
        int status{200};
        std::string location;
        std::string body;
        // Sent as Content-Length instead of the body's size when set.
        std::optional<std::uint64_t> length;
        // No Content-Length: the body as one chunk, so only reading says how
        // long it is.
        bool chunked{};
        // Before anything is sent back.
        int delay_ms{};
    };

    LoopbackHttp() {
        WSADATA wsa{};
        Require(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "Winsock for the test server");
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        Require(listener_ != INVALID_SOCKET &&
                    bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
                    listen(listener_, 8) == 0,
                "the test server listens");
        int size = sizeof(address);
        (void)getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size);
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { Serve(); });
    }

    ~LoopbackHttp() {
        closesocket(listener_);
        thread_.join();
        WSACleanup();
    }

    LoopbackHttp(const LoopbackHttp&) = delete;
    LoopbackHttp& operator=(const LoopbackHttp&) = delete;

    void Set(const std::string& path, Route route) {
        std::scoped_lock lock(mutex_);
        routes_[path] = std::move(route);
    }

    [[nodiscard]] std::string Base() const { return std::format("http://127.0.0.1:{}", port_); }

    [[nodiscard]] std::vector<std::string> Requests() const {
        std::scoped_lock lock(mutex_);
        return requests_;
    }

    [[nodiscard]] std::string Agent() const {
        std::scoped_lock lock(mutex_);
        return agent_;
    }

private:
    void Serve() {
        for (;;) {
            const SOCKET client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) return;
            std::string request;
            char buffer[4096];
            while (request.find("\r\n\r\n") == std::string::npos && request.size() < 65536) {
                const int got = recv(client, buffer, sizeof(buffer), 0);
                if (got <= 0) break;
                request.append(buffer, static_cast<std::size_t>(got));
            }
            const std::size_t first = request.find(' ');
            const std::size_t second =
                first == std::string::npos ? first : request.find(' ', first + 1);
            const std::string path = second == std::string::npos
                                         ? std::string()
                                         : request.substr(first + 1, second - first - 1);
            std::string agent;
            if (const std::size_t at = request.find("User-Agent: "); at != std::string::npos) {
                agent = request.substr(at + 12, request.find("\r\n", at) - at - 12);
            }
            Route route{404, {}, {}, {}};
            {
                std::scoped_lock lock(mutex_);
                requests_.push_back(path);
                if (!agent.empty()) agent_ = agent;
                if (const auto found = routes_.find(path); found != routes_.end()) {
                    route = found->second;
                }
            }
            if (route.delay_ms > 0) Sleep(static_cast<DWORD>(route.delay_ms));
            std::string response =
                std::format("HTTP/1.1 {} Test\r\nConnection: close\r\n", route.status);
            if (route.chunked) {
                response += "Transfer-Encoding: chunked\r\n\r\n";
                response +=
                    std::format("{:x}\r\n", route.body.size()) + route.body + "\r\n0\r\n\r\n";
            } else {
                response +=
                    std::format("Content-Length: {}\r\n", route.length.value_or(route.body.size()));
                if (!route.location.empty()) response += "Location: " + route.location + "\r\n";
                response += "\r\n" + route.body;
            }
            std::size_t sent = 0;
            while (sent < response.size()) {
                const int chunk =
                    static_cast<int>(std::min<std::size_t>(response.size() - sent, 65536));
                const int done = send(client, response.data() + sent, chunk, 0);
                if (done <= 0) break;
                sent += static_cast<std::size_t>(done);
            }
            shutdown(client, SD_SEND);
            closesocket(client);
        }
    }

    SOCKET listener_{INVALID_SOCKET};
    std::uint16_t port_{};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::map<std::string, Route> routes_;
    std::vector<std::string> requests_;
    std::string agent_;
};

void TestHttpRules() {
    const auto url = ParseHttpUrl(L"HTTPS://GitHub.com/allenk/GpuThermalGuard/releases?x=1#top");
    Require(url && url->secure && url->host == L"github.com" && url->port == 443 &&
                url->path == L"/allenk/GpuThermalGuard/releases?x=1",
            "an https URL: host in lower case, the fragment dropped");
    const auto plain = ParseHttpUrl(L"http://127.0.0.1:8080");
    Require(plain && !plain->secure && plain->port == 8080 && plain->path == L"/",
            "http with a port and no path");
    for (const wchar_t* bad :
         {L"", L"ftp://github.com/x", L"https://", L"https:///x", L"https://user@github.com/x",
          L"https://git hub.com/x", L"https://github.com/a b", L"https://git_hub.com/x",
          L"https://github.com:0/x", L"https://github.com:70000/x", L"https://github.com:/x",
          L"//github.com/x", L"https://.github.com/x", L"https://github.com\u00e9/x"}) {
        Require(!ParseHttpUrl(bad), "refused: " + Narrow(bad));
    }
    Require(!ParseHttpUrl(L"https://github.com/" + std::wstring(4096, L'a')), "too long");

    const UpdateSource releases = ReleasesSource(L"GpuThermalGuard/test");
    const auto allowed = [&](const wchar_t* text, const FetchPolicy& policy) {
        const auto parsed = ParseHttpUrl(text);
        return parsed && Allowed(*parsed, policy);
    };
    Require(allowed(L"https://github.com/x", releases.policy) &&
                allowed(L"https://objects.githubusercontent.com/x", releases.policy) &&
                allowed(L"https://release-assets.githubusercontent.com/x", releases.policy),
            "GitHub and its content hosts");
    Require(!allowed(L"https://www.github.com/x", releases.policy) &&
                !allowed(L"https://evilgithub.com/x", releases.policy) &&
                !allowed(L"https://githubusercontent.com/x", releases.policy) &&
                !allowed(L"https://githubusercontent.com.example.com/x", releases.policy) &&
                !allowed(L"http://github.com/x", releases.policy) &&
                !allowed(L"http://127.0.0.1/x", releases.policy),
            "nothing else, and never plain http");
    FetchPolicy tests = releases.policy;
    tests.loopback_http = true;
    Require(allowed(L"http://127.0.0.1:9/x", tests) && !allowed(L"http://127.0.0.2/x", tests) &&
                !allowed(L"http://localhost/x", tests),
            "plain http to 127.0.0.1 only, and only when a test asks");

    const auto from = *ParseHttpUrl(L"https://github.com/a/b");
    Require(RedirectTarget(from, L"https://objects.githubusercontent.com/z?sig=1") ==
                std::optional<std::wstring>(L"https://objects.githubusercontent.com/z?sig=1"),
            "an absolute redirect");
    Require(RedirectTarget(from, L"/a/releases/download/v1.2.5/x") ==
                std::optional<std::wstring>(L"https://github.com/a/releases/download/v1.2.5/x"),
            "a path on the same host");
    Require(RedirectTarget(*ParseHttpUrl(L"http://127.0.0.1:81/a"), L"/b") ==
                std::optional<std::wstring>(L"http://127.0.0.1:81/b"),
            "the port is kept");
    Require(!RedirectTarget(from, L"//evil.example/x") && !RedirectTarget(from, L"relative/x") &&
                !RedirectTarget(from, L""),
            "nothing else");
    // Authorities that would name another host to a looser parser.
    for (const wchar_t* trick :
         {L"https://github.com\\@evil.example/x", L"https://github.com%2Eevil.example/x",
          L"https://github.com:443@evil.example/x", L"https://github.com@evil.example/x"}) {
        Require(!ParseHttpUrl(trick) && !RedirectTarget(from, trick), "refused: " + Narrow(trick));
    }
    Require(!allowed(L"https://github.com:8443/x", releases.policy) &&
                allowed(L"https://github.com:443/x", releases.policy),
            "https on port 443 only");
}

void TestSourceKinds() {
    const UpdateSource releases = SourceOf(L"", L"GpuThermalGuard/1.2.5");
    Require(releases.kind == UpdateSource::Kind::Releases &&
                releases.location == kPublicRepository &&
                releases.agent == L"GpuThermalGuard/1.2.5" && !releases.policy.loopback_http,
            "no source set: the public releases");
    Require(ManifestUrl(releases) ==
                L"https://github.com/allenk/GpuThermalGuard/releases/latest/download/"
                L"gtg-update.json",
            "the manifest of the latest release");
    Require(ReleaseFileUrl(releases, Version{1, 2, 5}, "GpuThermalGuard.exe") ==
                L"https://github.com/allenk/GpuThermalGuard/releases/download/v1.2.5/"
                L"GpuThermalGuard.exe",
            "files from the release the manifest names");
    Require(Describe(releases) == L"GitHub (allenk/GpuThermalGuard)", "described for the reader");
    const UpdateSource folder = SourceOf(L"\\\\server\\share\\GTG", L"x");
    Require(folder.kind == UpdateSource::Kind::Folder &&
                folder.location == L"\\\\server\\share\\GTG" && folder.policy.hosts.empty() &&
                Describe(folder) == folder.location,
            "a folder set: that folder");
    Require(CheckIntervalMs(UpdateSource::Kind::Folder) == 15 * 60'000 &&
                CheckIntervalMs(UpdateSource::Kind::Releases) == 24 * 60 * 60'000,
            "a folder every 15 minutes, GitHub once a day");
    Require(NextCheckMs(UpdateSource::Kind::Releases, false) == 24 * 60 * 60'000 &&
                NextCheckMs(UpdateSource::Kind::Releases, true) == 60 * 60'000 &&
                NextCheckMs(UpdateSource::Kind::Folder, true) == 15 * 60'000,
            "after a failed check, within the hour");

    // A stored source: absent or cleared is GitHub; anything unusable turns
    // updates off rather than sending them elsewhere.
    Require(!ClassifySource(false, std::nullopt).invalid &&
                ClassifySource(false, std::nullopt).source.empty(),
            "absent: GitHub");
    Require(!ClassifySource(true, std::wstring()).invalid, "cleared: GitHub");
    const StoredSource unc = ClassifySource(true, std::wstring(L"\\\\host\\share"));
    Require(!unc.invalid && unc.source == L"\\\\host\\share", "a UNC path: that folder");
    const StoredSource drive = ClassifySource(true, std::wstring(L"K:\\GTG"));
    Require(drive.invalid && drive.source.empty() && drive.text == L"K:\\GTG",
            "not a UNC path: invalid, kept to correct");
    const StoredSource other = ClassifySource(true, std::nullopt);
    Require(other.invalid && other.source.empty() && other.text.empty(),
            "not text, or too long: invalid");
}

// Which certificate staged files must be signed with: the administrator's for
// any source, else the public releases' own for GitHub, else none.
void TestPinInEffect() {
    const std::optional<Digest> configured = ParseDigest(Hex64('A'));
    Require(PinInEffect(configured, UpdateSource::Kind::Releases) == configured,
            "a pin set wins over the public releases' own");
    Require(PinInEffect(configured, UpdateSource::Kind::Folder) == configured,
            "a pin set applies to a folder");
    Require(PinInEffect(std::nullopt, UpdateSource::Kind::Releases) == kPublicReleaseSigner,
            "GitHub without a pin: the public releases' certificate");
    Require(!PinInEffect(std::nullopt, UpdateSource::Kind::Folder),
            "a folder without a pin: none, as before");

    // The built-in pin is the certificate the release workflow checks it signed with.
    const std::string certificate = Get(std::filesystem::path(GTG_SOURCE_DIR) / "tools" / "update" /
                                        "public-release-signer.cer");
    Require(!certificate.empty(), "the public release certificate is in the tree");
    const Sha256 sha;
    Require(sha.Of(certificate) == kPublicReleaseSigner,
            "the built-in pin is the SHA-256 of tools/update/public-release-signer.cer");
}

// The release reader end to end, against a server that answers as GitHub does:
// the latest release redirects to its tag, the tag to a content host.
void TestReleasesLoopback() {
    const Sha256 sha;
    LoopbackHttp server;
    const std::string exe = "MZ stand-in executable";
    const std::string dll(70'000, 'd');
    ManifestText text;
    text.exe_sha = ToHex(*sha.Of(exe));
    text.build = text.exe_sha;
    text.dll_sha = ToHex(*sha.Of(dll));
    text.exe_size = std::to_string(exe.size());
    text.dll_size = std::to_string(dll.size());
    text.dir = "\"\"";
    const std::string repo = "/allenk/GpuThermalGuard";
    const std::string tag = repo + "/releases/download/v1.2.1/";
    server.Set(repo + "/releases/latest/download/gtg-update.json",
               {302, tag + "gtg-update.json", {}, {}});
    server.Set(tag + "gtg-update.json", {302, server.Base() + "/assets/manifest", {}, {}});
    server.Set("/assets/manifest", {200, {}, text.Text(sha), {}});
    server.Set(tag + "GpuThermalGuard.exe", {302, "/assets/exe", {}, {}});
    server.Set("/assets/exe", {200, {}, exe, {}});
    server.Set(tag + "gtg_overlay.dll", {200, {}, dll, {}});

    UpdateSource source = ReleasesSource(L"GpuThermalGuard/test");
    const std::string base = server.Base();
    source.location = std::wstring(base.begin(), base.end()) + L"/allenk/GpuThermalGuard";
    source.policy.loopback_http = true;
    const SourceManifest loaded = ReadManifest(source, sha);
    Require(loaded.manifest && loaded.manifest->version == Version{1, 2, 1},
            "the manifest through two redirects: " + Narrow(loaded.problem) + " " + loaded.detail);
    Require(server.Agent() == "GpuThermalGuard/test", "the user agent names the build");
    const Manifest& manifest = *loaded.manifest;
    const ManifestVariant& variant = *manifest.Find("overlay");
    TempFolder temp(L"releases");
    const StageResult staged = Stage(source, manifest, variant, temp.Path(), sha);
    Require(staged.Ok() && staged.files.size() == 2 && Get(staged.files[0].path) == exe &&
                Get(staged.files[1].path) == dll,
            "staged from the release: " + Narrow(staged.problem));
    const auto requests = server.Requests();
    Require(std::count(requests.begin(), requests.end(), tag + "GpuThermalGuard.exe") == 1 &&
                std::none_of(requests.begin(), requests.end(),
                             [](const std::string& path) {
                                 return path.find("latest/download/Gpu") != std::string::npos;
                             }),
            "the files come from the release the manifest names, never from latest");

    // Each refusal both as the read reports it and as staging does.
    const std::wstring dll_url = ReleaseFileUrl(source, manifest.version, "gtg_overlay.dll");
    const auto refused = [&](const FetchResult::Status status, const std::string& why) {
        Require(FetchBounded(dll_url, dll.size(), source.policy, source.agent).status == status,
                why + ": the read");
        Require(!Stage(source, manifest, variant, temp.Path(), sha).Ok(), why + ": staging");
    };
    server.Set(tag + "gtg_overlay.dll", {200, {}, std::string(dll.size(), 'x'), {}});
    refused(FetchResult::Status::Ok, "a changed file of the listed size is refused by its hash");
    server.Set(tag + "gtg_overlay.dll", {200, {}, dll + "x", {}});
    refused(FetchResult::Status::TooLarge, "a longer file");
    server.Set(tag + "gtg_overlay.dll", {200, {}, "small", 100'000'000});
    refused(FetchResult::Status::TooLarge, "a length past the bound, before the body is read");
    server.Set(tag + "gtg_overlay.dll", {200, {}, dll + "x", {}, true});
    refused(FetchResult::Status::TooLarge, "a body without a length that runs past the bound");
    server.Set(tag + "gtg_overlay.dll", {302, "https://example.com/gtg_overlay.dll", {}, {}});
    refused(FetchResult::Status::Refused, "a redirect to a host not allowed");
    server.Set(tag + "gtg_overlay.dll", {302, "http://127.0.0.2/gtg_overlay.dll", {}, {}});
    refused(FetchResult::Status::Refused, "a redirect to another address over plain http");
    server.Set(tag + "gtg_overlay.dll", {302, tag + "gtg_overlay.dll", {}, {}});
    refused(FetchResult::Status::Refused, "a redirect loop ends");
    server.Set(tag + "gtg_overlay.dll", {404, {}, {}, {}});
    refused(FetchResult::Status::Missing, "a missing file");
    server.Set(tag + "gtg_overlay.dll", {500, {}, {}, {}});
    refused(FetchResult::Status::Failed, "a server error");
    server.Set(tag + "gtg_overlay.dll", {200, {}, dll, {}, true});
    Require(Stage(source, manifest, variant, temp.Path(), sha).Ok(), "a body without a length");
    server.Set(tag + "gtg_overlay.dll", {200, {}, dll, {}});
    Require(Stage(source, manifest, variant, temp.Path(), sha).Ok(), "and it works again");

    ManifestVariant in_folder = variant;
    in_folder.dir = "builds/x";
    const std::size_t before = server.Requests().size();
    Require(!Stage(source, manifest, in_folder, temp.Path(), sha).Ok() &&
                server.Requests().size() == before,
            "a release has no folders: refused before anything is read");

    server.Set(repo + "/releases/latest/download/gtg-update.json", {404, {}, {}, {}});
    const SourceManifest missing = ReadManifest(source, sha);
    Require(!missing.manifest && !missing.problem.empty() &&
                missing.detail.find("http=404") != std::string::npos,
            "a latest release without a manifest");
    server.Set(repo + "/releases/latest/download/gtg-update.json",
               {200, {}, std::string(kMaxManifestBytes + 1, ' '), {}});
    Require(!ReadManifest(source, sha).manifest, "a manifest over 64 KiB");
    server.Set(repo + "/releases/latest/download/gtg-update.json", {200, {}, "{}", {}});
    const SourceManifest invalid = ReadManifest(source, sha);
    Require(!invalid.manifest && !invalid.detail.empty(), "an invalid manifest says why");

    UpdateSource plain = source;
    plain.policy.loopback_http = false;
    Require(FetchBounded(ManifestUrl(plain), 100, plain.policy, plain.agent).status ==
                FetchResult::Status::Refused,
            "plain http is refused unless a test allows it");
    FetchPolicy hurried = source.policy;
    hurried.deadline_ms = 300;
    server.Set("/slow", {200, {}, "late", {}, false, 1'000});
    const FetchResult slow =
        FetchBounded(std::wstring(base.begin(), base.end()) + L"/slow", 100, hurried, source.agent);
    Require(slow.status == FetchResult::Status::Failed && slow.error == ERROR_TIMEOUT,
            "a read past its deadline is abandoned");
    UpdateSource closed = source;
    closed.location = L"http://127.0.0.1:1/allenk/GpuThermalGuard";
    const FetchResult nothing = FetchBounded(ManifestUrl(closed), 100, closed.policy, closed.agent);
    Require(nothing.status == FetchResult::Status::Failed && nothing.error != 0,
            "nothing listening: a failure with its error");
}

void TestWorkFolder() {
    TempFolder temp(L"acl");
    const auto work = temp.Path() / kWorkFolderName;
    DWORD error = 0;
    auto check = [&](const std::string& when) {
        PACL dacl = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        Require(
            GetNamedSecurityInfoW(work.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                  nullptr, &dacl, nullptr, &descriptor) == ERROR_SUCCESS,
            when + ": access list readable");
        SECURITY_DESCRIPTOR_CONTROL control = 0;
        DWORD revision = 0;
        GetSecurityDescriptorControl(descriptor, &control, &revision);
        bool system = false;
        bool administrators = false;
        for (DWORD i = 0; dacl != nullptr && i < dacl->AceCount; ++i) {
            void* ace = nullptr;
            if (!GetAce(dacl, i, &ace)) continue;
            auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
            const PSID sid = &allowed->SidStart;
            const bool inherits =
                (allowed->Header.AceFlags & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)) ==
                (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE);
            const bool full = allowed->Mask == FILE_ALL_ACCESS && inherits &&
                              allowed->Header.AceType == ACCESS_ALLOWED_ACE_TYPE;
            system |= full && IsWellKnownSid(sid, WinLocalSystemSid);
            administrators |= full && IsWellKnownSid(sid, WinBuiltinAdministratorsSid);
        }
        const bool exact = dacl != nullptr && dacl->AceCount == 2;
        LocalFree(descriptor);
        Require((control & SE_DACL_PROTECTED) != 0, when + ": nothing inherited from the parent");
        Require(exact && system && administrators, when + ": SYSTEM and Administrators only");
    };
    Require(SecureFolder(work, kWorkFolderSddl, error), "work folder created");
    check("created");
    Unlock(work);
    Require(SecureFolder(work, kWorkFolderSddl, error), "an existing folder secured again");
    check("existing");

    // With the owner granted too, files made inside inherit the list.
    const auto open = temp.Path() / "owner";
    Require(SecureFolder(open, L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;OW)", error),
            "owner-writable folder");
    Put(open / "inside.txt", "x");

    const InstallFolder writable = InspectInstallFolder(temp.Path());
    Require(writable.user_writable && !writable.remote && writable.acls,
            "a temporary folder is the user's to write");
    wchar_t system[MAX_PATH]{};
    GetSystemDirectoryW(system, MAX_PATH);
    Require(!InspectInstallFolder(system).user_writable, "System32 is not the user's to write");
    // Elevated or not, with UAC on or off: a program without administrator
    // rights cannot write where only administrators may.
    const auto admins = temp.Path() / "admins";
    Require(SecureFolder(admins, L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", error),
            "administrators-only folder");
    Require(!InspectInstallFolder(admins).user_writable,
            "a folder only administrators may write is not the user's to write");
    Require(InspectInstallFolder(L"\\\\host\\share\\GTG").remote, "a share is remote");
}

void TestPreconditions() {
    const std::wstring name = std::format(L"Local\\GtgUpdateTest.{}", GetCurrentProcessId());
    Require(!UpdateRunning(name.c_str()), "no mutex: no update running");
    const HANDLE mutex = CreateMutexW(nullptr, TRUE, name.c_str());
    Require(mutex != nullptr && UpdateRunning(name.c_str()), "the mutex held: an update running");
    CloseHandle(mutex);
    Require(!UpdateRunning(name.c_str()), "released: none running");
    // Their values are this machine's; they must answer without failing.
    (void)ServiceInstalled();
    (void)SmartAppControlState();
}

void TestPlanText() {
    Plan plan;
    // A CJK folder name, written by code point.
    plan.install = std::filesystem::path(L"C:\\Program Files\\GTG ") / std::wstring(1, 0x6E2C);
    plan.entry = "GpuThermalGuard.exe";
    plan.old_version = "1.2.0";
    plan.old_build = Hex64('1');
    plan.new_version = "1.2.1";
    plan.new_build = Hex64('2');
    plan.release = Hex64('3');
    plan.reinstall = true;
    PlanFile file;
    file.name = "GpuThermalGuard.exe";
    file.staged =
        plan.install / L"_gtg_update" / L"staged" / L"0123456789ABCDEF" / L"GpuThermalGuard.exe";
    file.size = 2340120;
    file.sha256 = *ParseDigest(Hex64('2'));
    plan.files.push_back(file);
    plan.wait.push_back({1234, 133'764'000'000'000'000ULL});
    plan.wait.push_back({5678, 18'446'744'073'709'551'615ULL});
    const std::string text = FormatPlan(plan);
    const auto parsed = json::Parse(text);
    Require(parsed.value.has_value(), "the plan is JSON: " + parsed.error);
    const json::Value& root = *parsed.value;
    Require(root.Find("format")->integer == 1, "format");
    Require(root.Find("install")->text == Utf8(plan.install.wstring()), "install path in UTF-8");
    Require(root.Find("new")->Find("version")->text == "1.2.1", "new version");
    Require(root.Find("reinstall")->boolean, "reinstall");
    const json::Value& staged = root.Find("files")->items[0];
    Require(staged.Find("sha256")->text == Hex64('2') && staged.Find("size")->integer == 2340120,
            "file hash and size");
    Require(root.Find("wait")->items[1].Find("created")->text == "18446744073709551615",
            "creation times as text, exact");
    Require(root.Find("mutex")->text == "Global\\GpuThermalGuard.Update.v1", "the update mutex");
    Require(
        root.Find("ready_ms")->integer == 90'000 && root.Find("wait_exit_ms")->integer == 60'000,
        "time limits");
    const HANDLE self = GetCurrentProcess();
    Require(ProcessCreated(self) != 0, "this process has a creation time");
}

void TestEmbeddedScript() {
    const auto script = EmbeddedScript(GetModuleHandleW(nullptr));
    Require(script.has_value(), "the script is compiled in");
    const std::string source =
        Get(std::filesystem::path(GTG_SOURCE_DIR) / "src" / "update" / "apply-update.ps1");
    Require(*script == source, "the compiled-in script is apply-update.ps1");
    for (const wchar_t* name : {kLoadedFileName, kReadyFileName, kOutcomeFileName, kLogFileName}) {
        const std::string quoted = "'" + Utf8(name) + "'";
        Require(script->find(quoted) != std::string_view::npos, "the script names " + Utf8(name));
    }
    for (const char* forbidden : {"Start-Process", "Invoke-Item", "explorer.exe", "EncodedCommand",
                                  "Get-FileHash", "Invoke-Expression", "iex "}) {
        Require(script->find(forbidden) == std::string_view::npos,
                std::string("the script never uses ") + forbidden);
    }
    for (const char c : *script) {
        Require(static_cast<unsigned char>(c) < 0x80,
                "ASCII only: Windows PowerShell reads it as ANSI");
    }
}

void TestHandOff() {
    TempFolder temp(L"handoff");
    const auto work = temp.Path() / "work";
    std::filesystem::create_directories(work);
    Plan plan;
    plan.install = temp.Path();
    plan.entry = "GpuThermalGuard.exe";
    const DWORD self = GetCurrentProcessId();
    // A script that does what the real one does first: name itself in
    // loaded.json, then leave its environment behind for the test to read.
    const std::string answers =
        "param($Plan)\n"
        "$d = Split-Path -Parent $Plan\n"
        "[IO.File]::WriteAllText((Join-Path $d 'env.txt'), [string]$env:PSModulePath)\n"
        "[IO.File]::WriteAllText((Join-Path $d 'loaded.json'), '{\"pid\":' + $PID + '}')\n";
    // Each script runs in the work folder: it must be gone before the folder is.
    auto finished = [](const DWORD pid) {
        const HANDLE process = pid != 0 ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
        if (process == nullptr) return;
        WaitForSingleObject(process, 10'000);
        CloseHandle(process);
    };
    // The script starts in its parent's job. Here the parent is this process, in
    // a job of its own, so the script must be found in it; apply_test.ps1 checks
    // the opposite under the real supervisor.
    const HANDLE job = CreateJobObjectW(nullptr, nullptr);
    Require(job != nullptr && AssignProcessToJobObject(job, GetCurrentProcess()),
            "this test in a job of its own");
    CloseHandle(job);
    SetEnvironmentVariableW(L"PSModulePath", L"X:\\left-by-a-shell");
    const HandOffResult ok = HandOff(work, plan, answers, self, 20'000);
    SetEnvironmentVariableW(L"PSModulePath", nullptr);
    finished(ok.script_pid);
    Require(ok.ok && ok.script_pid != 0,
            "a script that answers is handed the update: " + Narrow(ok.problem));
    Require(ok.in_job, "a script started under this process is in this process's job");
    Require(std::filesystem::exists(work / kPlanFileName), "the plan stays for the script");
    Require(Get(work / kScriptFileName) == answers, "the script written is the one given");
    Require(Get(work / "env.txt").find("left-by-a-shell") == std::string::npos,
            "the script does not inherit PSModulePath");
    Require(!std::filesystem::exists(work / kUnprotectedFileName),
            "a build that protects a GPU writes no unprotected file");

    // A build with no GPU says so beside the plan, so the next build's
    // supervisor may count a Tray without one as ready.
    plan.unprotected = true;
    const HandOffResult early = HandOff(work, plan, "exit 5\n", self, 20'000);
    plan.unprotected = false;
    Require(!early.ok && early.problem.find(L"5") != std::wstring::npos,
            "a script that ends early fails at once: " + Narrow(early.problem));
    Require(!std::filesystem::exists(work / kPlanFileName), "and its plan is deleted");
    Require(std::filesystem::exists(work / kUnprotectedFileName),
            "a build with no GPU writes the unprotected file");

    // Answers for another process, then waits for its plan to go.
    const std::string wrong =
        "param($Plan)\n"
        "$d = Split-Path -Parent $Plan\n"
        "[IO.File]::WriteAllText((Join-Path $d 'loaded.json'), '{\"pid\":4}')\n"
        "while ([IO.File]::Exists($Plan)) { Start-Sleep -Milliseconds 100 }\n";
    const ULONGLONG begin = GetTickCount64();
    const HandOffResult late = HandOff(work, plan, wrong, self, 2'000);
    finished(late.script_pid);
    Require(!late.ok && GetTickCount64() - begin >= 2'000,
            "no answer naming the script: it waits, then fails");
    Require(!std::filesystem::exists(work / kPlanFileName),
            "the plan is deleted, so a late script stops");
    Require(!std::filesystem::exists(work / kUnprotectedFileName),
            "a hand-off by a build that protects clears an unprotected file left behind");

    const HandOffResult no_parent = HandOff(work, plan, answers, 0, 2'000);
    Require(!no_parent.ok, "no supervisor to start it under");
}

const char* VerdictName(const SignerVerdict verdict) {
    switch (verdict) {
        case SignerVerdict::Pinned:
            return "pinned";
        case SignerVerdict::Unsigned:
            return "unsigned";
        case SignerVerdict::Invalid:
            return "invalid";
        case SignerVerdict::OtherSigner:
            return "other-signer";
        case SignerVerdict::Unavailable:
            return "unavailable";
    }
    return "?";
}

hosts::Status SampleStatus() {
    hosts::Status status;
    status.has_source = true;
    status.accepts_triggers = true;
    status.pin_set = true;
    status.folder_writable = true;
    status.phase = hosts::Phase::Waiting;
    status.wait = hosts::WaitFor::Overlay;
    status.source = hosts::SourceIs::Newer;
    status.source_version = {1, 2, 1};
    status.source_release = {1, 2, 3, 4, 5, 6, 7, 8};
    status.running_build = {9, 10, 11, 12, 13, 14, 15, 16};
    status.trigger = 0x0102030405060708ULL;
    status.answer = hosts::Answer::Accepted;
    return status;
}

void TestHostsWire() {
    const hosts::Status status = SampleStatus();
    const hosts::StatusBody body = hosts::WriteStatus(status);
    Require(hosts::ReadStatus(body) == status, "a status reads back as written");
    Require(hosts::ReadStatus(hosts::WriteStatus({})) == hosts::Status{}, "an empty status too");
    Require(!hosts::ReadStatus(std::span(body).first(hosts::kStatusBodyBytes - 1)),
            "a status of another size is refused");
    for (const std::size_t reserved : {7, 33, 36, 39}) {
        auto changed = body;
        changed[reserved] = 1;
        Require(!hosts::ReadStatus(changed),
                std::format("a reserved status byte ({}) that is not zero is refused", reserved));
    }
    auto flags = body;
    flags[0] |= 0x10;
    Require(!hosts::ReadStatus(flags), "an unknown status flag is refused");
    for (const auto& [at, value] :
         std::vector<std::pair<std::size_t, std::uint8_t>>{{1, hosts::kPhaseCount},
                                                           {2, hosts::kWaitForCount},
                                                           {3, hosts::kSourceIsCount},
                                                           {32, hosts::kAnswerCount}}) {
        auto changed = body;
        changed[at] = value;
        Require(!hosts::ReadStatus(changed),
                std::format("a value this version does not know (byte {}) is refused", at));
    }
    auto no_trigger = body;
    for (std::size_t i = 24; i < 32; ++i)
        no_trigger[i] = 0;
    Require(!hosts::ReadStatus(no_trigger), "an answer without a trigger is refused");
    auto no_answer = body;
    no_answer[32] = 0;
    Require(!hosts::ReadStatus(no_answer), "a trigger without an answer is refused");
    hosts::Status unanswered = status;
    unanswered.trigger = 0;
    Require(hosts::ReadStatus(hosts::WriteStatus(unanswered))->answer == hosts::Answer::None,
            "an answer is written only with its trigger");

    hosts::Trigger trigger;
    trigger.id = 0xFEDCBA9876543210ULL;
    for (std::size_t i = 0; i < trigger.release.size(); ++i)
        trigger.release[i] = static_cast<std::uint8_t>(i + 1);
    trigger.force = true;
    const hosts::TriggerBody sent = hosts::WriteTrigger(trigger);
    Require(hosts::ReadTrigger(sent) == trigger, "a trigger reads back as written");
    trigger.force = false;
    Require(hosts::ReadTrigger(hosts::WriteTrigger(trigger)) == trigger, "without force too");
    Require(!hosts::ReadTrigger(std::span(sent).first(hosts::kTriggerBodyBytes - 1)),
            "a trigger of another size is refused");
    hosts::Trigger zero = trigger;
    zero.id = 0;
    Require(!hosts::ReadTrigger(hosts::WriteTrigger(zero)), "a trigger without an id is refused");
    auto trigger_flags = sent;
    trigger_flags[40] |= 0x02;
    Require(!hosts::ReadTrigger(trigger_flags), "an unknown trigger flag is refused");
    auto trigger_reserved = sent;
    trigger_reserved[47] = 1;
    Require(!hosts::ReadTrigger(trigger_reserved), "a reserved trigger byte must be zero");
}

void TestTriggerRules() {
    using hosts::Answer;
    TriggerGate gate;
    gate.allowed = true;
    gate.has_source = true;
    gate.idle = true;
    const std::int64_t now = 1'800'000'000'000;
    Require(AdmitTrigger(gate, now) == Answer::Accepted, "allowed, a source, idle: read it");
    TriggerGate refused = gate;
    refused.allowed = false;
    refused.has_source = false;
    refused.idle = false;
    Require(AdmitTrigger(refused, now) == Answer::NotAllowed, "not allowed comes first");
    refused.allowed = true;
    Require(AdmitTrigger(refused, now) == Answer::NoSource, "then no source");
    refused.has_source = true;
    Require(AdmitTrigger(refused, now) == Answer::Busy, "then an update in progress");

    gate.last_unix_ms = now - kTriggerIntervalMs + 1;
    Require(AdmitTrigger(gate, now) == Answer::TooSoon, "one trigger per 30 minutes");
    gate.last_unix_ms = now - kTriggerIntervalMs;
    Require(AdmitTrigger(gate, now) == Answer::Accepted, "30 minutes later, again");
    gate.last_unix_ms = now + 60'000;
    Require(AdmitTrigger(gate, now) == Answer::TooSoon,
            "a clock set back a minute still holds the next trigger off");
    gate.last_unix_ms = now + kTriggerIntervalMs;
    Require(AdmitTrigger(gate, now) == Answer::Accepted,
            "a time further ahead than the interval does not hold triggers off for longer");
    gate.last_unix_ms = now - 1'000;
    Require(AdmitTrigger(gate, now, true) == Answer::Accepted,
            "a forced request skips the interval");
    Require(AdmitTrigger(gate, now, false) == Answer::TooSoon, "an ordinary one does not");
    refused.idle = false;
    refused.last_unix_ms = now - 1'000;
    Require(AdmitTrigger(refused, now, true) == Answer::Busy, "force skips nothing else");
    refused.has_source = false;
    Require(AdmitTrigger(refused, now, true) == Answer::NoSource, "nor the source");
    refused.allowed = false;
    Require(AdmitTrigger(refused, now, true) == Answer::NotAllowed, "nor the switch");

    // A request is answered once: its repeats and replays are recognised by
    // id, and the build an update starts begins with the ones acted on before.
    SeenTriggers seen;
    Require(!seen.First(0), "id 0 is never a request");
    Require(seen.First(5) && !seen.First(5), "the first time only");
    for (std::uint64_t id = 100; id < 116; ++id)
        Require(seen.First(id), "sixteen others");
    Require(seen.First(5), "remembered for the last sixteen");
    std::vector<std::uint64_t> kept;
    for (std::uint64_t id = 1; id <= kKeptTriggers + 2; ++id)
        kept = KeepTrigger(std::move(kept), id);
    Require(kept.size() == kKeptTriggers && kept.front() == 3 && kept.back() == kKeptTriggers + 2,
            "the last eight acted on, oldest first");
    kept = KeepTrigger(std::move(kept), 0);
    kept = KeepTrigger(std::move(kept), kKeptTriggers + 2);
    Require(kept.size() == kKeptTriggers && kept.back() == kKeptTriggers + 2,
            "neither 0 nor one already kept is added");
    const std::vector<std::uint8_t> stored = StoredTriggers(kept);
    Require(stored.size() == kKeptTriggers * 8 && KeptTriggers(stored) == kept,
            "kept as 8 bytes each and read back");
    std::vector<std::uint8_t> odd = stored;
    odd.pop_back();
    Require(KeptTriggers(odd).empty(), "a value that is not whole ids is not read");
    Require(KeptTriggers(std::vector<std::uint8_t>(8 * (kKeptTriggers + 1), 1)).empty(),
            "nor one longer than eight");
    Require(std::wstring_view(AnswerName(Answer::TooSoon)) == L"too-soon" &&
                std::wstring_view(AnswerName(Answer::Updated)) == L"updated",
            "answers by name, for the journal");

    hosts::Trigger trigger;
    trigger.id = 1;
    trigger.release[0] = 7;
    Digest release{};
    release[0] = 7;
    Digest other = release;
    other[31] = 1;
    const Decision newer{Compare::Newer, false};
    const Decision same{Compare::SameBuild, false};
    const Decision older{Compare::Older, false};
    const Decision floor{Compare::Newer, true};
    Require(DecideTrigger(trigger, other, newer) == Answer::OtherRelease,
            "only the release that was asked for");
    Require(DecideTrigger(trigger, release, newer) == Answer::Accepted, "a newer release");
    Require(DecideTrigger(trigger, release, same) == Answer::AlreadyCurrent,
            "the build that runs: nothing to do");
    Require(DecideTrigger(trigger, release, older) == Answer::NotNewer, "an older release: no");
    Require(DecideTrigger(trigger, release, floor) == Answer::BelowFloor, "below the floor: no");
    trigger.force = true;
    Require(DecideTrigger(trigger, release, same) == Answer::Accepted, "forced: the same build");
    Require(DecideTrigger(trigger, release, older) == Answer::Accepted, "forced: an older one");
    Require(DecideTrigger(trigger, release, floor) == Answer::BelowFloor,
            "forced, below the floor: still no");
    Require(DecideTrigger(trigger, other, same) == Answer::OtherRelease,
            "forced, another release: still no");

    Require(SourceIsOf(std::nullopt, false) == hosts::SourceIs::Unknown &&
                SourceIsOf(std::nullopt, true) == hosts::SourceIs::Unreadable &&
                SourceIsOf(floor, false) == hosts::SourceIs::BelowFloor &&
                SourceIsOf(older, false) == hosts::SourceIs::Older,
            "the source as the status reports it");
    Require(AnswerOfOutcome("updated") == Answer::Updated &&
                AnswerOfOutcome("rolled_back") == Answer::RolledBack &&
                AnswerOfOutcome("aborted") == Answer::Aborted &&
                AnswerOfOutcome("anything else") == Answer::Failed,
            "the script's outcome as an answer");
}

void TestHostsUpdates() {
    using hosts::Answer;
    std::uint64_t next_id = 100;
    HostsUpdates updates([&] { return next_id++; });
    SourceView source;
    source.version = {1, 2, 1};
    source.release[0] = 0xAB;
    const auto at_release = [&] {
        hosts::Status status = SampleStatus();
        status.phase = hosts::Phase::Idle;
        status.source = hosts::SourceIs::SameBuild;
        status.source_release = hosts::ShortOf(source.release);
        status.trigger = 0;
        status.answer = Answer::None;
        return status;
    };
    const auto behind = [&] {
        hosts::Status status = at_release();
        status.source = hosts::SourceIs::Newer;
        return status;
    };
    const auto host = [](const std::uint64_t id, const bool online,
                         std::optional<hosts::Status> status) {
        HostView view;
        view.id = id;
        view.name = L"h" + std::to_wstring(id);
        view.online = online;
        view.version = {1, 2, 0};
        view.status = std::move(status);
        return view;
    };
    hosts::Status unwilling = behind();
    unwilling.accepts_triggers = false;
    std::vector<HostView> table{host(1, true, at_release()), host(2, true, behind()),
                                host(3, true, behind()),     host(4, true, std::nullopt),
                                host(5, false, behind()),    host(6, true, unwilling)};
    std::uint64_t now = 1'000;

    updates.Tick(now, table, std::nullopt);
    Require(!updates.CanAsk(2) && !updates.UpdateAll(now),
            "nothing is asked before this machine's source is read");
    updates.Tick(now, table, source);
    Require(updates.CanAsk(1) && updates.CanAsk(2), "a willing host, at the release or not");
    Require(!updates.CanAsk(4) && !updates.CanAsk(5) && !updates.CanAsk(6) && !updates.CanAsk(9),
            "not a host without the updater, one offline, one unwilling or one not paired");
    Require(updates.Candidates() == std::vector<std::uint64_t>{2, 3},
            "update all asks the willing hosts that are behind, in order");

    Require(updates.Ask(1, true, now), "a forced update of one host");
    Require(!updates.CanAsk(1) && !updates.Ask(1, true, now), "once until it answers");
    const auto outbox = updates.TakeOutbox();
    Require(outbox.size() == 1 && outbox[0].first == 1, "one trigger, to that host");
    const auto sent = hosts::ReadTrigger(outbox[0].second);
    Require(sent && sent->id == 100 && sent->force && sent->release == source.release,
            "naming this machine's release");
    Require(updates.TakeOutbox().empty(), "taken once");
    now += kTriggerAnswerMs;
    updates.Tick(now, table, source);
    Require(updates.AskedOf(1)->ask == HostsUpdates::Ask::NoAnswer && updates.CanAsk(1),
            "no echo in 15 s: no answer, and it can be asked again");
    Require(updates.Ask(1, false, now), "asked again");
    table[0].status->trigger = 101;
    table[0].status->answer = Answer::TooSoon;
    updates.Tick(now + 1'000, table, source);
    Require(updates.AskedOf(1)->ask == HostsUpdates::Ask::Answered &&
                updates.AskedOf(1)->answer == Answer::TooSoon,
            "the answer is read from the status that echoes the trigger");
    (void)updates.TakeOutbox();

    // Update all: host 2, then host 3.
    Require(updates.UpdateAll(now), "update all starts");
    Require(updates.Progress().current == 2u &&
                updates.Progress().pending == std::vector<std::uint64_t>{3},
            "one host at a time");
    Require(!updates.CanAsk(3) && !updates.UpdateAll(now), "nothing else while it runs");
    const auto first = updates.TakeOutbox();
    Require(first.size() == 1 && first[0].first == 2 && !hosts::ReadTrigger(first[0].second)->force,
            "an ordinary trigger, not a forced one");
    const std::uint64_t id2 = hosts::ReadTrigger(first[0].second)->id;
    table[1].status->trigger = id2;
    table[1].status->answer = Answer::Accepted;
    updates.Tick(now += 1'000, table, source);
    Require(updates.Progress().current == 2u, "accepted: waits");
    table[1].online = false;
    updates.Tick(now += 5'000, table, source);
    Require(updates.Progress().run == HostsUpdates::Run::Running,
            "a host restarting is waited for");
    table[1].online = true;
    table[1].status->answer = Answer::Updated;
    updates.Tick(now += 1'000, table, source);
    Require(updates.Progress().current == 2u,
            "updated, but its state still shows the old version: waits");
    table[1].version = {1, 2, 1};
    updates.Tick(now += 1'000, table, source);
    Require(updates.Progress().done == std::vector<std::uint64_t>{2} &&
                updates.Progress().current == 3u,
            "back with the new version: the next host");
    const auto second = updates.TakeOutbox();
    Require(second.size() == 1 && second[0].first == 3, "the next trigger");
    table[2].status->trigger = hosts::ReadTrigger(second[0].second)->id;
    table[2].status->answer = Answer::RolledBack;
    updates.Tick(now += 1'000, table, source);
    Require(updates.Progress().run == HostsUpdates::Run::Stopped &&
                updates.Progress().stopped_at == 3u &&
                updates.Progress().stopped_answer == Answer::RolledBack,
            "a rollback stops update all, at that host");

    // A host that never answers, one that never comes back, the reader.
    HostsUpdates silent([&] { return next_id++; });
    std::vector<HostView> two{host(2, true, behind()), host(3, true, behind())};
    silent.Tick(now, two, source);
    Require(silent.UpdateAll(now), "update all");
    silent.Tick(now + kTriggerAnswerMs, two, source);
    Require(silent.Progress().run == HostsUpdates::Run::Stopped &&
                silent.Progress().stopped_no_answer && silent.Progress().stopped_at == 2u,
            "no answer stops it");
    HostsUpdates away([&] { return next_id++; });
    away.Tick(now, two, source);
    Require(away.UpdateAll(now), "update all");
    const auto asked = away.TakeOutbox();
    two[0].status->trigger = hosts::ReadTrigger(asked[0].second)->id;
    two[0].status->answer = Answer::Accepted;
    away.Tick(now + 1'000, two, source);
    two[0].online = false;
    away.Tick(now + 2'000, two, source);
    away.Tick(now + 2'000 + kRolloutAwayMs, two, source);
    Require(away.Progress().run == HostsUpdates::Run::Stopped && away.Progress().stopped_away,
            "a host that does not come back stops it");
    HostsUpdates reader([&] { return next_id++; });
    reader.Tick(now, two, source);
    two[0].online = true;
    reader.Tick(now, two, source);
    Require(reader.UpdateAll(now), "update all");
    reader.Stop();
    Require(
        reader.Progress().run == HostsUpdates::Run::Stopped && reader.Progress().stopped_by_reader,
        "the reader stops it");
    two[1].status = at_release();
    HostsUpdates skip([&] { return next_id++; });
    skip.Tick(now, two, source);
    Require(skip.UpdateAll(now) && skip.Progress().pending.empty() && skip.Progress().current == 2u,
            "a host already at the release is not asked");

    // A confirmation runs ticks inside it, which replace every host: a host
    // found before it must be a copy, not a pointer into what the tick frees.
    static_assert(std::is_same_v<decltype(std::declval<const HostsUpdates&>().Find(0)),
                                 std::optional<HostView>> &&
                  std::is_same_v<decltype(std::declval<const HostsUpdates&>().Source()),
                                 std::optional<SourceView>>);
    {
        HostsUpdates churn([&] { return next_id++; });
        churn.Tick(now, std::vector<HostView>{host(7, true, behind())}, source);
        const auto kept = churn.Find(7);
        for (int i = 0; i < 50; ++i)
            churn.Tick(now, std::vector<HostView>{host(8 + i, true, behind())}, source);
        Require(kept && kept->name == L"h7", "a host found before the ticks is still whole after");
    }

    // The case from the field: a host updates within the seconds the request
    // is repeated, and the build the update started gets the last repeats. It
    // answers them again -- busy while it confirms the update, too soon after.
    // Once accepted, an answer only admission gives is not taken, and an
    // outcome is final.
    {
        HostsUpdates repeat([&] { return next_id++; });
        std::vector<HostView> pair{host(2, true, behind()), host(3, true, behind())};
        repeat.Tick(now, pair, source);
        Require(repeat.UpdateAll(now), "update all");
        const std::uint64_t id = hosts::ReadTrigger(repeat.TakeOutbox()[0].second)->id;
        pair[0].status->trigger = id;
        pair[0].status->answer = Answer::Accepted;
        repeat.Tick(now += 1'000, pair, source);
        pair[0].online = false;
        repeat.Tick(now += 1'000, pair, source);
        pair[0].online = true;
        pair[0].version = {1, 2, 1};
        pair[0].status->source = hosts::SourceIs::Unknown;
        pair[0].status->answer = Answer::Busy;
        repeat.Tick(now += 1'000, pair, source);
        Require(repeat.Progress().run == HostsUpdates::Run::Running &&
                    repeat.Progress().current == 2u &&
                    repeat.AskedOf(2)->answer == Answer::Accepted,
                "busy after accepted is the request's repeat: ignored");
        pair[0].status->answer = Answer::Updated;
        repeat.Tick(now += 1'000, pair, source);
        Require(repeat.Progress().done == std::vector<std::uint64_t>{2} &&
                    repeat.Progress().current == 3u,
                "updated: the next host");
        pair[0].status->answer = Answer::TooSoon;
        repeat.Tick(now += 1'000, pair, source);
        Require(repeat.AskedOf(2)->answer == Answer::Updated &&
                    repeat.Progress().run == HostsUpdates::Run::Running,
                "an outcome is final");
        // Before its own first check the updated host does not say it is at the
        // release; the request it answered "updated" does.
        Require(repeat.AtRelease(*repeat.Find(2)), "updated from here: at the release");
        repeat.Stop();
        Require(repeat.Candidates() == std::vector<std::uint64_t>{3},
                "and update all does not ask it again");
        source.release[1] = 1;
        repeat.Tick(now += 1'000, pair, source);
        Require(!repeat.AtRelease(*repeat.Find(2)), "until the source names another release");
        source.release[1] = 0;

        const std::vector<std::wstring> journal = repeat.TakeJournal();
        const auto has = [&](const std::wstring_view part) {
            return std::any_of(journal.begin(), journal.end(), [&](const std::wstring& line) {
                return line.find(part) != std::wstring::npos;
            });
        };
        Require(has(L"asked h2 to update to") && has(L"h2 answered accepted") &&
                    has(L"h2 answered busy") && has(L"h2 answered updated") &&
                    has(L"asked h3 to update to"),
                "the journal tells what was asked and answered");
        Require(repeat.TakeJournal().empty(), "taken once");
    }
    {
        HostsUpdates stops([&] { return next_id++; });
        std::vector<HostView> one{host(2, true, behind())};
        stops.Tick(now, one, source);
        Require(stops.UpdateAll(now), "update all");
        one[0].status->trigger = hosts::ReadTrigger(stops.TakeOutbox()[0].second)->id;
        one[0].status->answer = Answer::StageFailed;
        stops.Tick(now + 1'000, one, source);
        const std::vector<std::wstring> journal = stops.TakeJournal();
        Require(std::any_of(journal.begin(), journal.end(),
                            [](const std::wstring& line) {
                                return line.find(L"update all stopped at h2: stage-failed") !=
                                       std::wstring::npos;
                            }),
                "and where update all stopped, and why");
    }

    // A host that stops taking requests after the list was made is skipped.
    HostsUpdates later([&] { return next_id++; });
    std::vector<HostView> both{host(2, true, behind()), host(3, true, behind())};
    later.Tick(now, both, source);
    Require(later.UpdateAll(now), "update all");
    const auto ask2 = later.TakeOutbox();
    both[0].status->trigger = hosts::ReadTrigger(ask2[0].second)->id;
    both[0].status->answer = Answer::Updated;
    both[0].version = {1, 2, 1};
    both[1].status->accepts_triggers = false;
    later.Tick(now + 1'000, both, source);
    Require(later.Progress().run == HostsUpdates::Run::Done && later.TakeOutbox().empty(),
            "a host that no longer takes requests is not asked");
}

// The link carries the two update bodies as they were sealed, both ways, and
// keeps showing the state beside them.
void TestLinkCarriesUpdates() {
    using namespace gtg::peer;
    const std::array<std::uint8_t, 4> loopback{127, 0, 0, 1};
    WSADATA wsa{};
    Require(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "the test's own socket");
    const auto free_port = [] {
        const SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int size = sizeof(local);
        bind(probe, reinterpret_cast<const sockaddr*>(&local), sizeof(local));
        getsockname(probe, reinterpret_cast<sockaddr*>(&local), &size);
        closesocket(probe);
        return ntohs(local.sin_port);
    };
    const std::uint16_t port = free_port();
    Link a;
    Link b;
    Link::Options alpha;
    alpha.self = {0xA1, L"Alpha"};
    alpha.port = port;
    alpha.bind_address = loopback;
    Link::Options bravo;
    bravo.self = {0xB2, L"Bravo"};
    bravo.port = static_cast<std::uint16_t>(port + 1);
    bravo.bind_address = loopback;
    Require(a.Start(alpha) && b.Start(bravo), "two links start");
    HostState state;
    state.version = {1, 2, 1};
    state.protection = Protection::Normal;
    hosts::Status status_a = SampleStatus();
    const hosts::StatusBody body_a = hosts::WriteStatus(status_a);
    const auto await = [&](Link& link, auto ready) -> std::optional<Table> {
        const std::uint64_t deadline = GetTickCount64() + 8'000;
        while (GetTickCount64() < deadline) {
            a.Publish(state);
            b.Publish(state);
            a.PublishUpdate(body_a);
            if (auto table = link.TryRead(); table && ready(*table)) return table;
            Sleep(20);
        }
        return std::nullopt;
    };
    Require(await(a, [](const Table& t) { return t.status == LinkStatus::Running; }).has_value(),
            "the link runs");
    a.CreateCode();
    const auto issued = await(a, [](const Table& t) { return t.invitation.has_value(); });
    Require(issued.has_value(), "a code");
    const auto code = ParseCode(std::string_view(issued->invitation->code));
    b.Join(*code.code);
    const auto paired = await(b, [](const Table& t) { return t.hosts.size() == 1; });
    Require(paired.has_value(), "paired");

    const auto b_reads_status = await(b, [&](const Table& t) {
        return t.hosts.size() == 1 && t.hosts[0].state && t.hosts[0].update_status == body_a;
    });
    Require(
        b_reads_status && hosts::ReadStatus(*b_reads_status->hosts[0].update_status) == status_a,
        "the status arrives beside the state, as sealed");
    Require(!b_reads_status->hosts[0].update_trigger, "and no trigger nobody sent");

    hosts::Trigger trigger;
    trigger.id = 77;
    trigger.release[3] = 9;
    trigger.force = true;
    const hosts::TriggerBody body_t = hosts::WriteTrigger(trigger);
    b.Trigger(0xA1, body_t);
    const auto a_reads_trigger = await(a, [&](const Table& t) {
        return t.hosts.size() == 1 && t.hosts[0].update_trigger == body_t;
    });
    Require(
        a_reads_trigger && hosts::ReadTrigger(*a_reads_trigger->hosts[0].update_trigger) == trigger,
        "the trigger reaches the host it was sent to");
    Require(a_reads_trigger->hosts[0].state.has_value(), "the state keeps arriving");
    // Repeated for kTriggerRounds rounds, then no more.
    const auto last_heard = [&] {
        std::uint64_t at = 0;
        (void)await(a, [&](const Table& t) {
            at = t.hosts.empty() ? 0 : t.hosts[0].update_trigger_ms;
            return true;
        });
        return at;
    };
    Sleep(static_cast<DWORD>(kTriggerRounds + 2) * 1'000);
    const std::uint64_t settled = last_heard();
    Sleep(2'500);
    Require(settled != 0 && last_heard() == settled,
            "a trigger goes out with its first rounds only");
    a.Stop();
    b.Stop();
    WSACleanup();
}

void TestBadgeRule() {
    BadgeState offered;
    offered.decision = Decision{Compare::Newer, false};
    Require(ShowsBadge(offered), "a newer release shows the badge");
    BadgeState state = offered;
    state.decision = Decision{Compare::SameBuild, false};
    Require(!ShowsBadge(state), "the same build shows none");
    state.decision = Decision{Compare::Older, false};
    Require(!ShowsBadge(state), "an older release shows none");
    state.decision = Decision{Compare::Newer, true};
    Require(!ShowsBadge(state), "a release below the floor shows none");
    state.decision.reset();
    Require(!ShowsBadge(state), "nothing to compare shows none");
    state = offered;
    state.problem = true;
    Require(!ShowsBadge(state), "a problem shows none");
    BadgeState updating;
    updating.updating = true;
    Require(ShowsBadge(updating), "the reader's update on its way shows the badge");
    state = updating;
    state.reinstall = true;
    Require(!ShowsBadge(state), "a reinstall shows none");
    state = updating;
    state.requested = true;
    Require(!ShowsBadge(state), "a paired host's request shows none");
}

void TestBadgePaint() {
    for (const int size : {16, 20, 24, 32, 48}) {
        const BadgeBox box = BadgeBoxFor(size);
        Require(box.diameter * 2 >= size && box.diameter * 2 <= size + 1 && box.rim >= 1 &&
                    box.left + box.diameter == size,
                std::format("the dot is half the icon, top right, at {} px", size));
        const auto at = [size](const int x, const int y) {
            return static_cast<std::size_t>(y) * static_cast<std::size_t>(size) +
                   static_cast<std::size_t>(x);
        };
        // A half-transparent icon of distinct pixels.
        std::vector<std::uint32_t> pixels(static_cast<std::size_t>(size) * size);
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            pixels[i] = 0x80000000u | ((static_cast<std::uint32_t>(i) * 2654435761u) & 0xFFFFFF);
        }
        const std::vector<std::uint32_t> before = pixels;
        PaintBadge(pixels, size);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                if (x >= box.left && y < box.diameter) continue;
                Require(pixels[at(x, y)] == before[at(x, y)],
                        std::format("the icon is untouched outside the dot at {} px", size));
            }
        }
        const int middle = box.left + box.diameter / 2;
        Require(pixels[at(middle, box.diameter / 2)] == kBadgeFill,
                std::format("the dot's middle is the fill, opaque, at {} px", size));
        Require(pixels[at(size - 1, 0)] == before[at(size - 1, 0)] &&
                    pixels[at(box.left, 0)] == before[at(box.left, 0)],
                std::format("the box's corners stay the icon's at {} px", size));
        const std::uint32_t top = pixels[at(middle, 0)];
        Require((top >> 24) > 0x80 && ((top >> 8) & 0xFF) < ((kBadgeFill >> 8) & 0xFF),
                std::format("the dot's top edge is the dark rim at {} px", size));
        // Over an opaque icon everything stays opaque.
        std::vector<std::uint32_t> opaque(pixels.size(), 0xFF336699u);
        PaintBadge(opaque, size);
        for (const std::uint32_t pixel : opaque) {
            Require((pixel >> 24) == 0xFF,
                    std::format("an opaque icon stays opaque at {} px", size));
        }
    }
    std::vector<std::uint32_t> short_buffer(4, 0);
    PaintBadge(short_buffer, 16);
    Require(short_buffer == std::vector<std::uint32_t>(4, 0), "a buffer too short is left alone");
}

BITMAPINFO TopDown32(const int size) {
    BITMAPINFO header{};
    header.bmiHeader.biSize = sizeof(header.bmiHeader);
    header.bmiHeader.biWidth = size;
    header.bmiHeader.biHeight = -size;
    header.bmiHeader.biPlanes = 1;
    header.bmiHeader.biBitCount = 32;
    header.bmiHeader.biCompression = BI_RGB;
    return header;
}

std::vector<std::uint32_t> BitmapPixels(const HBITMAP bitmap, const int size) {
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(size) * size);
    BITMAPINFO header = TopDown32(size);
    const HDC screen = GetDC(nullptr);
    const int rows = GetDIBits(screen, bitmap, 0, static_cast<UINT>(size), pixels.data(), &header,
                               DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    Require(rows == size, "the icon's pixels");
    return pixels;
}

// An icon's pixels as straight-alpha 0xAARRGGBB rows, top down, and its mask,
// white where the icon is transparent.
std::vector<std::uint32_t> IconPixels(const HICON icon, int& size,
                                      std::vector<std::uint32_t>* mask = nullptr) {
    ICONINFO info{};
    Require(GetIconInfo(icon, &info) != FALSE, "the icon's bitmaps");
    BITMAP shape{};
    GetObjectW(info.hbmColor, sizeof(shape), &shape);
    size = shape.bmWidth;
    std::vector<std::uint32_t> pixels = BitmapPixels(info.hbmColor, size);
    if (mask != nullptr) *mask = BitmapPixels(info.hbmMask, size);
    DeleteObject(info.hbmColor);
    DeleteObject(info.hbmMask);
    return pixels;
}

void TestBadgeOnIcons() {
    Require(WithBadge(nullptr) == nullptr, "no icon, no badge");
    const std::filesystem::path icons = std::filesystem::path(GTG_SOURCE_DIR) / "assets" / "icons";
    // The state icons the tray shows, at its sizes for 100, 125, 150 and 200 %.
    for (const char* name :
         {"tray-armed", "tray-warning", "tray-protected", "tray-fault", "GpuThermalGuard"}) {
        for (const int size : {16, 20, 24, 32}) {
            const std::wstring file = (icons / (std::string(name) + ".ico")).wstring();
            const auto icon = static_cast<HICON>(
                LoadImageW(nullptr, file.c_str(), IMAGE_ICON, size, size, LR_LOADFROMFILE));
            Require(icon != nullptr, std::format("{} at {} px loads", name, size));
            int read = 0;
            const std::vector<std::uint32_t> before = IconPixels(icon, read);
            Require(read == size, std::format("{} at {} px is {} px", name, size, size));
            Require(std::any_of(before.begin(), before.end(),
                                [](std::uint32_t p) { return (p >> 24) == 0; }) &&
                        std::any_of(before.begin(), before.end(),
                                    [](std::uint32_t p) { return (p >> 24) == 0xFF; }),
                    std::format("{} at {} px has its alpha", name, size));
            const HICON badged = WithBadge(icon);
            Require(badged != nullptr, std::format("{} at {} px takes the badge", name, size));
            std::vector<std::uint32_t> mask;
            const std::vector<std::uint32_t> after = IconPixels(badged, read, &mask);
            std::vector<std::uint32_t> expected = before;
            PaintBadge(expected, size);
            Require(read == size && after == expected,
                    std::format("{} at {} px is the state icon with the badge on it", name, size));
            for (std::size_t i = 0; i < after.size(); ++i) {
                Require(((mask[i] & 0xFFFFFF) != 0) == ((after[i] >> 24) == 0),
                        std::format("{} at {} px is masked where it is transparent", name, size));
            }
            DestroyIcon(badged);
            DestroyIcon(icon);
        }
    }
}

void TestBadgeWithoutAlpha() {
    // An icon whose transparency is only in its mask: the left half.
    constexpr int kSize = 16;
    BITMAPINFO header = TopDown32(kSize);
    void* bits = nullptr;
    const HBITMAP color = CreateDIBSection(nullptr, &header, DIB_RGB_COLORS, &bits, nullptr, 0);
    Require(color != nullptr && bits != nullptr, "a colour bitmap");
    const std::vector<std::uint32_t> rgb(static_cast<std::size_t>(kSize) * kSize, 0x00336699u);
    std::memcpy(bits, rgb.data(), rgb.size() * sizeof(rgb[0]));
    std::vector<std::uint8_t> mask_bits(static_cast<std::size_t>(kSize) * 2, 0);
    for (int y = 0; y < kSize; ++y)
        mask_bits[static_cast<std::size_t>(y) * 2] = 0xFF;
    const HBITMAP mask = CreateBitmap(kSize, kSize, 1, 1, mask_bits.data());
    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmMask = mask;
    info.hbmColor = color;
    const HICON icon = CreateIconIndirect(&info);
    DeleteObject(color);
    DeleteObject(mask);
    Require(icon != nullptr, "an icon without alpha");
    const HICON badged = WithBadge(icon);
    Require(badged != nullptr, "an icon without alpha takes the badge");
    int read = 0;
    const std::vector<std::uint32_t> after = IconPixels(badged, read);
    std::vector<std::uint32_t> expected(rgb.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        expected[i] = i % kSize < 8 ? 0 : 0xFF336699u;
    }
    PaintBadge(expected, kSize);
    Require(read == kSize && after == expected, "an icon without alpha keeps its mask's shape");
    DestroyIcon(badged);
    DestroyIcon(icon);
}

}  // namespace

int main(int argc, char** argv) {
    // For trust_test.ps1, which signs files with a certificate it makes in
    // memory: verify-signer <file> <pin as 64 hex digits>.
    if (argc == 4 && std::string(argv[1]) == "verify-signer") {
        const auto pin = ParseDigest(argv[3]);
        if (!pin) return 2;
        Sha256 sha;
        const SignerResult result = VerifySigner(argv[2], *pin, sha);
        std::cout << VerdictName(result.verdict)
                  << std::format(" status=0x{:08X}", static_cast<unsigned long>(result.status))
                  << '\n';
        return 0;
    }
    // For publish_test.ps1: reads a source as GTG does and stages it.
    // fetch-host <url>: one read by the updater's rules from the URL's own host,
    // to see Windows refuse a bad certificate.
    if (argc == 3 && std::string(argv[1]) == "fetch-host") {
        const std::string url = argv[2];
        const std::wstring wide(url.begin(), url.end());
        const auto parsed = ParseHttpUrl(wide);
        if (!parsed) return 2;
        FetchPolicy policy;
        policy.hosts = {parsed->host};
        const FetchResult fetched = FetchBounded(wide, 1 << 20, policy, L"GpuThermalGuard/check");
        std::cout << std::format("status={} http={} error={} bytes={}",
                                 static_cast<int>(fetched.status), fetched.http_status,
                                 fetched.error, fetched.bytes.size())
                  << '\n';
        return fetched.status == FetchResult::Status::Ok ? 0 : 1;
    }
    // fetch <url> <max bytes>: one read from GitHub, by the updater's rules.
    if (argc == 4 && std::string(argv[1]) == "fetch") {
        const Sha256 sha;
        const std::string url = argv[2];
        const UpdateSource releases = ReleasesSource(L"GpuThermalGuard/check");
        const FetchResult fetched =
            FetchBounded(std::wstring(url.begin(), url.end()), std::stoull(argv[3]),
                         releases.policy, releases.agent);
        std::cout << std::format("status={} http={} error={} bytes={} sha256={}",
                                 static_cast<int>(fetched.status), fetched.http_status,
                                 fetched.error, fetched.bytes.size(),
                                 fetched.status == FetchResult::Status::Ok
                                     ? ToHex(*sha.Of(fetched.bytes))
                                     : std::string("-"))
                  << '\n';
        return fetched.status == FetchResult::Status::Ok ? 0 : 1;
    }
    // check-source <folder | github> <variant> [pin as 64 hex digits]
    if (argc >= 4 && std::string(argv[1]) == "check-source") {
        const Sha256 sha;
        const bool github = std::string(argv[2]) == "github";
        const std::filesystem::path folder(argv[2]);
        const UpdateSource source =
            github ? ReleasesSource(L"GpuThermalGuard/check") : FolderSource(folder.wstring());
        const SourceManifest loaded = github || folder.native().starts_with(L"\\\\")
                                          ? ReadManifest(source, sha)
                                          : LoadManifest(folder, sha);
        if (!loaded.manifest) {
            std::cout << "refused: " << Narrow(loaded.problem) << ' ' << loaded.detail << '\n';
            return 1;
        }
        const Manifest& manifest = *loaded.manifest;
        const ManifestVariant* variant = manifest.Find(argv[3]);
        if (variant == nullptr) {
            std::cout << "refused: no variant " << argv[3] << '\n';
            return 1;
        }
        TempFolder work(L"check-source");
        const StageResult staged = Stage(source, manifest, *variant, work.Path(), sha);
        if (!staged.Ok()) {
            std::cout << "refused: " << Narrow(staged.problem) << '\n';
            return 1;
        }
        std::string signers = "unsigned";
        if (argc >= 5) {
            const auto pin = ParseDigest(std::string_view(argv[4]));
            if (!pin) return 2;
            for (const StagedFile& file : staged.files) {
                const SignerResult signer = VerifySigner(file.path, *pin, sha);
                if (signer.verdict != SignerVerdict::Pinned) {
                    std::cout << "refused: " << file.name << ' ' << VerdictName(signer.verdict)
                              << '\n';
                    return 1;
                }
            }
            signers = "pinned";
        }
        std::cout << std::format(
                         "ok version={} release={} files={} {}", FormatVersion(manifest.version),
                         ToHex(manifest.release).substr(0, 16), staged.files.size(), signers)
                  << '\n';
        return 0;
    }
    const std::vector<std::pair<std::string, std::function<void()>>> tests{
        {"DigestHex", TestDigestHex},
        {"JsonAccepts", TestJsonAccepts},
        {"JsonRefuses", TestJsonRefuses},
        {"JsonQuote", TestJsonQuote},
        {"NamesAndDirs", TestNamesAndDirs},
        {"VersionAndTime", TestVersionAndTime},
        {"ManifestAccepts", TestManifestAccepts},
        {"ManifestRefuses", TestManifestRefuses},
        {"Decision", TestDecision},
        {"SafeMoment", TestSafeMoment},
        {"SourcePath", TestSourcePath},
        {"HttpRules", TestHttpRules},
        {"SourceKinds", TestSourceKinds},
        {"PinInEffect", TestPinInEffect},
        {"ReleasesLoopback", TestReleasesLoopback},
        {"ReadBounded", TestReadBounded},
        {"LoadAndStage", TestLoadAndStage},
        {"WorkFolder", TestWorkFolder},
        {"Preconditions", TestPreconditions},
        {"PlanText", TestPlanText},
        {"EmbeddedScript", TestEmbeddedScript},
        {"HandOff", TestHandOff},
        {"HostsWire", TestHostsWire},
        {"TriggerRules", TestTriggerRules},
        {"HostsUpdates", TestHostsUpdates},
        {"LinkCarriesUpdates", TestLinkCarriesUpdates},
        {"BadgeRule", TestBadgeRule},
        {"BadgePaint", TestBadgePaint},
        {"BadgeOnIcons", TestBadgeOnIcons},
        {"BadgeWithoutAlpha", TestBadgeWithoutAlpha},
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << tests.size() - static_cast<std::size_t>(failures) << "/" << tests.size()
              << " tests passed\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
