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
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
using namespace oceanbase::common;
using namespace oceanbase::sql;

TEST(AIPrompt, RepeatedAndNestedArguments)
{
  ObArenaAllocator alloc;
  ObJsonObject *prompt = nullptr;
  ObString json("{\"template\":\"{0}/{0}/{0}\",\"args\":[\"long-argument\"]}");
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(alloc, json, prompt));
  ObString result;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(alloc, prompt, result));
  EXPECT_EQ(ObString("long-argument/long-argument/long-argument"), result);
  json = ObString("{\"template\":\"{0}\",\"args\":[{\"template\":\"hello {0}\",\"args\":[\"世界\"]}]}");
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(alloc, json, prompt));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(alloc, prompt, result));
  EXPECT_EQ(ObString("hello 世界"), result);
}
TEST(AIPrompt, InvalidIndex)
{
  ObArenaAllocator alloc;
  for (const char *text : {"{\"template\":\"{2}\",\"args\":[\"a\"]}",
                           "{\"template\":\"{999999999999999999999999}\",\"args\":[\"a\"]}"}) {
    ObString json(text), result;
    ObJsonObject *prompt = nullptr;
    ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(alloc, json, prompt));
    EXPECT_NE(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(alloc, prompt, result));
  }
}

TEST(AIPrompt, JsonAndEscapes)
{
  ObArenaAllocator alloc;
  ObString json("{\"template\":\"{0}|{1}|{2}\",\"args\":[[1,2],{\"a\":1},\"世界\\n\\\"\\\\\"]}");
  ObJsonObject *prompt=nullptr;
  ASSERT_EQ(OB_SUCCESS,ObAIFuncJsonUtils::get_json_object_form_str(alloc,json,prompt));
  ObString result;
  ASSERT_EQ(OB_SUCCESS,ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(alloc,prompt,result));
  EXPECT_EQ(ObString("[1, 2]|{\"a\": 1}|世界\n\"\\"),result);
}

TEST(AIPrompt, LargeUnmatchedBraces)
{
  ObArenaAllocator alloc;
  std::string input(100000, '{');
  ObString text(input.size(),input.data()), result;
  ObJsonArray *args=nullptr;
  ObJsonObject *prompt=nullptr;
  ASSERT_EQ(OB_SUCCESS,ObAIFuncJsonUtils::get_json_array(alloc,args));
  ASSERT_EQ(OB_SUCCESS,ObAIFuncPromptObjectUtils::construct_prompt_object(alloc,text,args,prompt));
  ASSERT_EQ(OB_SUCCESS,ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(alloc,prompt,result));
  EXPECT_EQ(text,result);
}
int main(int argc, char **argv) { ::testing::InitGoogleTest(&argc, argv); return RUN_ALL_TESTS(); }
