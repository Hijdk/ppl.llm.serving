#include "simple_flags.h"

#include "llm.grpc.pb.h"
#include "ppl/common/log.h"
#include "rapidjson/document.h"
#include "rapidjson/istreamwrapper.h"
#include "grpc++/grpc++.h"
#include "sentencepiece_processor.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <random>

using namespace grpc;
using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using namespace std::chrono;
using namespace ppl::llm;

static pthread_cond_t finished_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static std::unordered_map<int, std::string> rsp_stream_store;
static int finished_cnt = 0;
static int num_request = 0;

Define_string_opt("--target", g_flag_target, "localhost:23333", "ip:port");
Define_string_opt("--tokenizer", g_flag_tokenizer, "", "Path to the tokenizer");
Define_string_opt("--dataset", g_flag_dataset, "", "Path to the dataset.");
Define_string_opt("--request_rate", g_flag_request_rate, "inf",
                  "Number of request per second. If this is inf, then all the requests are sent at time 0. Otherwise, "
                  "we use Poisson process to synthesize the request arrival times.");

struct TidRecord {
    int prompt_len;
    int output_len;
    bool is_prefill = true;
    std::chrono::_V2::system_clock::time_point prefill_time;
    std::chrono::_V2::system_clock::time_point finished_time;
};
static std::unordered_map<int64_t, TidRecord> tid_record_map;

void SampleRequest(const std::string& dataset_path, const sentencepiece::SentencePieceProcessor& tokenizer,
                   std::vector<std::shared_ptr<proto::BatchedRequest>>* req_list) {
    std::ifstream ifs(dataset_path);
    rapidjson::IStreamWrapper isw(ifs);
    rapidjson::Document root;
    root.ParseStream(isw);
    if (root.HasParseError()) {
        return;
    }
    LOG(INFO) << "request size: " << root.Size();
    uint64_t tid = 0;
    for (size_t i = 0; i < root.Size(); ++i) {
        const auto& convs = root[i]["conversations"];
        const std::string prompt = convs[0]["value"].GetString();
        const std::string ans = convs[1]["value"].GetString();

        std::vector<int> prompt_token_ids;
        std::vector<int> ans_token_ids;
        tokenizer.Encode(prompt, &prompt_token_ids);
        tokenizer.Encode(ans, &ans_token_ids);

        auto batch_req = std::make_shared<proto::BatchedRequest>(); // batch_size = 1
        auto* req = batch_req->add_req();
        req->set_id(tid);
        req->set_prompt(prompt);
        req->set_temperature(1);
        req->set_generation_length(ans_token_ids.size());
        req_list->push_back(batch_req);

        auto& tid_record = tid_record_map.emplace(tid, TidRecord()).first->second;
        tid_record.prompt_len = prompt_token_ids.size();
        tid_record.output_len = ans_token_ids.size();
        tid++;
    }
}

enum CallStatus { CREATE, PROCESS, PROCESSED, FINISH, FAILED };

class GenerationClientAsync {
public:
    GenerationClientAsync(std::shared_ptr<Channel> channel) : stub_(proto::LLMService::NewStub(channel)) {}

    void Generation(const std::vector<std::shared_ptr<proto::BatchedRequest>> req_list) {
        std::random_device rd;
        std::mt19937 gen(rd());

        for (size_t i = 0; i < req_list.size(); i++) {
            const auto& req_batch = *req_list[i];
            AsyncClientCall* call = new AsyncClientCall;

            call->response_reader = stub_->PrepareAsyncGeneration(&call->context, req_batch, &cq_);
            call->response_reader->StartCall((void*)call);

            if (g_flag_request_rate == "inf") { // continuous send, no interval
                continue;
            }
            float request_rate = std::stof(g_flag_request_rate);
            std::exponential_distribution<> dist(request_rate);
            float sleep_time = dist(gen);
            int sleep_s = int(sleep_time);
            int sleep_us = (sleep_time - float(sleep_s)) * 1000000;
            std::this_thread::sleep_for(std::chrono::microseconds(sleep_s * 1000000 + sleep_us));
        }
        pthread_mutex_lock(&lock);
        while (finished_cnt < num_request) {
            pthread_cond_wait(&finished_cond, &lock);
        }
        pthread_mutex_unlock(&lock);
    }

