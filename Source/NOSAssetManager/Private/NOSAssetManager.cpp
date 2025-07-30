// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include "NOSAssetManager.h"


#include "ActorFactories/ActorFactory.h"
#include "ActorFactories/ActorFactoryBasicShape.h"
#include "Subsystems/PlacementSubsystem.h"
#include "Elements/Actor/ActorElementData.h"
#include "UObject/SoftObjectPath.h"
#include "Blueprint/UserWidget.h"
#include "UObject/Object.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "StaticMeshResources.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/Engine.h"
#include "AssetRegistry/AssetData.h"
#include "Editor/EditorEngine.h"
#include "Materials/Material.h"
#include <vector>

IMPLEMENT_MODULE(FNOSAssetManager, NOSAssetManager)

template<typename T>
inline const T& FinishBuffer(flatbuffers::FlatBufferBuilder& builder, flatbuffers::Offset<T> const& offset)
{
	builder.Finish(offset);
	auto buf = builder.Release();
	return *flatbuffers::GetRoot<T>(buf.data());
}

FNOSAssetManager::FNOSAssetManager()
{
}

void FNOSAssetManager::StartupModule()
{
	NOSClient = &FModuleManager::LoadModuleChecked<FNOSClient>("NOSClient");
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	AssetRegistryModule.Get().OnAssetAdded().AddRaw(this, &FNOSAssetManager::OnAssetCreated);
	AssetRegistryModule.Get().OnAssetRemoved().AddRaw(this, &FNOSAssetManager::OnAssetDeleted);

	NOSClient->OnNOSConnected.AddLambda([this](nos::fb::Node const* appNode)
		{
			RescanAndSendAll();
		});

	ScanAssets();
	ScanUMGs();
	ScanRenderTargets();
	SetupCustomSpawns();
}

void FNOSAssetManager::ShutdownModule()
{
}

bool FNOSAssetManager::HideFromOutliner() const
{
	return true;
}

void FNOSAssetManager::OnAssetCreated(const FAssetData& createdAsset)
{
	if (!NOSClient || !NOSClient->IsConnected())
	{
		return;
	}
	RescanAndSendAll();
}

void FNOSAssetManager::OnAssetDeleted(const FAssetData& removedAsset)
{
	if (!NOSClient || !NOSClient->IsConnected())
	{
		return;
	}
	RescanAndSendAll();
}

void FNOSAssetManager::SendList(const char* ListName, const TArray<FString>& Value)
{
	if (!(NOSClient && NOSClient->IsConnected()))
		return;

	FString prefixedStringList = PrefixStringList(ListName);

	flatbuffers::FlatBufferBuilder mb;
	std::vector<std::string> NameList;
	for (const auto& name : Value)
	{
		NameList.push_back(TCHAR_TO_UTF8(*name));
	}
	
	auto offset = nos::app::CreateUpdateStringList(mb, nos::fb::CreateStringList(mb, mb.CreateString(TCHAR_TO_UTF8(*prefixedStringList)), mb.CreateVectorOfStrings(NameList)));
	mb.Finish(offset);
	auto buf = mb.Release();
	auto root = flatbuffers::GetRoot<nos::app::UpdateStringList>(buf.data());
	NOSClient->AppServiceClient->UpdateStringList(root);
}

void FNOSAssetManager::SendList(const char* ListName, const TAssetNameToPathMap& Value)
{
	TArray<FString> Names;
	for (auto [Name, _] : Value)
		Names.Add(Name);

	SendList(ListName, Names);
}

void FNOSAssetManager::SendList(const char* ListName, const TAssetNameToObjectMap& Value)
{
	TArray<FString> Names;
	for (auto [Name, _] : Value)
		Names.Add(Name);
	
	SendList(ListName, Names);
}

void FNOSAssetManager::SendAssetList()
{
	if (!(NOSClient && NOSClient->IsConnected()))
	{
		return;
	}

	TSet<FString> SpawnTags;

	for (auto& [spawnTag, AssetPath] : SpawnableAssets)
	{
		TSoftClassPtr<AActor> ActorClass = TSoftClassPtr<AActor>(FSoftObjectPath(*AssetPath.ToString()));
		UClass* LoadedAsset = ActorClass.LoadSynchronous();
		if (LoadedAsset && !LoadedAsset->HasAnyClassFlags(CLASS_Abstract))
			SpawnTags.Add(spawnTag);
	}
	for (auto& [spawnTag, x] : CustomSpawns)
	{
		SpawnTags.Add(spawnTag);
	}

	TArray<FString> SpawnTagsArray;
	for (auto& Tag : SpawnTags)
	{
		SpawnTagsArray.Add(Tag);
	}
	SpawnTagsArray.Sort();
	
	SendList("UE5_ACTOR_LIST", SpawnTagsArray);
}


void FNOSAssetManager::SendRenderTargetList()
{
	SendList("UE5_RENDER_TARGET_LIST", RenderTargets);
}

