/*
 * Copyright MediaZ Teknoloji A.S. All Rights Reserved.
 */

#pragma once

#include "Engine/TextureRenderTarget2D.h"
#pragma warning (disable : 4800)
#pragma warning (disable : 4668)

void MemoryBarrier();
#include "Windows/AllowWindowsPlatformTypes.h"
#pragma intrinsic(_InterlockedCompareExchange64)
#define InterlockedCompareExchange64 _InterlockedCompareExchange64
#include <d3d12.h>
#include "Windows/HideWindowsPlatformTypes.h"

#include <shared_mutex>

#include "NOSActorProperties.h"
#include "Nodos/AppAPI.h" 
#include <nosFlatBuffersCommon.h>
#include "NOSClient.h"
#include "RHI.h"
#include "NOSGPUBuffer.h"

#include "nosVulkanSubsystem/Types_generated.h"

#define NOS_D3D12_ASSERT_SUCCESS(expr)                                                               \
    {                                                                                               \
        HRESULT re = (expr);                                                                        \
        while (FAILED(re))                                                                          \
        {                                                                                           \
            std::string __err = std::system_category().message(re);                                 \
            char errbuf[1024];                                                                      \
            std::snprintf(errbuf, 1024, "[%lx] %s (%s:%d)", re, __err.c_str(), __FILE__, __LINE__); \
            NOS_ABORT;                                                                           \
        }                                                                                           \
    }

enum CmdState
{
	Pending,
	Recording,
	Running,
};

struct CmdStruct
{
	struct ID3D12CommandAllocator* CmdAlloc;
	struct ID3D12GraphicsCommandList* CmdList;
	struct ID3D12Fence* CmdFence;
	uint64_t CmdFenceValue;
	CmdState State;
};


struct SyncSemaphoresExport
{
	HANDLE InputSemaphore;
	HANDLE OutputSemaphore;
};

struct SharedResourceInfo
{
	SharedResourceInfo() = default;
	SharedResourceInfo(const SharedResourceInfo&) = delete;
	SharedResourceInfo(SharedResourceInfo&&) = delete;
	SharedResourceInfo& operator=(const SharedResourceInfo&) = delete;
	SharedResourceInfo& operator=(SharedResourceInfo&&) = delete;
	~SharedResourceInfo();
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> DstTexture = 0;
	TObjectPtr<UNOSGPUBuffer> DstBuffer = 0;
	HANDLE SharedHandle = 0;
};

struct ResourcePropertyInfo
{
	ResourcePropertyInfo(nos::fb::ShowAs InShowAs) : ShowAs(InShowAs) {}
	ResourcePropertyInfo(const ResourcePropertyInfo&) = delete;
	ResourcePropertyInfo(ResourcePropertyInfo&&) = delete;
	ResourcePropertyInfo& operator=(const ResourcePropertyInfo&) = delete;
	ResourcePropertyInfo& operator=(ResourcePropertyInfo&&) = delete;
	nos::fb::ShowAs ShowAs = nos::fb::ShowAs::NONE;
	UPROPERTY()
	/// This might be null, so check before use
	TSharedPtr<SharedResourceInfo> ActiveDestinationSharedResource;
};

//This class manages copy operations between textures of Nodos and unreal 2d texture target
class NOSSCENETREEMANAGER_API NOSResourceShareManager
{
//protected:
public:
	NOSResourceShareManager();
	static NOSResourceShareManager* singleton;

	static NOSResourceShareManager* GetInstance();

	~NOSResourceShareManager();
	
	nos::Buffer AddResourcePin(NOSProperty*);
	void UpdateResourcePin(NOSProperty*, nos::fb::ShowAs);
	
	/// Checks properties RT against current SharedResource destination, updates the destination if needed and returns the new destination texture value
	/// Returns nullopt if no change was needed
	std::optional<nos::Buffer> GetUpdatedResourcePinValue(NOSProperty* NosProperty);
	void UpdatePinShowAs(NOSProperty* NosProperty, nos::fb::ShowAs NewShowAs);
	void Reset();
	void ResourceDestroyed(NOSProperty* texture);
	void SetupFences(FRHICommandListImmediate& RHICmdList, nos::fb::ShowAs CopyShowAs, TMap<ID3D12Fence*, uint64_t>& SignalGroup, uint64_t frameNumber);
	void ProcessCopies(nos::fb::ShowAs);
	void OnBeginFrame();
	void OnEndFrame();
	bool SwitchStateToSynced();
	void SwitchStateToIdle_GRPCThread(uint64_t LastFrameNumber);
	void ImportResource(nos::fb::UUID const& pinId, std::variant<nos::sys::vulkan::TTexture, nos::sys::vulkan::Buffer> res);

	class FNOSClient* NOSClient;
	
	struct ID3D12Device* Dev;
	struct ID3D12CommandQueue* CmdQueue;
	size_t CommandListCount = 10;
	std::vector<CmdStruct*> Cmds;

	TMap<FGuid, NOSProperty*> PendingCopyQueue;


	uint64_t FrameCounter = 0;
	ID3D12Fence* InputFence = nullptr;
	ID3D12Fence* OutputFence= nullptr;

	mutable FCriticalSection CriticalSectionState;
	
	SyncSemaphoresExport SyncSemaphoresExportHandles;
	
	nos::app::ExecutionState ExecutionState = nos::app::ExecutionState::IDLE;
	
	void RenewSemaphores();
private:
	UPROPERTY()
	TQueue<TPair<TSharedPtr<SharedResourceInfo>, uint32_t>> ResourcesToDelete;
	UPROPERTY()
	/// All texture property values are checked against the current SharedResource destination each frame, so we must keep them
	TMap<NOSProperty*, TSharedPtr<ResourcePropertyInfo>> ResourceProperties;

	/// This compares the current SharedResource destination against the property's current render target(UE side)
	/// If there is a difference, it creates a new SharedResource and deletes the old one
	/// Also updates the nodos pin value and orphanness state
	void CheckAndUpdateResourcePinValues();

	void Initiate();
	class NOSGPUFailSafeRunnable* FailSafeRunnable = nullptr;
	FRunnableThread* FailSafeThread = nullptr;
};

