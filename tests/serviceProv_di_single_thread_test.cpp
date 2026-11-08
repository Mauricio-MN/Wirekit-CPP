#define SERVICE_PROVIDER_SINGLE_THREAD
#define SERVICE_PROVIDER_ENABLE_DI
#include "../serviceProv.h"

#include <cassert>
#include <memory>
#include <stdexcept>

struct IClock {
    virtual ~IClock() = default;
    virtual int now() const = 0;
};

struct FixedClock final : IClock {
    int now() const override { return 42; }
};

struct Logger {
    explicit Logger(std::shared_ptr<IClock> c) : clock(std::move(c)) {}
    std::shared_ptr<IClock> clock;
};

struct CycleA;
struct CycleB;
struct CycleA { explicit CycleA(std::shared_ptr<CycleB>) {} };
struct CycleB { explicit CycleB(std::shared_ptr<CycleA>) {} };

int main() {
    service::Container container;
    container.bind<IClock, FixedClock>();
    container.bind<Logger, Logger>(service::Lifetime::Transient);

    const auto clock = container.resolve<IClock>();
    assert(clock == container.resolve<IClock>());
    assert(clock->now() == 42);

    const auto first = container.resolve<Logger>();
    assert(first != container.resolve<Logger>());
    assert(first->clock == clock);
    assert(container.make<Logger>()->clock == clock);

    container.bind<CycleA, CycleA>(service::Lifetime::Transient);
    container.bind<CycleB, CycleB>(service::Lifetime::Transient);
    bool cycle = false;
    try {
        container.resolve<CycleA>();
    } catch (const std::logic_error&) {
        cycle = true;
    }
    assert(cycle);
    assert(container.resolve<IClock>() == clock);

    // Container and provider share the single-thread configuration.
    service::ServiceProvider<IClock> provider;
    service::Context context("clocks");
    provider.addInstance(context, clock);
    assert(provider.callMethodGet(context, &IClock::now).size() == 1);
    container.clear();
    provider.clear();
}
