#pragma once
#include "ApplicationManager.h"

#define REGISTER_APPLICATION(Type) \
    namespace \
    { \
        struct ApplicationAutoRegistrar \
        { \
            ApplicationAutoRegistrar() \
            { \
                ApplicationManager::GetInstance()->RegisterApplication<Type>(#Type); \
            } \
            ~ApplicationAutoRegistrar() {} \
        }; \
        static ApplicationAutoRegistrar GAutoReg_##Type; \
    }

#define SET_DEFAULT_APPLICATION(Type) \
    namespace \
    { \
        struct ApplicationDefaultSetter \
        { \
            ApplicationDefaultSetter() \
            { \
                ApplicationManager::GetInstance()->SetDefault(#Type); \
            } \
        }; \
        static ApplicationDefaultSetter GDefaultSetter; \
    }