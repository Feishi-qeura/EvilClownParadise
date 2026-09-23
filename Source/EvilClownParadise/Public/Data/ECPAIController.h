#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "ECPAIController.generated.h"

/**
 * 
 */
UCLASS()
class EVILCLOWNPARADISE_API AECPAIController : public AAIController
{
	GENERATED_BODY()
public:
	AECPAIController();


protected:
	virtual void BeginPlay() override;
	
	virtual void PostInitializeComponents() override;
	
	UPROPERTY(EditDefaultsOnly, Category="ECP|AI")
	TObjectPtr<class UStateTree> DefaultStateTree;
	
	UPROPERTY(EditAnywhere)
	TObjectPtr<class UAISenseConfig_Sight> SightConfig;
	
private:
	UFUNCTION()
	void OnTargetPerceptionUpdated(AActor* Actor, struct FAIStimulus Stimulus);
	
	UPROPERTY(VisibleAnywhere, Category = "ECP|AI")
	TObjectPtr<class UStateTreeAIComponent> StateTreeAIComp;
	
	UPROPERTY(VisibleAnywhere, Category = "ECP|AI")
	TObjectPtr<class UAIPerceptionComponent> AIPerceptionComp;
};