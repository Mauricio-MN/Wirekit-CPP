#ifndef WIREKIT_HUB_H
#define WIREKIT_HUB_H

#include <algorithm>
#include <atomic>
#include <cstdint>
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
    Channel(const Channel& other) : name(other.name) {}
    Channel(Channel&& other) noexcept : name(std::move(other.name)) {}

    Channel& operator=(const Channel& other) {
        if (this != &other) {
            name = other.name;
        }
        return *this;
    }

    Channel& operator=(Channel&& other) noexcept {
        if (this != &other) {
            name = std::move(other.name);
        }
        return *this;
    }

    std::string getName() const {
        return name;
    }

};

class ChannelHandle final {
private:
    std::string name;
    std::shared_ptr<void> groupState;
    std::weak_ptr<const void> owner;
    template <class>
    friend class Hub;

    explicit ChannelHandle(std::string channelName) : name(std::move(channelName)) {}

    const std::string& getChannelRef() const noexcept {
        return name;
    }

public:
    ChannelHandle(const ChannelHandle&) = delete;
    ChannelHandle& operator=(const ChannelHandle&) = delete;

    std::string getName() const {
        return name;
    }
};

using ChannelPtr = std::shared_ptr<ChannelHandle>;

} // namespace wirekit

namespace wirekit {

template <class T>
class Hub {
public:
    struct Subscription {
        ChannelPtr channel;
        std::uint64_t id = 0;

        explicit operator bool() const noexcept {
            return channel && id != 0;
        }
    };

private:
    struct InstanceEntry {
        std::uint64_t id;
        std::shared_ptr<T> instance;
    };

    using InstanceList = std::vector<InstanceEntry>;

    struct GroupState {
        std::shared_ptr<const InstanceList> instances;
    };

    std::unordered_map<std::string, ChannelPtr> channelMap;
    mutable detail::SyncMutex channelsMutex;
    std::shared_ptr<const void> ownerToken = std::make_shared<int>(0);
    std::uint64_t nextSubscriptionId = 1;

    ChannelPtr registerChannelLocked(const std::string& channelId) {
        const auto it = channelMap.find(channelId);
        if (it != channelMap.end()) {
            return it->second;
        }

        ChannelPtr registeredChannel(new ChannelHandle(channelId));
        registeredChannel->groupState = std::make_shared<GroupState>();
        registeredChannel->owner = ownerToken;
        channelMap.emplace(channelId, registeredChannel);
        return registeredChannel;
    }

    void validateChannelHandle(const ChannelPtr& channel) const {
        if (!channel) {
            throw std::invalid_argument("Hub channel must not be null");
        }
        if (channel->owner.lock() != ownerToken) {
            throw std::invalid_argument("Hub channel handle belongs to another Hub");
        }
    }

    template <class InstanceFactory>
    ChannelPtr addInstanceImpl(
        const std::string& channelId,
        InstanceFactory&& factory,
        std::uint64_t* subscriptionId = nullptr,
        const ChannelHandle* expectedHandle = nullptr) {
        std::shared_ptr<T> instance = std::forward<InstanceFactory>(factory)();
        if (!instance) {
            throw std::logic_error("Hub factory returned a null instance");
        }

        std::lock_guard<detail::SyncMutex> lock(channelsMutex);
        ChannelPtr registeredChannel;
        if (expectedHandle) {
            const auto channelIt = channelMap.find(channelId);
            if (channelIt == channelMap.end() || channelIt->second.get() != expectedHandle) {
                throw std::invalid_argument("Hub channel handle is no longer registered");
            }
            registeredChannel = channelIt->second;
        } else {
            registeredChannel = registerChannelLocked(channelId);
        }
        auto state = std::static_pointer_cast<GroupState>(registeredChannel->groupState);
        if (nextSubscriptionId == 0) {
            throw std::overflow_error("Hub subscription identifiers exhausted");
        }
        const std::uint64_t id = nextSubscriptionId++;
        const auto currentInstances = std::atomic_load_explicit(
            &state->instances,
            std::memory_order_acquire);
        auto updatedInstances = currentInstances
            ? std::make_shared<InstanceList>(*currentInstances)
            : std::make_shared<InstanceList>();
        updatedInstances->push_back({id, std::move(instance)});
        std::atomic_store_explicit(
            &state->instances,
            std::shared_ptr<const InstanceList>(std::move(updatedInstances)),
            std::memory_order_release);
        if (subscriptionId) {
            *subscriptionId = id;
        }
        return registeredChannel;
    }

