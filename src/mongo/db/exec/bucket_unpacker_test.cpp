/**
 *    Copyright (C) 2021-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/bson/json.h"
#include "mongo/bson/util/bsoncolumnbuilder.h"
#include "mongo/db/exec/bucket_unpacker.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

constexpr auto kUserDefinedTimeName = "time"_sd;
constexpr auto kUserDefinedMetaName = "myMeta"_sd;

/**
 * A fixture to test the BucketUnpacker
 */
class BucketUnpackerTest : public mongo::unittest::Test {
public:
    // First exponent for 10^exp that would go over the BSON max limit when filled with Timestamp
    // fields. See BucketUnpackerV1::kTimestampObjSizeTable for more details
    static constexpr int kBSONSizeExceeded10PowerExponentTimeFields = 6;

    /**
     * Makes a fresh BucketUnpacker, resets it to unpack the given 'bucket', and then returns it
     * before actually doing any unpacking.
     */
    BucketUnpacker makeBucketUnpacker(std::set<std::string> fields,
                                      BucketUnpacker::Behavior behavior,
                                      BSONObj bucket,
                                      boost::optional<std::string> metaFieldName = boost::none) {
        auto spec = BucketSpec{kUserDefinedTimeName.toString(), metaFieldName, std::move(fields)};

        BucketUnpacker unpacker{std::move(spec), behavior};
        unpacker.reset(std::move(bucket));
        return unpacker;
    }

    /**
     * Constructs a 'BucketUnpacker' based on the provided parameters and then resets it to unpack
     * the given 'bucket'. Asserts that 'reset()' throws the given 'errorCode'.
     */
    void assertUnpackerThrowsCode(std::set<std::string> fields,
                                  BucketUnpacker::Behavior behavior,
                                  BSONObj bucket,
                                  boost::optional<std::string> metaFieldName,
                                  int errorCode) {
        auto spec = BucketSpec{kUserDefinedTimeName.toString(), metaFieldName, std::move(fields)};
        BucketUnpacker unpacker{std::move(spec), behavior};
        ASSERT_THROWS_CODE(unpacker.reset(std::move(bucket)), AssertionException, errorCode);
    }

    void assertGetNext(BucketUnpacker& unpacker, const Document& expected) {
        ASSERT_DOCUMENT_EQ(unpacker.getNext(), expected);
    }

    std::pair<BSONObj, StringData> buildUncompressedBucketForMeasurementCount(int num) {
        BSONObjBuilder root;
        {
            BSONObjBuilder builder(root.subobjStart("control"_sd));
            builder.append("version"_sd, 1);
        }
        {
            BSONObjBuilder data(root.subobjStart("data"_sd));
            {
                DecimalCounter<uint32_t> fieldNameCounter;
                BSONObjBuilder builder(data.subobjStart("time"_sd));
                for (int i = 0; i < num; ++i, ++fieldNameCounter) {
                    builder.append(fieldNameCounter, Date_t::now());
                }
            }
        }
        BSONObj obj = root.obj();
        return {obj, "time"_sd};
    }

