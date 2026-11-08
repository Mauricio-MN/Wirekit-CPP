#include "wirekit.h"

#include <iostream>
#include <memory>
#include <vector>

class Counter {
private:
    int value;

public:
    explicit Counter(int initialValue) : value(initialValue) {}

    void increment(int amount) {
        value += amount;
    }

    int getValue() const {
        return value;
    }
};

int main() {
    wirekit::Hub<Counter> hub;
    wirekit::Channel channel("registration-modes");

    // This overload copies the concrete object into the hub.
    Counter prototype(3);
    hub.addInstance(channel, prototype);

    // This overload shares ownership with the caller; no raw pointer is used.
    auto sharedCounter = std::make_shared<Counter>(10);
    hub.addInstance(channel, sharedCounter);

    hub.callMethod(channel, &Counter::increment, 1);
    const std::vector<int> values = hub.callMethodGet(channel, &Counter::getValue);

    std::cout << "Hub copy: " << values.at(0) << '\n';
    std::cout << "Original object: " << prototype.getValue() << '\n';
    std::cout << "Shared object: " << sharedCounter->getValue() << '\n';

    // erase removes the hub's ownership; sharedCounter remains alive here
    // because this scope still owns its shared_ptr.
    hub.erase(channel);
}
