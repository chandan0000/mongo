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
#pragma once

#include "mongo/db/process_health/fault.h"

#include "mongo/db/service_context.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace process_health {

/**
 * Internal implementation of the Fault class.
 * @see Fault
 */
class FaultImpl : public FaultInternal {
public:
    explicit FaultImpl(ClockSource* clockSource);

    ~FaultImpl() override = default;

    // Fault interface.

    UUID getId() const override;

    double getSeverity() const override;

    Milliseconds getActiveFaultDuration() const override;

    Milliseconds getDuration() const override;

    void appendDescription(BSONObjBuilder* builder) const override;

    // FaultFacetsContainer interface.

    std::vector<FaultFacetPtr> getFacets() const override;

    FaultFacetPtr getFaultFacet(FaultFacetType type) override;

    void updateWithSuppliedFacet(FaultFacetType type, FaultFacetPtr facet) override;

    void garbageCollectResolvedFacets() override;

private:
    const UUID _id = UUID::gen();

    ClockSource* const _clockSource;
    const Date_t _startTime;

    mutable Mutex _mutex = MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "FaultImpl::_mutex");
    // We don't need a map by type because we expect to have only few facets.
    // Linear search is much faster, we want to avoid any lock contention here.
    std::deque<FaultFacetPtr> _facets;
};

}  // namespace process_health
}  // namespace mongo