    template <class InstanceFactory>
    Subscription subscribeImpl(
        const std::string& channelId,
        InstanceFactory&& factory,
        const ChannelHandle* expectedHandle = nullptr) {
        std::uint64_t id = 0;
        ChannelPtr registeredChannel = addInstanceImpl(
            channelId,
            std::forward<InstanceFactory>(factory),
            &id,
            expectedHandle);
        return {std::move(registeredChannel), id};
    }

    std::shared_ptr<const InstanceList> snapshotInstances(Channel& channel) const {
        ChannelPtr registeredChannel;
        {
            std::lock_guard<detail::SyncMutex> lock(channelsMutex);
            const auto it = channelMap.find(channel.getChannelRef());
            if (it == channelMap.end()) {
                return {};
            }
            registeredChannel = it->second;
        }
        return snapshotInstances(*registeredChannel);
    }

    std::shared_ptr<const InstanceList> snapshotInstances(ChannelHandle& channel) const {
        if (channel.owner.lock() != ownerToken) {
            throw std::invalid_argument("Hub channel handle belongs to another Hub");
        }
        const auto state = std::static_pointer_cast<GroupState>(channel.groupState);
        return state
            ? std::atomic_load_explicit(&state->instances, std::memory_order_acquire)
            : std::shared_ptr<const InstanceList>{};
    }

    template <typename Method, typename... Args>
    void dispatch(
        const std::shared_ptr<const InstanceList>& instances,
        Method method,
        Args... args) const {
        if (!instances) {
            return;
        }
        for (const InstanceEntry& entry : *instances) {
            std::invoke(method, *entry.instance, args...);
        }
    }

    template <typename Method, typename... Args>
    auto dispatchGet(
        const std::shared_ptr<const InstanceList>& instances,
        Method method,
        Args... args) const
        -> std::vector<std::invoke_result_t<Method, T&, Args...>> {
        using Result = std::invoke_result_t<Method, T&, Args...>;
        static_assert(!std::is_void_v<Result>, "callMethodGet requires a non-void method");
        std::vector<Result> results;
        if (!instances) {
            return results;
        }
        results.reserve(instances->size());
        for (const InstanceEntry& entry : *instances) {
            results.push_back(std::invoke(method, *entry.instance, args...));
        }
        return results;
    }

    bool removeInstanceImpl(
        const std::string& channelId,
        const std::shared_ptr<T>& instance,
        const ChannelHandle* expectedHandle = nullptr) {
        if (!instance) {
            throw std::invalid_argument("Hub instance must not be null");
        }

        std::shared_ptr<const InstanceList> removedInstances;
        {
            std::lock_guard<detail::SyncMutex> lock(channelsMutex);
            const auto channelIt = channelMap.find(channelId);
            if (channelIt == channelMap.end() ||
                (expectedHandle && channelIt->second.get() != expectedHandle)) {
                return false;
            }

            const auto state = std::static_pointer_cast<GroupState>(channelIt->second->groupState);
            const auto currentInstances = std::atomic_load_explicit(
                &state->instances,
                std::memory_order_acquire);
            if (!currentInstances) {
                return false;
            }
            auto updatedInstances = std::make_shared<InstanceList>(*currentInstances);
            const auto instanceIt = std::find_if(
                updatedInstances->begin(), updatedInstances->end(),
                [&instance](const InstanceEntry& entry) { return entry.instance == instance; });
            if (instanceIt == updatedInstances->end()) {
                return false;
            }

            updatedInstances->erase(instanceIt);
            if (updatedInstances->empty()) {
                removedInstances = std::atomic_exchange_explicit(
                    &state->instances,
                    std::shared_ptr<const InstanceList>{},
                    std::memory_order_acq_rel);
            } else {
                std::atomic_store_explicit(
                    &state->instances,
                    std::shared_ptr<const InstanceList>(std::move(updatedInstances)),
                    std::memory_order_release);
            }
        }
        return true;
    }

    void eraseChannelById(const std::string& channelId) {
        std::shared_ptr<const InstanceList> removedInstances;
        ChannelPtr removedChannel;
        {
            std::lock_guard<detail::SyncMutex> lock(channelsMutex);
            const auto channelIt = channelMap.find(channelId);
            if (channelIt != channelMap.end()) {
                removedChannel = std::move(channelIt->second);
                auto state = std::static_pointer_cast<GroupState>(removedChannel->groupState);
                removedInstances = std::atomic_exchange_explicit(
                    &state->instances,
                    std::shared_ptr<const InstanceList>{},
                    std::memory_order_acq_rel);
                channelMap.erase(channelIt);
            }
        }
    }

