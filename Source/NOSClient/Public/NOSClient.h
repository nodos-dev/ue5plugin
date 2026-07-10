/*
 * Copyright MediaZ Teknoloji A.S. All Rights Reserved.
 */

#pragma once

#include "Engine/EngineTypes.h"

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include <numeric>
#include "Logging/LogMacros.h"
#include "Engine/World.h"
#include "Modules/ModuleInterface.h"

#pragma warning (disable : 4800)
#pragma warning (disable : 4668)

#include "Nodos/AppHelpers.hpp"
#include <uuid.h>
#include "nosFlatBuffersCommon.h"
#include "AppEvents_generated.h"
#include <nosFlatBuffersCommon.h>
#include <functional> 

struct ExecuteInfo
{
	uint64_t FrameNumber;
	TArray<TPair<uuids::uuid, nos::Buffer>> PinValueUpdates;
};
struct ExecuteFrameNumberQueue : public TQueue<ExecuteInfo>
{
	ExecuteInfo PopFrameNumber(uint64_t frameNumber, float maxWaitTime)
	{
		ExecuteInfo executeInfo{};
		DiscardExcessThenDequeue(executeInfo, frameNumber, true, maxWaitTime);
		return executeInfo;
	}
	void EnqueueExecuteStart(nos::app::AppExecuteStart const* appExecuteStart)
	{
		ExecuteInfo start{};
		start.FrameNumber = appExecuteStart->frame_counter();
		if (auto* pinValueUpdates = appExecuteStart->pin_value_updates())
			for (auto const& pinValueUpdate : *pinValueUpdates)
			{
				uuids::uuid pinId(pinValueUpdate->pin_id()->bytes()->begin(), pinValueUpdate->pin_id()->bytes()->end());
				start.PinValueUpdates.Emplace(pinId, nos::Buffer(pinValueUpdate->value()->data(), pinValueUpdate->value()->size()));
			}
		if (appExecuteStart->reset())
		{
			std::scoped_lock lock(Guard);
			Empty();
		}
		else
			Enqueue(std::move(start));
	}
private:
	void DiscardExcessThenDequeue(ExecuteInfo& result, uint64_t requestedFrameNumber, bool wait, float maxWaitTime)
	{
		std::scoped_lock lock(Guard);
		uint32_t tryCount = 0;
		bool dequeued = false;
		bool oldLiveNow = LiveNow;
		constexpr int retryCount = 20;
		FPlatformProcess::ConditionalSleep([&]()
			{
				while (Peek(result))
				{
					LiveNow = true;
					dequeued = true;
					if (result.FrameNumber <= requestedFrameNumber)
					{
						ExecuteInfo pop;
						Pop();
					}
					if (result.FrameNumber >= requestedFrameNumber)
					{
						return true;
					}
				}

				return !LiveNow || !wait || tryCount++ > retryCount;
			}, maxWaitTime / retryCount);

		LiveNow = dequeued;
		if (oldLiveNow != LiveNow)
			UE_LOG(LogCore, Warning, TEXT("LiveNow Changed"));

		if (LiveNow && result.FrameNumber != requestedFrameNumber)
			UE_LOG(LogCore, Warning, TEXT("Mismatch between popped frame number and requested frame number: %i, %i"), result.FrameNumber, requestedFrameNumber);
	}
	
	bool LiveNow = true;
	std::mutex Guard;
};

class UNOSCustomTimeStep;
typedef std::function<void()> Task;

DECLARE_LOG_CATEGORY_EXTERN(LogNOSClient, Log, All);

//events coming from Nodos
DECLARE_EVENT(FNOSClient, FNOSNodeConnected);
DECLARE_EVENT_OneParam(FNOSClient, FNOSContextMenuRequested, nos::app::AppContextMenuRequest const&);
DECLARE_EVENT_OneParam(FNOSClient, FNOSContextMenuCommandFired, nos::app::AppContextMenuAction const&);
DECLARE_EVENT(FNOSClient, FNOSPreNodeRemoved);
DECLARE_EVENT(FNOSClient, FNOSNodeRemoved);
DECLARE_EVENT_FourParams(FNOSClient, FNOSPinValueChanged, nos::fb::UUID const&, uint8_t const*, size_t, bool);
DECLARE_EVENT_TwoParams(FNOSClient, FNOSPinShowAsChanged, nos::fb::UUID const&, nos::fb::ShowAs);
DECLARE_EVENT_OneParam(FNOSClient, FNOSFunctionCalled, nos::app::FunctionCall const&);
DECLARE_EVENT_OneParam(FNOSClient, FNOSNodeSelected, nos::fb::UUID const&);
DECLARE_EVENT_OneParam(FNOSClient, FNOSNodeImported, nos::fb::Node const&);
DECLARE_EVENT(FNOSClient, FNOSConnectionClosed);
DECLARE_EVENT_TwoParams(FNOSClient, FNOSActorSpawnedDestroyed, AActor*, bool);

