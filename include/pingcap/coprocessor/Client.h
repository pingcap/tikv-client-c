#pragma once
#include <pingcap/kv/Cluster.h>
#include <pingcap/kv/RegionClient.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>

namespace pingcap
{
namespace coprocessor
{

enum ReqType : int16_t
{
    Select = 101,
    Index = 102,
    DAG = 103,
    Analyze = 104,
    Checksum = 105,
};

struct KeyRange
{
    std::string start_key;
    std::string end_key;
    KeyRange(const std::string & start_key_, const std::string & end_key_) :
    start_key(start_key_), end_key(end_key_) {}
    void set_pb_range(::coprocessor::KeyRange * range) const
    {
        range->set_start(start_key);
        range->set_end(end_key);
    }
};

struct Request
{
    int64_t tp;
    uint64_t start_ts;
    std::string data;
    std::vector<KeyRange> ranges;
    int64_t schema_version;
};

struct copTask
{
    kv::RegionVerID region_id;
    std::vector<KeyRange> ranges;
    Request * req;
    kv::StoreType store_type;
};

class ResponseIter
{
public:
    struct Result {
        std::shared_ptr<::coprocessor::Response> resp;
        Exception error;

        Result() {}
        Result(std::shared_ptr<::coprocessor::Response> resp_) : resp(resp_) {}
        Result(const Exception & err) : error(err) {}

        const std::string & data() {
            return resp->data();
        }
    };

    ResponseIter(Request * req_, std::vector<copTask> && tasks_, kv::Cluster * cluster_, int concurrency_)
        : cop_req(req_), tasks(std::move(tasks_)), cluster(cluster_), concurrency(concurrency_), cancelled(false), log(&Logger::get("pingcap/coprocessor"))
    {}

    ~ResponseIter() {
        cancelled = true;
        for (auto it = worker_threads.begin(); it != worker_threads.end(); it++)
        {
            it->join();
        }
    }

    // send all tasks.
    void open()
    {
        unfinished_thread = concurrency;
        for (int i = 0; i < concurrency; i++)
        {
            std::thread worker(&ResponseIter::thread, this);
            worker_threads.push_back(std::move(worker));
        }
        log->debug("coprocessor has " + std::to_string(tasks.size()) + " tasks.");
    }

    std::pair<Result, bool> next()
    {
        std::unique_lock<std::mutex> lk(results_mutex);
        cond_var.wait(lk, [this]{return unfinished_thread == 0 || cancelled || results.size() > 0; });
        if (cancelled)
        {
            return std::make_pair(Result(), false);
        }
        if (results.size() > 0)
        {
            auto ret =  std::make_pair(results.front(), true);
            results.pop();
            return ret;
        }
        else {
            return std::make_pair(Result(), false);
        }
    }

private:
    void thread() {
        while(true) {
            if (cancelled) {
                log->information("cop task has been cancelled");
                unfinished_thread--;
                return;
            }
            std::unique_lock<std::mutex> lk(fetch_mutex);
            if (tasks.size() == task_index) {
                unfinished_thread--;
                return;
            }
            const copTask & task = tasks[task_index];
            task_index ++;
            lk.unlock();
            handle_task(task);
        }
    }

    std::vector<copTask> handle_task_impl(kv::Backoffer & bo, const copTask & task);
    void handle_task(const copTask & task);

    Request * cop_req;
    size_t task_index = 0;
    std::vector<copTask> tasks;
    std::vector<std::thread> worker_threads;

    kv::Cluster * cluster;
    int concurrency;

    std::mutex results_mutex;
    std::mutex fetch_mutex;

    std::queue<Result> results;
    Exception cop_error;

    std::atomic_int unfinished_thread;
    std::atomic_bool cancelled;
    std::condition_variable cond_var;

    Logger * log;
};

struct Client
{
    static ResponseIter send(kv::Cluster * cluster, Request * cop_req, int concurrency, kv::StoreType store_type = kv::TiKV);
};

} // namespace coprocessor
} // namespace pingcap
