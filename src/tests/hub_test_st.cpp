#define WIREKIT_SINGLE_THREAD
#include "wirekit.h"

#include <cassert>
#include <memory>
#include <vector>

struct Counter {
    int value = 0;

    void add(int amount) {
        value += amount;
    }

    int get() const {
        return value;
    }
};

int main() {
    wirekit::Hub<Counter> hub;
    wirekit::Channel channel("single-thread");
    Counter initial{5};
    hub.addInstance(channel, initial);

    hub.callMethod(channel, &Counter::add, 3);
    assert((hub.callMethodGet(channel, &Counter::get) == std::vector<int>{8}));

    std::weak_ptr<wirekit::Channel> expiredChannel;
    {
        auto temporaryChannel = hub.registerChannel("manual-garbage-collection");
        expiredChannel = temporaryChannel;
    }
    assert(!expiredChannel.expired());

    hub.collectGarbage();
    assert(expiredChannel.expired());
    assert(!hub.findChannel("manual-garbage-collection"));

    hub.clear();
    assert(!hub.findChannel("single-thread"));
    assert(hub.registerChannel("single-thread"));

    wirekit::Channel retainedChannel("retained-service");
    Counter retainedService{11};
    hub.addInstance(retainedChannel, retainedService);
    hub.collectGarbage();
    assert((hub.callMethodGet(retainedChannel, &Counter::get) == std::vector<int>{11}));
    hub.erase(retainedChannel);
}
