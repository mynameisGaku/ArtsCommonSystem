#pragma once

#include <Pch.h>

#include "Blackboard.h"
#include "BlackboardKeys.h"
#include "Transform.h"

class GameObject : public IObject
{
public:
    GameObject()
    {
        AddComponent<Transform>();
    }

    virtual ~GameObject()
    {
        Destroy();
    }

    Blackboard& GetBlackboard()
    {
        return m_Blackboard;
    }

    template<class T, class... Args>
    T* AddComponent(Args&&... args)
    {
        static_assert(std::is_base_of_v<Component, T>);

        if constexpr (std::is_same_v<T, Transform>)
        {
            if (GetComponent<Transform>() != nullptr)
            {
                return GetComponent<Transform>();
            }
        }

        T* c = new T(std::forward<Args>(args)...);
        c->SetOwner(this);
        m_Components.push_back(c);
        return c;
    }

    template<class T>
    T* GetComponent() const
    {
        static_assert(std::is_base_of_v<Component, T>);

        for (Component* c : m_Components)
        {
            if (auto casted = dynamic_cast<T*>(c))
            {
                return casted;
            }
        }

        return nullptr;
    }

    template<class T>
    bool RemoveComponent()
    {
        static_assert(std::is_base_of_v<Component, T>);

        if constexpr (std::is_same_v<T, Transform>)
        {
            return false;
        }

        for (auto it = m_Components.begin(); it != m_Components.end(); ++it)
        {
            if (dynamic_cast<T*>(*it) != nullptr)
            {
                delete* it;
                m_Components.erase(it);
                return true;
            }
        }

        return false;
    }

    void Awake() override
    {
        for (Component* c : m_Components)
        {
            if (c->IsEnabled())
            {
                c->Awake();
            }
        }
    }

    void Start() override
    {
        for (Component* c : m_Components)
        {
            if (c->IsEnabled())
            {
                c->Start();
            }
        }
    }

    void Update(float deltaTime) override
    {
        for (Component* c : m_Components)
        {
            if (c->IsEnabled())
            {
                c->Update(deltaTime);
            }
        }
    }

    void FixedUpdate(float fixedDeltaTime) override
    {
        for (Component* c : m_Components)
        {
            if (c->IsEnabled())
            {
                c->FixedUpdate(fixedDeltaTime);
            }
        }
    }

    void Draw(float deltaTime) override
    {
        // sort
        std::sort(
            m_Components.begin(),
            m_Components.end(),
            [](Component* a, Component* b)
            {
                return a->GetDrawOrder() < b->GetDrawOrder();
			});

        for (Component* c : m_Components)
        {
            if (c->IsEnabled())
            {
                c->Draw(deltaTime);
            }
        }
    }

    void Destroy() override
    {
        for (Component* c : m_Components)
        {
            c->Destroy();
            delete c;
        }

        m_Components.clear();
    }

    void DontDestroyOnLoad()
    {
        m_DontDestroyOnLoad = true;
    }

    bool IsDontDestroyOnLoad() const
    {
        return m_DontDestroyOnLoad;
    }

    void SetDrawOrder(int order)
	{
        m_DrawOrder = order;
	}

    int GetDrawOrder() const
    {
        return m_DrawOrder;
	}

private:
    std::vector<Component*> m_Components;
    bool m_DontDestroyOnLoad = false;
	int m_DrawOrder = 0;
    Blackboard m_Blackboard;
};
