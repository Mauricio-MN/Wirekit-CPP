// Each class ASKS for what it needs in its constructor (never `new`).
// wire() decides WHO answers each request; the container builds and hands it over.
// Swap one line in wire() and every class gets another implementation.
// Build: g++ -std=c++17 -pthread -Isrc/include src/examples/container/di_example.cpp
#define WIREKIT_ENABLE_DI
#include "wirekit.h"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

// ---- Contracts -------------------------------------------------------------
struct ILogger {
    virtual ~ILogger() = default;
    virtual void log(const std::string& message) const = 0;
};

struct IDatabase {
    virtual ~IDatabase() = default;
    virtual void save(const std::string& order) = 0;
    virtual std::size_t count() const = 0;
};

struct INotifier {
    virtual ~INotifier() = default;
    virtual void send(const std::string& message) const = 0;
};

// ---- Implementations: none of them knows how its dependencies are built ----
struct ConsoleLogger final : ILogger {
    void log(const std::string& message) const override { std::cout << "  [log] " << message << '\n'; }
};

struct SqlDatabase final : IDatabase {
    explicit SqlDatabase(std::shared_ptr<ILogger> logger) : logger(std::move(logger)) {
        this->logger->log("database connected");
    }
    void save(const std::string& order) override {
        ++saved;
        logger->log("saved " + order);
    }
    std::size_t count() const override { return saved; }

    std::shared_ptr<ILogger> logger;
    std::size_t saved = 0;
};

struct FakeDatabase final : IDatabase {
    void save(const std::string& order) override { orders.push_back(order); }
    std::size_t count() const override { return orders.size(); }
    std::vector<std::string> orders;
};

struct EmailNotifier final : INotifier {
    explicit EmailNotifier(std::shared_ptr<ILogger> logger) : logger(std::move(logger)) {}
    void send(const std::string& message) const override { logger->log("email: " + message); }
    std::shared_ptr<ILogger> logger;
};

struct SmsNotifier final : INotifier {
    explicit SmsNotifier(std::shared_ptr<ILogger> logger) : logger(std::move(logger)) {}
    void send(const std::string& message) const override { logger->log("sms: " + message); }
    std::shared_ptr<ILogger> logger;
};

// ---- Business logic: depends only on interfaces ----------------------------
using Notifiers = wirekit::Hub<INotifier>;

class OrderService {
public:
    OrderService(std::shared_ptr<IDatabase> database, std::shared_ptr<Notifiers> notifiers)
        : database(std::move(database)), notifiers(std::move(notifiers)) {}

    void place(const std::string& item) {
        database->save(item);
        // One call reaches every registered notifier (the multicast half of the library).
        notifiers->callMethod(channel, &INotifier::send, "order placed: " + item);
    }

private:
    std::shared_ptr<IDatabase> database;
    std::shared_ptr<Notifiers> notifiers;
    wirekit::Channel channel{"order-placed"};
};

// ---- Composition root: the only place that names concrete types ------------
void wire(wirekit::Container& container, bool testing) {
    container.bind<ILogger, ConsoleLogger>();

    if (testing) {
        container.bind<IDatabase, FakeDatabase>();
    } else {
        container.bind<IDatabase, SqlDatabase>();
    }

    // bind<I, Impl>()   = "build Impl by calling its constructor".
    // bindFactory<I>(f) = "build I by running MY code f" (here: create the hub, add email and SMS).
    // `c` is the container, so f can still ask it for dependencies.
    container.bindFactory<Notifiers>([](wirekit::Container& c) {
        auto hub = std::make_shared<Notifiers>();
        auto channel = hub->registerChannel("order-placed");
        hub->addInstance(channel, c.create<EmailNotifier>());
        hub->addInstance(channel, c.create<SmsNotifier>());
        return hub;
    });
}

int main() {
    std::cout << "Production:\n";
    {
        wirekit::Container container;
        wire(container, false);

        // Two services, one shared database: "connected" appears once.
        auto first = container.create<OrderService>();
        auto second = container.create<OrderService>();
        first->place("book");
        second->place("lamp");
    }

    std::cout << "Test (fake database, same OrderService):\n";
    {
        wirekit::Container container;
        wire(container, true);

        container.create<OrderService>()->place("book");
        auto database = std::static_pointer_cast<FakeDatabase>(container.resolve<IDatabase>());
        std::cout << "  fake saw " << database->count() << " order(s): " << database->orders[0] << '\n';
    }
}
