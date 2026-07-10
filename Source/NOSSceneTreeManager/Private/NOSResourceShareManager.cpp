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

#include "nosSysVulkan/nosVulkanSubsystem.h"
#include "nosSysVulkan/ResourceShare_generated.h"

#include "UObject/UObjectGlobals.h"
#include "Engine/TextureRenderTarget2D.h"
#include "NOSGPUBuffer.h"

NOSResourceShareManager* NOSResourceShareManager::singleton;

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

nosBufferInfo ConvertNosGpuBufferToNosBufferInfo(UNOSGPUBuffer const& nosGpuBuffer)
{
	nosBufferInfo info = {
		.Size = (uint32_t)nosGpuBuffer.GetBufferSize(),
		.Usage = (nosBufferUsage)(NOS_BUFFER_USAGE_TRANSFER_DST | NOS_BUFFER_USAGE_TRANSFER_SRC | NOS_BUFFER_USAGE_STORAGE_BUFFER),
	};

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

UNOSGPUBuffer* GetPropertyNosGpuBuffer(NOSProperty* NosProp)
{
	UObject* Obj = NosProp->GetRawObjectContainer();
	FObjectProperty* Prop = CastField<FObjectProperty>(NosProp->Property);
	if (!Obj || !Prop)
	{
		return nullptr;
	}
	return Cast<UNOSGPUBuffer>(Prop->GetObjectPropertyValue(Prop->ContainerPtrToValuePtr<UTextureRenderTarget2D>(Obj)));
}

std::optional<std::pair<TSharedPtr<SharedResourceInfo>, nos::Buffer>> CreateDestinationForRenderTarget(ID3D12Device& device, UTextureRenderTarget2D& sourceRT, FString const& destName)
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

	auto sharedInfo = MakeShared<SharedResourceInfo>();

	sharedInfo->DstTexture = NewRenderTarget2D;
	sharedInfo->SharedHandle = handle;
	return std::make_pair(std::move(sharedInfo), nos::Buffer::From(retTexture));
}

