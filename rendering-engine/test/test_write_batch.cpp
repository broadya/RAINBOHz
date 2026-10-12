// SPDX-License-Identifier: Apache-2.0

// Tests of WriteBatch: rows staged per table, written in phase order, with
// handles for new nodes and per-unit isolation of failures.

#include "rainbohz/rendering_engine/write_batch.hpp"

#include "rainbohz/rendering_engine/engine_db.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using rainbohz::rendering_engine::DbColumnType;
using rainbohz::rendering_engine::DbRow;
using rainbohz::rendering_engine::EngineDb;
using rainbohz::rendering_engine::NodeHandle;
using rainbohz::rendering_engine::optionalDouble;
using rainbohz::rendering_engine::requireInt64;
using rainbohz::rendering_engine::WriteBatch;
using rainbohz::rendering_engine::WriteOperation;
using rainbohz::rendering_engine::WritePhase;

/// A database shaped like a timeline: a head, a list of points by NEXT, and
/// frames keyed by number.
class WriteBatchDb : public ::testing::Test {
protected:
    WriteBatchDb() {
        db.execute("CREATE NODE TABLE Head(id SERIAL, PRIMARY KEY(id));");
        db.execute("CREATE NODE TABLE Point(id SERIAL, t DOUBLE, PRIMARY KEY(id));");
        db.execute("CREATE NODE TABLE Frame(n INT32, PRIMARY KEY(n));");
        db.execute("CREATE REL TABLE NEXT(FROM Head TO Point, FROM Point TO Point, ONE_ONE);");
        db.execute("CREATE REL TABLE IN_FRAME(FROM Point TO Frame, MANY_MANY);");
        head = batch.addNodeInsert("CREATE (n:Head)", {});
        point = batch.addNodeInsert("CREATE (n:Point {t: r.t})", {{"t", DbColumnType::Double}});
        frame = batch.addStatement(WritePhase::Node,
            "UNWIND $rows AS r MERGE (:Frame {n: CAST(r.n AS INT32)});",
            {{"n", DbColumnType::Int64}}, true);
        deletePoint = batch.addStatement(WritePhase::Delete,
            "UNWIND $rows AS r MATCH (p:Point {id: r.id}) DETACH DELETE p;",
            {{"id", DbColumnType::Int64}});
        headNext = batch.addEdgeInsert("NEXT", "Head", "Point");
        pointNext = batch.addEdgeInsert("NEXT", "Point", "Point");
        inFrame = batch.addEdgeInsert("IN_FRAME", "Point", "Frame");
    }

    /// Stages a list of `times` from a new head, each point in frame 0.
    void stageList(const std::vector<double>& times) {
        const NodeHandle h = batch.stageNode(head, {});
        NodeHandle previous = h;
        bool first = true;
        for (const double t : times) {
            const NodeHandle p = batch.stageNode(point, {t});
            batch.stageEdge(first ? headNext : pointNext, previous, p);
            batch.stageRow(frame, {std::int64_t{0}});
            batch.stageEdge(inFrame, p, std::int64_t{0});
            previous = p;
            first = false;
        }
    }

    /// The times of every list, each read from its head in order.
    std::vector<std::vector<double>> lists() {
        std::vector<std::vector<double>> result;
        for (const DbRow& h : db.query("MATCH (h:Head) RETURN h.id ORDER BY h.id;")) {
            std::vector<double> times;
            std::vector<DbRow> next =
                db.query("MATCH (h:Head)-[:NEXT]->(p:Point) WHERE h.id = $id RETURN p.id, p.t;",
                    {{"id", requireInt64(h.at(0))}});
            while (!next.empty()) {
                times.push_back(*optionalDouble(next.front().at(1)));
                next = db.query(
                    "MATCH (a:Point)-[:NEXT]->(p:Point) WHERE a.id = $id RETURN p.id, p.t;",
                    {{"id", requireInt64(next.front().at(0))}});
            }
            result.push_back(times);
        }
        return result;
    }

    /// Counts the rows a query returns as its single count.
    std::int64_t count(const std::string& cypher) {
        return requireInt64(db.query(cypher).at(0).at(0));
    }

    EngineDb db;
    WriteBatch batch{db};
    WriteOperation head, point, frame, deletePoint, headNext, pointNext, inFrame;
};

