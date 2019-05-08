#pragma once

#define DR_WAV_IMPLEMENTATION
#include "miniaudio/extras/dr_wav.h"

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_PULSEAUDIO
#include "miniaudio/miniaudio.h"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

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
        if (!silence) {
            std::unique_lock<std::mutex> lock{ mutex };
            cv_finished.wait(lock, [this] { return silence; });
        }
    }

    bool is_playing() const
    {
        return !silence;
    }

protected:
    audio() //this class is not supposed to be used directly
        : decoder{}
    {
    }

private:
    void rewind()
    {
        ma_decoder_seek_to_pcm_frame(&decoder, 0);
    }

    unsigned data(void* output, unsigned frame_count)
    {
        const auto framesDecoded = ma_decoder_read_pcm_frames(&decoder, output, frame_count);

        bool silenceNow = framesDecoded == 0;
        if (silenceNow && !silence) {
            {
                std::lock_guard<std::mutex> lock { mutex };
                silence = true;
            }
            finish_playing_callback();
        } else {
            silence = silenceNow;
        }

        return unsigned(framesDecoded);
    }

    void finish_playing_callback()
    {
        cv_finished.notify_all();
    }

protected:
    ma_decoder decoder;

private:
    mutable std::mutex mutex;
    mutable std::condition_variable cv_finished;
    bool silence{ true };
};

/* Audio played directly from the file */
class audio_file final : public audio {

public:
    audio_file(const char* file_name)
    {
        ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 2, 44100); //TODO read from device
        ma_result result = ma_decoder_init_file(file_name, &cfg, &decoder);
        if (result != MA_SUCCESS) {
            throw mapp_exception{};
        }
    }
};

/* Audio played directly from the memory. Does not take ownership of the memory */
class audio_memory_view final : public audio {
public:
    audio_memory_view(const void* data, std::size_t size)
    {
        ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 2, 44100); //TODO read from device
        ma_result result = ma_decoder_init_memory(data, size, &cfg, &decoder);
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
    using float32 = float; // TODO
    static_assert(sizeof(float32) == 4, "Platform is not supported");

public:
    explicit oastream(const oastream_config& config = oastream_config{})
        : device{}
        , dev_config(make_ma_config(config))
    {
        if (ma_device_init(nullptr, &dev_config, &device) != MA_SUCCESS) {
            throw mapp_exception{};
        }
    }

    ~oastream()
    {
        ma_device_uninit(&device);
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

    void stop()
    {
        if (!silence)
            ma_device_stop(&device);
    }

    // starts when the stream is stopped
    void play(audio& audio)
    {
        audio.rewind();
        audio.silence = false;
        audios.push_back(&audio);
        play_impl();
    }

    void wait() const
    {
        if (!silence) {
            std::unique_lock<std::mutex> lock{ mutex };
            cv_finished.wait(lock, [this] { return silence; });
        }
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
        std::size_t buffer_size_frames = frameCount * dev_config.playback.channels;

        frames_buffer.reserve(buffer_size_frames);
        float32* audio_output = frames_buffer.data();
        auto fOutput = static_cast<float32*>(pOutput);
        std::memset(fOutput, 0, buffer_size_frames * sizeof(float32));

        for (auto* audio : audios) {
            const auto framesDecoded = audio->data(audio_output, frameCount);
            for (unsigned i = 0; i < framesDecoded * dev_config.playback.channels; ++i)
                fOutput[i] += audio_output[i];
        }

        //remove all finished audios:
        auto toRemove = std::remove_if(audios.begin(), audios.end(), [](const audio* a) { return !a->is_playing(); });
        audios.erase(toRemove, audios.end());
        //if (audios.empty()) {
        //    ma_device_stop(&device); //TODO: https://github.com/dr-soft/miniaudio/issues/64
        //}
        if (audios.empty() && !silence) {
            {
                std::lock_guard<std::mutex> lock{ mutex };
                silence = true;
            }
            finish_playing_callback();
        }
        silence = audios.empty();
    }

    void finish_playing_callback()
    {
        cv_finished.notify_all();
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
        silence = false;
        if (ma_device__get_state(&device) != MA_STATE_STOPPED) //TODO: race condition
            return;
        if (ma_device_start(&device) != MA_SUCCESS) {
            throw mapp_exception{};
        }
    }

private:
    ma_device device;
    ma_device_config dev_config;
    mutable std::mutex mutex;
    mutable std::condition_variable cv_finished;
    std::vector<audio*> audios;
    std::vector<float32> frames_buffer;
    bool silence{ true };
};

oastream& operator<<(oastream& aout, audio& a)
{
    aout.play(a);
    return aout;
}
}
