#include "pch.h"
#include "ECSTestApp.h"

ECSTestApp::ECSTestApp()
{
	m_pECS = nullptr;
}

ECSTestApp::~ECSTestApp()
{
}

void ECSTestApp::OnStart()
{
	m_pECS = new seecs::ECS;
}

void ECSTestApp::Update()
{
}

void ECSTestApp::Draw()
{
}

void ECSTestApp::OnDestroy()
{
	delete m_pECS;
}