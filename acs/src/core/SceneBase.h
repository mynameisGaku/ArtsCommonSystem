#pragma once

#include <Pch.h>
#include "components/GOC_Camera.h"
#include "components/GOC_Sky.h"

class SceneBase : public IScene
{
public:

    SceneBase()
    {
        m_NewObjects = new std::vector<GameObject*>();
        m_Objects = new std::vector<GameObject*>();
        m_DestroyQueue = new std::vector<GameObject*>();
    }

    virtual ~SceneBase()
    {
        delete m_NewObjects;
        m_NewObjects = nullptr;
        delete m_Objects;
        m_Objects = nullptr;
        delete m_DestroyQueue;
        m_DestroyQueue = nullptr;
    }

    void Initialize() final
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
                auto* cam = cameraObj->AddGOC<GOC_Camera>();
                cam->SetNearFar(0.1f, 10000000.0f);
                cam->SetFov(75.0f);
            }

            // Sky default setup
            {
                skyObj->SetLayer("Sky");
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

    void Update(float deltaTime) final
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

    void FixedUpdate(float fixedDeltaTime) final
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

    void Render(float deltaTime) final
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

    void Destroy() final
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

public:
    template<class T, class... Args>
    T* CreateGameObject(Args&&... args)
    {
        static_assert(std::is_base_of_v<GameObject, T>);

        T* obj = new T(std::forward<Args>(args)...);
        obj->SetScene(this);
        obj->SetLayer("Default");
        (*m_NewObjects).push_back(obj);
        return obj;
    }

    void DestroyGameObject(GameObject* obj)
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

    GOC_Camera* GetMainCamera() const
    {
        return m_pMainCamera;
    }

    std::vector<GameObject*> CollectDontDestroyOnLoad()
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

    void InjectDontDestroyOnLoad(const std::vector<GameObject*>& objs)
    {
        for (GameObject* obj : objs)
        {
            if (obj != nullptr)
            {
                (*m_Objects).push_back(obj);
            }
        }
    }

    int GetLayerIndex(const std::string& layerName)
    {
        return m_pLayerManager->GetLayerValue(layerName);
    }

protected:

    /// <summary>
    /// 生成した最初にのみ呼ばだされるオーバーライドできる仮想メソッド。
    /// </summary>
    virtual void OnAwake()
    {
    }

    /// <summary>
    /// 初期化処理を行うためにオーバーライドできる仮想メソッド。
    /// </summary>
    virtual void OnStart()
    {
    }

    /// <summary>
    /// 更新処理の前に呼び出される仮想メソッド。
    /// </summary>
    /// <param name="deltaTime">前フレームからの経過時間（秒）。</param>
    virtual void OnPreUpdate(float deltaTime)
    {
	}

    /// <summary>
    /// 更新処理の直後に呼び出される、オーバーライド可能な仮想メソッド。
    /// </summary>
    /// <param name="deltaTime">前回の更新からの経過時間（通常は秒単位）。</param>
    virtual void OnPostUpdate(float deltaTime)
    {
	}

    /// <summary>
    /// 固定更新の前に呼び出される仮想メソッド。前処理や初期化のためにオーバーライドして使用します。
    /// </summary>
    /// <param name="fixedDeltaTime">この固定更新フレームの経過時間（秒）。物理演算や時間依存処理の更新間隔を表します。</param>
    virtual void OnPreFixedUpdate(float fixedDeltaTime)
    {
    }

    /// <summary>
    /// 固定更新（FixedUpdate）の処理後に呼び出される仮想メソッド。オーバーライドしてポスト固定更新の処理を実装するためのコールバック。
    /// </summary>
    /// <param name="fixedDeltaTime">直近の固定更新サイクルの経過時間（秒）。float 型で渡される固定デルタタイム。</param>
    virtual void OnPostFixedUpdate(float fixedDeltaTime)
    {
    }

    /// <summary>
    /// レンダリング直前に呼び出される仮想メソッド。サブクラスでオーバーライドして、レンダリング前の更新処理を行うために使用します。
    /// </summary>
    /// <param name="deltaTime">前フレームからの経過時間（秒）。フレームごとの時間差に基づく処理に使用します。</param>
    virtual void OnPreRender(float deltaTime)
    {
	}

    /// <summary>
    /// レンダリング完了後に呼び出される仮想メソッド。ポストレンダリング処理を実装するためにオーバーライドします。
    /// </summary>
    /// <param name="deltaTime">前フレームからの経過時間（秒）。フレーム間の時間差を表します。</param>
    virtual void OnPostRender(float deltaTime)
    {
	}

    /// <summary>
    /// オーバーライド可能な仮想メソッドで、オブジェクトやコンポーネントが破棄されるときに呼び出されます。既定の実装は何も行わないため、派生クラスでリソース解放やクリーンアップ処理を実装するためにオーバーライドします。
    /// </summary>
    virtual void OnDestroy()
    {
    }

private:
    /// <summary>
    /// シーンにメインカメラが存在しない場合、既存のカメラをメインに設定するか、新しいカメラオブジェクトと CameraComponent を生成してメインカメラを設定します。
    /// </summary>
    void EnsureDefaultCamera()
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

        // 必要なら初期位置
        // cameraObj->GetTransform().SetLocalPosition(...);
    }

    GOC_Camera* FindAnyCamera()
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

    void CallAwakeForNewObjects()
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

    void CallStartOnce()
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

    void FlushDestroyQueue()
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

    static bool ErasePtr(std::vector<GameObject*>& v, GameObject* ptr)
    {
        auto it = std::find(v.begin(), v.end(), ptr);
        if (it == v.end())
        {
            return false;
        }

        v.erase(it);
        return true;
    }

private:
    std::vector<GameObject*>* m_Objects;
    std::vector<GameObject*>* m_NewObjects;
    std::vector<GameObject*>* m_DestroyQueue;

    LayerManager* m_pLayerManager = nullptr;

    GOC_Camera* m_pMainCamera = nullptr;
    bool m_Started = false;
    bool m_Awaked = false;
};