    // Loop while listening for completed responses.
    void AsyncCompleteRpc() {
        void* got_tag;
        bool ok = false;
        // Block until the next result is available in the completion queue "cq".
        LOG(INFO) << "Wait for response";
        while (cq_.Next(&got_tag, &ok)) {
            if (!got_tag) {
                LOG(ERROR) << "Get tag failed";
            }

            // The tag in this example is the memory location of the call object
            AsyncClientCall* call = static_cast<AsyncClientCall*>(got_tag);
            call->HandleResponse(ok);

            if (finished_cnt >= num_request) {
                pthread_cond_signal(&finished_cond);
                break;
            }
        }
    }

private:
    struct AsyncClientCall {
        void HandleResponse(bool responseStatus) {
            switch (callStatus_) {
                case CREATE:
                    if (responseStatus) {
                        response_reader->Read(&reply, (void*)this);
                        callStatus_ = PROCESS;
                    } else {
                        response_reader->Finish(&status, (void*)this);
                        callStatus_ = FINISH;
                    }
                    break;
                case PROCESS:
                    if (responseStatus) {
                        auto& rsp = this->reply;
                        int tid = rsp.id();
                        if (tid_record_map[tid].is_prefill == true) {
                            tid_record_map[tid].prefill_time = std::chrono::high_resolution_clock::now();
                            tid_record_map[tid].is_prefill = false;
                        }
                        const std::string& rsp_stream = rsp.generated();
                        rsp_stream_store[tid] += rsp_stream;
                        response_reader->Read(&reply, (void*)this);
                    } else {
                        response_reader->Finish(&status, (void*)this);
                        callStatus_ = FINISH;
                    }
                    break;
                case FINISH:
                    __sync_fetch_and_add(&finished_cnt, 1);
                    tid_record_map[reply.id()].finished_time = std::chrono::high_resolution_clock::now();
                    LOG(INFO) << "Finish: " << finished_cnt << "/" << num_request;
                    if (status.ok()) {
                        LOG(INFO) << "Server Response Completed: " << reply.id();
                    } else {
                        LOG(ERROR) << "RPC failed";
                    }
                    delete this;
                    break;
                default:
                    LOG(ERROR) << "impossible or invalid status";
                    break;
            }
        };

        CallStatus callStatus_ = CREATE;
        proto::Response reply;
        ClientContext context;
        Status status;
        std::unique_ptr<ClientAsyncReader<proto::Response>> response_reader;
    };

    std::unique_ptr<proto::LLMService::Stub> stub_;

    CompletionQueue cq_;
};

