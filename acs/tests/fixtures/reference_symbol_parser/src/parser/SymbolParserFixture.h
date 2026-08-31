// SPDX-License-Identifier: MIT
#pragma once

namespace acs
{
namespace reference_fixture
{
class FForward;

enum EShiftFlags : unsigned
{
    ShiftNone = 0u,
    ShiftRead = 1u << 0,
    ShiftWrite = 1u << 1,
};

template<class T>
class TBox
{
public:
    using FValue = T;
    static constexpr int kCallLimit = MakeLimit();
    static constexpr int kLambdaLimit = [] { return 64; }();
    void (*OnValue)(T&) = nullptr;

    TBox& operator=(const TBox&) = delete;
    void Set(T value);
    void Set(const T& value);
    int operator()(const T& value) const noexcept;
    class FForward* Forward() noexcept { return nullptr; }
    const char* Title() const noexcept { return "TBox"; }
    void Bind(class FForward& value) noexcept;

private:
    struct FSlot
    {
    };
};

struct FAggregate
{
    int values[2][3]{};
    union
    {
        struct
        {
            int x;
        } first;
        struct
        {
            int y;
        } second;
    };
};

struct FTaggedValue
{
    int kind = 0;
    union
    {
        bool b;
        double num;
        const char* str;
        unsigned handle;
    } v;
};

struct FDefined
{
    static int Build(int value) noexcept;
};

inline int FDefined::Build(int value) noexcept
{
    return value;
}

namespace detail
{
struct FInside
{
};
}

struct FAfter
{
};

inline int ClampValue(int value, int minimum, int maximum) noexcept
{
    const int candidate = SelectValue(value);
    return candidate < minimum ? minimum : (candidate > maximum ? maximum : candidate);
}

template<typename FSubmit>
inline unsigned ForEachValue(unsigned count, FSubmit&& submit) noexcept(noexcept(submit(unsigned{})))
{
    for (unsigned index = 0; index < count; ++index)
    {
        submit(index);
    }
    return count;
}
}
}

#define ACS_REFERENCE_EXPECT(expr) \
    do \
    { \
        Check(expr); \
    } while (false)

#define NOMINMAX

namespace acs::reference_fixture
{
class FForward
{
public:
    int forwardValue = 0;
};

template<typename T> class FConversion
{
public:
    operator const T&() const noexcept;

    void Visit() noexcept
    {
        struct FLocalContext
        {
            int hiddenValue = 0; struct FLocalLeak { int hiddenNested = 0; };
        };
    }

private:
    friend class FFriendOnly;
};

class FOwner
{
private:
    class FNested;
};

class FOwner::FNested
{
public:
    int nestedValue = 0;
};

struct FCompactAggregate
{
    union { bool compactFlag; int compactCount; } compact{};

    union FStorage
    {
        FStorage() noexcept {}
        int storedValue;
    } storage;
};

struct FMatrixLike
{
    int matrixValues[2];

    constexpr FMatrixLike() noexcept
        : matrixValues{1, 2} {}

    FMatrixLike(int value) noexcept:matrixValues{value, value}{}

    static FMatrixLike IdentityLike() noexcept { return {}; }
};

template<typename Signature>
class TCallable;

template<typename... Arguments>
class TCallable<void(Arguments...)>
{
};

template<typename T>
struct TTraits;

template<>
struct TTraits<int>
{
};

template<>
struct TTraits<float>
{
};
}

#if defined(_WIN32)
    #define ACS_REFERENCE_PLATFORM() 1
#else
    #define ACS_REFERENCE_PLATFORM() 0
#endif

#define ACS_DETAIL_REFERENCE_INTERNAL() 0
