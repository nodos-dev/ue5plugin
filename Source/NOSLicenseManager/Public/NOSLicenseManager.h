/*
 * Copyright MediaZ Teknoloji A.S. All Rights Reserved.
 */

#pragma once
#include "Modules/ModuleInterface.h"

class NOSLICENSEMANAGER_API FNOSLicenseManager : public IModuleInterface
{
public:
	//Empty constructor
	FNOSLicenseManager();

	//Called on startup of the module on Unreal Engine start
	virtual void StartupModule() override;

	//Called on shutdown of the module on Unreal Engine exit
	virtual void ShutdownModule() override;

	bool RegisterFeature(AActor* actor, USceneComponent* component, FProperty* property, FString featureName,
	                     uint32_t count, FString message = "", uint64_t buildTime = 0);
	
	bool UnregisterFeature(AActor* actor, USceneComponent* component, FProperty* property, FString featureName);

private:

	bool UpdateFeature(bool registerFeature, AActor* actor, USceneComponent* component, FProperty* property, FString featureName, uint32_t count = 0, FString message = "", uint64_t buildTime = 0);
	
};
