// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include "NOSClient.h"
#include "NOSCustomTimeStep.h"
// std
#include <cstdio>
#include <string>
#include <filesystem>

// UE
#include "HardwareInfo.h"
#include "LevelEditor.h"
#include "LevelEditorActions.h"
#include "Runtime/Engine/Classes/Kismet/GameplayStatics.h"
#include "Engine/EngineCustomTimeStep.h"
#include "Misc/MessageDialog.h"
#include "ShaderCompiler.h"
#include "AssetCompilingManager.h"
#include "Interfaces/IPluginManager.h"
#include "Json.h"


//Nodos
#include "nosFlatBuffersCommon.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "Toolkits/FConsoleCommandExecutor.h"
#include "NOSSettings.h"

#define LOCTEXT_NAMESPACE "FNOSClient"
#pragma optimize("", off)

DEFINE_LOG_CATEGORY(LogNOSClient);
#define LOG(x) UE_LOG(LogNOSClient, Display, TEXT(x))
#define LOGF(x, y) UE_LOG(LogNOSClient, Display, TEXT(x), y)

FGuid FNOSClient::NodeId = {};
FString FNOSClient::AppKey = "";

void* FNodos::LibHandle = nullptr;
nos::app::FN_MakeAppServiceClient* FNodos::MakeAppServiceClient = nullptr;
nos::app::FN_ShutdownClient* FNodos::ShutdownClient = nullptr;

FString FNodos::GetNodosSDKDir()
{
	FString NosmanPath = GET_NOS_CONFIG_VALUE(NosmanPath);
	if (FPaths::IsRelative(NosmanPath))
	{
		NosmanPath = FPaths::Combine(FPaths::EngineDir(), NosmanPath);
	}

	FString NosmanWorkingDirectory = FPaths::GetPath(NosmanPath);
	//execute nosman to get the path
	int32 ReturnCode;
	FString OutResults;
	FString OutErrors;
	if (!FPaths::FileExists(*NosmanPath))
	{
		return "";
	}
	FPlatformProcess::ExecProcess(*NosmanPath, TEXT("sdk-info 1.2.0"), &ReturnCode, &OutResults, &OutErrors, *NosmanWorkingDirectory);
	if(ReturnCode != 0)
		FPlatformProcess::ExecProcess(*NosmanPath, TEXT("sdk-info 16.0.0 process"), &ReturnCode, &OutResults, &OutErrors, *NosmanWorkingDirectory);
	LOGF("Nodos SDK path is %s", *OutResults);

	TSharedPtr<FJsonObject> SDKInfoJsonParsed;
	TSharedRef<TJsonReader<TCHAR>> SDKInfoJsonReader = TJsonReaderFactory<TCHAR>::Create(OutResults);
	if (!FJsonSerializer::Deserialize(SDKInfoJsonReader, SDKInfoJsonParsed))
	{
		return "";
	}

	FString SDKPath = SDKInfoJsonParsed->GetStringField("path");

	return SDKPath;
}

bool FNodos::Initialize()
{
	FString SdkPath = GetNodosSDKDir();
	FString SdkBinPath = FPaths::Combine(SdkPath, TEXT("bin"));
	FPlatformProcess::PushDllDirectory(*SdkBinPath);
	FString SdkDllPath = FPaths::Combine(SdkBinPath, "nosAppSDK.dll");

	FString CmdAppKey;
	if (FParse::Value(FCommandLine::Get(), TEXT("nosname"), CmdAppKey))
	{
		LOGF("Nodos app key is provided: %s", *CmdAppKey);
		FNOSClient::AppKey = CmdAppKey;
	}
	else
	{
		FNOSClient::AppKey = "UE5";
	}
	
	if (!FPaths::FileExists(SdkDllPath))
	{
		UE_LOG(LogNOSClient, Error, TEXT("Failed to find the nosAppSDK.dll at %s. Plugin will not be functional."), *SdkPath);
		return false;
	}

	LibHandle = FPlatformProcess::GetDllHandle(*SdkDllPath);

	if (LibHandle == nullptr)
	{
		UE_LOG(LogNOSClient, Error, TEXT("Failed to load required library %s. Plugin will not be functional."), *SdkDllPath);
		return false;
	}

	auto CheckCompatible = (nos::app::FN_CheckSDKCompatibility*)FPlatformProcess::GetDllExport(LibHandle, TEXT("CheckSDKCompatibility"));
	bool IsCompatible = CheckCompatible && CheckCompatible(NOS_APPLICATION_SDK_VERSION_MAJOR, NOS_APPLICATION_SDK_VERSION_MINOR, NOS_APPLICATION_SDK_VERSION_PATCH);
	if (!IsCompatible)
	{
		UE_LOG(LogNOSClient, Error, TEXT("Nodos SDK is incompatible with the plugin. The plugin uses a different version of the SDK (%s) that what is available in your system."), *SdkDllPath)
		return false;
	}

	MakeAppServiceClient = (nos::app::FN_MakeAppServiceClient*)FPlatformProcess::GetDllExport(LibHandle, TEXT("MakeAppServiceClient"));
	ShutdownClient = (nos::app::FN_ShutdownClient*)FPlatformProcess::GetDllExport(LibHandle, TEXT("ShutdownClient"));
	
	if (!MakeAppServiceClient || !ShutdownClient)
	{
		UE_LOG(LogNOSClient, Error, TEXT("Failed to load some of the functions in Nodos SDK. The plugin uses a different version of the SDK (%s) that what is available in your system."), *SdkDllPath)
		return false;
	}

	return true;
}

