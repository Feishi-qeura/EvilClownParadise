#include "Characters/ECPPlayerBase.h"
#include "Components/SkeletalMeshComponent.h"

void AECPPlayerBase::ClearTeammateFocus()
{
	if (USkeletalMeshComponent* FocusMesh=FocusedTeammateMesh.Get())
	{
		// 其他系统若已接管叠加材质，不覆盖它的新表现。
		if (FocusMesh->GetOverlayMaterial()==TeammateOutlineMaterial) FocusMesh->SetOverlayMaterial(OriginalTeammateOverlay);
	}
	FocusedTeammateMesh.Reset();
	OriginalTeammateOverlay=nullptr;
}

void AECPPlayerBase::UpdateTeammateFocus()
{
	FHitResult Hit;
	AECPCharBase* Target=IsLocallyControlled() && TracePickup(Hit) ? Cast<AECPCharBase>(Hit.GetActor()) : nullptr;
	USkeletalMeshComponent* FocusMesh=Target && Target->CanBeCarriedBy(this) && TeammateOutlineMaterial ? Target->GetMesh() : nullptr;
	if (FocusedTeammateMesh.Get()==FocusMesh) return;
	ClearTeammateFocus();
	if (FocusMesh)
	{
		FocusedTeammateMesh=FocusMesh;
		OriginalTeammateOverlay=FocusMesh->GetOverlayMaterial();
		FocusMesh->SetOverlayMaterial(TeammateOutlineMaterial);
	}
}
