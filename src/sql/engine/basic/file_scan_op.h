// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#pragma once
#include "sql/engine/basic/file_scan/file_scan_utils.h"
#include "sql/engine/basic/ob_function_table_op.h"
#include <optional>
namespace oceanbase
{
namespace sql
{
class FileScanSpec : public ObFunctionTableSpec
{
  OB_UNIS_VERSION_V(1);

public:
  FileScanSpec(common::ObIAllocator &alloc, ObPhyOperatorType type)
      : ObFunctionTableSpec(alloc, type), column_indexes_(alloc)
  {
  }
  common::ObFixedArray<int64_t, common::ObIAllocator> column_indexes_;
};
class FileScanOp : public ObOperator
{
public:
  FileScanOp(ObExecContext &ctx, const ObOpSpec &spec, ObOpInput *input)
      : ObOperator(ctx, spec, input), position_(0)
  {
  }
  int inner_open() override;
  int inner_get_next_row() override;
  int inner_get_next_batch(int64_t max_rows) override;
  int inner_rescan() override;
  int inner_close() override;
  void destroy() override;

private:
  int fill_row(size_t row, int64_t output_index);
  std::optional<rust::Box<file_sql::FileScan>> reader_;
  file_sql::Batch batch_;
  size_t position_;
};
} // namespace sql
} // namespace oceanbase
