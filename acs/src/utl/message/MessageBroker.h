#pragma once

#include <Pch.h>

// 購読解除を管理するトークン
class Subscription
{
public:
    using UnsubscribeFunc = std::function<void()>;

    Subscription() = default;

    Subscription(UnsubscribeFunc func)
        : unsubscribeFunc_(std::move(func))
    {
    }

    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    Subscription(Subscription&& other) noexcept
        : unsubscribeFunc_(std::move(other.unsubscribeFunc_))
    {
        other.unsubscribeFunc_ = nullptr;
    }

    Subscription& operator=(Subscription&& other) noexcept
    {
        if (this != &other)
        {
            Dispose();
            unsubscribeFunc_ = std::move(other.unsubscribeFunc_);
            other.unsubscribeFunc_ = nullptr;
        }
        return *this;
    }

    ~Subscription()
    {
        Dispose();
    }

    void Dispose()
    {
        if (unsubscribeFunc_)
        {
            unsubscribeFunc_();
            unsubscribeFunc_ = nullptr;
        }
    }

private:
    UnsubscribeFunc unsubscribeFunc_;
};

// 型消去のための基底クラス
struct ISubject
{
    virtual ~ISubject() = default;
};

// 特定のメッセージ型を管理するクラス
template <typename T>
class Subject : public ISubject
{
public:
    using Callback = std::function<void(const T&)>;

    int Add(Callback callback)
    {
        int id = nextId_++;
        subscribers_[id] = std::move(callback);
        return id;
    }

    void Remove(int id)
    {
        subscribers_.erase(id);
    }

    void OnNext(const T& msg)
    {
        auto tempSubscribers = subscribers_;
        for (const auto& pair : tempSubscribers)
        {
            pair.second(msg);
        }
    }

private:
    int nextId_ = 0;
    std::unordered_map<int, Callback> subscribers_;
};

// 受信設定を行うビルダークラス (単一)
template <typename T>
class Observable
{
public:
    Observable()
        : filter_([](const T&)
            {
                return true;
            })
    {
    }

    Observable<T>& Where(std::function<bool(const T&)> predicate)
    {
        auto prevFilter = filter_;
        filter_ = [prevFilter, predicate](const T& t)
            {
                return prevFilter(t) && predicate(t);
            };
        return *this;
    }

    Subscription Subscribe(std::function<void(const T&)> onNext);

private:
    std::function<bool(const T&)> filter_;
};

// C++11向け: index_sequence 代替
template <size_t... Is>
struct IndexSequence
{
};

template <size_t N, size_t... Is>
struct MakeIndexSequence : MakeIndexSequence<N - 1, N - 1, Is...>
{
};

template <size_t... Is>
struct MakeIndexSequence<0, Is...>
{
    using type = IndexSequence<Is...>;
};

// tuple apply (C++11)
template <typename Func, typename Tuple, size_t... Is>
auto ApplyImpl(Func&& f, Tuple&& t, IndexSequence<Is...>)
-> decltype(f(std::get<Is>(t)...))
{
    return f(std::get<Is>(t)...);
}

template <typename Func, typename Tuple>
auto Apply(Func&& f, Tuple&& t)
-> decltype(ApplyImpl(
    std::forward<Func>(f),
    std::forward<Tuple>(t),
    typename MakeIndexSequence<std::tuple_size<typename std::decay<Tuple>::type>::value>::type()
))
{
    return ApplyImpl(
        std::forward<Func>(f),
        std::forward<Tuple>(t),
        typename MakeIndexSequence<std::tuple_size<typename std::decay<Tuple>::type>::value>::type()
    );
}

// ここが修正ポイント：ローカルクラス内メンバテンプレートをやめて、外に出す
template <typename T, typename... Ts>
T PopFrontFromQueues(std::tuple<std::deque<Ts>...>& queues)
{
    auto& q = std::get<std::deque<T>>(queues);
    T v = q.front();
    q.pop_front();
    return v;
}

// 可変長 Zip: Ts... が全部そろった時だけ通知
template <typename... Ts>
class ObservableZip
{
public:
    using Callback = std::function<void(const Ts&...)>;

    ObservableZip()
        : filters_(std::function<bool(const Ts&)>([](const Ts&)
            {
                return true;
            })...)
    {
    }

