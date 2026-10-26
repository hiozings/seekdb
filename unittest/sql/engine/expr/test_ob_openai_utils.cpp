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
TEST(OpenAI, RestoreEmbeddingOrder)
{
  ObArenaAllocator alloc;
  ObString json("{\"data\":[{\"index\":1,\"embedding\":[2]},{\"index\":0,\"embedding\":[1]}]}");
  ObJsonObject *response = nullptr;
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(alloc, json, response));
  ObOpenAIUtils::ObOpenAIEmbed provider;
  ASSERT_EQ(OB_SUCCESS, provider.parse_output(alloc, response, result));
  ObString text;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::print_json_to_str(alloc, result, text));
  EXPECT_EQ(ObString("[[1], [2]]"), text);
}
TEST(OpenAI, EmptyCompletionRejected)
{
  ObArenaAllocator alloc;
  ObString json("{\"choices\":[{\"message\":{\"content\":\"\"}}]}");
  ObJsonObject *response = nullptr;
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(alloc, json, response));
  ObOpenAIUtils::ObOpenAIComplete provider;
  EXPECT_NE(OB_SUCCESS, provider.parse_output(alloc, response, result));
}
TEST(Ollama, FactoryAndNonStreaming)
{
  ObArenaAllocator alloc;
  ObAIFuncIComplete *provider = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_complete_provider(alloc, ObString("oLlAmA"), provider));
  ObString model("arbitrary"), system, prompt("hello");
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider->get_body(alloc, model, system, prompt, nullptr, body));
  ASSERT_NE(nullptr, body->get_value("stream"));
  EXPECT_FALSE(body->get_value("stream")->get_boolean());
}

TEST(OpenAI, RejectMalformedEmbeddingIndices)
{
  for (const char *text : {
      "{\"data\":{}}", "{\"data\":[]}", "{\"data\":[null]}",
      "{\"data\":[{\"embedding\":[1]}]}",
      "{\"data\":[{\"index\":-1,\"embedding\":[1]}]}",
      "{\"data\":[{\"index\":1,\"embedding\":[1]}]}",
      "{\"data\":[{\"index\":0.5,\"embedding\":[1]}]}",
      "{\"data\":[{\"index\":0,\"embedding\":{}}]}",
      "{\"data\":[{\"index\":0,\"embedding\":[1]},{\"index\":0,\"embedding\":[2]}]}"}) {
    SCOPED_TRACE(text);
    ObArenaAllocator alloc;
    ObString json(text);
    ObJsonObject *response = nullptr;
    ObIJsonBase *result = nullptr;
    ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(alloc, json, response));
    ObOpenAIUtils::ObOpenAIEmbed provider;
    EXPECT_NE(OB_SUCCESS, provider.parse_output(alloc, response, result));
    EXPECT_EQ(nullptr,result);
  }
}
TEST(OpenAI, RejectMalformedCompletion)
{
  for (const char *text : {"{}", "{\"choices\":[]}", "{\"choices\":[{\"message\":{\"content\":null}}]}",
                           "{\"choices\":[{\"message\":{\"content\":42}}]}"}) {
    ObArenaAllocator alloc;
    ObString json(text);
    ObJsonObject *response = nullptr;
    ObIJsonBase *result = nullptr;
    ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(alloc, json, response));
    ObOpenAIUtils::ObOpenAIComplete provider;
    EXPECT_NE(OB_SUCCESS, provider.parse_output(alloc, response, result));
  }
}
TEST(Ollama, NativeOptionsAndEmbeddingBody)
{
  ObArenaAllocator alloc;
  ObString json("{\"temperature\":0.25,\"max_tokens\":12}");
  ObJsonObject *config = nullptr, *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(alloc,json,config));
  ObOllamaUtils::ObOllamaComplete complete;
  ObString model("arbitrary"), system("system"), prompt("用户输入");
  ASSERT_EQ(OB_SUCCESS,complete.get_body(alloc,model,system,prompt,config,body));
  ASSERT_NE(nullptr,body->get_value("options"));
  auto *options=static_cast<ObJsonObject *>(body->get_value("options"));
  EXPECT_EQ(12,options->get_value("num_predict")->get_int());
  EXPECT_EQ(nullptr,config->get_value("num_predict"));
  ObOllamaUtils::ObOllamaEmbed embed;
  ObArray<ObString> inputs;
  ASSERT_EQ(OB_SUCCESS,inputs.push_back(prompt));
  ASSERT_EQ(OB_SUCCESS,inputs.push_back(system));
  ASSERT_EQ(OB_SUCCESS,embed.get_body(alloc,model,inputs,nullptr,body));
  EXPECT_EQ(2,body->get_value("input")->element_count());
}

TEST(Dashscope, RejectEmptyAndNonStringCompletion)
{
  for (const char *text : {"{\"output\":{\"choices\":[{\"message\":{\"content\":\"\"}}]}}",
                           "{\"output\":{\"choices\":[{\"message\":{\"content\":42}}]}}"}) {
    ObArenaAllocator alloc;
    ObString json(text);
    ObJsonObject *response = nullptr;
    ObIJsonBase *result = nullptr;
    ASSERT_EQ(OB_SUCCESS,ObAIFuncJsonUtils::get_json_object_form_str(alloc,json,response));
    ObDashscopeUtils::ObDashscopeComplete provider;
    EXPECT_NE(OB_SUCCESS,provider.parse_output(alloc,response,result));
  }
}
int main(int argc, char **argv) { ::testing::InitGoogleTest(&argc, argv); return RUN_ALL_TESTS(); }
