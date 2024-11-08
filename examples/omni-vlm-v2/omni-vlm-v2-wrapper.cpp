#include "base64.hpp"
#include "log.h"
#include "common.h"
#include "sampling.h"
#include "clip-v2.h"
#include "omni-vlm-v2.h"
#include "llama.h"
#include "ggml.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <string>
#include <iostream>

#include "omni-vlm-v2-wrapper.h"


struct omnivlm_context {
    struct clip_ctx * ctx_clip = NULL;
    struct llama_context * ctx_llama = NULL;
    struct llama_model * model = NULL;
};

void* internal_chars = nullptr;

static struct gpt_params params;
static struct llama_model* model;
static struct omnivlm_context* ctx_omnivlm;

static struct omni_image_embed * load_image(omnivlm_context * ctx_omnivlm, gpt_params * params, const std::string & fname) {

    // load and preprocess the image
    omni_image_embed * embed = NULL;
    embed = omnivlm_image_embed_make_with_filename(ctx_omnivlm->ctx_clip, params->n_threads, fname.c_str());
    if (!embed) {
        fprintf(stderr, "%s: is %s really an image file?\n", __func__, fname.c_str());
        return NULL;
    }

    return embed;
}

static struct llama_model * omnivlm_init(gpt_params * params) {
    llama_backend_init();
    llama_numa_init(params->numa);

    llama_model_params model_params = llama_model_params_from_gpt_params(*params);

    llama_model * model = llama_load_model_from_file(params->model.c_str(), model_params);
    if (model == NULL) {
        LOG_TEE("%s: unable to load model\n" , __func__);
        return NULL;
    }
    return model;
}

static struct omnivlm_context * omnivlm_init_context(gpt_params * params, llama_model * model) {
    const char * clip_path = params->mmproj.c_str();

    auto prompt = params->prompt;
    if (prompt.empty()) {
        prompt = "describe the image in detail.";
    }

    auto ctx_clip = clip_model_load(clip_path, /*verbosity=*/ 10);


    llama_context_params ctx_params = llama_context_params_from_gpt_params(*params);
    ctx_params.n_ctx           = params->n_ctx < 2048 ? 2048 : params->n_ctx; // we need a longer context size to process image embeddings

    llama_context * ctx_llama = llama_new_context_with_model(model, ctx_params);

    if (ctx_llama == NULL) {
        LOG_TEE("%s: failed to create the llama_context\n" , __func__);
        return NULL;
    }

    ctx_omnivlm = (struct omnivlm_context *)malloc(sizeof(omnivlm_context));

    ctx_omnivlm->ctx_llama = ctx_llama;
    ctx_omnivlm->ctx_clip = ctx_clip;
    ctx_omnivlm->model = model;
    return ctx_omnivlm;
}

static bool eval_tokens(struct llama_context * ctx_llama, std::vector<llama_token> tokens, int n_batch, int * n_past) {
    int N = (int) tokens.size();
    for (int i = 0; i < N; i += n_batch) {
        int n_eval = (int) tokens.size() - i;
        if (n_eval > n_batch) {
            n_eval = n_batch;
        }
        if (llama_decode(ctx_llama, llama_batch_get_one(&tokens[i], n_eval, *n_past, 0))) {
            LOG_TEE("%s : failed to eval. token %d/%d (batch size %d, n_past %d)\n", __func__, i, N, n_batch, *n_past);
            return false;
        }
        *n_past += n_eval;
    }
    return true;
}

static bool eval_id(struct llama_context * ctx_llama, int id, int * n_past) {
    std::vector<llama_token> tokens;
    tokens.push_back(id);
    return eval_tokens(ctx_llama, tokens, 1, n_past);
}

static bool eval_string(struct llama_context * ctx_llama, const char* str, int n_batch, int * n_past, bool add_bos){
    std::string              str2     = str;
    std::vector<llama_token> embd_inp = ::llama_tokenize(ctx_llama, str2, add_bos, true);
    eval_tokens(ctx_llama, embd_inp, n_batch, n_past);
    return true;
}

static const char * sample(struct llama_sampling_context * ctx_sampling,
                           struct llama_context * ctx_llama,
                           int * n_past) {
    const llama_token id = llama_sampling_sample(ctx_sampling, ctx_llama, NULL);
    llama_sampling_accept(ctx_sampling, ctx_llama, id, true);
    static std::string ret;
    if (llama_token_is_eog(llama_get_model(ctx_llama), id)) {
        ret = "</s>";
    } else {
        ret = llama_token_to_piece(ctx_llama, id);
    }
    eval_id(ctx_llama, id, n_past);
    return ret.c_str();
}