void FNodos::Shutdown()
{
	if (LibHandle)
	{
		FPlatformProcess::FreeDllHandle(LibHandle);
		LibHandle = nullptr;
		MakeAppServiceClient = nullptr;
		ShutdownClient = nullptr;
		LOG("Unloaded Nodos SDK dll successfully.");
	}
}

TMap<FGuid, std::vector<uint8>> ParsePins(nos::fb::Node const& archive)
{
	TMap<FGuid, std::vector<uint8>> re;
	if (!flatbuffers::IsFieldPresent(&archive, nos::fb::Node::VT_PINS))
	{
		return re;
	}
	for (auto pin : *archive.pins())
	{
		std::vector<uint8> data(pin->data()->size(), 0);
		memcpy(data.data(), pin->data()->data(), data.size());
		re.Add(*(FGuid*)pin->id()->bytes()->Data(), data);
	}
	return re;
}

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

void NOSEventDelegates::HandleEvent(const nos::app::EngineEvent* event)
{
	using namespace nos::app;
	switch (event->event_type())
	{
	case EngineEventUnion::AppConnectedEvent: {
		OnAppConnected(event->event_as<AppConnectedEvent>()->node());
		break;
	}
	case EngineEventUnion::FullNodeUpdate: {
		OnNodeUpdated(*event->event_as<nos::FullNodeUpdate>()->node());
		break;
	}
	case EngineEventUnion::AppContextMenuRequest: {
		OnContextMenuRequested(*event->event_as<AppContextMenuRequest>());
		break;
	}
	case EngineEventUnion::AppContextMenuAction: {
		OnContextMenuCommandFired(*event->event_as<AppContextMenuAction>());
		break;
	}
	case EngineEventUnion::NodeRemovedEvent: {
		OnNodeRemoved();
		break;
	}
	case EngineEventUnion::AppPinShowAsChanged: {
		auto const& pinShowAsChanged = event->event_as<AppPinShowAsChanged>();
		OnPinShowAsChanged(*pinShowAsChanged->pin_id(), pinShowAsChanged->show_as());
		break;
	}
	case EngineEventUnion::AppPinValueChanged: {
		auto const& pinValueChanged = event->event_as<AppPinValueChanged>();
		OnPinValueChanged(*pinValueChanged->pin_id(),
			pinValueChanged->value()->Data(),
			pinValueChanged->value()->size(),
			pinValueChanged->reset(),
			pinValueChanged->frame_number());
		break;
	}
	case EngineEventUnion::FunctionCall: {
		auto const& functionCall = event->event_as<nos::app::FunctionCall>();
		OnFunctionCall(functionCall);
		break;
	}
	case EngineEventUnion::NotifyNodeSelected: {
		OnNodeSelected(*event->event_as<NotifyNodeSelected>()->node_id());
		break;
	}
	case EngineEventUnion::NodeImported: {
		OnNodeImported(*event->event_as<nos::app::NodeImported>()->node());
		break;
	}

	case EngineEventUnion::StateChanged: {
		OnStateChanged(event->event_as<nos::app::StateChanged>()->state());
		break;
	}
	case EngineEventUnion::TerminationRequest: {
		OnCloseApp();
		break;
	}
	case EngineEventUnion::ConsoleCommand: {
		OnConsoleCommand(event->event_as<nos::app::ConsoleCommand>());
		break;
	}
	case EngineEventUnion::ConsoleAutoCompleteSuggestionRequest: {
		OnConsoleAutoCompleteSuggestionRequest(event->event_as<nos::app::ConsoleAutoCompleteSuggestionRequest>());
		break;
	}
	case EngineEventUnion::AppExecuteInfo: {
		OnExecuteAppInfo(event->event_as<nos::app::AppExecuteInfo>());
		break;
	}
	case EngineEventUnion::LoadNodesOnPaths: {
		OnLoadNodesOnPaths(event->event_as<nos::app::LoadNodesOnPaths>());
		break;
	}
	case EngineEventUnion::AppExecuteStart: {
		OnExecuteStart(event->event_as<nos::app::AppExecuteStart>());
		break;
	}
	default:
		break;
	}
}

