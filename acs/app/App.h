#pragma once
#include <Pch.h>

class App final : public IApplication
{
public:
	void OnStart() override;
	void OnUpdate() override;
	void OnDraw() override;
	void OnDestroy() override;
private:

};

REGISTER_APPLICATION(App)
SET_DEFAULT_APPLICATION(App)