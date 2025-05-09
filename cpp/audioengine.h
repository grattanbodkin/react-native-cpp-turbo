#ifndef AUDIOENGINE_H
#define AUDIOENGINE_H

#include <iostream>
#include <cassert>

#include <SpliceAssetData.h>
#include <elem/Runtime.h>
#include <elem/JSON.h>
#include <containers/choc_Value.h>
#include <thread>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <span>
#include <array>
#include <atomic>
#include <functional>
#include <thread>
#include <future>
#include<concurrentqueue/concurrentqueue.h>

#include "audio/choc_AudioFileFormat_Ogg.h"
#include "audio/choc_AudioFileFormat_WAV.h"
#include "containers/choc_Value.h"

#include "SpliceAssetData.h"
#include "audio/choc_AudioFileFormat.h"
#include "audio/choc_AudioFileFormat_MP3.h"

#define MINIAUDIO_IMPLEMENTATION
#include "vendor/miniaudio.h"

constexpr auto RUNTIME_QUEUE_CAPACITY = 8;


namespace elementary {

    struct DeviceProxy {
        elem::Runtime<float> runtime;
        std::vector<float> scratchData;

        DeviceProxy(double sampleRate, size_t blockSize)
            : scratchData(2 * blockSize), runtime(sampleRate, blockSize) {}

        void process(float* outputData, size_t numChannels, size_t numFrames) {
            if (scratchData.size() < (numChannels * numFrames))
                scratchData.resize(numChannels * numFrames);

            auto* deinterleaved = scratchData.data();
            std::array<float*, 2> ptrs {deinterleaved, deinterleaved + numFrames};
 
            runtime.process(
                nullptr,
                0,
                ptrs.data(),
                numChannels,
                numFrames,
                nullptr
            );

            for (size_t i = 0; i < numChannels; ++i) {
                for (size_t j = 0; j < numFrames; ++j) {
                    outputData[i + numChannels * j] = deinterleaved[i * numFrames + j];
                }
            }
        }
    };

    struct ConcurrentQueueTraits : public moodycamel::ConcurrentQueueDefaultTraits
    {
        static const size_t BLOCK_SIZE = 256;                 /// Use bigger blocks
        static const size_t IMPLICIT_INITIAL_INDEX_SIZE = 16; /// Use 16 blocks
    };

    std::vector<std::string> splitAny(const std::string& input,
        const std::string& delims = "/\\")
    {
        std::vector<std::string> tokens;
        std::size_t start = input.find_first_not_of(delims), end = 0;

        while (start != std::string::npos) {
        end = input.find_first_of(delims, start);
        tokens.emplace_back(input.substr(start, end - start));
        start = input.find_first_not_of(delims, end);
    }

return tokens;
}

    /// Convenience alias for splitting URLs or file paths:
    inline std::vector<std::string> splitPath(const std::string& path)
    {
        return splitAny(path, "/\\");
    }

    // A simple periodic timer that uses a thread to call a user-provided callback
    class PeriodicTimer {
        public:
            PeriodicTimer() : running(false) {}
        
            ~PeriodicTimer() {
                stop();
            }
        
            // Starts the timer with a user-provided callback and interval in milliseconds
            void start(std::function<void()> callback, std::chrono::milliseconds interval) {
                stop(); // Ensure any existing timer is stopped
        
                running = true;
                worker = std::thread([this, callback, interval]() {
                    auto next_tick = std::chrono::steady_clock::now();
                    while (running) {
                        // Calculate next tick time first to compensate for drift
                        next_tick += interval;
                        
                        try {
                            callback(); // Safe to call, should not block for long
                        } catch (const std::exception& e) {
                            // Consider logging or handling the exception
                            // For now, we just continue with the timer
                        }
                        
                        std::unique_lock<std::mutex> lock(mtx);
                        // Wait until next tick time or until signaled to stop
                        cv.wait_until(lock, next_tick, [this]() { return !running; });
                    }
                });
            }
        
            // Stops the timer safely
            void stop() {
                if (running) {
                    {
                        std::lock_guard<std::mutex> lock(mtx);
                        running = false;
                    }
                    cv.notify_all();
                    if (worker.joinable())
                        worker.join();
                }
            }
        
        private:
            std::thread worker;
            std::mutex mtx;
            std::condition_variable cv;
            std::atomic<bool> running;
    };

    class AudioEngine {
        public:
            AudioEngine() : 
            deviceConfig(ma_device_config_init(ma_device_type_playback)), 
            deviceInitialized(false) 
            {
                initializeDevice();
                // startEventQueueTimer();
            }

            ~AudioEngine() 
            {
                if (deviceInitialized) {
                    ma_device_uninit(&device);
                }
                eventTimer.stop();
            }

            elem::Runtime<float>& getRuntime() 
            {
                return proxy->runtime;
            }

            bool applyInstructions(elem::js::Value const& instructions) 
            {
                if(instructions.isObject())
                {
                    // Check if the instructions contain a resource map in addition to graph description
                    // and load the resources if present prior to rendering graph changes.
                    const auto& obj = instructions.getObject(); 
                    if(auto it = obj.find("resources"); it != obj.end())
                    {
                        if(it->second.isObject())
                        {
                            loadResources(it->second);
                        }
                        else
                        {
                            std::cerr << "Error: incorrect resource map format" << std::endl;
                            return false;
                        }
                    }
                    if(auto it = obj.find("graph"); it != obj.end())
                    {
                        if(it->second.isArray())
                        {
                            getRuntime().applyInstructions(it->second);
                        }
                        else
                        {
                            std::cerr << "Error: incorrect graph description format" << std::endl;
                            return false;
                        }
                    }
                }
                return true;
            }

