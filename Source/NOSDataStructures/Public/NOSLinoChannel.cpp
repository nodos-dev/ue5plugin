/*
 * Copyright MediaZ Teknoloji A.S. All Rights Reserved.
 */

#pragma once
#include "NOSLinoChannel.h"
#include "UObject/ObjectMacros.h"
#include "Engine/TextureRenderTarget2D.h"

bool FNOSLinoChannel::UpdateRenderTarget(UWorld* world)
{
	if (!RenderTarget || RenderTarget->SizeX != Size.X || RenderTarget->SizeY != Size.Y)
	{
		RenderTarget = NewObject<UTextureRenderTarget2D>(world, UTextureRenderTarget2D::StaticClass(), NAME_None, RF_Transient);
		RenderTarget->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA16f;
		RenderTarget->ClearColor = FLinearColor::Black;
		RenderTarget->bAutoGenerateMips = false;
		RenderTarget->InitAutoFormat(Size.X, Size.Y);
		RenderTarget->UpdateResourceImmediate(true);
		return true;
	}
	return false;
}