// DECLARE_EVENT_OneParam(FNOSClient, FNOSConsoleCommandExecuted, FString);

/**
 * Implements communication with the Nodos Engine
 */
class FNOSClient;

class NOSCLIENT_API NOSEventDelegates : public nos::app::AppEventDelegates
{
public:
	virtual ~NOSEventDelegates() {}

	virtual void HandleEvent(const nos::app::EngineEvent* event) override;
	virtual void OnConnectionClosed() override;
	void OnAppConnected();
	void OnContextMenuRequested(nos::app::AppContextMenuRequest const& request);
	void OnContextMenuCommandFired(nos::app::AppContextMenuAction const& action);
	void OnNodeRemoved();
	void OnPinValueChanged(nos::fb::UUID const& pinId, uint8_t const* data, size_t size, bool reset, uint64_t frameNumber);
	void OnPinShowAsChanged(nos::fb::UUID const& pinId, nos::fb::ShowAs newShowAs);
	void OnExecuteAppInfo(nos::app::AppExecuteInfo const* appExecuteInfo);
	void OnFunctionCall(nos::app::FunctionCall const* functionCall);
	void OnNodeSelected(nos::fb::UUID const& nodeId);
	void OnNodeImported(nos::fb::Node const& appNode);
	void OnStateChanged(nos::app::ExecutionState newState);
	void OnConsoleCommand(nos::app::ConsoleCommand const* consoleCommand);
	void OnConsoleAutoCompleteSuggestionRequest(nos::app::ConsoleAutoCompleteSuggestionRequest const* consoleAutoCompleteSuggestionRequest);
	void OnLoadNodesOnPaths(nos::app::LoadNodesOnPaths const* loadNodesOnPathsRequest, nos::fb::UUID const* requestId);
	void OnCloseApp();
	void OnExecuteStart(nos::app::AppExecuteStart const* appExecuteStart);

	FNOSClient* PluginClient;

	ExecuteFrameNumberQueue ExecuteQueue{};
};

class NOSCLIENT_API UENodeStatusHandler
{
public:
	void SetClient(FNOSClient* PluginClient);
	void Add(std::string const& Id, nos::fb::TNodeStatusMessage const& Status);
	void Remove(std::string const& Id);
	void Update();
private:
	void SendStatus();
	FNOSClient* PluginClient = nullptr;
	std::unordered_map<std::string, nos::fb::TNodeStatusMessage> StatusMessages;
	bool Dirty = false;
};

class FPSCounter
{
public:
	bool Update(float dt);
	nos::fb::TNodeStatusMessage GetNodeStatusMessage() const;
private:
	float DeltaTimeAccum = 0;
	uint64_t FrameCount = 0;
	float FramesPerSecond = 0;
};

class NOSCLIENT_API FNodos : public nos::app::IAppApiProcLoader
{
public:
	FString GetNodosSDKDir();
	bool Initialize();
	void Shutdown();
	std::shared_ptr<nos::app::AppApi> Api;
	ProcFuncPtr GetProcAddress(const char* name) const override;
private:
	// Nodos SDK DLL handle
	void* LibHandle = nullptr;
};

extern FNodos GNodos;

template <typename DelegateT>
class Chain : public DelegateT
{
public:
	using Super = DelegateT;
	using HandleT = FDelegateHandle;

	// expose head bind API
	using Super::AddRaw;
	using Super::AddUObject;
	using Super::AddSP;
	using Super::AddLambda;
	using Super::Remove;
	using Super::Clear;
	using Super::IsBound;

	// tail access
	DelegateT& Tail() { return TailDelegate; }
	const DelegateT& Tail() const { return TailDelegate; }

	// tail bind helpers
	template <typename UserClass>
	HandleT AddTailRaw(UserClass* Obj, auto Method) { return TailDelegate.AddRaw(Obj, Method); }
	template <typename UserClass>
	HandleT AddTailUObject(UserClass* Obj, auto Method) { return TailDelegate.AddUObject(Obj, Method); }
	template <typename UserClass, ESPMode Mode>
	HandleT AddTailSP(TSharedPtr<UserClass, Mode> Obj, auto Method) { return TailDelegate.AddSP(Obj, Method); }
	template <typename Functor>
	HandleT AddTailLambda(Functor&& Fn) { return TailDelegate.AddLambda(Forward<Functor>(Fn)); }

	// chained broadcast: head then tail (perfect-forwarded)
	template <typename... CallArgs>
	void Broadcast(CallArgs&&... a)
	{
		Super::Broadcast(Forward<CallArgs>(a)...);
		TailDelegate.Broadcast(Forward<CallArgs>(a)...);
	}

	void ClearAll() { Super::Clear(); TailDelegate.Clear(); }

private:
	DelegateT TailDelegate;
};


