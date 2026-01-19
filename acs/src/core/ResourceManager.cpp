#include "ResourceManager.h"

ResourceManager* ResourceManager::pInstance = nullptr;

ResourceManager::ResourceManager()
{
	SetReserve(200);
}

ResourceManager::~ResourceManager()
{
	ReleaseAllResource();
}

void ResourceManager::SetReserve(size_t size)
{
	m_caches.reserve(size);
}

int ResourceManager::LoadGraph(const std::string& path)
{
	auto it = m_caches.find(path);
	if (it != m_caches.end())
	{
		return it->second->Handle();
	}

	int handle = DxLib::LoadGraph(path.c_str());

	if (handle == -1)
	{
		return -1;
	}

	ResourceCache* cache = new ResourceCache(handle, ResourceType::GRAPH);
	m_caches[path] = cache;

	return handle;
}

int ResourceManager::LoadSound(const std::string& path)
{
	auto it = m_caches.find(path);
	if (it != m_caches.end())
	{
		return it->second->Handle();
	}

	int handle = DxLib::LoadSoundMem(path.c_str());
	if (handle == -1) return -1;

	ResourceCache* cache = new ResourceCache(handle, ResourceType::SOUND);
	m_caches[path] = cache;

	return handle;
}

int ResourceManager::LoadModel(const std::string& path)
{
	auto it = m_caches.find(path);
	if (it != m_caches.end())
	{
		return DxLib::MV1DuplicateModel(it->second->Handle());
	}

	int handle = DxLib::MV1LoadModel(path.c_str());
	if (handle == -1) return -1;

	ResourceCache* cache = new ResourceCache(handle, ResourceType::MODEL);
	m_caches[path] = cache;

	return DxLib::MV1DuplicateModel(handle);
}

int ResourceManager::LoadDivGraph(const std::string& path, int AllNum, int XNum, int YNum, int XSize, int YSize, int* HandleBuf)
{
	auto it = m_caches.find(path);
	if (it != m_caches.end())
	{
		const std::vector<int>& handles = it->second->Handles();

		for (int i = 0; i < AllNum && i < (int)handles.size(); ++i)
		{
			HandleBuf[i] = handles[i];
		}
		return 0;
	}

	std::vector<int> tempHandles(AllNum);

	int res = DxLib::LoadDivGraph(path.c_str(), AllNum, XNum, YNum, XSize, YSize, tempHandles.data());

	if (res == -1) return -1;

	ResourceCache* cache = new ResourceCache(tempHandles, ResourceType::DIV_GRAPH);
	m_caches[path] = cache;

	for (int i = 0; i < AllNum; ++i)
	{
		HandleBuf[i] = tempHandles[i];
	}

	return 0;
}

void ResourceManager::ReleaseAllResource()
{
	for (auto& pair : m_caches)
	{
		delete pair.second;
	}
	m_caches.clear();
}

void ResourceManager::ReleaseResource(const std::string& path)
{
	auto it = m_caches.find(path);
	if (it != m_caches.end())
	{
		delete it->second;
		m_caches.erase(it);
	}
}