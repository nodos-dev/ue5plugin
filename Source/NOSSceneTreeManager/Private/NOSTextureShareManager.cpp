// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include "NOSTextureShareManager.h"

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

#include "NOSGPUFailSafe.h"

#include "nosVulkanSubsystem/nosVulkanSubsystem.h"

NOSTextureShareManager* NOSTextureShareManager::singleton;

//#define FAIL_SAFE_THREAD
//#define DEBUG_FRAME_SYNC_LOG

nosTextureInfo GetResourceInfo(NOSProperty* nosprop)
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

NOSTextureShareManager::NOSTextureShareManager()
{
	Initiate();
}

NOSTextureShareManager* NOSTextureShareManager::GetInstance()
{
	if (singleton == nullptr) {
		singleton = new NOSTextureShareManager();
	}
	return singleton;
}

NOSTextureShareManager::~NOSTextureShareManager()
{
}

nos::sys::vulkan::TTexture NOSTextureShareManager::AddTexturePin(NOSProperty* nosprop)
{
	ResourceInfo copyInfo;
	nos::sys::vulkan::TTexture texture;

	if(!CreateTextureResource(nosprop, texture, copyInfo))
	{
		return texture;
	}
	

	{
		//start property pins as output pins
		Copies.Add(nosprop, copyInfo);
	}
	return texture;
}

bool NOSTextureShareManager::CreateTextureResource(NOSProperty* nosprop, nos::sys::vulkan::TTexture& Texture, ResourceInfo& Resource)
	{
	nosTextureInfo info = GetResourceInfo(nosprop);
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
	
    HANDLE handle;
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
	Resource.DstResource = NewRenderTarget2D;
	Resource.ShowAs = nosprop->PinShowAs;
	return true;
}


void NOSTextureShareManager::UpdateTexturePin(NOSProperty* nosprop, nos::fb::ShowAs RealShowAs)
{
	UpdatePinShowAs(nosprop, RealShowAs);
}

bool NOSTextureShareManager::UpdateTexturePin(NOSProperty* NosProperty, nos::sys::vulkan::TTexture& Texture)
{
	nosTextureInfo info = GetResourceInfo(NosProperty);

	auto resourceInfo = Copies.Find(NosProperty);
	if (resourceInfo == nullptr)
		return false;

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
		
		ResourcesToDelete.Enqueue({resourceInfo->DstResource, GFrameCounter});
		nos::fb::ShowAs tmp = resourceInfo->ShowAs;
		if(!CreateTextureResource(NosProperty, Texture, *resourceInfo))
		{
			return changed;
		}
		
		resourceInfo->ShowAs = tmp;
		Copies[NosProperty] = *resourceInfo;
	}

	return changed;
}

void NOSTextureShareManager::UpdatePinShowAs(NOSProperty* NosProperty, nos::fb::ShowAs NewShowAs)
{
	if(Copies.Contains(NosProperty))
	{
		auto resourceInfo = Copies.Find(NosProperty);
		resourceInfo->ShowAs = NewShowAs;
	}
}

