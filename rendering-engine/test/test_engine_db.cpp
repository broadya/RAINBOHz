// SPDX-License-Identifier: Apache-2.0

// Tests for the in-memory engine-db wrapper.
//
// SCAFFOLDING. These assert the properties the scaffolding is meant to
// establish -- that an embedded in-memory Ladybug instance opens, accepts DDL
// and DML, reports errors as exceptions, and is genuinely process-local -- and
// nothing about the engine itself, which does not exist yet.

#include "rainbohz/rendering_engine/engine_db.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {

using rainbohz::rendering_engine::Census;
using rainbohz::rendering_engine::DbRow;
using rainbohz::rendering_engine::DbValue;
using rainbohz::rendering_engine::EngineDb;
using rainbohz::rendering_engine::EngineDbError;
using rainbohz::rendering_engine::optionalDouble;
using rainbohz::rendering_engine::optionalInt64;
using rainbohz::rendering_engine::optionalString;
using rainbohz::rendering_engine::requireInt64;
using rainbohz::rendering_engine::Transaction;

TEST(EngineDb, ReportsLinkedLadybugVersion) {
    EXPECT_FALSE(EngineDb::ladybugVersion().empty());
}

TEST(EngineDb, OpensInMemoryByDefault) {
    EXPECT_NO_THROW({ EngineDb db; });
}

TEST(EngineDb, AcceptsSchemaAndData) {
    EngineDb db;
    db.execute("CREATE NODE TABLE Partial(id INT64, PRIMARY KEY(id));");
    db.execute("CREATE (:Partial {id: 7});");

    const std::string result = db.queryToString("MATCH (p:Partial) RETURN p.id;");
    EXPECT_NE(result.find('7'), std::string::npos);
}

TEST(EngineDb, InvalidCypherThrows) {
    EngineDb db;
    EXPECT_THROW(db.execute("THIS IS NOT CYPHER"), EngineDbError);
}

TEST(EngineDb, QueryAgainstMissingTableThrows) {
    EngineDb db;
    EXPECT_THROW(db.queryToString("MATCH (n:NoSuchTable) RETURN n;"), EngineDbError);
}

// The in-memory mode is load-bearing: engine-db is transient in the deployed configuration,
// and two engine instances must not share state through the filesystem.
TEST(EngineDb, InstancesAreIndependent) {
    EngineDb first;
    first.execute("CREATE NODE TABLE OnlyInFirst(id INT64, PRIMARY KEY(id));");

    EngineDb second;
    EXPECT_THROW(second.queryToString("MATCH (n:OnlyInFirst) RETURN n;"), EngineDbError);
}

TEST(EngineDb, InMemoryWritesNothingToDisk) {
    const auto before = std::filesystem::current_path();
    {
        EngineDb db;
        db.execute("CREATE NODE TABLE Ephemeral(id INT64, PRIMARY KEY(id));");
        db.execute("CREATE (:Ephemeral {id: 1});");
    }
    EXPECT_FALSE(std::filesystem::exists(before / ":memory:"));
}

// The row census. It has to work on a database whose schema has not been
// applied yet, because the alternative is that the one report that says
// whether anything loaded cannot be produced when nothing did.
TEST(EngineDb, CensusOfAnEmptyCatalogIsEmpty) {
    EngineDb db;
    const Census census = db.census();
    EXPECT_TRUE(census.empty());
    EXPECT_EQ(census.totalNodes, 0);
    EXPECT_EQ(census.totalRelationships, 0);
    EXPECT_TRUE(census.nodes.empty());
    EXPECT_TRUE(census.relationships.empty());
}

// Declared but unpopulated tables are omitted. A schema declares far more
// tables than any one score fills, so listing the empty ones would bury the
// filled ones.
TEST(EngineDb, CensusOmitsDeclaredButEmptyTables) {
    EngineDb db;
    db.execute("CREATE NODE TABLE Partial(id INT64, PRIMARY KEY(id));");
    db.execute("CREATE NODE TABLE Label(id INT64, PRIMARY KEY(id));");
    db.execute("CREATE REL TABLE HAS_LABEL(FROM Partial TO Label);");

    const Census census = db.census();
    EXPECT_TRUE(census.empty());
    EXPECT_TRUE(census.nodes.empty());
    EXPECT_TRUE(census.relationships.empty());
}

