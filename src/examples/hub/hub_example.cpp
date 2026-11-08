#include "request_types.h"
#include "user_service.h"

#include <iostream>
#include <vector>

void printIds(
    example::UserService& userService,
    const wirekit::ChannelPtr& channel,
    const char* label) {
    const auto ids = userService.callMethodGet(channel, &example::IUser::getId);
    std::cout << label;
    for (std::size_t index = 0; index < ids.size(); ++index) {
        if (index != 0) {
            std::cout << ", ";
        }
        std::cout << ids[index];
    }
    std::cout << '\n';
}

int main() {
    using namespace example;

    // 1. Create the hub. It owns the service instances and their lifecycle.
    UserService userService;

    // 2. A Channel is a shared grouping key. Instances registered with this
    //    channel will receive the same broadcast calls.
    wirekit::Channel request("request-42");

    // 3. Request types are compile-time tags. No StaffRequest or NormalRequest
    //    objects are needed or kept alive; the factory uses typeid on the type.

    // The first registration creates and returns the shared Channel handle.
    const auto channel = userService.addInstance<StaffRequest>(request);

    // Reuse that handle to put a second implementation in the same group.
    userService.addInstance<NormalRequest>(channel);

    // 4. callMethod broadcasts setId(21) to every IUser in this channel.
    userService.callMethod(channel, &IUser::setId, 21);

    // 5. callMethodGet also broadcasts, then returns one result per instance
    //    in registration order: staff first, normal second.
    const std::vector<int> ids = userService.callMethodGet(channel, &IUser::getId);

    std::cout << "Staff ID: " << ids.at(0) << '\n';
    std::cout << "Normal ID: " << ids.at(1) << '\n';

    // Other code does not need a global hub: pass the same hub by
    // reference to a function, while passing the channel handle separately.
    printIds(userService, channel, "Access through a function: ");

    // findChannel only looks up an existing name; it never creates a channel.
    const auto recoveredChannel = userService.findChannel("request-42");
    if (recoveredChannel) {
        printIds(userService, recoveredChannel, "Found named channel: ");
    }
    if (!userService.findChannel("missing-request")) {
        std::cout << "Missing channel was not created by lookup.\n";
    }

    // A different name creates an independent group in the same hub.
    wirekit::Channel backgroundTask("background-task");
    const auto backgroundChannel = userService.addInstance<NormalRequest>(backgroundTask);
    userService.callMethod(backgroundChannel, &IUser::setId, 7);
    printIds(userService, backgroundChannel, "Independent channel: ");

    // The caller-object overload is also available. It reads the caller's type
    // while addInstance runs; it does not retain a reference to that object.
    wirekit::Channel callerBasedRequest("caller-based-request");
    wirekit::ChannelPtr callerBasedChannel;
    {
        StaffRequest shortLivedCaller;
        callerBasedChannel = userService.addInstance(callerBasedRequest, shortLivedCaller);
    }
    userService.callMethod(callerBasedChannel, &IUser::setId, 13);
    printIds(userService, callerBasedChannel, "After caller leaves scope: ");

    // Remove all groups explicitly. The hub itself remains alive until
    // main exits; another UserService object would own a separate registry.
    userService.erase(channel);
    userService.erase(backgroundChannel);
    userService.erase(callerBasedChannel);
}