// agent.cpp — Agent loop implementation for Llamaste LLM-OS
//
// Implements the multi-round agent loop: the LLM generates responses that
// may include tool_calls, which are dispatched via the ToolRegistry. Tool
// results are appended to the conversation and the LLM is re-invoked until
// it produces a final text response (or max_rounds is reached).

#include "agent.h"
#include <algorithm>
#include <atomic>
#include <stdexcept>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// parse_qwen_tool_calls — extract <tool_call> XML blocks from Qwen2.5 output
// ---------------------------------------------------------------------------

nlohmann::json parse_qwen_tool_calls(const std::string& text) {
    json calls = json::array();
    static std::atomic<int> call_counter{0};

    const std::string START = "<tool_call>";
    const std::string END   = "</tool_call>";

    size_t pos = 0;
    while (true) {
        size_t start_pos = text.find(START, pos);
        if (start_pos == std::string::npos) break;

        size_t json_start = start_pos + START.size();
        size_t end_pos    = text.find(END, json_start);
        if (end_pos == std::string::npos) break;

        // Trim whitespace from the JSON block
        std::string raw = text.substr(json_start, end_pos - json_start);
        size_t first = raw.find_first_not_of(" \t\n\r");
        size_t last  = raw.find_last_not_of(" \t\n\r");
        if (first == std::string::npos) {
            pos = end_pos + END.size();
            continue;
        }
        raw = raw.substr(first, last - first + 1);

        // Parse the JSON object inside the tags
        auto obj = json::parse(raw, nullptr, false);
        if (obj.is_discarded() || !obj.is_object()) {
            fprintf(stderr, "[tool_call] Failed to parse tool call JSON: %.200s\n", raw.c_str());
            pos = end_pos + END.size();
            continue;
        }

        std::string name = obj.value("name", "");
        if (name.empty()) {
            pos = end_pos + END.size();
            continue;
        }

        // Arguments can be a JSON object (ideal) or a pre-serialized string
        json arguments = json::object();
        if (obj.contains("arguments")) {
            if (obj["arguments"].is_object()) {
                arguments = obj["arguments"];
            } else if (obj["arguments"].is_string()) {
                auto parsed = json::parse(obj["arguments"].get<std::string>(), nullptr, false);
                if (!parsed.is_discarded()) arguments = parsed;
            }
        }

        // Build OpenAI-format tool_call entry
        int id = ++call_counter;
        json tc;
        tc["id"]   = "call_" + std::to_string(id);
        tc["type"] = "function";
        json func;
        func["name"]      = name;
        func["arguments"] = arguments.dump();  // must be serialized string per OpenAI spec
        tc["function"] = func;
        calls.push_back(tc);

        fprintf(stderr, "[tool_call] Parsed: name=%s args=%s\n",
                name.c_str(), arguments.dump().c_str());

        pos = end_pos + END.size();
    }

    return calls;
}

// ---------------------------------------------------------------------------
// ConversationState
// ---------------------------------------------------------------------------

void ConversationState::add_user_message(const std::string& content) {
    Message msg;
    msg.role = "user";
    msg.content = content;
    messages_.push_back(std::move(msg));
}

void ConversationState::add_assistant_message(const std::string& content) {
    Message msg;
    msg.role = "assistant";
    msg.content = content;
    messages_.push_back(std::move(msg));
}

void ConversationState::add_assistant_tool_calls(const std::vector<ToolCall>& calls) {
    Message msg;
    msg.role = "assistant";
    msg.tool_calls = calls;
    messages_.push_back(std::move(msg));
}

void ConversationState::add_tool_result(const std::string& call_id, const std::string& name, const std::string& result) {
    Message msg;
    msg.role = "tool";
    msg.tool_call_id = call_id;
    msg.name = name;
    msg.content = result;
    messages_.push_back(std::move(msg));
}