void NOSEventDelegates::OnAppConnected(nos::fb::Node const* appNode)
{
	if (appNode)
	{
		FNOSClient::NodeId = *(FGuid*)appNode->id();
	}
	
	if (!PluginClient)
	{
		return;
	}

	LOG("Connected to nosEngine");
	PluginClient->Connected();

	nos::fb::TNode copy;
	bool NodeIsPresent = false;
	if(appNode)
	{
		NodeIsPresent = true;
		appNode->UnPackTo(&copy);
	}
	PluginClient->TaskQueue.Enqueue([NOSClient = PluginClient, copy, NodeIsPresent]()
		{
			if(!NodeIsPresent)
			{
				NOSClient->OnNOSConnected.Broadcast(nullptr);
				return;
			}
			flatbuffers::FlatBufferBuilder fbb;
			auto offset = nos::fb::CreateNode(fbb, &copy);
			fbb.Finish(offset);
			auto buf = fbb.Release();
			NOSClient->OnNOSConnected.Broadcast(flatbuffers::GetRoot<nos::fb::Node>(buf.data()));
		});

    
}

void NOSEventDelegates::OnNodeUpdated(nos::fb::Node const& appNode)
{
	LOG("Node update from Nodos");

	if (!PluginClient)
	{
		return;
	}
	if (!FNOSClient::NodeId.IsValid())
	{
		FNOSClient::NodeId = *(FGuid*)appNode.id();
		PluginClient->Connected();

		nos::fb::TNode copy2;
		appNode.UnPackTo(&copy2);
		PluginClient->TaskQueue.Enqueue([NOSClient = PluginClient, copy2]()
			{
				flatbuffers::FlatBufferBuilder fbb;
				auto offset = nos::fb::CreateNode(fbb, &copy2);
				fbb.Finish(offset);
				auto buf = fbb.Release();
				NOSClient->OnNOSNodeImported.Broadcast(*flatbuffers::GetRoot<nos::fb::Node>(buf.data()));
			});
		return;
	}

	nos::fb::TNode copy;
	appNode.UnPackTo(&copy);
	PluginClient->TaskQueue.Enqueue([NOSClient = PluginClient, copy]()
		{
			flatbuffers::FlatBufferBuilder fbb;
			auto offset = nos::fb::CreateNode(fbb, &copy);
			fbb.Finish(offset);
			auto buf = fbb.Release();
			NOSClient->OnNOSNodeUpdated.Broadcast(*flatbuffers::GetRoot<nos::fb::Node>(buf.data()));
		});
}

void NOSEventDelegates::OnConnectionClosed()
{
	LOG("Connection with Nodos is finished.");
	FNOSClient::NodeId = {};
	if (!PluginClient)
	{
		return;
	}
	PluginClient->Disconnected();
	
	PluginClient->TaskQueue.Enqueue([NOSClient = PluginClient]()
		{
			NOSClient->OnNOSConnectionClosed.Broadcast();
		});
}

void NOSEventDelegates::OnStateChanged(nos::app::ExecutionState newState)
{
	LOGF("Execution state is changed from Nodos to %s", *FString(newState == nos::app::ExecutionState::SYNCED ? "synced" : "idle"));
	if (!PluginClient)
	{
		return;
	}

	PluginClient->OnNOSStateChanged_GRPCThread.Broadcast(newState);

	//PluginClient->TaskQueue.Enqueue([NOSClient = PluginClient, newState]()
	//	{
	//		NOSClient->OnNOSStateChanged.Broadcast(newState);
	//	});
	
}

