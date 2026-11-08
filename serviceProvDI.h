#ifndef SERVICE_PROV_DI_H
#define SERVICE_PROV_DI_H

#ifndef SERVICE_PROV_H
#error "Define SERVICE_PROVIDER_ENABLE_DI and include serviceProv.h instead of this header"
#endif

#include <functional>
#include <typeindex>
#include <utility>

namespace service {

namespace detail {
#ifdef SERVICE_PROVIDER_SINGLE_THREAD
using CreationMutex = ProviderMutex;
#else
using CreationMutex = std::recursive_mutex;
#endif
} // namespace detail

enum class Lifetime { Transient, Singleton };

// Experimental: constructors may take std::shared_ptr<T> dependencies; they are resolved automatically.
class Container {
private:
    static constexpr std::size_t maxAutoWiredDependencies = 8;

    // Converts to any std::shared_ptr<U> by resolving U from the container.
    struct Injector {
        Container& container;

        template <class U>
        operator std::shared_ptr<U>() const {
            return container.resolve<U>();
        }
    };

    template <class Impl, std::size_t... Index>
    static constexpr bool constructibleWith(std::index_sequence<Index...>) {
        return std::is_constructible_v<Impl, decltype((void)Index, std::declval<Injector>())...>;
    }

    template <class Impl, std::size_t... Index>
    static std::shared_ptr<Impl> makeWith(Container& container, std::index_sequence<Index...>) {
        return std::make_shared<Impl>(((void)Index, Injector{container})...);
    }

    // Picks the constructor with the most injectable parameters.
    template <class Impl, std::size_t Count>
    static std::shared_ptr<Impl> autoWire(Container& container) {
        if constexpr (Count == 0) {
            static_assert(
                std::is_constructible_v<Impl>,
                "Impl needs a constructor whose parameters are all std::shared_ptr dependencies");
            return std::make_shared<Impl>();
        } else if constexpr (constructibleWith<Impl>(std::make_index_sequence<Count>{})) {
            return makeWith<Impl>(container, std::make_index_sequence<Count>{});
        } else {
            return autoWire<Impl, Count - 1>(container);
        }
    }

    using Factory = std::function<std::shared_ptr<void>(Container&)>;

    struct Binding {
        Factory create;
        Lifetime lifetime = Lifetime::Singleton;
        std::shared_ptr<void> singleton;
    };

    struct Lookup {
        Factory create;
        Lifetime lifetime = Lifetime::Singleton;
        std::shared_ptr<void> singleton;
    };

    class ResolutionGuard {
    public:
        ResolutionGuard(const Container* owner, std::type_index type) {
            auto& active = stack();
            for (const auto& entry : active) {
                if (entry.first == owner && entry.second == type) {
                    throw std::logic_error("Circular dependency detected for " + std::string(type.name()));
                }
            }
            active.emplace_back(owner, type);
        }

        ~ResolutionGuard() {
            stack().pop_back();
        }

        ResolutionGuard(const ResolutionGuard&) = delete;
        ResolutionGuard& operator=(const ResolutionGuard&) = delete;

    private:
        static std::vector<std::pair<const Container*, std::type_index>>& stack() {
            static thread_local std::vector<std::pair<const Container*, std::type_index>> active;
            return active;
        }
    };

    std::unordered_map<std::type_index, Binding> bindings;
    mutable detail::ProviderMutex mutex;
    // Serializes singleton creation; never held while `mutex` is taken by user code.
    mutable detail::CreationMutex creationMutex;

    void store(std::type_index key, Binding binding) {
        Binding replaced;  // Destroyed after the lock is released.
        std::lock_guard<detail::ProviderMutex> lock(mutex);
        auto& slot = bindings[key];
        replaced = std::move(slot);
        slot = std::move(binding);
    }

