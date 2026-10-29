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
  int refresh_work_presence();
  // Worker-thread owned. Once AI work is seen, retain ordinary polling so DROP
  // and definition changes cannot strand old durable tasks.
  int64_t checked_schema_version_ = -1;
  bool checked_durable_queue_ = false;
  bool has_ai_work_ = false;
  DISALLOW_COPY_AND_ASSIGN(AiColumnWorker);
};
} }
