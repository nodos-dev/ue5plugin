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

#include "D3D12RHI.h"
#include "ID3D12DynamicRHI.h"
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
//#define DEBUG_NODOS_TEXTURE_COPIES

nosTextureInfo ConvertRenderTargetToNosTextureInfo(UTextureRenderTarget2D const& renderTarget)
{
	nosTextureInfo info = {
		.Width = (uint32_t)renderTarget.GetSurfaceWidth(),
		.Height = (uint32_t)renderTarget.GetSurfaceHeight(),
		.Usage = (nosImageUsage)(NOS_IMAGE_USAGE_RENDER_TARGET | NOS_IMAGE_USAGE_SAMPLED | NOS_IMAGE_USAGE_TRANSFER_SRC | NOS_IMAGE_USAGE_TRANSFER_DST),
	};

	switch (renderTarget.RenderTargetFormat)
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

UTextureRenderTarget2D* GetPropertyRenderTarget(NOSProperty* nosprop)
{
	UObject* obj = nosprop->GetRawObjectContainer();
	FObjectProperty* prop = CastField<FObjectProperty>(nosprop->Property);
	if (!obj || !prop)
	{
		return nullptr;
	}

	return Cast<UTextureRenderTarget2D>(prop->GetObjectPropertyValue(prop->ContainerPtrToValuePtr<UTextureRenderTarget2D>(obj)));
}


std::optional<std::pair<TSharedPtr<SharedResourceInfo>, nos::sys::vulkan::TTexture>> TryCreateDestinationForRT(ID3D12Device& device, UTextureRenderTarget2D& sourceRT, FString const& destName)
{
	auto info = ConvertRenderTargetToNosTextureInfo(sourceRT);

	auto newRTName = destName + FGuid::NewGuid().ToString();

	UTextureRenderTarget2D* NewRenderTarget2D = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), *newRTName, RF_MarkAsRootSet);
	check(NewRenderTarget2D);
	NewRenderTarget2D->Rename(*newRTName);
	NewRenderTarget2D->RenderTargetFormat = sourceRT.RenderTargetFormat;
	NewRenderTarget2D->ClearColor = sourceRT.ClearColor;
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
		return std::nullopt;
	}
	FRHITexture* RHITexture = RHIResource;

	ID3D12Resource* DXResource = static_cast<ID3D12Resource*>(RHITexture->GetNativeResource());
	DXResource->SetName(*destName);

	HANDLE handle = 0;
	NOS_D3D12_ASSERT_SUCCESS(device.CreateSharedHandle(DXResource, 0, GENERIC_ALL, 0, &handle));
	nos::sys::vulkan::TTexture retTexture;
	retTexture.resolution = nos::sys::vulkan::SizePreset::CUSTOM;
	retTexture.width = info.Width;
	retTexture.height = info.Height;
	retTexture.format = nos::sys::vulkan::Format(info.Format);
	retTexture.usage = nos::sys::vulkan::ImageUsage(info.Usage) | nos::sys::vulkan::ImageUsage::SAMPLED;
	auto& Ext = retTexture.external_memory;
	Ext.mutate_handle_type(NOS_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE);
	Ext.mutate_handle((uint64_t)handle);
	D3D12_RESOURCE_DESC desc = DXResource->GetDesc();
	Ext.mutate_allocation_size(device.GetResourceAllocationInfo(0, 1, &desc).SizeInBytes);
	Ext.mutate_pid(FPlatformProcess::GetCurrentProcessId());
	retTexture.unmanaged = true;
	retTexture.unscaled = true;
	retTexture.handle = 0;

	auto sharedInfo = MakeShared<SharedResourceInfo>();

	sharedInfo->DstResource = NewRenderTarget2D;
	sharedInfo->SharedHandle = handle;
	return std::make_pair(std::move(sharedInfo), std::move(retTexture));
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
	auto& texPropInfo = *TextureProperties.Add(nosprop, MakeShared<TexturePropertyInfo>(nosprop->PinShowAs));
	auto propRT = GetPropertyRenderTarget(nosprop);
	nosprop->IsOrphan = true;
	if (!propRT)
	{
		return {};
	}
	auto copyInfoTexPair = TryCreateDestinationForRT(*Dev, *propRT, nosprop->DisplayName);
	if(!copyInfoTexPair)
	{
		return {};
	}
	nosprop->IsOrphan = false;
	texPropInfo.ActiveDestinationSharedResource = std::move(copyInfoTexPair->first);
	return copyInfoTexPair->second;
}