    Lookup lookup(std::type_index key) const {
        std::lock_guard<detail::ProviderMutex> lock(mutex);
        const auto it = bindings.find(key);
        if (it == bindings.end()) {
            throw std::out_of_range(std::string("No binding for ") + key.name());
        }
        Lookup found;
        found.lifetime = it->second.lifetime;
        found.singleton = it->second.singleton;
        if (!found.singleton) {
            found.create = it->second.create;
        }
        return found;
    }

    // Factories run with no registry lock so they can resolve their own dependencies.
    std::shared_ptr<void> build(std::type_index key, const Factory& create) {
        ResolutionGuard guard(this, key);
        std::shared_ptr<void> instance = create(*this);
        if (!instance) {
            throw std::logic_error(std::string("Container factory returned null for ") + key.name());
        }
        return instance;
    }

public:
    Container() = default;
    Container(const Container&) = delete;
    Container& operator=(const Container&) = delete;
    ~Container() { clear(); }

    template <class Interface>
    Container& bindFactory(
        std::function<std::shared_ptr<Interface>(Container&)> factory,
        Lifetime lifetime = Lifetime::Singleton) {
        if (!factory) {
            throw std::invalid_argument("Container factory must not be empty");
        }
        Binding binding;
        binding.lifetime = lifetime;
        binding.create = [factory = std::move(factory)](Container& container) -> std::shared_ptr<void> {
            return factory(container);
        };
        store(typeid(Interface), std::move(binding));
        return *this;
    }

    // Without Deps, dependencies are deduced from the constructor; with Deps they are passed in order.
    template <class Interface, class Impl, class... Deps>
    Container& bind(Lifetime lifetime = Lifetime::Singleton) {
        static_assert(std::is_convertible_v<Impl*, Interface*>, "Impl must derive publicly from Interface");
        return bindFactory<Interface>(
            [](Container& container) -> std::shared_ptr<Interface> {
                if constexpr (sizeof...(Deps) == 0) {
                    return autoWire<Impl, maxAutoWiredDependencies>(container);
                } else {
                    return std::make_shared<Impl>(container.resolve<Deps>()...);
                }
            },
            lifetime);
    }

    // Builds an unregistered consumer with its constructor dependencies injected.
    template <class Consumer>
    std::shared_ptr<Consumer> make() {
        return autoWire<Consumer, maxAutoWiredDependencies>(*this);
    }

    template <class Interface>
    Container& bindInstance(std::shared_ptr<Interface> instance) {
        if (!instance) {
            throw std::invalid_argument("Container instance must not be null");
        }
        Binding binding;
        binding.singleton = instance;
        binding.create = [instance](Container&) -> std::shared_ptr<void> { return instance; };
        store(typeid(Interface), std::move(binding));
        return *this;
    }

    template <class Interface>
    std::shared_ptr<Interface> resolve() {
        const std::type_index key(typeid(Interface));
        Lookup found = lookup(key);
        if (found.singleton) {
            return std::static_pointer_cast<Interface>(found.singleton);
        }
        if (found.lifetime == Lifetime::Transient) {
            return std::static_pointer_cast<Interface>(build(key, found.create));
        }

        // Recursive, so a singleton factory can resolve other singletons; each is built exactly once.
        std::lock_guard<detail::CreationMutex> creation(creationMutex);
        found = lookup(key);
        if (found.singleton) {
            return std::static_pointer_cast<Interface>(found.singleton);
        }
        std::shared_ptr<void> instance = build(key, found.create);
        {
            std::lock_guard<detail::ProviderMutex> lock(mutex);
            const auto it = bindings.find(key);
            if (it != bindings.end() && !it->second.singleton) {
                it->second.singleton = instance;
            }
        }
        return std::static_pointer_cast<Interface>(instance);
    }

    void clear() {
        decltype(bindings) removed;
        {
            std::lock_guard<detail::ProviderMutex> lock(mutex);
            removed.swap(bindings);
        }
    }
};

} // namespace service

#endif
