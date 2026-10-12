#pragma once

#include "Commandlets/Commandlet.h"
#include "ECPProjectCleanupCommandlet.generated.h"

/** Offline editor utility for the reviewed Project content cleanup manifest. */
UCLASS()
class ECPFOUNDATIONEDITOR_API UECPProjectCleanupCommandlet : public UCommandlet
{
    GENERATED_BODY()
public:
    UECPProjectCleanupCommandlet();
    virtual int32 Main(const FString& Params) override;
};