static const char* process_prompt(struct omnivlm_context * ctx_omnivlm, struct omni_image_embed * image_embed, gpt_params * params, const std::string & prompt) {
    int n_past = 0;

    const int max_tgt_len = params->n_predict < 0 ? 256 : params->n_predict;

    std::string full_prompt = "<|im_start|>system\nYou are Nano-Omni-VLM, created by Nexa AI. You are a helpful assistant.<|im_end|>\n<|im_start|>user\n" \
                                + prompt + "\n<|vision_start|><|image_pad|><|vision_end|><|im_end|>";
    size_t image_pos = full_prompt.find("<|image_pad|>");
    std::string system_prompt, user_prompt;

    // new templating mode: Provide the full prompt including system message and use <image> as a placeholder for the image
    system_prompt = full_prompt.substr(0, image_pos);
    user_prompt = full_prompt.substr(image_pos + std::string("<|image_pad|>").length());
    if (params->verbose_prompt) {
        auto tmp = ::llama_tokenize(ctx_omnivlm->ctx_llama, system_prompt, true, true);
        for (int i = 0; i < (int) tmp.size(); i++) {
            LOG_TEE("%6d -> '%s'\n", tmp[i], llama_token_to_piece(ctx_omnivlm->ctx_llama, tmp[i]).c_str());
        }
    }
    // LOG_TEE("user_prompt: %s\n", user_prompt.c_str());
    if (params->verbose_prompt) {
        auto tmp = ::llama_tokenize(ctx_omnivlm->ctx_llama, user_prompt, true, true);
        for (int i = 0; i < (int) tmp.size(); i++) {
            LOG_TEE("%6d -> '%s'\n", tmp[i], llama_token_to_piece(ctx_omnivlm->ctx_llama, tmp[i]).c_str());
        }
    }

    eval_string(ctx_omnivlm->ctx_llama, system_prompt.c_str(), params->n_batch, &n_past, true);
    omnivlm_eval_image_embed(ctx_omnivlm->ctx_llama, image_embed, params->n_batch, &n_past);
    eval_string(ctx_omnivlm->ctx_llama, user_prompt.c_str(), params->n_batch, &n_past, false);

    // generate the response

    LOG("\n");

    struct llama_sampling_context * ctx_sampling = llama_sampling_init(params->sparams);
    if (!ctx_sampling) {
        LOG_TEE("%s: failed to initialize sampling subsystem\n", __func__);
        exit(1);
    }

    std::string response = "";
    for (int i = 0; i < max_tgt_len; i++) {
        const char * tmp = sample(ctx_sampling, ctx_omnivlm->ctx_llama, &n_past);
        if (strcmp(tmp, "<|im_end|>") == 0) break;
        if (strcmp(tmp, "</s>") == 0) break;
        // if (strstr(tmp, "###")) break; // Yi-VL behavior
        // printf("%s", tmp);
        response += tmp;
        // if (strstr(response.c_str(), "<|im_end|>")) break; // Yi-34B llava-1.6 - for some reason those decode not as the correct token (tokenizer works)
        // if (strstr(response.c_str(), "<|im_start|>")) break; // Yi-34B llava-1.6
        // if (strstr(response.c_str(), "USER:")) break; // mistral llava-1.6

        fflush(stdout);
    }

    llama_sampling_free(ctx_sampling);
    printf("\n");

    // const char* ret_char_ptr = (const char*)(malloc(sizeof(char)*response.size()));
    if(internal_chars != nullptr) { free(internal_chars); }
    internal_chars = malloc(sizeof(char)*(response.size()+1));
    strncpy((char*)(internal_chars), response.c_str(), response.size());
    ((char*)(internal_chars))[response.size()] = '\0';
    return (const char*)(internal_chars);
}

static void omnivlm_free(struct omnivlm_context * ctx_omnivlm) {
    if (ctx_omnivlm->ctx_clip) {
        clip_free(ctx_omnivlm->ctx_clip);
        ctx_omnivlm->ctx_clip = NULL;
    }

    llama_free(ctx_omnivlm->ctx_llama);
    llama_free_model(ctx_omnivlm->model);
    llama_backend_free();
}

static void print_usage(int argc, char ** argv, const gpt_params & params) {
    gpt_params_print_usage(argc, argv, params);

    LOG_TEE("\n example usage:\n");
    LOG_TEE("\n     %s -m <llava-v1.5-7b/ggml-model-q5_k.gguf> --mmproj <llava-v1.5-7b/mmproj-model-f16.gguf> --image <path/to/an/image.jpg> --image <path/to/another/image.jpg> [--temp 0.1] [-p \"describe the image in detail.\"]\n", argv[0]);
    LOG_TEE("\n note: a lower temperature value like 0.1 is recommended for better quality.\n");
}

// inference interface definition
void omnivlm_init(const char* llm_model_path, const char* projector_model_path) {
    const char* argv = "hello-omni-vlm-wrapper-cli";
    char* nc_argv = const_cast<char*>(argv);
    if (!gpt_params_parse(1, &nc_argv, params)) {
        print_usage(1, &nc_argv, {});
        throw std::runtime_error("init params error.");
    }
    params.model = llm_model_path;
    params.mmproj = projector_model_path;
    model = omnivlm_init(&params);
    if (model == nullptr) {
        fprintf(stderr, "%s: error: failed to init omnivlm model\n", __func__);
        throw std::runtime_error("Failed to init omnivlm model");
    }
    ctx_omnivlm = omnivlm_init_context(&params, model);
}

const char* omnivlm_inference(const char *prompt, const char *imag_path) {
    std::string image = imag_path;
    params.prompt = prompt;
    auto * image_embed = load_image(ctx_omnivlm, &params, image);
    if (!image_embed) {
        LOG_TEE("%s: failed to load image %s. Terminating\n\n", __func__, image.c_str());
        throw std::runtime_error("failed to load image " + image);
    }
    // process the prompt
    const char* ret_chars = process_prompt(ctx_omnivlm, image_embed, &params, params.prompt);

    // llama_perf_print(ctx_omnivlm->ctx_llama, LLAMA_PERF_TYPE_CONTEXT);
    omnivlm_image_embed_free(image_embed);

    return ret_chars;
}

void omnivlm_free() {
    if(internal_chars != nullptr) { free(internal_chars); }
    ctx_omnivlm->model = NULL;
    omnivlm_free(ctx_omnivlm);
    llama_free_model(model);
}
