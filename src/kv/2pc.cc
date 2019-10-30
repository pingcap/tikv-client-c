#include <pingcap/kv/2pc.h>
#include <pingcap/kv/RegionClient.h>
#include <pingcap/kv/Txn.h>

namespace pingcap
{
namespace kv
{

TwoPhaseCommitter::TwoPhaseCommitter(Txn * txn)
{
    commited = false;
    lock_ttl = 3000;
    txn->walkBuffer([&](const std::string & key, const std::string & value) {
        keys.push_back(key);
        mutations.emplace(key, value);
    });
    cluster = txn->cluster;
    start_ts = txn->start_ts;
    primary_lock = keys[0];
}

void TwoPhaseCommitter::execute()
{
    try
    {
        Backoffer prewrite_bo(prewriteMaxBackoff);
        prewriteKeys(prewrite_bo, keys);
        commit_ts = cluster->pd_client->getTS();
        // TODO: check expired
        Backoffer commit_bo(commitMaxBackoff);
        commitKeys(commit_bo, keys);
        // TODO: Process commit exception
    }
    catch (Exception & e)
    {
        if (!commited)
        {
            // TODO: Rollback keys.
        }
        e.rethrow();
    }
}

void TwoPhaseCommitter::prewriteSingleBatch(Backoffer & bo, const BatchKeys & batch)
{
    auto req = new kvrpcpb::PrewriteRequest();
    for (const std::string & key : batch.keys)
    {
        auto * mut = req->add_mutations();
        mut->set_key(key);
        mut->set_value(mutations[key]);
    }
    req->set_primary_lock(keys[0]);
    req->set_start_version(start_ts);
    req->set_lock_ttl(lock_ttl);
    // TODO: use corrent txn size.
    req->set_txn_size(500);
    req->set_primary_lock(primary_lock);

    auto rpc_call = std::make_shared<pingcap::kv::RpcCall<kvrpcpb::PrewriteRequest>>(req);
    RegionClient region_client(cluster->region_cache, cluster->rpc_client, batch.region);
    for (;;)
    {
        try
        {
            region_client.sendReqToRegion(bo, rpc_call);
        }
        catch (Exception & e)
        {
            // Region Error.
            bo.backoff(boRegionMiss, e);
            prewriteKeys(bo, batch.keys);
            return;
        }

        auto * res = rpc_call->getResp();

        if (res->errors_size() != 0)
        {
            // TODO resolve lock and retry
            throw Exception("meet lock error", LockError);
        }

        return;
    }
}

void TwoPhaseCommitter::commitSingleBatch(Backoffer & bo, const BatchKeys & batch)
{
    auto req = new kvrpcpb::CommitRequest();
    for (const auto & key : batch.keys)
    {
        req->add_keys(key);
    }
    req->set_start_version(start_ts);
    req->set_commit_version(commit_ts);

    auto rpc_call = std::make_shared<pingcap::kv::RpcCall<kvrpcpb::CommitRequest>>(req);
    RegionClient region_client(cluster->region_cache, cluster->rpc_client, batch.region);
    try
    {
        region_client.sendReqToRegion(bo, rpc_call);
    }
    catch (Exception & e)
    {
        bo.backoff(boRegionMiss, e);
        commitKeys(bo, batch.keys);
        return;
    }
    auto * res = rpc_call->getResp();
    if (res->has_error())
    {
        throw Exception("meet errors", LockError);
    }

    commited = true;
}

} // namespace kv
} // namespace pingcap
