/*
 * Copyright MediaZ Teknoloji A.S. All Rights Reserved.
 */

#pragma once
#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "Nodos/AppAPI.h"
#include "AppEvents_generated.h"
#include <nosFlatBuffersCommon.h>
#include "NOSSceneTree.h"
#include "NOSClient.h"
#include "NOSViewportClient.h"
#include "NOSAssetManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogNOSSceneTreeManager, Log, All);

struct NOSPortal
{
	FGuid Id;
	FGuid SourceId;

	FString DisplayName;
	FString TypeName;
	FString CategoryName;
	nos::fb::ShowAs ShowAs;
	FString UniqueName;
};

//Accumulates the node updates produced during a single OnNOSLoadNodesOnPaths call so they can be
//flushed to Nodos as one BatchAppEvent instead of many individual SendPartialNodeUpdate messages.
//Builder holds the serialized data; Events holds one AppEvent offset (a PartialNodeUpdate) per node.
struct FNodeUpdateBatch
{
	flatbuffers::FlatBufferBuilder Builder;
	std::vector<flatbuffers::Offset<nos::app::AppEvent>> Events;
};

//This class holds the list of all properties and pins 
class NOSSCENETREEMANAGER_API FNOSPropertyManager
{
public:
	FNOSPropertyManager(NOSSceneTree& sceneTree);

	TSharedPtr<NOSProperty> CreateProperty(UObject* container,
		FProperty* uproperty,
		FString parentCategory = FString(""),
		bool bAddToCache = true);

	void SetPropertyValue();
	bool CheckPinShowAs(nos::fb::CanShowAs CanShowAs, nos::fb::ShowAs ShowAs);
	void CreatePortal(FGuid PropertyId, nos::fb::ShowAs ShowAs);
	void CreatePortal(FProperty* uproperty, UObject* Container, nos::fb::ShowAs ShowAs);
	void ActorDeleted(FGuid DeletedActorId);
	flatbuffers::Offset<nos::fb::Pin> SerializePortal(flatbuffers::FlatBufferBuilder& fbb, NOSPortal Portal, NOSProperty* SourceProperty);
	void CreatePortalForTransformProperty(USceneComponent* RootComponent, const FName& Name);
	
	FNOSClient* NOSClient = nullptr;
	NOSSceneTree& SceneTree;

	TMap<FGuid, FGuid> PropertyToPortalPin;
	TMap<FGuid, NOSPortal> PortalPinsById;
	TMap<FGuid, TSharedPtr<NOSProperty>> PropertiesById;

	TMap<TPair<FProperty*, void*>, TSharedPtr<NOSProperty>> PropertiesByPropertyAndContainer;
	TMap<TPair<void*, UFunction*>, TSharedPtr<NOSFunction>> FunctionsByContainerAndUEFunction;
	void Reset(bool ResetPortals = true);

	TOptional<uint64_t> OnBeginFrame(bool bConsumeExecuteFrame);
	void OnEndFrame();
};

struct SavedActorData
{
	TMap<FString, FString> Metadata;
	FName NodosUniqueName;
	FName NodosDisplayName;
};

class NOSSCENETREEMANAGER_API FNOSActorManager
{
public:
	FNOSActorManager(NOSSceneTree& SceneTree) : SceneTree(SceneTree)
	{
		NOSAssetManager = &FModuleManager::LoadModuleChecked<FNOSAssetManager>("NOSAssetManager");
		NOSClient = &FModuleManager::LoadModuleChecked<FNOSClient>("NOSClient");
		RegisterDelegates();
	};

	AActor* GetParentTransformActor();
	AActor* SpawnActor(FString SpawnTag, NOSSpawnActorParameters Params = {}, TMap<FString, FString> Metadata = {});
	AActor* SpawnUMGRenderManager(FString umgTag,UUserWidget* widget);
	void ClearActors();
	AActor* GetRealityLinoManager();
	
	void ReAddActorsToSceneTree();

