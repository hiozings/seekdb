#pragma once
#include "share/ob_thread_pool.h"
namespace oceanbase { namespace observer {
// Durable queue consumer. Only short claim/publication transactions take write locks.
// One execution slot is shared by every endpoint, including URL aliases.
class AiColumnWorker : public share::ObThreadPool
{
public:
  AiColumnWorker() = default;
  ~AiColumnWorker() { stop(); wait(); }
  void run1() override;
private:
  int process_one();
  DISALLOW_COPY_AND_ASSIGN(AiColumnWorker);
};
} }
