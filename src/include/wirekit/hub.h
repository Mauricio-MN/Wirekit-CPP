#ifndef WIREKIT_HUB_H
#define WIREKIT_HUB_H

#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>
#include <unordered_map>

#ifndef WIREKIT_SINGLE_THREAD
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>
#endif

#include "detail.h"

namespace wirekit {

class Channel {
private:
    std::string name;
    template <class>
    friend class Hub;

    const std::string& getChannelRef() const noexcept {
        return name;
    }

public:
    explicit Channel(std::string channelName) : name(std::move(channelName)) {}
    Channel() : name("Default") {}

    std::string getName() const {
        return name;
    }

};

using ChannelPtr = std::shared_ptr<Channel>;

} // namespace wirekit

namespace wirekit {

template <class T>
class Hub {
private:
    using InstanceList = std::vector<std::shared_ptr<T>>;
    std::unordered_map<std::string, std::shared_ptr<const InstanceList>> objs;
    std::unordered_map<std::string, ChannelPtr> channelMap;
    mutable detail::SyncMutex channelsMutex;
#ifndef WIREKIT_SINGLE_THREAD
    std::atomic_bool collect{true};
    std::condition_variable garbageCollectorWake;
    int garbageWait;
    std::string garbageCursor;
    bool hasGarbageCursor = false;
    std::thread garbageCollectorThread;

    static constexpr std::size_t garbageCollectionBatchSize = 4;
#endif

    ChannelPtr registerChannelLocked(const ChannelPtr& channel) {
        const std::string& channelId = channel->getChannelRef();
        const auto it = channelMap.find(channelId);
        if (it != channelMap.end()) {
            return it->second;
        }

        channelMap.emplace(channelId, channel);
        return channel;
    }

    template <class InstanceFactory>
    ChannelPtr addInstanceImpl(Channel& channel, InstanceFactory&& factory) {
        std::shared_ptr<T> instance = std::forward<InstanceFactory>(factory)();
        if (!instance) {
            throw std::logic_error("Hub factory returned a null instance");
        }

        std::lock_guard<detail::SyncMutex> lock(channelsMutex);
        ChannelPtr registeredChannel = registerChannelLocked(std::make_shared<Channel>(channel));
        auto& currentInstances = objs[registeredChannel->getChannelRef()];
        auto updatedInstances = currentInstances
            ? std::make_shared<InstanceList>(*currentInstances)
            : std::make_shared<InstanceList>();
        updatedInstances->push_back(std::move(instance));
        currentInstances = std::move(updatedInstances);
        return registeredChannel;
    }

    std::shared_ptr<const InstanceList> snapshotInstances(Channel& channel) const {
        std::shared_ptr<const InstanceList> snapshot;
        {
            std::lock_guard<detail::SyncMutex> lock(channelsMutex);
            const auto it = objs.find(channel.getChannelRef());
            if (it == objs.end() || !it->second || it->second->empty()) {
                throw std::out_of_range("No service instances for channel: " + channel.getName());
            }
            snapshot = it->second;
        }
        return snapshot;
    }