	void RegisterDelegates();
	void PreSave(UWorld* World, FObjectPreSaveContext Context);
	void PostSave(UWorld* World, FObjectPostSaveContext Context);


	NOSActorReference ParentTransformActor;
	NOSActorReference RealityLinoManager;

	NOSSceneTree& SceneTree;
	class FNOSAssetManager* NOSAssetManager;
	class FNOSClient* NOSClient;
	
	TSet<FGuid> ActorIds;
	TArray< TPair<NOSActorReference, SavedActorData> > Actors;
};


class ContextMenuActions
{
public:
	TArray<TPair<FString, std::function<void(class FNOSSceneTreeManager*, AActor*)>>>  ActorMenu;
	TArray<TPair<FString, Task>>  FunctionMenu;
	TArray<TPair<FString, std::function<void(class FNOSSceneTreeManager*, FGuid)>>>   PortalPropertyMenu;
	ContextMenuActions();
	std::vector<flatbuffers::Offset<nos::ContextMenuItem>> SerializeActorMenuItems(flatbuffers::FlatBufferBuilder& fbb);
	std::vector<flatbuffers::Offset<nos::ContextMenuItem>> SerializePortalPropertyMenuItems(flatbuffers::FlatBufferBuilder& fbb);
	void ExecuteActorAction(uint32 command, class FNOSSceneTreeManager* NOSSceneTreeManager, AActor* actor);
	void ExecutePortalPropertyAction(uint32 command, class FNOSSceneTreeManager* NOSSceneTreeManager, FGuid PortalId);
};


class NOSSCENETREEMANAGER_API FNOSSceneTreeManager : public IModuleInterface {

public:
	//Empty constructor
	FNOSSceneTreeManager();

	//Called on startup of the module on Unreal Engine start
	virtual void StartupModule() override;

	//Called on shutdown of the module on Unreal Engine exit
	virtual void ShutdownModule() override;

	bool Tick(float dt);
	bool CheckNewLevels(float dt);

	void OnBeginFrame();
	void OnEndFrame();

	//every function of this class runs in game thread
	void OnNOSNodeSelected(nos::fb::UUID const& nodeId);

	//called when connection is ended with Nodos
	void OnNOSConnectionClosed();

	//called when a pin value changed from Nodos
	void OnNOSPinValueChanged(nos::fb::UUID const& pinId, uint8_t const* data, size_t size, bool reset);

	//called when a pins show as changed
	void OnNOSPinShowAsChanged(nos::fb::UUID const& pinId, nos::fb::ShowAs newShowAs);

	//called when a function is called from Nodos
	void OnNOSFunctionCalled(nos::app::FunctionCall const& functionCall);

	//called when a context menu is requested on some node on Nodos
	void OnNOSContextMenuRequested(nos::app::AppContextMenuRequest const& request);

	//called when a action is selected from context menu
	void OnNOSContextMenuCommandFired(nos::app::AppContextMenuAction const& action);

	void OnNOSNodeRemoved();

	void OnNOSStateChanged_GRPCThread(nos::app::ExecutionState);
	
	void OnNOSLoadNodesOnPaths(const TArray<FString>& paths);
	//END OF Nodos DELEGATES
	 
	void PopulateAllChildsOfActor(FGuid ActorId, FNodeUpdateBatch* OptBatch = nullptr);

	void PopulateAllChildsOfSceneComponentNode(SceneComponentNode* SceneComponentNode, FNodeUpdateBatch* OptBatch = nullptr);

	void SendSyncSemaphores(bool RenewSemaphores);
	/// Ask Nodos to restart the synchronization for this node. Unreal cannot leave
	/// the synchronized state on its own: it would stop signalling the shared
	/// timelines while Nodos still waits on them. Nodos takes the node out and
	/// back in, and both sides start a fresh epoch together.
	void RequestSyncRecovery();
	
	//Called when the level is initiated
	void OnPostWorldInit(UWorld* World, const UWorld::InitializationValues InitValues);

	//Called when the level destruction began
	void OnPreWorldFinishDestroy(UWorld* World);

