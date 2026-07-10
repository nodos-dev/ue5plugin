// Copyright MediaZ Teknoloji A.S. All Rights Reserved.


#include "NOSEditorTickableActor.h"
#include "Engine/World.h"

// Sets default values
ANOSEditorTickableActor::ANOSEditorTickableActor()
{
 	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;

}

// Called when the game starts or when spawned
void ANOSEditorTickableActor::BeginPlay()
{
	Super::BeginPlay();
	
}

void ANOSEditorTickableActor::Tick(float DeltaTime)
{
#if WITH_EDITOR
	if (GetWorld() != nullptr && GetWorld()->WorldType == EWorldType::Editor)
	{
		BlueprintEditorTick(DeltaTime);
	}
	else
#endif
	{
		Super::Tick(DeltaTime);
	}
}


bool ANOSEditorTickableActor::ShouldTickIfViewportsOnly() const
{
	if (GetWorld() != nullptr && GetWorld()->WorldType == EWorldType::Editor && UseEditorTick)
	{
		return true;
	}
	else
	{
		return false;
	}
}