    // Simple bucket compressor. Does not handle data fields out of order and does not sort fields
    // on time.
    // TODO (SERVER-58578): Replace with real bucket compressor
    BSONObj compress(BSONObj uncompressed, StringData timeField) {
        // Rewrite data fields as columns.
        BSONObjBuilder builder;
        for (auto& elem : uncompressed) {
            if (elem.fieldNameStringData() == "control") {
                BSONObjBuilder control(builder.subobjStart("control"));

                // Set right version, leave other control fields unchanged
                for (const auto& controlField : elem.Obj()) {
                    if (controlField.fieldNameStringData() == "version") {
                        control.append("version", 2);
                    } else {
                        control.append(controlField);
                    }
                }

                continue;
            }
            if (elem.fieldNameStringData() != "data") {
                // Non-data fields can be unchanged.
                builder.append(elem);
                continue;
            }

            BSONObjBuilder dataBuilder = builder.subobjStart("data");
            std::list<BSONColumnBuilder> columnBuilders;
            size_t numTimeFields = 0;
            for (auto& column : elem.Obj()) {
                // Compress all data fields
                columnBuilders.emplace_back(column.fieldNameStringData());
                auto& columnBuilder = columnBuilders.back();

                for (auto& measurement : column.Obj()) {
                    int index = std::atoi(measurement.fieldName());
                    for (int i = columnBuilder.size(); i < index; ++i) {
                        columnBuilder.skip();
                    }
                    columnBuilder.append(measurement);
                }
                if (columnBuilder.fieldName() == timeField) {
                    numTimeFields = columnBuilder.size();
                }
            }

            for (auto& builder : columnBuilders) {
                for (size_t i = builder.size(); i < numTimeFields; ++i) {
                    builder.skip();
                }

                BSONBinData binData = builder.finalize();
                dataBuilder.append(builder.fieldName(), binData);
            }
        }
        return builder.obj();
    }
};

TEST_F(BucketUnpackerTest, UnpackBasicIncludeAllMeasurementFields) {
    std::set<std::string> fields{
        "_id", kUserDefinedMetaName.toString(), kUserDefinedTimeName.toString(), "a", "b"};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
        "time: {'0':1, '1':2}, "
        "a:{'0':1, '1':2}, b:{'1':1}}}");

    auto unpacker = makeBucketUnpacker(std::move(fields),
                                       BucketUnpacker::Behavior::kInclude,
                                       std::move(bucket),
                                       kUserDefinedMetaName.toString());

    ASSERT_TRUE(unpacker.hasNext());
    assertGetNext(unpacker,
                  Document{fromjson("{time: 1, myMeta: {m1: 999, m2: 9999}, _id: 1, a: 1}")});

    ASSERT_TRUE(unpacker.hasNext());
    assertGetNext(unpacker,
                  Document{fromjson("{time: 2, myMeta: {m1: 999, m2: 9999}, _id: 2, a :2, b: 1}")});
    ASSERT_FALSE(unpacker.hasNext());
}

TEST_F(BucketUnpackerTest, ExcludeASingleField) {
    std::set<std::string> fields{"b"};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
        "time: {'0':1, '1':2}, "
        "a:{'0':1, '1':2}, b:{'1':1}}}");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketUnpacker::Behavior::kExclude,
                                           std::move(bucket),
                                           kUserDefinedMetaName.toString());

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker,
                      Document{fromjson("{time: 1, myMeta: {m1: 999, m2: 9999}, _id: 1, a: 1}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker,
                      Document{fromjson("{time: 2, myMeta: {m1: 999, m2: 9999}, _id: 2, a: 2}")});
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    test(compress(bucket, "time"_sd));
}

TEST_F(BucketUnpackerTest, EmptyIncludeGetsEmptyMeasurements) {
    std::set<std::string> fields{};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
        "time: {'0':1, '1':2}, "
        "a:{'0':1, '1':2}, b:{'1':1}}}");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketUnpacker::Behavior::kInclude,
                                           std::move(bucket),
                                           kUserDefinedMetaName.toString());

        // We should produce empty documents, one per measurement in the bucket.
        for (auto idx = 0; idx < 2; ++idx) {
            ASSERT_TRUE(unpacker.hasNext());
            assertGetNext(unpacker, Document(fromjson("{}")));
        }
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    test(compress(bucket, "time"_sd));
}

TEST_F(BucketUnpackerTest, EmptyExcludeMaterializesAllFields) {
    std::set<std::string> fields{};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
        "time: {'0':1, '1':2}, "
        "a:{'0':1, '1':2}, b:{'1':1}}}");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketUnpacker::Behavior::kExclude,
                                           std::move(bucket),
                                           kUserDefinedMetaName.toString());
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker,
                      Document{fromjson("{time: 1, myMeta: {m1: 999, m2: 9999}, _id: 1, a: 1}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(
            unpacker,
            Document{fromjson("{time: 2, myMeta: {m1: 999, m2: 9999}, _id: 2, a :2, b: 1}")});
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    test(compress(bucket, "time"_sd));
}