    void eraseChannelHandle(const ChannelPtr& channel) {
        std::shared_ptr<const InstanceList> removedInstances;
        ChannelPtr removedChannel;
        {
            std::lock_guard<detail::SyncMutex> lock(channelsMutex);
            const auto channelIt = channelMap.find(channel->getChannelRef());
            if (channelIt == channelMap.end() || channelIt->second != channel) {
                return;
            }
            removedChannel = std::move(channelIt->second);
            const auto state = std::static_pointer_cast<GroupState>(removedChannel->groupState);
            removedInstances = std::atomic_exchange_explicit(
                &state->instances,
                std::shared_ptr<const InstanceList>{},
                std::memory_order_acq_rel);
            channelMap.erase(channelIt);
        }
    }

public:
    Hub() = default;
    explicit Hub(int) {}

    Hub(const Hub&) = delete;
    Hub& operator=(const Hub&) = delete;

    virtual ~Hub() {
        clear();
    }

    ChannelPtr registerChannel(std::string channelID) {
        Channel channel(std::move(channelID));
        return registerChannel(channel);
    }

    ChannelPtr registerChannel(Channel& channel) {
        std::lock_guard<detail::SyncMutex> lock(channelsMutex);
        return registerChannelLocked(channel.getChannelRef());
    }

    ChannelPtr registerChannel(ChannelPtr channel) {
        validateChannelHandle(channel);
        std::lock_guard<detail::SyncMutex> lock(channelsMutex);
        return registerChannelLocked(channel->getChannelRef());
    }

    ChannelPtr findChannel(const std::string& channelID) const {
        std::lock_guard<detail::SyncMutex> lock(channelsMutex);
        const auto it = channelMap.find(channelID);
        return it == channelMap.end() ? ChannelPtr{} : it->second;
    }

    template <class CallClass,
              std::enable_if_t<!detail::IsSharedPtr<std::decay_t<CallClass>>::value, int> = 0>
    ChannelPtr addInstance(Channel& channel, CallClass& callClass) {
        return addInstanceImpl(channel.getChannelRef(), [this, &callClass] {
            return createFor(callClass);
        });
    }

    template <class CallClass,
              std::enable_if_t<!detail::IsSharedPtr<std::decay_t<CallClass>>::value, int> = 0>
    ChannelPtr addInstance(ChannelPtr channel, CallClass& callClass) {
        validateChannelHandle(channel);
        return addInstanceImpl(channel->getChannelRef(), [this, &callClass] {
            return createFor(callClass);
        }, nullptr, channel.get());
    }

    template <typename AlternativeClass>
    ChannelPtr addInstance(Channel& channel) {
        return addInstanceImpl(channel.getChannelRef(), [this] {
            return createFor<AlternativeClass>();
        });
    }

    template <typename AlternativeClass>
    ChannelPtr addInstance(ChannelPtr channel) {
        validateChannelHandle(channel);
        return addInstanceImpl(channel->getChannelRef(), [this] {
            return createFor<AlternativeClass>();
        }, nullptr, channel.get());
    }

    ChannelPtr addInstance(Channel& channel, T& obj) {
        return addInstanceImpl(channel.getChannelRef(), [&obj] {
            return std::make_shared<T>(obj);
        });
    }

    ChannelPtr addInstance(Channel& channel, std::shared_ptr<T> obj) {
        if (!obj) {
            throw std::invalid_argument("Hub instance must not be null");
        }
        return addInstanceImpl(channel.getChannelRef(), [instance = std::move(obj)] { return instance; });
    }

    template <typename Derived, std::enable_if_t<std::is_convertible_v<Derived*, T*>, int> = 0>
    ChannelPtr addInstance(Channel& channel, const std::shared_ptr<Derived>& obj) {
        if (!obj) {
            throw std::invalid_argument("Hub instance must not be null");
        }
        std::shared_ptr<T> instance = obj;
        return addInstanceImpl(channel.getChannelRef(), [instance = std::move(instance)] { return instance; });
    }

    ChannelPtr addInstance(ChannelPtr channel, T& obj) {
        validateChannelHandle(channel);
        return addInstanceImpl(
            channel->getChannelRef(),
            [&obj] { return std::make_shared<T>(obj); },
            nullptr,
            channel.get());
    }

