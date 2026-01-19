#include "GameObject.h"

GameObject::GameObject()
{
    AddGOC<Transform>();
}

GameObject::~GameObject()
{
    Destroy();
}

void GameObject::Awake()
{
    for (GO_Component* c : m_GOCs)
    {
        if (c->IsEnabled())
        {
            c->Awake();
        }
    }
}

void GameObject::Start()
{
    for (GO_Component* c : m_GOCs)
    {
        if (c->IsEnabled())
        {
            c->Start();
        }
    }
}

void GameObject::Update(float deltaTime)
{
    for (GO_Component* c : m_GOCs)
    {
        if (c->IsEnabled())
        {
            c->Update(deltaTime);
        }
    }
}

void GameObject::FixedUpdate(float fixedDeltaTime)
{
    for (GO_Component* c : m_GOCs)
    {
        if (c->IsEnabled())
        {
            c->FixedUpdate(fixedDeltaTime);
        }
    }
}

void GameObject::Draw(float deltaTime)
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

void GameObject::Destroy()
{
    for (GO_Component* c : m_GOCs)
    {
        c->Destroy();
        delete c;
    }

    m_GOCs.clear();
    m_ComponentCache.clear();
}

void GameObject::DontDestroyOnLoad()
{
    m_DontDestroyOnLoad = true;
}

bool GameObject::IsDontDestroyOnLoad() const
{
    return m_DontDestroyOnLoad;
}

void GameObject::SetLayer(const std::string& layerName)
{
    if (m_pScene)
    {
        m_LayerIndex = m_pScene->GetLayerIndex(layerName);
    }
}

int GameObject::GetLayerIndex() const
{
    return m_LayerIndex;
}

void GameObject::SetScene(SceneBase* scene)
{
    m_pScene = scene;
}

SceneBase* GameObject::GetScene() const
{
    return m_pScene;
}

SceneContext& GameObject::GetSceneContext()
{
    return m_pScene->GetContext();
}

void GameObject::SetName(const std::string& name)
{
    m_Name = name;
}

const std::string& GameObject::GetName() const
{
    return m_Name;
}

void GameObject::SetEnabled(bool enable)
{
    m_isEnabled = enable;
}

bool GameObject::IsEnabled() const
{
    return m_isEnabled;
}