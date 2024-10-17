// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

//Nodos plugin includes
#include "NOSSceneTreeManager.h"
#include "NOSClient.h"
#include "NOSTextureShareManager.h"
#include "NOSAssetManager.h"
#include "NOSViewportManager.h"
#include "ARenderTargetViewer.h"

//unreal engine includes
#include "EngineUtils.h"
#include "GameFramework/WorldSettings.h"
#include "Kismet2/ComponentEditorUtils.h"
#include "Kismet/GameplayStatics.h"
#include "EditorCategoryUtils.h"
#include "FileHelpers.h"
#include "ObjectEditorUtils.h"
#include "HardwareInfo.h"
#include "LevelSequence.h"
#include "PacketHandler.h"

DEFINE_LOG_CATEGORY(LogNOSSceneTreeManager);
#define LOG(x) UE_LOG(LogNOSSceneTreeManager, Display, TEXT(x))
#define LOGF(x, y) UE_LOG(LogNOSSceneTreeManager, Display, TEXT(x), y)


IMPLEMENT_MODULE(FNOSSceneTreeManager, NOSSceneTreeManager)

UWorld* FNOSSceneTreeManager::daWorld = nullptr;

static TAutoConsoleVariable<int32> CVarReloadLevelFrameCount(TEXT("Nodos.ReloadFrameCount"), 10, TEXT("Reload frame count"));

#define NOS_POPULATE_UNREAL_FUNCTIONS //uncomment if you want to see functions 

TMap<FGuid, const nos::fb::Pin*> ParsePins(const nos::fb::Node* archive)
{
	TMap<FGuid, const nos::fb::Pin*> re;
	if (!flatbuffers::IsFieldPresent(archive, nos::fb::Node::VT_PINS))
	{
		return re;
	}
	for (auto pin : *(archive->pins()))
	{
		re.Add(*(FGuid*)pin->id()->bytes()->Data(), pin);
	}
	return re;
}


FNOSSceneTreeManager::FNOSSceneTreeManager() : NOSPropertyManager(SceneTree)
{

}
void FNOSSceneTreeManager::OnMapChange(uint32 MapFlags)
{
	FString WorldName = GEditor->GetEditorWorldContext().World()->GetMapName();
	LOGF("OnMapChange with editor world contexts world %s", *WorldName);
	FNOSSceneTreeManager::daWorld = GEditor ? GEditor->GetEditorWorldContext().World() : GEngine->GetCurrentPlayWorld();
	if (!GEngine->GameViewport || !GEngine->GameViewport->IsStatEnabled("FPS"))
	{ 
		GEngine->Exec(daWorld, TEXT("Stat FPS"));
	}
	RescanScene();
	SendNodeUpdate(FNOSClient::NodeId, true);
}

void FNOSSceneTreeManager::OnNewCurrentLevel()
{
	FString WorldName = GEditor->GetEditorWorldContext().World()->GetMapName();
	LOGF("OnNewCurrentLevel with editor world contexts world %s", *WorldName);
	//todo we may need to fill these according to the level system
}

void FNOSSceneTreeManager::AddCustomFunction(NOSCustomFunction* CustomFunction)
{
	CustomFunctions.Add(CustomFunction->Id, CustomFunction);
	SendEngineFunctionUpdate();
}


void FNOSSceneTreeManager::AddToBeAddedActors()
{
	for (auto weakActorPtr : ActorsToBeAdded)
	{
		if (!weakActorPtr.IsValid())
			continue;
		SendActorAdded(weakActorPtr.Get());
	}
	ActorsToBeAdded.Empty();
}

void FNOSSceneTreeManager::DeleteToBeDeletedActors()
{
	for (auto actorGuid : ActorsToBeDeleted)
	{
		if (auto node = SceneTree.GetNodeFromActorId(actorGuid))
		{
			SendActorNodeDeleted(node);
		}
	}
	ActorsToBeDeleted.Empty();
}

void FNOSSceneTreeManager::ChangeParentActors()
{
	for (auto& [actorGuid, parentGuid] : ActorsToBeParentChanged)
	{
		SendParentChanged(actorGuid, parentGuid);
	}
	ActorsToBeParentChanged.Empty();
}

void FNOSSceneTreeManager::OnBeginFrame()
{

	if(ToggleExecutionStateToSynced)
	{
		ToggleExecutionStateToSynced = false;
		ExecutionState = nos::app::ExecutionState::SYNCED;
		if (NOSTextureShareManager::GetInstance()->SwitchStateToSynced())
		{
			SendSyncSemaphores(false);
		}
	}
	
	NOSPropertyManager.OnBeginFrame();
	NOSTextureShareManager::GetInstance()->OnBeginFrame();
}

void FNOSSceneTreeManager::OnEndFrame()
{
	NOSPropertyManager.OnEndFrame();
	NOSTextureShareManager::GetInstance()->OnEndFrame();


	if (bTwoWayBindingEnabled)
	{
		flatbuffers::FlatBufferBuilder fb;
		std::vector<flatbuffers::Offset<nos::app::AppExecutePinValueUpdate>> pinValueUpdates;

		for (auto& [id, portal] : NOSPropertyManager.PortalPinsById)
		{
			if (NOSPropertyManager.PropertiesById.Contains(portal.SourceId))
			{
				auto prop = NOSPropertyManager.PropertiesById.FindRef(portal.SourceId);
				if (!prop)
				{
					continue;
				}
				if (prop->TypeName == "nos.sys.vulkan.Texture")
				{
					continue;
				}
				auto val = prop->data;
				auto updatedVal = prop->UpdatePinValue();
				if (val.size() != updatedVal.size() || memcmp(val.data(), updatedVal.data(), val.size()) != 0)
				{
					pinValueUpdates.push_back(
								nos::app::CreateAppExecutePinValueUpdateDirect(fb, (nos::fb::UUID*)&portal.Id, &updatedVal));
					if (prop->Property)
					{
						if (auto objectContainer = prop->GetRawObjectContainer())
						{
							const FString OnChangedFunctionName = TEXT("OnChanged_") + prop->Property->GetName();
							UFunction* OnChanged = objectContainer->GetClass()->FindFunctionByName(*OnChangedFunctionName);
							if (OnChanged)
							{
								objectContainer->Modify();
								objectContainer->ProcessEvent(OnChanged, nullptr);
							}
						}

					}
				}
			}
		}

		auto offset = nos::CreateAppEventOffset(fb, nos::app::CreateExecutionCompletedDirect(fb, (nos::fb::UUID*)&FNOSClient::NodeId,
																	NOSTextureShareManager::GetInstance()->FrameCounter, 
																	&pinValueUpdates));
		fb.Finish(offset);
		auto buf = fb.Release();
		auto root = flatbuffers::GetRoot<nos::app::AppEvent>(buf.data());
		NOSClient->AppServiceClient->Send(*root);
	}
}

void FNOSSceneTreeManager::StartupModule()
{
	if (!FApp::HasProjectName())
	{
		return;
	}

	auto hwinfo = FHardwareInfo::GetHardwareInfo(NAME_RHI);
	if ("D3D12" != hwinfo)
	{
		FMessageDialog::Debugf(FText::FromString("Nodos plugin supports DirectX12 only!"));
		return;
	}
	
	bIsModuleFunctional = true; 

	NOSClient = &FModuleManager::LoadModuleChecked<FNOSClient>("NOSClient");
	NOSAssetManager = &FModuleManager::LoadModuleChecked<FNOSAssetManager>("NOSAssetManager");
	NOSViewportManager = &FModuleManager::LoadModuleChecked<FNOSViewportManager>("NOSViewportManager");

	NOSPropertyManager.NOSClient = NOSClient;

	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FNOSSceneTreeManager::Tick));
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FNOSSceneTreeManager::CheckNewLevels), .5f);
	NOSActorManager = new FNOSActorManager(this, SceneTree, NOSPropertyManager);
	//Bind to Nodos events
	NOSClient->OnNOSNodeSelected.AddRaw(this, &FNOSSceneTreeManager::OnNOSNodeSelected);
	NOSClient->OnNOSConnected.AddRaw(this, &FNOSSceneTreeManager::OnNOSConnected);
	NOSClient->OnNOSNodeUpdated.AddRaw(this, &FNOSSceneTreeManager::OnNOSNodeUpdated);
	NOSClient->OnNOSConnectionClosed.AddRaw(this, &FNOSSceneTreeManager::OnNOSConnectionClosed);
	NOSClient->OnNOSPinValueChanged.AddRaw(this, &FNOSSceneTreeManager::OnNOSPinValueChanged);
	NOSClient->OnNOSPinShowAsChanged.AddRaw(this, &FNOSSceneTreeManager::OnNOSPinShowAsChanged);
	NOSClient->OnNOSFunctionCalled.AddRaw(this, &FNOSSceneTreeManager::OnNOSFunctionCalled);
	NOSClient->OnNOSContextMenuRequested.AddRaw(this, &FNOSSceneTreeManager::OnNOSContextMenuRequested);
	NOSClient->OnNOSContextMenuCommandFired.AddRaw(this, &FNOSSceneTreeManager::OnNOSContextMenuCommandFired);
	NOSClient->OnNOSNodeImported.AddRaw(this, &FNOSSceneTreeManager::OnNOSNodeImported);
	NOSClient->OnNOSNodeRemoved.AddRaw(this, &FNOSSceneTreeManager::OnNOSNodeRemoved);
	NOSClient->OnNOSStateChanged_GRPCThread.AddRaw(this, &FNOSSceneTreeManager::OnNOSStateChanged_GRPCThread);
	NOSClient->OnNOSLoadNodesOnPaths.AddRaw(this, &FNOSSceneTreeManager::OnNOSLoadNodesOnPaths);

	FCoreDelegates::OnBeginFrame.AddRaw(this, &FNOSSceneTreeManager::OnBeginFrame);
	FCoreDelegates::OnEndFrame.AddRaw(this, &FNOSSceneTreeManager::OnEndFrame);

	
	FEditorDelegates::PostPIEStarted.AddRaw(this, &FNOSSceneTreeManager::HandleBeginPIE);
	FEditorDelegates::EndPIE.AddRaw(this, &FNOSSceneTreeManager::HandleEndPIE);
	FEditorDelegates::NewCurrentLevel.AddRaw(this, &FNOSSceneTreeManager::OnNewCurrentLevel);
	FEditorDelegates::MapChange.AddRaw(this, &FNOSSceneTreeManager::OnMapChange);

	FWorldDelegates::OnPostWorldInitialization.AddRaw(this, &FNOSSceneTreeManager::OnPostWorldInit);
	FWorldDelegates::OnPreWorldFinishDestroy.AddRaw(this, &FNOSSceneTreeManager::OnPreWorldFinishDestroy);
	FWorldDelegates::LevelAddedToWorld.AddRaw(this, &FNOSSceneTreeManager::OnLevelAddedToWorld);
	FWorldDelegates::PreLevelRemovedFromWorld.AddRaw(this, &FNOSSceneTreeManager::OnLevelRemovedFromWorld);

	GEngine->OnLevelActorAttached().AddRaw(this, &FNOSSceneTreeManager::OnActorAttached);
	GEngine->OnLevelActorDetached().AddRaw(this, &FNOSSceneTreeManager::OnActorDetached);
	
#ifdef VIEWPORT_TEXTURE
	UNOSViewportClient::NOSViewportDestroyedDelegate.AddRaw(this, &FNOSSceneTreeManager::DisconnectViewportTexture);
	//custom pins
	{
		auto nosprop = NOSPropertyManager.CreateProperty(nullptr, UNOSViewportClient::StaticClass()->FindPropertyByName("ViewportTexture"));
		if(nosprop) {
			NOSPropertyManager.CreatePortal(nosprop->Id, nos::fb::ShowAs::INPUT_PIN);
			nosprop->PinShowAs = nos::fb::ShowAs::INPUT_PIN;
			CustomProperties.Add(nosprop->Id, nosprop);
			ViewportTextureProperty = nosprop.Get();
		}
	}
#endif

	//custom functions 
	{
		NOSCustomFunction* noscf = new NOSCustomFunction;
		FString UniqueFunctionName("Toggle Two Way Binding");
		noscf->Id = StringToFGuid(UniqueFunctionName);

		noscf->Serialize = [funcid = noscf->Id, this](flatbuffers::FlatBufferBuilder& fbb)->flatbuffers::Offset<nos::fb::Node>
			{
				std::vector<uint8_t> data;
				return nos::fb::CreateNodeDirect(fbb, (nos::fb::UUID*)&funcid, "Toggle Two Way Binding", "UE5.UE5", false, true, 0, 0, nos::fb::NodeContents::Job, nos::fb::CreateJob(fbb).Union(), TCHAR_TO_ANSI(*FNOSClient::AppKey), 0, "Control"
				, 0, false, nullptr, 0, "Toggle the two way binding feature for portal pins.");
			};
		noscf->Function = [this](TMap<FGuid, std::vector<uint8>> properties)
			{
				ToggleTwoWayBinding();
			};
		CustomFunctions.Add(noscf->Id, noscf);
	}
	{
		NOSCustomFunction* noscf = new NOSCustomFunction;
		FString UniqueFunctionName("Refresh Scene Outliner");
		noscf->Id = StringToFGuid(UniqueFunctionName);

		FString AlwaysUpdatePinName("Always Update Scene Outliner");
		FGuid alwaysUpdateId = StringToFGuid(UniqueFunctionName + AlwaysUpdatePinName);
		noscf->Params.Add(alwaysUpdateId, "Always Update Scene Outliner");
		noscf->Serialize = [funcid = noscf->Id, alwaysUpdateId, this](flatbuffers::FlatBufferBuilder& fbb)->flatbuffers::Offset<nos::fb::Node>
			{
				std::vector<uint8_t> data;
				data.push_back(AlwaysUpdateOnActorSpawns ? 1 : 0);
				std::vector<flatbuffers::Offset<nos::fb::Pin>> spawnPins = {
					nos::fb::CreatePinDirect(fbb, (nos::fb::UUID*)&alwaysUpdateId, TCHAR_TO_ANSI(TEXT("Always Update Scene Outliner")), TCHAR_TO_ANSI(TEXT("bool")), nos::fb::ShowAs::PROPERTY, nos::fb::CanShowAs::PROPERTY_ONLY, "UE PROPERTY", 0, &data, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  nos::fb::PinContents::JobPin, 0, 0, nos::fb::PinValueDisconnectBehavior::KEEP_LAST_VALUE,
					"Update scene outliner when an actor is spawned instead of waiting for refreshing.\nDecreases performance for dynamic scenes."),
				};
				return nos::fb::CreateNodeDirect(fbb, (nos::fb::UUID*)&funcid, "Refresh Scene Outliner", "UE5.UE5", false, true, &spawnPins, 0, nos::fb::NodeContents::Job, nos::fb::CreateJob(fbb).Union(), TCHAR_TO_ANSI(*FNOSClient::AppKey), 0, "Control"
				, 0, false, nullptr, 0, "Add actors spawned since last refresh to the scene outliner.");
			};
		noscf->Function = [this, alwaysUpdateId = alwaysUpdateId](TMap<FGuid, std::vector<uint8>> properties)
			{
				DeleteToBeDeletedActors();
				AddToBeAddedActors();
				ChangeParentActors();
				AlwaysUpdateOnActorSpawns = static_cast<bool>(properties[alwaysUpdateId][0]);
			};
		CustomFunctions.Add(noscf->Id, noscf);
	}
	{
		NOSCustomFunction* noscf = new NOSCustomFunction;
		FString UniqueFunctionName("Spawn Actor");
		NOSSpawnActorFunctionPinIds PinIds(UniqueFunctionName);
		noscf->Id = StringToFGuid(UniqueFunctionName);
		noscf->Params.Add(PinIds.ActorPinId, "Spawn Actor");
		noscf->Serialize = [funcid = noscf->Id, PinIds](flatbuffers::FlatBufferBuilder& fbb)->flatbuffers::Offset<nos::fb::Node>
		{
			std::string empty = "None";
			auto data = std::vector<uint8_t>(empty.begin(), empty.end());
			data.push_back(0);
			
			std::vector<flatbuffers::Offset<nos::fb::Pin>> spawnPins = {
				nos::fb::CreatePinDirect(fbb, (nos::fb::UUID*)&PinIds.ActorPinId, TCHAR_TO_ANSI(TEXT("Actor List")), TCHAR_TO_ANSI(TEXT("string")), nos::fb::ShowAs::PROPERTY, nos::fb::CanShowAs::PROPERTY_ONLY, "UE PROPERTY", nos::fb::CreateVisualizerDirect(fbb, nos::fb::VisualizerType::COMBO_BOX, TCHAR_TO_UTF8(*PrefixStringList("UE5_ACTOR_LIST"))), &data, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  nos::fb::PinContents::JobPin),
			};
			FillSpawnActorFunctionTransformPins(fbb, spawnPins, PinIds);
			return nos::fb::CreateNodeDirect(fbb, (nos::fb::UUID*)&funcid, "Spawn Actor", "UE5.UE5", false, true, &spawnPins, 0, nos::fb::NodeContents::Job, nos::fb::CreateJob(fbb).Union(), TCHAR_TO_ANSI(*FNOSClient::AppKey), 0, "Control");
		};
		noscf->Function = [this, PinIds](TMap<FGuid, std::vector<uint8>> properties)
		{
			FString SpawnTag((char*)properties.FindChecked(PinIds.ActorPinId).data());
			if(SpawnTag.IsEmpty())
			{
				return;
			}
			AActor* SpawnedActor = NOSActorManager->SpawnActor(SpawnTag, GetSpawnActorParameters(properties, PinIds));
			LOGF("Actor with tag %s is spawned", *SpawnTag);
		};
		CustomFunctions.Add(noscf->Id, noscf);
	}
	{
		NOSCustomFunction* noscf = new NOSCustomFunction;
		FString UniqueFunctionName("Reload Level");
		noscf->Id = StringToFGuid(UniqueFunctionName);
		noscf->Serialize = [funcid = noscf->Id, this](flatbuffers::FlatBufferBuilder& fbb)->flatbuffers::Offset<nos::fb::Node>
			{
				return nos::fb::CreateNodeDirect(fbb, (nos::fb::UUID*)&funcid, "Reload Level", "UE5.UE5", false, true, 0, 0, nos::fb::NodeContents::Job, nos::fb::CreateJob(fbb).Union(), TCHAR_TO_ANSI(*FNOSClient::AppKey), 0, "Control"
				, 0, false, nullptr, 0, "Reload current level");
			};
		noscf->Function = [this](TMap<FGuid, std::vector<uint8>> properties)
			{
				ReloadCurrentMap();
			};
		CustomFunctions.Add(noscf->Id, noscf);
	}
	{
		NOSCustomFunction* noscf = new NOSCustomFunction;
		FString UniqueFunctionName("Spawn Render Target Viewer Actor");
		NOSSpawnActorFunctionPinIds PinIds(UniqueFunctionName);
		noscf->Id = StringToFGuid(UniqueFunctionName);
		noscf->Params.Add(PinIds.ActorPinId, "Spawn");
		noscf->Serialize = [funcid = noscf->Id, PinIds](flatbuffers::FlatBufferBuilder& fbb)->flatbuffers::Offset<nos::fb::Node>
		{
			std::string empty = "None";
			auto data = std::vector<uint8_t>(empty.begin(), empty.end());
			data.push_back(0);
			
			std::vector<flatbuffers::Offset<nos::fb::Pin>> spawnPins = {
				nos::fb::CreatePinDirect(fbb, (nos::fb::UUID*)&PinIds.ActorPinId, TCHAR_TO_ANSI(TEXT("Render Target List")), TCHAR_TO_ANSI(TEXT("string")), nos::fb::ShowAs::PROPERTY, nos::fb::CanShowAs::PROPERTY_ONLY, "UE PROPERTY", nos::fb::CreateVisualizerDirect(fbb, nos::fb::VisualizerType::COMBO_BOX, TCHAR_TO_UTF8(*PrefixStringList("UE5_RENDER_TARGET_LIST"))), &data, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  nos::fb::PinContents::JobPin),
			};
			FillSpawnActorFunctionTransformPins(fbb, spawnPins, PinIds);
			return nos::fb::CreateNodeDirect(fbb, (nos::fb::UUID*)&funcid, "Spawn Render Target Viewer Actor", "UE5.UE5", false, true, &spawnPins, 0, nos::fb::NodeContents::Job, nos::fb::CreateJob(fbb).Union(), TCHAR_TO_ANSI(*FNOSClient::AppKey), 0, "Control");
		};
		noscf->Function = [this, PinIds](TMap<FGuid, std::vector<uint8>> properties)
		{
			AActor* SpawnedActor = NOSActorManager->SpawnActor("RenderTargetViewer", GetSpawnActorParameters(properties, PinIds));
			if(auto RenderTargetViewer = Cast<ARenderTargetViewer>(SpawnedActor))
			{
				RenderTargetViewer->RenderTargetAssetName = FString((char*)properties.FindChecked(PinIds.ActorPinId).data());
				RenderTargetViewer->UpdateRenderTargetReference();

				PopulateAllChildsOfActor(RenderTargetViewer->GetActorGuid());

				auto RenderTargetView = FindFProperty<FObjectProperty>(RenderTargetViewer->GetClass(), "RenderTargetView");
				NOSPropertyManager.CreatePortal(RenderTargetView, RenderTargetViewer, nos::fb::ShowAs::OUTPUT_PIN);

				LOGF("Render target viewer actor is spawned %s", *RenderTargetViewer->GetFName().ToString());
			}
		};
		CustomFunctions.Add(noscf->Id, noscf);
	}


	LOG("NOSSceneTreeManager module successfully loaded.");
}