    ChannelPtr addInstance(ChannelPtr channel, std::shared_ptr<T> obj) {
        validateChannelHandle(channel);
        if (!obj) {
            throw std::invalid_argument("Hub instance must not be null");
        }
        return addInstanceImpl(
            channel->getChannelRef(),
            [instance = std::move(obj)] { return instance; },
            nullptr,
            channel.get());
    }

    template <typename Derived, std::enable_if_t<std::is_convertible_v<Derived*, T*>, int> = 0>
    ChannelPtr addInstance(ChannelPtr channel, const std::shared_ptr<Derived>& obj) {
        validateChannelHandle(channel);
        if (!obj) {
            throw std::invalid_argument("Hub instance must not be null");
        }
        std::shared_ptr<T> instance = obj;
        return addInstanceImpl(
            channel->getChannelRef(),
            [instance = std::move(instance)] { return instance; },
            nullptr,
            channel.get());
    }

    template <class CallClass,
              std::enable_if_t<!detail::IsSharedPtr<std::decay_t<CallClass>>::value, int> = 0>
    Subscription subscribe(Channel& channel, CallClass& callClass) {
        return subscribeImpl(channel.getChannelRef(), [this, &callClass] { return createFor(callClass); });
    }

    template <typename AlternativeClass>
    Subscription subscribe(Channel& channel) {
        return subscribeImpl(channel.getChannelRef(), [this] { return createFor<AlternativeClass>(); });
    }

    Subscription subscribe(Channel& channel, T& obj) {
        return subscribeImpl(channel.getChannelRef(), [&obj] { return std::make_shared<T>(obj); });
    }

    Subscription subscribe(Channel& channel, std::shared_ptr<T> obj) {
        if (!obj) {
            throw std::invalid_argument("Hub instance must not be null");
        }
        return subscribeImpl(channel.getChannelRef(), [instance = std::move(obj)] { return instance; });
    }

    template <typename Derived, std::enable_if_t<std::is_convertible_v<Derived*, T*>, int> = 0>
    Subscription subscribe(Channel& channel, const std::shared_ptr<Derived>& obj) {
        if (!obj) {
            throw std::invalid_argument("Hub instance must not be null");
        }
        std::shared_ptr<T> instance = obj;
        return subscribeImpl(channel.getChannelRef(), [instance = std::move(instance)] { return instance; });
    }

    template <class CallClass,
              std::enable_if_t<!detail::IsSharedPtr<std::decay_t<CallClass>>::value, int> = 0>
    Subscription subscribe(ChannelPtr channel, CallClass& callClass) {
        validateChannelHandle(channel);
        return subscribeImpl(
            channel->getChannelRef(),
            [this, &callClass] { return createFor(callClass); },
            channel.get());
    }

    template <typename AlternativeClass>
    Subscription subscribe(ChannelPtr channel) {
        validateChannelHandle(channel);
        return subscribeImpl(
            channel->getChannelRef(),
            [this] { return createFor<AlternativeClass>(); },
            channel.get());
    }

    Subscription subscribe(ChannelPtr channel, T& obj) {
        validateChannelHandle(channel);
        return subscribeImpl(
            channel->getChannelRef(),
            [&obj] { return std::make_shared<T>(obj); },
            channel.get());
    }

    Subscription subscribe(ChannelPtr channel, std::shared_ptr<T> obj) {
        validateChannelHandle(channel);
        if (!obj) {
            throw std::invalid_argument("Hub instance must not be null");
        }
        return subscribeImpl(
            channel->getChannelRef(),
            [instance = std::move(obj)] { return instance; },
            channel.get());
    }

    template <typename Derived, std::enable_if_t<std::is_convertible_v<Derived*, T*>, int> = 0>
    Subscription subscribe(ChannelPtr channel, const std::shared_ptr<Derived>& obj) {
        validateChannelHandle(channel);
        if (!obj) {
            throw std::invalid_argument("Hub instance must not be null");
        }
        std::shared_ptr<T> instance = obj;
        return subscribeImpl(
            channel->getChannelRef(),
            [instance = std::move(instance)] { return instance; },
            channel.get());
    }

    bool removeInstance(Channel& channel, const std::shared_ptr<T>& instance) {
        return removeInstanceImpl(channel.getChannelRef(), instance);
    }

    bool removeInstance(ChannelPtr channel, const std::shared_ptr<T>& instance) {
        validateChannelHandle(channel);
        return removeInstanceImpl(channel->getChannelRef(), instance, channel.get());
    }

