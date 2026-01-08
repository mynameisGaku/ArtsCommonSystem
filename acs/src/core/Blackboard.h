#pragma once

#include <Pch.h>

class Blackboard
{
public:
    using Key = std::uint32_t;

    struct Entry
    {
        Key key = 0;
        std::uint32_t type = 0;
        std::uint32_t version = 0;

        static constexpr int PayloadSize = 64;
        unsigned char payload[PayloadSize]{};
        std::uint16_t bytes = 0;
    };

public:
    template<class T>
    void Set(Key key, const T& value)
    {
        static_assert(sizeof(T) <= Entry::PayloadSize);

        Entry* e = FindOrCreate(key);
        e->type = TypeId<T>();
        e->bytes = static_cast<std::uint16_t>(sizeof(T));
        std::memcpy(e->payload, &value, sizeof(T));
        ++e->version;
    }

    template<class T>
    bool Get(Key key, T& out) const
    {
        const Entry* e = Find(key);
        if (e == nullptr)
        {
            return false;
        }

        if (e->type != TypeId<T>())
        {
            return false;
        }

        if (e->bytes != sizeof(T))
        {
            return false;
        }

        std::memcpy(&out, e->payload, sizeof(T));
        return true;
    }

    std::uint32_t Version(Key key) const
    {
        const Entry* e = Find(key);
        if (e == nullptr)
        {
            return 0;
        }
        return e->version;
    }

private:
    template<class T>
    static std::uint32_t TypeId()
    {
        static int dummy;
        return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&dummy));
    }

    Entry* FindOrCreate(Key key)
    {
        for (auto& e : m_Entries)
        {
            if (e.key == key)
            {
                return &e;
            }
        }

        Entry ne;
        ne.key = key;
        m_Entries.push_back(ne);
        return &m_Entries.back();
    }

    const Entry* Find(Key key) const
    {
        for (const auto& e : m_Entries)
        {
            if (e.key == key)
            {
                return &e;
            }
        }
        return nullptr;
    }

private:
    std::vector<Entry> m_Entries;
};
