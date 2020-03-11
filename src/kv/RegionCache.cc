#include <pingcap/Exception.h>
#include <pingcap/kv/RegionCache.h>
#include <pingcap/pd/CodecClient.h>

namespace pingcap
{
namespace kv
{

RPCContextPtr RegionCache::getRPCContext(Backoffer & bo, const RegionVerID & id, const StoreType store_type)
{
    for (;;)
    {
        auto region = getRegionByIDFromCache(id);
        if (region == nullptr)
            return nullptr;
        const auto & meta = region->meta;
        std::vector<metapb::Peer> peers;
        if (store_type == TiKV)
            peers.push_back(region->peer);
        else
            peers = selectLearner(bo, meta);

        for (auto peer : peers)
        {
            std::string addr = getStore(bo, peer.store_id()).addr;
            if (addr == "") {
                //dropRegion(id);
                dropStore(peer.store_id());
                bo.backoff(boRegionMiss,
                           Exception("miss store, region id is: " + std::to_string(id.id) + " store id is: " +
                                     std::to_string(peer.store_id()),
                                     StoreNotReady));
                continue;
            }
            return std::make_shared<RPCContext>(id, meta, peer, addr);
        }
        dropRegion(id);
        bo.backoff(boRegionMiss,
                   Exception("miss store, region id is: " + std::to_string(id.id), StoreNotReady));
    }
}