void NOSTextureShareManager::UpdateTexturePinValues()
{
	/// This should only send pin values for non-broadcasted property value changes since they are already handled in NOSSceneTreeManager's code
	for (auto const& [prop, texPropInfo] : TextureProperties)
	{
		// This will update stored nodos pin value and orphanness state, but wont send the pin value update
		auto oldDestPtr = texPropInfo->ActiveDestinationSharedResource.Get();
		prop->UpdatePinValue();
		auto newDestPtr = texPropInfo->ActiveDestinationSharedResource.Get();
		if (oldDestPtr != newDestPtr)
		{
			flatbuffers::FlatBufferBuilder mb;
			auto offset2 = nos::app::CreateSetPinValueDirect(mb, (nos::fb::UUID*)&prop->Id, &prop->data);
			mb.Finish(offset2);
			auto buf = mb.Release();
			auto root = flatbuffers::GetRoot<nos::app::SetPinValue>(buf.data());
			NOSClient->AppServiceClient->NotifyPinValueChanged(*root);
		}
	}
}

void NOSTextureShareManager::UpdateTexturePin(NOSProperty* nosprop, nos::fb::ShowAs RealShowAs)
{
	UpdatePinShowAs(nosprop, RealShowAs);
}

std::optional<nos::sys::vulkan::TTexture> NOSTextureShareManager::GetUpdatedTexturePinValue(NOSProperty* NosProperty)
{
	auto* texPropInfo = TextureProperties.Find(NosProperty);
	ensureMsgf(texPropInfo, TEXT("Texture property %s is not registered!"), *NosProperty->DisplayName);
	if (!texPropInfo)
		texPropInfo = &TextureProperties.Add(NosProperty, MakeShared<TexturePropertyInfo>(NosProperty->PinShowAs));

	auto* propertyRenderTarget = GetPropertyRenderTarget(NosProperty);
	auto curDestinationResource = (*texPropInfo)->ActiveDestinationSharedResource;
	// Both new and old is null, nothing changed
	if (!propertyRenderTarget && !curDestinationResource)
		return std::nullopt;
	bool hadDestinationResource = curDestinationResource != nullptr;

	// Both were present, check if destination suits the property RT
	if (propertyRenderTarget && hadDestinationResource)
	{
		auto newRTinfo = ConvertRenderTargetToNosTextureInfo(*propertyRenderTarget);
		auto oldDestRTInfo = ConvertRenderTargetToNosTextureInfo(*curDestinationResource->DstResource);
		if (!(oldDestRTInfo.Width != newRTinfo.Width ||
			oldDestRTInfo.Height != newRTinfo.Height ||
			oldDestRTInfo.Format != newRTinfo.Format ||
			oldDestRTInfo.Usage != (newRTinfo.Usage | NOS_IMAGE_USAGE_SAMPLED)))
			return std::nullopt;
	}

	// If old resource was present, it will be deleted, so remember its ShowAs
	if (curDestinationResource)
	{
		ResourcesToDelete.Enqueue({ std::move(curDestinationResource), GFrameCounter });
		(*texPropInfo)->ActiveDestinationSharedResource = nullptr;
	}

	/* TODO: We should set to orphan before changing pin value and set to orphan after changing pin value to stop nodos side creating a new resource while we are changing it.When we go from orphan to activated, nodos creates a new resource automatically, and starts thinking that the app will import the texture, which is not the case with UE. But when we send the new pin value, nodos will switch to importing from the app so this is handled indirectly.
	  Or, add a mechanism to un-orphan a pin with a value, so that nodos won't try to create a new resource while un-orphaning
	*/
	auto changePinOrphanness = [&](bool newOrphan)
		{
			ensureMsgf(NosProperty->IsOrphan != newOrphan, TEXT("Texture property %s orphanness didn't change!"), *NosProperty->DisplayName);
			NosProperty->IsOrphan = newOrphan;
			if(newOrphan)
				NosProperty->OrphanMessage = "Texture Render Target is not assigned or incompatible!";
			else
				NosProperty->OrphanMessage = "";

			flatbuffers::FlatBufferBuilder mb;
			auto pinOrphanStateOffset = nos::fb::CreatePinOrphanStateDirect(mb, newOrphan ? nos::fb::PinOrphanStateType::ORPHAN : nos::fb::PinOrphanStateType::ACTIVE, newOrphan ? TCHAR_TO_UTF8(*NosProperty->OrphanMessage) : 0);
			auto offset = nos::CreateAppEventOffset(mb, nos::CreatePartialPinUpdateDirect(mb, (nos::fb::UUID*)&NosProperty->Id, nullptr, pinOrphanStateOffset));
			mb.Finish(offset);
			auto buf = mb.Release();
			auto root = flatbuffers::GetRoot<nos::app::AppEvent>(buf.data());
			NOSClient->AppServiceClient->Send(*root);
		};

	if (propertyRenderTarget)
	{
		auto newShareInfoAndTex = TryCreateDestinationForRT(*Dev, *propertyRenderTarget, NosProperty->DisplayName);
		if(!newShareInfoAndTex)
		{
			// If oldResource was present, then it is now deleted and new texture is not created, so it changed
			if (hadDestinationResource)
			{
				changePinOrphanness(true);
				// Old was present but new is not, so return invalid texture
				return nos::sys::vulkan::TTexture{};
			}
			// Both old and new are not present, so nothing changed
			return std::nullopt;
		}
		if (!hadDestinationResource)
			changePinOrphanness(false);
		(*texPropInfo)->ActiveDestinationSharedResource = std::move(newShareInfoAndTex->first);
		return newShareInfoAndTex->second;
	}
	changePinOrphanness(false);
	// Old was present but new is not, so return invalid texture
	return nos::sys::vulkan::TTexture{};
}