	void OnLevelAddedToWorld(ULevel* Level, UWorld* World);

	void OnLevelRemovedFromWorld(ULevel* Level, UWorld* World);

	//delegate called when a property is changed from unreal engine editor
	//it updates thecorresponding property in Nodos
	void OnPropertyChanged(UObject* ObjectBeingModified, FPropertyChangedEvent& PropertyChangedEvent);

	//Called when an actor is spawned into the world
	void OnActorSpawned(AActor* InActor);

	//Called when an actor is destroyed from the world
	void OnActorDestroyed(AActor* InActor);

	void OnActorAttached(AActor* Actor, const AActor* ParentActor);
	void OnActorDetached(AActor* Actor, const AActor* ParentActor);

	TSharedPtr<NOSFunction> FindFunctionByActorAndName(FGuid ActorId, const FString& FunctionName);

	//called when unreal engine node is imported from Nodos
	void OnNOSNodeImported(nos::fb::Node const& appNode);

	//Set a properties value
	void SetPropertyValue(FGuid pinId, void* newval, size_t size);

#ifdef VIEWPORT_TEXTURE
	//Set viewport texture pin's container to current viewport client's texture on play
	void ConnectViewportTexture();

	//Set viewport texture pin's container to null
	void DisconnectViewportTexture();
#endif

	//Rescans the current viewports world scene to get the current state of the scene outliner
	void RescanScene(bool reset = true);

	TSharedPtr<NOSFunction> AddFunctionToActorNode(ActorNode* actorNode, UFunction* UEFunction, UObject* Container);
	//Populates node with child actors/components, functions and properties
	bool PopulateNode(TreeNode* node);

	//Sends node updates to the Nodos. When OptBatch is given, the update is queued into it
	//instead of being sent immediately (see OnNOSLoadNodesOnPaths / FNodeUpdateBatch).
	void SendNodeUpdate(FGuid NodeId, bool bResetRootPins = true, bool filterPinsWhileSending = false, FNodeUpdateBatch* OptBatch = nullptr);

	//Serializes a node update into the given builder and returns its offset.
	//Returns a null offset when the node no longer exists in the scene tree.
	flatbuffers::Offset<nos::PartialNodeUpdate> BuildNodeUpdate(flatbuffers::FlatBufferBuilder& Builder, FGuid NodeId, bool bResetRootPins, bool filterPinsWhileSending);

	void SendEngineFunctionUpdate();

	//Sends pin value changed event to Nodos
	void SendPinValueChanged(FGuid propertyId, std::vector<uint8> data);

	//Sends pin updates to the root node 
	void SendPinUpdate();
	
	void RemovePortal(FGuid PortalId);
	
	//Sends pin to add to a node
	void SendPinAdded(FGuid NodeId, TSharedPtr<NOSProperty> const& nosprop);

	//Add to to-be-added actors list or send directly if always updating
	void SendActorAddedOnUpdate(AActor* actor, FString spawnTag = FString());

	//Adds the node to scene tree and sends it to Nodos
	void SendActorAdded(AActor* actor, FString spawnTag = FString());

	void SendActorDeletedOnUpdate(AActor* actor);

	//Deletes the node from scene tree and sends it to Nodos
	void SendActorDeleted(AActor* Actor);

	void SendParentChangedOnUpdate(FGuid Actor, FGuid ParentActor);

	void SendParentChanged(FGuid Actor, FGuid ParentActor);

	void SendActorNodeDeleted(ActorNode* node);
	
	void PopulateAllChildsOfActor(AActor* actor, FNodeUpdateBatch* OptBatch = nullptr);

	//This populates the node, its direct descendants, all of its child components and all of their children.
	void PopulateNodeAndDirectDescendants(TreeNode* Node, FNodeUpdateBatch* OptBatch = nullptr);

	void PopulateAndSendNode(TreeNode* Node, bool filterPinsWhileSending, FNodeUpdateBatch* OptBatch = nullptr);

