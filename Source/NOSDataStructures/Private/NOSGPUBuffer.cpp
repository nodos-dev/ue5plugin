#include "NOSGPUBuffer.h"

bool UNOSGPUBuffer::IsCreated() const
{
	return Buffer.Buffer.IsValid();
}

size_t UNOSGPUBuffer::GetBufferSize() const
{
	return Buffer.NumBytes;
}