void NOSTextureShareManager::UpdatePinShowAs(NOSProperty* NosProperty, nos::fb::ShowAs NewShowAs)
{
	if(auto* texPropInfo = TextureProperties.Find(NosProperty))
		(*texPropInfo)->ShowAs = NewShowAs;
	else
	{
		ensureMsgf(NosProperty->TypeName != nos::sys::vulkan::Texture::GetFullyQualifiedName(), TEXT("Texture property %s is not registered!"), *NosProperty->DisplayName);
	}
}

void NOSTextureShareManager::TextureDestroyed(NOSProperty* textureProp)
{
	auto texPropInfo = TextureProperties.Find(textureProp);
	if ((*texPropInfo)->ActiveDestinationSharedResource)
		ResourcesToDelete.Enqueue({ std::move((*texPropInfo)->ActiveDestinationSharedResource), GFrameCounter });
	TextureProperties.Remove(textureProp);
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

/// Returns only the active copies that match the given ShowAs
void GetActiveTextureCopiesWithShowAs(nos::fb::ShowAs FilterShowAs, TMap<NOSProperty*, TSharedPtr<TexturePropertyInfo>>& textureProperties, TMap<UTextureRenderTarget2D*, TSharedPtr<SharedResourceInfo>>& FilteredCopies)
{
	for (auto const& [nosprop, info] : textureProperties)
	{
		auto* URT = GetPropertyRenderTarget(nosprop);
		if (info->ActiveDestinationSharedResource == nullptr || !URT || info->ShowAs != FilterShowAs)
			continue;
		bool resourcesCompatible = info->ActiveDestinationSharedResource->DstResource->SizeX == URT->SizeX &&
			info->ActiveDestinationSharedResource->DstResource->SizeY == URT->SizeY &&
			info->ActiveDestinationSharedResource->DstResource->RenderTargetFormat == URT->RenderTargetFormat;
		ensureMsgf(resourcesCompatible, TEXT("Texture property %s has incompatible resources!"), *nosprop->DisplayName);
		if (!resourcesCompatible)
			continue;
		FilteredCopies.Add(URT, info->ActiveDestinationSharedResource);
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
				TSharedPtr<std::atomic<bool>> bHasSignalled;
				GetID3D12DynamicRHI()->RHIWaitManualFence(ExecutingCmdList, InputFence, (2 * frameNumber) + 1, bHasSignalled);
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
				TSharedPtr<std::atomic<bool>> bHasSignalled;
				GetID3D12DynamicRHI()->RHIWaitManualFence(ExecutingCmdList, OutputFence, (2 * frameNumber), bHasSignalled);
			});
			SignalGroup.Add(OutputFence, (2 * frameNumber) + 1);

#ifdef DEBUG_FRAME_SYNC_LOG
			UE_LOG(LogTemp, Warning, TEXT("Out pins are waiting on %d") , 2 * frameNumber);
#endif
		}
	}
}

