/*
 * Copyright MediaZ Teknoloji A.S. All Rights Reserved.
 */

#pragma once

#include "Engine/EngineTypes.h"

#include "CoreMinimal.h"
#include <numeric>
#include "Logging/LogMacros.h"

#pragma warning (disable : 4800)
#pragma warning (disable : 4668)

#include "Nodos/AppAPI.h"
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
	ExecuteInfo PopFrameNumber(uint64_t frameNumber)
	{
		ExecuteInfo executeInfo{};
		DiscardExcessThenDequeue(executeInfo, frameNumber, true);
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
	void DiscardExcessThenDequeue(ExecuteInfo& result, uint64_t requestedFrameNumber, bool wait)
	{
		std::scoped_lock lock(Guard);
		u32 tryCount = 0;
		bool dequeued = false;
		bool oldLiveNow = LiveNow;
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

				return !LiveNow || !wait || tryCount++ > 20;
			}, 0.001f);

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
DECLARE_EVENT_OneParam(FNOSClient, FNOSNodeConnected, nos::fb::Node const*);
DECLARE_EVENT_OneParam(FNOSClient, FNOSNodeUpdated, nos::fb::Node const&);
DECLARE_EVENT_OneParam(FNOSClient, FNOSContextMenuRequested, nos::app::AppContextMenuRequest const&);
DECLARE_EVENT_OneParam(FNOSClient, FNOSContextMenuCommandFired, nos::app::AppContextMenuAction const&);
DECLARE_EVENT(FNOSClient, FNOSNodeRemoved);
DECLARE_EVENT_FourParams(FNOSClient, FNOSPinValueChanged, nos::fb::UUID const&, uint8_t const*, size_t, bool);
DECLARE_EVENT_TwoParams(FNOSClient, FNOSPinShowAsChanged, nos::fb::UUID const&, nos::fb::ShowAs);
DECLARE_EVENT_OneParam(FNOSClient, FNOSFunctionCalled, nos::app::FunctionCall const&);
DECLARE_EVENT_OneParam(FNOSClient, FNOSNodeSelected, nos::fb::UUID const&);
DECLARE_EVENT_OneParam(FNOSClient, FNOSNodeImported, nos::fb::Node const&);
DECLARE_EVENT(FNOSClient, FNOSConnectionClosed);

// DECLARE_EVENT_OneParam(FNOSClient, FNOSConsoleCommandExecuted, FString);

/**
 * Implements communication with the Nodos Engine
 */
class FNOSClient;

class NOSCLIENT_API NOSEventDelegates : public nos::app::IEventDelegates
{
public:
	virtual ~NOSEventDelegates() {}

	virtual void HandleEvent(const nos::app::EngineEvent* event) override;
	virtual void OnConnectionClosed() override;
	void OnAppConnected(nos::fb::Node const* appNode);
	void OnNodeUpdated(nos::fb::Node const& appNode);
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
	void OnLoadNodesOnPaths(nos::app::LoadNodesOnPaths const* loadNodesOnPathsRequest);
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

class NOSCLIENT_API FNodos
{
public:
	static FString GetNodosSDKDir();
	static bool Initialize();
	static void Shutdown();
	static nos::app::FN_MakeAppServiceClient* MakeAppServiceClient;
	static nos::app::FN_ShutdownClient* ShutdownClient;
private:
	// Nodos SDK DLL handle
	static void* LibHandle;
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
	virtual void Connected();

	//This function is called when the connection with the Nodos Engine is finished
	virtual void Disconnected();
	 
	/// @return Connection status with Nodos Engine
	virtual bool IsConnected();

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
	
	//Grpc client to communicate
	TSharedPtr<NOSEventDelegates> EventDelegates = 0;

	//To send events to Nodos and communication
	nos::app::IAppServiceClient* AppServiceClient = nullptr;

	//Task queue
	TQueue<Task, EQueueMode::Mpsc> TaskQueue;

	//Custom time step implementation for Nodos controlling the unreal editor in play mode
	UPROPERTY()
	TWeakObjectPtr<UNOSCustomTimeStep> NOSTimeStep = nullptr;
	bool CustomTimeStepBound = false;

	// Nodos root node id
	static FGuid NodeId;
	// The app key we are using for Nodos
	static FString AppKey;

	TMap<FGuid, FName> PathUpdates;

	FNOSNodeConnected OnNOSConnected;
	FNOSNodeUpdated OnNOSNodeUpdated;
	FNOSContextMenuRequested OnNOSContextMenuRequested;
	FNOSContextMenuCommandFired OnNOSContextMenuCommandFired;
	FNOSNodeRemoved OnNOSNodeRemoved;
	FNOSPinValueChanged OnNOSPinValueChanged;
	FNOSPinShowAsChanged OnNOSPinShowAsChanged;
	FNOSFunctionCalled OnNOSFunctionCalled;
	FNOSNodeSelected OnNOSNodeSelected;
	FNOSNodeImported OnNOSNodeImported;
	FNOSConnectionClosed OnNOSConnectionClosed;
	TMulticastDelegate<void(nos::app::ExecutionState), FDefaultTSDelegateUserPolicy> OnNOSStateChanged_GRPCThread;
	TMulticastDelegate<void(const TArray<FString>&), FDefaultTSDelegateUserPolicy> OnNOSLoadNodesOnPaths;
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
	NOSConsoleOutput(FNOSClient* NOSClient)
		: FOutputDevice(), NOSClient(NOSClient)
	{
	}

	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const class FName& Category) override
	{
		if (!NOSClient || !NOSClient->AppServiceClient || FString(V).IsEmpty())
		{
			return;
		}
		
		flatbuffers::FlatBufferBuilder mb;
		auto offset = nos::CreateAppEventOffset(mb ,nos::app::CreateConsoleOutputDirect(mb, TCHAR_TO_UTF8(V)));
		mb.Finish(offset);
		auto buf = mb.Release();
		auto root = flatbuffers::GetRoot<nos::app::AppEvent>(buf.data());
		NOSClient->AppServiceClient->Send(*root);
	}
};