void FNOSAssetManager::SendUMGList()
{
	SendList("UE5_UMG_LIST", UMGs);
}

TSet<FTopLevelAssetPath> FNOSAssetManager::GetAssetPathsOfClass(UClass* ParentClass)
{
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	// Scanning once should be enough, since any new assets will also be added to the registry
	if (!AssetPathsScanned)
	{
		TArray< FString > ContentPaths;
		ContentPaths.Add(TEXT("/Game"));
		ContentPaths.Add(TEXT("/Script"));
		ContentPaths.Add(TEXT("/Nodos"));
		ContentPaths.Add(TEXT("/Reality"));
		AssetRegistryModule.Get().ScanPathsSynchronous(ContentPaths);
		AssetPathsScanned = true;
	}

	FTopLevelAssetPath BaseClassName = FTopLevelAssetPath(ParentClass);
	TSet< FTopLevelAssetPath > DerivedAssetPaths;
	{
		TArray< FTopLevelAssetPath > BaseNames;
		BaseNames.Add(BaseClassName);

		TSet< FTopLevelAssetPath > Excluded;
		AssetRegistryModule.Get().GetDerivedClassNames(BaseNames, Excluded, DerivedAssetPaths);
	}
	return DerivedAssetPaths;
}

void FNOSAssetManager::RescanAndSendAll()
{
	ScanAssets();
	ScanUMGs();
	ScanRenderTargets();

	SendAssetList();
	SendUMGList();
	SendRenderTargetList();
}

void FNOSAssetManager::ScanAssets(
	TAssetNameToPathMap& Map,
	UClass* ParentClass)
{
	Map.Empty();
	TSet< FTopLevelAssetPath > DerivedAssetPaths = GetAssetPathsOfClass(ParentClass);
	for (auto AssetPath : DerivedAssetPaths)
	{
		FString AssetName = AssetPath.GetAssetName().ToString();
		if (AssetName.StartsWith(TEXT("SKEL_")))
		{
			continue;
		}
		AssetName.RemoveFromEnd(TEXT("_C"), ESearchCase::CaseSensitive);
		Map.Add(AssetName, AssetPath);
	}
}

UObject* FNOSAssetManager::FindRenderTarget(const FString& Name)
{
	FSoftObjectPath AssetPath = RenderTargets.FindRef(Name);
	return AssetPath.TryLoad();
}


void FNOSAssetManager::GetAssetsByClassType(
	TAssetNameToObjectMap& Map,
	const UClass* ParentClass)
{
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	Map.Empty();
	TArray<FAssetData> ObjectList;
	AssetRegistryModule.Get().GetAssetsByClass(ParentClass->GetClassPathName(), ObjectList);
	for (const FAssetData& Object : ObjectList)
		Map.Add(Object.AssetName.ToString(), Object.GetSoftObjectPath());
}

void FNOSAssetManager::ScanRenderTargets()
{
	GetAssetsByClassType(RenderTargets, UTextureRenderTarget2D::StaticClass());
}

void FNOSAssetManager::ScanUMGs()
{
	ScanAssets(UMGs, UUserWidget::StaticClass());
}

void FNOSAssetManager::ScanAssets()
{
	ScanAssets(SpawnableAssets, AActor::StaticClass());
}

void FNOSAssetManager::SetupCustomSpawns()
{
	CustomSpawns.Add("Cube", [this](FTransform Transform)
		{
			return SpawnBasicShape(UActorFactoryBasicShape::BasicCube, Transform);
		});
	CustomSpawns.Add("Sphere", [this](FTransform Transform)
		{
			return SpawnBasicShape(UActorFactoryBasicShape::BasicSphere, Transform);
		});
	CustomSpawns.Add("Cylinder", [this](FTransform Transform)
		{
			return SpawnBasicShape(UActorFactoryBasicShape::BasicCylinder, Transform);
		});
	CustomSpawns.Add("Cone", [this](FTransform Transform)
		{
			return SpawnBasicShape(UActorFactoryBasicShape::BasicCone, Transform);
		});
	CustomSpawns.Add("Plane", [this](FTransform Transform)
		{
			return SpawnBasicShape(UActorFactoryBasicShape::BasicPlane, Transform);
		});
	CustomSpawns.Add("RealityParentTransform", [this](FTransform Transform)
		{
			FActorSpawnParameters sp;
			sp.bHideFromSceneOutliner = true;
			sp.Name = "Reality Parent Transform Actor";
			sp.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
			AActor* SpawnedActor = GEngine->GetWorldContextFromGameViewport(GEngine->GameViewport)->World()->SpawnActor(AActor::StaticClass(), &Transform, sp);
			SpawnedActor->SetActorLabel("Reality Parent Transform Actor");
			auto RootComponent = NewObject<USceneComponent>(SpawnedActor, FName("DefaultSceneRoot"));
			SpawnedActor->SetRootComponent(RootComponent);
			RootComponent->CreationMethod = EComponentCreationMethod::Instance;
			RootComponent->RegisterComponent();
			SpawnedActor->AddInstanceComponent(RootComponent);
			
			return SpawnedActor;
		});
}

