/*
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>
#include "sql/engine/expr/ob_expr_ai/ob_expr_ai_embed.h"
using namespace oceanbase::common;
using namespace oceanbase::sql;

TEST(AIEmbedBatch, NullSkipAndEvaluatedRowsDoNotCallEndpoint)
{
  ObArenaAllocator allocator;
  ObExecContext exec(allocator);
  ObEvalCtx ctx(exec, &allocator);
  constexpr int64_t count = 4;
  alignas(64) char frames_storage[3][4096] = {};
  char *frames[] = {frames_storage[0], frames_storage[1], frames_storage[2]};
  ctx.frames_ = frames;
  ObExpr model, content, output;
  ObExpr *expressions[] = {&model, &content, &output};
  for (int i = 0; i < 3; ++i) {
    ObExpr &expr = *expressions[i];
    expr.frame_idx_ = i;
    expr.datum_off_ = 0;
    expr.eval_info_off_ = 1024;
    expr.eval_flags_off_ = 1088;
    expr.batch_result_ = true;
    expr.batch_idx_mask_ = -1;
    expr.datum_meta_.type_ = ObVarcharType;
    for (int64_t j = 0; j < count; ++j) {
      expr.locate_expr_datum(ctx,j).set_null();
    }
  }
  ObExpr *args[] = {&model, &content};
  output.args_ = args;
  output.arg_cnt_ = 2;
  for (int64_t j = 0; j < count; ++j) { model.locate_expr_datum(ctx,j).set_string("unused-model"); }
  alignas(8) uint64_t skip_memory = 0;
  ObBitVector &skip = *reinterpret_cast<ObBitVector *>(&skip_memory);
  skip.set(1);
  output.get_evaluated_flags(ctx).set(2);
  output.locate_expr_datum(ctx,1).set_string("skipped");
  output.locate_expr_datum(ctx,2).set_string("already-evaluated");
  ASSERT_EQ(OB_SUCCESS, ObExprAIEmbed::eval_ai_embed_batch(output,ctx,skip,count));
  EXPECT_TRUE(output.locate_expr_datum(ctx,0).is_null());
  EXPECT_TRUE(output.locate_expr_datum(ctx,3).is_null());
  EXPECT_EQ(ObString("skipped"),output.locate_expr_datum(ctx,1).get_string());
  EXPECT_EQ(ObString("already-evaluated"),output.locate_expr_datum(ctx,2).get_string());
  EXPECT_FALSE(output.get_evaluated_flags(ctx).at(1));
  EXPECT_TRUE(output.get_evaluated_flags(ctx).at(0));
  EXPECT_TRUE(output.get_evaluated_flags(ctx).at(3));
}
int main(int argc, char **argv) { ::testing::InitGoogleTest(&argc, argv); return RUN_ALL_TESTS(); }
