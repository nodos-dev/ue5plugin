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
	bool IsCreated() const;
	size_t GetBufferSize() const;
	FRWBuffer Buffer;
	size_t RequestedSize = 10000;
};
