#define WIREKIT_ENABLE_DI
#include "wirekit.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

struct IClock {
    virtual ~IClock() = default;
    virtual int now() const = 0;
};

struct FixedClock final : IClock {
    int now() const override { return 42; }
};

struct ILogger {
    virtual ~ILogger() = default;
};

struct Logger final : ILogger {
    explicit Logger(std::shared_ptr<IClock> c) : clock(std::move(c)) {}
    std::shared_ptr<IClock> clock;
};

struct Service {
    Service(const std::shared_ptr<ILogger>& l, std::shared_ptr<IClock> c) : logger(l), clock(std::move(c)) {}
    std::shared_ptr<ILogger> logger;
    std::shared_ptr<IClock> clock;
};

struct CycleA;
struct CycleB;
struct CycleA { explicit CycleA(std::shared_ptr<CycleB>) {} };
struct CycleB { explicit CycleB(std::shared_ptr<CycleA>) {} };

std::atomic<int> slowConstructions{0};

struct SlowClock final : IClock {
    SlowClock() {
        ++slowConstructions;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    int now() const override { return 1; }
};

std::atomic<int> probesDestroyed{0};

struct Probe {
    explicit Probe(wirekit::Container& c) : container(c) {}
    // Resolving here would deadlock if destruction ran while the container's lock is held.
    ~Probe() {
        if (armed) {
            container.resolve<IClock>();
            ++probesDestroyed;
        }
    }
    wirekit::Container& container;
    bool armed = false;
};

int main() {
    wirekit::Container container;
    container.bind<IClock, FixedClock>();
    container.bind<ILogger, Logger>(wirekit::Lifetime::Transient);

    auto clockA = container.resolve<IClock>();
    assert(clockA == container.resolve<IClock>());
    assert(clockA->now() == 42);

    auto loggerA = container.resolve<ILogger>();
    auto loggerB = container.resolve<ILogger>();
    assert(loggerA != loggerB);
    assert(static_cast<Logger&>(*loggerA).clock == clockA);

    // Unregistered consumer, wired through its constructor.
    auto consumer = container.create<Service>();
    assert(consumer->clock == clockA);
    assert(consumer->logger != loggerA);

    // Explicit dependency list still works.
    wirekit::Container explicitContainer;
    explicitContainer.bind<IClock, FixedClock>();
    explicitContainer.bind<ILogger, Logger, IClock>();
    assert(static_cast<Logger&>(*explicitContainer.resolve<ILogger>()).clock == explicitContainer.resolve<IClock>());

    bool missing = false;
    try {
        container.resolve<CycleA>();
    } catch (const std::out_of_range&) {
        missing = true;
    }
    assert(missing);

    container.bind<CycleA, CycleA>(wirekit::Lifetime::Transient);
    container.bind<CycleB, CycleB>(wirekit::Lifetime::Transient);
    bool cycle = false;
    try {
        container.resolve<CycleA>();
    } catch (const std::logic_error&) {
        cycle = true;
    }
    assert(cycle);
    // The guard must unwind so a later valid resolve still works.
    assert(container.resolve<IClock>() == clockA);

    auto fixed = std::make_shared<FixedClock>();
    container.bindInstance<IClock>(fixed);
    assert(container.resolve<IClock>() == fixed);

    wirekit::Container concurrent;
    concurrent.bind<IClock, FixedClock>();
    std::vector<std::shared_ptr<IClock>> results(4);
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < results.size(); ++i) {
        threads.emplace_back([&, i] { results[i] = concurrent.resolve<IClock>(); });
    }
    for (auto& t : threads) t.join();
    for (const auto& r : results) assert(r == results[0]);

    // A singleton is built exactly once even when many threads ask at the same moment,
    // including one whose factory resolves another singleton (recursive creation).
    wirekit::Container racing;
    racing.bind<IClock, SlowClock>();
    racing.bind<ILogger, Logger>();
    std::vector<std::shared_ptr<ILogger>> loggers(8);
    std::vector<std::thread> racers;
    for (std::size_t i = 0; i < loggers.size(); ++i) {
        racers.emplace_back([&, i] { loggers[i] = racing.resolve<ILogger>(); });
    }
    for (auto& t : racers) t.join();
    assert(slowConstructions.load() == 1);
    for (const auto& l : loggers) assert(l == loggers[0]);

    // Replacing a binding destroys the old instance after the lock is released.
    wirekit::Container replacing;
    replacing.bind<IClock, FixedClock>();
    auto armedProbe = std::make_shared<Probe>(replacing);
    armedProbe->armed = true;
    replacing.bindInstance<Probe>(armedProbe);
    armedProbe.reset();
    replacing.bindInstance<Probe>(std::make_shared<Probe>(replacing));
    assert(probesDestroyed.load() == 1);

    // Error paths leave the container usable.
    wirekit::Container errors;
    bool emptyFactoryRejected = false;
    try {
        errors.bindFactory<IClock>(nullptr);
    } catch (const std::invalid_argument&) {
        emptyFactoryRejected = true;
    }
    assert(emptyFactoryRejected);

    bool nullInstanceRejected = false;
    try {
        errors.bindInstance<IClock>(nullptr);
    } catch (const std::invalid_argument&) {
        nullInstanceRejected = true;
    }
    assert(nullInstanceRejected);

    errors.bindFactory<IClock>(
        [](wirekit::Container&) { return std::shared_ptr<IClock>(); },
        wirekit::Lifetime::Transient);
    bool nullProductRejected = false;
    try {
        errors.resolve<IClock>();
    } catch (const std::logic_error&) {
        nullProductRejected = true;
    }
    assert(nullProductRejected);

    bool missingDependency = false;
    try {
        wirekit::Container empty;
        empty.create<Service>();  // neither dependency is bound
    } catch (const std::out_of_range&) {
        missingDependency = true;
    }
    assert(missingDependency);

    errors.bind<IClock, FixedClock>();
    assert(errors.resolve<IClock>()->now() == 42);
    errors.clear();
    bool clearedBinding = false;
    try {
        errors.resolve<IClock>();
    } catch (const std::out_of_range&) {
        clearedBinding = true;
    }
    assert(clearedBinding);

    // Rebinding replaces the old singleton.
    wirekit::Container rebinding;
    rebinding.bind<IClock, FixedClock>();
    const auto oldClock = rebinding.resolve<IClock>();
    rebinding.bind<IClock, SlowClock>();
    assert(rebinding.resolve<IClock>() != oldClock);
    assert(rebinding.resolve<IClock>()->now() == 1);

    // A singleton keeps its dependency; a transient dependency is captured once.
    wirekit::Container lifetimes;
    lifetimes.bind<IClock, FixedClock>(wirekit::Lifetime::Transient);
    lifetimes.bind<ILogger, Logger>();
    const auto singletonLogger = lifetimes.resolve<ILogger>();
    assert(singletonLogger == lifetimes.resolve<ILogger>());
    assert(static_cast<Logger&>(*singletonLogger).clock != lifetimes.resolve<IClock>());

    // Coexists with the hub under the same macro.
    wirekit::Hub<IClock> hub;
    hub.addInstance(*hub.registerChannel("clocks"), fixed);
    assert((hub.callMethodGet(hub.registerChannel("clocks"), &IClock::now) == std::vector<int>{42}));
}
