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

struct ResourceInfo
{
	NOSProperty* SrcNosp = 0;
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> DstResource = 0;
	nos::fb::ShowAs ShowAs;
};

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
	bool UpdateTexturePin(NOSProperty* NosProperty, nos::sys::vulkan::TTexture& Texture);
	void UpdatePinShowAs(NOSProperty* NosProperty, nos::fb::ShowAs NewShowAs);
	void Reset();
	void TextureDestroyed(NOSProperty* texture);
	void SetupFences(FRHICommandListImmediate& RHICmdList, nos::fb::ShowAs CopyShowAs, TMap<ID3D12Fence*, uint64_t>& SignalGroup, uint64_t frameNumber);
	void ProcessCopies(nos::fb::ShowAs, TMap<NOSProperty*, ResourceInfo>& CopyMap);
	void OnBeginFrame();
	void OnEndFrame();
	bool SwitchStateToSynced();
	void SwitchStateToIdle_GRPCThread(uint64_t LastFrameNumber);

	class FNOSClient* NOSClient;
	
	struct ID3D12Device* Dev;
	struct ID3D12CommandQueue* CmdQueue;
	size_t CommandListCount = 10;
	std::vector<CmdStruct*> Cmds;

	TMap<FGuid, NOSProperty*> PendingCopyQueue;

	TQueue<TPair<TObjectPtr<UTextureRenderTarget2D>, uint32_t>> ResourcesToDelete;
	
	TMap<NOSProperty*, ResourceInfo> CopyOnTick;
	UPROPERTY()
	TMap<NOSProperty*, ResourceInfo> Copies;

	uint64_t FrameCounter = 0;
	ID3D12Fence* InputFence = nullptr;
	ID3D12Fence* OutputFence= nullptr;

	mutable FCriticalSection CriticalSectionState;
	
	SyncSemaphoresExport SyncSemaphoresExportHandles;
	
	nos::app::ExecutionState ExecutionState = nos::app::ExecutionState::IDLE;
	
	void RenewSemaphores();
private:
bool CreateTextureResource(NOSProperty*, nos::sys::vulkan::TTexture& Texture, ResourceInfo& Resource);

private:
	void Initiate();
	class NOSGPUFailSafeRunnable* FailSafeRunnable = nullptr;
	FRunnableThread* FailSafeThread = nullptr;
};

