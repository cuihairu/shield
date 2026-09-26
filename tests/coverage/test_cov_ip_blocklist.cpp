#define BOOST_TEST_MODULE CovIpBlocklist
#include <boost/asio/ip/address.hpp>
#include <boost/test/unit_test.hpp>
#include <string>
#include <vector>

#include "shield/net/ip_blocklist.hpp"

namespace {

using shield::net::IpBlocklist;
using shield::net::parse_blocklist_entry;
using shield::net::Rule;

}  // namespace

BOOST_AUTO_TEST_SUITE(IpBlocklistCoverage)

// A default-constructed blocklist denies nothing and reports itself empty.
BOOST_AUTO_TEST_CASE(EmptyBlocklistDeniesNothing) {
    IpBlocklist list;
    BOOST_CHECK(list.empty());
    BOOST_CHECK_EQUAL(list.size(), 0u);
    BOOST_CHECK(!list.blocked("127.0.0.1"));
    BOOST_CHECK(!list.blocked("203.0.113.7"));
    BOOST_CHECK(!list.blocked("::1"));
    // Unparseable text can never match an installed rule.
    BOOST_CHECK(!list.blocked("not-an-ip"));
}

// An empty entry list installs cleanly and stays a no-op.
BOOST_AUTO_TEST_CASE(EmptyEntryListIsNoOp) {
    IpBlocklist list;
    BOOST_CHECK(list.set_rules({}));
    BOOST_CHECK(list.empty());
    BOOST_CHECK(!list.blocked("127.0.0.1"));
}

// A plain address denies exactly that address, not its neighbours.
BOOST_AUTO_TEST_CASE(ExactAddressRule) {
    IpBlocklist list;
    BOOST_REQUIRE(list.set_rules({"203.0.113.7"}));
    BOOST_CHECK(!list.empty());
    BOOST_CHECK_EQUAL(list.size(), 1u);
    BOOST_CHECK(list.blocked("203.0.113.7"));
    BOOST_CHECK(!list.blocked("203.0.113.6"));
    BOOST_CHECK(!list.blocked("203.0.113.8"));
    // A v4 rule must not leak onto the v6 family.
    BOOST_CHECK(!list.blocked("::1"));
    BOOST_CHECK(!list.blocked("2001:db8::1"));
}

// A /32 rule is the same as an exact address; /0 would deny everything.
BOOST_AUTO_TEST_CASE(Ipv4PrefixLengths) {
    IpBlocklist exact;
    BOOST_REQUIRE(exact.set_rules({"203.0.113.7/32"}));
    BOOST_CHECK(exact.blocked("203.0.113.7"));
    BOOST_CHECK(!exact.blocked("203.0.113.8"));

    IpBlocklist all;
    BOOST_REQUIRE(all.set_rules({"0.0.0.0/0"}));
    BOOST_CHECK(all.blocked("127.0.0.1"));
    BOOST_CHECK(all.blocked("203.0.113.7"));
    // A /0 v4 rule still does not reach the v6 family.
    BOOST_CHECK(!all.blocked("::1"));
}

// The documented example: a /24 block covers its whole range exactly.
BOOST_AUTO_TEST_CASE(Ipv4CidrBlock) {
    IpBlocklist list;
    BOOST_REQUIRE(list.set_rules({"198.51.100.0/24"}));
    BOOST_CHECK(list.blocked("198.51.100.0"));
    BOOST_CHECK(list.blocked("198.51.100.1"));
    BOOST_CHECK(list.blocked("198.51.100.255"));
    // Just outside the block on either side.
    BOOST_CHECK(!list.blocked("198.51.99.255"));
    BOOST_CHECK(!list.blocked("198.51.101.0"));
}

// A prefix that is not a whole number of bytes exercises the partial-byte
// mask (e.g. /20 stops inside the third byte).
BOOST_AUTO_TEST_CASE(Ipv4NonByteAlignedPrefix) {
    IpBlocklist list;
    BOOST_REQUIRE(list.set_rules({"10.0.0.0/20"}));
    // 10.0.0.0 - 10.0.15.255 are inside; 10.0.16.x is not.
    BOOST_CHECK(list.blocked("10.0.0.0"));
    BOOST_CHECK(list.blocked("10.0.15.255"));
    BOOST_CHECK(!list.blocked("10.0.16.0"));
    BOOST_CHECK(!list.blocked("11.0.0.0"));
}

