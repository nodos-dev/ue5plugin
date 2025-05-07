/*
 * Copyright MediaZ Teknoloji A.S. All Rights Reserved.
 */

#pragma once

// std
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>

// UE
#include "Engine/EngineCustomTimeStep.h"

// Nodos Plugin
#include "Editor.h"
#include "NOSClient.h"
#include "NOSCustomTimeStep.generated.h"

UCLASS()
class NOSCLIENT_API UNOSCustomTimeStep : public UEngineCustomTimeStep
{
	GENERATED_BODY()
public:
	/** This CustomTimeStep became the Engine's CustomTimeStep. */
	bool Initialize(class UEngine* InEngine) override
	{
		return true;
	}

	/** This CustomTimeStep stop being the Engine's CustomTimeStep. */
	void Shutdown(class UEngine* InEngine) override
	{

	}

	void Step(const nos::fb::vec2u* deltaSeconds)
	{
		std::unique_lock lock(Mutex);
		if(deltaSeconds && deltaSeconds->x() != 0)
		{
			CustomDeltaTime = deltaSeconds->x() / (double)deltaSeconds->y();
		}
		else
		{
			// When Nodos was in free-run mode, it was sending 1/60 as default. But after 1.4 we are sending null to denote free-run mode.
			// To keep the same behavior, we use the default 1/60 here.
			// TODO: Support free-run mode without this hard-coded default.
			CustomDeltaTime = 1. / 60.;
		}
		IsReadyForNextStep = true;
		lock.unlock();
		CV.notify_one();
	}

	/**
	 * Update FApp::CurrentTime/FApp::DeltaTime and optionally wait until the end of the frame.
	 * @return	true if the Engine's TimeStep should also be performed; false otherwise.
	 */
	bool UpdateTimeStep(class UEngine* InEngine) override
	{
		// UpdateApplicationLastTime();
		if (FMath::IsNearlyZero(FApp::GetLastTime()))
		{
			FApp::SetCurrentTime(FPlatformTime::Seconds() - 0.0001);
		}
		FApp::SetCurrentTime(FApp::GetLastTime() + CustomDeltaTime);
		FApp::UpdateLastTime();
		FApp::SetDeltaTime(CustomDeltaTime);	
		if (PluginClient && PluginClient->IsConnected() /*&& IsGameRunning()*/)
		{
			// std::unique_lock lock(Mutex);
			// CV.wait(lock, [this] { return IsReadyForNextStep; });
			IsReadyForNextStep = false;
			return false;
		}
		else
		{
			return true;
		}
	}

	/** The state of the CustomTimeStep. */
	ECustomTimeStepSynchronizationState GetSynchronizationState() const override
	{
		if (PluginClient && PluginClient->IsConnected())
		{
			return ECustomTimeStepSynchronizationState::Synchronized;
		}
		else
		{
			return ECustomTimeStepSynchronizationState::Closed;
		}
	}

	class FNOSClient* PluginClient = nullptr;


private:
	bool IsGameRunning()
	{

			return (GEditor && GEditor->IsPlaySessionInProgress());
	}

	std::mutex Mutex;
	std::condition_variable CV;
	bool IsReadyForNextStep = false;
	double CustomDeltaTime = 1. / 50.;
};