void FNOSSceneTreeManager::ShutdownModule()
{
	LOG("NOSSceneTreeManager module successfully shut down.");
}

bool FNOSSceneTreeManager::Tick(float dt)
{
	if (bTwoWayBindingEnabled && !bTwoWayBindingStatusSent)
	{
		nos::fb::TNodeStatusMessage TwoWayBindingStatus;
		TwoWayBindingStatus.text = "Two way binding is enabled for portal pins.";
		TwoWayBindingStatus.type = nos::fb::NodeStatusMessageType::WARNING;
		NOSClient->UENodeStatusHandler.Add("two_way_binding_status", TwoWayBindingStatus);
		bTwoWayBindingStatusSent = true;
	}
	else if (!bTwoWayBindingEnabled && bTwoWayBindingStatusSent)
	{
		NOSClient->UENodeStatusHandler.Remove("two_way_binding_status");
		bTwoWayBindingStatusSent = false;
	}
	return true;
}

bool FNOSSceneTreeManager::CheckNewLevels(float dt)
{
	auto& streamingLevels = FNOSSceneTreeManager::daWorld->GetStreamingLevels();
	for (auto* level : streamingLevels)
	{
		auto loadedLevel = level->GetLoadedLevel();
		if (!loadedLevel)
		{
			AlreadyLoadedStreamingLevels.Remove(loadedLevel);
			continue;
		}
		if (!AlreadyLoadedStreamingLevels.Contains(loadedLevel))
		{
			OnLevelAddedToWorld(loadedLevel, nullptr);
			AlreadyLoadedStreamingLevels.Add(loadedLevel);
		}
	}
	return true;
}

void FNOSSceneTreeManager::OnNOSConnected(nos::fb::Node const* appNode)
{
	if(!appNode)
	{
		return;
	}
		
	SceneTree.Root->Id = *(FGuid*)appNode->id();
	//add executable path
	if(appNode->pins() && appNode->pins()->size() > 0)
	{
		std::vector<flatbuffers::Offset<nos::PartialPinUpdate>> PinUpdates;
		flatbuffers::FlatBufferBuilder fb1;
		for (auto pin : *appNode->pins())
		{
			PinUpdates.push_back(nos::CreatePartialPinUpdate(fb1, pin->id(), 0, nos::fb::CreateOrphanStateDirect(fb1, true, "Binding in progress")));
		}
		auto offset = nos::CreatePartialNodeUpdateDirect(fb1, (nos::fb::UUID*)&FNOSClient::NodeId, nos::ClearFlags::NONE, 0, 0, 0, 0, 0, 0, 0, &PinUpdates);
		fb1.Finish(offset);
		auto buf = fb1.Release();
		auto root = flatbuffers::GetRoot<nos::PartialNodeUpdate>(buf.data());
		NOSClient->AppServiceClient->SendPartialNodeUpdate(*root);
	}
	RescanScene();
	SendNodeUpdate(FNOSClient::NodeId, false);
	if((appNode->pins() && appNode->pins()->size() > 0 )|| (appNode->contents_as_Graph()->nodes() && appNode->contents_as_Graph()->nodes()->size() > 0))
	{
		LOG("Node import request recieved on connection");
		OnNOSNodeImported(*appNode);
	}
	//else
		//SendSyncSemaphores(true);
}

void FNOSSceneTreeManager::OnNOSNodeUpdated(nos::fb::Node const& appNode)
{
	FString NodeName(appNode.name()->c_str());
	LOGF("On NOS Node updated for %s", *NodeName);
	
	if (FNOSClient::NodeId != SceneTree.Root->Id)
	{
		SceneTree.Root->Id = *(FGuid*)appNode.id();
		RescanScene();
		SendNodeUpdate(FNOSClient::NodeId);
		//SendSyncSemaphores(true);
	}
	auto texman = NOSTextureShareManager::GetInstance();
	for (auto& [id, pin] : ParsePins(&appNode))
	{
		if (texman->PendingCopyQueue.Contains(id))
		{
			auto nosprop = texman->PendingCopyQueue.FindRef(id);
			auto ShowAs = nosprop->PinShowAs;
			if(NOSPropertyManager.PropertyToPortalPin.Contains(nosprop->Id))
			{
				auto PortalId = NOSPropertyManager.PropertyToPortalPin.FindRef(nosprop->Id); 
				if(NOSPropertyManager.PortalPinsById.Contains(PortalId))
				{
					auto& Portal = NOSPropertyManager.PortalPinsById.FindChecked(PortalId);
					ShowAs = Portal.ShowAs;
				}
			}
			texman->UpdatePinShowAs(nosprop, ShowAs);
		}
	}
}

void FNOSSceneTreeManager::OnNOSNodeSelected(nos::fb::UUID const& nodeId)
{
	FGuid id = *(FGuid*)&nodeId;
	if(auto node = SceneTree.GetNode(id))
	{
		if(auto actorNode = node->GetAsActorNode())
		{
			if(actorNode->actor.Get())
				PopulateAllChildsOfActor(actorNode->actor.Get());
		}
		
		else if (PopulateNode(id))
		{
			SendNodeUpdate(id);
		}
	}
}

void FNOSSceneTreeManager::LoadNodesOnPath(FString NodePath)
{
	TArray<FString> NodeNames;
	NodePath.ParseIntoArray(NodeNames, TEXT("/"));

	auto CurrentNode = SceneTree.Root;
	for(auto nodeName : NodeNames)
	{
		//find node from the children of the current node
		for(auto child : CurrentNode->Children)
		{
			if(child && child->Name == nodeName)
			{
				LOGF("Populating node named %s", *child->Name);
				if(auto actorNode = child->GetAsActorNode())
				{
					PopulateAllChildsOfActor(actorNode->actor.Get());
				}
				else if (PopulateNode(child->Id))
				{
					SendNodeUpdate(child->Id);
				}
				CurrentNode = child;
				break;
			}
		}
	}
}

bool IsActorDisplayable(const AActor* Actor)
{
	static const FName SequencerActorTag(TEXT("SequencerActor"));

	if(Actor == nullptr)
	{
		return false;
	}

	return Actor &&
		Actor->IsEditable() &&																	// Only show actors that are allowed to be selected and drawn in editor
		Actor->IsListedInSceneOutliner() &&
		(((Actor->GetWorld() && Actor->GetWorld()->IsPlayInEditor()) || !Actor->HasAnyFlags(RF_Transient)) ||
			(Actor->ActorHasTag(SequencerActorTag))) &&
		!Actor->IsTemplate() &&																	// Should never happen, but we never want CDOs displayed
		!Actor->IsA(AWorldSettings::StaticClass()) &&											// Don't show the WorldSettings actor, even though it is technically editable
		IsValidChecked(Actor);// &&																// We don't want to show actors that are about to go away
		//!Actor->IsHidden();
}

void FNOSSceneTreeManager::OnNOSConnectionClosed()
{
	NOSActorManager->ClearActors();
	if(ExecutionState == nos::app::ExecutionState::SYNCED)
	{
		ExecutionState = nos::app::ExecutionState::IDLE;
		NOSTextureShareManager::GetInstance()->SwitchStateToIdle_GRPCThread(0);
	}
}

void FNOSSceneTreeManager::OnNOSPinValueChanged(nos::fb::UUID const& pinId, uint8_t const* data, size_t size, bool reset)
{
	FGuid Id = *(FGuid*)&pinId;
	if (CustomProperties.Contains(Id))
	{
		auto nosprop = CustomProperties.FindRef(Id);
		std::vector<uint8_t> copy(size, 0);
		memcpy(copy.data(), data, size);

		nosprop->SetPropValue((void*)copy.data(), size);
		return;
	}
	SetPropertyValue(Id, (void*)data, size);
}

void FNOSSceneTreeManager::OnNOSPinShowAsChanged(nos::fb::UUID const& Id, nos::fb::ShowAs newShowAs)
{
	FGuid pinId = *(FGuid*)&Id;
	if (CustomProperties.Contains(pinId))
	{
		UE_LOG(LogTemp, Warning, TEXT("Custom Property ShowAs changed."));
	}
	else if (NOSPropertyManager.PropertiesById.Contains(pinId) && !NOSPropertyManager.PropertyToPortalPin.Contains(pinId))
	{
		NOSPropertyManager.CreatePortal(pinId, newShowAs);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("Property with given id is not found."));
	}


	if (NOSPropertyManager.PropertiesById.Contains(pinId))
	{
		auto& NosProperty = NOSPropertyManager.PropertiesById.FindChecked(pinId);
		NosProperty->PinShowAs = newShowAs;
		if (NOSPropertyManager.PropertyToPortalPin.Contains(pinId))
		{
			auto PortalId = NOSPropertyManager.PropertyToPortalPin.FindRef(pinId);
			if (NOSPropertyManager.PortalPinsById.Contains(PortalId))
			{
				auto& Portal = NOSPropertyManager.PortalPinsById.FindChecked(PortalId);
				Portal.ShowAs = newShowAs;
				NOSClient->AppServiceClient->SendPinShowAsChange(reinterpret_cast<nos::fb::UUID&>(PortalId), newShowAs);
				NOSTextureShareManager::GetInstance()->UpdatePinShowAs(NosProperty.Get(), newShowAs);
			}
		}
	}
	if(NOSPropertyManager.PortalPinsById.Contains(pinId))
	{
		auto Portal = NOSPropertyManager.PortalPinsById.Find(pinId);
		Portal->ShowAs = newShowAs;
		if(NOSPropertyManager.PropertiesById.Contains(Portal->SourceId))
		{
			auto NosProperty = NOSPropertyManager.PropertiesById.FindRef(Portal->SourceId);
			flatbuffers::FlatBufferBuilder mb;
			auto offset = nos::CreateAppEventOffset(mb ,nos::app::CreateAppPinShowAsChanged(mb, (nos::fb::UUID*)&Portal->SourceId, newShowAs));
			mb.Finish(offset);
			auto buf = mb.Release();
			auto root = flatbuffers::GetRoot<nos::app::AppEvent>(buf.data());
			NOSClient->AppServiceClient->Send(*root);
			NOSTextureShareManager::GetInstance()->UpdatePinShowAs(NosProperty.Get(), newShowAs);
		}
	}
}

