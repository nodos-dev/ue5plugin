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
	void AllocateBlocking(size_t SizeInBytes, const TCHAR* DebugName,
		EBufferUsageFlags UsageFlags = BUF_UnorderedAccess | BUF_ShaderResource,
		EPixelFormat Format = PF_R32_UINT,
		ERHIAccess ResourceState = ERHIAccess::UAVMask);
	void Release() { Buffer.Release(); }
	FBufferRHIRef const& GetUnderlyingBuffer() const { return Buffer.Buffer; }
	FRWBuffer Buffer;
};
