// [SHIELD_NET] Connection-time IP blocklist
//
// Header-only on purpose: config-time validation parses the same entries the
// accept path matches, but shield_config sits *below* shield_net in the
// dependency graph, so the parser cannot live in the net library. The type
// only needs boost::asio::ip (header-only), which keeps this includable from
// both without a link cycle.
#pragma once

#include <array>
#include <boost/asio/ip/address.hpp>
#include <charconv>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace shield::net {

/// @brief One parsed blocklist rule: an address plus a prefix length.
///        Exposed at namespace scope so configuration validation reuses the
///        accept path's parser instead of re-implementing (and drifting from)
///        the accepted syntax.
struct Rule {
    boost::asio::ip::address addr;
    /// Number of leading bits that must match. Equal to the address width
    /// (32 for v4, 128 for v6) for a plain exact-match entry.
    unsigned prefix_len = 0;
};

/// @brief Address width in bits: 32 for IPv4, 128 for IPv6.
inline unsigned address_width_bits(const boost::asio::ip::address& addr) {
    return addr.is_v4() ? 32u : 128u;
}

namespace detail {

/// @brief Strip surrounding spaces and tabs.
inline std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

}  // namespace detail

/// @brief Parse a single blocklist entry ("203.0.113.7", "198.51.100.0/24",
///        "2001:db8::/32"). Surrounding whitespace is ignored; an empty or
///        whitespace-only entry is a no-op that returns true with a
///        zero-initialized @p out. On failure @p error names the problem.
inline bool parse_blocklist_entry(std::string_view raw, Rule* out,
                                  std::string* error) {
    const std::string_view text = detail::trim(raw);
    if (text.empty()) {
        *out = Rule{};
        return true;
    }

    std::string_view addr_part = text;
    std::string_view prefix_part;
    const std::size_t slash = text.find('/');
    if (slash != std::string_view::npos) {
        addr_part = text.substr(0, slash);
        prefix_part = text.substr(slash + 1);
    }

    boost::system::error_code ec;
    const auto addr = boost::asio::ip::make_address(addr_part, ec);
    if (ec) {
        if (error) {
            *error = "not an IP address: " + std::string(text);
        }
        return false;
    }

    unsigned prefix_len = address_width_bits(addr);
    if (!prefix_part.empty()) {
        unsigned parsed = 0;
        const auto* first = prefix_part.data();
        const auto* last = first + prefix_part.size();
        const auto res = std::from_chars(first, last, parsed);
        if (res.ec != std::errc{} || res.ptr != last) {
            if (error) {
                *error = "prefix length is not a number: " + std::string(text);
            }
            return false;
        }
        if (parsed > address_width_bits(addr)) {
            if (error) {
                *error = "prefix length out of range: " + std::string(text);
            }
            return false;
        }
        prefix_len = parsed;
    }

    out->addr = addr;
    out->prefix_len = prefix_len;
    return true;
}

namespace detail {

/// @brief Compare the leading @p bits of two equal-width address byte arrays.
template <typename ByteT, std::size_t N>
inline bool prefix_matches_bytes(const std::array<ByteT, N>& lhs,
                                 const std::array<ByteT, N>& rhs,
                                 unsigned bits) {
    const unsigned full_bytes = bits / 8;
    for (unsigned i = 0; i < full_bytes; ++i) {
        if (lhs[i] != rhs[i]) {
            return false;
        }
    }
    const unsigned remaining_bits = bits % 8;
    if (remaining_bits == 0) {
        return true;
    }
    const ByteT mask = static_cast<ByteT>(0xFFu << (8 - remaining_bits));
    return (lhs[full_bytes] & mask) == (rhs[full_bytes] & mask);
}

/// @brief True when @p addr's leading @p rule.prefix_len bits equal the rule's.
///
/// Both sides are converted within their own family. Widening v4 into the
/// v4-mapped v6 layout would shift the address to bytes 12..15, so a /32 rule
/// would end up comparing the "::ffff" marker instead of the address and
/// would match every v4 peer. Rules and lookups are family-separated
/// elsewhere, so the two sides here always agree.
inline bool prefix_matches(const Rule& rule,
                           const boost::asio::ip::address& addr) {
    if (rule.addr.is_v4()) {
        return prefix_matches_bytes(rule.addr.to_v4().to_bytes(),
                                    addr.to_v4().to_bytes(), rule.prefix_len);
    }
    return prefix_matches_bytes(rule.addr.to_v6().to_bytes(),
                                addr.to_v6().to_bytes(), rule.prefix_len);
}

}  // namespace detail