TEST_F(BucketUnpackerTest, SparseColumnsWhereOneColumnIsExhaustedBeforeTheOther) {
    std::set<std::string> fields{};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
        "time: {'0':1, '1':2}, "
        "a:{'0':1}, b:{'1':1}}}");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketUnpacker::Behavior::kExclude,
                                           std::move(bucket),
                                           kUserDefinedMetaName.toString());
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker,
                      Document{fromjson("{time: 1, myMeta: {m1: 999, m2: 9999}, _id: 1, a: 1}")});
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker,
                      Document{fromjson("{time: 2, myMeta: {m1: 999, m2: 9999}, _id: 2, b: 1}")});
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    test(compress(bucket, "time"_sd));
}

TEST_F(BucketUnpackerTest, UnpackBasicIncludeWithDollarPrefix) {
    std::set<std::string> fields{
        "_id", "$a", "b", kUserDefinedMetaName.toString(), kUserDefinedTimeName.toString()};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
        "time: {'0':1, '1':2}, "
        "$a:{'0':1, '1':2}, b:{'1':1}}}");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketUnpacker::Behavior::kInclude,
                                           std::move(bucket),
                                           kUserDefinedMetaName.toString());
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker,
                      Document{fromjson("{time: 1, myMeta: {m1: 999, m2: 9999}, _id: 1, $a: 1}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(
            unpacker,
            Document{fromjson("{time: 2, myMeta: {m1: 999, m2: 9999}, _id: 2, $a: 2, b: 1}")});
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    test(compress(bucket, "time"_sd));
}

TEST_F(BucketUnpackerTest, BucketsWithMetadataOnly) {
    std::set<std::string> fields{};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2}, "
        "time: {'0':1, '1':2}}}");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketUnpacker::Behavior::kExclude,
                                           std::move(bucket),
                                           kUserDefinedMetaName.toString());
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker,
                      Document{fromjson("{time: 1, myMeta: {m1: 999, m2: 9999}, _id: 1}")});
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker,
                      Document{fromjson("{time: 2, myMeta: {m1: 999, m2: 9999}, _id: 2}")});
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    test(compress(bucket, "time"_sd));
}

TEST_F(BucketUnpackerTest, UnorderedRowKeysDoesntAffectMaterialization) {
    std::set<std::string> fields{};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'1':1, '0':2, '2': "
        "3}, time: {'1':1, '0': 2, "
        "'2': 3}}}");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketUnpacker::Behavior::kExclude,
                                           std::move(bucket),
                                           kUserDefinedMetaName.toString());
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker,
                      Document{fromjson("{time: 1, myMeta: {m1: 999, m2: 9999}, _id: 1}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker,
                      Document{fromjson("{time: 2, myMeta: {m1: 999, m2: 9999}, _id: 2}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker,
                      Document{fromjson("{time: 3, myMeta: {m1: 999, m2: 9999}, _id: 3}")});
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    // bucket compressor does not handle unordered row keys
}

TEST_F(BucketUnpackerTest, MissingMetaFieldDoesntMaterializeMetadata) {
    std::set<std::string> fields{};

    auto bucket = fromjson(
        "{control: {'version': 1}, data: {_id: {'0':1, '1':2, '2': 3}, time: {'0':1, '1': 2, '2': "
        "3}}}");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketUnpacker::Behavior::kExclude,
                                           std::move(bucket),
                                           kUserDefinedMetaName.toString());
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: 1, _id: 1}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: 2, _id: 2}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: 3, _id: 3}")});
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    test(compress(bucket, "time"_sd));
}