// IPv6 rules are kept in their own table and never match v4 addresses.
BOOST_AUTO_TEST_CASE(Ipv6RulesAreFamilyScoped) {
    IpBlocklist list;
    BOOST_REQUIRE(list.set_rules({"::1", "2001:db8::/32"}));
    BOOST_CHECK_EQUAL(list.size(), 2u);
    BOOST_CHECK(list.blocked("::1"));
    BOOST_CHECK(list.blocked("2001:db8::1"));
    BOOST_CHECK(list.blocked("2001:db8:ffff::9"));
    BOOST_CHECK(!list.blocked("2001:db9::1"));
    BOOST_CHECK(!list.blocked("127.0.0.1"));
    BOOST_CHECK(!list.blocked("203.0.113.7"));
}

// A /128 IPv6 rule is an exact match; /64 is a half-prefix.
BOOST_AUTO_TEST_CASE(Ipv6PrefixLengths) {
    IpBlocklist exact;
    BOOST_REQUIRE(exact.set_rules({"2001:db8::1/128"}));
    BOOST_CHECK(exact.blocked("2001:db8::1"));
    BOOST_CHECK(!exact.blocked("2001:db8::2"));

    IpBlocklist half;
    BOOST_REQUIRE(half.set_rules({"2001:db8::/64"}));
    // A /64 pins the first four groups (2001:0db8:0000:0000), so everything
    // from 2001:db8:: up to 2001:db8:0:ffff:ffff:ffff:ffff:ffff is inside.
    BOOST_CHECK(half.blocked("2001:db8::1"));
    BOOST_CHECK(half.blocked("2001:db8:0:0:ffff::1"));
    BOOST_CHECK(half.blocked("2001:db8::ffff:ffff:ffff:ffff"));
    // One group further out leaves the prefix.
    BOOST_CHECK(!half.blocked("2001:db8:1::1"));
    BOOST_CHECK(!half.blocked("2001:db9::1"));
}

// Several rules coexist; each denies its own range.
BOOST_AUTO_TEST_CASE(MultipleRulesCoexist) {
    IpBlocklist list;
    BOOST_REQUIRE(list.set_rules({"203.0.113.7", "198.51.100.0/24", "::1"}));
    BOOST_CHECK_EQUAL(list.size(), 3u);
    BOOST_CHECK(list.blocked("203.0.113.7"));
    BOOST_CHECK(list.blocked("198.51.100.42"));
    BOOST_CHECK(list.blocked("::1"));
    BOOST_CHECK(!list.blocked("8.8.8.8"));
}

// Surrounding whitespace is tolerated, and a blank entry is a no-op rather
// than an error (hand-edited YAML lists pick up blank lines easily).
BOOST_AUTO_TEST_CASE(WhitespaceAndBlankEntriesAreTolerated) {
    IpBlocklist list;
    BOOST_REQUIRE(list.set_rules({"  203.0.113.7  ", "", "   "}));
    BOOST_CHECK_EQUAL(list.size(), 1u);
    BOOST_CHECK(list.blocked("203.0.113.7"));
}

// A malformed entry is rejected and the previous rule set survives intact:
// a typo must not silently widen access.
BOOST_AUTO_TEST_CASE(MalformedEntryKeepsPreviousRules) {
    IpBlocklist list;
    BOOST_REQUIRE(list.set_rules({"203.0.113.7"}));

    std::string error;
    BOOST_CHECK(!list.set_rules({"198.51.100.0/24", "not-an-ip"}, &error));
    BOOST_CHECK(error.find("not an IP address") != std::string::npos);
    // The original rule is still in force and the new one was not applied.
    BOOST_CHECK_EQUAL(list.size(), 1u);
    BOOST_CHECK(list.blocked("203.0.113.7"));
    BOOST_CHECK(!list.blocked("198.51.100.1"));
}

