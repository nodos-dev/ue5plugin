// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include "NOSResourceShareManager.h"

#include "HardwareInfo.h"

#pragma warning (disable : 4800)
#pragma warning (disable : 4668)

#include "Windows/AllowWindowsPlatformTypes.h"
#pragma intrinsic(_InterlockedCompareExchange64)
#define InterlockedCompareExchange64 _InterlockedCompareExchange64
#include <d3d12.h>
#include "Windows/HideWindowsPlatformTypes.h"

#include "D3D12RHIPrivate.h"
#include "D3D12RHI.h"
#include "D3D12Resources.h"
#include "ID3D12DynamicRHI.h"
#include "D3D12CommandContext.h"
#include "RHI.h"
#include "NOSActorProperties.h"
#include "RHIResources.h"
#include "TextureResource.h"
#include "RenderGraphEvent.h"

#include "NOSClient.h"

#include <Builtins_generated.h>

#include "NOSGPUBuffer.h"
#include "NOSGPUFailSafe.h"

#include "nosVulkanSubsystem/nosVulkanSubsystem.h"

NOSResourceShareManager* NOSResourceShareManager::singleton;

//#define FAIL_SAFE_THREAD
//#define DEBUG_FRAME_SYNC_LOG
//#define DEBUG_NODOS_TEXTURE_COPIES

nosTextureInfo GetTextureInfo(NOSProperty* nosprop)
{
	UObject* obj = nosprop->GetRawObjectContainer();
	FObjectProperty* prop = CastField<FObjectProperty>(nosprop->Property);

	if (!obj)
	{
		return nosTextureInfo{
			.Width = 1600,
			.Height = 900,
			.Format = nosFormat::NOS_FORMAT_R16G16B16A16_SFLOAT,
			.Usage = (nosImageUsage)(NOS_IMAGE_USAGE_RENDER_TARGET | NOS_IMAGE_USAGE_SAMPLED | NOS_IMAGE_USAGE_TRANSFER_SRC | NOS_IMAGE_USAGE_TRANSFER_DST) 
		};
	}

	UTextureRenderTarget2D* trt2d = Cast<UTextureRenderTarget2D>(prop->GetObjectPropertyValue(prop->ContainerPtrToValuePtr<UTextureRenderTarget2D>(obj)));

	if (!trt2d)
	{
		return nosTextureInfo{
			.Width = 1600,
			.Height = 900,
			.Format = nosFormat::NOS_FORMAT_R16G16B16A16_SFLOAT,
			.Usage = (nosImageUsage)(NOS_IMAGE_USAGE_RENDER_TARGET | NOS_IMAGE_USAGE_SAMPLED | NOS_IMAGE_USAGE_TRANSFER_SRC | NOS_IMAGE_USAGE_TRANSFER_DST)
		};
	}

	nosTextureInfo info = {
		.Width = (uint32_t)trt2d->GetSurfaceWidth(),
		.Height = (uint32_t)trt2d->GetSurfaceHeight(),
		.Usage = (nosImageUsage)(NOS_IMAGE_USAGE_RENDER_TARGET | NOS_IMAGE_USAGE_SAMPLED | NOS_IMAGE_USAGE_TRANSFER_SRC | NOS_IMAGE_USAGE_TRANSFER_DST),
	};

	switch (trt2d->RenderTargetFormat)
	{
	case ETextureRenderTargetFormat::RTF_R8:
		info.Format = NOS_FORMAT_R8_UNORM;
		break;
	case ETextureRenderTargetFormat::RTF_RG8:
		info.Format = NOS_FORMAT_R8G8_UNORM;
		break;
	case ETextureRenderTargetFormat::RTF_RGBA8:
		info.Format = NOS_FORMAT_R8G8B8A8_UNORM;
		break;
	case ETextureRenderTargetFormat::RTF_RGBA8_SRGB:
		info.Format = NOS_FORMAT_R8G8B8A8_SRGB;
		break;

	case ETextureRenderTargetFormat::RTF_R16f:
		info.Format = NOS_FORMAT_R16_SFLOAT;
		break;
	case ETextureRenderTargetFormat::RTF_RG16f:
		info.Format = NOS_FORMAT_R16G16_SFLOAT;
		break;
	case ETextureRenderTargetFormat::RTF_RGBA16f:
		info.Format = NOS_FORMAT_R16G16B16A16_SFLOAT;
		break;

	case ETextureRenderTargetFormat::RTF_R32f:
		info.Format = NOS_FORMAT_R32_SFLOAT;
		break;
	case ETextureRenderTargetFormat::RTF_RG32f:
		info.Format = NOS_FORMAT_R32G32_SFLOAT;
		break;
	case ETextureRenderTargetFormat::RTF_RGBA32f:
		info.Format = NOS_FORMAT_R32G32B32A32_SFLOAT;
		break;

	case ETextureRenderTargetFormat::RTF_RGB10A2:
		info.Format = NOS_FORMAT_A2R10G10B10_UNORM_PACK32;
		break;
	}

	return info;
}