std::optional<std::pair<TSharedPtr<SharedResourceInfo>, nos::Buffer>> CreateDestinationForBuffer(ID3D12Device& Device, UNOSGPUBuffer& SourceBuffer, FString const& DstName)
{
	if (!SourceBuffer.IsCreated())
	{
		return std::nullopt;
	}

	UNOSGPUBuffer* SharedBuffer = NewObject<UNOSGPUBuffer>(GetTransientPackage(), *(DstName + FGuid::NewGuid().ToString()), RF_MarkAsRootSet);
	check(SharedBuffer);

	// Initialize the buffer on the render thread
	SharedBuffer->AllocateBlocking(SourceBuffer.GetBufferSize(), *DstName, SourceBuffer.GetUnderlyingBuffer()->GetUsage() | BUF_Shared);

	// Get the D3D12 resource from the RHI buffer
	FRHIBuffer* RHIBuffer = SharedBuffer->GetUnderlyingBuffer();
	if (!RHIBuffer || !RHIBuffer->IsValid())
	{
		return std::nullopt;
	}
	auto DXResource = GetID3D12DynamicRHI()->RHIGetResource(RHIBuffer);
	DXResource->SetName(*DstName);
	D3D12_RESOURCE_DESC Desc = DXResource->GetDesc();
	
	// Create shared handle for the buffer
	HANDLE SharedHandle = 0;
	NOS_D3D12_ASSERT_SUCCESS(Device.CreateSharedHandle(DXResource, 0, GENERIC_ALL, 0, &SharedHandle));
	nos::sys::vulkan::Buffer Buffer{};
	// Set up the Vulkan buffer structure
	Buffer.mutate_size_in_bytes(RHIBuffer->GetSize());
	Buffer.mutate_usage(nos::sys::vulkan::BufferUsage(NOS_BUFFER_USAGE_TRANSFER_DST | NOS_BUFFER_USAGE_TRANSFER_SRC | NOS_BUFFER_USAGE_STORAGE_BUFFER));
	nosMemoryFlags MemoryFlags = NOS_MEMORY_FLAGS_DEVICE_MEMORY;
	if (int(SharedBuffer->GetUnderlyingBuffer()->GetUsage()) & int(BUF_KeepCPUAccessible))
	{
		reinterpret_cast<int&>(MemoryFlags) |= NOS_MEMORY_FLAGS_HOST_VISIBLE;
	}
	Buffer.mutate_memory_flags((nos::sys::vulkan::MemoryFlags)MemoryFlags);
	auto& Ext = Buffer.mutable_external_memory();
	Ext.mutate_handle_type(NOS_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE);
	Ext.mutate_handle((uint64_t)SharedHandle);
	
	Ext.mutate_allocation_size(Device.GetResourceAllocationInfo(0, 1, &Desc).SizeInBytes);
	Ext.mutate_pid(FPlatformProcess::GetCurrentProcessId());

	auto SharedInfo = MakeShared<SharedResourceInfo>();

	SharedInfo->DstBuffer = SharedBuffer;
	SharedInfo->SharedHandle = SharedHandle;
	return std::make_pair(std::move(SharedInfo), nos::Buffer::From(Buffer));
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

nos::Buffer NOSResourceShareManager::AddResourcePin(NOSProperty* nosprop)
{
	auto& resPropInfo = *ResourceProperties.Add(nosprop, MakeShared<ResourcePropertyInfo>(nosprop->PinShowAs));
	nosprop->IsOrphan = true;
	std::optional<std::pair<TSharedPtr<SharedResourceInfo>, nos::Buffer>> copyInfoResDataPair;
	auto& PinId = reinterpret_cast<nos::fb::UUID const&>(nosprop->Id);
	if (auto propRT = GetPropertyRenderTarget(nosprop))
	{
		copyInfoResDataPair = CreateDestinationForRenderTarget(*Dev, *propRT, nosprop->DisplayName);
		nos::sys::vulkan::TTexture tex = copyInfoResDataPair->second.As<nos::sys::vulkan::TTexture>();
		ImportResource(PinId, tex);
	}
	else if (auto propBuffer = GetPropertyNosGpuBuffer(nosprop))
	{
		copyInfoResDataPair = CreateDestinationForBuffer(*Dev, *propBuffer, nosprop->DisplayName);
		auto buf = copyInfoResDataPair->second.As<nos::sys::vulkan::Buffer>();
		ImportResource(PinId, *buf);
	}
	else
	{
		nosprop->OrphanMessage = "No resource is present in property!";
		return {};
	}
	nosprop->IsOrphan = false;
	resPropInfo.ActiveDestinationSharedResource = std::move(copyInfoResDataPair->first);
	return copyInfoResDataPair->second;
}

void NOSResourceShareManager::ImportResource(nos::fb::UUID const& pinId, std::variant<nos::sys::vulkan::TTexture, nos::sys::vulkan::Buffer> res)
{
	flatbuffers::FlatBufferBuilder fbb;
	nos::sys::vulkan::TImportResource importResource;
	importResource.pin_id = std::make_unique<nos::fb::UUID>(pinId);
	if (auto* tex = std::get_if<nos::sys::vulkan::TTexture>(&res))
	{
		importResource.external_resource.Set(std::move(*tex));
	}
	else if (auto buf = std::get_if<nos::sys::vulkan::Buffer>(&res))
	{
		importResource.external_resource.Set(std::move(*buf));
	}
	else
	{
		ensureMsgf(false, TEXT("Unsupported resource type in ImportResource!"));
		return;
	}
	nos::sys::vulkan::TResourceShareMessage msg;
	msg.message.Set(std::move(importResource));
	std::vector<uint8_t> importResourceMsgBuf = nos::Buffer::From(msg);
	auto offset = nos::CreateAppEventOffset(fbb, nos::app::CreateCustomMessageDirect(fbb, "nos.sys.vulkan", "nos.sys.vulkan.ResourceShareMessage", &importResourceMsgBuf));
	fbb.Finish(offset);
	auto buf = fbb.Release();
	auto root = flatbuffers::GetRoot<nos::app::AppEvent>(buf.data());
	NOSClient->AppServiceClient->Send(root);
}

void NOSResourceShareManager::CheckAndUpdateResourcePinValues()
{
	/// This should only send pin values for non-broadcasted property value changes since they are already handled in NOSSceneTreeManager's code
	for (auto const& [Prop, ResPropInfo] : ResourceProperties)
	{
		// This will update stored nodos pin value and orphanness state, but wont send the pin value update
		Prop->UpdatePinValue();
	}
}

void NOSResourceShareManager::UpdateResourcePin(NOSProperty* nosprop, nos::fb::ShowAs RealShowAs)
{
	UpdatePinShowAs(nosprop, RealShowAs);
}

std::optional<nos::Buffer> NOSResourceShareManager::GetUpdatedResourcePinValue(NOSProperty* NosProperty)
{
	auto* resPropInfo = ResourceProperties.Find(NosProperty);
	ensureMsgf(resPropInfo, TEXT("Resource property %s is not registered!"), *NosProperty->DisplayName);
	if (!resPropInfo)
		resPropInfo = &ResourceProperties.Add(NosProperty, MakeShared<ResourcePropertyInfo>(NosProperty->PinShowAs));

	auto* propertyRenderTarget = GetPropertyRenderTarget(NosProperty);
	auto* propertyBuffer = GetPropertyNosGpuBuffer(NosProperty);
	auto curDestinationResource = (*resPropInfo)->ActiveDestinationSharedResource;
	// Both new and old is null, nothing changed
	if (!(propertyRenderTarget || propertyBuffer) && !curDestinationResource)
		return std::nullopt;
	bool hadDestinationResource = curDestinationResource != nullptr;
	
	auto invalidTexData = nos::Buffer::From(nos::sys::vulkan::TTexture{});
	auto invalidBufData = nos::Buffer::From(nos::sys::vulkan::Buffer{});

	// Both were present, check if destination suits the property RT
	if (hadDestinationResource)
	{
		if (propertyRenderTarget)
		{
			auto newRTinfo = ConvertRenderTargetToNosTextureInfo(*propertyRenderTarget);
			auto oldDestRTInfo = ConvertRenderTargetToNosTextureInfo(*curDestinationResource->DstTexture);
			if (!(oldDestRTInfo.Width != newRTinfo.Width ||
				oldDestRTInfo.Height != newRTinfo.Height ||
				oldDestRTInfo.Format != newRTinfo.Format ||
				oldDestRTInfo.Usage != (newRTinfo.Usage | NOS_IMAGE_USAGE_SAMPLED)))
				return std::nullopt;
		}
		else if (propertyBuffer)
		{
			auto newBufInfo = ConvertNosGpuBufferToNosBufferInfo(*propertyBuffer);
			auto oldDestBufInfo = ConvertNosGpuBufferToNosBufferInfo(*curDestinationResource->DstBuffer);
			if (!(oldDestBufInfo.Size != newBufInfo.Size ||
				oldDestBufInfo.Usage != newBufInfo.Usage))
				return std::nullopt;
		}
	}

	// If old resource was present, it will be deleted, so remember its ShowAs
	if (curDestinationResource)
	{
		ResourcesToDelete.Enqueue({ std::move(curDestinationResource), GFrameCounter });
		(*resPropInfo)->ActiveDestinationSharedResource = nullptr;
	}

	/* TODO: We should set to orphan before changing pin value and set to orphan after changing pin value to stop nodos side creating a new resource while we are changing it.When we go from orphan to activated, nodos creates a new resource automatically, and starts thinking that the app will import the texture, which is not the case with UE. But when we send the new pin value, nodos will switch to importing from the app so this is handled indirectly.
	  Or, add a mechanism to un-orphan a pin with a value, so that nodos won't try to create a new resource while un-orphaning
	*/
	auto changePinOrphanness = [&](bool newOrphan)
		{
			ensureMsgf(NosProperty->IsOrphan != newOrphan, TEXT("Resource property %s orphan state didn't change!"), *NosProperty->DisplayName);
			NosProperty->IsOrphan = newOrphan;
			if(newOrphan)
				NosProperty->OrphanMessage = "Resource is not assigned or incompatible!";
			else
				NosProperty->OrphanMessage = "";

			flatbuffers::FlatBufferBuilder mb;
			auto pinOrphanStateOffset = nos::fb::CreatePinOrphanStateDirect(mb, newOrphan ? nos::fb::PinOrphanStateType::ORPHAN : nos::fb::PinOrphanStateType::ACTIVE, newOrphan ? TCHAR_TO_UTF8(*NosProperty->OrphanMessage) : 0);
			auto offset = nos::CreateAppEventOffset(mb, nos::CreatePartialPinUpdateDirect(mb, (nos::fb::UUID*)&NosProperty->Id, nullptr, pinOrphanStateOffset));
			mb.Finish(offset);
			auto buf = mb.Release();
			auto root = flatbuffers::GetRoot<nos::app::AppEvent>(buf.data());
			NOSClient->AppServiceClient->Send(root);
		};

	if (propertyRenderTarget)
	{
		auto newShareInfoAndTex = CreateDestinationForRenderTarget(*Dev, *propertyRenderTarget, NosProperty->DisplayName);
		if(!newShareInfoAndTex)
		{
			// If oldResource was present, then it is now deleted and new texture is not created, so it changed
			if (hadDestinationResource)
			{
				changePinOrphanness(true);
				// Old was present but new is not, so return invalid texture
				return invalidTexData;
			}
			// Both old and new are not present, so nothing changed
			return std::nullopt;
		}
		if (!hadDestinationResource)
			changePinOrphanness(false);
		(*resPropInfo)->ActiveDestinationSharedResource = std::move(newShareInfoAndTex->first);
		auto tex = newShareInfoAndTex->second.As<nos::sys::vulkan::TTexture>();
		ImportResource(reinterpret_cast<nos::fb::UUID const&>(NosProperty->Id), tex);
		return newShareInfoAndTex->second;
	}
	if (propertyBuffer)
	{
		auto newShareInfoAndBuf = CreateDestinationForBuffer(*Dev, *propertyBuffer, NosProperty->DisplayName);
		if (!newShareInfoAndBuf)
		{
			// If oldResource was present, then it is now deleted and new buffer is not created, so it changed
			if (hadDestinationResource)
			{
				changePinOrphanness(true);
				// Old was present but new is not, so return invalid buffer
				return invalidBufData;
			}
			// Both old and new are not present, so nothing changed
			return std::nullopt;
		}
		if (!hadDestinationResource)
			changePinOrphanness(false);
		(*resPropInfo)->ActiveDestinationSharedResource = std::move(newShareInfoAndBuf->first);
		auto buf = newShareInfoAndBuf->second.As<nos::sys::vulkan::Buffer>();
		ImportResource(reinterpret_cast<nos::fb::UUID const&>(NosProperty->Id), *buf);
	}
	changePinOrphanness(true);
	// Old was present but new is not, so return invalid texture
	return NosProperty->TypeName == nos::sys::vulkan::Texture::GetFullyQualifiedName() ? invalidTexData : invalidBufData;
}

void NOSResourceShareManager::UpdatePinShowAs(NOSProperty* NosProperty, nos::fb::ShowAs NewShowAs)
{
	if(auto* ResPropInfo = ResourceProperties.Find(NosProperty))
		(*ResPropInfo)->ShowAs = NewShowAs;
	else
	{
		auto isValidResource = NosProperty->TypeName == nos::sys::vulkan::Texture::GetFullyQualifiedName() ||
			NosProperty->TypeName == nos::sys::vulkan::Buffer::GetFullyQualifiedName();
		//ensureMsgf(isValidResource, TEXT("Resource property %s has unsupported type %s and is not registered!"), *NosProperty->DisplayName, *FString(NosProperty->TypeName.c_str()));
	}
}

void NOSResourceShareManager::ResourceDestroyed(NOSProperty* ResProp)
{
	auto ResPropInfo = ResourceProperties.Find(ResProp);
	if ((*ResPropInfo)->ActiveDestinationSharedResource)
		ResourcesToDelete.Enqueue({ std::move((*ResPropInfo)->ActiveDestinationSharedResource), GFrameCounter });
	ResourceProperties.Remove(ResProp);
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
void GetActiveResourceCopiesWithShowAs(nos::fb::ShowAs FilterShowAs, TMap<NOSProperty*, TSharedPtr<ResourcePropertyInfo>>& resourceProperties, TArray<TPair<TSharedPtr<SharedResourceInfo>, TObjectPtr<UObject>>>& FilteredCopies)
{
	for (auto const& [nosprop, info] : resourceProperties)
	{
		if (!info->ActiveDestinationSharedResource || info->ShowAs != FilterShowAs)
			continue;
		bool resourcesCompatible = true;
		auto* URT = GetPropertyRenderTarget(nosprop);
		auto* UBuffer = GetPropertyNosGpuBuffer(nosprop);
		if (!(URT || UBuffer))
		{
			ensureMsgf(false, TEXT("Resource property %s has no valid resource!"), *nosprop->DisplayName);
			continue;
		}
		if (URT)
		{
			resourcesCompatible = info->ActiveDestinationSharedResource->DstTexture->SizeX == URT->SizeX &&
				info->ActiveDestinationSharedResource->DstTexture->SizeY == URT->SizeY &&
				info->ActiveDestinationSharedResource->DstTexture->RenderTargetFormat == URT->RenderTargetFormat;
			ensureMsgf(resourcesCompatible, TEXT("Texture property %s has incompatible resources!"), *nosprop->DisplayName);
		}
		else if (UBuffer)
		{
			resourcesCompatible = info->ActiveDestinationSharedResource->DstBuffer->GetBufferSize() == UBuffer->GetBufferSize();
			ensureMsgf(resourcesCompatible, TEXT("Buffer property %s has incompatible resources!"), *nosprop->DisplayName);
		}
		if (!resourcesCompatible)
			continue;
		FilteredCopies.Add(TPair<TSharedPtr<SharedResourceInfo>, TObjectPtr<UObject>>(
			info->ActiveDestinationSharedResource,
			URT ? TObjectPtr<UObject>(URT) : TObjectPtr<UObject>(UBuffer)
			));
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

void NOSResourceShareManager::ProcessCopies(nos::fb::ShowAs CopyShowAs)
{
	CheckAndUpdateResourcePinValues();
	TArray<TPair<TSharedPtr<SharedResourceInfo>, TObjectPtr<UObject>>> CopiesFiltered;
	GetActiveResourceCopiesWithShowAs(CopyShowAs, ResourceProperties, CopiesFiltered);

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
			for (auto& [ResInfo, Object] : CopiesFiltered)
			{
				if (auto* SourceTexture = Cast<UTextureRenderTarget2D>(Object))
				{
					FRHICopyTextureInfo CopyInfo;
					CopyInfo.Size = FIntVector(SourceTexture->SizeX, SourceTexture->SizeY, 1);
					FRHITexture* dst = ResInfo->DstTexture->GetRenderTargetResource()->GetRenderTargetTexture();
					FRHITexture* src = SourceTexture->GetRenderTargetResource()->GetRenderTargetTexture();
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
				else if (auto* SourceBuffer = Cast<UNOSGPUBuffer>(Object))
				{
					FRHIBuffer* dst = ResInfo->DstBuffer->Buffer.Buffer;
					FRHIBuffer* src = SourceBuffer->Buffer.Buffer;
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
					RHICmdList.CopyBufferRegion(dst, 0, src, 0, src->GetSize());
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
	ProcessCopies(nos::fb::ShowAs::INPUT_PIN);
}

void NOSResourceShareManager::OnEndFrame()
{
	ProcessCopies(nos::fb::ShowAs::OUTPUT_PIN);
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
	for(auto& [prop, info] : ResourceProperties)
	{
		if(info->ActiveDestinationSharedResource)
			ResourcesToDelete.Enqueue({ std::move(info->ActiveDestinationSharedResource), GFrameCounter });
	}
	ResourceProperties.Empty();
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
		DstBuffer->Release();
	}
}