nlohmann::json ConversationState::to_messages_json() const {
    json messages_array = json::array();

    // System prompt is always first
    if (!system_prompt.empty()) {
        json sys_msg;
        sys_msg["role"] = "system";
        sys_msg["content"] = system_prompt;
        messages_array.push_back(sys_msg);
    }

    // Then all conversation messages
    for (const auto& msg : messages_) {
        json j;
        j["role"] = msg.role;

        if (msg.role == "assistant" && !msg.tool_calls.empty()) {
            // Assistant message with tool calls
            // Content may be null/empty when tool calls are present
            j["content"] = nullptr;
            json tc_array = json::array();
            for (const auto& tc : msg.tool_calls) {
                json tc_obj;
                tc_obj["id"] = tc.id;
                tc_obj["type"] = "function";
                json func;
                func["name"] = tc.name;
                func["arguments"] = tc.arguments;
                tc_obj["function"] = func;
                tc_array.push_back(tc_obj);
            }
            j["tool_calls"] = tc_array;
        } else if (msg.role == "tool") {
            // Tool result message
            j["content"] = msg.content;
            j["tool_call_id"] = msg.tool_call_id;
            j["name"] = msg.name;
        } else {
            // User or assistant text message
            j["content"] = msg.content;
        }

        messages_array.push_back(j);
    }

    return messages_array;
}

std::string ConversationState::serialize() const {
    json j;
    j["system_prompt"] = system_prompt;
    j["conversation_id"] = conversation_id;

    json msgs = json::array();
    for (const auto& msg : messages_) {
        json m;
        m["role"] = msg.role;
        m["content"] = msg.content;
        if (!msg.tool_call_id.empty()) m["tool_call_id"] = msg.tool_call_id;
        if (!msg.name.empty()) m["name"] = msg.name;

        if (!msg.tool_calls.empty()) {
            json tcs = json::array();
            for (const auto& tc : msg.tool_calls) {
                json t;
                t["id"] = tc.id;
                t["name"] = tc.name;
                t["arguments"] = tc.arguments;
                tcs.push_back(t);
            }
            m["tool_calls"] = tcs;
        }

        msgs.push_back(m);
    }
    j["messages"] = msgs;

    return j.dump(2);
}

ConversationState ConversationState::deserialize(const std::string& json_str) {
    ConversationState state;

    auto j = json::parse(json_str, nullptr, false);
    if (j.is_discarded()) return state;

    state.system_prompt = j.value("system_prompt", "");
    state.conversation_id = j.value("conversation_id", "");

    if (j.contains("messages") && j["messages"].is_array()) {
        for (const auto& m : j["messages"]) {
            Message msg;
            msg.role = m.value("role", "");
            msg.content = m.value("content", "");
            msg.tool_call_id = m.value("tool_call_id", "");
            msg.name = m.value("name", "");

            if (m.contains("tool_calls") && m["tool_calls"].is_array()) {
                for (const auto& t : m["tool_calls"]) {
                    ToolCall tc;
                    tc.id = t.value("id", "");
                    tc.name = t.value("name", "");
                    tc.arguments = t.value("arguments", "");
                    msg.tool_calls.push_back(std::move(tc));
                }
            }

            state.messages_.push_back(std::move(msg));
        }
    }

    return state;
}

void ConversationState::truncate(int max_messages) {
    if (max_messages < 0) max_messages = 0;
    if (static_cast<int>(messages_.size()) <= max_messages) return;

    // Remove from the front (oldest messages) to keep the most recent
    int to_remove = static_cast<int>(messages_.size()) - max_messages;
    messages_.erase(messages_.begin(), messages_.begin() + to_remove);

    // Remove orphaned leading tool results or assistant tool_calls without results
    while (!messages_.empty()) {
        if (messages_.front().role == "tool") {
            messages_.erase(messages_.begin());
        } else if (messages_.front().role == "assistant" && !messages_.front().tool_calls.empty()) {
            messages_.erase(messages_.begin());
            while (!messages_.empty() && messages_.front().role == "tool") {
                messages_.erase(messages_.begin());
            }
        } else {
            break;
        }
    }
}

int ConversationState::message_count() const {
    return static_cast<int>(messages_.size());
}

const std::vector<Message>& ConversationState::messages() const {
    return messages_;
}

// ---------------------------------------------------------------------------
// parse_tool_calls — extract tool calls from chat completion response
// ---------------------------------------------------------------------------

std::vector<ToolCall> parse_tool_calls(const std::string& response_json) {
    std::vector<ToolCall> calls;

    auto j = json::parse(response_json, nullptr, false);
    if (j.is_discarded()) return calls;

    // Navigate: choices[0].message.tool_calls
    if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty()) {
        return calls;
    }

    const auto& choice = j["choices"][0];
    if (!choice.contains("message")) return calls;

    const auto& message = choice["message"];
    if (!message.contains("tool_calls") || !message["tool_calls"].is_array()) {
        return calls;
    }

    for (const auto& tc : message["tool_calls"]) {
        ToolCall call;
        call.id = tc.value("id", "");

        if (tc.contains("function") && tc["function"].is_object()) {
            call.name = tc["function"].value("name", "");
            // arguments can be a string, object, null, or missing
            if (!tc["function"].contains("arguments") || tc["function"]["arguments"].is_null()) {
                call.arguments = "{}";
            } else if (tc["function"]["arguments"].is_string()) {
                call.arguments = tc["function"]["arguments"].get<std::string>();
            } else {
                call.arguments = tc["function"]["arguments"].dump();
            }
        }

        calls.push_back(std::move(call));
    }

    return calls;
}

