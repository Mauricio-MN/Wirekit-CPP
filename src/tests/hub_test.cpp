#include <wirekit/wirekit.h>

#include <atomic>
#include <cassert>
#include <chrono>
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

    hub.erase("shared");
    bool missingChannelReported = false;
    try {
        hub.callMethod(namedChannel, &IService::add, 1);
    } catch (const std::out_of_range&) {
        missingChannelReported = true;
    }
    assert(missingChannelReported);

    bool nullChannelReported = false;
    try {
        hub.callMethod(wirekit::ChannelPtr{}, &IService::add, 1);
    } catch (const std::invalid_argument&) {
        nullChannelReported = true;
    }
    assert(nullChannelReported);

    std::weak_ptr<wirekit::Channel> expiredChannel;
    {
        auto temporaryChannel = hub.registerChannel("temporary");
        expiredChannel = temporaryChannel;
    }
    for (int attempt = 0; attempt < 100 && !expiredChannel.expired(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(expiredChannel.expired());

    wirekit::Channel retainedServiceChannel("retained-service");
    auto retainedService = std::make_shared<ServiceImpl>(7);
    hub.addInstance(retainedServiceChannel, retainedService);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
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
    const auto inFlightHandle = hub.addInstance(
        inFlightChannel,
        std::static_pointer_cast<IService>(inFlightInstance));
    inFlightInstance.reset();

    std::thread inFlightCall([&] {
        hub.callMethod(inFlightHandle, &IService::add, 1);
    });
    callbackEntered.get_future().wait();
    hub.clear();
    assert(!weakInFlightInstance.expired());
    releaseCallback.set_value();
    inFlightCall.join();
    assert(weakInFlightInstance.expired());

    TestHub batchCollector(1);
    std::vector<wirekit::ChannelPtr> channelHandles;
    std::vector<std::weak_ptr<wirekit::Channel>> weakChannels;
    for (int index = 0; index < 300; ++index) {
        auto channelHandle = batchCollector.registerChannel("batch-" + std::to_string(index));
        weakChannels.push_back(channelHandle);
        channelHandles.push_back(std::move(channelHandle));
    }
    channelHandles.clear();
    for (int attempt = 0; attempt < 1000; ++attempt) {
        const bool allExpired = std::all_of(
            weakChannels.begin(),
            weakChannels.end(),
            [](const auto& channelHandle) { return channelHandle.expired(); });
        if (allExpired) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
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

    // The destructor wakes the collector instead of waiting out a long interval.
    const auto destroyStart = std::chrono::steady_clock::now();
    { TestHub slowCollector(60000); }
    assert(std::chrono::steady_clock::now() - destroyStart < std::chrono::seconds(2));

    // Creating and destroying hubs back to back must not race with collector start-up.
    for (int round = 0; round < 200; ++round) {
        TestHub shortLived(1);
        shortLived.registerChannel("round-" + std::to_string(round));
    }

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
