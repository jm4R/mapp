#pragma once

#define DR_WAV_IMPLEMENTATION
#include "miniaudio/extras/dr_wav.h"

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_PULSEAUDIO
#include "miniaudio/miniaudio.h"

#include <algorithm> //remove_if
#include <condition_variable> //condition_variable
#include <cstring> //memset
#include <exception> //exception
#include <functional> //function
#include <mutex> //mutex
#include <utility> //move
#include <vector> //vector

namespace mapp {

class mapp_exception final : std::exception {
public:
    enum reason {
        general
    } the_reason;

    mapp_exception(reason r = general)
        : the_reason(r)
    {
    }

    const char*
    what() const noexcept override
    {
        switch (the_reason) {
        case general:
            return "General MAPP problem";
        }
        return "";
    }
};

class audio {
    friend class oastream;

public:
    void wait() const
    {
        if (!m_silence) {
            std::unique_lock<std::mutex> lock{ m_mutex };
            m_cv_finished.wait(lock, [this] { return m_silence; });
        }
    }

    bool is_playing() const
    {
        return !m_silence;
    }

    /// Stops device asynchronously. Call wait to ensure audio is stopped. Finish callback will be called.
    void stop()
    {
        m_stop_later = true;
    }

    /// Callback can not call mapp API, see https://github.com/dr-soft/miniaudio/issues/64
    void set_finish_callback(std::function<void()> callback)
    {
        m_on_finish_callback = std::move(callback);
    }

protected:
    audio() /// This class is not supposed to be used directly.
        : m_decoder{}
    {
    }

private:
    void rewind()
    {
        m_stop_later = false;
        ma_decoder_seek_to_pcm_frame(&m_decoder, 0);
    }

    unsigned data(void* output, unsigned frame_count)
    {
        const auto framesDecoded = ma_decoder_read_pcm_frames(&m_decoder, output, frame_count);

        bool silenceNow = framesDecoded == 0 || m_stop_later;
        if (silenceNow && !m_silence) {
            {
                std::lock_guard<std::mutex> lock { m_mutex };
                m_silence = true;
            }
            finish_playing_callback();
        } else {
            m_silence = silenceNow;
        }

        return unsigned(framesDecoded);
    }

    void finish_playing_callback()
    {
        if (m_on_finish_callback)
            m_on_finish_callback();
        m_cv_finished.notify_all();
    }

protected:
    ma_decoder m_decoder;

private:
    mutable std::mutex m_mutex{};
    mutable std::condition_variable m_cv_finished{};
    std::function<void()> m_on_finish_callback{};
    bool m_silence{ true };
    bool m_stop_later{ false };
};

/// Audio played directly from the file
class audio_file final : public audio {

public:
    audio_file(const char* file_name)
    {
        ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 2, 44100); //TODO read from device
        ma_result result = ma_decoder_init_file(file_name, &cfg, &m_decoder);
        if (result != MA_SUCCESS) {
            throw mapp_exception{};
        }
    }
};

/// Audio played directly from the memory. Does not take ownership of the memory.
class audio_memory_view final : public audio {
public:
    audio_memory_view(const void* data, std::size_t size)
    {
        ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 2, 44100); //TODO read from device
        ma_result result = ma_decoder_init_memory(data, size, &cfg, &m_decoder);
        if (result != MA_SUCCESS) {
            throw mapp_exception{};
        }
    }
};

struct oastream_config {
    unsigned int sample_rate{ 44100 };
    unsigned int buffer_size_ms{ 200 };
    unsigned short channels{ 2 };
};

class oastream final {
    using float32 = float;
    static_assert(sizeof(float32) == 4, "Platform is not supported");

public:
    explicit oastream(const oastream_config& config = oastream_config{})
        : m_device{}
        , m_dev_config(make_ma_config(config))
    {
        if (ma_device_init(nullptr, &m_dev_config, &m_device) != MA_SUCCESS) {
            throw mapp_exception{};
        }
    }