void FNOSSceneTreeManager::OnNOSFunctionCalled(nos::app::FunctionCall const& functionCall)
{
	FGuid funcId = *(FGuid*)functionCall.function_id();
	TMap<FGuid, std::vector<uint8>> properties;
	if (auto* pinVals = functionCall.pin_values())
	{
		properties.Reserve(pinVals->size());

		for (auto* pinVal : *pinVals)
		{
			properties.Add(*(FGuid*)pinVal->pin_id(), std::vector<uint8>(pinVal->value()->begin(), pinVal->value()->end()));
		}
	}
	if (CustomFunctions.Contains(funcId))
	{
		auto noscf = CustomFunctions.FindRef(funcId);
		noscf->Function(properties);
	}
	else if (RegisteredFunctions.Contains(funcId))
	{
		auto nosfunc = RegisteredFunctions.FindRef(funcId);
		uint8* Parms = (uint8*)FMemory_Alloca_Aligned(nosfunc->Function->ParmsSize, nosfunc->Function->GetMinAlignment());
		nosfunc->Parameters = Parms;
		FMemory::Memzero(Parms, nosfunc->Function->ParmsSize);

		for (TFieldIterator<FProperty> It(nosfunc->Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			FProperty* LocalProp = *It;
			checkSlow(LocalProp);
			if (!LocalProp->HasAnyPropertyFlags(CPF_ZeroConstructor))
			{
				LocalProp->InitializeValue_InContainer(Parms);
			}
		}

		for (auto [id, val] : properties)
		{
			if (NOSPropertyManager.PropertiesById.Contains(id))
			{
				auto nosprop = NOSPropertyManager.PropertiesById.FindRef(id);
				nosprop->SetPropValue((void*)val.data(), val.size(), Parms);
			}
		}

		nosfunc->Invoke();
		
		for (auto nosprop : nosfunc->OutProperties)
		{
			SendPinValueChanged(nosprop->Id, nosprop->UpdatePinValue(Parms));
		}
		nosfunc->Parameters = nullptr;
		LOG("Unreal Engine function executed.");
	}
}
void FNOSSceneTreeManager::OnNOSContextMenuRequested(nos::app::AppContextMenuRequest const& request)
{
	FVector2D pos(request.pos()->x(), request.pos()->y());
	FGuid itemId = *(FGuid*)request.item_id();
	auto instigator = request.instigator();

	if (auto node = SceneTree.GetNode(itemId))
	{
		if (auto actorNode = node->GetAsActorNode())
		{
			if (!NOSClient->IsConnected())
			{
				return;
			}

			if (actorNode->nosMetaData.Contains(NosMetadataKeys::spawnTag))
			{
				if (actorNode->nosMetaData.FindRef(NosMetadataKeys::spawnTag) == FString("RealityParentTransform"))
				{
					return;
				}
			}

			flatbuffers::FlatBufferBuilder mb;
			std::vector<flatbuffers::Offset<nos::ContextMenuItem>> actions = menuActions.SerializeActorMenuItems(mb);
			auto posx = nos::fb::vec2(pos.X, pos.Y);
			auto offset = nos::app::CreateAppContextMenuUpdateDirect(mb, (nos::fb::UUID*)&itemId, &posx, instigator, &actions);
			mb.Finish(offset);
			auto buf = mb.Release();
			auto root = flatbuffers::GetRoot<nos::app::AppContextMenuUpdate>(buf.data());
			NOSClient->AppServiceClient->SendContextMenuUpdate(*root);
		}
	}
	else if(NOSPropertyManager.PortalPinsById.Contains(itemId))
	{
		auto NosProperty = NOSPropertyManager.PortalPinsById.FindRef(itemId);
		
		flatbuffers::FlatBufferBuilder mb;
		std::vector<flatbuffers::Offset<nos::ContextMenuItem>> actions = menuActions.SerializePortalPropertyMenuItems(mb);
		auto posx = nos::fb::vec2(pos.X, pos.Y);
		auto offset = nos::app::CreateAppContextMenuUpdateDirect(mb, (nos::fb::UUID*)&itemId, &posx, instigator, &actions);
		mb.Finish(offset);
		auto buf = mb.Release();
		auto root = flatbuffers::GetRoot<nos::app::AppContextMenuUpdate>(buf.data());
		NOSClient->AppServiceClient->SendContextMenuUpdate(*root);
	}
}

void FNOSSceneTreeManager::OnNOSContextMenuCommandFired(nos::app::AppContextMenuAction const& action)
{
	FGuid itemId = *(FGuid*)action.item_id();
	uint32 actionId = action.command();
	if (auto node = SceneTree.GetNode(itemId))
	{
		if (auto actorNode = node->GetAsActorNode())
		{
			auto actor = actorNode->actor.Get();
			if (!actor)
			{
				return;
			}
			menuActions.ExecuteActorAction(actionId, this, actor);
		}
	}
	else if(NOSPropertyManager.PortalPinsById.Contains(itemId))
	{
		menuActions.ExecutePortalPropertyAction(actionId, this, itemId);
	}
}

void FNOSSceneTreeManager::OnNOSNodeRemoved()
{
	NOSActorManager->ClearActors();
	NOSTextureShareManager::GetInstance()->Reset();
	NOSClient->ReloadingLevel = CVarReloadLevelFrameCount.GetValueOnAnyThread();
	ReloadCurrentMap();
}

void FNOSSceneTreeManager::OnNOSStateChanged_GRPCThread(nos::app::ExecutionState newState)
{
	if(ExecutionState != newState)
	{
		if (newState == nos::app::ExecutionState::SYNCED)
		{
			ToggleExecutionStateToSynced = true;
		}
		else if (newState == nos::app::ExecutionState::IDLE)
		{
			ExecutionState = newState;
			NOSTextureShareManager::GetInstance()->SwitchStateToIdle_GRPCThread(0);
		}
	}
}

void FNOSSceneTreeManager::OnNOSLoadNodesOnPaths(const TArray<FString>& paths)
{
	for(auto path : paths)
	{
		LoadNodesOnPath(path);
	}
}

void FNOSSceneTreeManager::OnPostWorldInit(UWorld* World, const UWorld::InitializationValues InitValues)
{
	auto WorldContext = GEngine->GetWorldContextFromGameViewport(GEngine->GameViewport);
	if (World != WorldContext->World())
	{
		return;
	}
	FOnActorSpawned::FDelegate ActorSpawnedDelegate = FOnActorSpawned::FDelegate::CreateRaw(this, &FNOSSceneTreeManager::OnActorSpawned);
	FOnActorDestroyed::FDelegate ActorDestroyedDelegate = FOnActorDestroyed::FDelegate::CreateRaw(this, &FNOSSceneTreeManager::OnActorDestroyed);
	World->AddOnActorSpawnedHandler(ActorSpawnedDelegate);
	World->AddOnActorDestroyedHandler(ActorDestroyedDelegate);

	if(GEditor && !GEditor->IsPlaySessionInProgress())
	{
		FNOSSceneTreeManager::daWorld = GEditor->GetEditorWorldContext().World();
	}
	else
	{
		FNOSSceneTreeManager::daWorld = GEngine->GetCurrentPlayWorld();
	}
}


void FNOSSceneTreeManager::OnPreWorldFinishDestroy(UWorld* World)
{
	//TODO check if we actually need this function
	return;
#if 0
	SceneTree.Clear();
	RegisteredProperties = Pins;
	PropertiesMap.Empty();
	NOSActorManager->ReAddActorsToSceneTree();
	RescanScene(false);
	SendNodeUpdate(FNOSClient::NodeId, false);
#endif
}

void FNOSSceneTreeManager::OnLevelAddedToWorld(ULevel* level, UWorld*)
{
	if (!level)
	{
		return;
	}

	for (auto Actor : level->Actors)
	{
		if (IsActorDisplayable(Actor))
		{
			SendActorAddedOnUpdate(Actor);
		}
	}
}

void FNOSSceneTreeManager::OnLevelRemovedFromWorld(ULevel* Level, UWorld* World)
{
	if (!Level)
	{
		return;
	}

	for (auto Actor : Level->Actors)
	{
		if (IsValid(Actor))
		{
			LOGF("%s is removed becasue level is removed", *(Actor->GetFName().ToString()));
			OnActorDestroyed(Actor);
		}
	}
	AlreadyLoadedStreamingLevels.Remove(Level);
}


struct PropUpdate
{
	FGuid actorId;
	FGuid pinId;
	FString displayName;
	FString componentName;
	FString PropertyPath;
	FString ContainerPath;
	void* newVal;
	size_t newValSize;
	void* defVal;
	size_t defValSize;
	nos::fb::ShowAs pinShowAs;
	bool IsPortal;
	FString FunctionName;
	FGuid FunctionId;
	FString FunctionPropertyName;
	bool IsActorTransform;
};

struct NodeAndActorGuid
{
	FGuid ActorGuid = {};
	FGuid NodeGuid = {};
};

struct NodeSpawnInfo
{
	TMap<FString, FString> Metadata;
	FString SpawnTag;
	bool DontAttachToRealityParent = false;
};


void GetNodesSpawnedByNodos(const nos::fb::Node* node, TMap<TPair<FGuid, FGuid>, NodeSpawnInfo>& spawnedByNodos)
{
	if (flatbuffers::IsFieldPresent(node, nos::fb::Node::VT_META_DATA_MAP))
	{
		if (auto entry = node->meta_data_map()->LookupByKey(NosMetadataKeys::spawnTag))
		{
			if (auto idEntry = node->meta_data_map()->LookupByKey(NosMetadataKeys::ActorGuid))
			{
				NodeSpawnInfo spawnInfo;
				spawnInfo.SpawnTag = FString(entry->value()->c_str());
				if(auto dontAttachToRealityParentEntry = node->meta_data_map()->LookupByKey(NosMetadataKeys::DoNotAttachToRealityParent))
					spawnInfo.DontAttachToRealityParent = strcmp(dontAttachToRealityParentEntry->value()->c_str(), "true") == 0;

				for(auto* entryx: *node->meta_data_map())
				{
					if(entryx)
						spawnInfo.Metadata.Add({entryx->key()->c_str(), entryx->value()->c_str()});
				}
				spawnedByNodos.Add({FGuid(FString(idEntry->value()->c_str())), *(FGuid*)node->id()} , spawnInfo);
			}
		}
	}
	if (flatbuffers::IsFieldPresent(node->contents_as_Graph(), nos::fb::Graph::VT_NODES))
	{
		for (auto child : *node->contents_as_Graph()->nodes())
		{
			GetNodesSpawnedByNodos(child, spawnedByNodos);
		}
	}
}

void GetUMGsByNodos(const nos::fb::Node* node, TMap<TPair<FGuid, FGuid>, FString>& UMGsByNodos)
{
	if (flatbuffers::IsFieldPresent(node, nos::fb::Node::VT_META_DATA_MAP))
	{
		if (auto entry = node->meta_data_map()->LookupByKey(NosMetadataKeys::umgTag))
		{
			if (auto idEntry = node->meta_data_map()->LookupByKey(NosMetadataKeys::ActorGuid))
			{
				UMGsByNodos.Add({FGuid(FString(idEntry->value()->c_str())), *(FGuid*)node->id()} ,FString(entry->value()->c_str()));
			}
		}
	}
	if (flatbuffers::IsFieldPresent(node->contents_as_Graph(), nos::fb::Graph::VT_NODES))
	{
		for (auto child : *node->contents_as_Graph()->nodes())
		{
			GetUMGsByNodos(child, UMGsByNodos);
		}
	}
}

void GetLevelSequenceActorsByNodos(const nos::fb::Node* node, TMap<TPair<FGuid, FGuid>, FString>& LevelSequenceActorsByNodos)
{
	if (flatbuffers::IsFieldPresent(node, nos::fb::Node::VT_META_DATA_MAP))
	{
		if (auto entry = node->meta_data_map()->LookupByKey("LevelSequenceName"))
		{
			if (auto idEntry = node->meta_data_map()->LookupByKey(NosMetadataKeys::ActorGuid))
			{
				LevelSequenceActorsByNodos.Add({FGuid(FString(idEntry->value()->c_str())), *(FGuid*)node->id()} ,FString(entry->value()->c_str()));
			}
		}
	}
	if (flatbuffers::IsFieldPresent(node->contents_as_Graph(), nos::fb::Graph::VT_NODES))
	{
		for (auto child : *node->contents_as_Graph()->nodes())
		{
			GetUMGsByNodos(child, LevelSequenceActorsByNodos);
		}
	}
}

void GetNodesWithProperty(const nos::fb::Node* node, std::vector<const nos::fb::Node*>& out)
{
	if (flatbuffers::IsFieldPresent(node, nos::fb::Node::VT_PINS) && node->pins()->size() > 0)
	{
		out.push_back(node);
	}

	if (flatbuffers::IsFieldPresent(node->contents_as_Graph(), nos::fb::Graph::VT_NODES))
	{
		for (auto child : *node->contents_as_Graph()->nodes())
		{
			GetNodesWithProperty(child, out);
		}
	}
	

}

void FNOSSceneTreeManager::OnActorSpawned(AActor* InActor)
{
	if (IsActorDisplayable(InActor))
	{
		SendActorAddedOnUpdate(InActor);
	}
}

void FNOSSceneTreeManager::OnActorDestroyed(AActor* InActor)
{
	LOGF("%s is destroyed.", *(InActor->GetFName().ToString()));
	if (auto ActorNode = SceneTree.GetNode(InActor))
	{
		// delete functions
		for (auto& Func : ActorNode->Functions)
			RegisteredFunctions.Remove(Func->Id);
	}
	SendActorDeletedOnUpdate(InActor);
	ActorsToBeAdded.Remove(InActor);
	if (auto actorNode = SceneTree.GetNode(InActor))
		ActorsToBeParentChanged.Remove(actorNode->Id);
}

void FNOSSceneTreeManager::OnActorAttached(AActor* Actor, const AActor* ParentActor)
{
	LOG("Actor Attached");
	
	if(!FNOSClient::NodeId.IsValid())
	{
		return;
	}

	if(auto ActorNode = SceneTree.GetNodeFromActorId(Actor->GetActorGuid()))
	{
		if(auto OldParentActorNode = ActorNode->Parent->GetAsActorNode())
		{
			erase_if(OldParentActorNode->Children, [ActorNode](TSharedPtr<TreeNode> x) {return x->Id == ActorNode->Id;});
		}
		if (!ParentActor)
			return;
		if(auto NewParentActorNode = SceneTree.GetNodeFromActorId(ParentActor->GetActorGuid()))
		{
			NewParentActorNode->Children.push_back(ActorNode->AsShared().ToSharedPtr());
			ActorNode->Parent = NewParentActorNode;

			SendParentChangedOnUpdate(ActorNode->Id, NewParentActorNode->Id);
		}
	}
}

void FNOSSceneTreeManager::OnActorDetached(AActor* Actor, const AActor* ParentActor)
{
	LOG("Actor Detached");
	
	if(!FNOSClient::NodeId.IsValid() || !daWorld->ContainsActor(Actor) || !IsValid(Actor) || Actor->IsPendingKillPending())
	{
		return;
	}

	if(auto ActorNode = SceneTree.GetNodeFromActorId(Actor->GetActorGuid()))
	{
		auto parentFolder = SceneTree.GetFolderOrRoot(ActorNode);
		if(auto OldParentActorNode = ActorNode->Parent->GetAsActorNode())
		{
			erase_if(OldParentActorNode->Children, [ActorNode](TSharedPtr<TreeNode> x) {return x->Id == ActorNode->Id;});
		}
		if (parentFolder)
		{
			parentFolder->Children.push_back(ActorNode->AsShared().ToSharedPtr());
			ActorNode->Parent = parentFolder;

			
			SendParentChangedOnUpdate(ActorNode->Id, parentFolder->Id);

			if (AlwaysUpdateOnActorSpawns)
			{
			}
		}
	}
	else if(AlwaysUpdateOnActorSpawns)
	{
		TSharedPtr<struct ActorNode> newNode = nullptr;
		TSharedPtr<TreeNode> mostRecentParent;
		newNode = SceneTree.AddActor(Actor->GetFolder().GetPath().ToString(), Actor, mostRecentParent);
		if (!newNode)
		{
			return;
		}
		if (!NOSClient->IsConnected())
		{
			return;
		}

		flatbuffers::FlatBufferBuilder mb;
		std::vector<flatbuffers::Offset<nos::fb::Node>> graphNodes = { mostRecentParent->Serialize(mb) };
		auto offset = nos::CreatePartialNodeUpdateDirect(mb, (nos::fb::UUID*)&mostRecentParent->Parent->Id, nos::ClearFlags::NONE, 0, 0, 0, 0, 0, &graphNodes);
		mb.Finish(offset);
		auto buf = mb.Release();
		auto root = flatbuffers::GetRoot<nos::PartialNodeUpdate>(buf.data());
		NOSClient->AppServiceClient->SendPartialNodeUpdate(*root);
	}
	else
		ActorsToBeAdded.AddUnique(Actor);
}

void FNOSSceneTreeManager::OnNOSNodeImported(nos::fb::Node const& appNode)
{
	//NOSActorManager->ClearActors();
	FNOSClient::NodeId = *(FGuid*)appNode.id();
	SceneTree.Root->Id = FNOSClient::NodeId;

	auto node = &appNode;

	std::vector<flatbuffers::Offset<nos::PartialPinUpdate>> PinUpdates;
	flatbuffers::FlatBufferBuilder fb1;
	if(node->pins() && node->pins()->size() > 0)
	{
		for (auto pin : *node->pins())
		{
			PinUpdates.push_back(nos::CreatePartialPinUpdate(fb1, pin->id(), 0, nos::fb::CreateOrphanStateDirect(fb1, true, "Object not found in the scene")));
		}
	}
	auto offset = nos::CreatePartialNodeUpdateDirect(fb1, (nos::fb::UUID*)&FNOSClient::NodeId, nos::ClearFlags::NONE, 0, 0, 0, 0, 0, 0, 0, &PinUpdates);
	fb1.Finish(offset);
	auto buf = fb1.Release();
	auto root = flatbuffers::GetRoot<nos::PartialNodeUpdate>(buf.data());
	NOSClient->AppServiceClient->SendPartialNodeUpdate(*root);

	flatbuffers::FlatBufferBuilder fb3;
	auto offset2 = nos::CreatePartialNodeUpdateDirect(fb3, (nos::fb::UUID*)&FNOSClient::NodeId, nos::ClearFlags::CLEAR_FUNCTIONS | nos::ClearFlags::CLEAR_NODES);
	fb3.Finish(offset2);
	auto buf2 = fb3.Release();
	auto root2 = flatbuffers::GetRoot<nos::PartialNodeUpdate>(buf2.data());
	NOSClient->AppServiceClient->SendPartialNodeUpdate(*root2);


	std::vector<const nos::fb::Node*> nodesWithProperty;
	GetNodesWithProperty(node, nodesWithProperty);
	std::vector<PropUpdate> updates;
	for (auto nodeW : nodesWithProperty)
	{
		FGuid id = *(FGuid*)(nodeW->id());
		for (auto prop : *nodeW->pins())
		{
			
			if (flatbuffers::IsFieldPresent(prop, nos::fb::Pin::VT_META_DATA_MAP))
			{
				FString componentName;
				FString displayName;
				FString PropertyPath;
				FString ContainerPath;
				FString FunctionName;
				FGuid FunctionId;
				FString FunctionPropertyName;
				char* valcopy = nullptr;
				char* defcopy = nullptr;
				size_t valsize = 0;
				size_t defsize = 0;
				bool IsActorTransform = false;
				if (flatbuffers::IsFieldPresent(prop, nos::fb::Pin::VT_DATA))
				{
					valcopy = new char[prop->data()->size()];
					valsize = prop->data()->size();
					memcpy(valcopy, prop->data()->data(), prop->data()->size());
				}
				if (flatbuffers::IsFieldPresent(prop, nos::fb::Pin::VT_DEF))
				{
					defcopy = new char[prop->def()->size()];
					defsize = prop->def()->size();
					memcpy(defcopy, prop->def()->data(), prop->def()->size());
				}

				if (auto entry = prop->meta_data_map()->LookupByKey("PropertyPath"))
				{
					PropertyPath = FString(entry->value()->c_str());
				}
				if (auto entry = prop->meta_data_map()->LookupByKey("ContainerPath"))
				{
					ContainerPath = FString(entry->value()->c_str());
				}
				if (auto entry = prop->meta_data_map()->LookupByKey("component"))
				{
					componentName = FString(entry->value()->c_str());
				}
				if (auto entry = prop->meta_data_map()->LookupByKey("actorId"))
				{
					FString actorIdString = FString(entry->value()->c_str());
					FGuid actorId;
					if (FGuid::Parse(actorIdString, actorId))
					{
						id = actorId;
					}
				}

				if (flatbuffers::IsFieldPresent(prop, nos::fb::Pin::VT_NAME))
				{
					displayName = FString(prop->name()->c_str());
				}

				bool IsPortal = prop->contents_type() == nos::fb::PinContents::PortalPin;

				if (auto entry = prop->meta_data_map()->LookupByKey("FunctionName"))
				{
					FunctionName = FString(entry->value()->c_str());
				}

				if (auto entry = prop->meta_data_map()->LookupByKey("FunctionId"))
				{
					FString functionIdString = FString(entry->value()->c_str());
					FGuid functionId;
					if (FGuid::Parse(functionIdString, functionId))
					{
						FunctionId = functionId;
					}
				}

				if (auto entry = prop->meta_data_map()->LookupByKey("FunctionPropertyName"))
				{
					FunctionPropertyName = FString(entry->value()->c_str());
				}

				if (auto entry = prop->meta_data_map()->LookupByKey("IsActorTransform"))
				{
					IsActorTransform = true;
				}
				
				updates.push_back({ id, *(FGuid*)prop->id(),displayName, componentName, PropertyPath, ContainerPath,valcopy, valsize, defcopy, defsize, prop->show_as(), IsPortal, FunctionName, FunctionId, FunctionPropertyName, IsActorTransform});
			}

		}
	}

	TMap<TPair<FGuid, FGuid>, NodeSpawnInfo> spawnedByNodos; //old guid (imported) x spawn tag
	GetNodesSpawnedByNodos(node, spawnedByNodos);

	TMap<TPair<FGuid, FGuid>, FString> UMGsByNodos; //old guid (imported) x spawn tag
	GetUMGsByNodos(node, UMGsByNodos);

	UWorld* World = GEngine->GetWorldContextFromGameViewport(GEngine->GameViewport)->World();

	TMap<FGuid, AActor*> sceneActorMap;
	if (World)
	{
		for (TActorIterator< AActor > ActorItr(World); ActorItr; ++ActorItr)
		{
			sceneActorMap.Add(ActorItr->GetActorGuid(), *ActorItr);
		}
	}

	FGuid OldParentTransformId = {};
	for (auto [oldGuid, spawnInfo] : spawnedByNodos)
	{
		if(spawnInfo.SpawnTag == "RealityParentTransform")
		{
			AActor* spawnedActor = NOSActorManager->SpawnActor(spawnInfo.SpawnTag);
			sceneActorMap.Add(oldGuid.Key, spawnedActor); //this will map the old id with spawned actor in order to match the old properties (imported from disk)
			NOSActorManager->ParentTransformActor = NOSActorReference(spawnedActor);
			NOSActorManager->ParentTransformActor->GetRootComponent()->SetMobility(EComponentMobility::Static);
			OldParentTransformId = oldGuid.Key;
		}
	}
	if(OldParentTransformId.IsValid())
	{
		// NodeAndActorGuid nog;
		// for(auto& [key, _]: spawnedByNodos)
		// {
		// 	if(key.key == OldParentTransformId)
		// 	{
		// 		nog = key;
		// 	}	
		// }
		//spawnedByNodos.Remove(nog);
	}
		
	for (auto [oldGuid, spawnInfo] : spawnedByNodos)
	{
		if(spawnInfo.SpawnTag == "RealityParentTransform")
		{
			continue;
		}
		if (!sceneActorMap.Contains(oldGuid.Key))
		{
			///spawn
			AActor* spawnedActor = NOSActorManager->SpawnActor(spawnInfo.SpawnTag, {.SpawnActorToWorldCoords = spawnInfo.DontAttachToRealityParent}, spawnInfo.Metadata);
			if (spawnedActor)
			{
				sceneActorMap.Add(oldGuid.Key, spawnedActor); //this will map the old id with spawned actor in order to match the old properties (imported from disk)
			}
		}
	}

	for (auto [oldGuid, umgTag] : UMGsByNodos)
	{
		if (!sceneActorMap.Contains(oldGuid.Key))
		{
			////
			UUserWidget* newWidget = NOSAssetManager->CreateUMGFromTag(umgTag);
			AActor* spawnedActor = NOSActorManager->SpawnUMGRenderManager(umgTag, newWidget);
			if (spawnedActor)
			{
				sceneActorMap.Add(oldGuid.Key, spawnedActor); //this will map the old id with spawned actor in order to match the old properties (imported from disk)
			}
		}
	}

	for (auto const& update : updates)
	{
		FGuid ActorId = update.actorId;

		if (update.IsActorTransform)
		{
			if (sceneActorMap.Contains(ActorId))
			{
				auto Actor = sceneActorMap.FindRef(ActorId);
				//create custom transform property shared ptr
				TSharedPtr<NOSCustomTransformProperty> customTransformProp(new NOSCustomTransformProperty(nullptr, NOSActorReference(Actor)));
				customTransformProp->SetPropValue(update.newVal, update.newValSize);
			}
			continue;
		}
		
		UObject* Container = nullptr;
		if (sceneActorMap.Contains(ActorId))
		{
			Container = sceneActorMap.FindRef(ActorId);
		}
		if (!update.componentName.IsEmpty())
		{
			Container = FindObject<USceneComponent>(Container, *update.componentName);
		}
		if(!Container)
		{
			continue;
		}
		TSharedPtr<NOSProperty> nosprop = nullptr;

		FProperty* PropertyToUpdate = FindFProperty<FProperty>(*update.PropertyPath);
		if(!PropertyToUpdate->IsValidLowLevel())
		{
			continue;
		}
		if (!update.FunctionName.IsEmpty())
		{

			continue;
		}
		if(!update.ContainerPath.IsEmpty())
		{
			bool IsResultUObject;
			void* UnknownContainer = FindContainerFromContainerPath(Container, update.ContainerPath, IsResultUObject);
			if(!UnknownContainer)
			{
				LOGF("No container is found from the saved properties path : %s", *update.ContainerPath)
				continue;
			}
			if(IsResultUObject)
			{
				nosprop = NOSPropertyFactory::CreateProperty((UObject*) UnknownContainer, PropertyToUpdate);
				if(Cast<UActorComponent>((UObject*)UnknownContainer) && nosprop)
					nosprop->BoundComponent =  NOSComponentReference((UActorComponent*)UnknownContainer);
			}
			else
			{
				nosprop = NOSPropertyFactory::CreateProperty(nullptr, PropertyToUpdate, FString(), (uint8*)UnknownContainer);
			}
		}
		else
		{
			nosprop = NOSPropertyFactory::CreateProperty(Container, PropertyToUpdate);
			if(Cast<UActorComponent>((UObject*)Container) && nosprop)
				nosprop->BoundComponent =  NOSComponentReference((UActorComponent*)Container);
		}
		if(!nosprop)
		{
			continue;
		}
		
		if (update.newValSize > 0)
		{
			nosprop->SetPropValue(update.newVal, update.newValSize);
		}
		if (!update.displayName.IsEmpty())
		{
			nosprop->DisplayName = update.displayName;
		}
		nosprop->UpdatePinValue();
		nosprop->PinShowAs = update.pinShowAs;
		if (nosprop && update.defValSize > 0)
		{
			nosprop->default_val = std::vector<uint8>(update.defValSize, 0);
			memcpy(nosprop->default_val.data(), update.defVal, update.defValSize);
		}
	}

	RescanScene(true);
	SendNodeUpdate(FNOSClient::NodeId, false);

	PinUpdates.clear();
	flatbuffers::FlatBufferBuilder fb2;
	std::vector<NOSPortal> NewPortals;
	for (auto const& update : updates)
	{
		FGuid ActorId = update.actorId;
		
		if (update.IsPortal)
		{


			UObject* Container = nullptr;
			if (sceneActorMap.Contains(ActorId))
			{
				Container = sceneActorMap.FindRef(ActorId);
				if(auto actor = Cast<AActor>(Container))
				{
					PopulateAllChildsOfActor(actor);
					while(actor->GetSceneOutlinerParent())
					{
						actor = actor->GetSceneOutlinerParent();
						PopulateAllChildsOfActor(actor);
					}
				}
			}
			else
			{
				continue;
			}

			if (update.IsActorTransform)
			{
				if (sceneActorMap.Contains(ActorId))
				{
					auto Actor = sceneActorMap.FindRef(ActorId);
					//create custom transform property shared ptr
					TSharedPtr<NOSCustomTransformProperty> customTransformProp(new NOSCustomTransformProperty(nullptr, NOSActorReference(Actor)));
					customTransformProp->SetPropValue(update.newVal, update.newValSize);

					//find the nosprop from the actor node
					if (auto ActorNode = SceneTree.GetNodeFromActorId(Actor->GetActorGuid()))
					{
						//iterate through the properties of the actor node and find the custom transform property
						for (auto prop : ActorNode->Properties)
						{
							if (prop->IsActorTransform)
							{
								SendPinValueChanged(prop->Id, prop->data);
								//create a portal pin for the custom transform property
								PinUpdates.push_back(nos::CreatePartialPinUpdate(fb2, (nos::fb::UUID*)&update.pinId,  (nos::fb::UUID*)&prop->Id, nos::fb::CreateOrphanStateDirect(fb2, false)));
								auto NosProperty = prop;
								NOSPortal NewPortal{update.pinId ,NosProperty->Id};
								NewPortal.DisplayName = FString("");
								UObject* parent = NosProperty->GetRawObjectContainer();
								FString parentName = "";
								FString parentUniqueName = "";
								AActor* parentAsActor = nullptr;
								while (parent)
								{
									parentName = parent->GetFName().ToString();
									parentUniqueName = parent->GetFName().ToString() + "-";
									if (auto actor = Cast<AActor>(parent))
									{
										parentName = actor->GetActorLabel();
										parentAsActor = actor;
									}
									if(auto component = Cast<USceneComponent>(parent))
										parentName = component->GetName();
									parentName += ".";
									parent = parent->GetTypedOuter<AActor>();
								}
								if (parentAsActor)
								{
									while (parentAsActor->GetSceneOutlinerParent())
									{
										parentAsActor = parentAsActor->GetSceneOutlinerParent();
										if (auto actorNode = SceneTree.GetNodeFromActorId(parentAsActor->GetActorGuid()))
										{
											if (actorNode->nosMetaData.Contains(NosMetadataKeys::spawnTag))
											{
												if (actorNode->nosMetaData.FindRef(NosMetadataKeys::spawnTag) == FString("RealityParentTransform"))
												{
													break;
												}
											}
										}
										parentName = parentAsActor->GetActorLabel() + "." + parentName;
										parentUniqueName = parentAsActor->GetFName().ToString() + "-" + parentUniqueName;
									}
								}
								NewPortal.UniqueName = parentUniqueName + NosProperty->DisplayName;
								NewPortal.DisplayName =  parentName + NosProperty->DisplayName;
								NewPortal.TypeName = FString(NosProperty->TypeName.c_str());
								NewPortal.CategoryName = NosProperty->CategoryName;
								NewPortal.ShowAs = update.pinShowAs;

								NOSPropertyManager.PortalPinsById.Add(NewPortal.Id, NewPortal);
								NOSPropertyManager.PropertyToPortalPin.Add(NosProperty->Id, NewPortal.Id);
								NewPortals.push_back(NewPortal);
								NOSClient->AppServiceClient->SendPinShowAsChange((nos::fb::UUID&)NosProperty->Id, update.pinShowAs);
							}
						}
					}
				}
				continue;
			}

			if (!update.FunctionName.IsEmpty())
			{
				//find function
				if (RegisteredFunctions.Contains(update.FunctionId))
				{
					auto func = RegisteredFunctions.FindRef(update.FunctionId);
					for (auto& prop : func->Properties)
					{

						bool match = false;
						
						if(!prop->Property)
						{
							// TODO: Checking with metadata should be enough by itself
							if (prop->DisplayName == update.FunctionPropertyName)
								match = true;
							else if (auto propFuncPropName = prop->nosMetaDataMap.Find(NosMetadataKeys::FunctionPropertyName);
								propFuncPropName && *propFuncPropName == update.FunctionPropertyName)
								match = true;
						}
						else
							if (prop->Property->GetFName().ToString() == update.FunctionPropertyName)
							{
								match = true;
							}

						if (!match)
							continue;

						PinUpdates.push_back(nos::CreatePartialPinUpdate(fb2, (nos::fb::UUID*)&update.pinId,  (nos::fb::UUID*)&prop->Id, nos::fb::CreateOrphanStateDirect(fb2, false)));
						auto NosProperty = prop;
						NOSPortal NewPortal{update.pinId ,NosProperty->Id};
						NewPortal.DisplayName = FString("");
						UObject* parent = NosProperty->GetRawObjectContainer();
						FString parentName = "";
						FString parentUniqueName = "";
						AActor* parentAsActor = nullptr;
						while (parent)
						{
							parentName = parent->GetFName().ToString();
							parentUniqueName = parent->GetFName().ToString() + "-";
							if (auto actor = Cast<AActor>(parent))
							{
								parentName = actor->GetActorLabel();
								parentAsActor = actor;
							}
							if(auto component = Cast<USceneComponent>(parent))
								parentName = component->GetName();
							parentName += ".";
							parent = parent->GetTypedOuter<AActor>();
						}
						if (parentAsActor)
						{
							while (parentAsActor->GetSceneOutlinerParent())
							{
								parentAsActor = parentAsActor->GetSceneOutlinerParent();
								if (auto actorNode = SceneTree.GetNodeFromActorId(parentAsActor->GetActorGuid()))
								{
									if (actorNode->nosMetaData.Contains(NosMetadataKeys::spawnTag))
									{
										if (actorNode->nosMetaData.FindRef(NosMetadataKeys::spawnTag) == FString("RealityParentTransform"))
										{
											break;
										}
									}
								}
								parentName = parentAsActor->GetActorLabel() + "." + parentName;
								parentUniqueName = parentAsActor->GetFName().ToString() + "-" + parentUniqueName;
							}
						}

						NewPortal.UniqueName = parentUniqueName + NosProperty->DisplayName;
						NewPortal.DisplayName =  parentName + NosProperty->DisplayName;
						NewPortal.TypeName = FString(NosProperty->TypeName.c_str());
						NewPortal.CategoryName = NosProperty->CategoryName;
						NewPortal.ShowAs = update.pinShowAs;

						NOSPropertyManager.PortalPinsById.Add(NewPortal.Id, NewPortal);
						NOSPropertyManager.PropertyToPortalPin.Add(NosProperty->Id, NewPortal.Id);
						NewPortals.push_back(NewPortal);
						NOSTextureShareManager::GetInstance()->UpdatePinShowAs(NosProperty.Get(), update.pinShowAs);
						NOSClient->AppServiceClient->SendPinShowAsChange((nos::fb::UUID&)NosProperty->Id, update.pinShowAs);
					}
				}
				continue;
			}


			if (!update.componentName.IsEmpty())
			{
				Container = FindObject<USceneComponent>(Container, *update.componentName);
			}
			if(!Container)
			{
				continue;
			}
			
			FProperty* PropertyToUpdate = FindFProperty<FProperty>(*update.PropertyPath);
			if(!PropertyToUpdate)
			{
				continue;
			}
			void* UnknownContainer = Container;
			if(!update.ContainerPath.IsEmpty())
			{
				bool discard;
				UnknownContainer = FindContainerFromContainerPath(Container, update.ContainerPath, discard);
			}
			if(!UnknownContainer)
			{
				continue;
			}
			if (NOSPropertyManager.PropertiesByPropertyAndContainer.Contains({PropertyToUpdate, UnknownContainer}))
			{
				auto NosProperty = NOSPropertyManager.PropertiesByPropertyAndContainer.FindRef({PropertyToUpdate, UnknownContainer});
				PinUpdates.push_back(nos::CreatePartialPinUpdate(fb2, (nos::fb::UUID*)&update.pinId,  (nos::fb::UUID*)&NosProperty->Id, nos::fb::CreateOrphanStateDirect(fb2, false)));
				NOSPortal NewPortal{update.pinId ,NosProperty->Id};
				
				NewPortal.DisplayName = FString("");
				UObject* parent = NosProperty->GetRawObjectContainer();
				FString parentName = "";
				FString parentUniqueName = "";
				AActor* parentAsActor = nullptr;
				while (parent)
				{
					parentName = parent->GetFName().ToString();
					parentUniqueName = parent->GetFName().ToString() + "-";
					if (auto actor = Cast<AActor>(parent))
					{
						parentName = actor->GetActorLabel();
						parentAsActor = actor;
					}
					if(auto component = Cast<USceneComponent>(parent))
						parentName = component->GetName();
					parentName += ".";
					parent = parent->GetTypedOuter<AActor>();
				}
				if (parentAsActor)
				{
					while (parentAsActor->GetSceneOutlinerParent())
					{
						parentAsActor = parentAsActor->GetSceneOutlinerParent();
						if (auto actorNode = SceneTree.GetNodeFromActorId(parentAsActor->GetActorGuid()))
						{
							if (actorNode->nosMetaData.Contains(NosMetadataKeys::spawnTag))
							{
								if (actorNode->nosMetaData.FindRef(NosMetadataKeys::spawnTag) == FString("RealityParentTransform"))
								{
									break;
								}
							}
						}
						parentName = parentAsActor->GetActorLabel() + "." + parentName;
						parentUniqueName = parentAsActor->GetFName().ToString() + "-" + parentUniqueName;
					}
				}

				NewPortal.UniqueName = parentUniqueName + NosProperty->DisplayName;
				NewPortal.DisplayName =  parentName + NosProperty->DisplayName;
				NewPortal.TypeName = FString(NosProperty->TypeName.c_str());
				NewPortal.CategoryName = NosProperty->CategoryName;
				NewPortal.ShowAs = update.pinShowAs;

				NOSPropertyManager.PortalPinsById.Add(NewPortal.Id, NewPortal);
				NOSPropertyManager.PropertyToPortalPin.Add(NosProperty->Id, NewPortal.Id);
				NewPortals.push_back(NewPortal);
				NOSTextureShareManager::GetInstance()->UpdatePinShowAs(NosProperty.Get(), update.pinShowAs);
				NOSClient->AppServiceClient->SendPinShowAsChange((nos::fb::UUID&)NosProperty->Id, update.pinShowAs);
			}
			
		}

	}
	for (auto const& update : updates)
	{
		if(update.defVal)
		{
			delete[] update.defVal;
		}
		if(update.newVal)
		{
			delete[] update.newVal;
		}
	}
	if (!PinUpdates.empty())
	{
		auto offset3 = nos::CreatePartialNodeUpdateDirect(fb2, (nos::fb::UUID*)&FNOSClient::NodeId, nos::ClearFlags::NONE, 0, 0, 0, 0, 0, 0, 0, &PinUpdates);
		fb2.Finish(offset3);
		auto buf3 = fb2.Release();
		auto root3 = flatbuffers::GetRoot<nos::PartialNodeUpdate>(buf3.data());
		NOSClient->AppServiceClient->SendPartialNodeUpdate(*root3);
	}
	for (auto& Portal : NewPortals)
	{
		if(NOSPropertyManager.PropertiesById.Contains(Portal.SourceId))
		{
			flatbuffers::FlatBufferBuilder fb4;
			auto SourceProperty = NOSPropertyManager.PropertiesById.FindRef(Portal.SourceId);
			auto UpdatedMetadata = SourceProperty->SerializeMetaData(fb4);
			auto offset4 = nos::CreateAppEventOffset(fb4, nos::app::CreatePinMetadataUpdateDirect(fb4, (nos::fb::UUID*)&Portal.Id, &UpdatedMetadata  ,true));
			fb4.Finish(offset4);
			auto buf4 = fb4.Release();
			auto root4 = flatbuffers::GetRoot<nos::app::AppEvent>(buf4.data());
			NOSClient->AppServiceClient->Send(*root4);
		}
	}
	//SendSyncSemaphores(true);
	flatbuffers::FlatBufferBuilder fb5;
	auto offset4 = nos::CreateAppEventOffset(fb5, nos::CreateRefreshPortals(fb5));
	fb5.Finish(offset4);
	auto buf5 = fb5.Release();
	auto root5 = flatbuffers::GetRoot<nos::app::AppEvent>(buf5.data());
	NOSClient->AppServiceClient->Send(*root5);
	LOG("Node from Nodos successfully imported");
}

void FNOSSceneTreeManager::SetPropertyValue(FGuid pinId, void* newval, size_t size)
{
	if (!NOSPropertyManager.PropertiesById.Contains(pinId))
	{
		UE_LOG(LogTemp, Warning, TEXT("The property with given id is not found."));
		return;
	}

	auto nosprop = NOSPropertyManager.PropertiesById.FindRef(pinId);
	if(!nosprop->GetRawContainer() && !nosprop->IsActorTransform)
	{
		return;
	}
	if (!NOSPropertyManager.PropertyToPortalPin.Contains(pinId))
	{
		NOSPropertyManager.CreatePortal(pinId, nos::fb::ShowAs::PROPERTY);
	}	
	nosprop->SetPropValue(newval, size);
	
}

#ifdef VIEWPORT_TEXTURE
void FNOSSceneTreeManager::ConnectViewportTexture()
{
	auto viewport = Cast<UNOSViewportClient>(GEngine->GameViewport);
	if (IsValid(viewport) && ViewportTextureProperty)
	{
		auto nosprop = ViewportTextureProperty;
		nosprop->ObjectPtr = viewport;

		auto tex = NOSTextureShareManager::GetInstance()->AddTexturePin(nosprop);
		nosprop->data = nos::Buffer::From(tex);
	}
}

void FNOSSceneTreeManager::DisconnectViewportTexture()
{
	if (ViewportTextureProperty) {
		NOSTextureShareManager::GetInstance()->TextureDestroyed(ViewportTextureProperty);
		ViewportTextureProperty->ObjectPtr = nullptr;
		auto tex = NOSTextureShareManager::GetInstance()->AddTexturePin(ViewportTextureProperty);
		ViewportTextureProperty->data = nos::Buffer::From
		(tex);
		NOSTextureShareManager::GetInstance()->TextureDestroyed(ViewportTextureProperty);
	}
}
#endif

void FNOSSceneTreeManager::RescanScene(bool reset)
{
	if (reset)
	{
		Reset();
	}

	UWorld* World = FNOSSceneTreeManager::daWorld;

	flatbuffers::FlatBufferBuilder fbb;
	std::vector<flatbuffers::Offset<nos::fb::Node>> actorNodes;
	TArray<AActor*> ActorsInScene;
	if (IsValid(World))
	{
		
		for (TActorIterator< AActor > ActorItr(World); ActorItr; ++ActorItr)
		{
			if (!IsActorDisplayable(*ActorItr))
			{
				continue;
			}
			AActor* parent = ActorItr->GetSceneOutlinerParent();

			if (parent)
			{
				// if (SceneTree.ChildMap.Contains(parent->GetActorGuid()))
				// {
				// 	SceneTree.ChildMap.Find(parent->GetActorGuid())->Add(*ActorItr);
				// }
				// else
				// {
				// 	SceneTree.ChildMap.FindOrAdd(parent->GetActorGuid()).Add(*ActorItr);
				// }
				continue;
			}

			ActorsInScene.Add(*ActorItr);
			auto newNode = SceneTree.AddActor(ActorItr->GetFolder().GetPath().ToString(), *ActorItr);
			if (newNode)
			{
				newNode->actor = NOSActorReference(*ActorItr);
			}
		}
#ifdef VIEWPORT_TEXTURE
		ConnectViewportTexture();
#endif
	}
	LOG("SceneTree is constructed.");
}

bool PropertyVisible(FProperty* ueproperty)
{
	return !ueproperty->HasAllPropertyFlags(CPF_DisableEditOnInstance) &&
		!ueproperty->HasAllPropertyFlags(CPF_Deprecated) &&
		//!ueproperty->HasAllPropertyFlags(CPF_EditorOnly) && //? dont know what this flag does but it hides more than necessary
		ueproperty->HasAllPropertyFlags(CPF_Edit) &&
		//ueproperty->HasAllPropertyFlags(CPF_BlueprintVisible) && //? dont know what this flag does but it hides more than necessary
		ueproperty->HasAllFlags(RF_Public);
}

uint32_t SwapEndian(uint32_t num)
{
	return ((num >> 24) & 0xff) | // move byte 3 to byte 0
		((num << 8) & 0xff0000) | // move byte 1 to byte 2
		((num >> 8) & 0xff00) | // move byte 2 to byte 1
		((num << 24) & 0xff000000); // byte 0 to byte 3
}

FString UEIdToNOSIDString(FGuid Guid)
{

	uint32 A = SwapEndian(Guid.A);
	uint32 B = SwapEndian(Guid.B);
	uint32 C = SwapEndian(Guid.C);
	uint32 D = SwapEndian(Guid.D);
	FString result = "";
	result.Appendf(TEXT("%08x-%04x-%04x-%04x-%04x%08x"), A, B >> 16, B & 0xFFFF, C >> 16, C & 0xFFFF, D);
	return result;
}

TSharedPtr<NOSFunction> FNOSSceneTreeManager::AddFunctionToActorNode(ActorNode* actorNode, UFunction* UEFunction, UObject* Container)
{
	auto UEFunctionName = UEFunction->GetFName().ToString();

	if (UEFunctionName.StartsWith("OnChanged_") || UEFunctionName.StartsWith("OnLengthChanged_") || UEFunctionName.StartsWith("UserConstructionScript"))
	{
		return nullptr; // do not export user's changed handler functions
	}

	auto OwnerClass = UEFunction->GetOwnerClass();
	if (!OwnerClass || !Cast<UBlueprint>(OwnerClass->ClassGeneratedBy))
	{
		return nullptr; // export only BP functions //? what we will show in Nodos
	}

	TSharedPtr<NOSFunction> nosfunc(new NOSFunction(Container, UEFunction));

	// Parse all function parameters.
	bool bNotSupported = false;
	for (TFieldIterator<FProperty> PropIt(UEFunction); PropIt && PropIt->HasAnyPropertyFlags(CPF_Parm); ++PropIt)
	{
		if (auto nosprop = NOSPropertyManager.CreateProperty(nullptr, *PropIt, "", false))
		{
			nosprop->Id = StringToFGuid(nosfunc->IdHashName + nosprop->DisplayName);
			NOSPropertyManager.PropertiesById.Add(nosprop->Id, nosprop);

			nosfunc->Properties.push_back(nosprop);
			nosprop->PinCanShowAs = nos::fb::CanShowAs::INPUT_PIN_OR_PROPERTY;
			//RegisteredProperties.Add(nosprop->Id, nosprop);			
			if (PropIt->HasAnyPropertyFlags(CPF_OutParm))
			{
				nosfunc->OutProperties.push_back(nosprop);
				nosprop->PinCanShowAs = nos::fb::CanShowAs::OUTPUT_PIN_OR_PROPERTY;
			}
		}
		else
		{
			bNotSupported = true;
			break;
		}
	}
	if(bNotSupported)
	{
		return nullptr;
	}

	//add trigger pin manually
	TSharedPtr<NOSTriggerProperty> triggerProp = TSharedPtr<NOSTriggerProperty>(new NOSTriggerProperty());
	triggerProp->Id = StringToFGuid(nosfunc->IdHashName + "Trigger");
	triggerProp->DisplayName = actorNode->actor->GetActorLabel() + "." + nosfunc->DisplayName;
	triggerProp->PropertyName = nosfunc->IdHashName + "Trigger";
	NOSPropertyManager.PropertiesById.Add(triggerProp->Id, triggerProp);
	nosfunc->Properties.push_back(triggerProp);

	for (auto prop : nosfunc->Properties)
	{
		prop->nosMetaDataMap.Add(NosMetadataKeys::FunctionName, UEFunction->GetFName().ToString());
		prop->nosMetaDataMap.Add(NosMetadataKeys::FunctionId, nosfunc->Id.ToString());
		prop->nosMetaDataMap.Add(NosMetadataKeys::ActorDisplayName, actorNode->actor->GetActorLabel());
		if (prop->Property)
		{
			prop->nosMetaDataMap.Add(NosMetadataKeys::FunctionPropertyName, prop->Property->GetFName().ToString());
		}
		else
		{
			prop->nosMetaDataMap.Add(NosMetadataKeys::FunctionPropertyName, "Trigger");
		}
		prop->nosMetaDataMap.Add(NosMetadataKeys::actorId, actorNode->actor->GetActorGuid().ToString());

		prop->IsFunctionProp = true;
		prop->FunctionId = nosfunc->Id;
	}

	actorNode->Functions.push_back(nosfunc);
	RegisteredFunctions.Add(nosfunc->Id, nosfunc);
	return nosfunc;
}

bool FNOSSceneTreeManager::PopulateNode(FGuid nodeId)
{
	auto treeNode = SceneTree.GetNode(nodeId);

	if (!treeNode || !treeNode->NeedsReload)
	{
		return false;
	}
	
	LOGF("Populating node with id %s", *nodeId.ToString());
	if (auto actorNode = treeNode->GetAsActorNode())
	{
		if (!IsValid(actorNode->actor.Get()))
		{
			return false;
		}
		bool ColoredChilds = false;
		if(actorNode->nosMetaData.Contains(NosMetadataKeys::NodeColor))
		{
			ColoredChilds = true;
		}
		auto ActorClass = actorNode->actor->GetClass();

		//ITERATE PROPERTIES BEGIN
		class FProperty* AProperty = ActorClass->PropertyLink;
		while (AProperty != nullptr)
		{
			FName CategoryName = FObjectEditorUtils::GetCategoryFName(AProperty);

			UClass* Class = ActorClass;

			if (FEditorCategoryUtils::IsCategoryHiddenFromClass(Class, CategoryName.ToString()) || !PropertyVisible(AProperty))
			{
				AProperty = AProperty->PropertyLinkNext;
				continue;
			}
			auto nosprop = NOSPropertyManager.CreateProperty(actorNode->actor.Get(), AProperty, FString(""));
			if (!nosprop)
			{
				AProperty = AProperty->PropertyLinkNext;
				continue;
			}
			//RegisteredProperties.Add(nosprop->Id, nosprop);
			actorNode->Properties.push_back(nosprop);

			for (auto it : nosprop->childProperties)
			{
				//RegisteredProperties.Add(it->Id, it);
				actorNode->Properties.push_back(it);
			}

			AProperty = AProperty->PropertyLinkNext;
		}

		//add custom transform property
		TSharedPtr<NOSCustomTransformProperty> customTransformProp(new NOSCustomTransformProperty(nullptr, actorNode->actor));
		actorNode->Properties.push_back(customTransformProp);
		NOSPropertyManager.PropertiesById.Add(customTransformProp->Id, customTransformProp);
		customTransformProp->UpdatePinValue();

		auto Components = actorNode->actor->GetComponents();

		TSet<TSharedPtr<NOSProperty>> PropsWithFunctions;

		for(auto NosProp : actorNode->Properties)
		{
			if(NosProp->EditConditionProperty)
			{
				for(auto prop : actorNode->Properties)
				{
					if(prop->Property == NosProp->EditConditionProperty)
					{
						
						NosProp->nosMetaDataMap.Add(NosMetadataKeys::EditConditionPropertyId, UEIdToNOSIDString(prop->Id));
						
						UE_LOG(LogNOSSceneTreeManager, Warning, TEXT("%s has edit condition named %s with pind id %s"), *NosProp->DisplayName, *prop->DisplayName,
							*NosProp->nosMetaDataMap[NosMetadataKeys::EditConditionPropertyId]);
					}
				}
			}
			if(NosProp->FunctionContainerClass)
			{
				PropsWithFunctions.Add(NosProp);
			}
		}

		
		//ITERATE PROPERTIES END

#ifdef NOS_POPULATE_UNREAL_FUNCTIONS 
		//ITERATE FUNCTIONS BEGIN
		auto ActorComponent = actorNode->actor->GetRootComponent();
		for (TFieldIterator<UFunction> FuncIt(ActorClass, EFieldIteratorFlags::IncludeSuper); FuncIt; ++FuncIt)
		{
			UFunction* UEFunction = *FuncIt;
			if(ActorClass->FindFunctionByName(UEFunction->GetFName()) != UEFunction)
				continue;
			UObject* Container =  actorNode->actor.Get();
			// LOGF("function with name %s is a function, indeed!", *UEFunction->GetFName().ToString());
			if (UEFunction->HasAllFunctionFlags(FUNC_BlueprintCallable /*| FUNC_Public*/) /*&&
				!UEFunction->HasAllFunctionFlags(FUNC_Event)*/) //commented out because custom events are seems to public? and not has FUNC_Event flags?
			{
				AddFunctionToActorNode(actorNode, UEFunction, Container);
			}
		}

		for (auto NosProp : PropsWithFunctions)
		{
			if(!NosProp->FunctionContainerClass)
				continue;
			for (TFieldIterator<UFunction> FuncIt(NosProp->FunctionContainerClass, EFieldIteratorFlags::IncludeSuper); FuncIt; ++FuncIt)
			{
				UFunction* UEFunction = *FuncIt;
				
				UObject* obj = NosProp->GetRawObjectContainer();
				FObjectProperty* prop = CastField<FObjectProperty>(NosProp->Property);
				UObject* FunctionContainer = prop->GetObjectPropertyValue_InContainer(obj);
				
				if(!FunctionContainer)
					continue;
				
				LOGF("function with name %s is a function, indeed! of a widgett!!", *UEFunction->GetFName().ToString());
				if (UEFunction->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_Event))
				{
					if(auto nosFunc = AddFunctionToActorNode(actorNode, UEFunction, FunctionContainer))
					{
						nosFunc->CategoryName = FunctionContainer->GetFName().ToString();	
					}
				}
			}
		}
		
		//ITERATE FUNCTIONS END
#endif
		//ITERATE CHILD COMPONENTS TO SHOW BEGIN
		erase_if(actorNode->Children, [](TSharedPtr<TreeNode> treeNode){return treeNode->GetAsSceneComponentNode() != nullptr;});
		
		TArray<AActor*> ChildActors;
		actorNode->actor->GetAttachedActors(ChildActors);
		for (auto child : ChildActors)
		{
			if(child->IsValidLowLevel() && !SceneTree.GetNode(child))
			{
				SceneTree.AddActor(actorNode, child);
			}
		}
		

		AActor* ActorContext = actorNode->actor.Get();
		TSet<UActorComponent*> ComponentsToAdd(ActorContext->GetComponents());

		const bool bHideConstructionScriptComponentsInDetailsView = false; //GetDefault<UBlueprintEditorSettings>()->bHideConstructionScriptComponentsInDetailsView;
		auto ShouldAddInstancedActorComponent = [bHideConstructionScriptComponentsInDetailsView](UActorComponent* ActorComp, USceneComponent* ParentSceneComp)
		{
			// Exclude nested DSOs attached to BP-constructed instances, which are not mutable.
			return (ActorComp != nullptr
				&& (!ActorComp->IsVisualizationComponent())
				&& (ActorComp->CreationMethod != EComponentCreationMethod::UserConstructionScript || !bHideConstructionScriptComponentsInDetailsView)
				&& (ParentSceneComp == nullptr || !ParentSceneComp->IsCreatedByConstructionScript() || !ActorComp->HasAnyFlags(RF_DefaultSubObject)))
				&& (ActorComp->CreationMethod != EComponentCreationMethod::Native || FComponentEditorUtils::GetPropertyForEditableNativeComponent(ActorComp));
		};

		// Filter the components by their visibility
		for (TSet<UActorComponent*>::TIterator It(ComponentsToAdd.CreateIterator()); It; ++It)
		{
			UActorComponent* ActorComp = *It;
			USceneComponent* SceneComp = Cast<USceneComponent>(ActorComp);
			USceneComponent* ParentSceneComp = SceneComp != nullptr ? SceneComp->GetAttachParent() : nullptr;
			if (!ShouldAddInstancedActorComponent(ActorComp, ParentSceneComp))
			{
				It.RemoveCurrent();
			}
		}

		TArray<TSharedPtr<SceneComponentNode>> OutArray;

		TFunction<void(USceneComponent*, TSharedPtr<TreeNode>)> AddInstancedComponentsRecursive = [&, this](USceneComponent* Component, TSharedPtr<TreeNode> ParentHandle)
		{
			if (Component != nullptr)
			{
				for (USceneComponent* ChildComponent : Component->GetAttachChildren())
				{
					if (ComponentsToAdd.Contains(ChildComponent) && ChildComponent->GetOwner() == Component->GetOwner())
					{
						ComponentsToAdd.Remove(ChildComponent);
						TSharedPtr<SceneComponentNode> NewParentHandle = nullptr;
						if (ParentHandle->GetAsActorNode())
						{
							// TODO: TSharedFromThis
							auto ParentAsActorNode = StaticCastSharedPtr<ActorNode>(ParentHandle);
							NewParentHandle = this->SceneTree.AddSceneComponent(ParentAsActorNode.Get(), ChildComponent);
						}
						else if (ParentHandle->GetAsSceneComponentNode())
						{
							auto ParentAsSceneComponentNode = StaticCastSharedPtr<SceneComponentNode>(ParentHandle);
							NewParentHandle = this->SceneTree.AddSceneComponent(ParentAsSceneComponentNode, ChildComponent);
						}


						if (!NewParentHandle)
						{
							LOG("A Child node other than actor or component is present!");
							continue;
						}
						if(ColoredChilds)
						{
							NewParentHandle->nosMetaData.Add("NodeColor", HEXCOLOR_Reality_Node);
						}
						NewParentHandle->Children.clear();
						OutArray.Add(NewParentHandle);

						AddInstancedComponentsRecursive(ChildComponent, NewParentHandle);
					}
				}
			}
		};

		USceneComponent* RootComponent = ActorContext->GetRootComponent();

		// Add the root component first
		if (RootComponent != nullptr)
		{
			// We want this to be first every time, so remove it from the set of components that will be added later
			ComponentsToAdd.Remove(RootComponent);

			// Add the root component first
			auto RootHandle = SceneTree.AddSceneComponent(actorNode, RootComponent);
			if(RootHandle)
			{
				if(ColoredChilds)
				{
					RootHandle->nosMetaData.Add("NodeColor", HEXCOLOR_Reality_Node);
				}
				actorNode->nosMetaData.FindOrAdd("ViewPinsOf") = UEIdToNOSIDString(RootHandle->Id); 
				// Clear the loading child
				RootHandle->Children.clear();
				OutArray.Add(RootHandle);
			}
			// Recursively add
			AddInstancedComponentsRecursive(RootComponent, RootHandle);
		}

		// Sort components by type (always put scene components first in the tree)
		ComponentsToAdd.Sort([](const UActorComponent& A, const UActorComponent& /* B */)
			{
				return A.IsA<USceneComponent>();
			});

		// Now add any remaining instanced owned components not already added above. This will first add any
		// unattached scene components followed by any instanced non-scene components owned by the Actor instance.
		for (UActorComponent* ActorComp : ComponentsToAdd)
		{
			// Create new subobject data with the original data as their parent.
			//OutArray.Add(SceneTree.AddSceneComponent(componentNode, ActorComp)); //TODO scene tree add actor components
		}
		//ITERATE CHILD COMPONENTS TO SHOW END

		treeNode->NeedsReload = false;
		return true;
	}
	else if (treeNode->GetAsSceneComponentNode())
	{
		auto Component = treeNode->GetAsSceneComponentNode()->sceneComponent;
		auto Actor = Component->GetOwner();
		auto ComponentNode = treeNode->GetAsSceneComponentNode();
		auto ComponentClass = Component->GetClass();

		for (FProperty* Property = ComponentClass->PropertyLink; Property; Property = Property->PropertyLinkNext)
		{

			FName CategoryName = FObjectEditorUtils::GetCategoryFName(Property);
			UClass* Class = Component->GetClass();

			if (FEditorCategoryUtils::IsCategoryHiddenFromClass(Class, CategoryName.ToString()) || !PropertyVisible(Property))
			{
				continue;
			}

			//check if the property is part of transform
			if (Property->GetFName() == FName("RelativeLocation") ||
				Property->GetFName() == FName("RelativeRotation") ||
				Property->GetFName() == FName("RelativeScale3D"))
			{
				continue;
			}

			auto nosprop = NOSPropertyManager.CreateProperty(Component.Get(), Property, FString(""));
			if (nosprop)
			{
				//RegisteredProperties.Add(nosprop->Id, nosprop);
				ComponentNode->Properties.push_back(nosprop);

				for (auto it : nosprop->childProperties)
				{
					//RegisteredProperties.Add(it->Id, it);
					ComponentNode->Properties.push_back(it);
				}

			}
		}
		
		for(auto NosProp : ComponentNode->Properties)
		{
			if(NosProp->EditConditionProperty)
			{
				for(auto prop : ComponentNode->Properties)
				{
					if(prop->Property == NosProp->EditConditionProperty)
					{
						NosProp->nosMetaDataMap.Add(NosMetadataKeys::EditConditionPropertyId, UEIdToNOSIDString(prop->Id));
						UE_LOG(LogNOSSceneTreeManager, Warning, TEXT("%s has edit condition named %s with pind id %s"), *NosProp->DisplayName, *prop->DisplayName,
							*NosProp->nosMetaDataMap[NosMetadataKeys::EditConditionPropertyId]);
					}
				}
			}
			NosProp->BoundComponent = NOSComponentReference(ComponentNode->sceneComponent);
		}
		treeNode->NeedsReload = false;
		return true;
	}
	return false;
}


