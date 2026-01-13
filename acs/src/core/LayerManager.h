#pragma once

#include <Pch.h>

class LayerManager
{
public:
	LayerManager();
	~LayerManager();

	void AddLayer(const std::string& layerName, int index);
	int GetLayerValue(const std::string& layerName);

private:
	std::unordered_map<std::string, int> m_Layers;
	int m_LatestIndex;
};