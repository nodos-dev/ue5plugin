/*
* Copyright MediaZ Teknoloji A.S. All Rights Reserved.
 */

#pragma once
#include "RHIUtilities.h"
#include "NOSGPUBuffer.generated.h"

UCLASS(BlueprintType)
class NOSDATASTRUCTURES_API UNOSGPUBuffer : public UObject
{
	GENERATED_BODY()

public:
	FRWBuffer Buffer;
	size_t InitialSize = 100000;
};
