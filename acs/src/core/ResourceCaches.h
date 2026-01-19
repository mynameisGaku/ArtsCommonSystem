#pragma once
#include <Pch.h>

enum ResourceType
{
	INVALID = -1,
	GRAPH,
	DIV_GRAPH,
	SOUND,
	MODEL,
};

class ResourceCache
{
public:
	ResourceCache(int handle, const ResourceType& resType)
		: hHandle(handle), type(resType) {
	}

	ResourceCache(const std::vector<int>& handles, const ResourceType& resType)
		: hHandles(handles), type(resType) {
	}

	~ResourceCache()
	{
		if (hHandle != -1)
		{
			switch (type)
			{
			case GRAPH:
				DxLib::DeleteGraph(hHandle);
				break;
			case SOUND:
				DxLib::DeleteSoundMem(hHandle);
				break;
			case MODEL:
				DxLib::MV1DeleteModel(hHandle);
				break;
			default:
				break;
			}
		}

		if (type == DIV_GRAPH && !hHandles.empty())
		{
			for (int h : hHandles)
			{
				DxLib::DeleteGraph(h);
			}
			hHandles.clear();
		}
	}

	bool WasLoaded() const { return hHandle != -1 || !hHandles.empty(); }

	int Handle() const { return hHandle; }

	const std::vector<int>& Handles() const { return hHandles; }

private:
	int hHandle = -1;
	std::vector<int> hHandles;
	ResourceType type{ INVALID };
};