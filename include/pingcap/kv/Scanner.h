#pragma once

#include <pingcap/Exception.h>
#include <pingcap/kv/Backoff.h>
#include <pingcap/kv/Snapshot.h>

namespace pingcap
{
namespace kv
{

struct Scanner
{
    Snapshot snap;
    std::string next_start_key;
    std::string end_key;
    int batch;

    std::vector<::kvrpcpb::KvPair> cache;
    size_t idx;
    bool valid;
    bool eof;


    Logger * log;

    Scanner(Snapshot & snapshot_, std::string start_key_, std::string end_key_, int batch_)
        : snap(snapshot_),
          next_start_key(start_key_),
          end_key(end_key_),
          batch(batch_),
          idx(0),
          valid(true),
          eof(false),
          log(&Logger::get("pingcap.tikv"))
    {
        next();
    }

    void next();

    std::string key()
    {
        if (valid)
            return cache[idx].key();
        return "";
    }

    std::string value()
    {
        if (valid)
            return cache[idx].value();
        return "";
    }

private:
    void getData(Backoffer & bo);
};

// end of namespace.
} // namespace kv
} // namespace pingcap
