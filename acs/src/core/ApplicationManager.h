#pragma once

#include <Pch.h>
#include "IApplication.h"

class ApplicationManager
{
private:
    static ApplicationManager* m_pInstance;

public:
    static ApplicationManager* GetInstance()
    {
        if (m_pInstance == nullptr)
        {
            m_pInstance = new ApplicationManager();
        }
        return m_pInstance;
    }

    void Destroy()
    {
        for (auto& pair : m_Applications)
        {
            delete pair.second;
        }
        m_Applications.clear();
        delete m_pInstance;
        m_pInstance = nullptr;
    }

    template<typename App>
    void RegisterApplication(const char* name)
    {
        static_assert(std::is_base_of<IApplication, App>::value,
            "Application registration failed. Please check the classes that the application extends.");

        const std::string key = name;
        if (m_Applications.find(key) != m_Applications.end())
        {
            return;
        }

        IApplication* app = new App();
        m_Applications[key] = app;
    }

    void SetDefault(const char* name)
    {
        const std::string key = name;
        auto it = m_Applications.find(key);
        if (it == m_Applications.end())
        {
            return;
        }
        m_pCurrentApp = it->second;
    }

    IApplication* GetCurrentApplication() const
    {
        return m_pCurrentApp;
    }

private:
    std::unordered_map<std::string, IApplication*> m_Applications;
    IApplication* m_pCurrentApp = nullptr;
};
