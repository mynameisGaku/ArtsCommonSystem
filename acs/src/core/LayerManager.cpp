#include "Pch.h"
#include "LayerManager.h"

LayerManager::LayerManager()
{
    m_Layers.clear();
}

LayerManager::~LayerManager()
{
    m_Layers.clear();
}

void LayerManager::AddLayer(const std::string& layerName, int index)
{
    auto it = m_Layers.find(layerName);
    if (it == m_Layers.end())
    {
        m_Layers[layerName] = index;
    }
    if (index > m_LatestIndex)
    {
        m_LatestIndex = index;
    }
}

int LayerManager::GetLayerValue(const std::string& layerName)
{
    if (m_Layers.find(layerName) == m_Layers.end())
    {
        AddLayer(layerName, m_LatestIndex + 1);
    }
    return m_Layers[layerName];
}