nosBufferInfo GetBufferInfo(NOSProperty* nosprop)
{
	UObject* obj = nosprop->GetRawObjectContainer();
	FObjectProperty* prop = CastField<FObjectProperty>(nosprop->Property);

	nosBufferInfo info
	{
		.Size = 1000,
		.Usage = nosBufferUsage(NOS_BUFFER_USAGE_TRANSFER_DST | NOS_BUFFER_USAGE_TRANSFER_SRC | NOS_BUFFER_USAGE_STORAGE_BUFFER),
	};
	if (!obj)
	{
		return info;
	}

	UNOSGPUBuffer* buf = Cast<UNOSGPUBuffer>(prop->GetObjectPropertyValue(prop->ContainerPtrToValuePtr<UNOSGPUBuffer>(obj)));
	
	if (!obj)
	{
		return info;
	}

	info.Size = buf->RequestedSize;
	return info;
}

NOSResourceShareManager::NOSResourceShareManager()
{
	Initiate();
}

NOSResourceShareManager* NOSResourceShareManager::GetInstance()
{
	if (singleton == nullptr) {
		singleton = new NOSResourceShareManager();
	}
	return singleton;
}

NOSResourceShareManager::~NOSResourceShareManager()
{
}

nos::sys::vulkan::TTexture NOSResourceShareManager::AddTexturePin(NOSProperty* nosprop)
{
	auto copyInfo = MakeShared<SharedResourceInfo>();
	nos::sys::vulkan::TTexture texture;
	if(!CreateTextureResource(nosprop, texture, *copyInfo))
	{
		return texture;
	}
	
	
	{
		//start property pins as output pins
		Copies.Add(nosprop, std::move(copyInfo));
	}
	return texture;
}

nos::sys::vulkan::Buffer NOSResourceShareManager::AddBufferPin(NOSProperty* nosprop)
{
	auto copyInfo = MakeShared<SharedResourceInfo>();
	nos::sys::vulkan::Buffer buffer;
	if(!CreateBufferResource(nosprop, buffer, *copyInfo))
	{
		return buffer;
	}
	
	{
		//start property pins as output pins
		Copies.Add(nosprop, std::move(copyInfo));
	}
	return buffer;
}

