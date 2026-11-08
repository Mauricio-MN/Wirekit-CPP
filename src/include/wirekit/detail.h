#ifndef WIREKIT_DETAIL_H
#define WIREKIT_DETAIL_H

#include <memory>
#include <mutex>
#include <type_traits>

namespace wirekit {

namespace detail {

template <class>
struct IsSharedPtr : std::false_type {};

template <class T>
struct IsSharedPtr<std::shared_ptr<T>> : std::true_type {};

#ifdef WIREKIT_SINGLE_THREAD
class SyncMutex {
public:
    void lock() noexcept {}
    void unlock() noexcept {}
};
#else
using SyncMutex = std::mutex;
#endif

} // namespace detail

} // namespace wirekit

#endif
