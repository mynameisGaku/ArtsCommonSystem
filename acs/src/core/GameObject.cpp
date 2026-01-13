#include "Pch.h"

void GameObject::SetLayer(const std::string& layerName)
{
	m_LayerIndex = m_pScene->GetLayerIndex(layerName);
}