    bool unsubscribe(const Subscription& subscription) {
        if (!subscription.channel) {
            throw std::invalid_argument("Hub subscription channel must not be null");
        }
        if (subscription.id == 0) {
            return false;
        }

        std::shared_ptr<const InstanceList> removedInstances;
        {
            std::lock_guard<detail::SyncMutex> lock(channelsMutex);
            if (subscription.channel->owner.lock() != ownerToken) {
                throw std::invalid_argument("Hub subscription belongs to another Hub");
            }
            if (!subscription.channel->groupState) {
                return false;
            }
            const auto state = std::static_pointer_cast<GroupState>(subscription.channel->groupState);
            const auto currentInstances = std::atomic_load_explicit(
                &state->instances,
                std::memory_order_acquire);
            if (!currentInstances) {
                return false;
            }

            auto updatedInstances = std::make_shared<InstanceList>(*currentInstances);
            const auto instanceIt = std::find_if(
                updatedInstances->begin(), updatedInstances->end(),
                [&subscription](const InstanceEntry& entry) { return entry.id == subscription.id; });
            if (instanceIt == updatedInstances->end()) {
                return false;
            }
            updatedInstances->erase(instanceIt);
            if (updatedInstances->empty()) {
                removedInstances = std::atomic_exchange_explicit(
                    &state->instances,
                    std::shared_ptr<const InstanceList>{},
                    std::memory_order_acq_rel);
            } else {
                std::atomic_store_explicit(
                    &state->instances,
                    std::shared_ptr<const InstanceList>(std::move(updatedInstances)),
                    std::memory_order_release);
            }
        }
        return true;
    }

    template <typename Method, typename... Args>
    void callMethod(Channel& channel, Method method, Args... args) const {
        dispatch(snapshotInstances(channel), method, args...);
    }

    template <typename Method, typename... Args>
    void callMethod(ChannelPtr channel, Method method, Args... args) const {
        validateChannelHandle(channel);
        dispatch(snapshotInstances(*channel), method, args...);
    }

    template <typename Method, typename... Args>
    std::vector<std::invoke_result_t<Method, T&, Args...>> callMethodGet(
        Channel& channel,
        Method method,
        Args... args) const {
        return dispatchGet(snapshotInstances(channel), method, args...);
    }

    template <typename Method, typename... Args>
    std::vector<std::invoke_result_t<Method, T&, Args...>> callMethodGet(
        ChannelPtr channel,
        Method method,
        Args... args) const {
        validateChannelHandle(channel);
        return dispatchGet(snapshotInstances(*channel), method, args...);
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

    void erase(ChannelPtr channel) {
        validateChannelHandle(channel);
        eraseChannelHandle(channel);
    }

    void clear() {
        std::vector<std::shared_ptr<const InstanceList>> removedInstances;
        decltype(channelMap) removedChannels;
        {
            std::lock_guard<detail::SyncMutex> lock(channelsMutex);
            removedInstances.reserve(channelMap.size());
            for (const auto& entry : channelMap) {
                auto state = std::static_pointer_cast<GroupState>(entry.second->groupState);
                auto instances = std::atomic_exchange_explicit(
                    &state->instances,
                    std::shared_ptr<const InstanceList>{},
                    std::memory_order_acq_rel);
                if (instances) {
                    removedInstances.push_back(std::move(instances));
                }
            }
            removedChannels.swap(channelMap);
        }
    }

    void erase(std::string channelId) {
        eraseChannelById(channelId);
    }

    void collectGarbage() {
        decltype(channelMap) removedChannels;
        {
            std::lock_guard<detail::SyncMutex> lock(channelsMutex);
            for (auto it = channelMap.begin(); it != channelMap.end();) {
                const auto state = std::static_pointer_cast<GroupState>(it->second->groupState);
                if (it->second.use_count() == 1 && !std::atomic_load_explicit(
                        &state->instances,
                        std::memory_order_acquire)) {
                    removedChannels.emplace(it->first, std::move(it->second));
                    it = channelMap.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
};

} // namespace wirekit

// Declares a hub subclass with the standard requester-based factory hook.
#define WIREKIT_DECLARE_FACTORY(ServiceName, InterfaceType) \
    class ServiceName final : public wirekit::Hub<InterfaceType> { \
    public: \
        std::shared_ptr<InterfaceType> createFor(std::type_info& requester) override; \
    }

#endif