bool NOSResourceShareManager::CreateTextureResource(NOSProperty* nosprop, nos::sys::vulkan::TTexture& Texture, SharedResourceInfo& Resource)
	{
	nosTextureInfo info = GetTextureInfo(nosprop);
	UObject* obj = nosprop->GetRawObjectContainer();
	FObjectProperty* prop = CastField<FObjectProperty>(nosprop->Property);
	UTextureRenderTarget2D* trt2d = Cast<UTextureRenderTarget2D>(prop->GetObjectPropertyValue(prop->ContainerPtrToValuePtr<UTextureRenderTarget2D>(obj)));
	if(!trt2d)
	{
		nosprop->IsOrphan = true;
		nosprop->OrphanMessage = "No texture resource bound to property!";
		return false;
	}
	
	UTextureRenderTarget2D* NewRenderTarget2D = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), *(nosprop->DisplayName +FGuid::NewGuid().ToString()), RF_MarkAsRootSet);
	check(NewRenderTarget2D);
	NewRenderTarget2D->Rename(*(nosprop->DisplayName +FGuid::NewGuid().ToString()));
	NewRenderTarget2D->RenderTargetFormat = trt2d->RenderTargetFormat;
	NewRenderTarget2D->ClearColor = trt2d->ClearColor;
	NewRenderTarget2D->bAutoGenerateMips = 0;
	NewRenderTarget2D->bCanCreateUAV = true;
	NewRenderTarget2D->bGPUSharedFlag = true;
	NewRenderTarget2D->InitAutoFormat(info.Width, info.Height);	
	NewRenderTarget2D->UpdateResourceImmediate(true);
	FlushRenderingCommands();
	
	auto rt = NewRenderTarget2D->GameThread_GetRenderTargetResource();
	//if (!rt) return false;
	auto RHIResource = rt->GetTexture2DRHI();
	if (!RHIResource || !RHIResource->IsValid())
	{
		return false;
	}
	FRHITexture* RHITexture = RHIResource;
	FD3D12Texture* Result((FD3D12Texture*)RHITexture->GetTextureBaseRHI());
	
	ID3D12Resource* DXResource = Result->GetResource()->GetResource();
    DXResource->SetName(*nosprop->DisplayName);
	
	HANDLE handle = 0;
    NOS_D3D12_ASSERT_SUCCESS(Dev->CreateSharedHandle(DXResource, 0, GENERIC_ALL, 0, &handle));
	Texture.resolution = nos::sys::vulkan::SizePreset::CUSTOM;
	Texture.width = info.Width;
	Texture.height = info.Height;
	Texture.format = nos::sys::vulkan::Format(info.Format);
	Texture.usage = nos::sys::vulkan::ImageUsage(info.Usage) | nos::sys::vulkan::ImageUsage::SAMPLED;
	auto& Ext = Texture.external_memory;
	Ext.mutate_handle_type(NOS_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE);
	Ext.mutate_handle((uint64_t)handle);
	D3D12_RESOURCE_DESC desc = DXResource->GetDesc();
	Ext.mutate_allocation_size(Dev->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes);
	Ext.mutate_pid(FPlatformProcess::GetCurrentProcessId());
	Texture.unmanaged = true;
	Texture.unscaled = true;
	Texture.handle = 0;

	Resource.SrcNosp = nosprop;
	Resource.DstTexture = NewRenderTarget2D;
	Resource.ShowAs = nosprop->PinShowAs;
	Resource.SharedHandle = handle;
	return true;
}

