#pragma once

#include <gtest/gtest.h>
#include <pingcap/kv/Cluster.h>


namespace
{

using namespace pingcap;
using namespace pingcap::kv;

inline ClusterPtr createCluster(const std::vector<std::string> & pd_addrs)
{
    ClusterConfig config;
    config.engine_key = "engine";
    config.engine_value = "zone";
    return std::make_unique<Cluster>(pd_addrs, config);
}

} // namespace
