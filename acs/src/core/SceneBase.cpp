#include <Pch.h>
#include "SceneBase.h"

SceneBase::SceneBase()
{
    m_NewObjects = new std::vector<GameObject*>();
    m_Objects = new std::vector<GameObject*>();
    m_DestroyQueue = new std::vector<GameObject*>();
    m_pContext = new SceneContext;
}

SceneBase::~SceneBase()
{
    delete m_NewObjects;
    m_NewObjects = nullptr;
    delete m_Objects;
    m_Objects = nullptr;
    delete m_DestroyQueue;
    m_DestroyQueue = nullptr;
    delete m_pContext;
    m_pContext = nullptr;
}

void SceneBase::Initialize()
{
    m_Started = false;

    m_pLayerManager = new LayerManager();
    m_pLayerManager->AddLayer("Sky", 0);
    m_pLayerManager->AddLayer("Default", 1);
    m_pLayerManager->AddLayer("UI", 2);

    // Scene default setup
    {
        GameObject* skyObj = CreateGameObject<GameObject>();
        GameObject* cameraObj = CreateGameObject<GameObject>();

        // Camera default setup
        {
            cameraObj->SetName("Main Camera");
            auto* cam = cameraObj->AddGOC<GOC_Camera>();
            cam->SetNearFar(0.1f, 10000000.0f);
            cam->SetFov(75.0f);
        }

        // Sky default setup
        {
            skyObj->SetLayer("Sky");
            skyObj->SetName("Default Sky");
            auto* sky = skyObj->AddGOC<GOC_Sky>();

            GOC_Sky::SetupParam sp;
            sp.Radius = 50000.0f;
            sp.PixelShaderPath = "assets/shader/SkyPS.pso";
            sky->Setup(sp);

            auto& env = sky->GetSettings();
            env.CurrentTime = 0.3f;
            env.TimeSpeed = 0.05f;
            env.CloudDensity = 0.5f;
            env.SunSize = 0.001f;
        }

        // Set default position
        {
            auto* cameraTrs = cameraObj->GetGOC<Transform>();
            cameraTrs->SetLocalRotation(ACSU_Math::Quaternion::Euler(ACSU_Math::Vector3(0.0f, 90.0f, 0.0f)));
            auto* skyTrs = skyObj->GetGOC<Transform>();
            skyTrs->SetLocalPosition(cameraTrs->GetWorldPosition());
        }
    }

    if (not m_Awaked)
    {
        OnAwake();
        m_Awaked = true;
    }

    OnStart();

    EnsureDefaultCamera();

    CallAwakeForNewObjects();
    CallStartOnce();
}

void SceneBase::Update(float deltaTime)
{
    CallAwakeForNewObjects();

    if (!m_Started)
    {
        CallStartOnce();
    }

    OnPreUpdate(deltaTime);

    for (GameObject* obj : (*m_Objects))
    {
        if (obj != nullptr)
        {
            obj->Update(deltaTime);
        }
    }

    OnPostUpdate(deltaTime);

    FlushDestroyQueue();
}

void SceneBase::FixedUpdate(float fixedDeltaTime)
{
    OnPreFixedUpdate(fixedDeltaTime);

    for (GameObject* obj : (*m_Objects))
    {
        if (obj != nullptr)
        {
            obj->FixedUpdate(fixedDeltaTime);
        }
    }

    OnPostFixedUpdate(fixedDeltaTime);
}

void SceneBase::Render(float deltaTime)
{
    OnPreRender(deltaTime);

    // 描画順にソート
    std::sort(
        (*m_Objects).begin(),
        (*m_Objects).end(),
        [](GameObject* a, GameObject* b)
        {
            return a->GetLayerIndex() < b->GetLayerIndex();
        });

    for (GameObject* obj : (*m_Objects))
    {
        if (obj != nullptr)
        {
            obj->Draw(deltaTime);
        }
    }

    OnPostRender(deltaTime);
}

void SceneBase::Destroy()
{
    OnDestroy();

    for (GameObject* obj : (*m_Objects))
    {
        if (obj != nullptr)
        {
            obj->Destroy();
            delete obj;
        }
    }

    (*m_Objects).clear();
    (*m_NewObjects).clear();
    (*m_DestroyQueue).clear();

    delete m_pLayerManager;

    m_pLayerManager = nullptr;
    m_pMainCamera = nullptr;
    m_Started = false;
}

void SceneBase::DestroyGameObject(GameObject* obj)
{
    if (obj == nullptr)
    {
        return;
    }

    if (std::find((*m_DestroyQueue).begin(), (*m_DestroyQueue).end(), obj) != (*m_DestroyQueue).end())
    {
        return;
    }

    (*m_DestroyQueue).push_back(obj);
}