TEST_F(BucketUnpackerTest, MissingMetaFieldDoesntMaterializeMetadataUnorderedKeys) {
    std::set<std::string> fields{};

    auto bucket = fromjson(
        "{control: {'version': 1}, data: {_id: {'1':1, '0':2, '2': 3}, time: {'1':1, '0': 2, '2': "
        "3}}}");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketUnpacker::Behavior::kExclude,
                                           std::move(bucket),
                                           kUserDefinedMetaName.toString());
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: 1, _id: 1}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: 2, _id: 2}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: 3, _id: 3}")});
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    // bucket compressor does not handle unordered row keys
}

TEST_F(BucketUnpackerTest, ExcludedMetaFieldDoesntMaterializeMetadataWhenBucketHasMeta) {
    std::set<std::string> fields{kUserDefinedMetaName.toString()};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2, '2': "
        "3}, time: {'0':1, '1': 2, "
        "'2': 3}}}");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketUnpacker::Behavior::kExclude,
                                           std::move(bucket),
                                           kUserDefinedMetaName.toString());
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: 1, _id: 1}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: 2, _id: 2}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: 3, _id: 3}")});
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    test(compress(bucket, "time"_sd));
}

TEST_F(BucketUnpackerTest, UnpackerResetThrowsOnUndefinedMeta) {
    std::set<std::string> fields{};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: undefined, data: {_id: {'0':1, '1':2, '2': 3}, time: "
        "{'0':1, '1': 2, '2': 3}}}");

    auto test = [&](BSONObj bucket) {
        assertUnpackerThrowsCode(fields,
                                 BucketUnpacker::Behavior::kExclude,
                                 std::move(bucket),
                                 kUserDefinedMetaName.toString(),
                                 5369600);
    };

    test(bucket);
    test(compress(bucket, "time"_sd));
}

TEST_F(BucketUnpackerTest, UnpackerResetThrowsOnUnexpectedMeta) {
    std::set<std::string> fields{};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: {'m1': 999, 'm2': 9999}, data: {_id: {'0':1, '1':2, '2': "
        "3}, time: {'0':1, '1': 2, "
        "'2': 3}}}");

    auto test = [&](BSONObj bucket) {
        assertUnpackerThrowsCode(fields,
                                 BucketUnpacker::Behavior::kExclude,
                                 std::move(bucket),
                                 boost::none /* no metaField provided */,
                                 5369601);
    };

    test(bucket);
    test(compress(bucket, "time"_sd));
}

TEST_F(BucketUnpackerTest, NullMetaInBucketMaterializesAsNull) {
    std::set<std::string> fields{};

    auto bucket = fromjson(
        "{control: {'version': 1}, meta: null, data: {_id: {'0':4, '1':5, '2':6}, time: {'0':4, "
        "'1': 5, '2': 6}}}");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketUnpacker::Behavior::kExclude,
                                           std::move(bucket),
                                           kUserDefinedMetaName.toString());
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: 4, myMeta: null, _id: 4}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: 5, myMeta: null, _id: 5}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: 6, myMeta: null, _id: 6}")});
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    test(compress(bucket, "time"_sd));
}

TEST_F(BucketUnpackerTest, GetNextHandlesMissingMetaInBucket) {
    std::set<std::string> fields{};

    auto bucket = fromjson(R"(
{
    control: {version: 1},
    data: {
        _id: {'0':4, '1':5, '2':6},
        time: {'0':4, '1': 5, '2': 6}
    }
})");

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(fields,
                                           BucketUnpacker::Behavior::kExclude,
                                           std::move(bucket),
                                           kUserDefinedMetaName.toString());
        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: 4, _id: 4}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: 5, _id: 5}")});

        ASSERT_TRUE(unpacker.hasNext());
        assertGetNext(unpacker, Document{fromjson("{time: 6, _id: 6}")});
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    test(compress(bucket, "time"_sd));
}

