// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include "NOSActorFunctions.h"
#include "NOSSceneTreeManager.h"
#include <Nodos/Nodos.h>

NOSFunction::NOSFunction(UObject* container, UFunction* function)
{
	Function = function;
	Container = container;
	FString ContainerUniqueName;
	if(container)
		ContainerUniqueName = container->GetFName().ToString();
	
	IdHashName = ContainerUniqueName + function->GetFName().ToString();
	Id = StringToFGuid(IdHashName);

	static const FName NAME_DisplayName("DisplayName");
	static const FName NAME_Category("Category");
	FunctionName = function->GetFName().ToString();

	DisplayName = function->GetDisplayNameText().ToString();
	CategoryName = function->HasMetaData(NAME_Category) ? function->GetMetaData(NAME_Category) : "Default";
}

flatbuffers::Offset<nos::fb::Node> NOSFunction::Serialize(flatbuffers::FlatBufferBuilder& fbb)
{
	std::vector<flatbuffers::Offset<nos::fb::Pin>> pins;
	for (auto property : Properties)
	{
		pins.push_back(property->Serialize(fbb));
	}

	return nos::fb::CreateNodeDirect(fbb, (nos::fb::UUID*)&Id, TCHAR_TO_UTF8(*DisplayName), TCHAR_TO_UTF8(*Function->GetClass()->GetFName().ToString()), true, &pins, 0, nos::fb::NodeContents::Job, nos::fb::CreateJob(fbb).Union(), TCHAR_TO_ANSI(*FNOSClient::AppKey), 0, TCHAR_TO_UTF8(*CategoryName));
}

void NOSFunction::Invoke() // runs in game thread
{
	if (!IsValid(Container))
	{
		UE_LOG(LogNOSSceneTreeManager, Error, TEXT("Unable to invoke function %s on invalid container"), *FunctionName);
		return;
	}
	Container->Modify();
	Container->ProcessEvent(Function, Parameters);
}

void FillSpawnActorFunctionTransformPins(flatbuffers::FlatBufferBuilder& Fbb,
	std::vector<flatbuffers::Offset<nos::fb::Pin>>& SpawnPins,
	NOSSpawnActorFunctionPinIds const& PinIds)
{
	auto uePinCategoryEntry = nos::fb::CreateMetaDataEntryDirect(Fbb, NOS_METADATA_KEY_PIN_CATEGORY, "UE PROPERTY");
	std::vector<decltype(uePinCategoryEntry)> metadataEntries = { uePinCategoryEntry };
	SpawnPins.push_back(nos::fb::CreatePinDirect(Fbb, (nos::fb::UUID*)&PinIds.SpawnToWorldCoordsPinId,
	                                            TCHAR_TO_ANSI(TEXT("Spawn To World Coordinates")),
	                                            TCHAR_TO_ANSI(TEXT("bool")), nos::fb::ShowAs::PROPERTY,
	                                            nos::fb::CanShowAs::PROPERTY_ONLY, 0, 0, 0, 0, 0, 0, 0, 0,
	                                            0, &metadataEntries, 0, nos::fb::PinContents::JobPin, 0, 0,
	                                            nos::fb::PinValueDisconnectBehavior::KEEP_LAST_VALUE,
	                                            "Set actor spawn transform with respect to the world transform. If set to false, it keeps the relative transform with respect to the parent actor."));

	SpawnPins.push_back(nos::fb::CreatePinDirect(Fbb, (nos::fb::UUID*)&PinIds.SpawnLocationPinId,
												TCHAR_TO_ANSI(TEXT("Spawn Location")),
												TCHAR_TO_ANSI(TEXT("nos.fb.vec3d")), nos::fb::ShowAs::PROPERTY, nos::fb::CanShowAs::PROPERTY_ONLY, 0, 0, 0, 0, 0, 0, 0, 0,
												0, &metadataEntries, 0, nos::fb::PinContents::JobPin));
	
	SpawnPins.push_back(nos::fb::CreatePinDirect(Fbb, (nos::fb::UUID*)&PinIds.SpawnRotationPinId,
													TCHAR_TO_ANSI(TEXT("Spawn Rotation")),
													TCHAR_TO_ANSI(TEXT("nos.fb.vec3d")), nos::fb::ShowAs::PROPERTY, nos::fb::CanShowAs::PROPERTY_ONLY, 0, 0, 0, 0, 0, 0, 0, 0,
													0, &metadataEntries, 0, nos::fb::PinContents::JobPin));
	FVector3d SpawnScale = FVector3d(1, 1, 1);
	std::vector<uint8_t> SpawnScaleData((uint8_t*)&SpawnScale, (uint8_t*)&SpawnScale + sizeof(FVector3d));
	SpawnPins.push_back(nos::fb::CreatePinDirect(Fbb, (nos::fb::UUID*)&PinIds.SpawnScalePinId,
													TCHAR_TO_ANSI(TEXT("Spawn Scale")),
													TCHAR_TO_ANSI(TEXT("nos.fb.vec3d")), nos::fb::ShowAs::PROPERTY, nos::fb::CanShowAs::PROPERTY_ONLY, 0, &SpawnScaleData, 0, 0, 0, 0, 0, 0,
													0, &metadataEntries, 0, nos::fb::PinContents::JobPin));
}

NOSSpawnActorParameters GetSpawnActorParameters(TMap<FGuid, std::vector<uint8>> const& Pins, NOSSpawnActorFunctionPinIds const& PinIds)
{
	bool SpawnToWorldCoords = *(bool*)Pins.FindChecked(PinIds.SpawnToWorldCoordsPinId).data();
	FTransform SpawnTransform = FTransform::Identity;
	SpawnTransform.SetLocation(*(FVector*)Pins.FindChecked(PinIds.SpawnLocationPinId).data());
	SpawnTransform.SetRotation(FQuat::MakeFromEuler(*(FVector*)Pins.FindChecked(PinIds.SpawnRotationPinId).data()));
	SpawnTransform.SetScale3D(*(FVector*)Pins.FindChecked(PinIds.SpawnScalePinId).data());
	return {SpawnToWorldCoords, SpawnTransform };
}
