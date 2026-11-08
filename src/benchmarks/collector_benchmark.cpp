#include <wirekit/wirekit.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

int main() {
    constexpr std::size_t channelCount = 50000;
    constexpr std::size_t activeChannelCount = channelCount / 2;
    constexpr std::size_t workerCount = 4;
    constexpr std::size_t samplesPerWorker = 10000;

    wirekit::Hub<int> hub;
    std::vector<std::string> channelNames;
    std::vector<wirekit::ChannelPtr> channelHandles;
    channelNames.reserve(channelCount);
    channelHandles.reserve(channelCount);

    for (std::size_t index = 0; index < channelCount; ++index) {
        channelNames.push_back("channel-" + std::to_string(index));
        channelHandles.push_back(hub.registerChannel(channelNames.back()));
    }
    for (std::size_t index = 0; index < activeChannelCount; ++index) {
        channelHandles[index].reset();
    }

    std::vector<std::vector<long long>> samples(workerCount);
    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    std::atomic_size_t readyWorkers{0};
    std::atomic_bool startWorkers{false};

    for (std::size_t worker = 0; worker < workerCount; ++worker) {
        workers.emplace_back([&, worker] {
            auto& workerSamples = samples[worker];
            workerSamples.reserve(samplesPerWorker);
            readyWorkers.fetch_add(1);
            while (!startWorkers.load()) {
                std::this_thread::yield();
            }
            for (std::size_t sample = 0; sample < samplesPerWorker; ++sample) {
                const std::size_t index = activeChannelCount +
                    (sample * 37 + worker * 101) % activeChannelCount;
                const auto started = std::chrono::steady_clock::now();
                const auto channel = hub.findChannel(channelNames[index]);
                const auto finished = std::chrono::steady_clock::now();
                if (!channel) {
                    std::terminate();
                }
                workerSamples.push_back(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count());
            }
        });
    }

    while (readyWorkers.load() != workerCount) {
        std::this_thread::yield();
    }
    const auto collectionStarted = std::chrono::steady_clock::now();
    startWorkers.store(true);
    hub.collectGarbage();
    const auto collectionFinished = std::chrono::steady_clock::now();

    for (std::thread& worker : workers) {
        worker.join();
    }

    std::vector<long long> allSamples;
    allSamples.reserve(workerCount * samplesPerWorker);
    for (const auto& workerSamples : samples) {
        allSamples.insert(allSamples.end(), workerSamples.begin(), workerSamples.end());
    }
    std::sort(allSamples.begin(), allSamples.end());

    const auto percentile = [&allSamples](double fraction) {
        const std::size_t index = static_cast<std::size_t>(fraction * (allSamples.size() - 1));
        return allSamples[index];
    };

    const auto collectionNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        collectionFinished - collectionStarted).count();
    std::cout << "channels=" << channelCount
              << " threads=" << workerCount
              << " collected_channels=" << activeChannelCount
              << " collectGarbage_ns=" << collectionNs
              << " findChannel_ns_p50=" << percentile(0.50)
              << " p95=" << percentile(0.95)
              << " p99=" << percentile(0.99)
              << " max=" << allSamples.back() << '\n';
}
