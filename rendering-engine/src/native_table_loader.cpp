// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/rendering_engine/native_table_loader.hpp"

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

/// @file
/// @brief Loads a score's Parquet tables into the native model with Arrow.
///
/// The file is organised as:
///
///   - reading one Parquet file whole, and one column of it as values;
///   - the narrowing of hzlc's column types to the schema's;
///   - a node file's columns, bound to row fields by member pointer;
///   - a relationship file's ends and properties, likewise;
///   - one loader per node table and per relationship pair the model holds,
///     found by name;
///   - loadNativeScore().
///
/// @see native_table_loader.hpp, ADR 0028

namespace rainbohz::rendering_engine::native {

namespace {

// ---------------------------------------------------------------------------
// Reading Parquet
// ---------------------------------------------------------------------------

/// Refuses a file of the score.
///
/// @param file     The file.
/// @param problem  What is wrong with it.
/// @throws ManifestError  Always, naming the file.
[[noreturn]] void refuse(const std::filesystem::path& file, const std::string& problem) {
    throw ManifestError{file.string() + ": " + problem};
}

/// Reads a Parquet file whole, on the calling thread.
///
/// @param file  The file.
/// @return Its table.
/// @throws ManifestError  If Arrow cannot open or read it.
std::shared_ptr<arrow::Table> readParquet(const std::filesystem::path& file) {
    try {
        arrow::Result<std::shared_ptr<arrow::io::ReadableFile>> opened =
            arrow::io::ReadableFile::Open(file.string());
        if (!opened.ok()) {
            refuse(file, "cannot be opened: " + opened.status().ToString());
        }
        arrow::Result<std::unique_ptr<parquet::arrow::FileReader>> reader =
            parquet::arrow::OpenFile(*opened, arrow::default_memory_pool());
        if (!reader.ok()) {
            refuse(file, "is not a Parquet file Arrow can read: " + reader.status().ToString());
        }
        // One file at a time, on this thread: a score's files are small, and
        // the load stays as single-threaded as the rest of it.
        (*reader)->set_use_threads(false);
        std::shared_ptr<arrow::Table> table;
        const arrow::Status read = (*reader)->ReadTable(&table);
        if (!read.ok()) {
            refuse(file, "cannot be read: " + read.ToString());
        }
        return table;
    } catch (const ManifestError&) {
        throw;
    } catch (const std::exception& e) {
        refuse(file, std::string{"cannot be read: "} + e.what());
    }
}

/// The values of one column, in row order, NULL as nullopt.
///
/// @tparam T  int64, double, bool or std::string.
/// @param file    The file, for a message.
/// @param table   Its table.
/// @param column  The column's index.
/// @return The values.
/// @throws ManifestError  If the column is not of the Arrow type `T` is read
///         from: int64, double, bool or string.
template <typename T>
std::vector<std::optional<T>> valuesOf(
    const std::filesystem::path& file, const arrow::Table& table, int column) {
    using Array = std::conditional_t<std::is_same_v<T, std::int64_t>, arrow::Int64Array,
        std::conditional_t<std::is_same_v<T, double>, arrow::DoubleArray,
            std::conditional_t<std::is_same_v<T, bool>, arrow::BooleanArray, arrow::StringArray>>>;
    const std::shared_ptr<arrow::ChunkedArray>& values = table.column(column);
    if (values->type()->id() != Array::TypeClass::type_id) {
        refuse(file, "column " + table.field(column)->name() + " is " + values->type()->ToString() +
                         "; " + std::string{Array::TypeClass::type_name()} + " is expected");
    }
    std::vector<std::optional<T>> result;
    result.reserve(static_cast<std::size_t>(values->length()));
    for (const std::shared_ptr<arrow::Array>& chunk : values->chunks()) {
        const auto& array = static_cast<const Array&>(*chunk);
        for (std::int64_t i = 0; i < array.length(); ++i) {
            if (array.IsNull(i)) {
                result.emplace_back(std::nullopt);
            } else if constexpr (std::is_same_v<T, std::string>) {
                result.emplace_back(array.GetString(i));
            } else {
                result.emplace_back(static_cast<T>(array.Value(i)));
            }
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// The schema's types
// ---------------------------------------------------------------------------

/// The Arrow-side type a schema type is read from: int64 for every integer,
/// double for FLOAT and DOUBLE.
///
/// @tparam T  The field's type in the model.
template <typename T>
using SourceOf = std::conditional_t<std::is_same_v<T, std::string> || std::is_same_v<T, bool>, T,
    std::conditional_t<std::is_floating_point_v<T>, double, std::int64_t>>;

/// Narrows a value read from a file to the schema's type, as Ladybug's COPY
/// casts it: an integer must fit, and a double fits a FLOAT unless it is
/// finite and beyond FLOAT's range.
///
/// @tparam T  The schema's type.
/// @param value  The value read.
/// @param file   The file, for a message.
/// @param what   The column, for a message.
/// @return The value as `T`.
/// @throws ManifestError  If it does not fit.
template <typename T>
T narrowed(SourceOf<T> value, const std::filesystem::path& file, std::string_view what) {
    if constexpr (std::is_same_v<T, float>) {
        if (std::isfinite(value) &&
            std::abs(value) > static_cast<double>(std::numeric_limits<float>::max())) {
            refuse(file, std::string{what} + " holds " + std::to_string(value) +
                             ", beyond the range of the schema's FLOAT");
        }
        return static_cast<float>(value);
    } else if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool> &&
                         !std::is_same_v<T, std::int64_t>) {
        if (value < std::numeric_limits<T>::min() || value > std::numeric_limits<T>::max()) {
            refuse(file, std::string{what} + " holds " + std::to_string(value) +
                             ", which does not fit the schema's " +
                             (sizeof(T) == 1 ? "INT8" : "INT32"));
        }
        return static_cast<T>(value);
    } else {
        return value;
    }
}

// ---------------------------------------------------------------------------
// Node files
// ---------------------------------------------------------------------------

/// One node table of the manifest, with its file read, and the columns read
/// from it so far.
class NodeSource {
public:
    /// Reads the table's file, if it has one, and checks that its columns
    /// are the manifest's.
    ///
    /// @param entry  The manifest's entry for the table.
    /// @throws ManifestError  If the file cannot be read, its columns are not
    ///         the manifest's, or it holds another number of rows.
    explicit NodeSource(const ManifestNodeTable& entry)
        : entry_{&entry}, file_{entry.file.value_or(std::filesystem::path{})} {
        if (!entry.file.has_value()) {
            return;
        }
        table_ = readParquet(file_);
        std::vector<std::string> names;
        for (const std::shared_ptr<arrow::Field>& field : table_->schema()->fields()) {
            names.push_back(field->name());
        }
        if (names != entry.columns) {
            refuse(file_, "its columns are not the " + std::to_string(entry.columns.size()) +
                              " the manifest lists, in that order");
        }
        if (table_->num_rows() != entry.rows) {
            refuse(file_, "holds " + std::to_string(table_->num_rows()) +
                              " row(s); the manifest lists " + std::to_string(entry.rows));
        }
    }

    /// How many rows the table has.
    ///
    /// @return The manifest's count, which the file's matches.
    std::size_t rows() const noexcept { return static_cast<std::size_t>(entry_->rows); }

    /// Sets one field of every row from a column, if the file has it. A
    /// column the file lacks leaves the field as the row type initialises
    /// it: the schema's DEFAULT, or NULL.
    ///
    /// @tparam Row  The table's row type.
    /// @tparam T    The field's type.
    /// @param name       The column, which is the schema's property name.
    /// @param[in,out] rows  The rows, one per row of the table; the field of
    ///                   each is set.
    /// @param field      The field.
    /// @throws ManifestError  If the column is of the wrong type or a value
    ///         does not fit the field.
    template <typename Row, typename T>
    void column(std::string_view name, std::vector<Row>& rows, std::optional<T> Row::* field) {
        const int index = indexOf(name);
        if (index < 0) {
            return;
        }
        read_.insert(std::string{name});
        const std::string what = entry_->table + "." + std::string{name};
        const std::vector<std::optional<SourceOf<T>>> values =
            valuesOf<SourceOf<T>>(file_, *table_, index);
        for (std::size_t i = 0; i < values.size(); ++i) {
            const std::optional<SourceOf<T>>& value = values[i];
            rows[i].*field = value.has_value() ? std::optional<T>{narrowed<T>(*value, file_, what)}
                                               : std::nullopt;
        }
    }

    /// Checks that every column of the file has been read: a column that is
    /// no property of the table is refused, as Ladybug's COPY refuses it.
    ///
    /// @throws ManifestError  If one has not.
    void requireAllRead() const {
        for (const std::string& name : entry_->columns) {
            if (!read_.contains(name)) {
                refuse(file_, entry_->table + " has no property " + name);
            }
        }
    }

private:
    /// The index of a column in the file.
    ///
    /// @param name  The column.
    /// @return Its index; -1 when there is no file or no such column.
    int indexOf(std::string_view name) const {
        if (table_ == nullptr) {
            return -1;
        }
        return table_->schema()->GetFieldIndex(std::string{name});
    }

    const ManifestNodeTable* entry_;      ///< The manifest's entry; not owned.
    std::filesystem::path file_;          ///< Its file; empty when it has none.
    std::shared_ptr<arrow::Table> table_; ///< The file's contents; null without a file.
    std::set<std::string> read_;          ///< The columns read so far.
};

/// Loads a node table into a vector of rows.
///
/// @tparam Row  The table's row type.
/// @param entry       The manifest's entry.
/// @param[out] rows   The table's rows; empty, since loadNativeScore() loads
///                    each table once.
/// @param bind        Binds the columns: called with the source and the
///                    rows, sized, to call NodeSource::column() per property.
/// @throws ManifestError  As NodeSource does.
template <typename Row, typename Bind>
void loadRows(const ManifestNodeTable& entry, std::vector<Row>& rows, Bind bind) {
    NodeSource source{entry};
    rows.resize(source.rows());
    bind(source, rows);
    source.requireAllRead();
}

/// Loads a node table with no properties, which is held as its row count.
///
/// @param entry       The manifest's entry.
/// @param[out] count  The table's row count.
/// @throws ManifestError  If it has a file.
void loadCount(const ManifestNodeTable& entry, std::size_t& count) {
    if (entry.file.has_value()) {
        refuse(*entry.file, entry.table + " has no properties, so no file is expected");
    }
    count = static_cast<std::size_t>(entry.rows);
}

/// How to load each node table the model holds, by name.
using NodeLoaders =
    std::map<std::string, std::function<void(const ManifestNodeTable&)>, std::less<>>;

/// The loaders of every canonical node table.
///
/// @param[in,out] c  The canonical tables the loaders fill.
/// @return The loaders, by table name.
NodeLoaders nodeLoaders(CanonicalTables& c) {
    NodeLoaders loaders;
    loaders["Score"] = [&c](const ManifestNodeTable& e) {
        loadRows(e, c.scores, [](NodeSource& s, auto& rows) {
            s.column("name", rows, &ScoreRow::name);
            s.column("tempo_ramp_physical_time", rows, &ScoreRow::tempoRampPhysicalTime);
        });
    };
    loaders["SoundEntity"] = [&c](const ManifestNodeTable& e) {
        loadRows(e, c.soundEntities,
            [](NodeSource& s, auto& rows) { s.column("name", rows, &SoundEntityRow::name); });
    };
    loaders["SoundSource"] = [&c](const ManifestNodeTable& e) {
        loadRows(e, c.soundSources,
            [](NodeSource& s, auto& rows) { s.column("name", rows, &SoundSourceRow::name); });
    };
    loaders["PartialSet"] = [&c](const ManifestNodeTable& e) {
        loadRows(e, c.partialSets,
            [](NodeSource& s, auto& rows) { s.column("name", rows, &PartialSetRow::name); });
    };
    loaders["Partial"] = [&c](const ManifestNodeTable& e) {
        loadRows(e, c.partials,
            [](NodeSource& s, auto& rows) { s.column("name", rows, &PartialRow::name); });
    };
    loaders["Label"] = [&c](const ManifestNodeTable& e) {
        loadRows(e, c.labels,
            [](NodeSource& s, auto& rows) { s.column("name", rows, &LabelRow::name); });
    };
    loaders["TempoMap"] = [&c](const ManifestNodeTable& e) {
        loadRows(e, c.tempoMaps, [](NodeSource& s, auto& rows) {
            s.column("name", rows, &TempoMapRow::name);
            s.column("ticks_per_beat", rows, &TempoMapRow::ticksPerBeat);
        });
    };
    loaders["TempoMapPoint"] = [&c](const ManifestNodeTable& e) {
        loadRows(e, c.tempoMapPoints, [](NodeSource& s, auto& rows) {
            s.column("position", rows, &TempoMapPointRow::position);
            s.column("start_tempo", rows, &TempoMapPointRow::startTempo);
            s.column("hold", rows, &TempoMapPointRow::hold);
        });
    };
    loaders["PartialSetMusicalTimeline"] = [&c](const ManifestNodeTable& e) {
        loadCount(e, c.partialSetMusicalTimelines);
    };
    loaders["PartialSetMusicalTimelinePoint"] = [&c](const ManifestNodeTable& e) {
        using Row = PartialSetMusicalTimelinePointRow;
        loadRows(e, c.partialSetMusicalTimelinePoints, [](NodeSource& s, auto& rows) {
            s.column("amplitude_curve_type", rows, &Row::amplitudeCurveType);
            s.column("amplitude_curve_amount", rows, &Row::amplitudeCurveAmount);
            s.column("frequency_curve_type", rows, &Row::frequencyCurveType);
            s.column("frequency_curve_amount", rows, &Row::frequencyCurveAmount);
        });
    };
    loaders["PartialStateSet"] = [&c](const ManifestNodeTable& e) {
        loadCount(e, c.partialStateSets);
    };
    loaders["PartialState"] = [&c](const ManifestNodeTable& e) {
        loadRows(e, c.partialStates, [](NodeSource& s, auto& rows) {
            s.column("frequency", rows, &PartialStateRow::frequency);
            s.column("phase", rows, &PartialStateRow::phase);
            s.column("phase_free_run", rows, &PartialStateRow::phaseFreeRun);
            s.column("amplitude", rows, &PartialStateRow::amplitude);
        });
    };
    loaders["SpatialPositionMusicalTimeline"] = [&c](const ManifestNodeTable& e) {
        loadCount(e, c.spatialPositionMusicalTimelines);
    };
    loaders["SpatialPositionMusicalTimelinePoint"] = [&c](const ManifestNodeTable& e) {
        using Row = SpatialPositionMusicalTimelinePointRow;
        loadRows(e, c.spatialPositionMusicalTimelinePoints, [](NodeSource& s, auto& rows) {
            s.column("azimuth_curve_type", rows, &Row::azimuthCurveType);
            s.column("azimuth_curve_amount", rows, &Row::azimuthCurveAmount);
            s.column("elevation_curve_type", rows, &Row::elevationCurveType);
            s.column("elevation_curve_amount", rows, &Row::elevationCurveAmount);
            s.column("distance_curve_type", rows, &Row::distanceCurveType);
            s.column("distance_curve_amount", rows, &Row::distanceCurveAmount);
        });
    };
    loaders["SpatialPositionState"] = [&c](const ManifestNodeTable& e) {
        loadRows(e, c.spatialPositionStates, [](NodeSource& s, auto& rows) {
            s.column("azimuth", rows, &SpatialPositionStateRow::azimuth);
            s.column("elevation", rows, &SpatialPositionStateRow::elevation);
            s.column("distance", rows, &SpatialPositionStateRow::distance);
        });
    };
    return loaders;
}

// ---------------------------------------------------------------------------
// Relationship files
// ---------------------------------------------------------------------------

/// How many rows a canonical node table holds, for checking edge ends.
///
/// @param c      The canonical tables loaded so far.
/// @param table  A node table's name.
/// @return Its row count; nullopt when the model holds no such canonical
///         table.
std::optional<std::size_t> rowsOf(const CanonicalTables& c, std::string_view table) {
    const std::map<std::string_view, std::size_t, std::less<>> counts{
        {"Score", c.scores.size()},
        {"SoundEntity", c.soundEntities.size()},
        {"SoundSource", c.soundSources.size()},
        {"PartialSet", c.partialSets.size()},
        {"Partial", c.partials.size()},
        {"Label", c.labels.size()},
        {"TempoMap", c.tempoMaps.size()},
        {"TempoMapPoint", c.tempoMapPoints.size()},
        {"PartialSetMusicalTimeline", c.partialSetMusicalTimelines},
        {"PartialSetMusicalTimelinePoint", c.partialSetMusicalTimelinePoints.size()},
        {"PartialStateSet", c.partialStateSets},
        {"PartialState", c.partialStates.size()},
        {"SpatialPositionMusicalTimeline", c.spatialPositionMusicalTimelines},
        {"SpatialPositionMusicalTimelinePoint", c.spatialPositionMusicalTimelinePoints.size()},
        {"SpatialPositionState", c.spatialPositionStates.size()},
    };
    const auto found = counts.find(table);
    return found == counts.end() ? std::nullopt : std::optional{found->second};
}

/// Loads one relationship pair's file into its edge list.
///
/// @tparam Properties  The relationship's properties.
/// @tparam Bind        Callable as `void(const RelColumns&, std::vector<Properties>&)`.
/// @param entry       The manifest's entry.
/// @param c           The canonical tables, for the ends' row counts.
/// @param properties  The relationship's property names, in the schema's
///                    order: the columns after `from` and `to`.
/// @param[in,out] edges  The pair's edges, empty, since loadNativeScore()
///                    loads each pair once; gains one per row.
/// @param bind        Sets each edge's properties from the file's columns.
/// @throws ManifestError  If the file cannot be read, its columns are not
///         `from`, `to` and `properties`, it holds another number of rows,
///         an end is NULL or names no row, or an edge breaks the pair's
///         multiplicity.
template <typename Properties, typename Bind>
void loadEdges(const ManifestRelationship& entry, const CanonicalTables& c,
    const std::vector<std::string>& properties, EdgeList<Properties>& edges, Bind bind) {
    const std::shared_ptr<arrow::Table> table = readParquet(entry.file);
    std::vector<std::string> expected{"from", "to"};
    expected.insert(expected.end(), properties.begin(), properties.end());
    std::vector<std::string> names;
    for (const std::shared_ptr<arrow::Field>& field : table->schema()->fields()) {
        names.push_back(field->name());
    }
    if (names != expected) {
        std::string list;
        for (const std::string& name : expected) {
            list += (list.empty() ? "" : ", ") + name;
        }
        refuse(entry.file, "its columns are not " + list + ", in that order");
    }
    if (table->num_rows() != entry.rows) {
        refuse(entry.file, "holds " + std::to_string(table->num_rows()) +
                               " edge(s); the manifest lists " + std::to_string(entry.rows));
    }

    const std::optional<std::size_t> fromRows = rowsOf(c, entry.from);
    const std::optional<std::size_t> toRows = rowsOf(c, entry.to);
    const std::vector<std::optional<std::int64_t>> from =
        valuesOf<std::int64_t>(entry.file, *table, 0);
    const std::vector<std::optional<std::int64_t>> to =
        valuesOf<std::int64_t>(entry.file, *table, 1);
    std::vector<Properties> values(from.size());
    bind(entry.file, *table, values);

    for (std::size_t i = 0; i < from.size(); ++i) {
        const auto end = [&](const std::optional<std::int64_t>& id, std::optional<std::size_t> rows,
                             const std::string& table) {
            if (id.has_value() && *id >= 0 && static_cast<std::size_t>(*id) < rows.value_or(0)) {
                return *id;
            }
            std::string problem = "edge " + std::to_string(i) + " has ";
            problem += table;
            problem += " ";
            problem += id.has_value() ? std::to_string(*id) : "NULL";
            problem += ", which names no row of ";
            problem += table;
            refuse(entry.file, problem);
        };
        const std::int64_t source = end(from[i], fromRows, entry.from);
        const std::int64_t target = end(to[i], toRows, entry.to);
        try {
            edges.add(source, target, values[i]);
        } catch (const NativeModelError& e) {
            refuse(entry.file, "edge " + std::to_string(i) + ": " + e.what());
        }
    }
}

/// Sets one property of every edge from a relationship file's column.
///
/// @tparam Properties  The relationship's properties.
/// @tparam T           The property's type.
/// @param file    The file, for messages.
/// @param table   Its table.
/// @param column  The property's column index.
/// @param name    The property, for messages.
/// @param[in,out] values  One properties value per edge; the field of each
///                is set.
/// @param field   The field.
/// @throws ManifestError  If the column is of the wrong type or a value
///         does not fit.
template <typename Properties, typename T>
void edgeColumn(const std::filesystem::path& file, const arrow::Table& table, int column,
    std::string_view name, std::vector<Properties>& values, std::optional<T> Properties::* field) {
    const std::vector<std::optional<SourceOf<T>>> read = valuesOf<SourceOf<T>>(file, table, column);
    for (std::size_t i = 0; i < read.size(); ++i) {
        const std::optional<SourceOf<T>>& value = read[i];
        values[i].*field =
            value.has_value() ? std::optional<T>{narrowed<T>(*value, file, name)} : std::nullopt;
    }
}

/// How to load each relationship pair the model holds, by (REL, FROM, TO).
using EdgeLoaders = std::map<std::tuple<std::string, std::string, std::string>,
    std::function<void(const ManifestRelationship&)>>;

/// Adds the loader of a pair without properties.
///
/// @param[in,out] loaders  Gains the loader.
/// @param c                The canonical tables.
/// @param rel              The relationship table.
/// @param from             The FROM node table.
/// @param to               The TO node table.
/// @param edges            The pair's edge list.
void plainPair(EdgeLoaders& loaders, CanonicalTables& c, const std::string& rel,
    const std::string& from, const std::string& to, EdgeList<>& edges) {
    loaders[{rel, from, to}] = [&c, &edges](const ManifestRelationship& e) {
        loadEdges(e, c, {}, edges,
            [](const std::filesystem::path&, const arrow::Table&, std::vector<NoProperties>&) {});
    };
}

/// Adds the loader of a POINT_AT pair.
///
/// @param[in,out] loaders  Gains the loader.
/// @param c                The canonical tables.
/// @param from             The FROM node table.
/// @param edges            The pair's edge list.
void pointAtPair(EdgeLoaders& loaders, CanonicalTables& c, const std::string& from,
    EdgeList<PointAtProperties>& edges) {
    loaders[{"POINT_AT", from, "TempoMap"}] = [&c, &edges](const ManifestRelationship& e) {
        loadEdges(e, c, {"sync_point", "offset_seconds"}, edges,
            [](const std::filesystem::path& file, const arrow::Table& table,
                std::vector<PointAtProperties>& values) {
                edgeColumn(
                    file, table, 2, "POINT_AT.sync_point", values, &PointAtProperties::syncPoint);
                edgeColumn(file, table, 3, "POINT_AT.offset_seconds", values,
                    &PointAtProperties::offsetSeconds);
            });
    };
}

/// The loaders of every canonical relationship pair.
///
/// @param[in,out] c  The canonical tables the loaders fill.
/// @return The loaders, by (REL, FROM, TO).
EdgeLoaders edgeLoaders(CanonicalTables& c) {
    EdgeLoaders loaders;
    plainPair(loaders, c, "HAS_SOUNDENTITY", "Score", "SoundEntity", c.hasSoundEntity);
    plainPair(loaders, c, "HAS_SOUNDSOURCE", "SoundEntity", "SoundSource", c.hasSoundSource);
    plainPair(loaders, c, "HAS_PARTIALSET", "SoundSource", "PartialSet", c.hasPartialSet);
    plainPair(loaders, c, "HAS_PARTIAL", "PartialSet", "Partial", c.hasPartial);
    plainPair(loaders, c, "HAS_LABEL", "Partial", "Label", c.partialLabels);
    plainPair(loaders, c, "HAS_LABEL", "Score", "Label", c.scoreLabels);
    plainPair(loaders, c, "NEXT_POINT", "Score", "TempoMap", c.scoreTempoMap);
    plainPair(loaders, c, "NEXT_POINT", "TempoMap", "TempoMapPoint", c.tempoMapHead);
    plainPair(loaders, c, "NEXT_POINT", "TempoMapPoint", "TempoMapPoint", c.tempoMapNext);
    plainPair(loaders, c, "HAS_MUSICAL_TIMELINE", "PartialSet", "PartialSetMusicalTimeline",
        c.setTimeline);
    plainPair(loaders, c, "NEXT_POINT", "PartialSetMusicalTimeline",
        "PartialSetMusicalTimelinePoint", c.setTimelineHead);
    plainPair(loaders, c, "NEXT_POINT", "PartialSetMusicalTimelinePoint",
        "PartialSetMusicalTimelinePoint", c.setTimelineNext);
    plainPair(loaders, c, "POINT_STATE", "PartialSetMusicalTimelinePoint", "PartialStateSet",
        c.setPointState);
    plainPair(
        loaders, c, "MEMBER_OF_SET_MANY_ONE", "PartialState", "PartialStateSet", c.stateMemberOf);
    plainPair(loaders, c, "STATE_OF", "PartialState", "Partial", c.stateOf);
    plainPair(loaders, c, "HAS_MUSICAL_TIMELINE", "SoundSource", "SpatialPositionMusicalTimeline",
        c.sourceTimeline);
    plainPair(loaders, c, "NEXT_POINT", "SpatialPositionMusicalTimeline",
        "SpatialPositionMusicalTimelinePoint", c.sourceTimelineHead);
    plainPair(loaders, c, "NEXT_POINT", "SpatialPositionMusicalTimelinePoint",
        "SpatialPositionMusicalTimelinePoint", c.sourceTimelineNext);
    plainPair(loaders, c, "POINT_STATE", "SpatialPositionMusicalTimelinePoint",
        "SpatialPositionState", c.sourcePointState);
    pointAtPair(loaders, c, "PartialSetMusicalTimelinePoint", c.setPointAt);
    pointAtPair(loaders, c, "SpatialPositionMusicalTimelinePoint", c.sourcePointAt);
    loaders[{"TEMPO_MAP_SYNCHRONIZATION_POINT", "TempoMap", "TempoMap"}] =
        [&c](const ManifestRelationship& e) {
            loadEdges(e, c, {"sync_point_parent", "sync_point_child"}, c.tempoMapSync,
                [](const std::filesystem::path& file, const arrow::Table& table,
                    std::vector<SyncPointProperties>& values) {
                    edgeColumn(file, table, 2, "TEMPO_MAP_SYNCHRONIZATION_POINT.sync_point_parent",
                        values, &SyncPointProperties::syncPointParent);
                    edgeColumn(file, table, 3, "TEMPO_MAP_SYNCHRONIZATION_POINT.sync_point_child",
                        values, &SyncPointProperties::syncPointChild);
                });
        };
    return loaders;
}

// ---------------------------------------------------------------------------
// EngineConfig
// ---------------------------------------------------------------------------

/// Builds the EngineConfig from the manifest's `engine_config`.
///
/// @param manifest  The manifest, for messages.
/// @param values    Its properties.
/// @return The row.
/// @throws ManifestError  If a property is not one of EngineConfig's, or its
///         value does not fit the schema's type.
EngineConfigRow engineConfigFrom(const std::filesystem::path& manifest,
    const std::vector<std::pair<std::string, std::int64_t>>& values) {
    EngineConfigRow row;
    for (const auto& [key, value] : values) {
        const std::string what = "engine_config." + key;
        if (key == "frameSize") {
            row.frameSize = narrowed<std::int32_t>(value, manifest, what);
        } else if (key == "sampleRate") {
            row.sampleRate = narrowed<std::int32_t>(value, manifest, what);
        } else if (key == "channelMode") {
            row.channelMode = narrowed<std::int8_t>(value, manifest, what);
        } else {
            refuse(manifest, "EngineConfig has no property " + key);
        }
    }
    return row;
}

} // namespace

NativeLoad loadNativeScore(const std::filesystem::path& manifest) {
    const ScoreManifest tables = readScoreManifest(manifest);
    NativeLoad load;
    CanonicalTables& c = load.model.canonical;

    if (!tables.engineConfig.empty()) {
        c.engineConfigs.push_back(engineConfigFrom(manifest, tables.engineConfig));
        ++load.rows;
    }

    // A table or pair listed twice is refused by name, whatever it holds:
    // a second listing would renumber the first's rows, or add its edges again.
    std::set<std::string> loadedTables;
    std::set<std::tuple<std::string, std::string, std::string>> loadedPairs;

    const NodeLoaders nodes = nodeLoaders(c);
    for (const ManifestNodeTable& entry : tables.nodes) {
        if (!loadedTables.insert(entry.table).second) {
            refuse(entry.file.value_or(manifest), entry.table + " is listed twice");
        }
        const auto loader = nodes.find(entry.table);
        if (loader == nodes.end()) {
            refuse(entry.file.value_or(manifest),
                "the native backend loads only a score's canonical tables, and " + entry.table +
                    " is not one of them; load this score with --backend ladybug");
        }
        loader->second(entry);
        ++load.nodeTables;
        load.rows += entry.rows;
    }

    const EdgeLoaders edges = edgeLoaders(c);
    for (const ManifestRelationship& entry : tables.relationships) {
        if (!loadedPairs.insert({entry.table, entry.from, entry.to}).second) {
            refuse(
                entry.file, entry.table + " " + entry.from + "->" + entry.to + " is listed twice");
        }
        const auto loader = edges.find({entry.table, entry.from, entry.to});
        if (loader == edges.end()) {
            refuse(entry.file, "the native backend loads only a score's canonical relationships, "
                               "and " +
                                   entry.table + " " + entry.from + "->" + entry.to +
                                   " is not one of them; load this score with --backend ladybug");
        }
        loader->second(entry);
        ++load.relationshipPairs;
        load.rows += entry.rows;
    }
    return load;
}

} // namespace rainbohz::rendering_engine::native
