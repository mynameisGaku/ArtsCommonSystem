#pragma once
#include "IObject.h"

class GameObject;

class GO_Component : public IObject
{
public:
    virtual ~GO_Component()
    {
    }

    void Awake() override
    {
    }

    void Start() override
    {
    }

    void Update(float deltaTime) override
    {
        (void)deltaTime;
    }

    void FixedUpdate(float fixedDeltaTime) override
    {
        (void)fixedDeltaTime;
    }

    void Draw(float deltaTime) override
    {
        (void)deltaTime;
    }

    void Destroy() override
    {
    }

    GameObject* GetGameObject() const
    {
        return m_Owner;
    }

    bool IsEnabled() const
    {
        return m_Enabled;
    }

    void SetEnabled(bool enabled)
    {
        m_Enabled = enabled;
    }

    void SetLayer(int order)
    {
        m_drawOrder = order;
	}

	int GetLayerIndex() const
    {
        return m_drawOrder;
	}

protected:
    friend class GameObject;
    friend class GOC_TransformUpdater;

    void SetOwner(GameObject* owner)
    {
        m_Owner = owner;
    }

private:
    GameObject* m_Owner = nullptr;
    bool m_Enabled = true;
	int m_drawOrder = 0;
};