TEST(EngineDb, CensusCountsRowsPerTable) {
    EngineDb db;
    db.execute("CREATE NODE TABLE Partial(id INT64, PRIMARY KEY(id));");
    db.execute("CREATE NODE TABLE Label(id INT64, PRIMARY KEY(id));");
    db.execute("CREATE REL TABLE HAS_LABEL(FROM Partial TO Label);");
    db.execute("CREATE (:Partial {id: 1});");
    db.execute("CREATE (:Partial {id: 2});");
    db.execute("CREATE (:Label {id: 1});");
    db.execute("MATCH (p:Partial), (l:Label) CREATE (p)-[:HAS_LABEL]->(l);");

    const Census census = db.census();
    EXPECT_FALSE(census.empty());
    EXPECT_EQ(census.totalNodes, 3);
    EXPECT_EQ(census.totalRelationships, 2);

    // Sorted by name, so Label precedes Partial.
    ASSERT_EQ(census.nodes.size(), 2U);
    EXPECT_EQ(census.nodes[0].first, "Label");
    EXPECT_EQ(census.nodes[0].second, 1);
    EXPECT_EQ(census.nodes[1].first, "Partial");
    EXPECT_EQ(census.nodes[1].second, 2);

    ASSERT_EQ(census.relationships.size(), 1U);
    EXPECT_EQ(census.relationships[0].first, "HAS_LABEL");
    EXPECT_EQ(census.relationships[0].second, 2);
}

// Statements can all apply and still store nothing: a CREATE whose MATCH
// found no rows is a success that does no work. This is the case the census
// exists to make visible, and the loader report alone cannot.
TEST(EngineDb, CensusDistinguishesAppliedFromStored) {
    EngineDb db;
    db.execute("CREATE NODE TABLE Partial(id INT64, PRIMARY KEY(id));");
    db.execute("CREATE NODE TABLE Label(id INT64, PRIMARY KEY(id));");
    db.execute("CREATE REL TABLE HAS_LABEL(FROM Partial TO Label);");
    // Accepted by the database; matches nothing, so creates nothing.
    db.execute("MATCH (p:Partial), (l:Label) CREATE (p)-[:HAS_LABEL]->(l);");

    EXPECT_TRUE(db.census().empty());
}

// ---------------------------------------------------------------------------
// Typed queries and transactions
// ---------------------------------------------------------------------------

/// Counts the rows of table Point.
///
/// @param db  A database with a Point node table.
/// @return The number of Point nodes.
std::int64_t countPoints(EngineDb& db) {
    return requireInt64(db.query("MATCH (p:Point) RETURN count(p);").at(0).at(0));
}

TEST(EngineDb, QueryReturnsTypedValues) {
    EngineDb db;
    db.execute("CREATE NODE TABLE Point(id SERIAL, t DOUBLE, f FLOAT, k INT8, n STRING, b BOOL, "
               "PRIMARY KEY(id));");
    db.execute("CREATE (:Point {t: 1.5, f: 0.25, k: 3, n: 'x', b: true});");
    db.execute("CREATE (:Point {});");

    const std::vector<DbRow> rows =
        db.query("MATCH (p:Point) RETURN p.id, p.t, p.f, p.k, p.n, p.b ORDER BY p.id;");
    ASSERT_EQ(rows.size(), 2U);

    EXPECT_EQ(requireInt64(rows[0][0]), 0);
    EXPECT_EQ(optionalDouble(rows[0][1]), 1.5);
    EXPECT_EQ(optionalDouble(rows[0][2]), 0.25); // FLOAT, widened exactly
    EXPECT_EQ(optionalInt64(rows[0][3]), 3);     // INT8, widened
    EXPECT_EQ(optionalString(rows[0][4]), "x");
    EXPECT_TRUE(std::get<bool>(rows[0][5]));

    // Unset properties read back as NULL.
    EXPECT_EQ(optionalDouble(rows[1][1]), std::nullopt);
    EXPECT_EQ(optionalString(rows[1][4]), std::nullopt);
}

