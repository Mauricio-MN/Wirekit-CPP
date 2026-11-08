#include <wirekit/wirekit.h>

#include <atomic>
#include <cassert>
#include <future>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <typeinfo>
#include <vector>

struct IService {
    virtual ~IService() = default;
    virtual void add(int amount) = 0;
    virtual int value() const = 0;
};

class ServiceImpl final : public IService {
private:
    int current;
    std::function<void()> onAdd;

public:
    explicit ServiceImpl(int initial, std::function<void()> callback = {})
        : current(initial), onAdd(std::move(callback)) {}

    void add(int amount) override {
        current += amount;
        if (onAdd) {
            auto callback = std::move(onAdd);
            callback();
        }
    }

    int value() const override {
        return current;
    }
};

class AtomicService final : public IService {
private:
    std::atomic<int> current{0};

public:
    void add(int amount) override {
        current.fetch_add(amount);
    }

    int value() const override {
        return current.load();
    }
};

class ThrowingService final : public IService {
public:
    void add(int) override { throw std::runtime_error("callback failed"); }
    int value() const override { return 0; }
};

struct Caller {};

class TestHub final : public wirekit::Hub<IService> {
public:
    using wirekit::Hub<IService>::Hub;

    std::shared_ptr<IService> createFor(std::type_info& requester) override {
        return std::make_shared<ServiceImpl>(requester == typeid(Caller) ? 10 : 1);
    }
};

