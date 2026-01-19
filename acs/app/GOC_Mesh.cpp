#include "GOC_Mesh.h"

void GOC_Mesh::Setup(const std::string& meshPath)
{
    m_hModel = ACS_LoadModel(meshPath);
}

void GOC_Mesh::Draw(float dt)
{
    GameObject* go = GetGameObject();
    if (go == nullptr) return;

    Transform* transform = go->GetGOC<Transform>();
    if (transform == nullptr) return;

    const Matrix4x4& worldMat4x4 = transform->GetWorldMatrix();

    MATRIX mat = worldMat4x4;

    MV1SetMatrix(m_hModel, mat);
    MV1DrawModel(m_hModel);
}