bool NOSResourceShareManager::CreateBufferResource(NOSProperty* nosprop, nos::sys::vulkan::Buffer& Buffer, SharedResourceInfo& Resource)
{
	nosBufferInfo info = GetBufferInfo(nosprop);
	UObject* obj = nosprop->GetRawObjectContainer();
	FObjectProperty* prop = CastField<FObjectProperty>(nosprop->Property);
	UNOSGPUBuffer* SrcBuffer = Cast<UNOSGPUBuffer>(prop->GetObjectPropertyValue(prop->ContainerPtrToValuePtr<UNOSGPUBuffer>(obj)));
	if(!SrcBuffer)
	{
		nosprop->IsOrphan = true;
		nosprop->OrphanMessage = "No buffer resource bound to property!";
		return false;
	}

	if (!SrcBuffer->IsCreated())
	{
		// Initialize the buffer on the render thread
		ENQUEUE_RENDER_COMMAND(InitializeNOSBuffer)(
			[SrcBuffer, info, nosprop](FRHICommandListImmediate& RHICmdList)
			{
				// Initialize the new buffer with the same properties as the source
				// Add BUF_Shared flag to enable resource sharing across processes
				SrcBuffer->Buffer.Initialize(
					RHICmdList,
					*nosprop->DisplayName,
					1,
					info.Size,
					PF_R32_UINT, // Format
					ERHIAccess::UAVMask,
					BUF_UnorderedAccess | BUF_ShaderResource
				);
			});

		FlushRenderingCommands();
	}
	
	UNOSGPUBuffer* SharedBuffer = NewObject<UNOSGPUBuffer>(GetTransientPackage(), *(nosprop->DisplayName +FGuid::NewGuid().ToString()), RF_MarkAsRootSet);
	check(SharedBuffer);

	// Initialize the buffer on the render thread
	ENQUEUE_RENDER_COMMAND(InitializeNOSBuffer)(
		[SharedBuffer, info, nosprop](FRHICommandListImmediate& RHICmdList)
		{
			// Initialize the new buffer with the same properties as the source
			// Add BUF_Shared flag to enable resource sharing across processes
			SharedBuffer->Buffer.Initialize(
				RHICmdList,
				*nosprop->DisplayName,
				1,
				info.Size,
				PF_R32_UINT, // Format
				ERHIAccess::UAVMask,
				BUF_UnorderedAccess | BUF_ShaderResource | BUF_Shared
			);
		});

	FlushRenderingCommands();

	// Get the D3D12 resource from the RHI buffer
	FRHIBuffer* RHIBuffer = SharedBuffer->Buffer.Buffer;
	if (!RHIBuffer || !RHIBuffer->IsValid())
	{
		return false;
	}

	FD3D12Buffer* D3D12Buffer = static_cast<FD3D12Buffer*>(RHIBuffer);
	ID3D12Resource* DXResource = D3D12Buffer->GetResource()->GetResource();
	DXResource->SetName(*nosprop->DisplayName);

	// Create shared handle for the buffer
	HANDLE handle = 0;
	NOS_D3D12_ASSERT_SUCCESS(Dev->CreateSharedHandle(DXResource, 0, GENERIC_ALL, 0, &handle));

	// Set up the Vulkan buffer structure
	Buffer.mutate_size_in_bytes(info.Size);
	Buffer.mutate_usage(nos::sys::vulkan::BufferUsage(info.Usage));
	Buffer.mutate_field_type(nos::sys::vulkan::FieldType::PROGRESSIVE);
	auto& Ext = Buffer.mutable_external_memory();
	Ext.mutate_handle_type(NOS_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE);
	Ext.mutate_handle((uint64_t)handle);
	
	D3D12_RESOURCE_DESC desc = DXResource->GetDesc();
	Ext.mutate_allocation_size(Dev->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes);
	Ext.mutate_pid(FPlatformProcess::GetCurrentProcessId());

	// Set up the shared resource info
	Resource.SrcNosp = nosprop;
	Resource.DstBuffer = SharedBuffer;
	Resource.ShowAs = nosprop->PinShowAs;
	Resource.SharedHandle = handle;

	return true;
}


void NOSResourceShareManager::UpdateTexturePin(NOSProperty* nosprop, nos::fb::ShowAs RealShowAs)
{
	UpdatePinShowAs(nosprop, RealShowAs);
}

bool NOSResourceShareManager::UpdateTexturePin(NOSProperty* NosProperty, nos::sys::vulkan::TTexture& Texture)
{
	nosTextureInfo info = GetTextureInfo(NosProperty);

	auto resourceInfoPtrPtr = Copies.Find(NosProperty);
	if (resourceInfoPtrPtr == nullptr)
		return false;
	auto& resourceInfoSharedPtr = *resourceInfoPtrPtr;

	if (Texture.external_memory.pid() != (uint64_t)FPlatformProcess::GetCurrentProcessId())
		return false;

	bool changed = false;

	nos::sys::vulkan::Format fmt = nos::sys::vulkan::Format(info.Format);
	nos::sys::vulkan::ImageUsage usage = nos::sys::vulkan::ImageUsage(info.Usage) | nos::sys::vulkan::ImageUsage::SAMPLED;

	if (Texture.width != info.Width ||
		Texture.height != info.Height ||
		Texture.format != fmt ||
		Texture.usage != usage)
	{
		changed = true;
		
		nos::fb::ShowAs tmp = resourceInfoSharedPtr->ShowAs;
		ResourcesToDelete.Enqueue({ std::move(resourceInfoSharedPtr), GFrameCounter });

		resourceInfoSharedPtr = MakeShared<SharedResourceInfo>();
		if(!CreateTextureResource(NosProperty, Texture, *resourceInfoSharedPtr))
		{
			return changed;
		}
		
		resourceInfoSharedPtr->ShowAs = tmp;
	}

	return changed;
}

