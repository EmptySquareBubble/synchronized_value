#include <atomic>
#include <thread>
#include <set>
#include <utility>
#include <compare>
#include <mutex>
#include <concepts>

// ---------------------------
// synchronized_value
// ---------------------------
template<typename T>
concept SynchronizedValue = requires { typename T::lockable_type;   };

template <SynchronizedValue... SVs>
class synchronized_scope;

namespace detail{
    struct lockable
    {
        std::atomic<std::thread::id> locker_thread_id;
        
        void lock()
        {
            const auto current_thread_id = std::this_thread::get_id();
                
            auto expected = std::thread::id{};
            while (!locker_thread_id.compare_exchange_weak(expected, current_thread_id, std::memory_order_acquire, std::memory_order_relaxed))
                expected = std::thread::id{};
        }

        void unlock()
        {
            locker_thread_id.store(std::thread::id{}, std::memory_order_release);
        }

        bool try_lock()
        {
            const auto current_thread_id = std::this_thread::get_id();
                
            auto expected = std::thread::id{};
            return locker_thread_id.compare_exchange_strong(expected, current_thread_id, std::memory_order_acquire, std::memory_order_relaxed);    
        }
    };
}
template <typename T>
class synchronized_value
{
public:
    using lockable_type = detail::lockable;

    auto operator<=>(const synchronized_value &other) const
    {
        synchronized_scope scope(const_cast<synchronized_value &>(*this), const_cast<synchronized_value &>(other));
        return obj <=> other.obj;
    }

    bool operator==(const synchronized_value &other) const
    {
        synchronized_scope scope(const_cast<synchronized_value &>(*this), const_cast<synchronized_value &>(other));
        return obj == other.obj;
    }

    template <typename U>
    synchronized_value(U &&val) : obj(std::forward<U>(val)) {}

    synchronized_value(const synchronized_value &) = delete;
    synchronized_value &operator=(const synchronized_value &) = delete;

    class access_proxy
    {
        synchronized_value<T>& ptr;
        bool owns_lock = false;
        struct no_escape_ptr
        {
            T *obj;
            T *operator->() const { return obj; }

            // prevent implicit conversion to T*
            operator T *() const = delete;
        };

    public:
        access_proxy(const access_proxy &) = delete;
        access_proxy &operator=(const access_proxy &) = delete;
        access_proxy(access_proxy &&) = delete;
        access_proxy &operator=(access_proxy &&) = delete;

        ~access_proxy()
        {
            if (owns_lock)
                ptr.lock.unlock();
        }

        access_proxy(synchronized_value<T> &p)
            : ptr(p)
        {

            const auto current_thread_id = std::this_thread::get_id();
            
            // already locked by current thread
            if (ptr.lock.locker_thread_id.load(std::memory_order_relaxed) == current_thread_id)
                return;

            owns_lock = true;
            ptr.lock.lock();
        }

        no_escape_ptr operator->() { return no_escape_ptr{&(ptr.obj)}; }
        T &operator*() { return ptr.obj; }

        access_proxy &operator=(const T &rhs)
        {
            ptr.obj = rhs;
            return *this;
        }

        access_proxy &operator=(T &&rhs)
        {
            ptr.obj = std::move(rhs);
            return *this;
        }

        operator T() const
        {
            return ptr.obj; 
        }
    };

    auto operator->()
    {
        return access_proxy{*this};
    }

    auto operator*()
    {
        return operator->();
    }
    
    private:
        lockable_type lock;
        T obj;
        
        template <SynchronizedValue... SVs>
        friend class synchronized_scope;
};

// ---------------------------
// synchronized_scope
// ---------------------------
template <SynchronizedValue... SVs>
class synchronized_scope
{
    detail::lockable dummy_lock;
    std::scoped_lock<typename SVs::lockable_type& ...> lock;

public:
    synchronized_scope(SVs &... svs)
        : dummy_lock{},
          lock( (svs.lock.locker_thread_id.load(std::memory_order_relaxed) != std::this_thread::get_id()
                    ? svs.lock
                    : dummy_lock) ... )
    {}
};