// A non-numeric prefix is rejected with a message that names the entry.
BOOST_AUTO_TEST_CASE(NonNumericPrefixRejected) {
    IpBlocklist list;
    std::string error;
    BOOST_CHECK(!list.set_rules({"203.0.113.0/abc"}, &error));
    BOOST_CHECK(error.find("prefix length is not a number") !=
                std::string::npos);
    BOOST_CHECK(list.empty());
}

// Trailing junk after the digits is not silently truncated ("/24x").
BOOST_AUTO_TEST_CASE(PrefixWithTrailingJunkRejected) {
    IpBlocklist list;
    std::string error;
    BOOST_CHECK(!list.set_rules({"203.0.113.0/24x"}, &error));
    BOOST_CHECK(error.find("prefix length is not a number") !=
                std::string::npos);
}

// A /33 on an IPv4 address is out of range; a /129 on IPv6 likewise.
BOOST_AUTO_TEST_CASE(PrefixOutOfRangeRejected) {
    IpBlocklist list;
    std::string error;
    BOOST_CHECK(!list.set_rules({"203.0.113.0/33"}, &error));
    BOOST_CHECK(error.find("prefix length out of range") != std::string::npos);

    BOOST_CHECK(!list.set_rules({"2001:db8::/129"}, &error));
    BOOST_CHECK(error.find("prefix length out of range") != std::string::npos);
    BOOST_CHECK(list.empty());
}

// clear() drops every rule and returns the list to a no-op.
BOOST_AUTO_TEST_CASE(ClearRemovesAllRules) {
    IpBlocklist list;
    BOOST_REQUIRE(list.set_rules({"203.0.113.7", "::1"}));
    BOOST_CHECK_EQUAL(list.size(), 2u);
    list.clear();
    BOOST_CHECK(list.empty());
    BOOST_CHECK_EQUAL(list.size(), 0u);
    BOOST_CHECK(!list.blocked("203.0.113.7"));
    BOOST_CHECK(!list.blocked("::1"));
}

// Installing a new rule set replaces the previous one (not merges).
BOOST_AUTO_TEST_CASE(SetRulesReplacesPreviousSet) {
    IpBlocklist list;
    BOOST_REQUIRE(list.set_rules({"203.0.113.7"}));
    BOOST_REQUIRE(list.set_rules({"198.51.100.0/24"}));
    BOOST_CHECK_EQUAL(list.size(), 1u);
    BOOST_CHECK(!list.blocked("203.0.113.7"));
    BOOST_CHECK(list.blocked("198.51.100.1"));
}

// The standalone parser reports the parsed address and prefix width.
BOOST_AUTO_TEST_CASE(ParserReportsAddressAndPrefix) {
    Rule rule;
    std::string error;

    BOOST_REQUIRE(parse_blocklist_entry("203.0.113.7", &rule, &error));
    BOOST_CHECK(rule.addr.is_v4());
    BOOST_CHECK_EQUAL(rule.prefix_len, 32u);
    BOOST_CHECK_EQUAL(rule.addr.to_string(), "203.0.113.7");

    BOOST_REQUIRE(parse_blocklist_entry("198.51.100.0/24", &rule, &error));
    BOOST_CHECK_EQUAL(rule.prefix_len, 24u);

    BOOST_REQUIRE(parse_blocklist_entry("2001:db8::/32", &rule, &error));
    BOOST_CHECK(rule.addr.is_v6());
    BOOST_CHECK_EQUAL(rule.prefix_len, 32u);

    // A /0 prefix is legal and means "every address in this family".
    BOOST_REQUIRE(parse_blocklist_entry("0.0.0.0/0", &rule, &error));
    BOOST_CHECK_EQUAL(rule.prefix_len, 0u);
}

// The textual lookup overload ignores addresses it cannot parse, matching the
// "an unparseable address was never installable" invariant.
BOOST_AUTO_TEST_CASE(TextualLookupIgnoresGarbage) {
    IpBlocklist list;
    BOOST_REQUIRE(list.set_rules({"0.0.0.0/0"}));
    BOOST_CHECK(list.blocked("127.0.0.1"));
    BOOST_CHECK(!list.blocked("garbage"));
    BOOST_CHECK(!list.blocked(""));
    BOOST_CHECK(!list.blocked("999.1.1.1"));
}

BOOST_AUTO_TEST_SUITE_END()
