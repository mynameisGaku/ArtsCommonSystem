#pragma once

#include <Pch.h>

#include "Blackboard.h"
#include "BlackboardKeys.h"
#include "Transform.h"
#include "components/GOC_TransformUpdater.h"

class SceneBase;

class GameObject : public IObject
{
public:
    GameObject()
    {
        AddGOC<Transform>();
        AddGOC<GOC_TransformUpdater>();
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
    T* AddGOC(Args&&... args)
    {
        static_assert(std::is_base_of_v<GO_Component, T>);

        if constexpr (std::is_same_v<T, Transform>)
        {
            if (GetGOC<Transform>() != nullptr)
            {
                return GetGOC<Transform>();
            }
        }

        T* c = new T(std::forward<Args>(args)...);
        c->SetOwner(this);
        m_GOCs.push_back(c);
        return c;
    }

    template<class T>
    T* GetGOC() const
    {
        static_assert(std::is_base_of_v<GO_Component, T>);

        for (GO_Component* c : m_GOCs)
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
        static_assert(std::is_base_of_v<GO_Component, T>);

        if constexpr (std::is_same_v<T, Transform>)
        {
            return false;
        }

        for (auto it = m_GOCs.begin(); it != m_GOCs.end(); ++it)
        {
            if (dynamic_cast<T*>(*it) != nullptr)
            {
                delete* it;
                m_GOCs.erase(it);
                return true;
            }
        }

        return false;
    }

    void Awake() override
    {
        for (GO_Component* c : m_GOCs)
        {
            if (c->IsEnabled())
            {
                c->Awake();
            }
        }
    }

    void Start() override
    {
        for (GO_Component* c : m_GOCs)
        {
            if (c->IsEnabled())
            {
                c->Start();
            }
        }
    }

    void Update(float deltaTime) override
    {
        for (GO_Component* c : m_GOCs)
        {
            if (c->IsEnabled())
            {
                c->Update(deltaTime);
            }
        }
    }

    void FixedUpdate(float fixedDeltaTime) override
    {
        for (GO_Component* c : m_GOCs)
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
            m_GOCs.begin(),
            m_GOCs.end(),
            [](GO_Component* a, GO_Component* b)
            {
                return a->GetLayerIndex() < b->GetLayerIndex();
			});

        for (GO_Component* c : m_GOCs)
        {
            if (c->IsEnabled())
            {
                c->Draw(deltaTime);
            }
        }
    }

    void Destroy() override
    {
        for (GO_Component* c : m_GOCs)
        {
            c->Destroy();
            delete c;
        }

        m_GOCs.clear();
    }

    void DontDestroyOnLoad()
    {
        m_DontDestroyOnLoad = true;
    }

    bool IsDontDestroyOnLoad() const
    {
        return m_DontDestroyOnLoad;
    }

    void SetLayer(const std::string& layerName);

    int GetLayerIndex() const
    {
        return m_LayerIndex;
	}
    
    void SetScene(SceneBase* scene)
    {
        m_pScene = scene;
    }

private:
    SceneBase* m_pScene;
    std::vector<GO_Component*> m_GOCs;
    bool m_DontDestroyOnLoad = false;
    int m_LayerIndex;
	int m_DrawOrder = 0;
    Blackboard m_Blackboard;
};