int main() {
    TestHub hub(10);
    wirekit::Channel namedChannel("shared");
    Caller caller;
    const auto channel = hub.addInstance(namedChannel, caller);
    const auto reusedChannel = hub.registerChannel("shared");
    assert(channel == reusedChannel);
    assert(hub.findChannel("shared") == channel);
    assert(!hub.findChannel("not-registered"));

    wirekit::Hub<IService> otherHub;
    bool foreignHandleRejected = false;
    try {
        otherHub.callMethod(channel, &IService::add, 1);
    } catch (const std::invalid_argument&) {
        foreignHandleRejected = true;
    }
    assert(foreignHandleRejected);
    wirekit::Channel foreignSubscriptionChannel("foreign-subscription");
    const auto foreignSubscription = otherHub.subscribe(
        foreignSubscriptionChannel,
        std::static_pointer_cast<IService>(std::make_shared<ServiceImpl>(0)));
    bool foreignSubscriptionRejected = false;
    try {
        hub.unsubscribe(foreignSubscription);
    } catch (const std::invalid_argument&) {
        foreignSubscriptionRejected = true;
    }
    assert(foreignSubscriptionRejected);

    wirekit::Channel emptyChannel("empty");
    hub.callMethod(emptyChannel, &IService::add, 1);
    assert(hub.callMethodGet(emptyChannel, &IService::value).empty());

    hub.addInstance<Caller>(channel);
    hub.addInstance(channel, std::static_pointer_cast<IService>(std::make_shared<ServiceImpl>(20)));
    auto derivedInstance = std::make_shared<ServiceImpl>(30);
    hub.addInstance(namedChannel, derivedInstance);
    auto derivedInstanceWithHandle = std::make_shared<ServiceImpl>(40);
    hub.addInstance(channel, derivedInstanceWithHandle);
    const auto values = hub.callMethodGet(channel, &IService::value);
    assert((values == std::vector<int>{10, 10, 20, 30, 40}));

    hub.callMethod(channel, &IService::add, 5);
    const auto updatedValues = hub.callMethodGet(namedChannel, &IService::value);
    assert((updatedValues == std::vector<int>{15, 15, 25, 35, 45}));

    wirekit::Channel unsubscribeChannel("unsubscribe");
    const auto removedService = std::make_shared<ServiceImpl>(1);
    const auto retainedForUnsubscribe = std::make_shared<ServiceImpl>(2);
    hub.addInstance(unsubscribeChannel, removedService);
    hub.addInstance(unsubscribeChannel, retainedForUnsubscribe);
    assert(hub.removeInstance(unsubscribeChannel, removedService));
    assert(!hub.removeInstance(unsubscribeChannel, removedService));
    assert((hub.callMethodGet(unsubscribeChannel, &IService::value) == std::vector<int>{2}));
    assert(hub.removeInstance(unsubscribeChannel, retainedForUnsubscribe));
    assert(hub.callMethodGet(unsubscribeChannel, &IService::value).empty());

    const auto factorySubscription = hub.subscribe<Caller>(unsubscribeChannel);
    assert(factorySubscription);
    assert((hub.callMethodGet(unsubscribeChannel, &IService::value) == std::vector<int>{10}));
    assert(hub.unsubscribe(factorySubscription));
    assert(!hub.unsubscribe(factorySubscription));
    assert(hub.callMethodGet(unsubscribeChannel, &IService::value).empty());

    hub.erase("shared");
    hub.callMethod(namedChannel, &IService::add, 1);
    hub.callMethod(channel, &IService::add, 1);
    assert(hub.callMethodGet(channel, &IService::value).empty());
    const auto replacementHandle = hub.registerChannel("shared");
    assert(replacementHandle != channel);
    const auto replacementService = std::make_shared<ServiceImpl>(3);
    hub.addInstance(replacementHandle, replacementService);
    hub.callMethod(channel, &IService::add, 1);
    assert(replacementService->value() == 3);
    hub.erase(channel);
    assert(hub.findChannel("shared") == replacementHandle);
    assert(!hub.removeInstance(channel, replacementService));
    bool staleHandleRejected = false;
    try {
        hub.addInstance(channel, std::make_shared<ServiceImpl>(4));
    } catch (const std::invalid_argument&) {
        staleHandleRejected = true;
    }
    assert(staleHandleRejected);
    assert((hub.callMethodGet(replacementHandle, &IService::value) == std::vector<int>{3}));
    hub.erase(replacementHandle);

    bool nullChannelReported = false;
    try {
        hub.callMethod(wirekit::ChannelPtr{}, &IService::add, 1);
    } catch (const std::invalid_argument&) {
        nullChannelReported = true;
    }
    assert(nullChannelReported);

    std::weak_ptr<wirekit::ChannelHandle> expiredChannel;
    {
        auto temporaryChannel = hub.registerChannel("temporary");
        expiredChannel = temporaryChannel;
    }
    assert(!expiredChannel.expired());
    hub.collectGarbage();
    assert(expiredChannel.expired());

    wirekit::Channel retainedServiceChannel("retained-service");
    auto retainedService = std::make_shared<ServiceImpl>(7);
    hub.addInstance(retainedServiceChannel, retainedService);
    hub.collectGarbage();
    assert((hub.callMethodGet(retainedServiceChannel, &IService::value) == std::vector<int>{7}));
    hub.erase(retainedServiceChannel);

    wirekit::Channel reentrantChannel("reentrant");
    ServiceImpl reentrantService(0, [&hub] { hub.erase("reentrant"); });
    hub.addInstance(reentrantChannel, reentrantService);
    hub.callMethod(reentrantChannel, &IService::add, 1);

    wirekit::Channel concurrentNamedChannel("concurrent");
    const auto concurrentChannel = hub.registerChannel(concurrentNamedChannel);
    hub.addInstance<Caller>(concurrentChannel);
    std::atomic_bool dispatchFailed{false};
    std::thread adder([&] {
        for (int attempt = 0; attempt < 50; ++attempt) {
            hub.addInstance<Caller>(concurrentChannel);
        }
    });
    std::thread dispatcher([&] {
        try {
            for (int attempt = 0; attempt < 50; ++attempt) {
                hub.callMethod(concurrentChannel, &IService::add, 1);
            }
        } catch (...) {
            dispatchFailed.store(true);
        }
    });
    adder.join();
    dispatcher.join();
    assert(!dispatchFailed.load());
    assert(hub.callMethodGet(concurrentChannel, &IService::value).size() == 51);

    wirekit::Channel firstSharedChannel("shared-instance-a");
    wirekit::Channel secondSharedChannel("shared-instance-b");
    std::shared_ptr<IService> sharedInstance = std::make_shared<ServiceImpl>(30);
    std::weak_ptr<IService> weakSharedInstance = sharedInstance;
    hub.addInstance(firstSharedChannel, sharedInstance);
    hub.addInstance(secondSharedChannel, sharedInstance);
    sharedInstance.reset();

    hub.erase(firstSharedChannel);
    assert(!weakSharedInstance.expired());
    const auto secondChannelHandle = hub.findChannel("shared-instance-b");
    assert(secondChannelHandle);
    assert((hub.callMethodGet(secondChannelHandle, &IService::value) == std::vector<int>{30}));

    hub.clear();
    assert(!hub.findChannel("shared-instance-b"));
    assert(weakSharedInstance.expired());

    wirekit::Channel externallyOwnedChannel("external-owner");
    auto externallyOwnedInstance = std::make_shared<ServiceImpl>(40);
    std::weak_ptr<IService> weakExternalInstance = externallyOwnedInstance;
    hub.addInstance(
        externallyOwnedChannel,
        std::static_pointer_cast<IService>(externallyOwnedInstance));
    hub.clear();
    assert(!weakExternalInstance.expired());
    externallyOwnedInstance.reset();
    assert(weakExternalInstance.expired());

    std::promise<void> callbackEntered;
    std::promise<void> releaseCallback;
    const auto releaseSignal = releaseCallback.get_future().share();
    wirekit::Channel inFlightChannel("clear-in-flight");
    auto inFlightInstance = std::make_shared<ServiceImpl>(0, [&] {
        callbackEntered.set_value();
        releaseSignal.wait();
    });
    std::weak_ptr<IService> weakInFlightInstance = inFlightInstance;
    const auto inFlightSubscription = hub.subscribe(
        inFlightChannel,
        std::static_pointer_cast<IService>(inFlightInstance));
    inFlightInstance.reset();

    std::thread inFlightCall([&] {
        hub.callMethod(inFlightSubscription.channel, &IService::add, 1);
    });
    callbackEntered.get_future().wait();
    assert(hub.unsubscribe(inFlightSubscription));
    assert(!weakInFlightInstance.expired());
    releaseCallback.set_value();
    inFlightCall.join();
    assert(weakInFlightInstance.expired());

    TestHub batchCollector;
    std::vector<wirekit::ChannelPtr> channelHandles;
    std::vector<std::weak_ptr<wirekit::ChannelHandle>> weakChannels;
    for (int index = 0; index < 300; ++index) {
        auto channelHandle = batchCollector.registerChannel("batch-" + std::to_string(index));
        weakChannels.push_back(channelHandle);
        channelHandles.push_back(std::move(channelHandle));
    }
    channelHandles.clear();
    batchCollector.collectGarbage();
    assert(std::all_of(
        weakChannels.begin(),
        weakChannels.end(),
        [](const auto& channelHandle) { return channelHandle.expired(); }));

    // An exception stops the broadcast: earlier instances ran, later ones were skipped.
    wirekit::Channel throwingChannel("throwing");
    const auto beforeThrow = std::make_shared<ServiceImpl>(0);
    const auto afterThrow = std::make_shared<ServiceImpl>(0);
    hub.addInstance(throwingChannel, beforeThrow);
    hub.addInstance(throwingChannel, std::make_shared<ThrowingService>());
    hub.addInstance(throwingChannel, afterThrow);
    bool callbackThrew = false;
    try {
        hub.callMethod(throwingChannel, &IService::add, 1);
    } catch (const std::runtime_error&) {
        callbackThrew = true;
    }
    assert(callbackThrew);
    assert(beforeThrow->value() == 1);
    assert(afterThrow->value() == 0);
    // The hub stays usable after the exception.
    assert(hub.callMethodGet(throwingChannel, &IService::value).size() == 3);
    hub.erase(throwingChannel);

    wirekit::Hub<IService> atomicHub;
    wirekit::Channel atomicChannel("atomic-service");
    atomicHub.addInstance(
        atomicChannel,
        std::static_pointer_cast<IService>(std::make_shared<AtomicService>()));
    std::vector<std::thread> callers;
    for (int caller = 0; caller < 4; ++caller) {
        callers.emplace_back([&] {
            for (int call = 0; call < 1000; ++call) {
                atomicHub.callMethod(atomicChannel, &IService::add, 1);
            }
        });
    }
    for (std::thread& caller : callers) {
        caller.join();
    }
    assert((atomicHub.callMethodGet(atomicChannel, &IService::value) == std::vector<int>{4000}));
}