	void ReloadCurrentMap();

	//Called when pie is started
	void HandleBeginPIE(bool bIsSimulating);

	//Called when pie is ending
	void HandleEndPIE(bool bIsSimulating);

	void HandleWorldChange();

	UObject* FindContainer(FGuid ActorId, FString ComponentName);

	void* FindContainerFromContainerPath(UObject* BaseContainer, FString ContainerPath, bool& IsResultUObject);

	// UObject* FNOSSceneTreeManager::FindObjectContainerFromContainerPath(UObject* BaseContainer, FString ContainerPath);
	//Remove properties of tree node from registered properties and pins
	void RemoveProperties(::TreeNode* Node,
	                      TSet<TSharedPtr<NOSProperty>>& PropertiesToRemove);

	void CheckPins(TSet<UObject*>& RemovedObjects,
		TSet<TSharedPtr<NOSProperty>>& PinsToRemove,
		TSet<TSharedPtr<NOSProperty>>& PropertiesToRemove);

	void Reset();

	void OnMapChange(uint32 MapFlags);
	void OnNewCurrentLevel();

	void AddCustomFunction(NOSCustomFunction* CustomFunction);
	
	void AddToBeAddedActors();
	void DeleteToBeDeletedActors();
	void ChangeParentActors();

	bool bTwoWayBindingEnabled = false;
	bool bTwoWayBindingStatusSent = false;
	void ToggleTwoWayBinding() { bTwoWayBindingEnabled = !bTwoWayBindingEnabled; }

	//the world we interested in
	static UWorld* daWorld;

	//all the properties registered 
	TMap<FGuid, TSharedPtr<NOSProperty>> RegisteredProperties;

	//all the properties registered mapped with property pointers
	TMap<FProperty*, TSharedPtr<NOSProperty>> PropertiesMap;

	//all the functions registered
	TMap<FGuid, TSharedPtr<NOSFunction>> RegisteredFunctions;

	//in/out pins of the Nodos node
	TMap<FGuid, TSharedPtr<NOSProperty>> Pins;

	//custom properties like viewport texture
	TMap<FGuid, TSharedPtr<NOSProperty>> CustomProperties;

#ifdef VIEWPORT_TEXTURE
	NOSProperty* ViewportTextureProperty;
#endif

	//custom functions like spawn actor
	TMap<FGuid, NOSCustomFunction*> CustomFunctions;

	//handles context menus and their actions
	friend class ContextMenuActions;
	class ContextMenuActions menuActions;

	//Scene tree holds the information to mimic the outliner in Nodos
	class NOSSceneTree SceneTree;
	
	//Class communicates with Nodos
	class FNOSClient* NOSClient;

	class FNOSAssetManager* NOSAssetManager;

	class FNOSViewportManager* NOSViewportManager;
	
	FNOSActorManager* NOSActorManager;

	FNOSPropertyManager NOSPropertyManager;

	bool bIsModuleFunctional = false;

	nos::app::ExecutionState ExecutionState = nos::app::ExecutionState::IDLE;

	bool ToggleExecutionStateToSynced = false;
	// The Nodos request this Unreal frame belongs to. Everything the frame does,
	// pin updates, copies, fence values and the completion, uses this one number.
	TOptional<uint64_t> ActiveNodosFrameNumber;
	// The fence generation that frame belongs to. A begin/end pair may not cross
	// an epoch boundary.
	TOptional<uint64_t> ActiveNodosFenceEpoch;
	bool ShowHiddenActorsOnNodos = false;

	bool AlwaysUpdateOnActorSpawns = false;
	TArray<TWeakObjectPtr<AActor>> ActorsToBeAdded;
	TArray<FGuid> ActorsToBeDeleted;
	TMap<FGuid, FGuid> ActorsToBeParentChanged;
	TSet<TWeakObjectPtr<ULevel>> AlreadyLoadedStreamingLevels;

	TSet<FGuid> ActorsDeletedFromNodos;

	static TSet<FGuid> PropertiesNeeded;

};

