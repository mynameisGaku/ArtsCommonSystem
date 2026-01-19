#pragma once

#include <Pch.h>

class GOC_Camera;

class SceneBase : public IScene
{
public:
    SceneBase();
    virtual ~SceneBase();

    void Initialize() final;
    void Update(float deltaTime) final;
    void FixedUpdate(float fixedDeltaTime) final;
    void Render(float deltaTime) final;
    void Destroy() final;

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

    void DestroyGameObject(GameObject* obj);
    GameObject* FindGameObject(const std::string& name);

    template <class T>
    T* FindObjectOfType()
    {
        if (m_Objects)
        {
            for (GameObject* obj : (*m_Objects))
            {
                if (!obj->IsEnabled()) continue;

                T* component = obj->GetGOC<T>();
                if (component != nullptr)
                {
                    return component;
                }
            }
        }

        if (m_NewObjects)
        {
            for (GameObject* obj : (*m_NewObjects))
            {
                if (!obj->IsEnabled()) continue;

                T* component = obj->GetGOC<T>();
                if (component != nullptr)
                {
                    return component;
                }
            }
        }

        return nullptr;
    }

    template <class T>
    std::vector<T*> FindObjectsOfType()
    {
        std::vector<T*> results;

        auto searchList = [&](const std::vector<GameObject*>* list)
            {
                if (!list) return;
                for (GameObject* obj : (*list))
                {
                    if (!obj->IsEnabled()) continue;

                    T* component = obj->GetGOC<T>();
                    if (component != nullptr)
                    {
                        results.push_back(component);
                    }
                }
            };

        searchList(m_Objects);
        searchList(m_NewObjects);

        return results;
    }

    GOC_Camera* GetMainCamera() const;
    std::vector<GameObject*> CollectDontDestroyOnLoad();
    void InjectDontDestroyOnLoad(const std::vector<GameObject*>& objs);

    int GetLayerIndex(const std::string& layerName);

    SceneContext& GetContext();
    const SceneContext& GetContext() const;

protected:
    /// <summary>
    /// 生成した最初にのみ呼ばだされるオーバーライドできる仮想メソッド。
    /// </summary>
    virtual void OnAwake();

    /// <summary>
    /// 初期化処理を行うためにオーバーライドできる仮想メソッド。
    /// </summary>
    virtual void OnStart();

    /// <summary>
    /// 更新処理の前に呼び出される仮想メソッド。
    /// </summary>
    virtual void OnPreUpdate(float deltaTime);

    /// <summary>
    /// 更新処理の直後に呼び出される、オーバーライド可能な仮想メソッド。
    /// </summary>
    virtual void OnPostUpdate(float deltaTime);

    /// <summary>
    /// 固定更新の前に呼び出される仮想メソッド。前処理や初期化のためにオーバーライドして使用します。
    /// </summary>
    virtual void OnPreFixedUpdate(float fixedDeltaTime);

    /// <summary>
    /// 固定更新（FixedUpdate）の処理後に呼び出される仮想メソッド。オーバーライドしてポスト固定更新の処理を実装するためのコールバック。
    /// </summary>
    virtual void OnPostFixedUpdate(float fixedDeltaTime);

    /// <summary>
    /// レンダリング直前に呼び出される仮想メソッド。サブクラスでオーバーライドして、レンダリング前の更新処理を行うために使用します。
    /// </summary>
    virtual void OnPreRender(float deltaTime);

    /// <summary>
    /// レンダリング完了後に呼び出される仮想メソッド。ポストレンダリング処理を実装するためにオーバーライドします。
    /// </summary>
    virtual void OnPostRender(float deltaTime);

    /// <summary>
    /// オーバーライド可能な仮想メソッドで、オブジェクトやコンポーネントが破棄されるときに呼び出されます。
    /// </summary>
    virtual void OnDestroy();

private:
    /// <summary>
    /// シーンにメインカメラが存在しない場合、既存のカメラをメインに設定するか、新しいカメラオブジェクトと CameraComponent を生成してメインカメラを設定します。
    /// </summary>
    void EnsureDefaultCamera();
    GOC_Camera* FindAnyCamera();
    void CallAwakeForNewObjects();
    void CallStartOnce();
    void FlushDestroyQueue();
    static bool ErasePtr(std::vector<GameObject*>& v, GameObject* ptr);

private:
    std::vector<GameObject*>* m_Objects;
    std::vector<GameObject*>* m_NewObjects;
    std::vector<GameObject*>* m_DestroyQueue;

    SceneContext* m_pContext;

    LayerManager* m_pLayerManager = nullptr;

    GOC_Camera* m_pMainCamera = nullptr;
    bool m_Started = false;
    bool m_Awaked = false;
};