void FNOSSceneTreeManager::SendNodeUpdate(FGuid nodeId, bool bResetRootPins)
{
	LOGF("Sending node update to Nodos with id %s", *nodeId.ToString());
	if (!NOSClient->IsConnected() || !nodeId.IsValid())
	{
		return;
	}

	if (nodeId == SceneTree.Root->Id)
	{
		if (!bResetRootPins)
		{
			flatbuffers::FlatBufferBuilder mb;
			std::vector<flatbuffers::Offset<nos::fb::Node>> graphNodes = SceneTree.Root->SerializeChildren(mb);
			std::vector<flatbuffers::Offset<nos::fb::Node>> graphFunctions;
			for (auto& [_, cfunc] : CustomFunctions)
			{
				graphFunctions.push_back(cfunc->Serialize(mb));
			}
		
			std::vector<flatbuffers::Offset<nos::fb::MetaDataEntry>> metadata = SceneTree.Root->SerializeMetaData(mb);
			auto offset = nos::CreatePartialNodeUpdateDirect(mb, (nos::fb::UUID*)&nodeId, nos::ClearFlags::CLEAR_FUNCTIONS | nos::ClearFlags::CLEAR_NODES, 0, 0, 0, &graphFunctions, 0, &graphNodes, 0, 0, &metadata);
			mb.Finish(offset);
			auto buf = mb.Release();
			auto root = flatbuffers::GetRoot<nos::PartialNodeUpdate>(buf.data());
			NOSClient->AppServiceClient->SendPartialNodeUpdate(*root);

			return;
		}

		flatbuffers::FlatBufferBuilder mb = flatbuffers::FlatBufferBuilder();
		std::vector<flatbuffers::Offset<nos::fb::Node>> graphNodes = SceneTree.Root->SerializeChildren(mb);
		std::vector<flatbuffers::Offset<nos::fb::Pin>> graphPins;
		for (auto& [_, property] : CustomProperties)
		{
			graphPins.push_back(property->Serialize(mb));
		}
		for (auto& [_, pin] : Pins)
		{
			graphPins.push_back(pin->Serialize(mb));
		}
		std::vector<flatbuffers::Offset<nos::fb::Node>> graphFunctions;
		for (auto& [_, cfunc] : CustomFunctions)
		{
			graphFunctions.push_back(cfunc->Serialize(mb));

		}
		
		std::vector<flatbuffers::Offset<nos::fb::MetaDataEntry>> metadata = SceneTree.Root->SerializeMetaData(mb);
		auto offset =  nos::CreatePartialNodeUpdateDirect(mb, (nos::fb::UUID*)(&nodeId), nos::ClearFlags::ANY & ~nos::ClearFlags::CLEAR_METADATA, 0, &graphPins, 0, &graphFunctions, 0, &graphNodes, 0, 0, &metadata);
		mb.Finish(offset);
		auto buf = mb.Release();
		auto root = flatbuffers::GetRoot<nos::PartialNodeUpdate>(buf.data());
		NOSClient->AppServiceClient->SendPartialNodeUpdate(*root);

		return;
	}
	auto treeNode = SceneTree.GetNode(nodeId);
	if (!(treeNode))
	{
		return;
	}
	flatbuffers::FlatBufferBuilder mb;
	std::vector<flatbuffers::Offset<nos::fb::Node>> graphNodes = treeNode->SerializeChildren(mb);
	std::vector<flatbuffers::Offset<nos::fb::Pin>> graphPins;
	if (treeNode->GetAsActorNode())
	{
		graphPins = treeNode->GetAsActorNode()->SerializePins(mb);
	}
	else if (treeNode->GetAsSceneComponentNode())
	{
		graphPins = treeNode->GetAsSceneComponentNode()->SerializePins(mb);
	}
	std::vector<flatbuffers::Offset<nos::fb::Node>> graphFunctions;
	if (treeNode->GetAsActorNode())
	{
		for (auto nosfunc : treeNode->GetAsActorNode()->Functions)
		{
			graphFunctions.push_back(nosfunc->Serialize(mb));
		}
	}
	auto metadata = treeNode->SerializeMetaData(mb);
	auto offset = nos::CreatePartialNodeUpdateDirect(mb, (nos::fb::UUID*)&nodeId, nos::ClearFlags::CLEAR_PINS | nos::ClearFlags::CLEAR_FUNCTIONS | nos::ClearFlags::CLEAR_NODES | nos::ClearFlags::CLEAR_METADATA, 0, &graphPins, 0, &graphFunctions, 0, &graphNodes, 0, 0, &metadata);
	mb.Finish(offset);
	auto buf = mb.Release();
	auto root = flatbuffers::GetRoot<nos::PartialNodeUpdate>(buf.data());
	NOSClient->AppServiceClient->SendPartialNodeUpdate(*root);
}