class NOSCLIENT_API FNOSClient : public IModuleInterface {

public:
	 
	//Empty constructor
	FNOSClient();

	//Called on startup of the module on Unreal Engine start
	virtual void StartupModule() override;

	//Called on shutdown of the module on Unreal Engine exit
	virtual void ShutdownModule() override;

	//This function is called when the connection with the Nodos Engine is started
	void Connected_GrpcThread();

	void OnStateChanged_GrpcThread(nos::app::ExecutionState newState);
	void NodeImported_GrpcThread(const nos::fb::Node& node);
	void NodeRemoved_GrpcThread();

	//This function is called when the connection with the Nodos Engine is finished
	void Disconnected_GrpcThread();
	 
	/// @return Connection status with Nodos Engine
	bool IsConnected();

	//Tries to initialize connection with the Nodos engine
	void TryConnect();

	void Initialize();

	//Tick is called every frame once and handles the tasks queued from grpc threads
	bool Tick(float dt);

	void OnBeginFrame();

	//Called when the level is initiated
	void OnPostWorldInit(UWorld* World, const UWorld::InitializationValues InitValues);
	 
	//Called when the level destruction began
	void OnPreWorldFinishDestroy(UWorld* World);

	//Called when the node is executed from Nodos
	void OnUpdatedNodeExecuted(nos::fb::vec2u deltaSeconds);

	bool ExecuteConsoleCommand(const TCHAR* Input);

	bool ExecInternal(const TCHAR* Input);

	void TogglePlayInEditor();

	//Grpc client to communicate
	TSharedPtr<NOSEventDelegates> EventDelegates = 0;

	//To send events to Nodos and communication
	std::unique_ptr<nos::app::AppServiceClient> AppServiceClient = nullptr;

	//Task queue
	TQueue<Task, EQueueMode::Mpsc> TaskQueue;

	//Custom time step implementation for Nodos controlling the unreal editor in play mode
	TObjectPtr<UNOSCustomTimeStep> NOSTimeStep = nullptr;

	// Nodos root node id
	static FGuid NodeId;
	// The app key we are using for Nodos
	static FString AppKey;
	bool NodePresent_GrpcThread = false;

	TMap<FGuid, FName> PathUpdates;

	Chain<FNOSNodeConnected> OnNOSConnected;
	Chain<FNOSContextMenuRequested> OnNOSContextMenuRequested;
	Chain<FNOSContextMenuCommandFired> OnNOSContextMenuCommandFired;
	Chain<FNOSPreNodeRemoved> OnNOSPreNodeRemoved;
	Chain<FNOSNodeRemoved> OnNOSNodeRemoved;
	Chain<FNOSPinValueChanged> OnNOSPinValueChanged;
	Chain<FNOSPinShowAsChanged> OnNOSPinShowAsChanged;
	Chain<FNOSFunctionCalled> OnNOSFunctionCalled;
	Chain<FNOSNodeSelected> OnNOSNodeSelected;
	Chain<FNOSNodeImported> OnNOSNodeImported;
	Chain<FNOSConnectionClosed> OnNOSConnectionClosed;
	TMulticastDelegate<void(nos::app::ExecutionState), FDefaultTSDelegateUserPolicy> OnNOSStateChanged_GRPCThread;
	TMulticastDelegate<void(const TArray<FString>&, FGuid), FDefaultTSDelegateUserPolicy> OnNOSLoadNodesOnPaths;
	Chain<FNOSActorSpawnedDestroyed> OnNOSActorSpawnedDestroyed;
	// FNOSConsoleCommandExecuted OnNOSConsoleCommandExecuted;
	
	UENodeStatusHandler UENodeStatusHandler;

	int ReloadingLevel = 0;

	bool bIsInitialized = false;
	
protected:
	void Reset();

	FPSCounter FPSCounter;
	bool IsWorldInitialized = false;

};

class NOSConsoleOutput : public FOutputDevice
{
public:
	FNOSClient* NOSClient;
	const TCHAR* Input = nullptr;
	NOSConsoleOutput(FNOSClient* NOSClient, const TCHAR* input)
		: FOutputDevice(), NOSClient(NOSClient), Input(input)
	{
	}

	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const class FName& Category) override
	{
		if (!NOSClient || !NOSClient->AppServiceClient || FString(V).IsEmpty())
		{
			return;
		}
		
		flatbuffers::FlatBufferBuilder mb;
		auto offset = nos::CreateAppEventOffset(mb ,nos::app::CreateConsoleOutputDirect(mb, TCHAR_TO_UTF8(V), Input ? TCHAR_TO_UTF8(Input) : nullptr));
		mb.Finish(offset);
		auto buf = mb.Release();
		auto root = flatbuffers::GetRoot<nos::app::AppEvent>(buf.data());
		NOSClient->AppServiceClient->Send(root);
	}
};
