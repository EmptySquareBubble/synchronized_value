#include <atomic>
#include <thread>
#include <set>
#include <utility>
#include <compare>

// ---------------------------
// synchronized_value
// ---------------------------
template <typename... SVs>
class synchronized_scope;

template <typename T>
class synchronized_value
{
    T obj;
    mutable std::atomic<std::thread::id> locker_thread_id;

    template <typename...>
    friend class synchronized_scope;

public:
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

public:
    template <typename U>
    synchronized_value(U &&val) : obj(std::forward<U>(val)) {}

    synchronized_value(const synchronized_value &) = delete;
    synchronized_value &operator=(const synchronized_value &) = delete;

    class access_proxy
    {
        T *ptr;
        bool owns_lock = false;
        std::atomic<std::thread::id> *locker_thread_id;

        struct no_escape_ptr
        {
            T *ptr;
            T *operator->() const { return ptr; }

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
                locker_thread_id->store(std::thread::id{}, std::memory_order_release);
        }

        access_proxy(T *p, std::atomic<std::thread::id> *id)
            : ptr(p), locker_thread_id(id)
        {

            const auto current_thread_id = std::this_thread::get_id();
            
            // already locked for by current thread
            if (locker_thread_id->load(std::memory_order_relaxed) == current_thread_id)
                return;

            owns_lock = true;
            auto expected = std::thread::id{};
            while (!locker_thread_id->compare_exchange_weak(expected, current_thread_id, std::memory_order_acquire, std::memory_order_relaxed))
                expected = std::thread::id{};
        }

        no_escape_ptr operator->() { return no_escape_ptr{ptr}; }
        T &operator*() { return *ptr; }

        access_proxy &operator=(const T &rhs)
        {
            *ptr = rhs;
            return *this;
        }

        access_proxy &operator=(T &&rhs)
        {
            *ptr = std::move(rhs);
            return *this;
        }

        operator T() const
        {
            return *ptr; 
        }
    };

    auto operator->()
    {
        return access_proxy{&obj, &locker_thread_id};
    }

    auto operator*()
    {
        return operator->();
    }
};

// ---------------------------
// synchronized_scope
// ---------------------------
template <typename... SVs>
class synchronized_scope
{
    std::set<std::atomic<std::thread::id> *> sorted_vals;

public:
    synchronized_scope(SVs &...svs)
    {
        const auto current_thread_id = std::this_thread::get_id();

        (([&]
          {
            if (svs.locker_thread_id.load(std::memory_order_relaxed) != current_thread_id)
                sorted_vals.insert(&(svs.locker_thread_id)); }()),
         ...);

        for (auto &val : sorted_vals)
        {
            auto expected = std::thread::id{};
            while (!val->compare_exchange_weak(expected, current_thread_id, std::memory_order_acquire, std::memory_order_relaxed))
                expected = std::thread::id{};
        }
    }

    ~synchronized_scope()
    {
        for (auto val : sorted_vals)
            val->store(std::thread::id{}, std::memory_order_release);
    }
};