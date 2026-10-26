// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#pragma once
#include "sql/engine/basic/file_scan/file_scan_utils.h"
#include "sql/engine/basic/ob_select_into_op.h"
#include <optional>
namespace oceanbase
{
namespace sql
{
class FileCopySpec : public ObSelectIntoSpec
{
  OB_UNIS_VERSION_V(1);

public:
  FileCopySpec(common::ObIAllocator &alloc, ObPhyOperatorType type)
      : ObSelectIntoSpec(alloc, type), column_names_(alloc)
  {
  }
  common::ObFixedArray<common::ObString, common::ObIAllocator> column_names_;
};
class FileCopyOp : public ObOperator
{
public:
  FileCopyOp(ObExecContext &ctx, const ObOpSpec &spec, ObOpInput *input)
      : ObOperator(ctx, spec, input), done_(false)
  {
  }
  int inner_open() override;
  int inner_get_next_row() override;
  int inner_get_next_batch(int64_t max_rows) override;
  int inner_close() override;
  void destroy() override;

private:
  int write_row(int64_t index);
  int drain(bool batch);
  bool done_;
  common::ObString authorized_root_;
  std::optional<rust::Box<file_sql::CopyWriter>> writer_;
};
} // namespace sql
} // namespace oceanbase
