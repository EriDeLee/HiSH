// Host fixture for feature/hish_main/src/main/cpp/hish_link_core.h.
// A fake file system (a table of emulated links) drives the real resolver.
#include "hish_link_core.h"
#include <cstdio>
#include <map>
#include <string>

static int failures;
static void Expect(bool condition, const char* name)
{
    if (condition) { printf("ok %s\n", name); return; }
    ++failures; printf("FAIL %s\n", name);
}

struct Fake { std::map<std::string, std::string> links; };
static int Inspect(void* context, const char* path, char* target, size_t capacity, size_t* length)
{
    const auto& links = static_cast<Fake*>(context)->links;
    const auto found = links.find(path);
    if (found == links.end() || found->second.size() > capacity) return 0;
    memcpy(target, found->second.data(), found->second.size());
    *length = found->second.size();
    return 1;
}
static const HishLinkOps ops{Inspect};

// The resolved path, or "!<errno>" when resolution fails.
static std::string Resolve(Fake& fake, const std::string& path, int follow, const char* base = "/base", size_t capacity = 4096)
{
    std::string out(capacity, '\0');
    const int error = HishLinkResolve(&ops, &fake, base, path.c_str(), follow, &out[0], capacity);
    if (error) return "!" + std::to_string(error);
    return out.c_str();
}
static std::string Failed(int code) { return "!" + std::to_string(code); }

