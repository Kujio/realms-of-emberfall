#include "db/db_worker.hpp"

#include <spdlog/spdlog.h>

namespace ef::db {

DbWorker::DbWorker(std::unique_ptr<Repository> repo, Mode mode) : repo_(std::move(repo)), mode_(mode) {
  if (mode_ == Mode::Threaded) thread_ = std::thread([this] { run(); });
}

DbWorker::~DbWorker() { stop_and_flush(); }

void DbWorker::post(Job job) {
  if (mode_ == Mode::Inline) {
    try {
      job(*repo_, *this);
    } catch (const std::exception& e) {
      spdlog::error("db job failed: {}", e.what());
    }
    return;
  }
  {
    std::lock_guard lock(mu_);
    if (stopping_) {
      spdlog::warn("db job dropped: worker is stopping");
      return;
    }
    jobs_.push_back(std::move(job));
  }
  cv_.notify_one();
}

void DbWorker::complete(Completion done) {
  std::lock_guard lock(done_mu_);
  done_.push_back(std::move(done));
}

std::size_t DbWorker::drain(std::size_t max_items) {
  std::size_t ran = 0;
  while (ran < max_items) {
    Completion c;
    {
      std::lock_guard lock(done_mu_);
      if (done_.empty()) break;
      c = std::move(done_.front());
      done_.pop_front();
    }
    c();
    ++ran;
  }
  return ran;
}

void DbWorker::stop_and_flush() {
  {
    std::lock_guard lock(mu_);
    if (stopping_) return;
    stopping_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void DbWorker::wait_idle() {
  if (mode_ == Mode::Inline) return;  // every job already ran inside post()
  std::unique_lock lock(mu_);
  // The worker thread empties the queue before it exits, so this also returns once it has stopped.
  idle_cv_.wait(lock, [this] { return jobs_.empty() && !running_; });
}

bool DbWorker::settle(int max_rounds) {
  for (int round = 0; round < max_rounds; ++round) {
    wait_idle();
    // A bounded drain per round: an inline worker's completions can post (and so run) more at once.
    if (drain(4096) == 0) return true;
  }
  return false;
}

std::size_t DbWorker::queued_jobs() const {
  std::lock_guard lock(mu_);
  return jobs_.size();
}

void DbWorker::run() {
  for (;;) {
    Job job;
    {
      std::unique_lock lock(mu_);
      cv_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
      if (jobs_.empty()) return;  // stopping and fully flushed
      job = std::move(jobs_.front());
      jobs_.pop_front();
      running_ = true;
    }
    try {
      job(*repo_, *this);
    } catch (const std::exception& e) {
      spdlog::error("db job failed: {}", e.what());
    }
    {
      std::lock_guard lock(mu_);
      running_ = false;
    }
    idle_cv_.notify_all();
  }
}

}  // namespace ef::db