    void eraseChannelById(const std::string& channelId) {
        std::shared_ptr<const InstanceList> removedInstances;
        ChannelPtr removedChannel;
        {
            std::lock_guard<detail::SyncMutex> lock(channelsMutex);
            const auto instancesIt = objs.find(channelId);
            if (instancesIt != objs.end()) {
                removedInstances = std::move(instancesIt->second);
                objs.erase(instancesIt);
            }
            const auto channelIt = channelMap.find(channelId);
            if (channelIt != channelMap.end()) {
                removedChannel = std::move(channelIt->second);
                channelMap.erase(channelIt);
            }
        }
    }

#ifndef WIREKIT_SINGLE_THREAD
    void checkChannelsThread() {
        while (collect.load()) {
            {
                std::unique_lock<detail::SyncMutex> lock(channelsMutex);
                garbageCollectorWake.wait_for(
                    lock,
                    std::chrono::milliseconds(garbageWait),
                    [this] { return !collect.load(); });
            }
            if (!collect.load()) {
                break;
            }

            std::size_t remaining = 0;
            {
                std::lock_guard<detail::SyncMutex> lock(channelsMutex);
                remaining = channelMap.size();
            }

            while (remaining > 0 && collect.load()) {
                std::vector<ChannelPtr> removedChannels;
                std::size_t processed = 0;
                {
                    std::lock_guard<detail::SyncMutex> lock(channelsMutex);
                    if (channelMap.empty()) {
                        hasGarbageCursor = false;
                        garbageCursor.clear();
                        break;
                    }

                    auto it = channelMap.begin();
                    if (hasGarbageCursor) {
                        const auto cursorIt = channelMap.find(garbageCursor);
                        if (cursorIt != channelMap.end()) {
                            it = cursorIt;
                        } else {
                            hasGarbageCursor = false;
                        }
                    }

                    const std::size_t batchLimit = std::min(
                        garbageCollectionBatchSize,
                        std::min(remaining, channelMap.size()));
                    while (processed < batchLimit && !channelMap.empty()) {
                        const auto current = it;
                        ++it;
                        if (it == channelMap.end()) {
                            it = channelMap.begin();
                        }

                        if (current->second.use_count() == 1 && objs.find(current->first) == objs.end()) {
                            removedChannels.push_back(std::move(current->second));
                            it = channelMap.erase(current);
                            if (it == channelMap.end() && !channelMap.empty()) {
                                it = channelMap.begin();
                            }
                        }

                        ++processed;
                        if (!channelMap.empty()) {
                            garbageCursor = it->first;
                            hasGarbageCursor = true;
                        } else {
                            garbageCursor.clear();
                            hasGarbageCursor = false;
                        }
                    }
                }

                if (processed >= remaining) {
                    remaining = 0;
                } else {
                    remaining -= processed;
                }
                if (remaining > 0) {
                    removedChannels.clear();
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                }
            }
        }
    }
#endif

public:
    Hub() : Hub(1000) {}

#ifdef WIREKIT_SINGLE_THREAD
    explicit Hub(int) {}
#else
    explicit Hub(int garbageWaitMilliseconds)
                : garbageWait(std::max(garbageWaitMilliseconds, 1)) {
                garbageCollectorThread = std::thread(&Hub::checkChannelsThread, this);
        }
#endif

    Hub(const Hub&) = delete;
    Hub& operator=(const Hub&) = delete;

    virtual ~Hub() {
#ifndef WIREKIT_SINGLE_THREAD
    {
        std::lock_guard<detail::SyncMutex> lock(channelsMutex);
        collect.store(false);
    }
        garbageCollectorWake.notify_all();
        if (garbageCollectorThread.joinable()) {
            garbageCollectorThread.join();
        }
#endif
        clear();
    }

    ChannelPtr registerChannel(std::string channelID) {
        Channel channel(std::move(channelID));
        return registerChannel(channel);
    }

    ChannelPtr registerChannel(Channel& channel) {
        return registerChannel(std::make_shared<Channel>(channel));
    }

    ChannelPtr registerChannel(ChannelPtr channel) {
        if (!channel) {
            throw std::invalid_argument("Hub channel must not be null");
        }

        std::lock_guard<detail::SyncMutex> lock(channelsMutex);
        return registerChannelLocked(channel);
    }

    ChannelPtr findChannel(const std::string& channelID) const {
        std::lock_guard<detail::SyncMutex> lock(channelsMutex);
        const auto it = channelMap.find(channelID);
        return it == channelMap.end() ? ChannelPtr{} : it->second;
    }

    template <class CallClass,
              std::enable_if_t<!detail::IsSharedPtr<std::decay_t<CallClass>>::value, int> = 0>
    ChannelPtr addInstance(Channel& channel, CallClass& callClass) {
        return addInstanceImpl(channel, [this, &callClass] {
            return createFor(callClass);
        });
    }

    template <class CallClass,
              std::enable_if_t<!detail::IsSharedPtr<std::decay_t<CallClass>>::value, int> = 0>
    ChannelPtr addInstance(ChannelPtr channel, CallClass& callClass) {
        if (!channel) {
            throw std::invalid_argument("Hub channel must not be null");
        }
        return addInstance(*channel, callClass);
    }

    template <typename AlternativeClass>
    ChannelPtr addInstance(Channel& channel) {
        return addInstanceImpl(channel, [this] {
            return createFor<AlternativeClass>();
        });
    }

    template <typename AlternativeClass>
    ChannelPtr addInstance(ChannelPtr channel) {
        if (!channel) {
            throw std::invalid_argument("Hub channel must not be null");
        }
        return addInstance<AlternativeClass>(*channel);
    }

    ChannelPtr addInstance(Channel& channel, T& obj) {
        return addInstanceImpl(channel, [&obj] {
            return std::make_shared<T>(obj);
        });
    }

    ChannelPtr addInstance(Channel& channel, std::shared_ptr<T> obj) {
        if (!obj) {
            throw std::invalid_argument("Hub instance must not be null");
        }
        return addInstanceImpl(channel, [instance = std::move(obj)] { return instance; });
    }

