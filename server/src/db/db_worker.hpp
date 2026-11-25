// Runs persistence (and password hashing) off the game thread.
//
// The game thread posts Jobs; a Job runs on the DB thread with the Repository and may call
// complete() to schedule a Completion that runs back on the game thread during drain().
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "db/repository.hpp"

namespace ef::db {

class DbWorker {
 public:
  using Completion = std::function<void()>;
  using Job = std::function<void(Repository&, DbWorker&)>;

  enum class Mode { Threaded, Inline };

  DbWorker(std::unique_ptr<Repository> repo, Mode mode);
  ~DbWorker();
  DbWorker(const DbWorker&) = delete;
  DbWorker& operator=(const DbWorker&) = delete;

  void post(Job job);
  // Called from a Job: queue a callback for the game thread.
  void complete(Completion done);
  // Game thread: run up to max_items pending completions. Returns how many ran.
  std::size_t drain(std::size_t max_items = 256);
  // Runs every queued job, then stops the thread. Remaining completions can still be drained.
  void stop_and_flush();
  // Game thread: blocks until every job posted so far has run, so their completions are ready to drain
  // (used at shutdown, to see which writes failed while the worker can still retry them).
  void wait_idle();
  std::size_t queued_jobs() const;

 private:
  void run();

  std::unique_ptr<Repository> repo_;
  Mode mode_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Job> jobs_;
  bool stopping_ = false;
  std::mutex done_mu_;
  std::deque<Completion> done_;
  std::thread thread_;
};

}  // namespace ef::db
