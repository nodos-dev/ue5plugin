#pragma once

struct NOSSpawnActorParameters
{
	bool SpawnActorToWorldCoords = false;
	FTransform SpawnTransform = FTransform::Identity;
	FName UniqueName = NAME_None;
	FName NodeDisplayName = NAME_None;
};