void NOSTextureShareManager::TextureDestroyed(NOSProperty* textureProp)
{
	Copies.Remove(textureProp);
	
	//TODO delete real resource	
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

void FilterCopies(nos::fb::ShowAs FilterShowAs, TMap<NOSProperty*, ResourceInfo>& Copies, TMap<UTextureRenderTarget2D*, ResourceInfo>& FilteredCopies)
{
	for (auto [nosprop, info] : Copies)
	{
		UObject* obj = nosprop->GetRawObjectContainer();
		if (!obj) continue;
		auto prop = CastField<FObjectProperty>(nosprop->Property);
		if (!prop) continue;
		auto URT = Cast<UTextureRenderTarget2D>(prop->GetObjectPropertyValue(prop->ContainerPtrToValuePtr<UTextureRenderTarget2D>(obj)));
		if (!URT) continue;
		
		 if(info.DstResource->SizeX != URT->SizeX || info.DstResource->SizeY != URT->SizeY)
		 {
		 	//todo texture is changed update it
		 	
			const nos::sys::vulkan::Texture* tex = flatbuffers::GetRoot<nos::sys::vulkan::Texture>(nosprop->data.data());
			nos::sys::vulkan::TTexture texture;
			tex->UnPackTo(&texture);

		 	auto TextureShareManager = NOSTextureShareManager::GetInstance();
			if (TextureShareManager->UpdateTexturePin(nosprop, texture))
			{
				// data = nos::Buffer::From(texture);
				flatbuffers::FlatBufferBuilder fb;
				auto offset = nos::sys::vulkan::CreateTexture(fb, &texture);
				fb.Finish(offset);
				nos::Buffer buffer = fb.Release();
				nosprop->data = buffer;
				
				if (!TextureShareManager->NOSClient->IsConnected() || nosprop->data.empty())
				{
					return;
				}
				
				flatbuffers::FlatBufferBuilder mb;
				auto offset2 = nos::app::CreateSetPinValueDirect(mb, (nos::fb::UUID*)&nosprop->Id, &nosprop->data);
				mb.Finish(offset2);
				auto buf = mb.Release();
				auto root = flatbuffers::GetRoot<nos::app::SetPinValue>(buf.data());
				TextureShareManager->NOSClient->AppServiceClient->NotifyPinValueChanged(*root);
			}
		 }
			
		if(info.ShowAs == FilterShowAs)
		{
			FilteredCopies.Add(URT, info);
		}
	}
}

void NOSTextureShareManager::SetupFences(FRHICommandListImmediate& RHICmdList, nos::fb::ShowAs CopyShowAs,
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

void NOSTextureShareManager::ProcessCopies(nos::fb::ShowAs CopyShowAs, TMap<NOSProperty*, ResourceInfo>& CopyMap)
{
	TMap<UTextureRenderTarget2D*, ResourceInfo> CopiesFiltered;
	FilterCopies(CopyShowAs, CopyMap, CopiesFiltered);

	//auto cmdData = GetNewCommandList();
	ENQUEUE_RENDER_COMMAND(FNOSClient_CopyOnTick)(
		[this, CopyShowAs, CopiesFiltered, frameNumber = FrameCounter](FRHICommandListImmediate& RHICmdList)
		{
			SCOPED_CONDITIONAL_DRAW_EVENTF(RHICmdList, NodosCopies_Output, CopyShowAs == nos::fb::ShowAs::OUTPUT_PIN, TEXT("Nodos Copies(Output)"));
			SCOPED_CONDITIONAL_DRAW_EVENTF(RHICmdList, NodosCopies_Input, CopyShowAs == nos::fb::ShowAs::INPUT_PIN, TEXT("Nodos Copies(Input)"));
			TMap<ID3D12Fence*, u64> SignalGroup;
			SetupFences(RHICmdList, CopyShowAs, SignalGroup, frameNumber);
			for (auto& [URT, pin] : CopiesFiltered)
			{
				FRHICopyTextureInfo CopyInfo;
				CopyInfo.Size = FIntVector(pin.DstResource->SizeX, pin.DstResource->SizeY, 1);
				FRHITexture* dst = pin.DstResource->GetRenderTargetResource()->GetRenderTargetTexture();
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
			for(auto& [fence, val] : SignalGroup)
			{
				RHICmdList.EnqueueLambda([CmdQueue = CmdQueue, fence, val](FRHICommandList& ExecutingCmdList)
				{
					GetID3D12DynamicRHI()->RHISignalManualFence(ExecutingCmdList, fence, val);
				});
			}
		});
}

void NOSTextureShareManager::OnBeginFrame()
{
	ProcessCopies(nos::fb::ShowAs::INPUT_PIN, Copies);
}

void NOSTextureShareManager::OnEndFrame()
{
	ProcessCopies(nos::fb::ShowAs::OUTPUT_PIN, Copies);
	FrameCounter++;
	while(!ResourcesToDelete.IsEmpty())
	{
		TPair<TObjectPtr<UTextureRenderTarget2D>, uint32_t> resource;
		ResourcesToDelete.Peek(resource);
		if(resource.Value + 5 <= GFrameCounter) // resources are deleted after 5 frames, because we need to make sure that they are no longer in use
		{
			FlushRenderingCommands();
			resource.Key->ReleaseResource();
			resource.Key = nullptr;
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

bool NOSTextureShareManager::SwitchStateToSynced()
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

void NOSTextureShareManager::SwitchStateToIdle_GRPCThread(uint64_t LastFrameNumber)
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

void NOSTextureShareManager::Reset()
{
	Copies.Empty();
	PendingCopyQueue.Empty();
}

void NOSTextureShareManager::Initiate()
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

void NOSTextureShareManager::RenewSemaphores()
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

	
	
