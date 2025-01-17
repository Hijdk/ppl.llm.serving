// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "common/request.h"
#include "common/response.h"
#include "common/processor.h"
#include "common/connection.h"
#include "backends/cuda/resource_manager.h"
#include "models/llama/llama_tokenizer.h"
#include "models/llama/llama_worker.h"
#include "models/resource.h"
#include "utils/utils.h"
#include "utils/tokenizer.h"
#include "utils/config_utils.h"

#include "ppl/common/log.h"

#include <unistd.h>
#include <unordered_map>
#include <pthread.h>

using namespace std;
using namespace ppl::llm;
using namespace ppl::common;
using namespace ppl::nn;

vector<string> prompts = {
    "Hello, my name is",
    "The president of the United States is",
    "The capital of France is",
    "The future of AI is",
};

class LocalConnection final : public Connection {
public:
    LocalConnection() {
        pthread_mutex_init(&finish_lock_, nullptr);
        pthread_cond_init(&finish_signal_, nullptr);
    }

    ~LocalConnection() {}

    void Send(const Response& rsp) override {
        auto& rsp_str = tid_rsp_map_->emplace(rsp.id, std::string()).first->second;
        rsp_str += rsp.generated;
        if (rsp.flag == Response::IS_LAST) {
            ++count_;
        }
        if (count_ >= wanted_) {
            pthread_cond_signal(&finish_signal_);
        }
    }

    void NotifyFailure(uint64_t id) override {}

    void Wait() {
        pthread_mutex_lock(&finish_lock_);
        while (count_ < wanted_) {
            pthread_cond_wait(&finish_signal_, &finish_lock_);
        }
        pthread_mutex_unlock(&finish_lock_);
    }

    void SetTidRspMap(std::unordered_map<uint64_t, std::string>* tid_rsp_map) {
        tid_rsp_map_ = tid_rsp_map;
    }

    void SetWanted(uint32_t wanted) {
        wanted_ = wanted;
    }

private:
    std::unordered_map<uint64_t, std::string>* tid_rsp_map_;
    uint32_t wanted_;
    uint32_t count_ = 0;

    pthread_mutex_t finish_lock_;
    pthread_cond_t finish_signal_;
};

class LLM {
public:
    LLM(const std::shared_ptr<RequestProcessor>& llm_worker) : llm_worker_(llm_worker) {}

    void Generate(const std::vector<std::shared_ptr<Request>>& req_list,
                  std::unordered_map<uint64_t, std::string>* tid_rsp_map) {
        conn_.SetWanted(req_list.size());
        conn_.SetTidRspMap(tid_rsp_map);

        for (auto req : req_list) {
            llm_worker_->Process(req, &conn_);
        }
        conn_.Wait();
        return;
    }

private:
    std::shared_ptr<RequestProcessor> llm_worker_;
    LocalConnection conn_;
};

int main(int argc, char const* argv[]) {
    if (argc != 2) {
        cerr << "usage: " << argv[0] << " server_config.json" << endl;
        return -1;
    }

    ServerConfig server_config;
    if (!ppl::llm::utils::ParseServerConfig(argv[1], &server_config)) {
        LOG(ERROR) << "ParseServerConfig failed, server config file: " << argv[1];
        return -1;
    }

    WorkerConfig worker_config;
    worker_config.top_p = server_config.top_p;
    worker_config.top_k = server_config.top_k;
    worker_config.max_running_batch = server_config.max_running_batch;
    worker_config.max_tokens_per_request = server_config.max_tokens_per_request;
    worker_config.max_tokens_per_step = server_config.max_tokens_per_step;

    ModelConfig model_config;
    if (!ppl::llm::utils::ParseModelConfig(server_config.model_param_path, &model_config)) {
        LOG(ERROR) << "PaseModelConfig failed, model_param_path: " << server_config.model_param_path;
        return -1;
    }
    LOG(INFO) << "Parse model model_config successed";

    // init nccl, cuda engine, kv cache, kv scale manager
    cuda::CudaResourceManager resource_manager;
    auto rc = resource_manager.Init(model_config, server_config);
    if (rc != RC_SUCCESS) {
        LOG(ERROR) << "init CudaResourceManager failed: " << GetRetCodeStr(rc);
        return -1;
    }
    std::shared_ptr<ppl::llm::utils::Tokenizer> tokenizer;

    if (server_config.model_type == "llama") {
        auto llama_tokenizer = std::make_shared<llama::LlamaTokenizer>();
        llama_tokenizer->Init(server_config.tokenizer_path);
        tokenizer = llama_tokenizer;
    } else {
        LOG(ERROR) << "not supported model: " << server_config.model_type;
        return -1;
    }

    Resource resource;
    resource.tensor_parallel_size = server_config.tensor_parallel_size;
    resource.kv_cache_max_tokens = resource_manager.kv_cache_max_tokens;
    resource.items = resource_manager.items.data();
    resource.sampler = resource_manager.sampler.get();
    resource.device_worker_pool = &resource_manager.device_worker_pool;
    resource.tokenizer = tokenizer.get();

    vector<std::shared_ptr<Request>> request_list;
    for (size_t i = 0; i < prompts.size(); ++i) {
        request_list.push_back(std::make_shared<Request>(i, prompts[i], 1.0, 64));
    }

    std::shared_ptr<RequestProcessor> llm_worker;
    if (server_config.model_type == "llama") {
        auto llama_worker = std::make_shared<llama::LLaMAWorker>(resource, model_config, worker_config);
        auto rc = llama_worker->Init();
        if (rc != RC_SUCCESS) {
            LOG(ERROR) << "llama_worker init failed: " << GetRetCodeStr(rc);
            return -1;
        }
        LOG(INFO) << "Init llama worker successed";
        llm_worker = llama_worker;
    } else {
        LOG(ERROR) << "not supported model: " << server_config.model_type;
        return -1;
    }

    LLM llm(llm_worker);
    unordered_map<uint64_t, string> tid_rsp_map;

    LOG(INFO) << "before generate";

    double generate_time;
    {
        ppl::llm::utils::TimingGuard __timing__(&generate_time);
        llm.Generate(request_list, &tid_rsp_map);
    }

    sleep(1);
    std::cout << "===================================" << std::endl;
    for (size_t i = 0; i < tid_rsp_map.size(); i++) {
        const std::string& prompt = request_list[i]->prompt;
        const std::string& answer = tid_rsp_map[request_list[i]->id];
        std::cout << "Prompt: " << prompt << std::endl;
        std::cout << "Answer: " << answer << std::endl;
        std::cout << "===================================" << std::endl;
    }

    std::cout << "generation time: " << generate_time << std::endl;
    exit(0);
    return 0;
}