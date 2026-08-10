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
	TObjectPtr<UTextureRenderTarget2D> DstResource = 0;
	HANDLE SharedHandle = 0;
};

struct TexturePropertyInfo
{
	TexturePropertyInfo(nos::fb::ShowAs InShowAs) : ShowAs(InShowAs) {}
	TexturePropertyInfo(const TexturePropertyInfo&) = delete;
	TexturePropertyInfo(TexturePropertyInfo&&) = delete;
	TexturePropertyInfo& operator=(const TexturePropertyInfo&) = delete;
	TexturePropertyInfo& operator=(TexturePropertyInfo&&) = delete;
	nos::fb::ShowAs ShowAs = nos::fb::ShowAs::NONE;
	UPROPERTY()
	/// This might be null, so check before use
	TSharedPtr<SharedResourceInfo> ActiveDestinationSharedResource;
};

//This class manages copy operations between textures of Nodos and unreal 2d texture target
class NOSSCENETREEMANAGER_API NOSTextureShareManager
{
//protected:
public:
	NOSTextureShareManager();
	static NOSTextureShareManager* singleton;

	static NOSTextureShareManager* GetInstance();

	~NOSTextureShareManager();
	
	nos::sys::vulkan::TTexture AddTexturePin(NOSProperty*);
	void UpdateTexturePin(NOSProperty*, nos::fb::ShowAs);
	
	/// Checks properties RT against current SharedResource destination, updates the destination if needed and returns the new destination texture value
	/// Returns nullopt if no change was needed
	std::optional<nos::sys::vulkan::TTexture> GetUpdatedTexturePinValue(NOSProperty* NosProperty);
	void UpdatePinShowAs(NOSProperty* NosProperty, nos::fb::ShowAs NewShowAs);
	void Reset();
	void TextureDestroyed(NOSProperty* texture);
	/// Refresh shared texture resources and pin orphan state without submitting GPU work.
	/// This must run before the node becomes synchronized so Nodos can make the path live.
	void UpdateTexturePinValues();
	void SetupFences(FRHICommandListImmediate& RHICmdList, nos::fb::ShowAs CopyShowAs, TMap<ID3D12Fence*, uint64_t>& SignalGroup, uint64_t frameNumber);
	void ProcessCopies(nos::fb::ShowAs CopyShowAs, uint64_t FrameNumber);
	void OnBeginFrame(uint64_t FrameNumber);
	void OnEndFrame(uint64_t FrameNumber);
	bool SwitchStateToSynced();
	void SwitchStateToIdle_GRPCThread(uint64_t LastFrameNumber);

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
	TMap<NOSProperty*, TSharedPtr<TexturePropertyInfo>> TextureProperties;

	/// Bootstrap newly created shared fences to the first real Nodos frame in the epoch.
	void InitializeFenceEpoch(uint64_t FrameNumber);
	void Initiate();
	bool bFenceEpochInitialized = false;
	class NOSGPUFailSafeRunnable* FailSafeRunnable = nullptr;
	FRunnableThread* FailSafeThread = nullptr;
};