/// @brief Deny list of remote addresses evaluated at accept time.
///
/// Complements the per-connection ingress rate limit (TokenBucket, see
/// session.hpp): the rate limit caps *volume* from a noisy-but-legitimate
/// client, while the blocklist rejects specific sources outright before a
/// session object is ever allocated.
///
/// Entries are a plain address ("203.0.113.7", "::1") or a CIDR block
/// ("198.51.100.0/24", "2001:db8::/32"). IPv4 and IPv6 are both supported and
/// kept in separate tables, so a v4 address is never tested against a v6 rule.
///
/// Rules are parsed once at install time, which is what lets configuration
/// validation reject a typo at startup instead of silently dropping the deny
/// rule during an incident. Lookup is a linear scan over the (small, bounded)
/// rule set, keeping the accept path allocation-free.
class IpBlocklist {
public:
    /// @brief Parse and install rules from config text. Returns false and
    ///        leaves the previous rule set untouched when any entry is
    ///        malformed; @p error then names the offending entry.
    bool set_rules(const std::vector<std::string>& entries,
                   std::string* error = nullptr) {
        // Parse into scratch vectors first: a malformed entry must not leave
        // the listener holding a half-applied rule set.
        std::vector<Rule> v4;
        std::vector<Rule> v6;
        for (const auto& entry : entries) {
            // A blank list element is a no-op, not a rule: installing the
            // parser's zero-initialized Rule would add an unspecified
            // address that silently matches nothing while inflating size().
            if (detail::trim(entry).empty()) {
                continue;
            }
            Rule rule;
            if (!parse_blocklist_entry(entry, &rule, error)) {
                return false;
            }
            if (rule.addr.is_v4()) {
                v4.push_back(rule);
            } else {
                v6.push_back(rule);
            }
        }
        std::unique_lock lock(mutex_);
        v4_rules_ = std::move(v4);
        v6_rules_ = std::move(v6);
        return true;
    }

    /// @brief Drop all rules (the blocklist becomes a no-op).
    void clear() {
        std::unique_lock lock(mutex_);
        v4_rules_.clear();
        v6_rules_.clear();
    }

    /// @brief True when no rule is installed.
    bool empty() const {
        std::shared_lock lock(mutex_);
        return v4_rules_.empty() && v6_rules_.empty();
    }

    /// @brief Number of installed rules (v4 + v6 combined).
    std::size_t size() const {
        std::shared_lock lock(mutex_);
        return v4_rules_.size() + v6_rules_.size();
    }

    /// @brief True when @p address matches an installed rule. A v4 address is
    ///        only tested against v4 rules and vice versa.
    bool blocked(const boost::asio::ip::address& address) const {
        if (!address.is_v4() && !address.is_v6()) {
            return false;
        }
        std::shared_lock lock(mutex_);
        const auto& rules = address.is_v4() ? v4_rules_ : v6_rules_;
        for (const auto& rule : rules) {
            if (detail::prefix_matches(rule, address)) {
                return true;
            }
        }
        return false;
    }

    /// @brief Convenience overload for the textual form. Unparseable text
    ///        never matches (it cannot be an installed rule either).
    bool blocked(std::string_view text) const {
        boost::system::error_code ec;
        const auto addr = boost::asio::ip::make_address(text, ec);
        if (ec) {
            return false;
        }
        return blocked(addr);
    }

private:
    mutable std::shared_mutex mutex_;
    std::vector<Rule> v4_rules_;
    std::vector<Rule> v6_rules_;
};

}  // namespace shield::net
