#pragma once

#include <Pch.h>
#include <string>
#include <unordered_map>
#include "ResourceCaches.h"

class ResourceManager
{
private:
	static ResourceManager* pInstance;

	ResourceManager();
	~ResourceManager();

public:
	ResourceManager(const ResourceManager&) = delete;
	ResourceManager& operator=(const ResourceManager&) = delete;

	static ResourceManager* GetInstance()
	{
		if (pInstance == nullptr)
		{
			pInstance = new ResourceManager();
		}
		return pInstance;
	}

	void Destroy()
	{
		if (pInstance != nullptr)
		{
			ReleaseAllResource();
			delete pInstance;
			pInstance = nullptr;
		}
	}

	void SetReserve(size_t size);

	int LoadGraph(const std::string& path);
	int LoadSound(const std::string& path);
	int LoadModel(const std::string& path);
	int LoadDivGraph(const std::string& path, int AllNum, int XNum, int YNum, int XSize, int YSize, int* HandleBuf);

	void ReleaseAllResource();
	void ReleaseResource(const std::string& path);

private:
	std::unordered_map<std::string, ResourceCache*> m_caches;
};

static int ACS_LoadGraph(const std::string& path)
{
	return ResourceManager::GetInstance()->LoadGraph(path);
}
static int ACS_LoadSound(const std::string& path)
{
	return ResourceManager::GetInstance()->LoadSound(path);
}
static int ACS_LoadModel(const std::string& path)
{
	return ResourceManager::GetInstance()->LoadModel(path);
}
static int ACS_LoadDivGraph(const std::string& path, int AllNum, int XNum, int YNum, int XSize, int YSize, int* HandleBuf)
{
	return ResourceManager::GetInstance()->LoadDivGraph(path, AllNum, XNum, YNum, XSize, YSize, HandleBuf);
}