bool NOSResourceShareManager::UpdateBufferPin(NOSProperty* NosProperty, nos::sys::vulkan::Buffer& Buffer)
{
	nosBufferInfo info = GetBufferInfo(NosProperty);
	auto resourceInfoPtrPtr = Copies.Find(NosProperty);
	if (resourceInfoPtrPtr == nullptr)
		return false;
	auto& resourceInfoSharedPtr = *resourceInfoPtrPtr;
	if (Buffer.external_memory().pid() != (uint64_t)FPlatformProcess::GetCurrentProcessId())
		return false;
	bool changed = false;
	if (Buffer.size_in_bytes() != info.Size)
	{
		changed = true;
		nos::fb::ShowAs tmp = resourceInfoSharedPtr->ShowAs;
		ResourcesToDelete.Enqueue({ std::move(resourceInfoSharedPtr), GFrameCounter });
		resourceInfoSharedPtr = MakeShared<SharedResourceInfo>();
		
		if(!CreateBufferResource(NosProperty, Buffer, *resourceInfoSharedPtr))
		{
			return changed;
		}
		
		resourceInfoSharedPtr->ShowAs = tmp;
	}
	
	return changed;
}

void NOSResourceShareManager::UpdatePinShowAs(NOSProperty* NosProperty, nos::fb::ShowAs NewShowAs)
{
	if(auto* resourceInfo = Copies.Find(NosProperty))
		(*resourceInfo)->ShowAs = NewShowAs;
}

void NOSResourceShareManager::TextureDestroyed(NOSProperty* textureProp)
{
	Copies.Remove(textureProp);
}

static HANDLE DupeHandle(uint64_t pid, HANDLE handle)
{
    HANDLE re = 0;
    HANDLE src = OpenProcess(GENERIC_ALL, false, pid);
    HANDLE cur = GetCurrentProcess();
    if (!DuplicateHandle(src, handle, cur, &re, GENERIC_ALL, 0, DUPLICATE_SAME_ACCESS))
    {
        return 0;
    }
    CloseHandle(src);
    return re;
}

static bool ImportSharedFence(uint64_t pid, HANDLE handle, ID3D12Device* pDevice, ID3D12Fence** pFence)
{
       HANDLE xmemory = DupeHandle(pid, handle);
       if (FAILED(pDevice->OpenSharedHandle(xmemory, IID_PPV_ARGS(pFence))))
       {
               return false;
       }
       CloseHandle(xmemory);
       return true;
}