TEST_F(BucketUnpackerTest, EmptyDataRegionInBucketIsTolerated) {
    std::set<std::string> fields{};

    auto bucket = Document{{"_id", 1},
                           {"control", Document{{"version", 1}}},
                           {"meta", Document{{"m1", 999}, {"m2", 9999}}},
                           {"data", Document{}}}
                      .toBson();

    auto test = [&](BSONObj bucket) {
        auto unpacker = makeBucketUnpacker(
            fields, BucketUnpacker::Behavior::kExclude, bucket, kUserDefinedMetaName.toString());
        ASSERT_FALSE(unpacker.hasNext());
    };

    test(bucket);
    test(compress(bucket, "time"_sd));
}

TEST_F(BucketUnpackerTest, UnpackerResetThrowsOnEmptyBucket) {
    std::set<std::string> fields{};

    auto bucket = Document{};
    assertUnpackerThrowsCode(std::move(fields),
                             BucketUnpacker::Behavior::kExclude,
                             bucket.toBson(),
                             kUserDefinedMetaName.toString(),
                             5346510);
}

TEST_F(BucketUnpackerTest, EraseMetaFromFieldSetAndDetermineIncludeMeta) {
    // Tests a missing 'metaField' in the spec.
    std::set<std::string> empFields{};
    auto spec = BucketSpec{kUserDefinedTimeName.toString(), boost::none, std::move(empFields)};
    ASSERT_FALSE(
        eraseMetaFromFieldSetAndDetermineIncludeMeta(BucketUnpacker::Behavior::kInclude, &spec));

    // Tests a spec with 'metaField' in include list.
    std::set<std::string> fields{kUserDefinedMetaName.toString()};
    auto specWithMetaInclude = BucketSpec{
        kUserDefinedTimeName.toString(), kUserDefinedMetaName.toString(), std::move(fields)};
    ASSERT_TRUE(eraseMetaFromFieldSetAndDetermineIncludeMeta(BucketUnpacker::Behavior::kInclude,
                                                             &specWithMetaInclude));
    ASSERT_EQ(specWithMetaInclude.fieldSet.count(kUserDefinedMetaName.toString()), 0);

    std::set<std::string> fieldsNoMeta{"foo"};
    auto specWithFooInclude = BucketSpec{
        kUserDefinedTimeName.toString(), kUserDefinedMetaName.toString(), std::move(fieldsNoMeta)};
    ASSERT_TRUE(eraseMetaFromFieldSetAndDetermineIncludeMeta(BucketUnpacker::Behavior::kExclude,
                                                             &specWithFooInclude));
    ASSERT_FALSE(eraseMetaFromFieldSetAndDetermineIncludeMeta(BucketUnpacker::Behavior::kInclude,
                                                              &specWithFooInclude));

    // Tests a spec with 'metaField' not in exclude list.
    std::set<std::string> excludeFields{};
    auto specWithMetaExclude = BucketSpec{
        kUserDefinedTimeName.toString(), kUserDefinedMetaName.toString(), std::move(excludeFields)};
    ASSERT_TRUE(eraseMetaFromFieldSetAndDetermineIncludeMeta(BucketUnpacker::Behavior::kExclude,
                                                             &specWithMetaExclude));
    ASSERT_FALSE(eraseMetaFromFieldSetAndDetermineIncludeMeta(BucketUnpacker::Behavior::kInclude,
                                                              &specWithMetaExclude));
}

TEST_F(BucketUnpackerTest, DetermineIncludeTimeField) {
    std::set<std::string> fields{kUserDefinedTimeName.toString()};
    auto spec = BucketSpec{
        kUserDefinedTimeName.toString(), kUserDefinedMetaName.toString(), std::move(fields)};
    ASSERT_TRUE(determineIncludeTimeField(BucketUnpacker::Behavior::kInclude, &spec));
    ASSERT_FALSE(determineIncludeTimeField(BucketUnpacker::Behavior::kExclude, &spec));
}

