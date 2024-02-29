/* Copyright (C) 2023-2024 Michal Kosciesza <michal@mkiol.net>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "tts_engine.hpp"

#include <dirent.h>
#include <fmt/format.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <locale>

#ifdef ARCH_X86_64
#include <rubberband/RubberBandStretcher.h>
#endif

#include "logger.hpp"
#include "media_compressor.hpp"

static std::string file_ext_for_format(tts_engine::audio_format_t format) {
    switch (format) {
        case tts_engine::audio_format_t::wav:
            return "wav";
        case tts_engine::audio_format_t::mp3:
            return "mp3";
        case tts_engine::audio_format_t::ogg_vorbis:
            return "ogg";
        case tts_engine::audio_format_t::ogg_opus:
            return "opus";
        case tts_engine::audio_format_t::flac:
            return "flac";
    }

    throw std::runtime_error("invalid audio format");
}

static media_compressor::format_t compressor_format_from_format(
    tts_engine::audio_format_t format) {
    switch (format) {
        case tts_engine::audio_format_t::wav:
            return media_compressor::format_t::wav;
        case tts_engine::audio_format_t::mp3:
            return media_compressor::format_t::mp3;
        case tts_engine::audio_format_t::ogg_vorbis:
            return media_compressor::format_t::ogg_vorbis;
        case tts_engine::audio_format_t::ogg_opus:
            return media_compressor::format_t::ogg_opus;
        case tts_engine::audio_format_t::flac:
            return media_compressor::format_t::flac;
    }

    throw std::runtime_error("invalid audio format");
}

std::ostream& operator<<(std::ostream& os, tts_engine::gpu_api_t api) {
    switch (api) {
        case tts_engine::gpu_api_t::opencl:
            os << "opencl";
            break;
        case tts_engine::gpu_api_t::cuda:
            os << "cuda";
            break;
        case tts_engine::gpu_api_t::rocm:
            os << "rocm";
            break;
    }

    return os;
}

std::ostream& operator<<(std::ostream& os, tts_engine::audio_format_t format) {
    switch (format) {
        case tts_engine::audio_format_t::wav:
            os << "wav";
            break;
        case tts_engine::audio_format_t::mp3:
            os << "mp3";
            break;
        case tts_engine::audio_format_t::ogg_vorbis:
            os << "ogg-vorbis";
            break;
        case tts_engine::audio_format_t::ogg_opus:
            os << "ogg-opus";
            break;
        case tts_engine::audio_format_t::flac:
            os << "flac";
            break;
    }

    return os;
}

std::ostream& operator<<(std::ostream& os,
                         tts_engine::text_format_t text_format) {
    switch (text_format) {
        case tts_engine::text_format_t::raw:
            os << "raw";
            break;
        case tts_engine::text_format_t::subrip:
            os << "subrip";
            break;
    }

    return os;
}

std::ostream& operator<<(std::ostream& os,
                         const tts_engine::model_files_t& model_files) {
    os << "model-path=" << model_files.model_path
       << ", vocoder-path=" << model_files.vocoder_path
       << ", diacritizer=" << model_files.diacritizer_path;

    return os;
}

std::ostream& operator<<(std::ostream& os,
                         const tts_engine::gpu_device_t& gpu_device) {
    os << "id=" << gpu_device.id << ", api=" << gpu_device.api
       << ", name=" << gpu_device.name
       << ", platform-name=" << gpu_device.platform_name;

    return os;
}

std::ostream& operator<<(std::ostream& os, const tts_engine::config_t& config) {
    os << "lang=" << config.lang << ", speaker=" << config.speaker_id
       << ", model-files=[" << config.model_files << "]"
       << ", speaker=" << config.speaker_id
       << ", ref_voice_file=" << config.ref_voice_file
       << ", text-format=" << config.text_format
       << ", sync_subs=" << config.sync_subs << ", options=" << config.options
       << ", lang_code=" << config.lang_code
       << ", share-dir=" << config.share_dir
       << ", cache-dir=" << config.cache_dir << ", data-dir=" << config.data_dir
       << ", speech-speed=" << config.speech_speed
       << ", use-gpu=" << config.use_gpu << ", gpu-device=["
       << config.gpu_device << "]"
       << ", audio-format=" << config.audio_format;
    return os;
}

std::ostream& operator<<(std::ostream& os, tts_engine::state_t state) {
    switch (state) {
        case tts_engine::state_t::idle:
            os << "idle";
            break;
        case tts_engine::state_t::stopping:
            os << "stopping";
            break;
        case tts_engine::state_t::stopped:
            os << "stopped";
            break;
        case tts_engine::state_t::initializing:
            os << "initializing";
            break;
        case tts_engine::state_t::encoding:
            os << "encoding";
            break;
        case tts_engine::state_t::error:
            os << "error";
            break;
    }

    return os;
}

tts_engine::tts_engine(config_t config, callbacks_t call_backs)
    : m_config{std::move(config)},
      m_call_backs{std::move(call_backs)},
      m_text_processor{config.use_gpu ? config.gpu_device.id : -1} {}

tts_engine::~tts_engine() {
    LOGD("tts dtor");

    if (!m_ref_voice_wav_file.empty()) unlink(m_ref_voice_wav_file.c_str());
}

void tts_engine::start() {
    LOGD("tts start");

    m_state = state_t::stopping;
    m_cv.notify_one();
    if (m_processing_thread.joinable()) m_processing_thread.join();

    m_queue = std::queue<task_t>{};
    m_state = state_t::idle;
    m_processing_thread = std::thread{&tts_engine::process, this};

    LOGD("tts start completed");
}

void tts_engine::stop() {
    LOGD("tts stop started");

    set_state(state_t::stopping);

    m_cv.notify_one();
    if (m_processing_thread.joinable()) m_processing_thread.join();

    set_state(state_t::stopped);

    LOGD("tts stop completed");
}

void tts_engine::request_stop() {
    LOGD("tts stop requested");

    set_state(state_t::stopping);
    m_cv.notify_one();
}

std::string tts_engine::first_file_with_ext(std::string dir_path,
                                            std::string&& ext) {
    auto* dirp = opendir(dir_path.c_str());
    if (!dirp) return {};

    while (auto* dirent = readdir(dirp)) {
        if (dirent->d_type != DT_REG) continue;

        std::string fn{dirent->d_name};

        if (!fn.empty() && fn.front() != '.' && fn.substr(fn.find_last_of('.') + 1) == ext)
            return dir_path.append("/").append(fn);
    }

    return {};
}

std::string tts_engine::find_file_with_name_prefix(std::string dir_path,
                                                   std::string prefix) {
    auto* dirp = opendir(dir_path.c_str());
    if (!dirp) return {};

    while (auto* dirent = readdir(dirp)) {
        if (dirent->d_type != DT_REG) continue;

        std::string fn{dirent->d_name};

        if (fn.size() < prefix.size()) continue;

        if (fn.substr(0, prefix.size()) == prefix)
            return dir_path.append("/").append(fn);
    }

    return {};
}

void tts_engine::encode_speech(std::string text) {
    if (is_shutdown()) return;

    auto tasks = make_tasks(text);

    if (tasks.empty()) {
        LOGW("no task to process");
        tasks.push_back(task_t{"", 0, 0, true, true});
    }

    {
        std::lock_guard lock{m_mutex};
        for (auto& task : tasks) m_queue.push(std::move(task));
    }

    LOGD("task pushed");

    m_cv.notify_one();
}

void tts_engine::set_speech_speed(unsigned int speech_speed) {
    m_config.speech_speed = std::clamp(speech_speed, 1u, 20u);
}

void tts_engine::set_ref_voice_file(std::string ref_voice_file) {
    m_config.ref_voice_file.assign(std::move(ref_voice_file));
    m_ref_voice_wav_file.clear();
}

void tts_engine::set_state(state_t new_state) {
    if (is_shutdown()) {
        switch (new_state) {
            case state_t::idle:
            case state_t::stopping:
            case state_t::initializing:
            case state_t::encoding:
                new_state = state_t::stopping;
                break;
            case state_t::stopped:
            case state_t::error:
                break;
        }
    }

    if (m_state != new_state) {
        LOGD("tts engine state: " << m_state << " => " << new_state);
        m_state = new_state;
        m_call_backs.state_changed(m_state);
    }
}

static decltype(timespec::tv_sec) create_date_sec(const std::string& file) {
    struct stat result;
    if (stat(file.c_str(), &result) == 0) return result.st_ctim.tv_sec;
    return 0;
}

std::string tts_engine::path_to_output_file(const std::string& text) const {
    auto hash = std::hash<std::string>{}(
        text + m_config.model_files.model_path +
        m_config.model_files.vocoder_path + m_config.ref_voice_file +
        std::to_string(create_date_sec(m_config.ref_voice_file)) +
        m_config.model_files.diacritizer_path + m_config.speaker_id +
        m_config.lang +
        (m_config.speech_speed == 10 ? ""
                                     : std::to_string(m_config.speech_speed)));
    return m_config.cache_dir + "/" + std::to_string(hash) + '.' +
           file_ext_for_format(m_config.audio_format);
}

std::string tts_engine::path_to_output_silence_file(
    size_t duration_msec, audio_format_t format) const {
    return fmt::format("{}/silence_{}.{}", m_config.cache_dir, duration_msec,
                       file_ext_for_format(format));
}

static bool file_exists(const std::string& file_path) {
    struct stat buffer {};
    return stat(file_path.c_str(), &buffer) == 0;
}

// source: https://stackoverflow.com/a/217605
// trim from start (in place)
static inline void ltrim(std::string& s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
                return !std::isspace(ch);
            }));
}
// trim from end (in place)
static inline void rtrim(std::string& s) {
    s.erase(std::find_if(s.rbegin(), s.rend(),
                         [](unsigned char ch) { return !std::isspace(ch); })
                .base(),
            s.end());
}
// trim from both ends (in place)
static inline void trim(std::string& s) {
    rtrim(s);
    ltrim(s);
}

std::vector<tts_engine::task_t> tts_engine::make_tasks(const std::string& text,
                                                       bool split) const {
    std::vector<tts_engine::task_t> tasks;

    if (m_config.text_format == text_format_t::subrip) {
        auto subrip_start_idx = text_tools::subrip_text_start(text, 100);
        if (subrip_start_idx) {
            auto segments =
                text_tools::subrip_text_to_segments(text, *subrip_start_idx);

            if (!segments.empty()) {
                tasks.reserve(segments.size());

                if (!m_config.sync_subs) {
                    segments.front().t0 = 0;
                    segments.front().t1 = 0;
                }

                tasks.push_back(task_t{std::move(segments.front().text),
                                       segments.front().t0, segments.front().t1,
                                       true, false});

                for (auto it = segments.begin() + 1; it != segments.end();
                     ++it) {
                    if (!m_config.sync_subs) {
                        it->t0 = 0;
                        it->t1 = 0;
                    }
                    tasks.push_back(task_t{std::move(it->text), it->t0, it->t1,
                                           false, false});
                }

                tasks.back().last = true;

                return tasks;
            }
        }

        LOGW("tts fallback to plain text");
    }

    if (split) {
        auto engine = m_config.has_option('a')
                          ? text_tools::split_engine_t::astrunc
                          : text_tools::split_engine_t::ssplit;
        auto [parts, _] =
            text_tools::split(text, engine, m_config.lang, m_config.nb_data);
        if (!parts.empty()) {
            tasks.reserve(parts.size());
            tasks.push_back(
                task_t{std::move(parts.front()), 0, 0, true, false});

            for (auto it = parts.begin() + 1; it != parts.end(); ++it) {
                trim(*it);
                if (!it->empty())
                    tasks.push_back(task_t{std::move(*it), 0, 0, false, false});
            }

            tasks.back().last = true;
        }
    } else {
        tasks.push_back(task_t{text, 0, 0, true, true});
    }

    return tasks;
}

#ifdef ARCH_X86_64
static void sample_buf_s16_to_f32(const int16_t* input, float* output,
                                  size_t size) {
    for (size_t i = 0; i < size; ++i)
        output[i] = static_cast<float>(input[i]) / 32768.0F;
}

static void sample_buf_f32_to_s16(const float* input, int16_t* output,
                                  size_t size) {
    for (size_t i = 0; i < size; ++i)
        output[i] = static_cast<int16_t>(input[i] * 32768.0F);
}

bool tts_engine::stretch(const std::string& input_file,
                         const std::string& output_file, double time_ration,
                         double pitch_ratio) {
    std::ifstream is{input_file, std::ios::binary | std::ios::ate};
    if (!is) {
        LOGE("failed to open input file for stretch: " << input_file);
        return false;
    }

    std::ofstream os{output_file, std::ios::binary};
    if (!os) {
        LOGE("failed to open output file for stretch: " << output_file);
        return false;
    }

    size_t size = is.tellg();
    if (size < sizeof(wav_header)) {
        LOGE("file header is too short");
        os.close();
        unlink(output_file.c_str());
        return false;
    }

    is.seekg(0, std::ios::beg);
    auto header = read_wav_header(is);

    if (header.num_channels != 1) {
        LOGE("stretching is supported only for mono");
        os.close();
        unlink(output_file.c_str());
        return false;
    }

    size -= is.tellg();

    LOGD("stretcher sample rate: " << header.sample_rate);

    RubberBand::RubberBandStretcher rb{
        header.sample_rate, /*mono*/ 1,
        RubberBand::RubberBandStretcher::DefaultOptions |
            RubberBand::RubberBandStretcher::OptionProcessOffline |
            RubberBand::RubberBandStretcher::OptionEngineFiner |
            RubberBand::RubberBandStretcher::OptionSmoothingOn |
            RubberBand::RubberBandStretcher::OptionTransientsSmooth |
            RubberBand::RubberBandStretcher::OptionWindowLong,
        time_ration, pitch_ratio};

    static const size_t buf_c_size = 8192;
    static const size_t buf_f_size = 4096;

    char buf_c[buf_c_size];
    float buf_f[buf_f_size];
    float* buf_f_ptr[2] = {buf_f, nullptr};  // mono

    while (is) {
        const auto size_to_read = std::min<size_t>(size, buf_c_size);
        const auto size_to_write = size_to_read / sizeof(int16_t);
        is.read(buf_c, size_to_read);
        sample_buf_s16_to_f32(reinterpret_cast<int16_t*>(buf_c), buf_f,
                              size_to_write);
        float* buf_f_c[2] = {buf_f, nullptr};
        rb.study(buf_f_c, size_to_write, !static_cast<bool>(is));
    }

    is.clear();
    is.seekg(sizeof(wav_header), std::ios::beg);
    os.seekp(sizeof(wav_header));

    while (is) {
        const auto size_to_read = std::min<size_t>(size, buf_c_size);
        const auto size_to_write = size_to_read / sizeof(int16_t);
        is.read(buf_c, size_to_read);
        sample_buf_s16_to_f32(reinterpret_cast<int16_t*>(buf_c), buf_f,
                              size_to_write);
        rb.process(buf_f_ptr, size_to_write, !is);

        while (true) {
            auto size_rb = rb.available();
            if (size_rb <= 0) break;

            auto size_r =
                rb.retrieve(buf_f_ptr, std::min<size_t>(size_rb, buf_f_size));
            if (size_r == 0) break;

            sample_buf_f32_to_s16(buf_f, reinterpret_cast<int16_t*>(buf_c),
                                  size_r);
            os.write(buf_c, size_r * sizeof(int16_t));
        }
    }

    auto data_size = static_cast<size_t>(os.tellp()) - sizeof(wav_header);

    if (data_size == 0) {
        os.close();
        unlink(output_file.c_str());
        return false;
    }

    os.seekp(0);
    write_wav_header(header.sample_rate, sizeof(int16_t), 1,
                     data_size / sizeof(int16_t), os);

    return true;
}
#endif  // ARCH_X86_64