// ---------------------------------------------------------------------------
// parse_content — extract text content from chat completion response
// ---------------------------------------------------------------------------

std::string parse_content(const std::string& response_json) {
    auto j = json::parse(response_json, nullptr, false);
    if (j.is_discarded()) return "";

    if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty()) {
        return "";
    }

    const auto& choice = j["choices"][0];
    if (!choice.contains("message")) return "";

    const auto& message = choice["message"];
    if (!message.contains("content") || message["content"].is_null()) {
        return "";
    }

    return message["content"].get<std::string>();
}

// ---------------------------------------------------------------------------
// build_inference_request — construct the /v1/chat/completions request body
// ---------------------------------------------------------------------------

std::string build_inference_request(
    const ConversationState& conv,
    const ToolRegistry& tools,
    int max_tokens,
    float temperature
) {
    json request;

    request["model"] = "llamaste";
    request["messages"] = conv.to_messages_json();
    request["max_tokens"] = max_tokens;
    request["temperature"] = temperature;
    request["stream"] = false;  // ensure non-streaming mode (streaming breaks keep-alive Post)

    // Explicit stop tokens for Qwen2.5 chatml format.
    // llama-server may run in "Content-only" mode when compiled without Jinja2 support
    // (--jinja flag silently ignored).  Without explicit stops, generation never terminates.
    // Qwen2.5-Instruct is trained to emit <|im_end|> after each turn — stop on that.
    request["stop"] = json::array({"<|im_end|>", "<|endoftext|>", "<|im_start|>"});

    // Parse tools JSON string into a JSON array and attach
    std::string tools_str = tools.to_openai_tools_json();
    auto tools_array = json::parse(tools_str, nullptr, false);
    if (!tools_array.is_discarded() && tools_array.is_array() && !tools_array.empty()) {
        request["tools"] = tools_array;
        // Grammar-constrained JSON: implemented as retry in llama_inference().
        // First pass runs without grammar (model chooses text vs tool calls).
        // If <tool_call> tags detected but JSON is malformed, llama_inference()
        // retries with a GBNF grammar that constrains output to valid tool call
        // JSON with known tool names.  See build_tool_call_gbnf() in child_main.cpp.
    }

    return request.dump();
}

// ---------------------------------------------------------------------------
// agent_turn — multi-round agent loop with tool dispatch
// ---------------------------------------------------------------------------

std::string agent_turn(
    ConversationState& conv,
    const ToolRegistry& tools,
    const std::function<std::string(const std::string&)>& inference_fn,
    int max_rounds
) {
    for (int round = 0; round < max_rounds; round++) {
        // Build and send inference request
        std::string request = build_inference_request(conv, tools);
        std::string response;
        try {
            response = inference_fn(request);
        } catch (const std::exception& e) {
            std::string err = std::string("[Inference error: ") + e.what() + "]";
            conv.add_assistant_message(err);
            return err;
        } catch (...) {
            std::string err = "[Inference error: unknown]";
            conv.add_assistant_message(err);
            return err;
        }

        // Try to parse tool calls
        auto tool_calls = parse_tool_calls(response);

        if (tool_calls.empty()) {
            // No tool calls — this is a final text response
            std::string content = parse_content(response);
            if (content.empty()) {
                content = "[No response from inference engine]";
            }
            conv.add_assistant_message(content);
            return content;
        }

        // Tool calls present — add assistant message with tool calls
        conv.add_assistant_tool_calls(tool_calls);

        // Dispatch each tool call and add results
        for (const auto& tc : tool_calls) {
            std::string result = tools.dispatch(tc.name, tc.arguments);

            // Truncate very long tool results to avoid blowing up context
            if (result.size() > 4000) {
                result = result.substr(0, 4000) + "...(truncated)";
            }

            conv.add_tool_result(tc.id, tc.name, result);
        }

        // Loop back to re-infer with tool results in context
    }

    // Max rounds reached without a final text response
    return "[Agent reached maximum tool call rounds without producing a final response]";
}