void FilterCopies(nos::fb::ShowAs FilterShowAs, TMap<NOSProperty*, TSharedPtr<SharedResourceInfo>>& Copies, TArray<TPair<TSharedPtr<SharedResourceInfo>, TObjectPtr<UObject>>>& FilteredCopies)
{
	for (auto const& [nosprop, info] : Copies)
	{
		UObject* obj = nosprop->GetRawObjectContainer();
		if (!obj) continue;
		auto prop = CastField<FObjectProperty>(nosprop->Property);
		if (!prop) continue;
		auto URT = Cast<UTextureRenderTarget2D>(prop->GetObjectPropertyValue(prop->ContainerPtrToValuePtr<UTextureRenderTarget2D>(obj)));
		auto Buffer = Cast<UNOSGPUBuffer>(prop->GetObjectPropertyValue(prop->ContainerPtrToValuePtr<UNOSGPUBuffer>(obj)));
		if (!URT && !Buffer) continue;

		auto TextureShareManager = NOSResourceShareManager::GetInstance();
		bool shouldSendPinValueChange = false;
		if (URT)
		{
			if(info->DstTexture->SizeX != URT->SizeX || info->DstTexture->SizeY != URT->SizeY)
			{
				//todo texture is changed update it
		 	
				const nos::sys::vulkan::Texture* tex = flatbuffers::GetRoot<nos::sys::vulkan::Texture>(nosprop->data.data());
				nos::sys::vulkan::TTexture texture;
				tex->UnPackTo(&texture);

				if (TextureShareManager->UpdateTexturePin(nosprop, texture))
				{
					// data = nos::Buffer::From(texture);
					flatbuffers::FlatBufferBuilder fb;
					auto offset = nos::sys::vulkan::CreateTexture(fb, &texture);
					fb.Finish(offset);
					nos::Buffer buffer = fb.Release();
					nosprop->data = buffer;
					shouldSendPinValueChange = true;
				}
			}
		}
		else if (Buffer)
		{
			auto existingBytes = info->DstBuffer->Buffer.NumBytes;
			if (existingBytes != Buffer->GetBufferSize())
			{
				nos::sys::vulkan::Buffer* buf = reinterpret_cast<nos::sys::vulkan::Buffer*>(nosprop->data.data());
				if (TextureShareManager->UpdateBufferPin(nosprop, *buf))
				{
					nosprop->data = nos::Buffer::From(buf);
					shouldSendPinValueChange = true;
				}
			}
		}

		if (!TextureShareManager->NOSClient->IsConnected() || nosprop->data.empty())
		{
			return;
		}

		if (shouldSendPinValueChange)
		{
			flatbuffers::FlatBufferBuilder mb;
			auto offset2 = nos::app::CreateSetPinValueDirect(mb, (nos::fb::UUID*)&nosprop->Id, &nosprop->data);
			mb.Finish(offset2);
			auto buf = mb.Release();
			auto root = flatbuffers::GetRoot<nos::app::SetPinValue>(buf.data());
			TextureShareManager->NOSClient->AppServiceClient->NotifyPinValueChanged(root);
		}
			
		if (info->ShowAs == FilterShowAs)
		{
			if (URT)
			{
				FilteredCopies.Add(TPair<TSharedPtr<SharedResourceInfo>, TObjectPtr<UObject>>(info, URT));
			}
			else if (Buffer)
			{
				FilteredCopies.Add(TPair<TSharedPtr<SharedResourceInfo>, TObjectPtr<UObject>>(info, Buffer));
			}
		}
	}
}

void NOSResourceShareManager::SetupFences(FRHICommandListImmediate& RHICmdList, nos::fb::ShowAs CopyShowAs,
	TMap<ID3D12Fence*, uint64_t>& SignalGroup, uint64_t frameNumber)
{
	if(ExecutionState == nos::app::ExecutionState::SYNCED)
	{
		if(CopyShowAs == nos::fb::ShowAs::INPUT_PIN)
		{
			RHICmdList.EnqueueLambda([CmdQueue = CmdQueue,InputFence = InputFence, frameNumber](FRHICommandList& ExecutingCmdList)
			{
				GetID3D12DynamicRHI()->RHIWaitManualFence(ExecutingCmdList, InputFence, (2 * frameNumber) + 1);
			});
			SignalGroup.Add(InputFence, (2 * frameNumber) + 2);

#ifdef DEBUG_FRAME_SYNC_LOG
			UE_LOG(LogTemp, Warning, TEXT("Input pins are waiting on %d") , 2 * frameNumber + 1);
#endif
			
		}
		else if (CopyShowAs == nos::fb::ShowAs::OUTPUT_PIN)
		{
			RHICmdList.EnqueueLambda([CmdQueue = CmdQueue, OutputFence = OutputFence, frameNumber = frameNumber](FRHICommandList& ExecutingCmdList)
			{
				
				GetID3D12DynamicRHI()->RHIWaitManualFence(ExecutingCmdList, OutputFence, (2 * frameNumber));
			});
			SignalGroup.Add(OutputFence, (2 * frameNumber) + 1);

#ifdef DEBUG_FRAME_SYNC_LOG
			UE_LOG(LogTemp, Warning, TEXT("Out pins are waiting on %d") , 2 * frameNumber);
#endif
		}
	}
}

