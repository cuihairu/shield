// Unit tests for the PostgreSQL ? → $N placeholder rewriter.
//
// The rewriter is a pure SQL scanner (no libpq dependency) extracted into
// shield_db_rewrite.hpp so this test can link the exact same code the plugin
// uses. It pins the SQL-aware behaviour: string literals, quoted identifiers,
// line/block comments and dollar-quoted strings must keep their '?' verbatim,
// while placeholders outside those constructs become $1, $2, ...
#define BOOST_TEST_MODULE DbPgsqlRewrite
#include <boost/test/unit_test.hpp>

#include "shield_db_rewrite.hpp"

using shield::db::pgsql::rewrite_placeholders;

BOOST_AUTO_TEST_CASE(PlainPlaceholders) {
    BOOST_CHECK_EQUAL(rewrite_placeholders("SELECT * FROM t WHERE a = ?"),
                      "SELECT * FROM t WHERE a = $1");
    BOOST_CHECK_EQUAL(
        rewrite_placeholders("INSERT INTO t(a,b,c) VALUES (?,?,?)"),
        "INSERT INTO t(a,b,c) VALUES ($1,$2,$3)");
}

BOOST_AUTO_TEST_CASE(RepeatedAndSequential) {
    // Same placeholder value reused: each ? gets the next index (libpq
    // positional semantics), no dedup.
    BOOST_CHECK_EQUAL(rewrite_placeholders("a = ? OR b = ? OR c = ?"),
                      "a = $1 OR b = $2 OR c = $3");
}

BOOST_AUTO_TEST_CASE(SingleQuotedLiteralKeepsQuestion) {
    BOOST_CHECK_EQUAL(
        rewrite_placeholders("SELECT '?' AS q, * FROM t WHERE a = ?"),
        "SELECT '?' AS q, * FROM t WHERE a = $1");
    // Escaped quote inside a literal: '' stays inside the literal.
    BOOST_CHECK_EQUAL(
        rewrite_placeholders("SELECT 'it''s ?' AS s, a = ? FROM t"),
        "SELECT 'it''s ?' AS s, a = $1 FROM t");
    // E'...' escape string: backslash escapes and quotes.
    BOOST_CHECK_EQUAL(
        rewrite_placeholders("SELECT E'\\'?\\'' AS e, a = ? FROM t"),
        "SELECT E'\\'?\\'' AS e, a = $1 FROM t");
}

BOOST_AUTO_TEST_CASE(DoubleQuotedIdentifierKeepsQuestion) {
    BOOST_CHECK_EQUAL(
        rewrite_placeholders("SELECT \"weird?col\" FROM t WHERE a = ?"),
        "SELECT \"weird?col\" FROM t WHERE a = $1");
    BOOST_CHECK_EQUAL(
        rewrite_placeholders("SELECT \"a\"\"?\"\"b\" FROM t WHERE a = ?"),
        "SELECT \"a\"\"?\"\"b\" FROM t WHERE a = $1");
}

BOOST_AUTO_TEST_CASE(LineCommentKeepsQuestion) {
    BOOST_CHECK_EQUAL(
        rewrite_placeholders("SELECT a FROM t -- a?b\nWHERE a = ?"),
        "SELECT a FROM t -- a?b\nWHERE a = $1");
    // Comment to EOF.
    BOOST_CHECK_EQUAL(rewrite_placeholders("SELECT 1 -- trailing ?"),
                      "SELECT 1 -- trailing ?");
}

BOOST_AUTO_TEST_CASE(BlockCommentKeepsQuestion) {
    BOOST_CHECK_EQUAL(
        rewrite_placeholders("SELECT a FROM t /* a?b */ WHERE a = ?"),
        "SELECT a FROM t /* a?b */ WHERE a = $1");
    // Unterminated block comment: consume to EOF.
    BOOST_CHECK_EQUAL(rewrite_placeholders("SELECT 1 /* ?"), "SELECT 1 /* ?");
}

BOOST_AUTO_TEST_CASE(DollarQuotedKeepsQuestion) {
    BOOST_CHECK_EQUAL(
        rewrite_placeholders("SELECT $body$ a?b $body$ FROM t WHERE a = ?"),
        "SELECT $body$ a?b $body$ FROM t WHERE a = $1");
    BOOST_CHECK_EQUAL(
        rewrite_placeholders("SELECT $$ a?b $$ FROM t WHERE a = ?"),
        "SELECT $$ a?b $$ FROM t WHERE a = $1");
    // Unterminated dollar quote: consume to EOF.
    BOOST_CHECK_EQUAL(rewrite_placeholders("SELECT $x$ a?b"), "SELECT $x$ a?b");
}

BOOST_AUTO_TEST_CASE(MixedConstructs) {
    const char* sql =
        "INSERT INTO t(a,b,c) VALUES ('?', ?) -- ?\n"
        "/* ? */ "
        "SELECT \"?\", ?";
    const char* expected =
        "INSERT INTO t(a,b,c) VALUES ('?', $1) -- ?\n"
        "/* ? */ "
        "SELECT \"?\", $2";
    BOOST_CHECK_EQUAL(rewrite_placeholders(sql), expected);
}

BOOST_AUTO_TEST_CASE(NullInput) {
    BOOST_CHECK_EQUAL(rewrite_placeholders(nullptr), "");
}