TEST(EngineDb, TypedReadersRefuseTheWrongType) {
    const DbValue text{std::string{"x"}};
    EXPECT_THROW(optionalDouble(text), EngineDbError);
    EXPECT_THROW(optionalInt64(DbValue{1.0}), EngineDbError);
    EXPECT_THROW(requireInt64(DbValue{}), EngineDbError);
}

TEST(EngineDb, QueryBindsParameters) {
    EngineDb db;
    db.execute("CREATE NODE TABLE Point(id SERIAL, t DOUBLE, n STRING, PRIMARY KEY(id));");
    for (int i = 0; i < 3; ++i) {
        db.query("CREATE (:Point {t: $t, n: $n});", {{"t", 0.5 * i}, {"n", std::string{"p"}}});
    }

    const std::vector<DbRow> rows =
        db.query("MATCH (p:Point) WHERE p.t > $above RETURN p.t ORDER BY p.t;", {{"above", 0.25}});
    ASSERT_EQ(rows.size(), 2U);
    EXPECT_EQ(optionalDouble(rows[0][0]), 0.5);
    EXPECT_EQ(optionalDouble(rows[1][0]), 1.0);
}

// A list parameter is what lets one statement serve many nodes: rendering's
// frontier walk binds every id still walking and follows them in one query.
TEST(EngineDb, QueryBindsAListOfIds) {
    EngineDb db;
    db.execute("CREATE NODE TABLE Point(id SERIAL, t DOUBLE, PRIMARY KEY(id));");
    for (int i = 0; i < 5; ++i) {
        db.query("CREATE (:Point {t: $t});", {{"t", 1.0 * i}});
    }

    const std::vector<DbRow> rows =
        db.query("UNWIND $ids AS id MATCH (p:Point {id: id}) RETURN p.t ORDER BY p.t;",
            {{"ids", std::vector<std::int64_t>{1, 3}}});
    ASSERT_EQ(rows.size(), 2U);
    EXPECT_EQ(optionalDouble(rows[0][0]), 1.0);
    EXPECT_EQ(optionalDouble(rows[1][0]), 3.0);

    // An empty list matches nothing rather than failing.
    EXPECT_TRUE(
        db.query("UNWIND $ids AS id MATCH (p:Point {id: id}) RETURN p.t;",
              {{"ids", std::vector<std::int64_t>{}}})
            .empty());
}

TEST(EngineDb, QueryReportsFailures) {
    EngineDb db;
    EXPECT_THROW(db.query("MATCH (n:NoSuchTable) RETURN n;"), EngineDbError);
    EXPECT_THROW(db.query("THIS IS NOT CYPHER"), EngineDbError);
}

TEST(EngineDb, CommittedTransactionKeepsItsWrites) {
    EngineDb db;
    db.execute("CREATE NODE TABLE Point(id SERIAL, PRIMARY KEY(id));");
    {
        Transaction transaction{db};
        db.execute("CREATE (:Point {});");
        transaction.commit();
    }
    EXPECT_EQ(countPoints(db), 1);
}

TEST(EngineDb, UncommittedTransactionRollsBack) {
    EngineDb db;
    db.execute("CREATE NODE TABLE Point(id SERIAL, PRIMARY KEY(id));");
    {
        Transaction transaction{db};
        db.execute("CREATE (:Point {});");
    }
    EXPECT_EQ(countPoints(db), 0);
}

// Ladybug may abandon a transaction itself when a statement in it fails. The
// guard must still leave the connection usable and nothing committed.
TEST(EngineDb, FailedStatementInTransactionLeavesNothing) {
    EngineDb db;
    db.execute("CREATE NODE TABLE Point(id SERIAL, PRIMARY KEY(id));");
    EXPECT_THROW(
        {
            Transaction transaction{db};
            db.execute("CREATE (:Point {});");
            db.execute("MATCH (n:NoSuchTable) RETURN n;");
            transaction.commit();
        },
        EngineDbError);
    EXPECT_EQ(countPoints(db), 0);

    Transaction next{db};
    db.execute("CREATE (:Point {});");
    next.commit();
    EXPECT_EQ(countPoints(db), 1);
}

} // namespace
