#pragma once
#include <cstdint>
namespace oceanbase { namespace observer {
// Persisted values: never reorder. retry_count is the number of retries already scheduled.
struct AiColumnTaskState
{
  enum State { WAITING = 0, RUNNING = 1, SUCCEEDED = 2, RETRY_WAIT = 3, FAILED = 4 };
  static bool terminal(int64_t state) { return state == SUCCEEDED || state == FAILED; }
  static bool claimable(int64_t state, int64_t eligible, int64_t lease, int64_t now)
  {
    return ((state == WAITING || state == RETRY_WAIT) && eligible <= now)
        || (state == RUNNING && lease <= now);
  }
  static State failure_state(int64_t retries, int64_t maximum)
  { return retries < maximum ? RETRY_WAIT : FAILED; }
  static bool accept_result(int64_t state, uint64_t owner, uint64_t expected,
                            int64_t lease, int64_t now)
  { return state == RUNNING && owner == expected && lease > now; }
  static int64_t retry_delay_us(int64_t retries)
  { return retries > 5 ? 30000000 : (1000000LL << (retries > 0 ? retries - 1 : 0)); }
};
} }