void FNOSSceneTreeManager::SendEngineFunctionUpdate()
{
	if (!NOSClient || !NOSClient->IsConnected())
	{
		return;
	}
	flatbuffers::FlatBufferBuilder mb = flatbuffers::FlatBufferBuilder();
	std::vector<flatbuffers::Offset<nos::fb::Node>> graphFunctions;
	for (auto& [_, cfunc] : CustomFunctions)
	{
		graphFunctions.push_back(cfunc->Serialize(mb));

	}
	std::vector<flatbuffers::Offset<nos::fb::MetaDataEntry>> metadata = SceneTree.Root->SerializeMetaData(mb);
	auto offset =  nos::CreatePartialNodeUpdateDirect(mb, (nos::fb::UUID*)(&FNOSClient::NodeId), nos::ClearFlags::CLEAR_FUNCTIONS, 0, 0, 0, &graphFunctions, 0, 0, 0, 0, &metadata);
	mb.Finish(offset);
	auto buf = mb.Release();
	auto root = flatbuffers::GetRoot<nos::PartialNodeUpdate>(buf.data());
	NOSClient->AppServiceClient->SendPartialNodeUpdate(*root);
}

void FNOSSceneTreeManager::SendPinValueChanged(FGuid propertyId, std::vector<uint8> data)
{
	if (!NOSClient->IsConnected() || data.empty())
	{
		return;
	}

	flatbuffers::FlatBufferBuilder mb;
	auto offset = nos::app::CreateSetPinValueDirect(mb, (nos::fb::UUID*)&propertyId, &data);
	mb.Finish(offset);
	auto buf = mb.Release();
	auto root = flatbuffers::GetRoot<nos::app::SetPinValue>(buf.data());
	NOSClient->AppServiceClient->NotifyPinValueChanged(*root);
}

