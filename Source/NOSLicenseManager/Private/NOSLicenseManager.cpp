// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include "NOSLicenseManager.h"
#include "NOSClient.h"
#include "NOSSceneTreeManager.h"
#include "GameFramework/Actor.h"
#include "Components/SceneComponent.h"
#include "UObject/UnrealType.h"

FNOSLicenseManager::FNOSLicenseManager()
{
}

void FNOSLicenseManager::StartupModule()
{
}

void FNOSLicenseManager::ShutdownModule()
{
}

bool FNOSLicenseManager::UpdateFeature(bool registerFeature, AActor* actor, USceneComponent* component, FProperty* property,
	FString featureName, uint32_t count, FString message, uint64_t buildTime)
{
	auto& NOSClient = FModuleManager::LoadModuleChecked<FNOSClient>("NOSClient");
	auto& NOSSceneTreeManager = FModuleManager::LoadModuleChecked<FNOSSceneTreeManager>("NOSSceneTreeManager");
	void* container = component ? (void*)component : (void*)actor;

	if (!container || !NOSClient.AppServiceClient)
	{
		return false;
	}
	
	if (NOSSceneTreeManager.NOSPropertyManager.PropertiesByPropertyAndContainer.Contains({property, container}))
	{
		auto nosprop = NOSSceneTreeManager.NOSPropertyManager.PropertiesByPropertyAndContainer.FindRef({property, container});
		flatbuffers::FlatBufferBuilder mb;
		//// TODO: find a way to get a build time
		auto offset = nos::CreateAppEventOffset(mb, nos::app::CreateFeatureRegistrationUpdateDirect(mb, (nos::fb::UUID*)&nosprop->Id, TCHAR_TO_UTF8(*featureName), !registerFeature, count, TCHAR_TO_UTF8(*message), buildTime));
		mb.Finish(offset);
		auto buf = mb.Release();
		auto root = flatbuffers::GetRoot<nos::app::AppEvent>(buf.data());
		NOSClient.AppServiceClient->Send(root);
		return true;
	}
	
	return false;
}
bool FNOSLicenseManager::RegisterFeature(AActor* actor, USceneComponent* component, FProperty* property,
                                        FString featureName, uint32_t count, FString message, uint64_t buildTime)
{
	return UpdateFeature(true, actor, component, property, featureName, count, message, buildTime);
}

bool FNOSLicenseManager::UnregisterFeature(AActor* actor, USceneComponent* component, FProperty* property, FString featureName)
{
	return UpdateFeature(false, actor, component, property, featureName);
}


IMPLEMENT_MODULE(FNOSLicenseManager, NOSLicenseManager)
