#include "NOSGPUBuffer.h"
#include "RenderingThread.h"

bool UNOSGPUBuffer::IsCreated() const
{
	return Buffer.Buffer.IsValid();
}

size_t UNOSGPUBuffer::GetBufferSize() const
{
	return Buffer.NumBytes;
}

void UNOSGPUBuffer::AllocateBlocking(size_t SizeInBytes, const TCHAR* DebugName, EBufferUsageFlags UsageFlags, EPixelFormat Format, ERHIAccess ResourceState)
{
	// Initialize the buffer on the render thread
	ENQUEUE_RENDER_COMMAND(InitializeNOSBuffer)(
		[&](FRHICommandListImmediate& RHICmdList)
		{
			Buffer.Release();
			Buffer.Initialize(
				RHICmdList,
				DebugName,
				1,
				SizeInBytes,
				Format,
				ResourceState,
				UsageFlags
			);
		});
	FlushRenderingCommands();
}
