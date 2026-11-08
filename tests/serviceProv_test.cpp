#include "serviceProv.h"

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

class TestProvider final : public service::ServiceProvider<IService> {
public:
    using service::ServiceProvider<IService>::ServiceProvider;

    std::shared_ptr<IService> getNewInstance(std::type_info& requester) override {
        return std::make_shared<ServiceImpl>(requester == typeid(Caller) ? 10 : 1);
    }
};

int main() {
    TestProvider provider(10);
    service::Context namedContext("shared");
    Caller caller;
    const auto context = provider.addInstance(namedContext, caller);
    const auto reusedContext = provider.registerContext("shared");
    assert(context == reusedContext);
    assert(provider.findContext("shared") == context);
    assert(!provider.findContext("not-registered"));

    provider.addInstance<Caller>(context);
    provider.addInstance(context, std::static_pointer_cast<IService>(std::make_shared<ServiceImpl>(20)));
    auto derivedInstance = std::make_shared<ServiceImpl>(30);
    provider.addInstance(namedContext, derivedInstance);
    auto derivedInstanceWithHandle = std::make_shared<ServiceImpl>(40);
    provider.addInstance(context, derivedInstanceWithHandle);
    const auto values = provider.callMethodGet(context, &IService::value);
    assert((values == std::vector<int>{10, 10, 20, 30, 40}));

    provider.callMethod(context, &IService::add, 5);
    const auto updatedValues = provider.callMethodGet(namedContext, &IService::value);
    assert((updatedValues == std::vector<int>{15, 15, 25, 35, 45}));

    provider.erase("shared");
    bool missingContextReported = false;
    try {
        provider.callMethod(namedContext, &IService::add, 1);
    } catch (const std::out_of_range&) {
        missingContextReported = true;
    }
    assert(missingContextReported);

    bool nullContextReported = false;
    try {
        provider.callMethod(service::ContextPtr{}, &IService::add, 1);
    } catch (const std::invalid_argument&) {
        nullContextReported = true;
    }
    assert(nullContextReported);

    std::weak_ptr<service::Context> expiredContext;
    {
        auto temporaryContext = provider.registerContext("temporary");
        expiredContext = temporaryContext;
    }
    for (int attempt = 0; attempt < 100 && !expiredContext.expired(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(expiredContext.expired());

    service::Context retainedServiceContext("retained-service");
    auto retainedService = std::make_shared<ServiceImpl>(7);
    provider.addInstance(retainedServiceContext, retainedService);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert((provider.callMethodGet(retainedServiceContext, &IService::value) == std::vector<int>{7}));
    provider.erase(retainedServiceContext);

    service::Context reentrantContext("reentrant");
    ServiceImpl reentrantService(0, [&provider] { provider.erase("reentrant"); });
    provider.addInstance(reentrantContext, reentrantService);
    provider.callMethod(reentrantContext, &IService::add, 1);

    service::Context concurrentNamedContext("concurrent");
    const auto concurrentContext = provider.registerContext(concurrentNamedContext);
    provider.addInstance<Caller>(concurrentContext);
    std::atomic_bool dispatchFailed{false};
    std::thread adder([&] {
        for (int attempt = 0; attempt < 50; ++attempt) {
            provider.addInstance<Caller>(concurrentContext);
        }
    });
    std::thread dispatcher([&] {
        try {
            for (int attempt = 0; attempt < 50; ++attempt) {
                provider.callMethod(concurrentContext, &IService::add, 1);
            }
        } catch (...) {
            dispatchFailed.store(true);
        }
    });
    adder.join();
    dispatcher.join();
    assert(!dispatchFailed.load());
    assert(provider.callMethodGet(concurrentContext, &IService::value).size() == 51);

    service::Context firstSharedContext("shared-instance-a");
    service::Context secondSharedContext("shared-instance-b");
    std::shared_ptr<IService> sharedInstance = std::make_shared<ServiceImpl>(30);
    std::weak_ptr<IService> weakSharedInstance = sharedInstance;
    provider.addInstance(firstSharedContext, sharedInstance);
    provider.addInstance(secondSharedContext, sharedInstance);
    sharedInstance.reset();

    provider.erase(firstSharedContext);
    assert(!weakSharedInstance.expired());
    const auto secondContextHandle = provider.findContext("shared-instance-b");
    assert(secondContextHandle);
    assert((provider.callMethodGet(secondContextHandle, &IService::value) == std::vector<int>{30}));

    provider.clear();
    assert(!provider.findContext("shared-instance-b"));
    assert(weakSharedInstance.expired());

    service::Context externallyOwnedContext("external-owner");
    auto externallyOwnedInstance = std::make_shared<ServiceImpl>(40);
    std::weak_ptr<IService> weakExternalInstance = externallyOwnedInstance;
    provider.addInstance(
        externallyOwnedContext,
        std::static_pointer_cast<IService>(externallyOwnedInstance));
    provider.clear();
    assert(!weakExternalInstance.expired());
    externallyOwnedInstance.reset();
    assert(weakExternalInstance.expired());

    std::promise<void> callbackEntered;
    std::promise<void> releaseCallback;
    const auto releaseSignal = releaseCallback.get_future().share();
    service::Context inFlightContext("clear-in-flight");
    auto inFlightInstance = std::make_shared<ServiceImpl>(0, [&] {
        callbackEntered.set_value();
        releaseSignal.wait();
    });
    std::weak_ptr<IService> weakInFlightInstance = inFlightInstance;
    const auto inFlightHandle = provider.addInstance(
        inFlightContext,
        std::static_pointer_cast<IService>(inFlightInstance));
    inFlightInstance.reset();

    std::thread inFlightCall([&] {
        provider.callMethod(inFlightHandle, &IService::add, 1);
    });
    callbackEntered.get_future().wait();
    provider.clear();
    assert(!weakInFlightInstance.expired());
    releaseCallback.set_value();
    inFlightCall.join();
    assert(weakInFlightInstance.expired());

    TestProvider batchCollector(1);
    std::vector<service::ContextPtr> contextHandles;
    std::vector<std::weak_ptr<service::Context>> weakContexts;
    for (int index = 0; index < 300; ++index) {
        auto contextHandle = batchCollector.registerContext("batch-" + std::to_string(index));
        weakContexts.push_back(contextHandle);
        contextHandles.push_back(std::move(contextHandle));
    }
    contextHandles.clear();
    for (int attempt = 0; attempt < 1000; ++attempt) {
        const bool allExpired = std::all_of(
            weakContexts.begin(),
            weakContexts.end(),
            [](const auto& contextHandle) { return contextHandle.expired(); });
        if (allExpired) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(std::all_of(
        weakContexts.begin(),
        weakContexts.end(),
        [](const auto& contextHandle) { return contextHandle.expired(); }));

    // An exception stops the broadcast: earlier instances ran, later ones were skipped.
    service::Context throwingContext("throwing");
    const auto beforeThrow = std::make_shared<ServiceImpl>(0);
    const auto afterThrow = std::make_shared<ServiceImpl>(0);
    provider.addInstance(throwingContext, beforeThrow);
    provider.addInstance(throwingContext, std::make_shared<ThrowingService>());
    provider.addInstance(throwingContext, afterThrow);
    bool callbackThrew = false;
    try {
        provider.callMethod(throwingContext, &IService::add, 1);
    } catch (const std::runtime_error&) {
        callbackThrew = true;
    }
    assert(callbackThrew);
    assert(beforeThrow->value() == 1);
    assert(afterThrow->value() == 0);
    // The provider stays usable after the exception.
    assert(provider.callMethodGet(throwingContext, &IService::value).size() == 3);
    provider.erase(throwingContext);

    // The destructor wakes the collector instead of waiting out a long interval.
    const auto destroyStart = std::chrono::steady_clock::now();
    { TestProvider slowCollector(60000); }
    assert(std::chrono::steady_clock::now() - destroyStart < std::chrono::seconds(2));

    // Creating and destroying providers back to back must not race with collector start-up.
    for (int round = 0; round < 200; ++round) {
        TestProvider shortLived(1);
        shortLived.registerContext("round-" + std::to_string(round));
    }

    service::ServiceProvider<IService> atomicProvider;
    service::Context atomicContext("atomic-service");
    atomicProvider.addInstance(
        atomicContext,
        std::static_pointer_cast<IService>(std::make_shared<AtomicService>()));
    std::vector<std::thread> callers;
    for (int caller = 0; caller < 4; ++caller) {
        callers.emplace_back([&] {
            for (int call = 0; call < 1000; ++call) {
                atomicProvider.callMethod(atomicContext, &IService::add, 1);
            }
        });
    }
    for (std::thread& caller : callers) {
        caller.join();
    }
    assert((atomicProvider.callMethodGet(atomicContext, &IService::value) == std::vector<int>{4000}));
}
