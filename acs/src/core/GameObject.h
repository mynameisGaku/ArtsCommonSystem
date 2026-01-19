#pragma once

#include <Pch.h>

class SceneBase;
struct SceneContext;
class Transform;

class GameObject : public IObject
{
public:
    GameObject();
    virtual ~GameObject();

    template<class T>
    static std::uint32_t GetComponentTypeID()
    {
        static char dummy;
        return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&dummy));
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
        m_ComponentCache[GetComponentTypeID<T>()] = c;
        return c;
    }

    template<class T>
    T* GetGOC() const
    {
        static_assert(std::is_base_of_v<GO_Component, T>);
        auto it = m_ComponentCache.find(GetComponentTypeID<T>());
        if (it != m_ComponentCache.end())
        {
            return static_cast<T*>(it->second);
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
        auto it = m_ComponentCache.find(GetComponentTypeID<T>());
        if (it == m_ComponentCache.end())
        {
            return false;
        }
        GO_Component* target = it->second;
        m_ComponentCache.erase(it);
        auto vecIt = std::find(m_GOCs.begin(), m_GOCs.end(), target);
        if (vecIt != m_GOCs.end())
        {
            delete* vecIt;
            m_GOCs.erase(vecIt);
            return true;
        }
        return false;
    }


    void Awake() override;
    void Start() override;
    void Update(float deltaTime) override;
    void FixedUpdate(float fixedDeltaTime) override;
    void Draw(float deltaTime) override;
    void Destroy() override;

    void DontDestroyOnLoad();
    bool IsDontDestroyOnLoad() const;

    void SetLayer(const std::string& layerName);
    int GetLayerIndex() const;

    void SetScene(SceneBase* scene);
    SceneBase* GetScene() const;
    SceneContext& GetSceneContext();

    void SetName(const std::string& name);
    const std::string& GetName() const;

    void SetEnabled(bool enable);
    bool IsEnabled() const;

private:
    std::string m_Name = "GameObject";
    SceneBase* m_pScene = nullptr;
    std::vector<GO_Component*> m_GOCs;
    std::unordered_map<std::uint32_t, GO_Component*> m_ComponentCache;
    bool m_DontDestroyOnLoad = false;
    int m_LayerIndex = 0;
    int m_DrawOrder = 0;
    bool m_isEnabled = true;
};