void tts_engine::apply_speed([[maybe_unused]] const std::string& file) const {
#ifdef ARCH_X86_64
    auto tmp_file = file + "_tmp";

    if (m_config.speech_speed > 0 && m_config.speech_speed <= 20 &&
        m_config.speech_speed != 10) {
        auto speech_speed = 20 - (m_config.speech_speed - 1);

        if (stretch(file, tmp_file, static_cast<double>(speech_speed) / 10.0,
                    1.0)) {
            unlink(file.c_str());
            rename(tmp_file.c_str(), file.c_str());
        }
    }
#endif  // ARCH_X86_64
}

void tts_engine::process() {
    LOGD("tts prosessing started");

    decltype(m_queue) queue;

    while (!is_shutdown()) {
        {
            std::unique_lock<std::mutex> lock{m_mutex};
            m_cv.wait(lock,
                      [this] { return is_shutdown() || !m_queue.empty(); });
            std::swap(queue, m_queue);
        }

        if (is_shutdown()) break;

        if (!model_created()) {
            set_state(state_t::initializing);

            create_model();

            if (!model_created()) {
                set_state(state_t::error);
                if (m_call_backs.error) m_call_backs.error();
            }
        }

        if (is_shutdown()) break;

        if (m_restart_requested) {
            m_restart_requested = false;

            if (!m_ref_voice_wav_file.empty()) {
                unlink(m_ref_voice_wav_file.c_str());
                m_ref_voice_wav_file.clear();
            }
        }

        setup_ref_voice();

        if (is_shutdown()) break;

        set_state(state_t::encoding);

        size_t speech_time = 0;
        size_t total_tasks_nb = 0;

        while (!is_shutdown() && !queue.empty()) {
            auto task = std::move(queue.front());

            if (task.first) {
                speech_time = 0;
                total_tasks_nb = queue.size();
            }

            queue.pop();

            if (task.empty() && task.last) {
                if (m_call_backs.speech_encoded) {
                    m_call_backs.speech_encoded({}, {}, audio_format_t::wav,
                                                1.0, true);
                }
                continue;
            }

            double progress =
                static_cast<double>(total_tasks_nb - queue.size()) /
                total_tasks_nb;

            auto output_file = path_to_output_file(task.text);

            if (!file_exists(output_file)) {
                auto new_text = m_text_processor.preprocess(
                    /*text=*/task.text, /*options=*/m_config.options,
                    /*lang=*/m_config.lang,
                    /*lang_code=*/m_config.lang_code,
                    /*prefix_path=*/m_config.share_dir,
                    /*diacritizer_path=*/m_config.model_files.diacritizer_path);

                auto output_file_wav =
                    m_config.audio_format == audio_format_t::wav
                        ? output_file
                        : output_file + ".wav";

                if (!encode_speech_impl(new_text, output_file_wav)) {
                    unlink(output_file.c_str());
                    LOGE("speech encoding error");
                    if (m_call_backs.speech_encoded) {
                        m_call_backs.speech_encoded(
                            "", "", m_config.audio_format, progress, task.last);
                    }

                    continue;
                }

                if (!model_supports_speed()) apply_speed(output_file_wav);

                if (m_config.audio_format != audio_format_t::wav) {
                    media_compressor{}.compress_to_file(
                        {output_file_wav}, output_file,
                        compressor_format_from_format(m_config.audio_format),
                        {media_compressor::quality_t::vbr_high,
                         false,
                         false,
                         {}});

                    unlink(output_file_wav.c_str());
                }
            }

            if (task.t1 != 0) {
                if (speech_time < task.t0) {
                    auto duration = task.t0 - speech_time;
                    speech_time += duration;

                    auto silence_out_file = path_to_output_silence_file(
                        duration, m_config.audio_format);

                    if (!file_exists(silence_out_file)) {
                        auto silence_out_file_wav = path_to_output_silence_file(
                            duration, audio_format_t::wav);

                        make_silence_wav_file(duration, silence_out_file_wav);

                        if (m_config.audio_format != audio_format_t::wav) {
                            media_compressor{}.compress_to_file(
                                {silence_out_file_wav}, silence_out_file,
                                compressor_format_from_format(
                                    m_config.audio_format),
                                {media_compressor::quality_t::vbr_high,
                                 false,
                                 false,
                                 {}});

                            unlink(silence_out_file_wav.c_str());
                        } else {
                            silence_out_file = std::move(silence_out_file_wav);
                        }
                    }

                    if (m_call_backs.speech_encoded) {
                        m_call_backs.speech_encoded("", silence_out_file,
                                                    m_config.audio_format,
                                                    progress, false);
                    }

                } else if (speech_time > task.t0) {
                    LOGW("speech delay: " << speech_time - task.t0);
                }

                speech_time += media_compressor{}.duration(output_file);
            }

            if (is_shutdown()) break;

            if (m_call_backs.speech_encoded) {
                m_call_backs.speech_encoded(task.text, output_file,
                                            m_config.audio_format, progress,
                                            task.last);
            }
        }

        if (!is_shutdown()) set_state(state_t::idle);
    }

    if (m_state != state_t::error) set_state(state_t::stopped);

    LOGD("tts processing done");
}