void NOSEventDelegates::OnConsoleCommand(nos::app::ConsoleCommand const* consoleCommand)
{
	if(!consoleCommand)
	{
		LOG("OnConsoleCommand request is NULL");
	}
	LOGF("Console command is here from nos %s", *FString(consoleCommand->command()->c_str()));
	if (!PluginClient)
	{
		return;
	}
	FString CommandString = FString(consoleCommand->command()->c_str());
	PluginClient->TaskQueue.Enqueue([NOSClient = PluginClient, CommandString]()
		{
			NOSClient->ExecuteConsoleCommand(*CommandString);
		});
}

void NOSEventDelegates::OnConsoleAutoCompleteSuggestionRequest(
	nos::app::ConsoleAutoCompleteSuggestionRequest const* consoleAutoCompleteSuggestionRequest)
{
	if(!consoleAutoCompleteSuggestionRequest)
	{
		LOG("OnConsoleCommand request is NULL");
	}
	LOGF("Console command auto-complete request is here from nos %s", *FString(consoleAutoCompleteSuggestionRequest->input()->c_str()));
	if (!PluginClient)
	{
		return;
	}
	FString InputString = FString(consoleAutoCompleteSuggestionRequest->input()->c_str());
	PluginClient->TaskQueue.Enqueue([NOSClient = PluginClient, InputString]()
		{
		    auto CmdExec = MakeUnique<FConsoleCommandExecutor>();
		    //IModularFeatures::Get().RegisterModularFeature(IConsoleCommandExecutor::ModularFeatureName(), CmdExec.Get());
		    TArray<FString> out; 
			CmdExec->GetAutoCompleteSuggestions(*InputString, out);

			flatbuffers::FlatBufferBuilder mb;
			std::vector<flatbuffers::Offset<flatbuffers::String>> suggestions;

			for(auto sugg : out)
			{
				suggestions.push_back(mb.CreateString(TCHAR_TO_UTF8(*sugg)));
			}
		    auto offset = nos::CreateAppEventOffset(mb, nos::app::CreateConsoleAutoCompleteSuggestionsUpdateDirect(mb, &suggestions));
		    mb.Finish(offset);
		    auto buf = mb.Release();
		    auto root = flatbuffers::GetRoot<nos::app::AppEvent>(buf.data());
		    NOSClient->AppServiceClient->Send(*root);
		});
}

void NOSEventDelegates::OnLoadNodesOnPaths(nos::app::LoadNodesOnPaths const* loadNodesOnPathsRequest)
{
	LOG("LoadNodesOnPaths request from Nodos");
	if (!PluginClient || !loadNodesOnPathsRequest->child_node_paths())
	{
		return;
	}
	TArray<FString> Paths;
	for(auto path : *loadNodesOnPathsRequest->child_node_paths())
	{
		Paths.Push(path->c_str());
	}
	PluginClient->TaskQueue.Enqueue([NOSClient = PluginClient, Paths]()
		{
			NOSClient->OnNOSLoadNodesOnPaths.Broadcast(Paths);
		});
}

void NOSEventDelegates::OnCloseApp()
{
	LOG("Closing UE per Nodos request.");
	if (!PluginClient)
	{
		return;
	}
	FGenericPlatformMisc::RequestExit(false);
}

void NOSEventDelegates::OnExecuteStart(nos::app::AppExecuteStart const* appExecuteStart)
{
	ExecuteQueue.EnqueueExecuteStart(appExecuteStart);
}

void NOSEventDelegates::OnNodeRemoved()
{
	LOG("Plugin node removed from Nodos");
	if (!PluginClient)
	{
		return;
	}

	if (PluginClient->NOSTimeStep.IsValid())
	{
		GEngine->SetCustomTimeStep(nullptr);
		PluginClient->NOSTimeStep = nullptr;
		PluginClient->CustomTimeStepBound = false;
	}

	PluginClient->TaskQueue.Enqueue([NOSClient = PluginClient]()
		{
			FNOSClient::NodeId = {};
			NOSClient->OnNOSNodeRemoved.Broadcast();
		});
}

void NOSEventDelegates::OnPinValueChanged(nos::fb::UUID const& pinId, uint8_t const* data, size_t size, bool reset, uint64_t frameNumber)
{
	if (!PluginClient)
	{
		return;
	}
	std::vector<uint8_t> copy(size, 0);
	memcpy(copy.data(), data, size);
	FGuid id = *(FGuid*)&pinId;

	PluginClient->TaskQueue.Enqueue([NOSClient = PluginClient, copy, size, id, reset]()
		{
			NOSClient->OnNOSPinValueChanged.Broadcast(*(nos::fb::UUID*)&id, copy.data(), size, reset);
		});

}