    ~oastream()
    {
        ma_device_uninit(&m_device);
    }

    oastream(const oastream&) = delete;
    oastream(oastream&&) = delete;

    oastream& operator=(const oastream&) = delete;
    oastream& operator=(oastream&&) = delete;

public:
    void start()
    {
        play_impl();
    }

    /// Removes all audios without stopping stream. Call wait before destroing any audio.
    /// Audios finish callbacks will not be invoked.
    void stop_audios()
    {
        m_stop_later = true;
    }

    /// Removes all audios and stops stream. Call wait before destroing any audio.
    void stop_stream()
    {
        stop_audios();
        ma_device_stop(&m_device);
    }

    /// Starts when the stream is stopped.
    void play(audio& audio)
    {
        audio.rewind();
        audio.m_silence = false;
        m_audios.push_back(&audio);
        play_impl();
    }

    void wait() const
    {
        if (!m_silence) {
            std::unique_lock<std::mutex> lock{ m_mutex };
            m_cv_finished.wait(lock, [this] { return m_silence; });
        }
    }

    /// Sets a volume in (0.0 - 1.0) range. Setting volume higher than 1.0 may cause crackles.
    void set_volume(float value)
    {
        m_volume = value;
    }

private:
    static void data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
    {
        (void)pInput;
        auto self = static_cast<oastream*>(pDevice->pUserData);
        self->data_callback_impl(pOutput, frameCount);
    }

    void data_callback_impl(void* pOutput, ma_uint32 frameCount)
    {
        std::size_t buffer_size_frames = frameCount * m_dev_config.playback.channels;

        m_frames_buffer.reserve(buffer_size_frames);
        float32* audio_output = m_frames_buffer.data();
        auto fOutput = static_cast<float32*>(pOutput);
        std::memset(fOutput, 0, buffer_size_frames * sizeof(float32));

        for (auto* audio : m_audios) {
            const auto framesDecoded = audio->data(audio_output, frameCount);
            for (unsigned i = 0; i < framesDecoded * m_dev_config.playback.channels; ++i)
                fOutput[i] += m_volume * audio_output[i];
        }

        // Remove all finished audios:
        auto toRemove = m_stop_later ? m_audios.begin()
                                    : std::remove_if(m_audios.begin(), m_audios.end(), [](const audio* a) { return !a->is_playing(); });
        m_audios.erase(toRemove, m_audios.end());
        if (m_audios.empty() && !m_silence) {
            {
                std::lock_guard<std::mutex> lock{ m_mutex };
                m_silence = true;
            }
            finish_playing_callback();
        }
        m_silence = m_audios.empty();
    }

    void finish_playing_callback()
    {
        m_stop_later = false;
        m_cv_finished.notify_all();
    }

    ma_device_config make_ma_config(const oastream_config& oa_cfg)
    {
        ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
        cfg.playback.format = ma_format_f32;
        cfg.playback.channels = oa_cfg.channels;
        cfg.sampleRate = oa_cfg.sample_rate;
        cfg.dataCallback = data_callback;
        cfg.pUserData = this;
        cfg.bufferSizeInMilliseconds = oa_cfg.buffer_size_ms;
        return cfg;
    }

    void play_impl()
    {
        m_silence = false;
        if (ma_device__get_state(&m_device) != MA_STATE_STOPPED) //TODO: race condition
            return;
        if (ma_device_start(&m_device) != MA_SUCCESS) {
            throw mapp_exception{};
        }
    }

private:
    ma_device m_device;
    ma_device_config m_dev_config;
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_cv_finished;
    std::vector<audio*> m_audios;
    std::vector<float32> m_frames_buffer;
    float m_volume { 1.0f };
    bool m_stop_later{ false };
    bool m_silence{ true };
};

oastream& operator<<(oastream& aout, audio& a)
{
    aout.play(a);
    return aout;
}
}
