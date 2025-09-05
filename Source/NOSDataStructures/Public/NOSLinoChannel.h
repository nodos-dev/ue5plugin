/*
 * Copyright MediaZ Teknoloji A.S. All Rights Reserved.
 */

#pragma once
//#if WITH_EDITORONLY_DATA
#include "NOSLinoChannel.generated.h"

USTRUCT(Blueprintable)
struct NOSDATASTRUCTURES_API FNOSLinoChannel
{
	GENERATED_BODY()
public:
	/** Please add a variable description */
	UPROPERTY(BlueprintReadWrite, meta = (DisplayName = "Name", Category = ""))
	FName Name = NAME_None;

	/** Please add a variable description */
	UPROPERTY(BlueprintReadWrite, meta = (DisplayName = "Size", Category = ""))
	FIntPoint Size = FIntPoint(1920, 1080);
};
