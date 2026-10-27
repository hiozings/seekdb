#include <cassert>
#include "observer/ai_service/ai_column_worker_state.h"
using oceanbase::observer::AiColumnTaskState;
int main()
{
  using S = AiColumnTaskState;
  assert(S::claimable(S::WAITING, 10, 0, 10));
  assert(!S::claimable(S::RETRY_WAIT, 11, 0, 10));
  assert(!S::claimable(S::RUNNING, 0, 11, 10));
  assert(S::claimable(S::RUNNING, 0, 10, 10));
  assert(!S::claimable(S::SUCCEEDED, 0, 0, 10));
  assert(!S::claimable(S::FAILED, 0, 0, 10));
  assert(S::failure_state(0, 0) == S::FAILED);
  assert(S::failure_state(0, 2) == S::RETRY_WAIT);
  assert(S::failure_state(2, 2) == S::FAILED);
  assert(S::terminal(S::SUCCEEDED) && S::terminal(S::FAILED));
  assert(!S::terminal(S::RETRY_WAIT));
  assert(S::accept_result(S::RUNNING, 17, 17, 20, 19));
  assert(!S::accept_result(S::RUNNING, 17, 18, 20, 19));
  assert(!S::accept_result(S::RUNNING, 17, 17, 20, 20));
  assert(!S::accept_result(S::FAILED, 17, 17, 20, 19));
  assert(S::retry_delay_us(1) >= 1000000);
  assert(S::retry_delay_us(100000) <= 30000000);
}
