// agent.h — Agent loop data structures and function declarations for Llamaste LLM-OS
//
// The agent loop is the core interaction model: the LLM receives a conversation
// with system prompt + user messages, optionally calls tools, and returns text.
// This module manages conversation state, tool call parsing, and the multi-round
// inference loop that dispatches tools until the LLM produces a final text response.

#pragma once
#include <string>
#include <vector>
#include <functional>
#include "json.hpp"
#include "tools.h"

// A single parsed tool call from the LLM response.
// Maps to the OpenAI chat completions tool_calls array entries.
struct ToolCall {
    std::string id;         // unique tool call ID (e.g., "call_abc123")
    std::string name;       // tool function name (e.g., "fs.read_file")
    std::string arguments;  // raw JSON string of arguments
};

// Conversation message with role and content.
// Follows the OpenAI chat completions message format:
// - system: system prompt
// - user: user input
// - assistant: LLM response (may include tool_calls)
// - tool: result from a tool invocation
struct Message {
    std::string role;      // "system", "user", "assistant", "tool"
    std::string content;
    std::string tool_call_id;  // for tool result messages
    std::string name;          // tool name for tool results
    std::vector<ToolCall> tool_calls;  // for assistant messages with tool calls
};

// Manages conversation state: message history, system prompt, truncation.
// Serializable to/from JSON for persistence in /data/llamaste/conversations/.
class ConversationState {
public:
    std::string system_prompt;
    std::string conversation_id;

    // Add a user message to the conversation.
    void add_user_message(const std::string& content);

    // Add an assistant text message (no tool calls).
    void add_assistant_message(const std::string& content);

    // Add an assistant message that contains tool calls (no text content).
    void add_assistant_tool_calls(const std::vector<ToolCall>& calls);

    // Add a tool result message responding to a specific tool call.
    void add_tool_result(const std::string& call_id, const std::string& name, const std::string& result);

    // Build the messages array as nlohmann::json for the chat completions API.
    // System prompt is prepended as the first message.
    nlohmann::json to_messages_json() const;

    // Serialize the full conversation state to a JSON string for persistence.
    std::string serialize() const;

    // Deserialize a conversation state from a JSON string.
    static ConversationState deserialize(const std::string& json_str);

    // Truncate old messages if history gets too long.
    // Keeps the system prompt and the most recent max_messages messages.
    // Removes oldest user/assistant/tool messages in order.
    void truncate(int max_messages);

    // Number of messages (excluding the system prompt).
    int message_count() const;

    // Read-only access to the message history.
    const std::vector<Message>& messages() const;

private:
    std::vector<Message> messages_;
};

// Parse tool_calls from an OpenAI-compatible chat completion response JSON string.
// Returns empty vector if the response has no tool calls (just text content).
std::vector<ToolCall> parse_tool_calls(const std::string& response_json);

// Extract the assistant's text content from a chat completion response JSON string.
// Returns empty string if the response is a tool_calls response with no text.
std::string parse_content(const std::string& response_json);

// Build the request body JSON string for /v1/chat/completions.
// Includes model, messages, tools, max_tokens, and temperature.
std::string build_inference_request(
    const ConversationState& conv,
    const ToolRegistry& tools,
    int max_tokens = 2048,
    float temperature = 0.7
);

// Run one turn of the agent loop:
// 1. Build inference request with current conversation state
// 2. Call inference_fn to get LLM response
// 3. If response has tool_calls, dispatch each via tools.dispatch()
// 4. Add tool results to conversation and re-infer
// 5. Repeat until no more tool calls or max_rounds reached
//
// The inference_fn callback sends a request JSON string to the inference
// endpoint and returns the raw response JSON string.
//
// Returns the final assistant text response.
std::string agent_turn(
    ConversationState& conv,
    const ToolRegistry& tools,
    const std::function<std::string(const std::string&)>& inference_fn,
    int max_rounds = 5
);