void NOSEventDelegates::OnPinShowAsChanged(nos::fb::UUID const& pinId, nos::fb::ShowAs newShowAs)
{
	LOG("Pin show as changed from Nodos");
	if (!PluginClient)
	{
		return;
	}
	

	FGuid id = *(FGuid*)&pinId;
	PluginClient->TaskQueue.Enqueue([NOSClient = PluginClient, id, newShowAs]()
		{
			NOSClient->OnNOSPinShowAsChanged.Broadcast(*(nos::fb::UUID*)&id, newShowAs);
		});
}

void NOSEventDelegates::OnFunctionCall(nos::app::FunctionCall const* functionCall)
{
	LOG("Function called from Nodos");
	if (!PluginClient)
	{
		return;
	}
	nos::app::TFunctionCall copy;
	functionCall->UnPackTo(&copy);
	PluginClient->TaskQueue.Enqueue([NOSClient = PluginClient, funcCall = std::move(copy)]()
		{
			auto funcCallFb = nos::Table<nos::app::FunctionCall>::From(funcCall);
			NOSClient->OnNOSFunctionCalled.Broadcast(*funcCallFb);
		});
}

void NOSEventDelegates::OnExecuteAppInfo(nos::app::AppExecuteInfo const* appExecuteInfo)
{
	if (!PluginClient)
	{
		return;
	}
	
	PluginClient->OnUpdatedNodeExecuted(*appExecuteInfo->delta_seconds());
}

void NOSEventDelegates::OnNodeSelected(nos::fb::UUID const& nodeId)
{
	LOG("Node selected from Nodos");
	if (!PluginClient)
	{
		return;
	}
	FGuid id = *(FGuid*)&nodeId;
	PluginClient->TaskQueue.Enqueue([NOSClient = PluginClient, id]()
		{
			NOSClient->OnNOSNodeSelected.Broadcast(*(nos::fb::UUID*)&id);
		});
}

void NOSEventDelegates::OnContextMenuRequested(nos::app::AppContextMenuRequest const& request)
{
	LOG("Context menu fired from Nodos");
	if (!PluginClient)
	{
		return;
	}

	
	nos::app::TAppContextMenuRequest copy;
	request.UnPackTo(&copy);
	PluginClient->TaskQueue.Enqueue([NOSClient = PluginClient, copy]()
		{
			flatbuffers::FlatBufferBuilder fbb;
			auto offset = nos::app::CreateAppContextMenuRequest(fbb, &copy);
			fbb.Finish(offset);
			auto buf = fbb.Release();
			NOSClient->OnNOSContextMenuRequested.Broadcast(*flatbuffers::GetRoot < nos::app::AppContextMenuRequest > (buf.data()));
		});
}

void NOSEventDelegates::OnContextMenuCommandFired(nos::app::AppContextMenuAction const& action)
{
	LOG("Context menu command fired from Nodos");
	if (!PluginClient)
	{
		return;
	}
		
	nos::app::TAppContextMenuAction copy;
	action.UnPackTo(&copy);
	PluginClient->TaskQueue.Enqueue([NOSClient = PluginClient, copy]()
		{
			flatbuffers::FlatBufferBuilder fbb;
			auto offset = nos::app::CreateAppContextMenuAction(fbb, &copy);
			fbb.Finish(offset);
			auto buf = fbb.Release();
			NOSClient->OnNOSContextMenuCommandFired.Broadcast(*flatbuffers::GetRoot<nos::app::AppContextMenuAction>(buf.data()));
		});
}

void NOSEventDelegates::OnNodeImported(nos::fb::Node const& appNode)
{
	LOG("Node imported from Nodos");
	if (!PluginClient)
	{
		return;
	}


	nos::fb::TNode copy;
	appNode.UnPackTo(&copy);
	PluginClient->TaskQueue.Enqueue([NOSClient = PluginClient, copy]()
		{
			flatbuffers::FlatBufferBuilder fbb;
			auto offset = nos::fb::CreateNode(fbb, &copy);
			fbb.Finish(offset);
			auto buf = fbb.Release();
			FNOSClient::NodeId = *(FGuid*)&copy.id;
			NOSClient->OnNOSNodeImported.Broadcast(*flatbuffers::GetRoot<nos::fb::Node>(buf.data()));

			auto WorldContext = GEngine->GetWorldContextFromGameViewport(GEngine->GameViewport);
			if (WorldContext->World())
			{
				nos::fb::TNodeStatusMessage MapNameStatus;
				MapNameStatus.text = TCHAR_TO_UTF8(*WorldContext->World()->GetMapName());
				MapNameStatus.type = nos::fb::NodeStatusMessageType::INFO;
				NOSClient->UENodeStatusHandler.Add("map_name", MapNameStatus);
			}
		});
}