TEST_F(BucketUnpackerTest, DetermineIncludeField) {
    std::string includedMeasurementField = "measurementField1";
    std::string excludedMeasurementField = "measurementField2";
    std::set<std::string> fields{kUserDefinedTimeName.toString(), includedMeasurementField};
    auto spec = BucketSpec{
        kUserDefinedTimeName.toString(), kUserDefinedMetaName.toString(), std::move(fields)};

    ASSERT_TRUE(determineIncludeField(
        kUserDefinedTimeName.toString(), BucketUnpacker::Behavior::kInclude, spec));
    ASSERT_FALSE(determineIncludeField(
        kUserDefinedTimeName.toString(), BucketUnpacker::Behavior::kExclude, spec));

    ASSERT_TRUE(
        determineIncludeField(includedMeasurementField, BucketUnpacker::Behavior::kInclude, spec));
    ASSERT_FALSE(
        determineIncludeField(includedMeasurementField, BucketUnpacker::Behavior::kExclude, spec));

    ASSERT_FALSE(
        determineIncludeField(excludedMeasurementField, BucketUnpacker::Behavior::kInclude, spec));
    ASSERT_TRUE(
        determineIncludeField(excludedMeasurementField, BucketUnpacker::Behavior::kExclude, spec));
}

/**
 * Manually computes the timestamp object size for n timestamps.
 */
auto expectedTimestampObjSize(int32_t rowKeyOffset, int32_t n) {
    BSONObjBuilder bob;
    for (auto i = 0; i < n; ++i) {
        bob.appendDate(std::to_string(i + rowKeyOffset), Date_t::now());
    }
    return bob.done().objsize();
}

TEST_F(BucketUnpackerTest, ExtractSingleMeasurement) {
    std::set<std::string> fields{
        "_id", kUserDefinedMetaName.toString(), kUserDefinedTimeName.toString(), "a", "b"};
    auto spec = BucketSpec{
        kUserDefinedTimeName.toString(), kUserDefinedMetaName.toString(), std::move(fields)};
    auto unpacker = BucketUnpacker{std::move(spec), BucketUnpacker::Behavior::kInclude};

    auto d1 = dateFromISOString("2020-02-17T00:00:00.000Z").getValue();
    auto d2 = dateFromISOString("2020-02-17T01:00:00.000Z").getValue();
    auto d3 = dateFromISOString("2020-02-17T02:00:00.000Z").getValue();
    auto bucket = BSON("control" << BSON("version" << 1) << "meta"
                                 << BSON("m1" << 999 << "m2" << 9999) << "data"
                                 << BSON("_id" << BSON("0" << 1 << "1" << 2 << "2" << 3) << "time"
                                               << BSON("0" << d1 << "1" << d2 << "2" << d3) << "a"
                                               << BSON("0" << 1 << "1" << 2 << "2" << 3) << "b"
                                               << BSON("1" << 1 << "2" << 2)));

    unpacker.reset(std::move(bucket));

    auto next = unpacker.extractSingleMeasurement(0);
    auto expected = Document{
        {"myMeta", Document{{"m1", 999}, {"m2", 9999}}}, {"_id", 1}, {"time", d1}, {"a", 1}};
    ASSERT_DOCUMENT_EQ(next, expected);

    next = unpacker.extractSingleMeasurement(2);
    expected = Document{{"myMeta", Document{{"m1", 999}, {"m2", 9999}}},
                        {"_id", 3},
                        {"time", d3},
                        {"a", 3},
                        {"b", 2}};
    ASSERT_DOCUMENT_EQ(next, expected);

    next = unpacker.extractSingleMeasurement(1);
    expected = Document{{"myMeta", Document{{"m1", 999}, {"m2", 9999}}},
                        {"_id", 2},
                        {"time", d2},
                        {"a", 2},
                        {"b", 1}};
    ASSERT_DOCUMENT_EQ(next, expected);

    // Can we extract the middle element again?
    next = unpacker.extractSingleMeasurement(1);
    ASSERT_DOCUMENT_EQ(next, expected);
}

