#include "NOSSettings.h"
#include "Interfaces/IPluginManager.h"
#include "NOSClient.h"

UNOSSettings::UNOSSettings(const FObjectInitializer& ObjectInitializer)
	// Where the installer puts Nodos, next to the engine folder.
	: NosmanPath(TEXT("../Nodos/nodos.exe"))
{
}

#if WITH_EDITOR
void UNOSSettings::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
	auto NOSClient = &FModuleManager::LoadModuleChecked<FNOSClient>("NOSClient");
	if(!NOSClient->bIsInitialized)
	{
		if (GNodos.Initialize())
		{
			NOSClient->Initialize();
		}
	}
	else
	{
		NOSClient->ShutdownModule();
		if (GNodos.Initialize())
		{
			NOSClient->Initialize();
		}
	}
}
#endif // WITH_EDITOR