int main()
{
    Fake none;
    Expect(Resolve(none, "a/b", 1) == "/base/a/b", "a relative path starts at the base directory");
    Expect(Resolve(none, "/x/y/../z", 1) == "/x/z", "dot-dot removes the previous component");
    Expect(Resolve(none, "/a//b/./c", 1) == "/a/b/c", "repeated slashes and dots are dropped");
    Expect(Resolve(none, "/../a", 1) == "/a", "dot-dot cannot climb above the root");
    Expect(Resolve(none, "/", 1) == "/", "the root stays the root");
    Expect(Resolve(none, "/a/b/", 1) == "/a/b/", "a trailing slash is kept");
    Expect(Resolve(none, "", 1) == Failed(ENOENT), "an empty path is ENOENT");
    Expect(Resolve(none, "a", 1, nullptr) == Failed(EINVAL), "a relative path needs a base");
    Expect(Resolve(none, "a", 1, "relative") == Failed(EINVAL), "a base must be absolute");
    Expect(Resolve(none, "a", 1, "/base/", 4096) == "/base/a", "a base with a trailing slash is normalised");

    Fake f;
    f.links["/d/l"] = "target";
    Expect(Resolve(f, "/d/l", 1) == "/d/target", "a final link is followed when asked");
    Expect(Resolve(f, "/d/l", 0) == "/d/l", "a final link stays when not following");
    Expect(Resolve(f, "l", 1, "/d") == "/d/target", "a relative path reaches a link in the base directory");
    f.links["/d/abs"] = "/etc/x";
    Expect(Resolve(f, "/d/abs", 1) == "/etc/x", "an absolute target replaces the whole prefix");
    f.links["/d/dl"] = "../real";
    Expect(Resolve(f, "/d/dl/f", 0) == "/real/f", "a directory link in the middle is always followed");
    f.links["/d/up"] = "/r/s";
    Expect(Resolve(f, "/d/up/../f", 0) == "/r/f", "dot-dot after a directory link goes up from the target, not the link");
    Expect(Resolve(f, "/d/l/", 0) == "/d/target/", "a trailing slash forces the final link to be followed");
    Expect(Resolve(f, "/d/l/.", 0) == "/d/target", "a link followed by a dot component is followed");
    f.links["/d/dangling"] = "missing";
    Expect(Resolve(f, "/d/dangling", 1) == "/d/missing", "a dangling link resolves to the path it points at");
    f.links["/a/b/c"] = "../../x";
    Expect(Resolve(f, "/a/b/c", 1) == "/x", "a relative target may climb out of the link's directory");
    f.links["/d/dot"] = ".";
    f.links["/d/dotdot"] = "..";
    Expect(Resolve(f, "/d/dot", 1) == "/d" && Resolve(f, "/d/dotdot", 1) == "/", "links to dot and dot-dot");

    Fake chain;
    chain.links["/c/l1"] = "l2";
    chain.links["/c/l2"] = "l3";
    chain.links["/c/l3"] = "end";
    Expect(Resolve(chain, "/c/l1", 1) == "/c/end", "a chain of links is followed to its end");
    Fake multi;
    multi.links["/a/l1"] = "sub";
    multi.links["/a/sub/l2"] = "../deep";
    Expect(Resolve(multi, "/a/l1/l2/f", 1) == "/a/deep/f", "several links in one path are all followed");

    Fake loop;
    loop.links["/l/a"] = "b";
    loop.links["/l/b"] = "a";
    loop.links["/l/self"] = "self";
    Expect(Resolve(loop, "/l/a", 1) == Failed(ELOOP), "a two-link loop ends in ELOOP");
    Expect(Resolve(loop, "/l/self", 1) == Failed(ELOOP), "a link to itself ends in ELOOP");
    Expect(Resolve(loop, "/l/a", 0) == "/l/a", "a loop is harmless when the link is not followed");

    Fake forty;
    for (int i = 0; i < 40; i++) forty.links["/f/n" + std::to_string(i)] = "n" + std::to_string(i + 1);
    Expect(Resolve(forty, "/f/n0", 1) == "/f/n40", "exactly forty links are allowed");
    forty.links["/f/n40"] = "n41";
    Expect(Resolve(forty, "/f/n0", 1) == Failed(ELOOP), "the forty-first link is ELOOP");

    Expect(Resolve(f, "/d/l", 1, "/base", 5) == Failed(ENAMETOOLONG), "a result that does not fit is ENAMETOOLONG");
    Fake wide;
    wide.links["/w/l"] = std::string(HISH_LINK_MAX_TARGET, 'x');
    Expect(Resolve(wide, "/w/l/" + std::string(5000, 'y'), 1) == Failed(ENAMETOOLONG), "an over-long spliced path is ENAMETOOLONG");

    const char* target = nullptr;
    size_t length = 0;
    char bytes[HISH_LINK_MAGIC_LENGTH + 64];
    size_t size = HishLinkFormat("../a/b", 6, bytes);
    Expect(size == HISH_LINK_MAGIC_LENGTH + 6 && HishLinkParse(bytes, size, &target, &length) && length == 6 && !memcmp(target, "../a/b", 6), "a marker round-trips");
    Expect(!HishLinkParse("not a marker at all", 19, &target, &length), "other bytes are not a marker");
    Expect(!HishLinkParse(HISH_LINK_MAGIC, HISH_LINK_MAGIC_LENGTH, &target, &length), "a marker needs a target");
    char withNul[HISH_LINK_MAGIC_LENGTH + 4];
    memcpy(withNul, HISH_LINK_MAGIC, HISH_LINK_MAGIC_LENGTH);
    memcpy(withNul + HISH_LINK_MAGIC_LENGTH, "a\0b", 4);
    Expect(!HishLinkParse(withNul, sizeof(withNul), &target, &length), "a target with a NUL byte is rejected");
    Expect(HishLinkFormat("", 0, bytes) == 0 && HishLinkFormat("a\0b", 3, bytes) == 0, "an empty or NUL target cannot be written");
    std::string longest(HISH_LINK_MAX_TARGET, 'p'), tooLong(HISH_LINK_MAX_TARGET + 1, 'p');
    std::string buffer(HISH_LINK_MAGIC_LENGTH + tooLong.size(), '\0');
    const size_t longestSize = HishLinkFormat(longest.data(), longest.size(), &buffer[0]);
    Expect(longestSize == HISH_LINK_MAGIC_LENGTH + HISH_LINK_MAX_TARGET && HishLinkParse(buffer.data(), longestSize, &target, &length) && length == HISH_LINK_MAX_TARGET, "the longest allowed target is accepted");
    Expect(HishLinkFormat(tooLong.data(), tooLong.size(), &buffer[0]) == 0, "a target one byte too long is refused");

    puts(failures ? "LINK_CORE_FAIL" : "LINK_CORE_PASS");
    return failures ? 1 : 0;
}
