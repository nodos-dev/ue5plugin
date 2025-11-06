/*
 * Copyright MediaZ Teknoloji A.S. All Rights Reserved.
 */

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/ARFilter.h"
#include "EngineUtils.h"
#include "Engine/EngineTypes.h"
#include "NOSClient.h"

#include "GenericPlatform/GenericPlatformMisc.h"

#include "NOSSpawnActorParameters.h"

inline FString PrefixStringList(const FString& inString)
{
	return FNOSClient::AppKey + "_" + inString;
}

class UUserWidget;
class NOSASSETMANAGER_API FNOSAssetManager : public IModuleInterface {

public:
	//Empty constructor
	FNOSAssetManager();

	//Called on startup of the module on Unreal Engine start
	virtual void StartupModule() override;

	//Called on shutdown of the module on Unreal Engine exit
	virtual void ShutdownModule() override;

	bool HideFromOutliner() const;

	//update asset lists when a new asset is created
	void OnAssetCreated(const FAssetData& createdAsset);

	//update asset lists when an asset deleted
	void OnAssetDeleted(const FAssetData& removedAsset);

	void SendAssetList();


	void SendUMGList();

	void RescanAndSendAll();

	TSet<FTopLevelAssetPath> GetAssetPathsOfClass(UClass* ParentClass);

	//scans the assets via the asset reggistry and stores for Nodos to use them to spawn actors
	void ScanAssets();

	void SetupCustomSpawns();

	AActor* SpawnBasicShape(FSoftObjectPath BasicShape, NOSSpawnActorParameters Params);

	void ScanUMGs();

	AActor* SpawnFromTag(FString SpawnTag, NOSSpawnActorParameters Params, TMap<FString, FString> Metadata = {});

	AActor* SpawnFromAssetPath(FTopLevelAssetPath AssetPath, NOSSpawnActorParameters Params = {});

	UUserWidget* CreateUMGFromTag(FString UMGTag);

	typedef TMap<FString, FTopLevelAssetPath> TAssetNameToPathMap;
	typedef TMap<FString, FSoftObjectPath> TAssetNameToObjectMap;

	//spawnable actor assets
	TAssetNameToPathMap SpawnableAssets;

	//UMG assets list
	TAssetNameToPathMap UMGs;

	//Render target list
	TAssetNameToObjectMap RenderTargets;

	void ScanRenderTargets();
	void SendRenderTargetList();
	void GetAssetsByClassType(TAssetNameToObjectMap& Map, const UClass* ParentClass);
	UObject* FindRenderTarget(const FString& Name);

	//for custom spawnables like basic shapes(cube, sphere etc.)
	TMap<FString, TFunction<AActor*(NOSSpawnActorParameters)>> CustomSpawns;

	TMap<FString, TFunction<AActor*(NOSSpawnActorParameters, TMap<FString, FString>)>> CustomSpawnsWithMetadata;
	
	//Class communicates with Nodos
	class FNOSClient* NOSClient = nullptr;
	bool AssetPathsScanned = false;

	void ScanAssets(TAssetNameToPathMap& Map, UClass* ParentClass);
	void SendList(const char* ListName, const TArray<FString>& Value);
	void SendList(const char* ListName, const TAssetNameToPathMap& Value);
	void SendList(const char* ListName, const TAssetNameToObjectMap& Value);
};