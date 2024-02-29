/* Copyright (C) 2023 Michal Kosciesza <michal@mkiol.net>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "whisper_engine.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <sstream>

#include "cpu_tools.hpp"
#include "logger.hpp"
#include "text_tools.hpp"

whisper_engine::whisper_engine(config_t config, callbacks_t call_backs)
    : stt_engine{std::move(config), std::move(call_backs)} {
    open_whisper_lib();
    m_wparams = make_wparams();
    m_speech_buf.reserve(m_speech_max_size);
}

whisper_engine::~whisper_engine() {
    LOGD("whisper dtor");

    stop();

    if (m_whisper_api.ok()) {
        if (m_whisper_ctx) {
            m_whisper_api.whisper_free(m_whisper_ctx);
            m_whisper_ctx = nullptr;
        }
    }

    m_whisper_api = {};

    if (m_whisperlib_handle) {
        dlclose(m_whisperlib_handle);
        m_whisperlib_handle = nullptr;
    }

    unsetenv("GGML_OPENCL_PLATFORM");
    unsetenv("GGML_OPENCL_DEVICE");
    unsetenv("HIP_VISIBLE_DEVICES");
    unsetenv("CUDA_VISIBLE_DEVICES");
}

bool whisper_engine::has_cuda() {
    auto handle = dlopen("libwhisper-cublas.so", RTLD_LAZY);
    if (!handle) {
        LOGW("failed to open whisper-cublas lib: " << dlerror());
        return false;
    }

    dlclose(handle);

    return true;
}

bool whisper_engine::has_opencl() {
    auto handle = dlopen("libwhisper-clblast.so", RTLD_LAZY);
    if (!handle) {
        LOGW("failed to open whisper-clblast lib: " << dlerror());
        return false;
    }

    dlclose(handle);

    return true;
}

bool whisper_engine::has_hip() {
    auto handle = dlopen("libwhisper-hipblas.so", RTLD_LAZY);
    if (!handle) {
        LOGW("failed to open whisper-hipblas lib: " << dlerror());
        return false;
    }

    dlclose(handle);

    return true;
}

void whisper_engine::open_whisper_lib() {
#ifdef ARCH_ARM_32
    if (cpu_tools::cpuinfo().feature_flags &
        cpu_tools::feature_flags_t::asimd) {
        LOGD("using whisper-openblas");
        m_whisperlib_handle = dlopen("libwhisper-openblas.so", RTLD_LAZY);
    } else {
        LOGW("using whisper-fallback");
        m_whisperlib_handle = dlopen("libwhisper-fallback.so", RTLD_LAZY);
    }
#elif ARCH_ARM_64
    LOGD("using whisper-openblas");
    m_whisperlib_handle = dlopen("libwhisper-openblas.so", RTLD_LAZY);
#else
    if (auto cpuinfo = cpu_tools::cpuinfo();
        cpuinfo.feature_flags & cpu_tools::feature_flags_t::avx &&
        cpuinfo.feature_flags & cpu_tools::feature_flags_t::avx2 &&
        cpuinfo.feature_flags & cpu_tools::feature_flags_t::fma &&
        cpuinfo.feature_flags & cpu_tools::feature_flags_t::f16c) {
        if (m_config.use_gpu) {
            if (m_config.gpu_device.api == gpu_api_t::cuda) {
                LOGD("using whisper-cublas");

                if (m_config.gpu_device.id >= 0)
                    setenv("CUDA_VISIBLE_DEVICES",
                           std::to_string(m_config.gpu_device.id).c_str(), 1);
                else
                    unsetenv("CUDA_VISIBLE_DEVICES");

                m_whisperlib_handle = dlopen("libwhisper-cublas.so", RTLD_LAZY);
                if (m_whisperlib_handle == nullptr)
                    LOGE("failed to open libwhisper-cublas.so: " << dlerror());
            } else if (m_config.gpu_device.api == gpu_api_t::rocm) {
                LOGD("using whisper-hipblas");

                if (m_config.gpu_device.id >= 0)
                    setenv("HIP_VISIBLE_DEVICES",
                           std::to_string(m_config.gpu_device.id).c_str(), 1);
                else
                    unsetenv("HIP_VISIBLE_DEVICES");

                m_whisperlib_handle =
                    dlopen("libwhisper-hipblas.so", RTLD_LAZY);
                if (m_whisperlib_handle == nullptr)
                    LOGE("failed to open libwhisper-hipblas.so: " << dlerror());
            } else if (m_config.gpu_device.api == gpu_api_t::opencl) {
                LOGD("using whisper-clblast");

                if (!m_config.gpu_device.platform_name.empty() &&
                    !m_config.gpu_device.name.empty()) {
                    setenv("GGML_OPENCL_PLATFORM",
                           m_config.gpu_device.platform_name.c_str(), 1);
                    setenv("GGML_OPENCL_DEVICE",
                           m_config.gpu_device.name.c_str(), 1);
                } else {
                    unsetenv("GGML_OPENCL_PLATFORM");
                    unsetenv("GGML_OPENCL_DEVICE");
                }

                m_whisperlib_handle =
                    dlopen("libwhisper-clblast.so", RTLD_LAZY);
                if (m_whisperlib_handle == nullptr)
                    LOGE("failed to open libwhisper-clblast.so: " << dlerror());
            }
        }

        if (m_whisperlib_handle == nullptr) {
            LOGD("using whisper-openblas");
            m_whisperlib_handle = dlopen("libwhisper-openblas.so", RTLD_LAZY);
        }
    } else {
        LOGW("using whisper-fallback");
        m_whisperlib_handle = dlopen("libwhisper-fallback.so", RTLD_LAZY);
    }
#endif

    if (m_whisperlib_handle == nullptr) {
        LOGE("failed to open whisper lib: " << dlerror());
        throw std::runtime_error("failed to open whisper lib");
    }

    m_whisper_api.whisper_init_from_file =
        reinterpret_cast<decltype(m_whisper_api.whisper_init_from_file)>(
            dlsym(m_whisperlib_handle, "whisper_init_from_file"));
    m_whisper_api.whisper_print_system_info =
        reinterpret_cast<decltype(m_whisper_api.whisper_print_system_info)>(
            dlsym(m_whisperlib_handle, "whisper_print_system_info"));
    m_whisper_api.whisper_full =
        reinterpret_cast<decltype(m_whisper_api.whisper_full)>(
            dlsym(m_whisperlib_handle, "whisper_full"));
    m_whisper_api.whisper_full_n_segments =
        reinterpret_cast<decltype(m_whisper_api.whisper_full_n_segments)>(
            dlsym(m_whisperlib_handle, "whisper_full_n_segments"));
    m_whisper_api.whisper_full_get_segment_text =
        reinterpret_cast<decltype(m_whisper_api.whisper_full_get_segment_text)>(
            dlsym(m_whisperlib_handle, "whisper_full_get_segment_text"));
    m_whisper_api.whisper_full_get_segment_t0 =
        reinterpret_cast<decltype(m_whisper_api.whisper_full_get_segment_t0)>(
            dlsym(m_whisperlib_handle, "whisper_full_get_segment_t0"));
    m_whisper_api.whisper_full_get_segment_t1 =
        reinterpret_cast<decltype(m_whisper_api.whisper_full_get_segment_t1)>(
            dlsym(m_whisperlib_handle, "whisper_full_get_segment_t1"));
    m_whisper_api.whisper_free =
        reinterpret_cast<decltype(m_whisper_api.whisper_free)>(
            dlsym(m_whisperlib_handle, "whisper_free"));
    m_whisper_api.whisper_full_default_params =
        reinterpret_cast<decltype(m_whisper_api.whisper_full_default_params)>(
            dlsym(m_whisperlib_handle, "whisper_full_default_params"));

    if (!m_whisper_api.ok()) {
        LOGE("failed to register whisper api");
        throw std::runtime_error("failed to register whisper api");
    }
}

void whisper_engine::push_buf_to_whisper_buf(
    const std::vector<in_buf_t::buf_t::value_type>& buf,
    whisper_buf_t& whisper_buf) {
    // convert s16 to f32 sample format
    std::transform(buf.cbegin(), buf.cend(), std::back_inserter(whisper_buf),
                   [](auto sample) {
                       return static_cast<whisper_buf_t::value_type>(sample) /
                              32768.0F;
                   });
}

void whisper_engine::push_buf_to_whisper_buf(in_buf_t::buf_t::value_type* data,
                                             in_buf_t::buf_t::size_type size,
                                             whisper_buf_t& whisper_buf) {
    // convert s16 to f32 sample format
    whisper_buf.reserve(whisper_buf.size() + size);
    for (size_t i = 0; i < size; ++i) {
        whisper_buf.push_back(static_cast<whisper_buf_t::value_type>(data[i]) /
                              32768.0F);
    }
}

void whisper_engine::reset_impl() { m_speech_buf.clear(); }

void whisper_engine::stop_processing_impl() {
    if (m_whisper_ctx) {
        LOGD("whisper cancel");
    }
}

void whisper_engine::start_processing_impl() { create_model(); }

void whisper_engine::create_model() {
    if (m_whisper_ctx) return;

    LOGD("creating whisper model");

    m_whisper_ctx = m_whisper_api.whisper_init_from_file(
        m_config.model_files.model_file.c_str());

    if (m_whisper_ctx == nullptr) {
        LOGE("failed to create whisper model");
        throw std::runtime_error("failed to create whisper model");
    }

    LOGD("whisper model created");
}

stt_engine::samples_process_result_t whisper_engine::process_buff() {
    if (!lock_buff_for_processing())
        return samples_process_result_t::wait_for_samples;

    auto eof = m_in_buf.eof;
    auto sof = m_in_buf.sof;

    LOGD("process samples buf: mode="
         << m_config.speech_mode << ", in-buf size=" << m_in_buf.size
         << ", speech-buf size=" << m_speech_buf.size() << ", sof=" << sof
         << ", eof=" << eof);

    if (sof) {
        m_speech_buf.clear();
        m_start_time.reset();
        m_vad.reset();
        reset_segment_counters();
    }

    m_denoiser.process(m_in_buf.buf.data(), m_in_buf.size);

    const auto& vad_buf =
        m_vad.remove_silence(m_in_buf.buf.data(), m_in_buf.size);

    bool vad_status = !vad_buf.empty();

    if (vad_status) {
        LOGD("vad: speech detected");

        if (m_config.speech_mode != speech_mode_t::manual &&
            m_config.speech_mode != speech_mode_t::single_sentence)
            set_speech_detection_status(
                speech_detection_status_t::speech_detected);

        if (m_config.text_format == text_format_t::raw)
            push_buf_to_whisper_buf(vad_buf, m_speech_buf);
        else
            push_buf_to_whisper_buf(m_in_buf.buf.data(), m_in_buf.size,
                                    m_speech_buf);

        restart_sentence_timer();
    } else {
        LOGD("vad: no speech");

        if (m_config.speech_mode == speech_mode_t::single_sentence &&
            m_speech_buf.empty() && sentence_timer_timed_out()) {
            LOGD("sentence timeout");
            m_call_backs.sentence_timeout();
        }

        if (m_config.speech_mode == speech_mode_t::automatic)
            set_speech_detection_status(speech_detection_status_t::no_speech);

        if (m_speech_buf.empty())
            m_segment_time_discarded_before +=
                (1000 * m_in_buf.size) / m_sample_rate;
        else
            m_segment_time_discarded_after +=
                (1000 * m_in_buf.size) / m_sample_rate;
    }

    m_in_buf.clear();

    auto decode_samples = [&] {
        if (m_speech_buf.size() > m_speech_max_size) {
            LOGD("speech buf reached max size");
            return true;
        }

        if (m_speech_buf.empty()) return false;

        if ((m_config.speech_mode == speech_mode_t::manual ||
             m_speech_detection_status ==
                 speech_detection_status_t::speech_detected) &&
            vad_status && !eof)
            return false;

        if ((m_config.speech_mode == speech_mode_t::manual ||
             m_config.speech_mode == speech_mode_t::single_sentence) &&
            m_speech_detection_status == speech_detection_status_t::no_speech &&
            !eof)
            return false;

        return true;
    }();

    if (!decode_samples) {
        if (eof || (m_config.speech_mode == speech_mode_t::manual &&
                    m_speech_detection_status ==
                        speech_detection_status_t::no_speech)) {
            flush(eof ? flush_t::eof : flush_t::regular);
            free_buf();
            return samples_process_result_t::no_samples_needed;
        }

        free_buf();
        return samples_process_result_t::wait_for_samples;
    }

    if (m_thread_exit_requested) {
        free_buf();
        return samples_process_result_t::no_samples_needed;
    }

    set_state(state_t::decoding);

    if (!vad_status) {
        set_speech_detection_status(speech_detection_status_t::no_speech);
    }

    LOGD("speech frame: samples=" << m_speech_buf.size());

    m_segment_time_offset += m_segment_time_discarded_before;
    m_segment_time_discarded_before = 0;

    decode_speech(m_speech_buf);

    m_segment_time_offset += (m_segment_time_discarded_after +
                              (1000 * m_speech_buf.size() / m_sample_rate));
    m_segment_time_discarded_after = 0;

    set_state(state_t::idle);

    if (m_config.speech_mode == speech_mode_t::single_sentence &&
        (!m_intermediate_text || m_intermediate_text->empty())) {
        LOGD("no speech decoded, forcing sentence timeout");
        m_call_backs.sentence_timeout();
    }

    m_speech_buf.clear();

    flush(eof || m_config.speech_mode == speech_mode_t::single_sentence
              ? flush_t::eof
              : flush_t::regular);

    free_buf();

    return samples_process_result_t::wait_for_samples;
}

static bool encoder_begin_callback([[maybe_unused]] void* ctx,
                                   [[maybe_unused]] void* state,
                                   void* user_data) {
    bool is_aborted = *static_cast<bool*>(user_data);
    return !is_aborted;
}

static bool abort_callback(void* user_data) {
    bool is_aborted = *static_cast<bool*>(user_data);
    return is_aborted;
}

whisper_full_params whisper_engine::make_wparams() {
    whisper_full_params wparams =
        m_whisper_api.whisper_full_default_params(WHISPER_SAMPLING_GREEDY);

    if (auto pos = m_config.lang.find('-'); pos != std::string::npos) {
        m_config.lang = m_config.lang.substr(0, pos);
    }

    wparams.language = m_config.lang_code.empty() ? m_config.lang.c_str()
                                                  : m_config.lang_code.c_str();
    wparams.speed_up = false;
    wparams.suppress_blank = true;
    wparams.suppress_non_speech_tokens = true;
    wparams.single_segment = false;
    wparams.translate = m_config.translate;
    wparams.n_threads = std::min(
        m_threads,
        std::max(1, static_cast<int>(std::thread::hardware_concurrency())));
    wparams.encoder_begin_callback = encoder_begin_callback;
    wparams.encoder_begin_callback_user_data = &m_thread_exit_requested;
    wparams.abort_callback = abort_callback;
    wparams.abort_callback_user_data = &m_thread_exit_requested;
    wparams.print_progress = false;
    wparams.print_timestamps = false;

    LOGD("cpu info: arch=" << cpu_tools::arch() << ", cores="
                           << std::thread::hardware_concurrency());
    LOGD("using threads: " << wparams.n_threads << "/"
                           << std::thread::hardware_concurrency());
    LOGD("system info: " << m_whisper_api.whisper_print_system_info());

    return wparams;
}

void whisper_engine::decode_speech(const whisper_buf_t& buf) {
    LOGD("speech decoding started");

    create_model();

    auto decoding_start = std::chrono::steady_clock::now();

    bool subrip = m_config.text_format == text_format_t::subrip;

    std::ostringstream os;

    if (auto ret = m_whisper_api.whisper_full(m_whisper_ctx, m_wparams,
                                              buf.data(), buf.size());
        ret == 0) {
        auto n = m_whisper_api.whisper_full_n_segments(m_whisper_ctx);
        LOGD("decoded segments: " << n);

        for (auto i = 0; i < n; ++i) {
            std::string text =
                m_whisper_api.whisper_full_get_segment_text(m_whisper_ctx, i);
            rtrim(text);
            ltrim(text);
#ifdef DEBUG
            LOGD("segment " << i << ": " << text);
#endif
            if (subrip) {
                size_t t0 = std::max<int64_t>(
                                0, m_whisper_api.whisper_full_get_segment_t0(
                                       m_whisper_ctx, i)) *
                            10;
                size_t t1 = std::max<int64_t>(
                                0, m_whisper_api.whisper_full_get_segment_t1(
                                       m_whisper_ctx, i)) *
                            10;

                t0 += m_segment_time_offset;
                t1 += m_segment_time_offset;

                text_tools::segment_t segment{i + 1 + m_segment_offset, t0, t1,
                                              text};
                text_tools::break_segment_to_multiline(
                    m_config.sub_config.min_line_length,
                    m_config.sub_config.max_line_length, segment);

                text_tools::segment_to_subrip_text(segment, os);
            } else {
                if (i != 0) os << ' ';
                os << text;
            }
        }

        m_segment_offset += n;
    } else {
        LOGE("whisper error: " << ret);
        return;
    }

    if (m_thread_exit_requested) return;

    auto decoding_dur = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - decoding_start)
                            .count();

    LOGD("speech decoded, stats: samples="
         << buf.size() << ", duration=" << decoding_dur << "ms ("
         << static_cast<double>(decoding_dur) /
                ((1000 * buf.size()) / static_cast<double>(m_sample_rate))
         << ")");

    auto result =
        merge_texts(m_intermediate_text.value_or(std::string{}), os.str());

#ifdef DEBUG
    LOGD("speech decoded: text=" << result);
#endif

    if (!m_intermediate_text || m_intermediate_text != result)
        set_intermediate_text(result);
}