void NOSResourceShareManager::ProcessCopies(nos::fb::ShowAs CopyShowAs, TMap<NOSProperty*, TSharedPtr<SharedResourceInfo>>& CopyMap)
{
	TArray<TPair<TSharedPtr<SharedResourceInfo>, TObjectPtr<UObject>>> CopiesFiltered;
	FilterCopies(CopyShowAs, CopyMap, CopiesFiltered);

	//auto cmdData = GetNewCommandList();
	ENQUEUE_RENDER_COMMAND(FNOSClient_CopyOnTick)(
		[this, CopyShowAs, CopiesFiltered, frameNumber = FrameCounter](FRHICommandListImmediate& RHICmdList)
		{
#ifdef DEBUG_NODOS_TEXTURE_COPIES
			SCOPED_CONDITIONAL_DRAW_EVENTF(RHICmdList, NodosCopies_Output, CopyShowAs == nos::fb::ShowAs::OUTPUT_PIN, TEXT("Nodos Copies(Output)"));
			SCOPED_CONDITIONAL_DRAW_EVENTF(RHICmdList, NodosCopies_Input, CopyShowAs == nos::fb::ShowAs::INPUT_PIN, TEXT("Nodos Copies(Input)"));
#endif
			TMap<ID3D12Fence*, u64> SignalGroup;
			SetupFences(RHICmdList, CopyShowAs, SignalGroup, frameNumber);
			for (auto& [pin, obj] : CopiesFiltered)
			{
				if (auto URT = Cast<UTextureRenderTarget2D>(obj))
				{
					FRHICopyTextureInfo CopyInfo;
					CopyInfo.Size = FIntVector(pin->DstTexture->SizeX, pin->DstTexture->SizeY, 1);
					FRHITexture* dst = pin->DstTexture->GetRenderTargetResource()->GetRenderTargetTexture();
					FRHITexture* src = URT->GetRenderTargetResource()->GetRenderTargetTexture();
					if(CopyShowAs == nos::fb::ShowAs::INPUT_PIN)
					{
						Swap(dst, src);
					}
					if (!src || !dst)
					{
						UE_LOG(LogTemp, Warning, TEXT("Texture is null!"));
						continue;
					}
					if(src->GetSizeXY() != dst->GetSizeXY())
					{
						UE_LOG(LogTemp, Warning, TEXT("Texture sizes are not equal!"));
						continue;
					}
					RHICmdList.CopyTexture(src, dst, CopyInfo);
				}
				else if (auto Buffer = Cast<UNOSGPUBuffer>(obj))
				{
					FRHIBuffer* dst = pin->DstBuffer->Buffer.Buffer;
					FRHIBuffer* src = Buffer->Buffer.Buffer;
					if(CopyShowAs == nos::fb::ShowAs::INPUT_PIN)
					{
						Swap(dst, src);
					}
					if (!src || !dst)
					{
						UE_LOG(LogTemp, Warning, TEXT("Buffer is null!"));
						continue;
					}
					if(src->GetSize() != dst->GetSize())
					{
						UE_LOG(LogTemp, Warning, TEXT("Buffer sizes are not equal! Src: %d, Dst: %d"), src->GetSize(), dst->GetSize());
						continue;
					}
					RHICmdList.CopyBufferRegion(dst, 0, src, 0, dst->GetSize());
				}
			}
			for(auto& [fence, val] : SignalGroup)
			{
				RHICmdList.EnqueueLambda([CmdQueue = CmdQueue, fence, val](FRHICommandList& ExecutingCmdList)
				{
					GetID3D12DynamicRHI()->RHISignalManualFence(ExecutingCmdList, fence, val);
				});
			}
		});
}

void NOSResourceShareManager::OnBeginFrame()
{
	ProcessCopies(nos::fb::ShowAs::INPUT_PIN, Copies);
}

