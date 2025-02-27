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

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/sbe_stage_builder_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo::sbe {

class SbeValueTest : public SbeStageBuilderTestFixture {};

TEST_F(SbeValueTest, CompareTwoArraySets) {
    using ValueFnType = void(value::ArraySet*);
    using AssertFnType = void(value::TypeTags, value::Value, value::TypeTags, value::Value);

    auto arraySetComparisonTestGenFn = [](std::function<ValueFnType> lhsValueGenFn,
                                          std::function<ValueFnType> rhsValueGenFn,
                                          std::function<AssertFnType> assertFn) {
        auto [lhsTag, lhsVal] = value::makeNewArraySet();
        value::ValueGuard lhsGuard{lhsTag, lhsVal};
        auto lhsView = value::getArraySetView(lhsVal);
        lhsValueGenFn(lhsView);

        auto [rhsTag, rhsVal] = value::makeNewArraySet();
        value::ValueGuard rhsGuard{rhsTag, rhsVal};
        auto rhsView = value::getArraySetView(rhsVal);
        rhsValueGenFn(rhsView);

        assertFn(lhsTag, lhsVal, rhsTag, rhsVal);
    };

    auto arraySetEqualityComparisonTestGenFn = [&](std::function<ValueFnType> lhsValueGenFn,
                                                   std::function<ValueFnType> rhsValueGenFn) {
        arraySetComparisonTestGenFn(lhsValueGenFn,
                                    rhsValueGenFn,
                                    [&](value::TypeTags lhsTag,
                                        value::Value lhsVal,
                                        value::TypeTags rhsTag,
                                        value::Value rhsVal) {
                                        ASSERT(valueEquals(lhsTag, lhsVal, rhsTag, rhsVal))
                                            << "lhs array set: " << std::make_pair(lhsTag, lhsVal)
                                            << "rhs array set: " << std::make_pair(rhsTag, rhsVal);
                                    });
    };

    auto arraySetInequalityComparisonTestGenFn = [&](std::function<ValueFnType> lhsValueGenFn,
                                                     std::function<ValueFnType> rhsValueGenFn) {
        arraySetComparisonTestGenFn(lhsValueGenFn,
                                    rhsValueGenFn,
                                    [&](value::TypeTags lhsTag,
                                        value::Value lhsVal,
                                        value::TypeTags rhsTag,
                                        value::Value rhsVal) {
                                        ASSERT(!valueEquals(lhsTag, lhsVal, rhsTag, rhsVal))
                                            << "lhs array set: " << std::make_pair(lhsTag, lhsVal)
                                            << "rhs array set: " << std::make_pair(rhsTag, rhsVal);
                                    });
    };

    auto addShortStringFn = [](value::ArraySet* set) {
        auto [rhsItemTag, rhsItemVal] = value::makeSmallString("abc"_sd);
        set->push_back(rhsItemTag, rhsItemVal);
    };
    auto addLongStringFn = [](value::ArraySet* set) {
        auto [rhsItemTag, rhsItemVal] = value::makeNewString("a long enough string"_sd);
        set->push_back(rhsItemTag, rhsItemVal);
    };
    auto addArrayFn = [](value::ArraySet* set) {
        auto bsonArr = BSON_ARRAY(1 << 2 << 3);
        auto [rhsItemTag, rhsItemVal] = value::copyValue(
            value::TypeTags::bsonArray, value::bitcastFrom<const char*>(bsonArr.objdata()));
        set->push_back(rhsItemTag, rhsItemVal);
    };
    auto addObjectFn = [](value::ArraySet* set) {
        auto bsonObj = BSON("c" << 1);
        auto [rhsItemTag, rhsItemVal] = value::copyValue(
            value::TypeTags::bsonObject, value::bitcastFrom<const char*>(bsonObj.objdata()));
        set->push_back(rhsItemTag, rhsItemVal);
    };
    auto addLongStringMultipleTimesFn = [&](value::ArraySet* set) {
        auto initSize = set->size();
        addLongStringFn(set);
        addLongStringFn(set);
        addLongStringFn(set);
        ASSERT(set->size() == initSize + 1)
            << "set: " << set << " should be of size " << initSize + 1;
    };
    auto addMultipleDecimalFn = [](value::ArraySet* set) {
        auto initSize = set->size();
        auto [rhsItemTag1, rhsItemVal1] = value::makeCopyDecimal(Decimal128{"3.14"});
        set->push_back(rhsItemTag1, rhsItemVal1);
        auto [rhsItemTag2, rhsItemVal2] = value::makeCopyDecimal(Decimal128{"2.71"});
        set->push_back(rhsItemTag2, rhsItemVal2);
        auto [rhsItemTag3, rhsItemVal3] = value::makeCopyDecimal(Decimal128{"3.14"});
        set->push_back(rhsItemTag3, rhsItemVal3);
        ASSERT(set->size() == initSize + 2)
            << "set: " << set << " should be of size " << initSize + 2;
    };

    // Compare ArraySets with single element of different (and mostly complex) types.
    arraySetEqualityComparisonTestGenFn(addShortStringFn, addShortStringFn);
    arraySetEqualityComparisonTestGenFn(addLongStringFn, addLongStringFn);
    arraySetEqualityComparisonTestGenFn(addArrayFn, addArrayFn);
    arraySetEqualityComparisonTestGenFn(addObjectFn, addObjectFn);
    arraySetEqualityComparisonTestGenFn(addMultipleDecimalFn, addMultipleDecimalFn);
    // Check whether adding a single complex type multiple times doesn't break the equality.
    arraySetEqualityComparisonTestGenFn(addLongStringMultipleTimesFn, addLongStringMultipleTimesFn);
    // Check whether the insertion into ArraySet is order agnostic.
    arraySetEqualityComparisonTestGenFn(
        [&](value::ArraySet* set) {
            addArrayFn(set);
            addMultipleDecimalFn(set);
            addObjectFn(set);
            addLongStringFn(set);
        },
        [&](value::ArraySet* set) {
            addObjectFn(set);
            addLongStringFn(set);
            addArrayFn(set);
            addMultipleDecimalFn(set);
        });

    // Check inequal ArraySets are actually not equal.
    arraySetInequalityComparisonTestGenFn(addShortStringFn, addLongStringFn);
    arraySetInequalityComparisonTestGenFn(addArrayFn, addObjectFn);
    arraySetInequalityComparisonTestGenFn(addMultipleDecimalFn, addObjectFn);
}

TEST_F(SbeValueTest, CompareTwoValueMapTypes) {
    using MapType = value::ValueMapType<size_t>;
    using ValueFnType = void(MapType*);
    using AssertFnType = void(const MapType&, const MapType&);

    auto arraySetComparisonTestGenFn = [](std::function<ValueFnType> lhsValueGenFn,
                                          std::function<ValueFnType> rhsValueGenFn,
                                          std::function<AssertFnType> assertFn) {
        CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kToLowerString);

        MapType lhsView{0, value::ValueHash(&collator), value::ValueEq(&collator)};
        lhsValueGenFn(&lhsView);

        MapType rhsView{0, value::ValueHash(&collator), value::ValueEq(&collator)};
        rhsValueGenFn(&rhsView);

        assertFn(lhsView, rhsView);
    };

    auto arraySetEqualityComparisonTestGenFn = [&](std::function<ValueFnType> lhsValueGenFn,
                                                   std::function<ValueFnType> rhsValueGenFn) {
        arraySetComparisonTestGenFn(
            lhsValueGenFn, rhsValueGenFn, [&](const MapType& lhs, const MapType& rhs) {
                ASSERT(lhs == rhs);
            });
    };

    auto arraySetInequalityComparisonTestGenFn = [&](std::function<ValueFnType> lhsValueGenFn,
                                                     std::function<ValueFnType> rhsValueGenFn) {
        arraySetComparisonTestGenFn(
            lhsValueGenFn, rhsValueGenFn, [&](const MapType& lhs, const MapType& rhs) {
                ASSERT(lhs != rhs);
            });
    };

    auto addShortStringKeyFn = [](MapType* set) {
        auto [rhsItemTag, rhsItemVal] = value::makeSmallString("abc"_sd);
        (*set)[{rhsItemTag, rhsItemVal}] = 1;
    };
    auto addLongStringKeyFn1 = [](MapType* set) {
        auto [rhsItemTag, rhsItemVal] = value::makeNewString("a long enough string"_sd);
        (*set)[{rhsItemTag, rhsItemVal}] = 2;
    };
    auto addLongStringKeyFn2 = [](MapType* set) {
        auto [rhsItemTag, rhsItemVal] = value::makeNewString("a long enough string"_sd);
        (*set)[{rhsItemTag, rhsItemVal}] = 12;
    };
    auto addArrayKeyFn = [](MapType* set) {
        auto bsonArr = BSON_ARRAY(1 << 2 << 3);
        auto [rhsItemTag, rhsItemVal] = value::copyValue(
            value::TypeTags::bsonArray, value::bitcastFrom<const char*>(bsonArr.objdata()));
        (*set)[{rhsItemTag, rhsItemVal}] = 3;
    };
    auto addObjectKeyFn = [](MapType* set) {
        auto bsonObj = BSON("c" << 1);
        auto [rhsItemTag, rhsItemVal] = value::copyValue(
            value::TypeTags::bsonObject, value::bitcastFrom<const char*>(bsonObj.objdata()));
        (*set)[{rhsItemTag, rhsItemVal}] = 4;
    };
    auto addLongStringMultipleTimesKeyFn = [&](MapType* set) {
        auto initSize = set->size();
        addLongStringKeyFn1(set);
        addLongStringKeyFn1(set);
        addLongStringKeyFn1(set);
        ASSERT(set->size() == initSize + 1)
            << "set: " << set << " should be of size " << initSize + 1;
    };
    auto addMultipleDecimalKeyFn = [](MapType* set) {
        auto initSize = set->size();
        auto [rhsItemTag1, rhsItemVal1] = value::makeCopyDecimal(Decimal128{"3.14"});
        (*set)[{rhsItemTag1, rhsItemVal1}] = 5;
        auto [rhsItemTag2, rhsItemVal2] = value::makeCopyDecimal(Decimal128{"2.71"});
        (*set)[{rhsItemTag2, rhsItemVal2}] = 6;
        auto [rhsItemTag3, rhsItemVal3] = value::makeCopyDecimal(Decimal128{"3.14"});
        (*set)[{rhsItemTag3, rhsItemVal3}] = 7;
        ASSERT(set->size() == initSize + 2)
            << "set: " << set << " should be of size " << initSize + 2;
    };

    // Compare MapTypes with single element of different (and mostly complex) types.
    arraySetEqualityComparisonTestGenFn(addShortStringKeyFn, addShortStringKeyFn);
    arraySetEqualityComparisonTestGenFn(addLongStringKeyFn1, addLongStringKeyFn1);
    arraySetEqualityComparisonTestGenFn(addArrayKeyFn, addArrayKeyFn);
    arraySetEqualityComparisonTestGenFn(addObjectKeyFn, addObjectKeyFn);
    arraySetEqualityComparisonTestGenFn(addMultipleDecimalKeyFn, addMultipleDecimalKeyFn);
    // Check whether adding a single complex type multiple times doesn't break the equality.
    arraySetEqualityComparisonTestGenFn(addLongStringMultipleTimesKeyFn,
                                        addLongStringMultipleTimesKeyFn);
    // Check whether the insertion into MapType is order agnostic.
    arraySetEqualityComparisonTestGenFn(
        [&](MapType* set) {
            addArrayKeyFn(set);
            addMultipleDecimalKeyFn(set);
            addObjectKeyFn(set);
            addLongStringKeyFn1(set);
        },
        [&](MapType* set) {
            addObjectKeyFn(set);
            addLongStringKeyFn1(set);
            addArrayKeyFn(set);
            addMultipleDecimalKeyFn(set);
        });

    // Check inequal MapTypes are actually not equal.
    arraySetInequalityComparisonTestGenFn(addShortStringKeyFn, addLongStringKeyFn1);
    arraySetInequalityComparisonTestGenFn(addLongStringKeyFn1, addLongStringKeyFn2);
    arraySetInequalityComparisonTestGenFn(addArrayKeyFn, addObjectKeyFn);
    arraySetInequalityComparisonTestGenFn(addMultipleDecimalKeyFn, addObjectKeyFn);
}

}  // namespace mongo::sbe