// The nodes and the relationships between them go in one transaction: COPY
// must find nodes inserted earlier in it.
TEST_F(WriteBatchDb, WritesListsWhoseNodesAreNewInTheSameFlush) {
    batch.beginUnit();
    stageList({1.0, 2.0, 3.0});
    batch.beginUnit();
    stageList({10.0, 20.0});

    const std::vector<std::optional<std::string>> results = batch.flush();
    ASSERT_EQ(results.size(), 2U);
    EXPECT_FALSE(results[0].has_value()) << *results[0];
    EXPECT_FALSE(results[1].has_value()) << *results[1];
    EXPECT_EQ(lists(), (std::vector<std::vector<double>>{{1.0, 2.0, 3.0}, {10.0, 20.0}}));
}

// Several units staging the same key of a create-if-missing statement send it
// once, so the frame exists once and every point reaches it.
TEST_F(WriteBatchDb, ADistinctStatementSendsEachRowOnce) {
    batch.beginUnit();
    stageList({1.0});
    batch.beginUnit();
    stageList({2.0, 3.0});
    for (const auto& result : batch.flush()) {
        EXPECT_FALSE(result.has_value()) << *result;
    }
    EXPECT_EQ(count("MATCH (f:Frame) RETURN count(f);"), 1);
    EXPECT_EQ(count("MATCH (:Point)-[e:IN_FRAME]->(:Frame) RETURN count(e);"), 3);
}

// Deletes run before inserts, so a list can be replaced in one flush even
// though NEXT allows each node only one successor.
TEST_F(WriteBatchDb, DeletesRunBeforeInserts) {
    batch.beginUnit();
    stageList({1.0, 2.0});
    ASSERT_FALSE(batch.flush().front().has_value());
    const std::vector<DbRow> old = db.query("MATCH (p:Point) RETURN p.id;");

    batch.beginUnit();
    for (const DbRow& row : old) {
        batch.stageRow(deletePoint, {requireInt64(row.at(0))});
    }
    const NodeHandle p = batch.stageNode(point, {5.0});
    const std::int64_t existingHead =
        requireInt64(db.query("MATCH (h:Head) RETURN h.id;").at(0).at(0));
    batch.stageEdge(headNext, existingHead, p);
    const std::vector<std::optional<std::string>> results = batch.flush();
    ASSERT_FALSE(results.front().has_value()) << *results.front();
    EXPECT_EQ(lists(), (std::vector<std::vector<double>>{{5.0}}));
}

// A unit whose write fails is isolated: the others are written, and it leaves
// nothing behind.
TEST_F(WriteBatchDb, AFailingUnitLeavesTheOthersWritten) {
    batch.beginUnit();
    stageList({1.0});
    batch.beginUnit();
    const NodeHandle orphan = batch.stageNode(point, {99.0});
    batch.stageEdge(inFrame, orphan, std::int64_t{7}); // frame 7 is never created
    batch.beginUnit();
    stageList({2.0});

    const std::vector<std::optional<std::string>> results = batch.flush();
    ASSERT_EQ(results.size(), 3U);
    EXPECT_FALSE(results[0].has_value());
    EXPECT_TRUE(results[1].has_value());
    EXPECT_FALSE(results[2].has_value());
    EXPECT_EQ(lists(), (std::vector<std::vector<double>>{{1.0}, {2.0}}));
    EXPECT_EQ(count("MATCH (p:Point) WHERE p.t = 99.0 RETURN count(p);"), 0);
    EXPECT_EQ(batch.unitCount(), 0U);
}

TEST_F(WriteBatchDb, AnEmptyFlushWritesNothing) {
    EXPECT_TRUE(batch.flush().empty());
    batch.beginUnit();
    const std::vector<std::optional<std::string>> results = batch.flush();
    ASSERT_EQ(results.size(), 1U);
    EXPECT_FALSE(results[0].has_value());
}

TEST_F(WriteBatchDb, RefusesMisuse) {
    EXPECT_THROW(batch.stageNode(point, {1.0}), std::invalid_argument); // no unit yet
    batch.beginUnit();
    EXPECT_THROW(batch.stageNode(point, {}), std::invalid_argument);   // wrong length
    EXPECT_THROW(batch.stageRow(point, {1.0}), std::invalid_argument); // not a statement
    const NodeHandle earlier = batch.stageNode(point, {1.0});
    batch.beginUnit();
    const NodeHandle later = batch.stageNode(point, {2.0});
    EXPECT_THROW(batch.stageEdge(pointNext, earlier, later), std::invalid_argument);
    EXPECT_THROW(batch.stageEdge(pointNext, NodeHandle{99}, later), std::invalid_argument);
    EXPECT_THROW(batch.addNodeInsert("CREATE (n:Point)", {{"k", DbColumnType::Int64}}),
        std::invalid_argument);
}

} // namespace
