/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2014 Facebook, Inc. (http://www.facebook.com)     |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/vm/treadmill.h"

#include <list>
#include <atomic>
#include <vector>
#include <memory>
#include <algorithm>

#include <sys/time.h>

#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>
#include <signal.h>

#include "hphp/util/logger.h"
#include "hphp/util/process.h"
#include "hphp/util/trace.h"
#include "hphp/util/rank.h"
#include "hphp/util/service-data.h"
#include "hphp/runtime/vm/jit/mc-generator.h"

namespace HPHP {  namespace Treadmill {

TRACE_SET_MOD(treadmill);

namespace {

//////////////////////////////////////////////////////////////////////

const int64_t ONE_SEC_IN_MICROSEC = 1000000;

struct RequestInfo {
  GenCount  startTime;
  pthread_t pthreadId;
};

pthread_mutex_t s_genLock = PTHREAD_MUTEX_INITIALIZER;
const GenCount kIdleGenCount = 0; // not processing any requests.
std::vector<RequestInfo> s_inflightRequests;
GenCount s_latestCount = 0;
std::atomic<GenCount> s_oldestRequestInFlight(0);

/*
 * We assign local, unique indexes to each thread, with hopes that
 * they are densely packed.
 *
 * The plan here is that each thread starts with s_thisThreadIdx as
 * -1.  And the first time a thread starts using the Treadmill it
 * allocates a new thread id from s_nextThreadIdx with fetch_add.
 */
std::atomic<int64_t> s_nextThreadIdx{0};
__thread int64_t s_thisThreadIdx{-1};

//////////////////////////////////////////////////////////////////////

/*
 * The next 2 functions should be used to manage the generation count/time
 * in the treadmill for both the requests and the work items.
 * The pattern is to call getTime() outside of the lock and correctTime()
 * while holding the lock.
 * That pattern guarantees a monotonically increasing counter.
 * The resolution being microseconds should give us all the room we need
 * to accommodate requests and work items at any conceivable rate and
 * correctTime() should give us correct behavior at any granularity of
 * gettimeofday().
 */

/*
 * Return the current time in microseconds.
 * Usually called outside of the lock.
 */
GenCount getTime() {
  struct timeval time;
  gettimeofday(&time, nullptr);
  return time.tv_sec * ONE_SEC_IN_MICROSEC + time.tv_usec;
}

/*
 * Return a monotonically increasing time given the last time recorded.
 * This must be called while holding the lock.
 */
GenCount correctTime(GenCount time) {
  s_latestCount = time <= s_latestCount ? s_latestCount + 1 : time;
  return s_latestCount;
}

struct GenCountGuard {
  GenCountGuard() {
    checkRank(RankTreadmill);
    pthread_mutex_lock(&s_genLock);
    pushRank(RankTreadmill);
  }
  ~GenCountGuard() {
    popRank(RankTreadmill);
    pthread_mutex_unlock(&s_genLock);
  }
};

//////////////////////////////////////////////////////////////////////

pthread_t getOldestRequestThreadId() {
  int64_t oldestStart = s_oldestRequestInFlight.load(std::memory_order_relaxed);
  for (auto& req : s_inflightRequests) {
    if (req.startTime == oldestStart) return req.pthreadId;
  }
  not_reached();
}

void checkOldest() {
  int64_t limit =
    RuntimeOption::MaxRequestAgeFactor * RuntimeOption::RequestTimeoutSeconds;
  if (!limit) return;

  int64_t ageOldest = getAgeOldestRequest();
  if (ageOldest > limit) {
    auto msg = folly::format("Oldest request has been running for {} "
                             "seconds. Aborting the server.", ageOldest).str();
    Logger::Error(msg);
    pthread_t oldestTid = getOldestRequestThreadId();
    pthread_kill(oldestTid, SIGABRT);
  }
}

void refreshStats() {
  static ServiceData::ExportedCounter* s_oldestRequestAgeStat =
    ServiceData::createCounter("treadmill.age");
  s_oldestRequestAgeStat->setValue(getAgeOldestRequest());
}

}

struct PendingTriggers : std::list<std::unique_ptr<WorkItem>> {
  ~PendingTriggers() {
    s_destroyed = true;
  }
  static bool s_destroyed;
};

static PendingTriggers s_tq;
bool PendingTriggers::s_destroyed = false;
void enqueueInternal(std::unique_ptr<WorkItem> gt) {
  if (PendingTriggers::s_destroyed) {
    return;
  }
  GenCount time = getTime();
  {
    GenCountGuard g;
    gt->m_gen = correctTime(time);
    s_tq.emplace_back(std::move(gt));
  }
}

void startRequest() {
  if (UNLIKELY(s_thisThreadIdx == -1)) {
    s_thisThreadIdx = s_nextThreadIdx.fetch_add(1);
  }
  auto const threadIdx = s_thisThreadIdx;

  GenCount startTime = getTime();
  {
    GenCountGuard g;
    refreshStats();
    checkOldest();
    if (threadIdx >= s_inflightRequests.size()) {
      s_inflightRequests.resize(threadIdx + 1, {kIdleGenCount, 0});
    } else {
      assert(s_inflightRequests[threadIdx].startTime == kIdleGenCount);
    }
    s_inflightRequests[threadIdx].startTime = correctTime(startTime);
    s_inflightRequests[threadIdx].pthreadId = Process::GetThreadId();
    FTRACE(1, "threadIdx {} pthreadId {} start @gen {}\n", threadIdx,
           s_inflightRequests[threadIdx].pthreadId,
           s_inflightRequests[threadIdx].startTime);
    if (s_oldestRequestInFlight.load(std::memory_order_relaxed) == 0) {
      s_oldestRequestInFlight = s_inflightRequests[threadIdx].startTime;
    }
  }
}

void finishRequest() {
  auto const threadIdx = s_thisThreadIdx;
  assert(threadIdx != -1);
  FTRACE(1, "tid {} finish\n", threadIdx);
  std::vector<std::unique_ptr<WorkItem>> toFire;
  {
    GenCountGuard g;
    assert(s_inflightRequests[threadIdx].startTime != kIdleGenCount);
    GenCount finishedRequest = s_inflightRequests[threadIdx].startTime;
    s_inflightRequests[threadIdx].startTime = kIdleGenCount;

    // After finishing a request, check to see if we've allowed any triggers
    // to fire and update the time of the oldest request in flight.
    // However if the request just finished is not the current oldest we
    // don't need to check anything as there cannot be any WorkItem to run.
    if (s_oldestRequestInFlight.load(std::memory_order_relaxed) ==
        finishedRequest) {
      GenCount limit = s_latestCount + 1;
      for (auto& val : s_inflightRequests) {
        if (val.startTime != kIdleGenCount && val.startTime < limit) {
          limit = val.startTime;
        }
      }
      // update "oldest in flight" or kill it if there are no running requests
      s_oldestRequestInFlight = limit == s_latestCount + 1 ? 0 : limit;

      // collect WorkItem to run
      auto it = s_tq.begin();
      auto end = s_tq.end();
      while (it != end) {
        TRACE(2, "considering delendum %d\n", int((*it)->m_gen));
        if ((*it)->m_gen >= limit) {
          TRACE(2, "not unreachable! %d\n", int((*it)->m_gen));
          break;
        }
        toFire.emplace_back(std::move(*it));
        it = s_tq.erase(it);
      }
    }
  }
  for (unsigned i = 0; i < toFire.size(); ++i) {
    toFire[i]->run();
  }
}

/*
 * Return the start time of the oldest request in seconds, rounded such that
 * time(nullptr) >= getOldestStartTime() is guaranteed to be true.
 *
 * Subtract s_nextThreadIdx because if n threads start at the same time,
 * one of their start times will be increased by n-1 (and we need to subtract
 * 1 anyway, to deal with exact seconds).
 */
int64_t getOldestStartTime() {
  int64_t time = s_oldestRequestInFlight.load(std::memory_order_relaxed);
  return (time - s_nextThreadIdx)/ ONE_SEC_IN_MICROSEC + 1;
}

int64_t getAgeOldestRequest() {
  int64_t start = s_oldestRequestInFlight.load(std::memory_order_relaxed);
  if (start == 0) return 0; // no request in flight
  int64_t time = getTime() - start;
  return time / ONE_SEC_IN_MICROSEC;
}

void deferredFree(void* p) {
  enqueue([p] { free(p); });
}

}}