AActor* FNOSAssetManager::SpawnBasicShape(FSoftObjectPath BasicShape, FTransform Transform)
{
	UWorld* CurrentWorld = GEngine->GetWorldContextFromGameViewport(GEngine->GameViewport)->World();

	FAssetData AssetData = FAssetData(LoadObject<UStaticMesh>(nullptr, *BasicShape.ToString()));
	UObject* Asset = AssetData.GetAsset();
	FActorSpawnParameters SpawnParams;
	SpawnParams.bHideFromSceneOutliner = HideFromOutliner();

	// Implemented on base of UActorFactory::CreateActor
	AActor* SpawnedActor = CurrentWorld->SpawnActor(AStaticMeshActor::StaticClass(), &Transform, SpawnParams);
	if (SpawnedActor)
	{
		FActorLabelUtilities::SetActorLabelUnique(SpawnedActor, Asset->GetName());

		// Implemented on base of UActorFactoryBasicShape::PostSpawnActor
		UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset);
		if (StaticMesh)
		{
			AStaticMeshActor* StaticMeshActor = CastChecked<AStaticMeshActor>(SpawnedActor);
			UStaticMeshComponent* StaticMeshComponent = StaticMeshActor->GetStaticMeshComponent();
			if (StaticMeshComponent)
			{
				StaticMeshComponent->UnregisterComponent();
				StaticMeshComponent->SetStaticMesh(StaticMesh);
				StaticMeshComponent->StaticMeshDerivedDataKey = StaticMesh->GetRenderData()->DerivedDataKey;
				StaticMeshComponent->SetMaterial(0, LoadObject<UMaterial>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")));
				StaticMeshComponent->RegisterComponent();
			}
		}

		SpawnedActor->PostEditChange();
		SpawnedActor->PostEditMove(true);
	}
	return SpawnedActor;
}

AActor* FNOSAssetManager::SpawnFromAssetPath(FTopLevelAssetPath AssetPath, FTransform Transform)
{
	TSoftClassPtr<AActor> ActorClass = TSoftClassPtr<AActor>(FSoftObjectPath(*AssetPath.ToString()));
	UClass* LoadedAsset = ActorClass.LoadSynchronous();
	if(!LoadedAsset)
	{
		return nullptr;
	}

	FActorSpawnParameters sp;
	sp.bHideFromSceneOutliner = HideFromOutliner();
	//todo look into hiding sp.bHideFromSceneOutliner = true;
	AActor* SpawnedActor = GEngine->GetWorldContextFromGameViewport(GEngine->GameViewport)->World()->SpawnActor(LoadedAsset, &Transform, sp);
	if (!SpawnedActor)
	{
		return nullptr;
	}
	if (!SpawnedActor->GetRootComponent())
	{
		auto RootComponent = NewObject<USceneComponent>(SpawnedActor, FName("DefaultSceneRoot"));

		SpawnedActor->SetRootComponent(RootComponent);
		RootComponent->CreationMethod = EComponentCreationMethod::Instance;
		RootComponent->RegisterComponent();
		SpawnedActor->AddInstanceComponent(RootComponent);
	}
	return SpawnedActor;
}

AActor* FNOSAssetManager::SpawnFromTag(FString SpawnTag, FTransform Transform, TMap<FString, FString> Metadata)
{	
	if (CustomSpawns.Contains(SpawnTag))
	{
		return CustomSpawns[SpawnTag](Transform);
	}
	
	if (CustomSpawnsWithMetadata.Contains(SpawnTag))
	{
		return CustomSpawnsWithMetadata[SpawnTag](Transform, Metadata);
	}

	if (SpawnableAssets.Contains(SpawnTag))
	{
		FTopLevelAssetPath AssetPath = SpawnableAssets.FindRef(SpawnTag);
		return SpawnFromAssetPath(AssetPath, Transform);
	}
	
	return nullptr;
}

UUserWidget* FNOSAssetManager::CreateUMGFromTag(FString UMGTag)
{
	if (UMGs.Contains(UMGTag))
	{
		FTopLevelAssetPath AssetPath = UMGs.FindRef(UMGTag);
		TSoftClassPtr<UUserWidget> WidgetClass = TSoftClassPtr<UUserWidget>(FSoftObjectPath(*AssetPath.ToString()));
		UClass* LoadedWidget = WidgetClass.LoadSynchronous();

		UWorld* CurrentWorld = GEngine->GetWorldContextFromGameViewport(GEngine->GameViewport)->World();
		return CreateWidget(CurrentWorld, WidgetClass.Get());
	}

	return nullptr;
}