GameObject* SceneBase::FindGameObject(const std::string& name)
{
    if (m_Objects)
    {
        for (GameObject* obj : (*m_Objects))
        {
            if (obj->GetName() == name)
            {
                return obj;
            }
        }
    }

    if (m_NewObjects)
    {
        for (GameObject* obj : (*m_NewObjects))
        {
            if (obj->GetName() == name)
            {
                return obj;
            }
        }
    }

    return nullptr;
}

GOC_Camera* SceneBase::GetMainCamera() const
{
    return m_pMainCamera;
}

std::vector<GameObject*> SceneBase::CollectDontDestroyOnLoad()
{
    std::vector<GameObject*> result;

    for (GameObject* obj : (*m_Objects))
    {
        if (obj != nullptr && obj->IsDontDestroyOnLoad())
        {
            result.push_back(obj);
        }
    }

    // 次シーンに渡すので、このシーン側の所有から外す
    for (GameObject* keep : result)
    {
        ErasePtr((*m_Objects), keep);
    }

    return result;
}

void SceneBase::InjectDontDestroyOnLoad(const std::vector<GameObject*>& objs)
{
    for (GameObject* obj : objs)
    {
        if (obj != nullptr)
        {
            (*m_Objects).push_back(obj);
        }
    }
}

int SceneBase::GetLayerIndex(const std::string& layerName)
{
    return m_pLayerManager->GetLayerValue(layerName);
}

SceneContext& SceneBase::GetContext()
{
    return *m_pContext;
}

const SceneContext& SceneBase::GetContext() const
{
    return *m_pContext;
}

// Protected Virtual Methods
void SceneBase::OnAwake() {}
void SceneBase::OnStart() {}
void SceneBase::OnPreUpdate(float deltaTime) {}
void SceneBase::OnPostUpdate(float deltaTime) {}
void SceneBase::OnPreFixedUpdate(float fixedDeltaTime) {}
void SceneBase::OnPostFixedUpdate(float fixedDeltaTime) {}
void SceneBase::OnPreRender(float deltaTime) {}
void SceneBase::OnPostRender(float deltaTime) {}
void SceneBase::OnDestroy() {}

// Private Helper Methods
void SceneBase::EnsureDefaultCamera()
{
    if (FindAnyCamera() != nullptr)
    {
        if (m_pMainCamera == nullptr)
        {
            m_pMainCamera = FindAnyCamera();
            m_pMainCamera->SetMain(true);
        }
        return;
    }

    GameObject* cameraObj = CreateGameObject<GameObject>();
    GOC_Camera* cam = cameraObj->AddGOC<GOC_Camera>();
    cam->SetMain(true);

    m_pMainCamera = cam;
}

GOC_Camera* SceneBase::FindAnyCamera()
{
    for (GameObject* obj : (*m_Objects))
    {
        if (obj == nullptr)
        {
            continue;
        }

        GOC_Camera* cam = obj->GetGOC<GOC_Camera>();
        if (cam != nullptr)
        {
            return cam;
        }
    }

    for (GameObject* obj : (*m_NewObjects))
    {
        if (obj == nullptr)
        {
            continue;
        }

        GOC_Camera* cam = obj->GetGOC<GOC_Camera>();
        if (cam != nullptr)
        {
            return cam;
        }
    }

    return nullptr;
}

void SceneBase::CallAwakeForNewObjects()
{
    if ((*m_NewObjects).empty())
    {
        return;
    }

    for (GameObject* obj : (*m_NewObjects))
    {
        (*m_Objects).push_back(obj);
        obj->Awake();
    }

    (*m_NewObjects).clear();
}

void SceneBase::CallStartOnce()
{
    for (GameObject* obj : (*m_Objects))
    {
        if (obj != nullptr)
        {
            obj->Start();
        }
    }

    m_Started = true;
}

void SceneBase::FlushDestroyQueue()
{
    if ((*m_DestroyQueue).empty())
    {
        return;
    }

    for (GameObject* obj : (*m_DestroyQueue))
    {
        if (obj == nullptr)
        {
            continue;
        }

        // まだ所有している場合のみ破棄
        if (ErasePtr((*m_Objects), obj))
        {
            obj->Destroy();
            delete obj;
        }
    }

    (*m_DestroyQueue).clear();
}

bool SceneBase::ErasePtr(std::vector<GameObject*>& v, GameObject* ptr)
{
    auto it = std::find(v.begin(), v.end(), ptr);
    if (it == v.end())
    {
        return false;
    }

    v.erase(it);
    return true;
}