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

	void SetDeltaSeconds(nos::fb::vec2u deltaSeconds)
	{
		NodosDeltaTime = deltaSeconds;
	}

	FString GetDisplayName() const override
	{
		FString frameRateString;
		if (NodosDeltaTime.y() != 0 && NodosDeltaTime.x() != 0)
		{
			double frameRate = static_cast<double>(NodosDeltaTime.y()) / static_cast<double>(NodosDeltaTime.x());
			frameRateString = FString::Printf(TEXT("Fixed Frame Rate: %.2f FPS"), frameRate);
		}
		else
		{
			frameRateString = TEXT("Free run");
		}
		return TEXT("Nodos - ") + frameRateString;
	}

	/**
	 * Update FApp::CurrentTime/FApp::DeltaTime and optionally wait until the end of the frame.
	 * @return	true if the Engine's TimeStep should also be performed; false otherwise.
	 */
	bool UpdateTimeStep(class UEngine* InEngine) override
	{
		if (NodosDeltaTime.x() == 0 && NodosDeltaTime.y() == 0)
		{
			return true;
		}
		else
		{
			double deltaTimeInSeconds = static_cast<double>(NodosDeltaTime.x()) / static_cast<double>(NodosDeltaTime.y());
			if (FMath::IsNearlyZero(FApp::GetLastTime()))
			{
				FApp::SetCurrentTime(FPlatformTime::Seconds() - 0.0001);
			}
			FApp::SetCurrentTime(FApp::GetLastTime() + deltaTimeInSeconds);
			FApp::UpdateLastTime();
			FApp::SetDeltaTime(deltaTimeInSeconds);
			return false;
		}
	}

	/** The state of the CustomTimeStep. */
	ECustomTimeStepSynchronizationState GetSynchronizationState() const override
	{
		return ECustomTimeStepSynchronizationState::Synchronized;
	}

private:
	nos::fb::vec2u NodosDeltaTime{};
};

