/**
 * EMBODIOS Minimal Model Loader Shim
 *
 * Provides the interface expected by llama-vocab.cpp
 * using our existing gguf_parser instead of file-based loading.
 */

#pragma once

#include "llama-impl.h"
#include "llama-arch.h"
#include "../include/gguf.h"

#include <string>
#include <vector>
#include <cstring>

// Forward declare our C parser
extern "C" {
    // From kernel/ai/gguf_parser.c
    struct gguf_context;
    struct gguf_context* gguf_parser_get_context(void);
    uint32_t gguf_parser_get_vocab_size(void);
    const char* gguf_parser_get_token(uint32_t token_id);
    float gguf_parser_get_token_score(uint32_t token_id);
    int gguf_parser_get_token_type(uint32_t token_id);
    uint32_t gguf_parser_get_bos_token_id(void);
    uint32_t gguf_parser_get_eos_token_id(void);
    const char* gguf_parser_get_string_value(const char* key);
    int64_t gguf_parser_get_int_value(const char* key);
    bool gguf_parser_key_exists(const char* key);
}

// Smart pointer wrapper for gguf_context
struct gguf_context_ptr {
    struct gguf_context* ctx;

    gguf_context_ptr() : ctx(nullptr) {}
    explicit gguf_context_ptr(struct gguf_context* c) : ctx(c) {}

    struct gguf_context* get() const { return ctx; }
    struct gguf_context* operator->() const { return ctx; }
    operator bool() const { return ctx != nullptr; }
};

/**
 * Minimal model loader that wraps gguf_parser for vocab loading
 */
struct llama_model_loader {
    gguf_context_ptr meta;
    LLM_KV llm_kv;

    llama_model_loader() : llm_kv(LLM_KV(LLM_ARCH_UNKNOWN)) {
        meta = gguf_context_ptr(gguf_parser_get_context());
    }

    // Get string value from GGUF
    bool get_key(const std::string& key, std::string& result, bool required = true) {
        const char* val = gguf_parser_get_string_value(key.c_str());
        if (val) {
            result = val;
            return true;
        }
        if (required) {
            LLAMA_LOG_WARN("required key not found: %s\n", key.c_str());
        }
        return false;
    }

    bool get_key(enum llm_kv kid, std::string& result, bool required = true) {
        return get_key(llm_kv.str(kid), result, required);
    }

    // Get integer value from GGUF
    bool get_key(const std::string& key, uint32_t& result, bool required = true) {
        if (gguf_parser_key_exists(key.c_str())) {
            result = (uint32_t)gguf_parser_get_int_value(key.c_str());
            return true;
        }
        if (required) {
            LLAMA_LOG_WARN("required key not found: %s\n", key.c_str());
        }
        return false;
    }

    bool get_key(enum llm_kv kid, uint32_t& result, bool required = true) {
        return get_key(llm_kv.str(kid), result, required);
    }

    // Get int32 value
    bool get_key(const std::string& key, int32_t& result, bool required = true) {
        uint32_t val;
        if (get_key(key, val, required)) {
            result = (int32_t)val;
            return true;
        }
        return false;
    }

    bool get_key(enum llm_kv kid, int32_t& result, bool required = true) {
        return get_key(llm_kv.str(kid), result, required);
    }

    // Get bool value
    bool get_key(const std::string& key, bool& result, bool required = true) {
        if (gguf_parser_key_exists(key.c_str())) {
            result = gguf_parser_get_int_value(key.c_str()) != 0;
            return true;
        }
        if (required) {
            LLAMA_LOG_WARN("required key not found: %s\n", key.c_str());
        }
        return false;
    }

    bool get_key(enum llm_kv kid, bool& result, bool required = true) {
        return get_key(llm_kv.str(kid), result, required);
    }

    // Get float value
    bool get_key(const std::string& key, float& result, bool required = true) {
        // TODO: Implement float reading in gguf_parser
        (void)key;
        (void)result;
        (void)required;
        return false;
    }

    // Array helpers (for vocab tokens)
    template<typename T>
    bool get_arr_n(const std::string& key, T& result, bool required = true) {
        if (key.find("tokens") != std::string::npos) {
            result = (T)gguf_parser_get_vocab_size();
            return true;
        }
        (void)required;
        return false;
    }

    template<typename T>
    bool get_arr_n(enum llm_kv kid, T& result, bool required = true) {
        return get_arr_n(llm_kv.str(kid), result, required);
    }
};
