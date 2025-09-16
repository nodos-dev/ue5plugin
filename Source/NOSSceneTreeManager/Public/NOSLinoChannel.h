/*
 * Copyright MediaZ Teknoloji A.S. All Rights Reserved.
 */

#pragma once
//#if WITH_EDITORONLY_DATA
//#include "NOSActorProperties.h"
#include "NOSLinoChannel.generated.h"

USTRUCT(Blueprintable)
struct NOSSCENETREEMANAGER_API FNOSLinoChannel
{
	GENERATED_BODY()
public:
	/** Please add a variable description */
	UPROPERTY(BlueprintReadWrite, meta = (DisplayName = "Name", Category = ""))
	FName Name = NAME_None;

	/** Please add a variable description */
	UPROPERTY(BlueprintReadWrite, meta = (DisplayName = "Size", Category = ""))
	FIntPoint Size;

	bool ValidateUpdateSize();
};