    RegionPtr RegionCache::getRegionByIDFromCache(const RegionVerID & id)
{
    std::shared_lock<std::shared_mutex> lock(region_mutex);
    auto it = regions.find(id);
    if (it == regions.end())
        return nullptr;
    return it->second;
}

RegionPtr RegionCache::getRegionByID(Backoffer & bo, const RegionVerID & id)
{
    std::shared_lock<std::shared_mutex> lock(region_mutex);
    auto it = regions.find(id);
    if (it == regions.end())
    {
        lock.unlock();

        auto region = loadRegionByID(bo, id.id);

        insertRegionToCache(region);

        return region;
    }
    return it->second;
}

KeyLocation RegionCache::locateKey(Backoffer & bo, const std::string & key)
{
    RegionPtr region = searchCachedRegion(key);
    if (region != nullptr)
    {
        return KeyLocation(region->verID(), region->startKey(), region->endKey());
    }

    region = loadRegionByKey(bo, key);

    insertRegionToCache(region);

    return KeyLocation(region->verID(), region->startKey(), region->endKey());
}

// selectLearner select all learner peers.
std::vector<metapb::Peer> RegionCache::selectLearner(Backoffer & bo, const metapb::Region & meta)
{
    std::vector<metapb::Peer> learners;
    for (int i = 0; i < meta.peers_size(); i++)
    {
        auto peer = meta.peers(i);
        if (peer.is_learner())
        {
            auto store_id = peer.store_id();
            auto labels = getStore(bo, store_id).labels;
            if (labels[learner_key] == learner_value)
            {
                learners.push_back(peer);
            }
        }
    }
    return learners;
}

RegionPtr RegionCache::loadRegionByID(Backoffer & bo, uint64_t region_id)
{
    for (;;)
    {
        try
        {
            auto [meta, leader] = pd_client->getRegionByID(region_id);

            // If the region is not found in cache, it must be out of date and already be cleaned up. We can
            // skip the RPC by returning RegionError directly.
            if (!meta.IsInitialized())
            {
                throw Exception("region not found for regionID " + std::to_string(region_id), RegionUnavailable);
            }
            if (meta.peers_size() == 0)
            {
                throw Exception("Receive Region with no peer", RegionUnavailable);
            }

            RegionPtr region = std::make_shared<Region>(meta, meta.peers(0), selectLearner(bo, meta));
            if (leader.IsInitialized())
            {
                region->switchPeer(leader.store_id());
            }
            log->debug("load region id: " + std::to_string(region->meta.id()) + " leader store id " + std::to_string(leader.store_id()));
            return region;
        }
        catch (const Exception & e)
        {
            bo.backoff(boPDRPC, e);
        }
    }
}

RegionPtr RegionCache::loadRegionByKey(Backoffer & bo, const std::string & key)
{
    for (;;)
    {
        try
        {
            auto [meta, leader] = pd_client->getRegionByKey(key);
            if (!meta.IsInitialized())
            {
                throw Exception("region not found for region key " + key, RegionUnavailable);
            }
            if (meta.peers_size() == 0)
            {
                throw Exception("Receive Region with no peer", RegionUnavailable);
            }
            RegionPtr region = std::make_shared<Region>(meta, meta.peers(0), selectLearner(bo, meta));
            if (leader.IsInitialized())
            {
                region->switchPeer(leader.store_id());
            }
            return region;
        }
        catch (const Exception & e)
        {
            bo.backoff(boPDRPC, e);
        }
    }
}

metapb::Store RegionCache::loadStore(Backoffer & bo, uint64_t id)
{
    for (;;)
    {
        try
        {
            // TODO:: The store may be not ready, it's better to check store's state.
            const auto & store = pd_client->getStore(id);
            log->information("load store id " + std::to_string(id) + " address %s", store.address());
            return store;
        }
        catch (Exception & e)
        {
            bo.backoff(boPDRPC, e);
        }
    }
}

Store RegionCache::reloadStore(Backoffer & bo, uint64_t id)
{
    auto store = loadStore(bo, id);
    std::map<std::string, std::string> labels;
    for (size_t i = 0; i < store.labels_size(); i++)
    {
        labels[store.labels(i).key()] = store.labels(i).value();
    }
    auto it = stores.emplace(id, Store(id, store.address(), store.peer_address(), labels));
    return it.first->second;
}

Store RegionCache::getStore(Backoffer & bo, uint64_t id)
{
    std::lock_guard<std::mutex> lock(store_mutex);
    auto it = stores.find(id);
    if (it != stores.end())
    {
        return (it->second);
    }
    return reloadStore(bo, id);
}

RegionPtr RegionCache::searchCachedRegion(const std::string & key)
{
    std::shared_lock<std::shared_mutex> lock(region_mutex);
    auto it = regions_map.upper_bound(key);
    if (it != regions_map.end() && it->second->contains(key))
    {
        return it->second;
    }
    // An empty string is considered to be largest string in order.
    if (regions_map.begin() != regions_map.end() && regions_map.begin()->second->contains(key))
    {
        return regions_map.begin()->second;
    }
    return nullptr;
}

void RegionCache::insertRegionToCache(RegionPtr region)
{
    std::unique_lock<std::shared_mutex> lock(region_mutex);
    regions_map[region->endKey()] = region;
    regions[region->verID()] = region;
}

void RegionCache::dropRegion(const RegionVerID & region_id)
{
    std::unique_lock<std::shared_mutex> lock(region_mutex);
    log->information("try drop region " + region_id.toString());
    auto iter_by_id = regions.find(region_id);
    if (iter_by_id != regions.end())
    {
        auto iter_by_key = regions_map.find(iter_by_id->second->endKey());
        if (iter_by_key != regions_map.end())
        {
            regions_map.erase(iter_by_key);
        }
        regions.erase(iter_by_id);
        log->information("drop region " + std::to_string(region_id.id) + " because of send failure");
    }
}

void RegionCache::dropStore(uint64_t failed_store_id)
{
    std::lock_guard<std::mutex> lock(store_mutex);
    if (stores.erase(failed_store_id))
    {
        log->information("drop store " + std::to_string(failed_store_id) + " because of send failure");
    }
}

void RegionCache::onSendReqFail(RPCContextPtr & ctx, const Exception & exc)
{
    const auto & failed_region_id = ctx->region;
    uint64_t failed_store_id = ctx->peer.store_id();
    dropRegion(failed_region_id);
    dropStore(failed_store_id);
}

void RegionCache::updateLeader(Backoffer & bo, const RegionVerID & region_id, uint64_t leader_store_id)
{
    auto region = getRegionByID(bo, region_id);
    if (!region->switchPeer(leader_store_id))
    {
        dropRegion(region_id);
    }
}

void RegionCache::onRegionStale(Backoffer & bo, RPCContextPtr ctx, const errorpb::EpochNotMatch & stale_epoch)
{
    log->information("region stale for region %d", (int)ctx->region.id);

    dropRegion(ctx->region);

    for (int i = 0; i < stale_epoch.current_regions_size(); i++)
    {
        auto meta = stale_epoch.current_regions(i);
        if (auto * pd = static_cast<pd::CodecClient *>(pd_client.get()))
        {
            pd->processRegionResult(meta);
        }
        RegionPtr region = std::make_shared<Region>(meta, meta.peers(0), selectLearner(bo, meta));
        region->switchPeer(ctx->peer.store_id());
        insertRegionToCache(region);
    }
}

std::pair<std::unordered_map<RegionVerID, std::vector<std::string>>, RegionVerID> RegionCache::groupKeysByRegion(
    Backoffer & bo, const std::vector<std::string> & keys)
{
    std::unordered_map<RegionVerID, std::vector<std::string>> result_map;
    KeyLocation loc;
    RegionVerID first;
    for (size_t i = 0; i < keys.size(); i++)
    {
        const std::string & key = keys[i];
        if (i == 0 || !loc.contains(key))
        {
            loc = locateKey(bo, key);
            if (i == 0)
                first = loc.region;
        }
        result_map[loc.region].push_back(key);
    }
    return std::make_pair(result_map, first);
}

} // namespace kv
} // namespace pingcap
