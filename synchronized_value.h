#include <print>
#include <mutex>
#include <tuple>
#include <string>
#include <utility>
#include <atomic>
#include <vector>
#include <ranges>
#include <functional>
#include <thread>
#include <algorithm>

// ---------------------------
// synchronized_value
// ---------------------------
template<typename... SVs>
class synchronized_scope;

enum class LockState : int {
        Unlocked = 0,
        Operator = 1,
        Scope = 2
    };
template<typename T>
class synchronized_value {
    T obj;
    mutable std::mutex mtx;
    
    mutable std::atomic<LockState> lock_state = LockState::Unlocked;
    mutable std::thread::id locker_thread_id; //for runtime error support while trying to lock same value from more scopes in single thread

    template<typename...>
    friend class synchronized_scope;

    std::mutex& get_mutex() const noexcept { return mtx; }

    // Called by synchronized_scope to acquire lock and mark participation
    void lock_for_scope() const {
        LockState expected = LockState::Unlocked;
        while(!lock_state.compare_exchange_weak(expected, LockState::Scope))
            expected = LockState::Unlocked;
        mtx.lock();
    }

    void unlock_from_scope() const {
        lock_state = LockState::Unlocked;
        lock_state.notify_one();
        mtx.unlock();
    }

public:
    auto operator<=>(const synchronized_value& other) const {
        synchronized_scope scope(const_cast<synchronized_value&>(*this), const_cast<synchronized_value&>(other));
        return obj <=> other.obj;
    }

    bool operator==(const synchronized_value& other) const {
        synchronized_scope scope(const_cast<synchronized_value&>(*this), const_cast<synchronized_value&>(other));
        return obj == other.obj;
    }
public:
    template<typename U>
    synchronized_value(U&& val) : obj(std::forward<U>(val)) {}

    synchronized_value(const synchronized_value&) = delete;
    synchronized_value& operator=(const synchronized_value&) = delete;

    class access_proxy {
        T* ptr;
        std::unique_lock<std::mutex> lock;
        std::atomic<LockState>& lock_state;
        
        struct no_escape_ptr {
            T* ptr;
            T* operator->() const { return ptr; }

            // prevent implicit conversion to T*
            operator T*() const = delete;
        };
    public:
        access_proxy(const access_proxy&) = delete;
        access_proxy& operator=(const access_proxy&) = delete;
        access_proxy(access_proxy&&) = delete;
        access_proxy& operator=(access_proxy&&) = delete;

        ~access_proxy() {
            if (lock.owns_lock()) {
                lock_state.store(LockState::Unlocked);
                lock_state.notify_one();
            }
        }

        access_proxy(T* p, std::mutex& mtx, std::atomic<LockState>& state, std::thread::id& locker_id)
            : ptr(p), lock_state(state)
        {
            LockState expected = LockState::Unlocked;

            while (true) {
                if (state.compare_exchange_weak(expected, LockState::Operator)) {
                    lock = std::unique_lock<std::mutex>(mtx);
                    break;
                } else {
                    if (expected == LockState::Scope && locker_id == std::this_thread::get_id()) {
                        break;
                    }
                    state.wait(expected); // wait until the lock state changes
                    expected = LockState::Unlocked;
                }
            }
        }

        no_escape_ptr operator->() { return no_escape_ptr{ptr}; }
        T& operator*() { return *ptr; }

        // Add assignment operator to forward to the underlying object
        access_proxy& operator=(const T& rhs) {
            *ptr = rhs;
            return *this;
        }

        access_proxy& operator=(T&& rhs) {
            *ptr = std::move(rhs);
            return *this;
        }

        operator T() const {
            return *ptr; // ptr is locked via unique_lock in proxy
        }
    };

    auto operator->() {
        return access_proxy{&obj, mtx, lock_state, locker_thread_id};
    }

    auto operator*() {
        return operator->();
    }
};

// ---------------------------
// synchronized_scope
// ---------------------------
template<typename... SVs>
class synchronized_scope {
    std::tuple<SVs*...> sv_ptrs;
    
public:
    synchronized_scope(SVs&... svs)
        : sv_ptrs(&svs...)
    {
        lock_all();
    }

    ~synchronized_scope() {
        unlock_all();
    }

private:
    void lock_all() {
        while(true) {
            //Build a vector of lock tasks for unlocked SVs
            using lock_entry = std::tuple<void*, std::function<void()>>; // key = pointer, action = lock & mark
            std::vector<lock_entry> lock_plan;

            std::vector<std::atomic<LockState>*> locks_to_wait_for;
            std::apply([&](auto*... svs) {
                (..., ([&] {
                    LockState expected = LockState::Unlocked;
                    if(svs->lock_state.compare_exchange_strong(expected, LockState::Scope)){
                        lock_plan.emplace_back(
                            static_cast<void*>(svs),
                            [svs, this]() {
                                svs->get_mutex().lock(); 
                                svs->locker_thread_id = std::this_thread::get_id();
                            }
                        );
                    }
                    else
                    {
                        if (svs->locker_thread_id == std::this_thread::get_id()) {
                            throw std::logic_error("synchronized_value used in nested scope by the same thread");
                        }
                        locks_to_wait_for.push_back(&svs->lock_state);
                    }
                }()));
            }, sv_ptrs);

            if (!locks_to_wait_for.empty()) {
                // Wait on all locked flags until they become false (unlocked)
                for (auto atomic_lock : locks_to_wait_for) {
                    // Wait while flag is true, unblock when false
                    atomic_lock->wait(LockState::Scope);
                }
                // After waking, loop again to retry locking
                continue;
            }

            //Sort by address to avoid deadlock
            std::sort(lock_plan.begin(), lock_plan.end(),
                    [](const lock_entry& a, const lock_entry& b) {
                        return std::less<void*>{}(std::get<0>(a), std::get<0>(b));
                    });

            //Execute lock actions
            for (auto& [_, action] : lock_plan) {
                action();
            }

            return;
        }
    }

    void unlock_all() {
        // Unlock and reset lock_state flags for all SVs
        std::apply([&](auto*... svs) {
            (([&] {
                svs->lock_state.store(LockState::Unlocked);
                svs->locker_thread_id = {};
                svs->get_mutex().unlock();
                svs->lock_state.notify_one();
                
            }()), ...);
        }, sv_ptrs);
    }
};