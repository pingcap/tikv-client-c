#include <pingcap/kv/RegionClient.h>

namespace pingcap
{
namespace kv
{

void RegionClient::onRegionError(Backoffer & bo, RPCContextPtr rpc_ctx, const errorpb::Error & err)
{
    if (err.has_not_leader())
    {
        auto not_leader = err.not_leader();
        if (not_leader.has_leader())
        {
            cache->updateLeader(bo, rpc_ctx->region, not_leader.leader().store_id());
            bo.backoff(boUpdateLeader, Exception("not leader", LeaderNotMatch));
        }
        else
        {
            cache->dropRegion(rpc_ctx->region);
            bo.backoff(boRegionMiss, Exception("not leader", LeaderNotMatch));
        }
        return;
    }

    if (err.has_store_not_match())
    {
        cache->dropStore(rpc_ctx->peer.store_id());
        return;
    }

    if (err.has_epoch_not_match())
    {
        cache->onRegionStale(bo, rpc_ctx, err.epoch_not_match());
        // Epoch not match should not retry, throw exception directly !!
        throw Exception("Region epoch not match!", RegionEpochNotMatch);
    }

    if (err.has_server_is_busy())
    {
        bo.backoff(boServerBusy, Exception("server busy", ServerIsBusy));
        return;
    }

    if (err.has_stale_command())
    {
        return;
    }

    if (err.has_raft_entry_too_large())
    {
        throw Exception("entry too large", RaftEntryTooLarge);
    }

    cache->dropRegion(rpc_ctx->region);
}

void RegionClient::onSendFail(Backoffer & bo, const Exception & e, RPCContextPtr rpc_ctx)
{
    cache->onSendReqFail(rpc_ctx, e);
    // Retry on send request failure when it's not canceled.
    // When a store is not available, the leader of related region should be elected quickly.
    bo.backoff(boTiKVRPC, e);
}

} // namespace kv
} // namespace pingcap
