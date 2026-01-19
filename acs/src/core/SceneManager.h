#pragma once

#include <Pch.h>
#include "IScene.h"
#include "SceneBase.h"

class SceneManager
{
private:
    static SceneManager* m_pInstance;

public:
    static SceneManager* GetInstance()
    {
        if (m_pInstance == nullptr)
        {
            m_pInstance = new SceneManager();
        }
        return m_pInstance;
    }

    void Destroy()
    {
        if (m_pCurrentScene != nullptr)
        {
            m_pCurrentScene->Destroy();
            m_pCurrentScene = nullptr;
        }

        for (auto& pair : m_Scenes)
        {
            delete pair.second;
        }
        m_Scenes.clear();

        delete m_pInstance;
        m_pInstance = nullptr;
    }

    template<typename Scene>
    void RegisterScene(const char* name)
    {
        static_assert(std::is_base_of<IScene, Scene>::value,
            "Scene registration failed. Please check the classes that the scene extends.");

        const std::string key = name;
        if (m_Scenes.find(key) != m_Scenes.end())
        {
            return;
        }

        SceneBase* scene = new Scene();
        m_Scenes[key] = scene;
    }

    void SetDefault(const char* name)
    {
        const std::string key = name;
        auto it = m_Scenes.find(key);
        if (it == m_Scenes.end())
        {
            return;
        }

        m_pCurrentScene = it->second;
        m_pCurrentScene->Initialize();
        m_NextSceneName = "";
    }

    void ChangeScene(const std::string& sceneName)
    {
        m_NextSceneName = sceneName;
    }

    IScene* GetCurrentScene() const
    {
        return m_pCurrentScene;
    }

    void Update()
    {
        if (m_NextSceneName != "")
        {
            std::vector<GameObject*> carry;

            if (m_pCurrentScene != nullptr)
            {
                if (auto base = dynamic_cast<SceneBase*>(m_pCurrentScene))
                {
                    carry = base->CollectDontDestroyOnLoad();
                }

                m_pCurrentScene->Destroy();
            }

            auto it = m_Scenes.find(m_NextSceneName);
            if (it != m_Scenes.end())
            {
                m_pCurrentScene = it->second;

                if (!carry.empty())
                {
                    if (auto base = dynamic_cast<SceneBase*>(m_pCurrentScene))
                    {
                        base->InjectDontDestroyOnLoad(carry);
                    }
                    else
                    {
                        for (GameObject* obj : carry)
                        {
                            if (obj != nullptr)
                            {
                                obj->Destroy();
                                delete obj;
                            }
                        }
                        carry.clear();
                    }
                }

                m_pCurrentScene->Initialize();
            }

            m_NextSceneName = "";
        }

        if (m_pCurrentScene != nullptr)
        {
            m_pCurrentScene->Update(ACSU_Time::DeltaTime());

            while (ACSU_Time::ConsumeFixedStep())
            {
                m_pCurrentScene->FixedUpdate(ACSU_Time::FixedDeltaTime());
            }
        }
    }

    void Draw()
    {
        if (m_pCurrentScene != nullptr)
        {
            m_pCurrentScene->Render(ACSU_Time::DeltaTime());
        }
    }

private:
    SceneManager()
    {
    }
private:
    std::string m_NextSceneName;
    SceneBase* m_pCurrentScene = nullptr;
    std::unordered_map<std::string, SceneBase*> m_Scenes;
};