FNOSClient::FNOSClient() {}

bool FNOSClient::IsConnected()
{
	return AppServiceClient && AppServiceClient->IsConnected();
}

void FNOSClient::Connected()
{
	TaskQueue.Enqueue([&]()
		{
			LOG("Sent map information to Nodos");
			auto WorldContext = GEngine->GetWorldContextFromGameViewport(GEngine->GameViewport);
			if (WorldContext->World())
			{
				nos::fb::TNodeStatusMessage MapNameStatus;
				MapNameStatus.text = TCHAR_TO_UTF8(*WorldContext->World()->GetMapName());
				MapNameStatus.type = nos::fb::NodeStatusMessageType::INFO;
				UENodeStatusHandler.Add("map_name", MapNameStatus);
			}
		});
}

void FNOSClient::Disconnected()
{
	if (NOSTimeStep.IsValid())
	{
		GEngine->SetCustomTimeStep(nullptr);
		NOSTimeStep = nullptr;
		CustomTimeStepBound = false;
	}
}

void FNOSClient::TryConnect()
{
	if (!IsWorldInitialized)
	{
		return;
	}

	if (!AppServiceClient && FNodos::MakeAppServiceClient)
	{
		auto ProjectPath = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
		auto ExePath = FString(FPlatformProcess::ExecutablePath());
		AppServiceClient = FNodos::MakeAppServiceClient("localhost:50053", nos::app::ApplicationInfo {
			.AppKey = TCHAR_TO_UTF8(*FNOSClient::AppKey),
			.AppName = "UE5"
		});
		EventDelegates = TSharedPtr<NOSEventDelegates>(new NOSEventDelegates());
		EventDelegates->PluginClient = this;
		UENodeStatusHandler.SetClient(this);
		AppServiceClient->RegisterEventDelegates(EventDelegates.Get());
		LOG("AppClient instance is created");
	}

	if (AppServiceClient)
	{
		AppServiceClient->TryConnect();
	}

	// (Samil) TODO: This connection logic should be provided by the SDK itself. 
	// App developers should not be required implement 'always connect' behaviour.
	//if (!Client->IsChannelReady)
	//{
	//	Client->IsChannelReady = (GRPC_CHANNEL_READY == Client->Connect());
	//}

	 if (!CustomTimeStepBound && IsConnected())
	 {
	 	NOSTimeStep = NewObject<UNOSCustomTimeStep>();
	 	NOSTimeStep->PluginClient = this;
	 	if (GEngine->SetCustomTimeStep(NOSTimeStep.Get()))
	 	{
	 		CustomTimeStepBound = true;
	 	}
	 }
	return;
}

void FNOSClient::Initialize()
{
	if (GEditor)
	{
		nos::fb::TNodeStatusMessage EditorWarningStatus;
		EditorWarningStatus.text = "WARNING: Editor Mode, Preview only!";
		EditorWarningStatus.type = nos::fb::NodeStatusMessageType::WARNING;
		UENodeStatusHandler.Add("editor_warning", EditorWarningStatus);
	}

	//Add Delegates
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FNOSClient::Tick));
	FWorldDelegates::OnPostWorldInitialization.AddRaw(this, &FNOSClient::OnPostWorldInit);
	FWorldDelegates::OnPreWorldFinishDestroy.AddRaw(this, &FNOSClient::OnPreWorldFinishDestroy);

	bIsInitialized = true;
}

void FNOSClient::OnBeginFrame()
{

}

void FNOSClient::OnPostWorldInit(UWorld* World, const UWorld::InitializationValues initValues)
{
	TaskQueue.Enqueue([World, this]()
		{
			auto WorldContext = GEngine->GetWorldContextFromGameViewport(GEngine->GameViewport);
			if (World != WorldContext->World())
			{
				return;
			}
			if (!GEngine->GameViewport || !GEngine->GameViewport->IsStatEnabled("FPS"))
			{ 
				GEngine->Exec(World, TEXT("Stat FPS"));
			}

			IsWorldInitialized = true;

			nos::fb::TNodeStatusMessage MapNameStatus;
			MapNameStatus.text = TCHAR_TO_UTF8(*World->GetMapName());
			MapNameStatus.type = nos::fb::NodeStatusMessageType::INFO;
			UENodeStatusHandler.Add("map_name", MapNameStatus);
		});
}