void tts_engine::make_silence_wav_file(size_t duration_msec,
                                       const std::string& output_file) const {
    const int sample_rate = 16000;
    const uint32_t nb_samples = sample_rate * (duration_msec / 1000.0);

    std::ofstream wav_file{output_file};

    write_wav_header(sample_rate, 2, 1, nb_samples, wav_file);

    std::string silence(nb_samples * 2, '\0');
    wav_file.write(silence.data(), silence.size());
}

void tts_engine::setup_ref_voice() {
    if (m_config.ref_voice_file.empty()) return;

    auto hash = std::hash<std::string>{}(m_config.ref_voice_file);

    m_ref_voice_wav_file =
        m_config.cache_dir + "/" + std::to_string(hash) + ".wav";

    if (!file_exists(m_ref_voice_wav_file)) {
        media_compressor{}.decompress_to_file(
            {m_config.ref_voice_file}, m_ref_voice_wav_file,
            {media_compressor::quality_t::vbr_medium, /*mono=*/false,
             /*sample_rate_16=*/false,
             /*stream=*/{}});
    }
}

// borrowed from:
// https://github.com/rhasspy/piper/blob/master/src/cpp/wavfile.hpp
void tts_engine::write_wav_header(int sample_rate, int sample_width,
                                  int channels, uint32_t num_samples,
                                  std::ofstream& wav_file) {
    wav_header header;
    header.data_size = num_samples * sample_width * channels;
    header.chunk_size = header.data_size + sizeof(wav_header) - 8;
    header.sample_rate = sample_rate;
    header.num_channels = channels;
    header.bytes_per_sec = sample_rate * sample_width * channels;
    header.block_align = sample_width * channels;
    wav_file.write(reinterpret_cast<const char*>(&header), sizeof(wav_header));
}

tts_engine::wav_header tts_engine::read_wav_header(std::ifstream& wav_file) {
    wav_header header;
    if (!wav_file.read(reinterpret_cast<char*>(&header), sizeof(wav_header)))
        throw std::runtime_error("failed to read file");

    return header;
}

float tts_engine::vits_length_scale(unsigned int speech_speed,
                                    float initial_length_scale) {
    return initial_length_scale *
           std::pow<float>(
               (-0.9f * std::clamp(speech_speed, 1u, 20u) + 19) / 10.0f, 2);
}

float tts_engine::overflow_duration_threshold(
    unsigned int speech_speed, float initial_duration_threshold) {
    return initial_duration_threshold *
           (static_cast<float>(std::clamp(speech_speed, 1u, 20u)) / 10.0f);
}
