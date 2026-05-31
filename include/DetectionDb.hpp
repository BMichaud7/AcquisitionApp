#pragma once
/**
 * @file DetectionDb.hpp
 * @brief Asynchronous PostgreSQL writer for Detection records.
 *
 * DetectionDb accepts Detection objects from any thread via push() and
 * writes them to PostgreSQL in background batches, decoupling the sweep
 * loop from database I/O latency.
 *
 * Batches are flushed when either:
 * - @p batch_size detections have accumulated, or
 * - @p flush_interval elapsed since the last flush.
 */
#include "Types.hpp"
#include <au/units/seconds.hh>
#include <au/prefix.hh>
#include <pqxx/pqxx>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <atomic>

namespace acq {

/**
 * @brief Writes Detection records to PostgreSQL in background batches.
 *
 * Non-copyable.  push() is safe to call from any thread.
 * The destructor flushes remaining records and joins the writer thread.
 */
class DetectionDb {
public:
    /**
     * @brief Open a PostgreSQL connection and start the writer thread.
     * @param conn_str        libpqxx connection string.
     * @param batch_size      Flush after accumulating this many records.
     * @param flush_interval  Flush after this interval even if batch is not full.
     * @throws std::exception if the initial connection fails.
     */
    explicit DetectionDb(const std::string& conn_str,
                         int batch_size = 100,
                         au::QuantityD<au::Seconds> flush_interval = au::milli(au::seconds)(500.0));
    ~DetectionDb();

    DetectionDb(const DetectionDb&)            = delete;
    DetectionDb& operator=(const DetectionDb&) = delete;

    /**
     * @brief Enqueue a Detection for writing.
     *
     * Returns immediately; the detection is written asynchronously.
     * Thread-safe.
     *
     * @param d Detection to persist.
     */
    void push(const Detection& d);

private:
    pqxx::connection            conn_;
    std::queue<Detection>       queue_;
    std::mutex                  mutex_;
    std::condition_variable     cv_;
    std::atomic<bool>           stopped_{false};
    std::thread                 thread_;
    int                         batch_size_;
    au::QuantityD<au::Seconds>  flush_interval_;

    void workerLoop();
    void flush(std::vector<Detection>& batch);
};

} // namespace acq