void NOSResourceShareManager::OnEndFrame()
{
	ProcessCopies(nos::fb::ShowAs::OUTPUT_PIN, Copies);
	FrameCounter++;
	while(!ResourcesToDelete.IsEmpty())
	{
		auto* resource = ResourcesToDelete.Peek();
		if(resource->Value + 5 <= GFrameCounter) // resources are deleted after 5 frames, because we need to make sure that they are no longer in use
		{
			FlushRenderingCommands();
			ResourcesToDelete.Pop();
		}
		else
		{
			break;
		}
	}
	// ENQUEUE_RENDER_COMMAND(FNOSClient_CopyOnTick)(
	// 	[this, FrameCount = GFrameCounter](FRHICommandListImmediate& RHICmdList)
	// 	{
	// 	});
}

bool NOSResourceShareManager::SwitchStateToSynced()
{
	FScopeLock Lock(&CriticalSectionState);
	RenewSemaphores();
	ENQUEUE_RENDER_COMMAND(FNOSClient_CopyOnTick)(
		[this](FRHICommandListImmediate& RHICmdList)
		{
			ExecutionState = nos::app::ExecutionState::SYNCED;
		});

	return true;
}

void NOSResourceShareManager::SwitchStateToIdle_GRPCThread(uint64_t LastFrameNumber)
{
	FScopeLock Lock(&CriticalSectionState);
	ExecutionState = nos::app::ExecutionState::IDLE;
	for(int i = 0; i < 2; i++)
	{
		if (InputFence && OutputFence)
		{
			InputFence->Signal(UINT64_MAX);
			OutputFence->Signal(UINT64_MAX);
		}
		FPlatformProcess::Sleep(0.2);
	}
	FrameCounter = 0;
}

void NOSResourceShareManager::Reset()
{

	Copies.Empty();
	PendingCopyQueue.Empty();
}

void NOSResourceShareManager::Initiate()
{
	auto hwinfo = FHardwareInfo::GetHardwareInfo(NAME_RHI);
	if ("D3D12" != hwinfo)
	{
		return;
	}

	NOSClient = &FModuleManager::LoadModuleChecked<FNOSClient>("NOSClient");
	
	// Create DX resources
	Dev = (ID3D12Device*)GetID3D12DynamicRHI()->RHIGetNativeDevice();
	CmdQueue = GetID3D12DynamicRHI()->RHIGetCommandQueue();
	CmdQueue->AddRef();
	
	
#ifdef FAIL_SAFE_THREAD 
			FailSafeRunnable = new NOSGPUFailSafeRunnable(CmdQueue, Dev);
			FailSafeThread = FRunnableThread::Create(FailSafeRunnable, TEXT("NOSGPUFailSafeThread"));

			FCoreDelegates::OnEnginePreExit.AddLambda([this]()
			{
				if(FailSafeRunnable && FailSafeThread)
				{
					FailSafeRunnable->Stop();
					FailSafeThread->WaitForCompletion();
				}
			});
#endif
	
	RenewSemaphores();
}

void NOSResourceShareManager::RenewSemaphores()
{
	if (InputFence)
	{
		::CloseHandle(SyncSemaphoresExportHandles.InputSemaphore);
		InputFence->Release();
		InputFence = nullptr;

	}
	if (OutputFence)
	{
		::CloseHandle(SyncSemaphoresExportHandles.OutputSemaphore);
		OutputFence->Release();
		OutputFence = nullptr;
	}

	FrameCounter = 0;
	
	Dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&InputFence));
	Dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&OutputFence));
	NOS_D3D12_ASSERT_SUCCESS(Dev->CreateSharedHandle(InputFence, 0, GENERIC_ALL, 0, &SyncSemaphoresExportHandles.InputSemaphore));
	NOS_D3D12_ASSERT_SUCCESS(Dev->CreateSharedHandle(OutputFence, 0, GENERIC_ALL, 0, &SyncSemaphoresExportHandles.OutputSemaphore));
}

SharedResourceInfo::~SharedResourceInfo()
{
	if (SharedHandle)
	{
		CloseHandle(SharedHandle);
	}
	if (DstTexture)
	{
		DstTexture->ReleaseResource();
	}
	if (DstBuffer)
	{
		DstBuffer->Buffer.Release();
	}
}