void FNOSClient::OnPreWorldFinishDestroy(UWorld* World)
{
	LOG("World is destroyed");

	if (!GEngine)
	{
		return;
	}
	auto WorldContext = GEngine->GetWorldContextFromGameViewport(GEngine->GameViewport);
	if (WorldContext->World())
	{
		nos::fb::TNodeStatusMessage MapNameStatus;
		MapNameStatus.text = TCHAR_TO_UTF8(*WorldContext->World()->GetMapName());
		MapNameStatus.type = nos::fb::NodeStatusMessageType::INFO;
		UENodeStatusHandler.Add("map_name", MapNameStatus);
	}
	if (World != WorldContext->World())
	{
		return;
	}
	
}



void FNOSClient::StartupModule() {

	if (!FApp::HasProjectName())
	{
		return;
	}

	auto hwinfo = FHardwareInfo::GetHardwareInfo(NAME_RHI);
	if ("D3D12" != hwinfo)
	{
		FMessageDialog::Debugf(FText::FromString("Nodos plugin supports DirectX12 only!"), 0);
		return;
	}

	if (!FNodos::Initialize())
	{
		return;
	}

	Initialize();
}

void FNOSClient::ShutdownModule()
{
	// AppServiceClient-/*>*/
	NOSTimeStep = nullptr;
	if(FNodos::ShutdownClient)
	{
		FNodos::ShutdownClient(AppServiceClient);
	}
	AppServiceClient = nullptr;
	FNodos::Shutdown();
	bIsInitialized = false;
}

bool FNOSClient::Tick(float dt)
{
	if (ReloadingLevel > 0)
	{
		ReloadingLevel--;
		return true;
	}

    TryConnect();
	while (!TaskQueue.IsEmpty() && ReloadingLevel <= 0) {
		Task task;
		TaskQueue.Dequeue(task);
		task();
	}


	for (IAssetCompilingManager* CompilingManager : FAssetCompilingManager::Get().GetRegisteredManagers())
	{
		int32 RemainingCount = CompilingManager->GetNumRemainingAssets();
		auto AssetTypeName = std::string(TCHAR_TO_UTF8(*CompilingManager->GetAssetTypeName().ToString())) + std::string("_compilation_warning");

		if (RemainingCount)
		{
			FText AssetTypePlural = FText::Format(CompilingManager->GetAssetNameFormat(), FText::AsNumber(100));
			FString CompilationWarning = FText::Format(LOCTEXT("AssetCompilingFmt", "Preparing {0} ({1})"), AssetTypePlural, RemainingCount).ToString();
			nos::fb::TNodeStatusMessage CompilationStatus;
			CompilationStatus.text = TCHAR_TO_UTF8(*CompilationWarning);
			CompilationStatus.type = nos::fb::NodeStatusMessageType::WARNING;
			UENodeStatusHandler.Add(AssetTypeName, CompilationStatus);
		}
		else
		{
			UENodeStatusHandler.Remove(AssetTypeName);
		}
	}

	UENodeStatusHandler.Update();
	return true;
}

void FNOSClient::OnUpdatedNodeExecuted(nos::fb::vec2u deltaSeconds)
{
	if (NOSTimeStep.IsValid())
	{
		NOSTimeStep->Step(deltaSeconds);
	}
}

bool FNOSClient::ExecuteConsoleCommand(const TCHAR* Input)
{
	IConsoleManager::Get().AddConsoleHistoryEntry(TEXT(""), Input);

	int32 Len = FCString::Strlen(Input);
	TArray<TCHAR> Buffer;
	Buffer.AddZeroed(Len+1);

	bool bHandled = false;
	const TCHAR* ParseCursor = Input;
	while (FParse::Line(&ParseCursor, Buffer.GetData(), Buffer.Num()))
	{
		bHandled = ExecInternal(Buffer.GetData()) || bHandled;
	}

	// return true if we successfully executed any of the commands 
	return bHandled;
}

