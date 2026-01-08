#pragma once
#include "IObject.h"

class GameObject;

class Component : public IObject
{
public:
    virtual ~Component()
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

    void SetDrawOrder(int order)
    {
        m_drawOrder = order;
	}

	int GetDrawOrder() const
    {
        return m_drawOrder;
	}

protected:
    friend class GameObject;

    void SetOwner(GameObject* owner)
    {
        m_Owner = owner;
    }

private:
    GameObject* m_Owner = nullptr;
    bool m_Enabled = true;
	int m_drawOrder = 0;
};
