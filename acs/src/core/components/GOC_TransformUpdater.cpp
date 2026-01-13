#include "GOC_TransformUpdater.h"

void GOC_TransformUpdater::Update(float dt)
{
	auto* trs = m_Owner->GetGOC<Transform>();
	m_Owner->GetBlackboard().Set(
		BlackboardKeys::TransformWorldTRS,
		TransformTRS{
			trs->GetWorldPosition(),
			trs->GetWorldRotation(),
			trs->GetWorldScale(),
			trs->GetForwardVector(),
			trs->GetUpVector()
		}
	);
}
