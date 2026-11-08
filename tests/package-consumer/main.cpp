#include <wirekit/wirekit.h>

#include <memory>
#include <vector>

struct Service {
    int calls = 0;

    void invoke() {
        ++calls;
    }

    int count() const {
        return calls;
    }
};

int main() {
    wirekit::Hub<Service> hub;
    const auto channel = hub.registerChannel("consumer-smoke");
    hub.addInstance(channel, std::make_shared<Service>());
    hub.callMethod(channel, &Service::invoke);
    return hub.callMethodGet(channel, &Service::count) == std::vector<int>{1} ? 0 : 1;
}