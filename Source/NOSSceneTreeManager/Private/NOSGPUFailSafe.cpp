// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include "NOSGPUFailSafe.h"
#include "NOSTextureShareManager.h"
#include "NOSSceneTreeManager.h"

NOSGPUFailSafeRunnable::NOSGPUFailSafeRunnable(ID3D12CommandQueue* _CmdQueue, ID3D12Device* Device) : CmdQueue(_CmdQueue)
{
	Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence));
	Event = CreateEventA(0, 0, 0, 0);
}

NOSGPUFailSafeRunnable::~NOSGPUFailSafeRunnable()
{
	bExit = true;
}

void NOSGPUFailSafeRunnable::Stop()
{
	bExit = true;
}

uint32 NOSGPUFailSafeRunnable::Run()
{
	while (!bExit)
	{
		auto Start = FPlatformTime::Seconds();
		CmdQueue->Signal(Fence, ++FenceValue);
		
		if (Fence->GetCompletedValue() < FenceValue)
		{
			Fence->SetEventOnCompletion(FenceValue, Event);
			if(WaitForSingleObject(Event, 2000) == WAIT_TIMEOUT)
			{
				UE_LOG(LogTemp, Error, TEXT("GPU 2 sec timeout, trying to recover shortly..."));
				auto NOSSceneTreeManager = &FModuleManager::LoadModuleChecked<FNOSSceneTreeManager>("NOSSceneTreeManager");
				NOSSceneTreeManager->ExecutionState = nos::app::ExecutionState::IDLE; 
				auto TextureManager = NOSTextureShareManager::GetInstance();
				if(TextureManager->InputFence && TextureManager->OutputFence)
				{
					TextureManager->ExecutionState = nos::app::ExecutionState::IDLE;
					for(int i = 0; i < 5; i++)
					{
						TextureManager->InputFence->Signal(UINT64_MAX);
						TextureManager->OutputFence->Signal(UINT64_MAX);
						FPlatformProcess::Sleep(0.2f);
					}
				}
				if(NOSSceneTreeManager->NOSClient)
				{
					flatbuffers::FlatBufferBuilder mb;
					auto offset = nos::CreateAppEventOffset(mb ,nos::app::CreateRecoverSync(mb, (nos::fb::UUID*)&FNOSClient::NodeId));
					mb.Finish(offset);
					auto buf = mb.Release();
					auto root = flatbuffers::GetRoot<nos::app::AppEvent>(buf.data());
					NOSSceneTreeManager->NOSClient->AppServiceClient->Send(*root);
				}
			}
			else
			{
				auto WaitStart = FPlatformTime::Seconds();
				FPlatformProcess::Sleep(2.0 - (WaitStart - Start));
			}
		}
	}
	return 0;
}
