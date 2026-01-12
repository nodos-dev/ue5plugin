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
		SetDeltaSeconds(CustomDeltaTime);
		return true;
	}

	/** This CustomTimeStep stop being the Engine's CustomTimeStep. */
	void Shutdown(class UEngine* InEngine) override
	{
		FApp::SetUseFixedTimeStep(false);
	}

	void SetDeltaSeconds(nos::fb::vec2u deltaSeconds)
	{
		CustomDeltaTime = deltaSeconds;
		if (CustomDeltaTime.x() == 0 || CustomDeltaTime.y() == 0)
		{
			FApp::SetUseFixedTimeStep(false);
		}
		else
		{
			FApp::SetUseFixedTimeStep(true);
			FApp::SetFixedDeltaTime(static_cast<double>(CustomDeltaTime.x()) / static_cast<double>(CustomDeltaTime.y()));
		}
	}

	/**
	 * Update FApp::CurrentTime/FApp::DeltaTime and optionally wait until the end of the frame.
	 * @return	true if the Engine's TimeStep should also be performed; false otherwise.
	 */
	bool UpdateTimeStep(class UEngine* InEngine) override
	{
		// Fixed time step will handle time update automatically
		return true;
	}

	/** The state of the CustomTimeStep. */
	ECustomTimeStepSynchronizationState GetSynchronizationState() const override
	{
		return ECustomTimeStepSynchronizationState::Synchronized;
	}

	class FNOSClient* PluginClient = nullptr;


private:

	nos::fb::vec2u CustomDeltaTime{};
};