TEST_F(BucketUnpackerTest, ExtractSingleMeasurementSparse) {
    std::set<std::string> fields{
        "_id", kUserDefinedMetaName.toString(), kUserDefinedTimeName.toString(), "a", "b"};
    auto spec = BucketSpec{
        kUserDefinedTimeName.toString(), kUserDefinedMetaName.toString(), std::move(fields)};
    auto unpacker = BucketUnpacker{std::move(spec), BucketUnpacker::Behavior::kInclude};

    auto d1 = dateFromISOString("2020-02-17T00:00:00.000Z").getValue();
    auto d2 = dateFromISOString("2020-02-17T01:00:00.000Z").getValue();
    auto bucket = BSON("control" << BSON("version" << 1) << "meta"
                                 << BSON("m1" << 999 << "m2" << 9999) << "data"
                                 << BSON("_id" << BSON("0" << 1 << "1" << 2) << "time"
                                               << BSON("0" << d1 << "1" << d2) << "a"
                                               << BSON("0" << 1) << "b" << BSON("1" << 1)));

    unpacker.reset(std::move(bucket));
    auto next = unpacker.extractSingleMeasurement(1);
    auto expected = Document{
        {"myMeta", Document{{"m1", 999}, {"m2", 9999}}}, {"_id", 2}, {"time", d2}, {"b", 1}};
    ASSERT_DOCUMENT_EQ(next, expected);

    // Can we extract the same element again?
    next = unpacker.extractSingleMeasurement(1);
    ASSERT_DOCUMENT_EQ(next, expected);

    next = unpacker.extractSingleMeasurement(0);
    expected = Document{
        {"myMeta", Document{{"m1", 999}, {"m2", 9999}}}, {"_id", 1}, {"time", d1}, {"a", 1}};
    ASSERT_DOCUMENT_EQ(next, expected);

    // Can we extract the same element twice in a row?
    next = unpacker.extractSingleMeasurement(0);
    ASSERT_DOCUMENT_EQ(next, expected);

    next = unpacker.extractSingleMeasurement(0);
    ASSERT_DOCUMENT_EQ(next, expected);
}

TEST_F(BucketUnpackerTest, ComputeMeasurementCountLowerBoundsAreCorrect) {
    // Test the case when the target size hits a table entry which represents the lower bound of an
    // interval.
    for (int i = 0; i <= kBSONSizeExceeded10PowerExponentTimeFields; ++i) {
        int bucketCount = std::pow(10, i);
        auto [bucket, timeField] = buildUncompressedBucketForMeasurementCount(bucketCount);
        ASSERT_EQ(bucketCount, BucketUnpacker::computeMeasurementCount(bucket, timeField));
    }
}

TEST_F(BucketUnpackerTest, ComputeMeasurementCountUpperBoundsAreCorrect) {
    // Test the case when the target size hits a table entry which represents the upper bound of an
    // interval.
    for (int i = 1; i <= kBSONSizeExceeded10PowerExponentTimeFields; ++i) {
        int bucketCount = std::pow(10, i) - 1;
        auto [bucket, timeField] = buildUncompressedBucketForMeasurementCount(bucketCount);
        ASSERT_EQ(bucketCount, BucketUnpacker::computeMeasurementCount(bucket, timeField));
    }
}

TEST_F(BucketUnpackerTest, ComputeMeasurementCountAllPointsInSmallerIntervals) {
    // Test all values for some of the smaller intervals up to 100 measurements.
    for (auto bucketCount = 0; bucketCount < 25; ++bucketCount) {
        auto [bucket, timeField] = buildUncompressedBucketForMeasurementCount(bucketCount);
        ASSERT_EQ(bucketCount, BucketUnpacker::computeMeasurementCount(bucket, timeField));
    }
}

TEST_F(BucketUnpackerTest, ComputeMeasurementCountInLargerIntervals) {
    auto testMeasurementCount = [&](int num) {
        auto [bucket, timeField] = buildUncompressedBucketForMeasurementCount(num);
        ASSERT_EQ(num, BucketUnpacker::computeMeasurementCount(bucket, timeField));
    };

    testMeasurementCount(2222);
    testMeasurementCount(11111);
    testMeasurementCount(449998);
}
}  // namespace
}  // namespace mongo