    template <typename Derived, std::enable_if_t<std::is_convertible_v<Derived*, T*>, int> = 0>
    ChannelPtr addInstance(Channel& channel, const std::shared_ptr<Derived>& obj) {
        if (!obj) {
            throw std::invalid_argument("Hub instance must not be null");
        }
        std::shared_ptr<T> instance = obj;
        return addInstanceImpl(channel, [instance = std::move(instance)] { return instance; });
    }

    ChannelPtr addInstance(ChannelPtr channel, T& obj) {
        if (!channel) {
            throw std::invalid_argument("Hub channel must not be null");
        }
        return addInstance(*channel, obj);
    }

    ChannelPtr addInstance(ChannelPtr channel, std::shared_ptr<T> obj) {
        if (!channel) {
            throw std::invalid_argument("Hub channel must not be null");
        }
        return addInstance(*channel, std::move(obj));
    }

    template <typename Derived, std::enable_if_t<std::is_convertible_v<Derived*, T*>, int> = 0>
    ChannelPtr addInstance(ChannelPtr channel, const std::shared_ptr<Derived>& obj) {
        if (!channel) {
            throw std::invalid_argument("Hub channel must not be null");
        }
        return addInstance(*channel, obj);
    }

    template <typename Method, typename... Args>
    void callMethod(Channel& channel, Method method, Args... args) const {
        const auto instances = snapshotInstances(channel);
        for (const std::shared_ptr<T>& instance : *instances) {
            std::invoke(method, *instance, args...);
        }
    }

    template <typename Method, typename... Args>
    void callMethod(ChannelPtr channel, Method method, Args... args) const {
        if (!channel) {
            throw std::invalid_argument("Hub channel must not be null");
        }
        callMethod(*channel, method, args...);
    }

    template <typename Method, typename... Args>
    std::vector<std::invoke_result_t<Method, T&, Args...>> callMethodGet(
        Channel& channel,
        Method method,
        Args... args) const {
        const auto instances = snapshotInstances(channel);

        using Result = std::invoke_result_t<Method, T&, Args...>;
        static_assert(!std::is_void_v<Result>, "callMethodGet requires a non-void method");
        std::vector<Result> results;
        results.reserve(instances->size());
        for (const std::shared_ptr<T>& instance : *instances) {
            results.push_back(std::invoke(method, *instance, args...));
        }
        return results;
    }

    template <typename Method, typename... Args>
    std::vector<std::invoke_result_t<Method, T&, Args...>> callMethodGet(
        ChannelPtr channel,
        Method method,
        Args... args) const {
        if (!channel) {
            throw std::invalid_argument("Hub channel must not be null");
        }
        return callMethodGet(*channel, method, args...);
    }

    template <class CallClass>
    std::shared_ptr<T> createFor(CallClass& callClass) {
        return createFor(typeid(callClass));
    }

    template <typename CallClassType>
    std::shared_ptr<T> createFor() {
        return createFor(typeid(CallClassType));
    }

    virtual std::shared_ptr<T> createFor(const std::type_info& type) {
        return createFor(const_cast<std::type_info&>(type));
    }

    virtual std::shared_ptr<T> createFor(std::type_info& type) {
        (void)type;
        if constexpr (!std::is_abstract_v<T> && std::is_default_constructible_v<T>) {
            return std::make_shared<T>();
        } else {
            throw std::logic_error("Hub requires an overriding factory for this service type");
        }
    }

    void erase(Channel& channel) {
        eraseChannelById(channel.getChannelRef());
    }

    void clear() {
        decltype(objs) removedInstances;
        decltype(channelMap) removedChannels;
        {
            std::lock_guard<detail::SyncMutex> lock(channelsMutex);
            removedInstances.swap(objs);
            removedChannels.swap(channelMap);
        }
    }

    void erase(std::string channelId) {
        eraseChannelById(channelId);
    }

#ifdef WIREKIT_SINGLE_THREAD
    void collectGarbage() {
        decltype(channelMap) removedChannels;
        {
            std::lock_guard<detail::SyncMutex> lock(channelsMutex);
            for (auto it = channelMap.begin(); it != channelMap.end();) {
                if (it->second.use_count() == 1 && objs.find(it->first) == objs.end()) {
                    removedChannels.emplace(it->first, std::move(it->second));
                    it = channelMap.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
#endif
};

} // namespace wirekit

// Declares a hub subclass with the standard requester-based factory hook.
#define WIREKIT_DECLARE_FACTORY(ServiceName, InterfaceType) \
    class ServiceName final : public wirekit::Hub<InterfaceType> { \
    public: \
        std::shared_ptr<InterfaceType> createFor(std::type_info& requester) override; \
    }

#endif