void FNOSSceneTreeManager::SendPinUpdate()
{
	if (!NOSClient->IsConnected())
	{
		return;
	}

	auto nodeId = FNOSClient::NodeId;

	flatbuffers::FlatBufferBuilder mb;
	std::vector<flatbuffers::Offset<nos::fb::Pin>> graphPins;
	for (auto& [_, pin] : CustomProperties)
	{
		graphPins.push_back(pin->Serialize(mb));
	}
	for (auto& [_, pin] : Pins)
	{
		graphPins.push_back(pin->Serialize(mb));
	}
	auto offset = nos::CreatePartialNodeUpdateDirect(mb, (nos::fb::UUID*)&nodeId, nos::ClearFlags::CLEAR_PINS, 0, &graphPins, 0, 0, 0, 0);
	mb.Finish(offset);
	auto buf = mb.Release();
	auto root = flatbuffers::GetRoot<nos::PartialNodeUpdate>(buf.data());
	NOSClient->AppServiceClient->SendPartialNodeUpdate(*root);

}

void FNOSSceneTreeManager::RemovePortal(FGuid PortalId)
{
	LOG("Portal is removed.");
	
	if(!NOSPropertyManager.PortalPinsById.Contains(PortalId))
	{
		return;
	}
	auto Portal = NOSPropertyManager.PortalPinsById.FindRef(PortalId);
	NOSPropertyManager.PortalPinsById.Remove(Portal.Id);
	NOSPropertyManager.PropertyToPortalPin.Remove(Portal.SourceId);

	if(!NOSClient->IsConnected())
	{
		return;
	}
	if(NOSPropertyManager.PropertiesById.Contains(Portal.SourceId))
	{
		auto SourceProp = NOSPropertyManager.PropertiesById.FindRef(Portal.SourceId);
		SourceProp->PinShowAs = nos::fb::ShowAs::PROPERTY;
		NOSTextureShareManager::GetInstance()->UpdatePinShowAs(SourceProp.Get(), SourceProp->PinShowAs);
		NOSClient->AppServiceClient->SendPinShowAsChange((nos::fb::UUID&)SourceProp->Id, SourceProp->PinShowAs);
	}
	flatbuffers::FlatBufferBuilder mb;
	std::vector<nos::fb::UUID> pinsToDelete;
	pinsToDelete.push_back(*(nos::fb::UUID*)&Portal.Id);

	auto offset = nos::CreatePartialNodeUpdateDirect(mb, (nos::fb::UUID*)&FNOSClient::NodeId, nos::ClearFlags::NONE, &pinsToDelete, 0, 0, 0, 0, 0);
	mb.Finish(offset);
	auto buf = mb.Release();
	auto root = flatbuffers::GetRoot<nos::PartialNodeUpdate>(buf.data());
	NOSClient->AppServiceClient->SendPartialNodeUpdate(*root);
}

void FNOSSceneTreeManager::SendPinAdded(FGuid NodeId, TSharedPtr<NOSProperty> const& nosprop)
{
	if (!NOSClient->IsConnected())
	{
		return;
	}
	flatbuffers::FlatBufferBuilder mb;
	std::vector<flatbuffers::Offset<nos::fb::Pin>> graphPins = { nosprop->Serialize(mb) };
	auto offset = nos::CreatePartialNodeUpdateDirect(mb, (nos::fb::UUID*)&NodeId, nos::ClearFlags::NONE, 0, &graphPins, 0, 0, 0, 0);
	mb.Finish(offset);
	auto buf = mb.Release();
	auto root = flatbuffers::GetRoot<nos::PartialNodeUpdate>(buf.data());
	NOSClient->AppServiceClient->SendPartialNodeUpdate(*root);

	return;
}

void FNOSSceneTreeManager::SendActorAddedOnUpdate(AActor* actor, FString spawnTag)
{
	if (SceneTree.GetNode(actor))
	{
		return;
	}

	if (AlwaysUpdateOnActorSpawns)
	{
		SendActorAdded(actor, spawnTag);
		return;
	}

	ActorsToBeAdded.AddUnique(TWeakObjectPtr<AActor>(actor));
}

void FNOSSceneTreeManager::SendActorAdded(AActor* actor, FString spawnTag)
{
	if(!FNOSClient::NodeId.IsValid())
	{
		return;
	}

	TSharedPtr<ActorNode> newNode = nullptr;
	if (auto sceneParent = actor->GetSceneOutlinerParent())
	{
		if (auto parentNode = SceneTree.GetNode(sceneParent))
		{
			newNode = SceneTree.AddActor(parentNode, actor);
			if (!newNode)
			{
				return;
			}
			if (!spawnTag.IsEmpty())
			{
				newNode->nosMetaData.Add(NosMetadataKeys::spawnTag, spawnTag);
			}
			if (!NOSClient->IsConnected())
			{
				return;
			}
			flatbuffers::FlatBufferBuilder mb;
			std::vector<flatbuffers::Offset<nos::fb::Node>> graphNodes = { newNode->Serialize(mb) };
			auto offset = nos::CreatePartialNodeUpdateDirect(mb, (nos::fb::UUID*)&parentNode->Id, nos::ClearFlags::NONE, 0, 0, 0, 0, 0, &graphNodes);
			mb.Finish(offset);
			auto buf = mb.Release();
			auto root = flatbuffers::GetRoot<nos::PartialNodeUpdate>(buf.data());
			NOSClient->AppServiceClient->SendPartialNodeUpdate(*root);
		}
	}
	else
	{
		TSharedPtr<TreeNode> mostRecentParent;
		newNode = SceneTree.AddActor(actor->GetFolder().GetPath().ToString(), actor, mostRecentParent);
		if (!newNode)
		{
			return;
		}
		if (!spawnTag.IsEmpty())
		{
			newNode->nosMetaData.Add(NosMetadataKeys::spawnTag, spawnTag);
		}
		if (!NOSClient->IsConnected())
		{
			return;
		}

		flatbuffers::FlatBufferBuilder mb;
		std::vector<flatbuffers::Offset<nos::fb::Node>> graphNodes = { mostRecentParent->Serialize(mb) };
		auto offset = nos::CreatePartialNodeUpdateDirect(mb, (nos::fb::UUID*)&mostRecentParent->Parent->Id, nos::ClearFlags::NONE, 0, 0, 0, 0, 0, &graphNodes);
		mb.Finish(offset);
		auto buf = mb.Release();
		auto root = flatbuffers::GetRoot<nos::PartialNodeUpdate>(buf.data());
		NOSClient->AppServiceClient->SendPartialNodeUpdate(*root);

	}
}

void FNOSSceneTreeManager::RemoveProperties(TreeNode* Node,
	TSet<TSharedPtr<NOSProperty>>& PropertiesToRemove)
{
	if (auto componentNode = Node->GetAsSceneComponentNode())
	{
		for (auto& prop : componentNode->Properties)
		{
			PropertiesToRemove.Add(prop);
			NOSPropertyManager.PropertiesById.Remove(prop->Id);
			NOSPropertyManager.PropertiesByPropertyAndContainer.Remove({prop->Property, prop->GetRawObjectContainer()});
		}
	}
	else if (auto actorNode = Node->GetAsActorNode())
	{
		for (auto& prop : actorNode->Properties)
		{
			PropertiesToRemove.Add(prop);
			NOSPropertyManager.PropertiesById.Remove(prop->Id);
			NOSPropertyManager.PropertiesByPropertyAndContainer.Remove({prop->Property, prop->GetRawObjectContainer()});
		}
	}
	for (auto& child : Node->Children)
	{
		RemoveProperties(child.Get(), PropertiesToRemove);
	}
}

void FNOSSceneTreeManager::CheckPins(TSet<UObject*>& RemovedObjects,
	TSet<TSharedPtr<NOSProperty>>& PinsToRemove,
	TSet<TSharedPtr<NOSProperty>>& PropertiesToRemove)
{
	for (auto& [id, pin] : Pins)
	{
		UObject* container = pin->GetRawObjectContainer();
		if (!container)
		{
			continue;
		}
		if (RemovedObjects.Contains(container))
		{
			PinsToRemove.Add(pin);
		}
	}
}

void FNOSSceneTreeManager::Reset()
{
	NOSTextureShareManager::GetInstance()->Reset();
	ActorsToBeAdded.Empty();
	SceneTree.Clear();
	Pins.Empty();
	NOSPropertyManager.Reset();
	NOSActorManager->ReAddActorsToSceneTree();
}

void FNOSSceneTreeManager::SendActorNodeDeleted(ActorNode* node)
{
	if (!node)
		return;

	//delete properties
	// can be optimized by using raw pointers
	TSet<TSharedPtr<NOSProperty>> propertiesToRemove;
	RemoveProperties(node, propertiesToRemove);
	TSet<FGuid> PropertiesWithPortals;
	TSet<FGuid> PortalsToRemove;
	auto texman = NOSTextureShareManager::GetInstance();
	for (auto prop : propertiesToRemove)
	{
		if(prop->TypeName == "nos.sys.vulkan.Texture")
		{
			texman->TextureDestroyed(prop.Get());
		}
		if (!NOSPropertyManager.PropertyToPortalPin.Contains(prop->Id))
		{
			continue;
		}
		auto portalId = NOSPropertyManager.PropertyToPortalPin.FindRef(prop->Id);
		PropertiesWithPortals.Add(prop->Id);
		if (!NOSPropertyManager.PortalPinsById.Contains(portalId))
		{
			continue;
		}
		PortalsToRemove.Add(portalId);
	}
	for (auto PropertyId : PropertiesWithPortals)
	{
		NOSPropertyManager.PropertyToPortalPin.Remove(PropertyId);
	}
	for (auto PortalId : PortalsToRemove)
	{
		NOSPropertyManager.PortalPinsById.Remove(PortalId);
	}

	for (auto child : node->Children)
	{
		if (auto childActorNode = child->GetAsActorNode())
		{
			OnActorDetached(childActorNode->actor.Get(), node->actor.Get());
		}
	}

	//delete from parent
	FGuid parentId = FNOSClient::NodeId;
	if (auto parent = node->Parent)
	{
		parentId = parent->Id;
		TSharedPtr<TreeNode> found;
		for(auto child : parent->Children)
		{
			if(child->Id == node->Id)
			{
				found = child;
			}
		}
		auto v = parent->Children;
		auto it = std::find(v.begin(), v.end(), found);
		if (it != v.end())
			v.erase(it);
	}
	//delete from map
	SceneTree.RemoveNode(node->Id);

	if (!NOSClient->IsConnected())
	{
		return;
	}

	if (!PortalsToRemove.IsEmpty())
	{
		std::vector<nos::fb::UUID> pinsToDelete;
		for (auto portalId : PortalsToRemove)
		{
			pinsToDelete.push_back(*(nos::fb::UUID*)&portalId);
		}
		flatbuffers::FlatBufferBuilder mb;
		auto offset = nos::CreatePartialNodeUpdateDirect(mb, (nos::fb::UUID*)&FNOSClient::NodeId, nos::ClearFlags::NONE, &pinsToDelete, 0, 0, 0, 0, 0);
		mb.Finish(offset);
		auto buf = mb.Release();
		auto root = flatbuffers::GetRoot<nos::PartialNodeUpdate>(buf.data());
		NOSClient->AppServiceClient->SendPartialNodeUpdate(*root);
	}

	flatbuffers::FlatBufferBuilder mb2;
	std::vector<nos::fb::UUID> graphNodes = { *(nos::fb::UUID*)&node->Id };
	auto offset = nos::CreatePartialNodeUpdateDirect(mb2, (nos::fb::UUID*)&parentId, nos::ClearFlags::NONE, 0, 0, 0, 0, &graphNodes, 0);
	mb2.Finish(offset);
	auto buf = mb2.Release();
	auto root = flatbuffers::GetRoot<nos::PartialNodeUpdate>(buf.data());
	NOSClient->AppServiceClient->SendPartialNodeUpdate(*root);
}

void FNOSSceneTreeManager::SendActorDeletedOnUpdate(AActor* actor)
{
	if (AlwaysUpdateOnActorSpawns || ActorsDeletedFromNodos.Contains(actor->GetActorGuid()))
	{
		ActorsDeletedFromNodos.Remove(actor->GetActorGuid());
		SendActorDeleted(actor);
		return;
	}
	ActorsToBeDeleted.Add(actor->GetActorGuid());
}

void FNOSSceneTreeManager::SendActorDeleted(AActor* Actor)
{
	if (auto node = SceneTree.GetNode(Actor))
	{
		SendActorNodeDeleted(node);
	}
}

void FNOSSceneTreeManager::SendParentChangedOnUpdate(FGuid Actor, FGuid ParentActor)
{
	if (AlwaysUpdateOnActorSpawns)
	{
		SendParentChanged(Actor, ParentActor);
		return;
	}
	ActorsToBeParentChanged.Add(Actor, ParentActor);
}

void FNOSSceneTreeManager::SendParentChanged(FGuid Actor, FGuid ParentActor)
{
	flatbuffers::FlatBufferBuilder fb;
	auto offset = nos::CreateAppEventOffset(fb, nos::app::CreateChangeNodeParent(fb, (nos::fb::UUID*)&Actor, (nos::fb::UUID*)&ParentActor));
	fb.Finish(offset);
	auto buf = fb.Release();
	auto root = flatbuffers::GetRoot<nos::app::AppEvent>(buf.data());
	NOSClient->AppServiceClient->Send(*root);
}

void FNOSSceneTreeManager::PopulateAllChildsOfActor(AActor* actor)
{
	LOGF("Populating all childs of %s", *actor->GetFName().ToString());
	FGuid ActorId = actor->GetActorGuid();
	PopulateAllChildsOfActor(ActorId);
}

void FNOSSceneTreeManager::ReloadCurrentMap()
{
	if (daWorld)
	{
#ifdef NOS_RELOAD_MAP_ON_EDIT_MODE
		if(GEditor && !GEditor->IsPlaySessionInProgress())
		{
			const FString FileToOpen = FPackageName::LongPackageNameToFilename(daWorld->GetOutermost()->GetName(), FPackageName::GetMapPackageExtension());
			const bool bLoadAsTemplate = false;
			const bool bShowProgress = true;
			FEditorFileUtils::LoadMap(FileToOpen, bLoadAsTemplate, bShowProgress);
		}
		else
#endif
			UGameplayStatics::OpenLevel(daWorld, daWorld->GetFName());
	}
}

void FNOSSceneTreeManager::PopulateAllChildsOfActor(FGuid ActorId)
{
	LOGF("Populating all childs of actor with id %s", *ActorId.ToString());
	if (PopulateNode(SceneTree.GetNodeIdActorId(ActorId)))
	{
		SendNodeUpdate(SceneTree.GetNodeIdActorId(ActorId));
	}

	if(auto ActorNode = SceneTree.GetNodeFromActorId(ActorId))
	{
		for (auto ChildNode : ActorNode->Children)
		{
			if (ChildNode->GetAsActorNode())
			{
				PopulateAllChildsOfActor(ChildNode->GetAsActorNode()->actor.Get());
			}
			else if (ChildNode->GetAsSceneComponentNode())
			{
				PopulateAllChildsOfSceneComponentNode(ChildNode->GetAsSceneComponentNode());
			}
		}

	}
}

void FNOSSceneTreeManager::PopulateAllChildsOfSceneComponentNode(SceneComponentNode* SceneComponentNode)
{
	if (!SceneComponentNode)
	{
		return;
	}

	if (PopulateNode(SceneComponentNode->Id))
	{
		SendNodeUpdate(SceneComponentNode->Id);
	}

	for (auto ChildNode : SceneComponentNode->Children)
	{
		if (ChildNode->GetAsActorNode())
		{
			PopulateAllChildsOfActor(ChildNode->GetAsActorNode()->actor.Get());
		}
		else if (ChildNode->GetAsSceneComponentNode())
		{
			PopulateAllChildsOfSceneComponentNode(ChildNode->GetAsSceneComponentNode());
		}
	}
}