void NOSTextureShareManager::ProcessCopies(nos::fb::ShowAs CopyShowAs, uint64_t FrameNumber)
{
	TMap<UTextureRenderTarget2D*, TSharedPtr<SharedResourceInfo>> CopiesFiltered;
	GetActiveTextureCopiesWithShowAs(CopyShowAs, TextureProperties, CopiesFiltered);

	//auto cmdData = GetNewCommandList();
	ENQUEUE_RENDER_COMMAND(FNOSClient_CopyOnTick)(
		[this, CopyShowAs, CopiesFiltered, FrameNumber](FRHICommandListImmediate& RHICmdList)
		{
#ifdef DEBUG_NODOS_TEXTURE_COPIES
			SCOPED_CONDITIONAL_DRAW_EVENTF(RHICmdList, NodosCopies_Output, CopyShowAs == nos::fb::ShowAs::OUTPUT_PIN, TEXT("Nodos Copies(Output)"));
			SCOPED_CONDITIONAL_DRAW_EVENTF(RHICmdList, NodosCopies_Input, CopyShowAs == nos::fb::ShowAs::INPUT_PIN, TEXT("Nodos Copies(Input)"));
#endif
			TMap<ID3D12Fence*, u64> SignalGroup;
			// FrameNumber comes directly from AppExecuteStart. Never derive cross-process
			// fence values from the independently-running Unreal frame counter.
			SetupFences(RHICmdList, CopyShowAs, SignalGroup, FrameNumber);
			for (auto& [URT, pin] : CopiesFiltered)
			{
				FRHICopyTextureInfo CopyInfo;
				CopyInfo.Size = FIntVector(pin->DstResource->SizeX, pin->DstResource->SizeY, 1);
				FRHITexture* dst = pin->DstResource->GetRenderTargetResource()->GetRenderTargetTexture();
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

void NOSTextureShareManager::OnBeginFrame(uint64_t FrameNumber)
{
	InitializeFenceEpoch(FrameNumber);
	// Publish the render target produced by the previous Unreal frame first. This
	// keeps the app/GPU handshake one frame deep: Nodos can consume frame N while
	// Unreal imports frame N inputs and renders the next image. Waiting to publish
	// until OnEndFrame serializes the full Unreal render into Nodos's frame budget.
	ProcessCopies(nos::fb::ShowAs::OUTPUT_PIN, FrameNumber);
	ProcessCopies(nos::fb::ShowAs::INPUT_PIN, FrameNumber);
}

void NOSTextureShareManager::OnEndFrame(uint64_t FrameNumber)
{
	FrameCounter = FrameNumber + 1;
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
		FPlatformProcess::Sleep(0.2f);
	}
	FrameCounter = 0;
}

void NOSTextureShareManager::Reset()
{
	for(auto& [prop, info] : TextureProperties)
	{
		if(info->ActiveDestinationSharedResource)
			ResourcesToDelete.Enqueue({ std::move(info->ActiveDestinationSharedResource), GFrameCounter });
	}
	TextureProperties.Empty();
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
	bFenceEpochInitialized = false;

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

void NOSTextureShareManager::InitializeFenceEpoch(uint64_t FrameNumber)
{
	FScopeLock Lock(&CriticalSectionState);
	if (bFenceEpochInitialized || !InputFence || !OutputFence)
	{
		return;
	}

	const uint64_t InitialValue = 2 * FrameNumber;
	if (InitialValue > 0)
	{
		NOS_D3D12_ASSERT_SUCCESS(InputFence->Signal(InitialValue));
		NOS_D3D12_ASSERT_SUCCESS(OutputFence->Signal(InitialValue));
	}

	bFenceEpochInitialized = true;
}

SharedResourceInfo::~SharedResourceInfo()
{
	if (SharedHandle)
	{
		CloseHandle(SharedHandle);
	}
	if (DstResource)
	{
		DstResource->ReleaseResource();
	}
}
