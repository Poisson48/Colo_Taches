#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include <optional>

#include "core/pairing.h"

static int g_tests = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    ++g_tests; \
    if (!(cond)) { \
        ++g_failures; \
        std::cerr << "FAIL [" << __LINE__ << "]: " #cond "\n"; \
    } \
} while(0)

#define CHECK_EQ(a, b) do { \
    ++g_tests; \
    if ((a) != (b)) { \
        ++g_failures; \
        std::cerr << "FAIL [" << __LINE__ << "]: " #a " != " #b "\n"; \
    } \
} while(0)

static std::vector<uint8_t> makeKey(uint8_t fill = 0x42)
{
    return std::vector<uint8_t>(32, fill);
}

void testRoundTripUri()
{
    std::string listId = "abc123";
    auto key = makeKey(0x7f);
    std::string title = "Liste de courses";

    std::string uri = core::buildJoinUri(listId, key, title);

    // Check scheme prefix
    CHECK(uri.substr(0, 20) == "colotaches://join/1/");

    auto info = core::parseJoinUri(uri);
    CHECK(info.has_value());
    if (info) {
        CHECK_EQ(info->listId, listId);
        CHECK_EQ(info->key, key);
        CHECK_EQ(info->title, title);
    }
}

void testRoundTripUriSpecialChars()
{
    std::string listId = "list-with-dashes";
    auto key = makeKey(0x01);
    std::string title = "Café & épicerie (spéciale)";

    std::string uri = core::buildJoinUri(listId, key, title);
    auto info = core::parseJoinUri(uri);
    CHECK(info.has_value());
    if (info) {
        CHECK_EQ(info->title, title);
        CHECK_EQ(info->key, key);
        CHECK_EQ(info->listId, listId);
    }
}

void testMalformedUris()
{
    // Wrong scheme
    CHECK(!core::parseJoinUri("http://example.com").has_value());
    CHECK(!core::parseJoinUri("").has_value());
    CHECK(!core::parseJoinUri("colotaches://join/1/").has_value());
    // Missing title segment
    CHECK(!core::parseJoinUri("colotaches://join/1/listid/keybase64url").has_value());
    // Wrong key length (after b64url decode, should be 32 bytes)
    // "YQ==" decodes to 1 byte 'a' — invalid
    CHECK(!core::parseJoinUri("colotaches://join/1/listid/YQ/title").has_value());
}

void testKeyEncoding()
{
    // Ensure key survives round-trip for boundary values
    auto key1 = makeKey(0x00);
    auto key2 = makeKey(0xFF);

    std::string uri1 = core::buildJoinUri("id1", key1, "T1");
    std::string uri2 = core::buildJoinUri("id2", key2, "T2");

    auto info1 = core::parseJoinUri(uri1);
    auto info2 = core::parseJoinUri(uri2);

    CHECK(info1.has_value());
    CHECK(info2.has_value());
    if (info1) CHECK_EQ(info1->key, key1);
    if (info2) CHECK_EQ(info2->key, key2);
}

int main()
{
    testRoundTripUri();
    testRoundTripUriSpecialChars();
    testMalformedUris();
    testKeyEncoding();

    if (g_failures == 0) {
        std::cout << "All " << g_tests << " pairing tests passed.\n";
        return 0;
    } else {
        std::cout << g_failures << "/" << g_tests << " pairing tests FAILED.\n";
        return 1;
    }
}
