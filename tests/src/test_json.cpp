#include <gtest/gtest.h>

#include "ignis/core/json_parse.h"

#include <string>

using namespace ignis;

namespace {

std::string nested_arrays(int depth) {
    return std::string(static_cast<size_t>(depth), '[') +
           std::string(static_cast<size_t>(depth), ']');
}

std::string nested_mixed(int depth) {
    std::string s;
    for (int i = 0; i < depth; ++i) s += (i % 2 == 0) ? "{\"a\":" : "[";
    s += "1";
    for (int i = depth - 1; i >= 0; --i) s += (i % 2 == 0) ? "}" : "]";
    return s;
}

} // namespace

TEST(JsonParse, ParsesValidDocuments) {
    auto r = parse_json_bounded(R"({"a": [1, 2.5, "x", true, null], "b": {"c": "d"}})");
    ASSERT_TRUE(r.ok()) << r.error();
    const auto& j = r.value();
    EXPECT_EQ(j["a"][0].get<int>(), 1);
    EXPECT_DOUBLE_EQ(j["a"][1].get<double>(), 2.5);
    EXPECT_TRUE(j["a"][3].get<bool>());
    EXPECT_TRUE(j["a"][4].is_null());
    EXPECT_EQ(j["b"]["c"].get<std::string>(), "d");
}

TEST(JsonParse, AllowsScalarTopLevel) {
    // parse_json_bounded itself does not require an object; scene loading does.
    EXPECT_TRUE(parse_json_bounded("42").ok());
    EXPECT_TRUE(parse_json_bounded("\"text\"").ok());
    EXPECT_TRUE(parse_json_bounded("null").ok());
    EXPECT_TRUE(parse_json_bounded("true").ok());
    EXPECT_TRUE(parse_json_bounded("[]").ok());
}

TEST(JsonParse, RejectsMalformedWithMessage) {
    for (const char* bad : {"", "{", "{\"a\":1", "[1,]", "{'a':1}", "{\"a\":1} x", "nul", "[1 2]"}) {
        auto r = parse_json_bounded(bad);
        EXPECT_FALSE(r.ok()) << "input: " << bad;
        EXPECT_FALSE(r.error().empty()) << "input: " << bad;
    }
}

TEST(JsonParse, DefaultDepthCapRejectsDeepNesting) {
    // The depth callback sees the root container at depth 0, so max_depth=64
    // allows 65 nested containers and rejects 66.
    EXPECT_TRUE(parse_json_bounded(nested_arrays(65)).ok());
    auto deep = parse_json_bounded(nested_arrays(66));
    ASSERT_FALSE(deep.ok());
    EXPECT_NE(deep.error().find("nesting"), std::string::npos) << deep.error();
    EXPECT_FALSE(parse_json_bounded(nested_arrays(100000)).ok());
}

TEST(JsonParse, CustomDepthCap) {
    EXPECT_TRUE(parse_json_bounded(nested_arrays(4), 3).ok());
    EXPECT_FALSE(parse_json_bounded(nested_arrays(5), 3).ok());
}

TEST(JsonParse, MixedObjectsAndArraysCountTowardDepth) {
    EXPECT_TRUE(parse_json_bounded(nested_mixed(10)).ok());
    EXPECT_FALSE(parse_json_bounded(nested_mixed(200)).ok());
}

TEST(JsonParse, FailureNeverThrows) {
    EXPECT_NO_THROW({
        const auto r = parse_json_bounded(std::string(1000, '['));
        EXPECT_FALSE(r.ok());
    });
}

TEST(JsonParse, WideShallowDocumentWithinDepthCapParses) {
    // 200k sibling elements at depth 1: the depth cap must reject depth,
    // not width.
    std::string s = "[";
    for (int i = 0; i < 200000; ++i) {
        if (i != 0) s += ",";
        s += "1";
    }
    s += "]";
    auto r = parse_json_bounded(s);
    ASSERT_TRUE(r.ok()) << r.error();
    EXPECT_EQ(r.value().size(), 200000u);
}