void FNOSSceneTreeManager::SendSyncSemaphores(bool RenewSemaphores)
{
	if(!FNOSClient::NodeId.IsValid())
	{
		UE_LOG(LogNOSSceneTreeManager, Error, TEXT("Sending sync semaphores with non-valid node Id, a deadlock might happen!"));
	}
	auto TextureShareManager = NOSTextureShareManager::GetInstance();
	if(RenewSemaphores)
	{
		TextureShareManager->RenewSemaphores();
	}

	uint64_t inputSemaphore = (uint64_t)TextureShareManager->SyncSemaphoresExportHandles.InputSemaphore;
	uint64_t outputSemaphore = (uint64_t)TextureShareManager->SyncSemaphoresExportHandles.OutputSemaphore;

	flatbuffers::FlatBufferBuilder mb;
	auto offset = nos::CreateAppEventOffset(mb, nos::app::CreateSetSyncSemaphores(mb, (nos::fb::UUID*)&FNOSClient::NodeId, FPlatformProcess::GetCurrentProcessId(), inputSemaphore, outputSemaphore));
	mb.Finish(offset);
	auto buf = mb.Release();
	auto root = flatbuffers::GetRoot<nos::app::AppEvent>(buf.data());
	NOSClient->AppServiceClient->Send(*root);
}

struct PortalSourceContainerInfo
{
	FGuid ActorId;
	FString ComponentName;
	FString PropertyPath;
	FString ContainerPath;
	FProperty* Property;
};

void FNOSSceneTreeManager::HandleWorldChange()
{
	LOG("Handling world change.");
	SceneTree.Clear();
	NOSTextureShareManager::GetInstance()->Reset();

	TArray<TTuple<PortalSourceContainerInfo, NOSPortal>> Portals;
	TSet<FGuid> ActorsToRescan;

	flatbuffers::FlatBufferBuilder mb;
	std::vector<nos::fb::UUID> graphPins;// = { *(nos::fb::UUID*)&node->Id };
	std::vector<flatbuffers::Offset<nos::PartialPinUpdate>> PinUpdates;

	for (auto [id, portal] : NOSPropertyManager.PortalPinsById)
	{
		if (!NOSPropertyManager.PropertiesById.Contains(portal.SourceId))
		{
			continue;
		}
		auto NosProperty = NOSPropertyManager.PropertiesById.FindRef(portal.SourceId);

		
		PortalSourceContainerInfo ContainerInfo; //= { .ComponentName = "", .PropertyPath =  PropertyPath, .Property = NosProperty->Property};
		ContainerInfo.Property = NosProperty->Property;
		if(NosProperty->nosMetaDataMap.Contains(NosMetadataKeys::PropertyPath))
		{
			ContainerInfo.PropertyPath = NosProperty->nosMetaDataMap.FindRef(NosMetadataKeys::PropertyPath);
		}
		
		if(NosProperty->nosMetaDataMap.Contains(NosMetadataKeys::ContainerPath))
		{
			ContainerInfo.ContainerPath = NosProperty->nosMetaDataMap.FindRef(NosMetadataKeys::ContainerPath);
		}
		
		if(NosProperty->nosMetaDataMap.Contains(NosMetadataKeys::actorId))
		{
			FString ActorIdString = NosProperty->nosMetaDataMap.FindRef(NosMetadataKeys::actorId);
			FGuid ActorId;
			FGuid::Parse(ActorIdString, ActorId);
			ContainerInfo.ActorId = ActorId;
			ActorsToRescan.Add(ActorId);
		}
		
		if(NosProperty->nosMetaDataMap.Contains(NosMetadataKeys::component))
		{
			FString ComponentName = NosProperty->nosMetaDataMap.FindRef(NosMetadataKeys::component);
			ContainerInfo.ComponentName = ComponentName;
		}
		
		Portals.Add({ContainerInfo, portal});
		graphPins.push_back(*(nos::fb::UUID*)&portal.Id);
		PinUpdates.push_back(nos::CreatePartialPinUpdate(mb, (nos::fb::UUID*)&portal.Id, 0, nos::fb::CreateOrphanStateDirect(mb, true, "Object not found in the world")));
	}

	if (!NOSClient->IsConnected())
	{
		return;
	}
	auto offset = nos::CreatePartialNodeUpdateDirect(mb, (nos::fb::UUID*)&FNOSClient::NodeId, nos::ClearFlags::NONE, 0, 0, 0, 0, 0, 0, 0, &PinUpdates);
	mb.Finish(offset);
	auto buf = mb.Release();
	auto root = flatbuffers::GetRoot<nos::PartialNodeUpdate>(buf.data());
	NOSClient->AppServiceClient->SendPartialNodeUpdate(*root);
	PinUpdates.clear();

	NOSPropertyManager.Reset(false);
	NOSActorManager->ReAddActorsToSceneTree();
	RescanScene(false);
	SendNodeUpdate(FNOSClient::NodeId, false);


	for (auto ActorId : ActorsToRescan)
	{
		PopulateAllChildsOfActor(ActorId);
	}

	for (TActorIterator< AActor > ActorItr(daWorld); ActorItr; ++ActorItr)
	{
		if(ActorsToRescan.Contains(ActorItr->GetActorGuid()))
		{
			if(auto parent = ActorItr->GetAttachParentActor())
			{
				while(parent->GetAttachParentActor())
					parent = parent->GetAttachParentActor();
				PopulateAllChildsOfActor(parent);
			}
		}
	}

	flatbuffers::FlatBufferBuilder mbb;
	std::vector<nos::fb::UUID> PinsToRemove;
	for (auto& [containerInfo, portal] : Portals)
	{
		UObject* ObjectContainer = FindContainer(containerInfo.ActorId, containerInfo.ComponentName);
		bool discard;
		void* UnknownContainer = FindContainerFromContainerPath(ObjectContainer, containerInfo.ContainerPath, discard);
		UnknownContainer = UnknownContainer ? UnknownContainer : ObjectContainer;
		if (portal.TypeName == "nos.fb.Transform")
		{
			//find actor node from container info
			auto ActorNode = SceneTree.GetNodeFromActorId(containerInfo.ActorId);
			if (ActorNode)
			{
				//iterate actors properties
				for (auto& prop : ActorNode->GetAsActorNode()->Properties)
				{
					if (prop->IsActorTransform)
					{
						//update portal source id
						portal.SourceId = prop->Id;
						NOSPropertyManager.PortalPinsById.Add(portal.Id, portal);
						NOSPropertyManager.PropertyToPortalPin.Add(prop->Id, portal.Id);
						PinUpdates.push_back(nos::CreatePartialPinUpdate(mbb, (nos::fb::UUID*)&portal.Id, (nos::fb::UUID*)&prop->Id, nos::fb::CreateOrphanStateDirect(mbb, false, "")));
						break;
					}
				}


			}
		}
		else if (NOSPropertyManager.PropertiesById.Contains(portal.SourceId))
		{
			if (auto propertyPin = NOSPropertyManager.PropertiesById.FindRef(portal.SourceId))
			{propertyPin->PinShowAs = portal.ShowAs;
				NOSTextureShareManager::GetInstance()->UpdatePinShowAs(propertyPin.Get(), propertyPin->PinShowAs);
				NOSClient->AppServiceClient->SendPinShowAsChange((nos::fb::UUID&)propertyPin->Id, propertyPin->PinShowAs);
				NOSPropertyManager.PropertyToPortalPin.Add(propertyPin->Id, portal.Id);
				PinUpdates.push_back(nos::CreatePartialPinUpdate(mbb, (nos::fb::UUID*)&portal.Id, (nos::fb::UUID*)&propertyPin->Id, nos::fb::CreateOrphanStateDirect(mbb, false, "Object not found in the world")));
			}
		}
		// TODO: This if might be covered by the previous one already
		else if (NOSPropertyManager.PropertiesByPropertyAndContainer.Contains({containerInfo.Property,UnknownContainer}))
		{
			auto NosProperty = NOSPropertyManager.PropertiesByPropertyAndContainer.FindRef({containerInfo.Property,UnknownContainer});
			if (NOSPropertyManager.PortalPinsById.Contains(portal.Id))
			{
				auto pPortal = NOSPropertyManager.PortalPinsById.Find(portal.Id);
				pPortal->SourceId = NosProperty->Id;
			}
			portal.SourceId = NosProperty->Id;
			NosProperty->PinShowAs = portal.ShowAs;
			NOSTextureShareManager::GetInstance()->UpdatePinShowAs(NosProperty.Get(), NosProperty->PinShowAs);
			NOSClient->AppServiceClient->SendPinShowAsChange((nos::fb::UUID&)NosProperty->Id, NosProperty->PinShowAs);
			NOSPropertyManager.PropertyToPortalPin.Add(NosProperty->Id, portal.Id);
			PinUpdates.push_back(nos::CreatePartialPinUpdate(mbb, (nos::fb::UUID*)&portal.Id, (nos::fb::UUID*)&NosProperty->Id, nos::fb::CreateOrphanStateDirect(mbb, false, "Object not found in the world")));
		}
		else
		{
			PinsToRemove.push_back(*(nos::fb::UUID*)&portal.Id);
		}
	}
	
	if (!PinUpdates.empty())
	{
		auto offset1 = 	nos::CreatePartialNodeUpdateDirect(mbb, (nos::fb::UUID*)&FNOSClient::NodeId, nos::ClearFlags::NONE, 0, 0, 0, 0, 0, 0, 0, &PinUpdates);
		mbb.Finish(offset1);
		auto buf1 = mbb.Release();
		auto root1 = flatbuffers::GetRoot<nos::PartialNodeUpdate>(buf1.data());
		NOSClient->AppServiceClient->SendPartialNodeUpdate(*root1);
	}
	if(!PinsToRemove.empty())
	{
		flatbuffers::FlatBufferBuilder mb2;
		auto offset2 = nos::CreatePartialNodeUpdateDirect(mb2, (nos::fb::UUID*)&FNOSClient::NodeId, nos::ClearFlags::NONE, &PinsToRemove);
		mb2.Finish(offset2);
		auto buf2 = mb2.Release();
		auto root2 = flatbuffers::GetRoot<nos::PartialNodeUpdate>(buf2.data());
		NOSClient->AppServiceClient->SendPartialNodeUpdate(*root2);
	}

	LOG("World change handled");
}

UObject* FNOSSceneTreeManager::FindContainer(FGuid ActorId, FString ComponentName)
{
	UObject* Container = nullptr;
	UWorld* World = FNOSSceneTreeManager::daWorld;
	for (TActorIterator< AActor > ActorItr(World); ActorItr; ++ActorItr)
	{
		if(ActorItr->GetActorGuid() == ActorId)
		{
			Container = *ActorItr;
		}
	}
	
	if (!ComponentName.IsEmpty())
	{
		Container = FindObject<USceneComponent>(Container, *ComponentName);
	}

	return Container;	
}

void* FNOSSceneTreeManager::FindContainerFromContainerPath(UObject* BaseContainer, FString ContainerPath, bool& IsResultUObject)
{
	if(!BaseContainer)
	{
		IsResultUObject = false;
		return nullptr;
	}
	TArray<FString> ChildContainerNames;
	ContainerPath.ParseIntoArray(ChildContainerNames, TEXT("/"));
	FProperty* Property = nullptr; 
	UClass* ContainerClass = BaseContainer->GetClass();
	void* Container = BaseContainer;
	IsResultUObject = true;
	for(auto ChildContainerName : ChildContainerNames)
	{
		if(!Container)
		{
			return nullptr;
		}
		Property = FindFProperty<FProperty>(ContainerClass, *ChildContainerName);
		if(FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property))
		{
			UObject* Object = ObjectProperty->GetObjectPropertyValue(ObjectProperty->ContainerPtrToValuePtr<UObject>(Container));
			ContainerClass = Object->GetClass();
			Container = Object;
			IsResultUObject = true;
		}
		else if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			uint8* StructInstance = StructProperty->ContainerPtrToValuePtr<uint8>(Container);
			ContainerClass = StructProperty->Struct->GetClass();
			Container = StructInstance;
			IsResultUObject = false;
		}
		else
		{
			return nullptr;
		}
	}
	return Container;
}

void FNOSSceneTreeManager::HandleBeginPIE(bool bIsSimulating)
{
	FString WorldName = GEditor->GetEditorWorldContext().World()->GetMapName();
	WorldName = GEngine->GetWorldContextFromGameViewport(GEngine->GameViewport)->World()->GetMapName();
	FNOSSceneTreeManager::daWorld = GEngine->GetWorldContextFromGameViewport(GEngine->GameViewport)->World();
	
	HandleWorldChange();

	FOnActorSpawned::FDelegate ActorSpawnedDelegate = FOnActorSpawned::FDelegate::CreateRaw(this, &FNOSSceneTreeManager::OnActorSpawned);
	FOnActorDestroyed::FDelegate ActorDestroyedDelegate = FOnActorDestroyed::FDelegate::CreateRaw(this, &FNOSSceneTreeManager::OnActorDestroyed);
	FNOSSceneTreeManager::daWorld->AddOnActorSpawnedHandler(ActorSpawnedDelegate);
	FNOSSceneTreeManager::daWorld->AddOnActorDestroyedHandler(ActorDestroyedDelegate);
}

void FNOSSceneTreeManager::HandleEndPIE(bool bIsSimulating)
{
	FString WorldName = GEditor->GetEditorWorldContext().World()->GetMapName();
	FNOSSceneTreeManager::daWorld = GEditor ? GEditor->GetEditorWorldContext().World() : GEngine->GetCurrentPlayWorld();
	HandleWorldChange();

	FOnActorSpawned::FDelegate ActorSpawnedDelegate = FOnActorSpawned::FDelegate::CreateRaw(this, &FNOSSceneTreeManager::OnActorSpawned);
	FOnActorDestroyed::FDelegate ActorDestroyedDelegate = FOnActorDestroyed::FDelegate::CreateRaw(this, &FNOSSceneTreeManager::OnActorDestroyed);
	FNOSSceneTreeManager::daWorld->AddOnActorSpawnedHandler(ActorSpawnedDelegate);
	FNOSSceneTreeManager::daWorld->AddOnActorDestroyedHandler(ActorDestroyedDelegate);
}


AActor* FNOSActorManager::GetParentTransformActor()
{
	if(!ParentTransformActor.Get())
	{
		ParentTransformActor = NOSActorReference(SpawnActor("RealityParentTransform"));
		ParentTransformActor->GetRootComponent()->SetMobility(EComponentMobility::Static);
	}

	return ParentTransformActor.Get();
}