int main(int argc, char* argv[]) {
    simple_flags::parse_args(argc, argv);
    if (!simple_flags::get_unknown_flags().empty()) {
        string content;
        for (auto it : simple_flags::get_unknown_flags()) {
            content += "'" + it + "', ";
        }
        content.resize(content.size() - 2); // remove last ', '
        content.append(".");
        LOG(ERROR) << "unknown option(s): " << content.c_str();
        return -1;
    }

    const std::string target_str = g_flag_target;
    const std::string tokenizer_path = g_flag_tokenizer; // LLaMA/tokenizer.model
    const std::string data_path = g_flag_dataset; // samples_1024.json

    sentencepiece::SentencePieceProcessor tokenizer;
    const auto tokenizer_status = tokenizer.Load(tokenizer_path);
    if (!tokenizer_status.ok()) {
        LOG(ERROR) << tokenizer_status.ToString();
        return -1;
    }
    LOG(INFO) << "VOCAB_SIZE: " << tokenizer.GetPieceSize() << "; BOS ID: " << tokenizer.bos_id()
              << "; EOS ID: " << tokenizer.eos_id() << "; PAD ID: " << tokenizer.pad_id();

    std::vector<std::shared_ptr<proto::BatchedRequest>> req_list;
    SampleRequest(data_path, tokenizer, &req_list);
    num_request = req_list.size();

    GenerationClientAsync generator(grpc::CreateChannel(target_str, grpc::InsecureChannelCredentials()));

    std::thread recv_thread = std::thread(&GenerationClientAsync::AsyncCompleteRpc, &generator);

    auto benchmark_start = std::chrono::high_resolution_clock::now();
    generator.Generation(req_list);
    auto benchmark_end = std::chrono::high_resolution_clock::now();

    auto benchmark_time =
        double(std::chrono::duration_cast<std::chrono::microseconds>(benchmark_end - benchmark_start).count()) /
        1000.0 / 1000.0;

    double total_prefill_latency = 0; // ms
    double total_decode_latency_per_token = 0; // ms
    double total_prompt_latency = 0; // ms
    int total_input_tokens = 0;
    int total_gen_tokens = 0;
    for (auto it = tid_record_map.begin(); it != tid_record_map.end(); ++it) {
        auto& tid_record = it->second;
        double prefill_latency = double(
            std::chrono::duration_cast<std::chrono::microseconds>(tid_record.prefill_time - benchmark_start).count() /
            1000.0); // ms

        double decoding_tatency = double(
            std::chrono::duration_cast<std::chrono::microseconds>(tid_record.finished_time - tid_record.prefill_time)
                .count() /
            1000.0); // ms
        double prompt_latency = double(
            std::chrono::duration_cast<std::chrono::microseconds>(tid_record.finished_time - benchmark_start).count() /
            1000.0); // ms
        // total_latency_per_token += (prompt_latency / tid_record.output_len);
        total_prompt_latency += prompt_latency;

        total_prefill_latency += prefill_latency;
        total_decode_latency_per_token += decoding_tatency / (tid_record.output_len - 1);

        total_input_tokens += tid_record.prompt_len;
        total_gen_tokens += tid_record.output_len;
    }
    double avg_latency_prefill = total_prefill_latency / num_request;
    double avg_latency_decode_per_token = total_decode_latency_per_token / num_request;
    double avg_latency_per_prompt = total_prompt_latency / num_request;

    fprintf(stderr, "[RESULT] benchmark time: %.2f s\n", benchmark_time);

    // 统计: avg inptu len, avg gen len, task num, total gen tokens
    fprintf(stderr, "[RESULT] request count: %d\n", num_request);
    fprintf(stderr, "[RESULT] avg input len: %d, total input len: %d\n", total_input_tokens / num_request,
            total_input_tokens);
    fprintf(stderr, "[RESULT] avg gen len: %d, total gen len: %d\n", total_gen_tokens / num_request, total_gen_tokens);
    fprintf(stderr, "[RESULT] time per token: %.2f ms\n", benchmark_time * 1000 / total_gen_tokens);
    fprintf(stderr, "[RESULT] avg latency prefill: %.2f ms\n", avg_latency_prefill);
    fprintf(stderr, "[RESULT] avg latency decoding: %.2f ms\n", avg_latency_decode_per_token);
    fprintf(stderr, "[RESULT] avg latency per prompt: %.2f ms\n", avg_latency_per_prompt);

    // tps1, tps2
    fprintf(stderr, "[RESULT] tokens out per sec: %.2f\n", total_gen_tokens / benchmark_time);
    fprintf(stderr, "[RESULT] tokens inout per sec: %.2f\n", (total_input_tokens + total_gen_tokens) / benchmark_time);
    // qps
    fprintf(stderr, "[RESULT] requests per sec: %.2f\n", num_request / benchmark_time);

    recv_thread.join();
    return 0;
}
