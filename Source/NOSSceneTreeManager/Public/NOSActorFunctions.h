/*
 * Copyright MediaZ Teknoloji A.S. All Rights Reserved.
 */

#pragma once
#include "NOSActorProperties.h"

struct NOSSCENETREEMANAGER_API NOSFunction
{
	NOSFunction(UObject* container, UFunction* function);

	UFunction* Function;
	UObject* Container;
	FString FunctionName;
	FString DisplayName;
	FString CategoryName;
	FString IdHashName;
	FGuid Id;
	uint8* Parameters = nullptr;
	std::vector<TSharedPtr<NOSProperty>> Properties;
	std::vector<TSharedPtr<NOSProperty>> OutProperties;

	flatbuffers::Offset<nos::fb::Node> Serialize(flatbuffers::FlatBufferBuilder& fbb, bool filterPins);

	void Invoke();
};

struct NOSSCENETREEMANAGER_API NOSCustomFunction
{
	FGuid Id;
	std::function<void(TMap<FGuid, std::vector<uint8>> pins)> Function;
	TMap<FGuid, std::string> Params;
	std::function<flatbuffers::Offset<nos::fb::Node>(flatbuffers::FlatBufferBuilder& fbb)> Serialize;
};

struct NOSSCENETREEMANAGER_API NOSSpawnActorFunctionPinIds
{
	NOSSpawnActorFunctionPinIds(FString UniqueFunctionName) : FunctionName(UniqueFunctionName)
	{
		ActorPinId = StringToFGuid(FunctionName + "Actor List");
		SpawnToWorldCoordsPinId = StringToFGuid(FunctionName + "Spawn To World Coordinates");
		SpawnLocationPinId = StringToFGuid(FunctionName + "Spawn Location");
		SpawnRotationPinId = StringToFGuid(FunctionName + "Spawn Rotation");
		SpawnScalePinId = StringToFGuid(FunctionName + "Spawn Scale");
	}
	
	FString FunctionName;
	FGuid ActorPinId;
	FGuid SpawnToWorldCoordsPinId;
	FGuid SpawnLocationPinId;
	FGuid SpawnRotationPinId;
	FGuid SpawnScalePinId;
};

struct NOSSCENETREEMANAGER_API NOSSpawnActorParameters
{
	bool SpawnActorToWorldCoords = false;
	FTransform SpawnTransform = FTransform::Identity;
	FName Name = NAME_None;
};

void NOSSCENETREEMANAGER_API FillSpawnActorFunctionTransformPins(flatbuffers::FlatBufferBuilder& Fbb,
                                                                std::vector<flatbuffers::Offset<nos::fb::Pin>>&SpawnPins,
                                                                NOSSpawnActorFunctionPinIds const& PinIds);

NOSSpawnActorParameters NOSSCENETREEMANAGER_API GetSpawnActorParameters(TMap<FGuid, std::vector<uint8>> const& Pins, NOSSpawnActorFunctionPinIds const& PinIds);