    // 特定型だけフィルタ（任意）
    template <typename U>
    ObservableZip<Ts...>& Where(std::function<bool(const U&)> predicate)
    {
        auto& f = std::get<std::function<bool(const U&)>>(filters_);
        auto prev = f;

        f = [prev, predicate](const U& u)
            {
                return prev(u) && predicate(u);
            };

        return *this;
    }

    Subscription Subscribe(Callback onNext);

private:
    std::tuple<std::function<bool(const Ts&)>...> filters_;
};

// シングルトンのブローカー
class MessageBroker
{
public:
    static MessageBroker& Default()
    {
        static MessageBroker instance;
        return instance;
    }

    template <typename T>
    void Publish(const T& message)
    {
        auto typeIdx = std::type_index(typeid(T));
        auto it = subjects_.find(typeIdx);
        if (it != subjects_.end())
        {
            static_cast<Subject<T>*>(it->second.get())->OnNext(message);
        }
    }

    // 単一型
    template <typename T>
    Observable<T> Receive()
    {
        return Observable<T>();
    }

    // 可変長（2個以上）
    template <typename T1, typename T2, typename... Ts>
    ObservableZip<T1, T2, Ts...> Receive()
    {
        return ObservableZip<T1, T2, Ts...>();
    }

    template <typename T>
    Subscription Register(std::function<void(const T&)> callback)
    {
        auto typeIdx = std::type_index(typeid(T));

        if (subjects_.find(typeIdx) == subjects_.end())
        {
            subjects_[typeIdx] = std::make_unique<Subject<T>>();
        }

        auto subject = static_cast<Subject<T>*>(subjects_[typeIdx].get());
        int id = subject->Add(std::move(callback));

        return Subscription(
            [this, typeIdx, id]()
            {
                auto it = subjects_.find(typeIdx);
                if (it != subjects_.end())
                {
                    auto subj = static_cast<Subject<T>*>(it->second.get());
                    subj->Remove(id);
                }
            }
        );
    }

private:
    MessageBroker() = default;
    ~MessageBroker() = default;
    MessageBroker(const MessageBroker&) = delete;
    MessageBroker& operator=(const MessageBroker&) = delete;

    std::unordered_map<std::type_index, std::unique_ptr<ISubject>> subjects_;
};

// Observable<T> の実装
template <typename T>
Subscription Observable<T>::Subscribe(std::function<void(const T&)> onNext)
{
    auto filter = filter_;
    return MessageBroker::Default().template Register<T>(
        [filter, onNext](const T& msg)
        {
            if (filter(msg))
            {
                onNext(msg);
            }
        }
    );
}

// ObservableZip<Ts...> の実装
template <typename... Ts>
Subscription ObservableZip<Ts...>::Subscribe(Callback onNext)
{
    struct State
    {
        std::tuple<std::deque<Ts>...> queues;
        std::tuple<std::function<bool(const Ts&)>...> filters;
        Callback onNext;
        bool active = true;

        bool AllNonEmptyImpl()
        {
            bool ok = true;

            int dummy[] =
            {
                0,
                (ok = ok && !std::get<std::deque<Ts>>(queues).empty(), 0)...
            };
            (void)dummy;

            return ok;
        }

        void TryEmit()
        {
            while (active && AllNonEmptyImpl())
            {
                std::tuple<Ts...> args(PopFrontFromQueues<Ts>(queues)...);

                Apply(
                    [this](const Ts&... a)
                    {
                        onNext(a...);
                    },
                    args
                );
            }
        }
    };

    auto state = std::make_shared<State>();
    state->filters = filters_;
    state->onNext = std::move(onNext);

    auto subs = std::make_shared<std::vector<Subscription>>();
    subs->reserve(sizeof...(Ts));

    int dummy[] =
    {
        0,
        (subs->push_back(
            MessageBroker::Default().template Register<Ts>(
                [state](const Ts& msg)
                {
                    if (!state->active)
                    {
                        return;
                    }

                    auto& f = std::get<std::function<bool(const Ts&)>>(state->filters);
                    if (!f(msg))
                    {
                        return;
                    }

                    std::get<std::deque<Ts>>(state->queues).push_back(msg);
                    state->TryEmit();
                }
            )
        ), 0)...
    };
    (void)dummy;

    return Subscription(
        [state, subs]()
        {
            state->active = false;

            for (size_t i = 0; i < subs->size(); i++)
            {
                (*subs)[i].Dispose();
            }

            subs->clear();
        }
    );
}