bool FNOSClient::ExecInternal(const TCHAR* Input)
{
	bool bWasHandled = false;
	UWorld* World = nullptr;
	UWorld* OldWorld = nullptr;
	NOSConsoleOutput ConsoleOutput(this);

	// The play world needs to handle these commands if it exists
	if (GIsEditor && GEditor->PlayWorld && !GIsPlayInEditorWorld)
	{
		World = GEditor->PlayWorld;
		OldWorld = SetPlayInEditorWorld(GEditor->PlayWorld);
	}

	ULocalPlayer* Player = GEngine->GetDebugLocalPlayer();
	if (Player)
	{
		UWorld* PlayerWorld = Player->GetWorld();
		if (!World)
		{
			World = PlayerWorld;
		}
		bWasHandled = Player->Exec(PlayerWorld, Input, ConsoleOutput);
	}

	if (!World)
	{
		World = GEditor->GetEditorWorldContext().World();
	}
	if (World)
	{
		if (!bWasHandled)
		{
			AGameModeBase* const GameMode = World->GetAuthGameMode();
			AGameStateBase* const GameState = World->GetGameState();
			if (GameMode && GameMode->ProcessConsoleExec(Input, ConsoleOutput, nullptr))
			{
				bWasHandled = true;
			}
			else if (GameState && GameState->ProcessConsoleExec(Input, ConsoleOutput, nullptr))
			{
				bWasHandled = true;
			}
		}

		if (!bWasHandled && !Player)
		{
			if (GIsEditor)
			{
				bWasHandled = GEditor->Exec(World, Input, ConsoleOutput);
			}
			else
			{
				bWasHandled = GEngine->Exec(World, Input, ConsoleOutput);
			}
		}
	}
	// Restore the old world of there was one
	if (OldWorld)
	{
		RestoreEditorWorld(OldWorld);
	}

	return bWasHandled;
	
}

void UENodeStatusHandler::SetClient(FNOSClient* _PluginClient)
{
	this->PluginClient = _PluginClient;
}

void UENodeStatusHandler::Add(std::string const& Id, nos::fb::TNodeStatusMessage const& Status)
{
	StatusMessages[Id] = Status;
	Dirty = true;
}

void UENodeStatusHandler::Remove(std::string const& Id)
{
	auto it = StatusMessages.find(Id);
	if (it != StatusMessages.end())
	{
		StatusMessages.erase(it);
		Dirty = true;
	}
}

void UENodeStatusHandler::Update()
{
	if (Dirty)
	{
		SendStatus();
	}
}

void UENodeStatusHandler::SendStatus()
{
	if (!PluginClient || !PluginClient->IsConnected() || !FNOSClient::NodeId.IsValid())
		return;
	flatbuffers::FlatBufferBuilder Builder;
	nos::app::TNodeStatusUpdate UpdateRequest;
	UpdateRequest.node_id = *reinterpret_cast<nos::fb::UUID*>(&FNOSClient::NodeId);
	for (auto& [_, StatusMsg] : StatusMessages)
	{
		UpdateRequest.status_messages.push_back(std::make_unique<nos::fb::TNodeStatusMessage>(StatusMsg));
	}
	auto offset = nos::CreateAppEventOffset(Builder, nos::app::CreateNodeStatusUpdate(Builder, &UpdateRequest));
	Builder.Finish(offset);
	auto buf = Builder.Release();
	auto root = flatbuffers::GetRoot<nos::app::AppEvent>(buf.data());
	PluginClient->AppServiceClient->Send(*root);

	Dirty = false;
}

bool FPSCounter::Update(float dt)
{
	++FrameCount;
	DeltaTimeAccum += dt;
	if (DeltaTimeAccum >= 1.f)
	{
		auto Prev = FramesPerSecond;
		FramesPerSecond = 1.f / (DeltaTimeAccum / float(FrameCount));
		FramesPerSecond = std::roundf(FramesPerSecond  * 100.f) / 100.f;
		DeltaTimeAccum = 0;
		FrameCount = 0;
		return Prev != FramesPerSecond;
	}
	return false;
}

nos::fb::TNodeStatusMessage FPSCounter::GetNodeStatusMessage() const
{
	nos::fb::TNodeStatusMessage FpsStatusMessage;

	FpsStatusMessage.text.resize(32);
	::snprintf(FpsStatusMessage.text.data(), 32, "%.2f FPS", FramesPerSecond);
	FpsStatusMessage.text.shrink_to_fit();

	FpsStatusMessage.type = nos::fb::NodeStatusMessageType::INFO;
	return FpsStatusMessage;
}

#pragma optimize("", on)
#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FNOSClient, ClientImpl)

