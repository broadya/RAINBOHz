// Tests for the Cypher statement splitter.
//
// No database is involved: the splitter is a pure function over a string, and
// keeping it testable without one is deliberate. Its behaviour has to match
// split_statements() in scripts/ladybug_ddl.py, because the Python tooling
// reports line numbers for the same files.

#include "rainbohz/rendering_engine/cypher_script.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using rainbohz::rendering_engine::splitStatements;

TEST(CypherScript, EmptySourceYieldsNothing) {
    EXPECT_TRUE(splitStatements("").empty());
    EXPECT_TRUE(splitStatements("   \n\n  \t ").empty());
}

TEST(CypherScript, CommentOnlySourceYieldsNothing) {
    EXPECT_TRUE(splitStatements("// just a comment\n/* and a block one */\n").empty());
}

TEST(CypherScript, StraySemicolonsProduceNoStatements) {
    EXPECT_TRUE(splitStatements(";;\n;").empty());
}

TEST(CypherScript, SplitsOnSemicolons) {
    const auto statements = splitStatements("CREATE (:A); CREATE (:B);");
    ASSERT_EQ(statements.size(), 2U);
    EXPECT_EQ(statements[0].text, "CREATE (:A)");
    EXPECT_EQ(statements[1].text, "CREATE (:B)");
}

TEST(CypherScript, TrailingStatementWithoutSemicolonIsKept) {
    const auto statements = splitStatements("CREATE (:A);\nCREATE (:B)\n");
    ASSERT_EQ(statements.size(), 2U);
    EXPECT_EQ(statements[1].text, "CREATE (:B)");
}

TEST(CypherScript, StripsLineComments) {
    const auto statements = splitStatements("// leading\nCREATE (:A); // trailing\n");
    ASSERT_EQ(statements.size(), 1U);
    EXPECT_EQ(statements[0].text, "CREATE (:A)");
}

TEST(CypherScript, StripsBlockComments) {
    const auto statements = splitStatements("CREATE /* mid */ (:A);");
    ASSERT_EQ(statements.size(), 1U);
    EXPECT_EQ(statements[0].text, "CREATE  (:A)");
}

TEST(CypherScript, SemicolonInsideStringLiteralIsNotATerminator) {
    const auto statements = splitStatements("CREATE (:Label {name: 'a;b'});");
    ASSERT_EQ(statements.size(), 1U);
    EXPECT_EQ(statements[0].text, "CREATE (:Label {name: 'a;b'})");
}

TEST(CypherScript, SemicolonInsideBacktickedIdentifierIsNotATerminator) {
    const auto statements = splitStatements("MATCH (n:`odd;name`) RETURN n;");
    ASSERT_EQ(statements.size(), 1U);
    EXPECT_EQ(statements[0].text, "MATCH (n:`odd;name`) RETURN n");
}

TEST(CypherScript, CommentMarkersInsideStringsAreNotComments) {
    const auto statements = splitStatements("CREATE (:Label {name: 'a // b'});");
    ASSERT_EQ(statements.size(), 1U);
    EXPECT_EQ(statements[0].text, "CREATE (:Label {name: 'a // b'})");
}

TEST(CypherScript, EscapedQuoteDoesNotCloseTheLiteral) {
    const auto statements = splitStatements(R"(CREATE (:Label {name: 'it\'s; here'});)");
    ASSERT_EQ(statements.size(), 1U);
    EXPECT_NE(statements[0].text.find("here"), std::string::npos);
}

// Line attribution is the whole reason statements are submitted one at a time.
TEST(CypherScript, AttributesStatementsToTheirSourceLines) {
    const std::string source = "// a comment on line 1\n" // 1
                               "CREATE (:A);\n"           // 2
                               "\n"                       // 3
                               "CREATE NODE TABLE B (\n"  // 4
                               "    id INT64,\n"          // 5
                               "    PRIMARY KEY(id)\n"    // 6
                               ");\n";                    // 7

    const auto statements = splitStatements(source);
    ASSERT_EQ(statements.size(), 2U);

    EXPECT_EQ(statements[0].startLine, 2U);
    EXPECT_EQ(statements[0].endLine, 2U);
    EXPECT_EQ(statements[0].lineSpan(), "line 2");

    EXPECT_EQ(statements[1].startLine, 4U);
    EXPECT_EQ(statements[1].endLine, 7U);
    EXPECT_EQ(statements[1].lineSpan(), "lines 4-7");
}

TEST(CypherScript, NewlinesInsideBlockCommentsStillCount) {
    const auto statements = splitStatements("/* one\ntwo\nthree */\nCREATE (:A);\n");
    ASSERT_EQ(statements.size(), 1U);
    EXPECT_EQ(statements[0].startLine, 4U);
}

// A missing semicolon merges two statements, which is exactly what Ladybug
// sees. The reported span then covers both, which is the useful diagnostic.
TEST(CypherScript, MissingSemicolonMergesStatementsIntoOneSpan) {
    const std::string source = "CREATE (:A)\n"   // 1, no terminator
                               "CREATE (:B);\n"; // 2

    const auto statements = splitStatements(source);
    ASSERT_EQ(statements.size(), 1U);
    EXPECT_EQ(statements[0].startLine, 1U);
    EXPECT_EQ(statements[0].endLine, 2U);
}

TEST(CypherScript, LabelsNodeTableDdl) {
    const auto statements = splitStatements("CREATE NODE TABLE Partial (\n id SERIAL\n);");
    ASSERT_EQ(statements.size(), 1U);
    EXPECT_EQ(statements[0].label(), "CREATE NODE TABLE Partial");
}

TEST(CypherScript, LabelsRelTableDdl) {
    const auto statements = splitStatements("CREATE REL TABLE HAS_LABEL(FROM Partial TO Label);");
    ASSERT_EQ(statements.size(), 1U);
    EXPECT_EQ(statements[0].label(), "CREATE REL TABLE HAS_LABEL");
}

TEST(CypherScript, LabelSkipsIfNotExists) {
    const auto statements = splitStatements("create node table if not exists Frame(x INT64);");
    ASSERT_EQ(statements.size(), 1U);
    EXPECT_EQ(statements[0].label(), "CREATE NODE TABLE Frame");
}

TEST(CypherScript, NonDdlIsLabelledByItsOwnText) {
    const auto statements = splitStatements("MATCH (p:Partial)\n  RETURN p;");
    ASSERT_EQ(statements.size(), 1U);
    EXPECT_EQ(statements[0].label(), "MATCH (p:Partial) RETURN p");
}

TEST(CypherScript, LongNonDdlLabelIsElided) {
    const std::string filler(200, 'x');
    const auto statements = splitStatements("RETURN '" + filler + "';");
    ASSERT_EQ(statements.size(), 1U);
    EXPECT_LE(statements[0].label().size(), 63U);
    EXPECT_TRUE(statements[0].label().ends_with("..."));
}

} // namespace