AActor* FNOSActorManager::SpawnActor(FString SpawnTag, NOSSpawnActorParameters Params, TMap<FString, FString> Metadata)
{
	if (!NOSAssetManager)
	{
		return nullptr;
	}

	AActor* SpawnedActor = NOSAssetManager->SpawnFromTag(SpawnTag, Params.SpawnTransform, Metadata);
	if (!SpawnedActor)
	{
		return nullptr;
	}
	bool bIsSpawningParentTransform = (SpawnTag == "RealityParentTransform");
	if(!bIsSpawningParentTransform)
	{
		if (!Params.SpawnActorToWorldCoords)
		{
			SpawnedActor->GetRootComponent()->SetMobility(EComponentMobility::Movable);
			SpawnedActor->AttachToComponent(GetParentTransformActor()->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
		}
	}

	ActorIds.Add(SpawnedActor->GetActorGuid());
	TMap<FString, FString> savedMetadata;

	for(auto& [key, value] : Metadata)
		savedMetadata.Add({ key, value});
	savedMetadata.Add({ NosMetadataKeys::spawnTag, SpawnTag});
	savedMetadata.Add({ NosMetadataKeys::NodeColor, HEXCOLOR_Reality_Node});
	savedMetadata.Add({ NosMetadataKeys::ActorGuid, SpawnedActor->GetActorGuid().ToString()});
	savedMetadata.Add(NosMetadataKeys::DoNotAttachToRealityParent, FString(Params.SpawnActorToWorldCoords ? "true" : "false"));
	SavedActorData savedData = {savedMetadata};
	Actors.Add({ NOSActorReference(SpawnedActor), savedData});
	TSharedPtr<TreeNode> mostRecentParent;
	TSharedPtr<ActorNode> ActorNode = SceneTree.AddActor(NAME_Reality_FolderName.ToString(), SpawnedActor, mostRecentParent);
	for(auto& [key, value] : Metadata)
		ActorNode->nosMetaData.Add({ key, value});
	ActorNode->nosMetaData.Add({ NosMetadataKeys::spawnTag, SpawnTag});
	ActorNode->nosMetaData.Add(NosMetadataKeys::NodeColor, HEXCOLOR_Reality_Node);
	ActorNode->nosMetaData.Add({ NosMetadataKeys::ActorGuid, SpawnedActor->GetActorGuid().ToString()});
	ActorNode->nosMetaData.Add(NosMetadataKeys::DoNotAttachToRealityParent, FString(Params.SpawnActorToWorldCoords ? "true" : "false"));
	
	if (!NOSClient->IsConnected())
	{
		return SpawnedActor;
	}

	flatbuffers::FlatBufferBuilder mb;
	std::vector<flatbuffers::Offset<nos::fb::Node>> graphNodes = { mostRecentParent->Serialize(mb) };
	auto offset = nos::CreatePartialNodeUpdateDirect(mb, (nos::fb::UUID*)&mostRecentParent->Parent->Id, nos::ClearFlags::NONE, 0, 0, 0, 0, 0, &graphNodes);
	mb.Finish(offset);
	auto buf = mb.Release();
	auto root = flatbuffers::GetRoot<nos::PartialNodeUpdate>(buf.data());
	NOSClient->AppServiceClient->SendPartialNodeUpdate(*root);

	NOSSceneTreeManager->PopulateAllChildsOfActor(SpawnedActor);
	for (auto prop : ActorNode->Properties)
	{
		if (prop->IsActorTransform)
		{
			if (!NOSPropertyManager->PropertyToPortalPin.Contains(prop->Id))
			{
				NOSPropertyManager->CreatePortal(prop->Id, nos::fb::ShowAs::PROPERTY);
			}	
			NOSSceneTreeManager->SendPinValueChanged(prop->Id, prop->data);
		}
	}

	return SpawnedActor;
}

AActor* FNOSActorManager::SpawnUMGRenderManager(FString umgTag, UUserWidget* widget)
{

	if (!NOSAssetManager)
	{
		return nullptr;
	}

	FString SpawnTag("CustomUMGRenderManager");
	AActor* UMGManager = NOSAssetManager->SpawnFromTag(SpawnTag);
	if (!UMGManager)
	{
		return nullptr;
	}
	UMGManager->AttachToComponent(GetParentTransformActor()->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);	
	UMGManager->Rename(*MakeUniqueObjectName(nullptr, AActor::StaticClass(), FName(umgTag)).ToString());

//	Cast<ANOSUMGRenderManager>(UMGManager)->Widget = widget;
	FObjectProperty* WidgetProperty = FindFProperty<FObjectProperty>(UMGManager->GetClass(), "Widget");
	if (WidgetProperty != nullptr)
		WidgetProperty->SetObjectPropertyValue_InContainer(UMGManager, widget);

	ActorIds.Add(UMGManager->GetActorGuid());
	TMap<FString, FString> savedMetadata;
	savedMetadata.Add({ NosMetadataKeys::umgTag, umgTag});
	savedMetadata.Add({ NosMetadataKeys::NodeColor, HEXCOLOR_Reality_Node});
	savedMetadata.Add({ NosMetadataKeys::ActorGuid, UMGManager->GetActorGuid().ToString()});
	SavedActorData savedData = {savedMetadata};
	Actors.Add({ NOSActorReference(UMGManager), savedData});
	TSharedPtr<TreeNode> mostRecentParent;
	TSharedPtr<ActorNode> ActorNode = SceneTree.AddActor(NAME_Reality_FolderName.ToString(), UMGManager, mostRecentParent);
	ActorNode->nosMetaData.Add(NosMetadataKeys::umgTag, umgTag);
	ActorNode->nosMetaData.Add(NosMetadataKeys::NodeColor, HEXCOLOR_Reality_Node);
	ActorNode->nosMetaData.Add({ NosMetadataKeys::ActorGuid, UMGManager->GetActorGuid().ToString()});


	if (!NOSClient->IsConnected())
	{
		return UMGManager;
	}

	flatbuffers::FlatBufferBuilder mb;
	std::vector<flatbuffers::Offset<nos::fb::Node>> graphNodes = { mostRecentParent->Serialize(mb) };
	auto offset = nos::CreatePartialNodeUpdateDirect(mb, (nos::fb::UUID*)&mostRecentParent->Parent->Id, nos::ClearFlags::NONE, 0, 0, 0, 0, 0, &graphNodes);
	mb.Finish(offset);
	auto buf = mb.Release();
	auto root = flatbuffers::GetRoot<nos::PartialNodeUpdate>(buf.data());
	NOSClient->AppServiceClient->SendPartialNodeUpdate(*root);

	return UMGManager;
}

void FNOSActorManager::ClearActors()
{
	LOG("Clearing all actors");

	if(NOSClient)
	{
		NOSClient->ExecuteConsoleCommand(TEXT("Nodos.viewport.disableViewport 0"));
	}
	
	// Remove/destroy actors from Editor and PIE worlds.
	//
	// Remove from current (Editor or PIE) world.
	if(ParentTransformActor)
	{
		ParentTransformActor->Destroy(false, false);
	}
	for (auto& [Actor, spawnTag] : Actors)
	{
		AActor* actor = Actor.Get();
		if (actor)
			actor->Destroy(false, false);
	}
	// When Play starts then actors are duplicated from Editor world into newly created PIE world.
	if (FNOSSceneTreeManager::daWorld)
	{
		EWorldType::Type CurrentWorldType = FNOSSceneTreeManager::daWorld->WorldType.GetValue();
		if (CurrentWorldType == EWorldType::PIE)
		{
			// Actor was removed from PIE world. Remove him also from Editor world.
			UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
			ParentTransformActor.UpdateActorPointer(EditorWorld);
			if(ParentTransformActor)
			{
				ParentTransformActor->Destroy();
			}
			for (auto& [Actor, spawnTag] : Actors)
			{
				Actor.UpdateActorPointer(EditorWorld);
				AActor* actor = Actor.Get();
				if (actor)
					actor->Destroy(false, false);
			}
		}
	}
	
	// Clear local structures.
	ActorIds.Reset();
	Actors.Reset();
	SceneTree.Clear();
}

void FNOSActorManager::ReAddActorsToSceneTree()
{
	for (auto& [Actor, SavedData] : Actors)
	{
		if (Actor.UpdateActualActorPointer())
		{
			AActor* actor = Actor.Get();
			if (!actor)
			{
				continue;
			}

			auto ActorNode = SceneTree.AddActor(NAME_Reality_FolderName.ToString(), actor);
			for(auto [key, value] : SavedData.Metadata)
			{
				ActorNode->nosMetaData.Add(key, value);
			}
		}
		else
		{
			Actor = NOSActorReference();
		}
	}
	Actors = Actors.FilterByPredicate([](const TPair<NOSActorReference, SavedActorData>& Actor)
		{
			return Actor.Key;
		});
}

void FNOSActorManager::RegisterDelegates()
{
	FEditorDelegates::PreSaveWorldWithContext.AddRaw(this, &FNOSActorManager::PreSave);
	FEditorDelegates::PostSaveWorldWithContext.AddRaw(this, &FNOSActorManager::PostSave);
}

void FNOSActorManager::PreSave(UWorld* World, FObjectPreSaveContext Context)
{
	for (auto [Actor, spawnTag] : Actors)
	{
		AActor* actor = Actor.Get();
		if (!actor)
		{
			continue;
		}

		actor->SetFlags(RF_Transient);
	}
}

void FNOSActorManager::PostSave(UWorld* World, FObjectPostSaveContext Context)
{
	for (auto [Actor, spawnTag] : Actors)
	{
		AActor* actor = Actor.Get();
		if (!actor)
		{
			continue;
		}
		actor->ClearFlags(RF_Transient);
	}
}

FNOSPropertyManager::FNOSPropertyManager(NOSSceneTree& sceneTree) : SceneTree(sceneTree)
{
}

void FNOSPropertyManager::CreatePortal(FGuid PropertyId, nos::fb::ShowAs ShowAs)
{
	if (!PropertiesById.Contains(PropertyId))
	{
		return;
	}
	auto NOSProperty = PropertiesById.FindRef(PropertyId);

	if(!CheckPinShowAs(NOSProperty->PinCanShowAs, ShowAs))
	{
		LOG("Pin can't be shown as the wanted type!");
		return;
	}

	if (NOSProperty->IsFunctionProp && NOSProperty->TypeName != "nos.exe")
	{
		auto& NOSSceneTreeManager = FModuleManager::LoadModuleChecked<FNOSSceneTreeManager>("NOSSceneTreeManager");
		if (NOSSceneTreeManager.RegisteredFunctions.Contains(NOSProperty->FunctionId))
		{
			auto Function = NOSSceneTreeManager.RegisteredFunctions.FindRef(NOSProperty->FunctionId);
			if (Function)
			{
				for (auto param : Function->Properties)
				{
					if (param->TypeName == "nos.exe")
					{
						if (!PropertyToPortalPin.Contains(param->Id))
						{
							CreatePortal(param->Id, nos::fb::ShowAs::INPUT_PIN);
						}
					}
				}
			}
		}
	}

	NOSTextureShareManager::GetInstance()->UpdatePinShowAs(NOSProperty.Get(), ShowAs);
	NOSClient->AppServiceClient->SendPinShowAsChange((nos::fb::UUID&)NOSProperty->Id, ShowAs);
	
	NOSPortal NewPortal{StringToFGuid(NOSProperty->Id.ToString()) ,PropertyId};
	NewPortal.DisplayName = FString("");
	UObject* parent = NOSProperty->GetRawObjectContainer();
	FString parentName = "";
	FString parentUniqueName = "";
	AActor* parentAsActor = nullptr;
	while (parent)
	{
		parentName = parent->GetFName().ToString();
		parentUniqueName = parent->GetFName().ToString() + "-";
		if (auto actor = Cast<AActor>(parent))
		{
			parentName = actor->GetActorLabel();
			parentAsActor = actor;
		}
		if(auto component = Cast<USceneComponent>(parent))
			parentName = component->GetName();
		parentName += ".";
		parent = parent->GetTypedOuter<AActor>();
	}
	if (parentAsActor)
	{
		while (parentAsActor->GetSceneOutlinerParent())
		{
			parentAsActor = parentAsActor->GetSceneOutlinerParent();
			if (auto actorNode = SceneTree.GetNodeFromActorId(parentAsActor->GetActorGuid()))
			{
				if (actorNode->nosMetaData.Contains(NosMetadataKeys::spawnTag))
				{
					if (actorNode->nosMetaData.FindRef(NosMetadataKeys::spawnTag) == FString("RealityParentTransform"))
					{
						break;
					}
				}
			}
			parentName = parentAsActor->GetActorLabel() + "." + parentName;
			parentUniqueName = parentAsActor->GetFName().ToString() + "-" + parentUniqueName;
		}
	}

	if (NOSProperty->nosMetaDataMap.Contains(NosMetadataKeys::FunctionName)&&NOSProperty->TypeName != "nos.exe")
	{
		FString functionName;
		functionName = NOSProperty->nosMetaDataMap.FindRef(NosMetadataKeys::FunctionName);
		FString actorName;
		if (NOSProperty->nosMetaDataMap.Contains(NosMetadataKeys::ActorDisplayName))
		{
			actorName = NOSProperty->nosMetaDataMap.FindRef(NosMetadataKeys::ActorDisplayName);
		}
		parentUniqueName = actorName + "-" + functionName + "-";
		parentName = actorName + "." + functionName + ".";
	}


	NewPortal.UniqueName = parentUniqueName + NOSProperty->DisplayName;

	if (NOSProperty->TypeName == "nos.exe")
	{
		NewPortal.UniqueName = parentUniqueName + NOSProperty->PropertyName;
	}

	NewPortal.DisplayName =  parentName + NOSProperty->DisplayName;
	NewPortal.TypeName = FString(NOSProperty->TypeName.c_str());
	NewPortal.CategoryName = NOSProperty->CategoryName;
	NewPortal.ShowAs = ShowAs;

	PortalPinsById.Add(NewPortal.Id, NewPortal);
	PropertyToPortalPin.Add(PropertyId, NewPortal.Id);

	if (!NOSClient->IsConnected())
	{
		return;
	}
	flatbuffers::FlatBufferBuilder mb;
	std::vector<flatbuffers::Offset<nos::fb::Pin>> graphPins = { SerializePortal(mb, NewPortal, NOSProperty.Get()) };
	auto offset = nos::CreatePartialNodeUpdateDirect(mb, (nos::fb::UUID*)&FNOSClient::NodeId, nos::ClearFlags::NONE, 0, &graphPins, 0, 0, 0, 0);
	mb.Finish(offset);
	auto buf = mb.Release();
	auto root = flatbuffers::GetRoot<nos::PartialNodeUpdate>(buf.data());
	NOSClient->AppServiceClient->SendPartialNodeUpdate(*root);
}

void FNOSPropertyManager::CreatePortal(FProperty* uproperty, UObject* Container, nos::fb::ShowAs ShowAs)
{
	if (PropertiesByPropertyAndContainer.Contains({uproperty, Container}))
	{
		auto NosProperty =PropertiesByPropertyAndContainer.FindRef({uproperty, Container});
		if(!CheckPinShowAs(NosProperty->PinCanShowAs, ShowAs))
		{
			LOG("Pin can't be shown as the wanted type!");
			return;
		}
		CreatePortal(NosProperty->Id, ShowAs);
	}
}

TSharedPtr<NOSProperty> FNOSPropertyManager::CreateProperty(UObject* container, FProperty* uproperty, FString parentCategory, bool bAddToCache)
{
	TSharedPtr<NOSProperty> NosProperty = NOSPropertyFactory::CreateProperty(container, uproperty, parentCategory);
	if (!NosProperty)
	{
		return nullptr;
	}
	if (bAddToCache)
	{
		PropertiesById.Add(NosProperty->Id, NosProperty);
		PropertiesByPropertyAndContainer.Add({NosProperty->Property, container}, NosProperty);
	}

	// if (NosProperty->ActorContainer)
	// {
	// 	ActorsPropertyIds.FindOrAdd(NosProperty->ActorContainer.Get()->GetActorGuid()).Add(NosProperty->Id);
	// }
	// else if (NosProperty->ComponentContainer)
	// {
	// 	ActorsPropertyIds.FindOrAdd(NosProperty->ComponentContainer.Actor.Get()->GetActorGuid()).Add(NosProperty->Id);
	// }

	for (auto Child : NosProperty->childProperties)
	{
		PropertiesById.Add(Child->Id, Child);
		PropertiesByPropertyAndContainer.Add({Child->Property, Child->GetRawContainer()}, Child);
		
		// if (Child->ActorContainer)
		// {
		// 	ActorsPropertyIds.FindOrAdd(Child->ActorContainer.Get()->GetActorGuid()).Add(Child->Id);
		// }
		// else if (Child->ComponentContainer)
		// {
		// 	ActorsPropertyIds.FindOrAdd(Child->ComponentContainer.Actor.Get()->GetActorGuid()).Add(Child->Id);
		// }
	}

	return NosProperty;
}

void FNOSPropertyManager::SetPropertyValue()
{
}

bool FNOSPropertyManager::CheckPinShowAs(nos::fb::CanShowAs CanShowAs, nos::fb::ShowAs ShowAs)
{
	if(ShowAs == nos::fb::ShowAs::INPUT_PIN)
	{
		return (CanShowAs == nos::fb::CanShowAs::INPUT_OUTPUT) || 
				(CanShowAs == nos::fb::CanShowAs::INPUT_PIN_OR_PROPERTY) || 
				(CanShowAs == nos::fb::CanShowAs::INPUT_OUTPUT_PROPERTY) || 
				(CanShowAs == nos::fb::CanShowAs::INPUT_PIN_ONLY); 
	}
	if(ShowAs == nos::fb::ShowAs::OUTPUT_PIN)
	{
		return (CanShowAs == nos::fb::CanShowAs::INPUT_OUTPUT) || 
				(CanShowAs == nos::fb::CanShowAs::OUTPUT_PIN_OR_PROPERTY) || 
				(CanShowAs == nos::fb::CanShowAs::INPUT_OUTPUT_PROPERTY) || 
				(CanShowAs == nos::fb::CanShowAs::OUTPUT_PIN_ONLY); 
	}
	if(ShowAs == nos::fb::ShowAs::PROPERTY)
	{
		// we show every pin as property to begin with, may change in the future
		return true;
	}
	return true;
}

void FNOSPropertyManager::ActorDeleted(FGuid DeletedActorId)
{
}

flatbuffers::Offset<nos::fb::Pin> FNOSPropertyManager::SerializePortal(flatbuffers::FlatBufferBuilder& fbb, NOSPortal Portal, NOSProperty* SourceProperty)
{
	auto SerializedMetadata = SourceProperty->SerializeMetaData(fbb);
	return nos::fb::CreatePinDirect(fbb, (nos::fb::UUID*)&Portal.Id, TCHAR_TO_UTF8(*Portal.UniqueName), TCHAR_TO_UTF8(*Portal.TypeName), Portal.ShowAs, SourceProperty->PinCanShowAs, TCHAR_TO_UTF8(*Portal.CategoryName), SourceProperty->SerializeVisualizer(fbb), 0, 0, 0, 0, 0, 0, SourceProperty->ReadOnly, 0, false, &SerializedMetadata, 0, nos::fb::PinContents::PortalPin, nos::fb::CreatePortalPin(fbb, (nos::fb::UUID*)&Portal.SourceId).Union(), 0, nos::fb::PinValueDisconnectBehavior::KEEP_LAST_VALUE, TCHAR_TO_UTF8(*SourceProperty->ToolTipText), TCHAR_TO_UTF8(*Portal.DisplayName));
}

void FNOSPropertyManager::Reset(bool ResetPortals)
{
	if (ResetPortals)
	{
		PropertyToPortalPin.Empty();
		PortalPinsById.Empty();
	}

	PropertiesById.Empty();
	PropertiesByPointer.Empty();
	PropertiesByPropertyAndContainer.Empty();
}

void FNOSPropertyManager::OnBeginFrame()
{
	if (NOSClient->EventDelegates)
	{
		auto executeInfo = NOSClient->EventDelegates->ExecuteQueue.PopFrameNumber(NOSTextureShareManager::GetInstance()->FrameCounter);
		for (auto& [id, val] : executeInfo.PinValueUpdates)
		{
			FGuid guid = *(FGuid*)&id;
			if (auto* NosPropertyIt = PropertiesById.Find(guid))
			{
				auto NosProperty = *NosPropertyIt;
				if (!NosProperty->GetRawContainer() && !NosProperty->IsActorTransform)
				{
					return;
				}
				if (!PropertyToPortalPin.Contains(guid))
				{
					CreatePortal(guid, nos::fb::ShowAs::PROPERTY);
				}
				NosProperty->SetPropValue(val.Data(), val.Size());
			}
		}
	}
	for (auto [id, portal] : PortalPinsById)
	{
		if (portal.ShowAs == nos::fb::ShowAs::OUTPUT_PIN || 
		    !PropertiesById.Contains(portal.SourceId))
		{
			continue;
		}
		
		auto NosProperty = PropertiesById.FindRef(portal.SourceId);

		if (portal.TypeName == "nos.sys.vulkan.Texture")
		{
			NOSTextureShareManager::GetInstance()->UpdateTexturePin(NosProperty.Get(), portal.ShowAs);
			continue;
		}
	}
}

void FNOSPropertyManager::OnEndFrame()
{
	// TODO: copy and dirty CPU out pins
}

std::vector<flatbuffers::Offset<nos::ContextMenuItem>> ContextMenuActions::SerializeActorMenuItems(flatbuffers::FlatBufferBuilder& fbb)
{
	std::vector<flatbuffers::Offset<nos::ContextMenuItem>> result;
	int command = 0;
	for (auto item : ActorMenu)
	{
		result.push_back(nos::CreateContextMenuItemDirect(fbb, TCHAR_TO_UTF8(*item.Key), command++, 0));
	}
	return result;
}

std::vector<flatbuffers::Offset<nos::ContextMenuItem>> ContextMenuActions::SerializePortalPropertyMenuItems(flatbuffers::FlatBufferBuilder& fbb)
{
	std::vector<flatbuffers::Offset<nos::ContextMenuItem>> result;
	int command = 0;
	for (auto item : PortalPropertyMenu)
	{
		result.push_back(nos::CreateContextMenuItemDirect(fbb, TCHAR_TO_UTF8(*item.Key), command++, 0));
	}
	return result;
}

ContextMenuActions::ContextMenuActions()
{
	TPair<FString, std::function<void(class FNOSSceneTreeManager*, AActor*)> > deleteAction(FString("Delete Actor"), [](class FNOSSceneTreeManager* NOSSceneTreeManager, AActor* actor)
		{
			NOSSceneTreeManager->ActorsDeletedFromNodos.Add(actor->GetActorGuid());
			actor->GetWorld()->EditorDestroyActor(actor, false);
		});
	ActorMenu.Add(deleteAction);
	TPair<FString, std::function<void(class FNOSSceneTreeManager*, FGuid)>> PortalDeleteAction(FString("Delete Bookmark"), [](class FNOSSceneTreeManager* NOSSceneTreeManager,FGuid id)
		{
			NOSSceneTreeManager->RemovePortal(id);	
		});
	PortalPropertyMenu.Add(PortalDeleteAction);
}

void ContextMenuActions::ExecuteActorAction(uint32 command, class FNOSSceneTreeManager* NOSSceneTreeManager, AActor* actor)
{
	if (ActorMenu.IsValidIndex(command))
	{
		ActorMenu[command].Value(NOSSceneTreeManager, actor);
	}
}
void ContextMenuActions::ExecutePortalPropertyAction(uint32 command, class FNOSSceneTreeManager* NOSSceneTreeManager, FGuid PortalId)
{
	if (PortalPropertyMenu.IsValidIndex(command))
	{
		PortalPropertyMenu[command].Value(NOSSceneTreeManager, PortalId);
	}
}