            int getSampleRate() 
            {
                return device.sampleRate;
            }

            std::string getLatestEvent() {
                static elem::js::Array latest;
                if (eventQueue.try_dequeue(latest)) {
                    const auto object = elem::js::Object{
                        {"events", std::move(latest)},
                    };
                    const auto serializedObject = elem::js::serialize(object);
                    return serializedObject;
                }
                return ""; 
            }

            bool loadResources(const elem::js::Value& resourceMap)
            {
                if(!resourceMap.isObject())
                {
                    std::cerr << "Error: Invalid JSON format." << std::endl;
                    return false;
                }
                auto obj = resourceMap.getObject();
                for (const auto& [key, value] : obj)
                {
                    try
                    {
                        const auto assetIdentifier = key;
                        if (elemRuntimeHasSharedResource(assetIdentifier))
                        {
                            continue;
                        }
                        if (!value.isString())
                        {
                            std::cerr << "Error: File path/url must be a string" << std::endl;
                            return false;
                        }
                        const auto rawFilePath = value.toString();
                        std::string filePath = rawFilePath.rfind("file://") == 0 ? rawFilePath.substr(7) : rawFilePath;
                        if (!std::filesystem::exists(filePath))
                        {
                            std::cerr << "File not found: " << filePath << '\n';
                            continue;
                        }
                        std::ifstream inputStream(filePath, std::ios::binary);
                        if (!inputStream)
                            throw std::runtime_error("Failed to open WAV file: " + filePath);
            
                        auto formatList = choc::audio::AudioFileFormatList{};
                        // formatList.addFormat(std::make_unique<choc::audio::MP3AudioFileFormat>());
                        formatList.addFormat(std::make_unique<choc::audio::WAVAudioFileFormat<false>>());
                        auto reader = formatList.createReader (filePath);
                        if (!reader) {
                            throw std::runtime_error("Failed to create file reader: " + filePath);
                        }
                        auto loadedAudio = reader->readEntireStream<float>();
                        auto& props = reader->getProperties();
                        if (loadedAudio.getNumFrames() == 0) {
                            throw std::runtime_error("Failed to read WAV file: " + filePath);
                        }
                        if(!elemRuntimeAddSharedResource(assetIdentifier, loadedAudio))
                        {
                            std::cerr << "Error: Failed to add shared resource: " << assetIdentifier << std::endl;
                        }
                    }
                    catch (const std::exception& e)
                    {
                        std::cerr << "Error loading audio resource: " << e.what() << std::endl;
                    }
                }
                return true;
            }


        private:

            void initializeDevice() {
                proxy = std::make_unique<DeviceProxy>(44100.0, 1024);

                deviceConfig.playback.pDeviceID = nullptr;
                deviceConfig.playback.format = ma_format_f32;
                deviceConfig.playback.channels = 2;
                deviceConfig.sampleRate = 44100;
                deviceConfig.dataCallback = audioCallback;
                deviceConfig.pUserData = proxy.get();

                ma_result result = ma_device_init(nullptr, &deviceConfig, &device);
                if (result != MA_SUCCESS) {
                    std::cerr << "Failed to initialize the audio device! Exiting..." << std::endl;
                    return;
                }

                deviceInitialized = true;
                if (ma_device_start(&device)) {
                    std::cerr << "Failed to start the audio device! Exiting..." << std::endl;
                    return;
                }
            }

            // TODO: 
            // - Look into using a different timer implementation (perhaps a known 3rd party library -- choc won't work out of the box for linux)
            // - Consider using EventEmitter in turbo module spec for getting events to JS
            void startEventQueueTimer() {
                eventTimer.start([&]() {
                    elem::js::Array batch;
                    getRuntime().processQueuedEvents(
                        [&](std::string const& type, elem::js::Value evt) {
                            batch.push_back(elem::js::Object({{"type", type}, {"event", evt}}));
                        });
                    eventQueue.try_enqueue(batch);
                }, std::chrono::milliseconds(1000/60));    
            }

            static void audioCallback(ma_device* pDevice, void* pOutput, const void* /* pInput */, ma_uint32 frameCount) {
                auto* proxy = static_cast<DeviceProxy*>(pDevice->pUserData);
                auto numChannels = static_cast<size_t>(pDevice->playback.channels);
                auto numFrames = static_cast<size_t>(frameCount);

                proxy->process(static_cast<float*>(pOutput), numChannels, numFrames);
            }

            bool elemRuntimeHasSharedResource(const std::string& name)
            {
                for (auto& key : getRuntime().getSharedResourceMapKeys())
                {
                    if (key == name)
                    {
                        return true;
                    }
                }
                return false;
            }

            bool elemRuntimeAddSharedResource(
                const std::string& name, choc::buffer::ChannelArrayBuffer<float> buffers)
            {
                auto numCh     = buffers.getNumChannels();
                auto numFrames = buffers.getNumFrames();
                auto view = buffers.getView();
                std::vector<float*> ptrs(numCh);

                for (size_t ch = 0; ch < numCh; ++ch) {
                    ptrs[ch] = buffers.getChannel(ch).data.data;
                }
                auto & runtime = getRuntime();
                return runtime.addSharedResource(name, std::make_unique<elem::AudioBufferResource>(ptrs.data(), numCh, numFrames));
            }


            std::unique_ptr<DeviceProxy> proxy;
            ma_device_config deviceConfig;
            ma_device device;
            bool deviceInitialized;
            moodycamel::ConcurrentQueue<elem::js::Array, ConcurrentQueueTraits> eventQueue;
            PeriodicTimer eventTimer;
    };
}

#endif // AUDIOENGINE_H
