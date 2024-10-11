// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include "NOSActorProperties.h"
#include "NOSTextureShareManager.h"
#include "EditorCategoryUtils.h"
#include "ObjectEditorUtils.h"
#include "NOSTrack.h"
#include "EngineUtils.h"
#include "Blueprint/UserWidget.h"
#include "NOSSceneTreeManager.h"
#include "PropertyEditorModule.h"

#define CHECK_PROP_SIZE() {if (size != Property->ElementSize){UE_LOG(LogNOSSceneTreeManager, Error, TEXT("Property size mismatch with Nodos"));return;}}

bool PropertyVisibleExp(FProperty* ueproperty)
{
	return !ueproperty->HasAllPropertyFlags(CPF_DisableEditOnInstance) &&
		!ueproperty->HasAllPropertyFlags(CPF_Deprecated) &&
		//!ueproperty->HasAllPropertyFlags(CPF_EditorOnly) && //? dont know what this flag does but it hides more than necessary
		ueproperty->HasAllPropertyFlags(CPF_Edit) &&
		//ueproperty->HasAllPropertyFlags(CPF_BlueprintVisible) && //? dont know what this flag does but it hides more than necessary
		ueproperty->HasAllFlags(RF_Public);
}

NOSProperty::NOSProperty(UObject* container, FProperty* uproperty, FString parentCategory, uint8* structPtr, NOSStructProperty* parentProperty)
{
	Id = FGuid::NewGuid();

	if (!container && !uproperty)
		return;

	Property = uproperty;
	if (Property->HasAnyPropertyFlags(CPF_OutParm))
	{
		ReadOnly = true;
	}

	StructPtr = structPtr;
	if (container && container->IsA<UActorComponent>())
	{
		ComponentContainer = NOSComponentReference((UActorComponent*)container);
	}
	else if (container && container->IsA<AActor>())
	{
		ActorContainer = NOSActorReference((AActor*)container);
	}
	else
	{
		ObjectPtr = container;
	}

	
	PropertyName = uproperty->GetFName().ToString();
	if (container && container->IsA<UActorComponent>())
	{
		PropertyName = *FString(container->GetFName().ToString() + "" + PropertyName);
	}

	auto metaDataMap = uproperty->GetMetaDataMap();
	if (metaDataMap)
	{
		static const FName NAME_DisplayName(TEXT("DisplayName"));
		static const FName NAME_Category(TEXT("Category"));
		static const FName NAME_UIMin(TEXT("UIMin"));
		static const FName NAME_UIMax(TEXT("UIMax"));
		static const FName NAME_EditCondition(TEXT("editcondition"));
		static const FName NAME_HiddenByDefault(TEXT("PinHiddenByDefault"));
		static const FName NAME_ToolTip(TEXT("ToolTip"));
		static const FName NAME_NOSCanShowAsOutput(TEXT("NOSCanShowAsOutput"));
		static const FName NAME_NOSCanShowAsInput(TEXT("NOSCanShowAsInput"));

		const auto& metaData = *metaDataMap;
		DisplayName = metaData.Contains(NAME_DisplayName) ? metaData[NAME_DisplayName] : uproperty->GetFName().ToString();
		CategoryName = (metaData.Contains(NAME_Category) ? metaData[NAME_Category] : "Default");
		UIMinString = metaData.Contains(NAME_UIMin) ? metaData[NAME_UIMin] : "";
		UIMaxString = metaData.Contains(NAME_UIMax) ? metaData[NAME_UIMax] : "";
		ToolTipText = metaData.Contains(NAME_ToolTip) ? metaData[NAME_ToolTip] : "";
		EditConditionPropertyName = metaData.Contains(NAME_EditCondition) ? metaData[NAME_EditCondition] : "";
		if(!EditConditionPropertyName.IsEmpty())
		{
			auto OwnerVariant = Property->GetOwnerVariant();
			auto OwnerStruct = Property->GetOwnerStruct();
			if(!OwnerStruct && OwnerVariant)
			{
				OwnerStruct = OwnerVariant.IsUObject() ? (UStruct*)OwnerVariant.ToUObject() : nullptr;
			}
			EditConditionProperty = FindFProperty<FProperty>(OwnerStruct, FName(EditConditionPropertyName));
		}
		if(metaData.Contains(NAME_HiddenByDefault))
		{
			nosMetaDataMap.Add(NosMetadataKeys::PinHidden, " ");
		}

		if(!metaData.Contains(NAME_NOSCanShowAsInput) && metaData.Contains(NAME_NOSCanShowAsOutput))
		{
			PinCanShowAs = nos::fb::CanShowAs::OUTPUT_PIN_OR_PROPERTY;
		}
		
		if(metaData.Contains(NAME_NOSCanShowAsInput) && !metaData.Contains(NAME_NOSCanShowAsOutput))
		{
			PinCanShowAs = nos::fb::CanShowAs::INPUT_PIN_OR_PROPERTY;
		}
		
	}
	else
	{
		DisplayName = uproperty->GetFName().ToString();
		CategoryName = "Default";
		UIMinString = "";
		UIMaxString = "";
	}
	IsAdvanced = uproperty->HasAllPropertyFlags(CPF_AdvancedDisplay);
	
	// For properties inside a struct, add them to their own category unless they just take the name of the parent struct.  
	// In that case push them to the parent category
	FName PropertyCategoryName = FObjectEditorUtils::GetCategoryFName(Property);
	if (parentProperty && (PropertyCategoryName == parentProperty->structprop->Struct->GetFName()))
	{
		CategoryName = parentCategory;
	}
	else
	{
		if (!parentCategory.IsEmpty())
		{
			if (CategoryName.IsEmpty())
			{
				CategoryName = parentCategory;
			}
			else
			{
				CategoryName = (parentCategory + "|" + CategoryName);
			}
		}
	}

	if (parentProperty)
	{
		DisplayName = parentProperty->DisplayName + "_" + DisplayName;
	}

	if(container)
	{
		static const FName PropertyEditor("PropertyEditor");
		FPropertyEditorModule& PropertyModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>(PropertyEditor);
		TArray<TSharedPtr<FPropertySection>> Sections;
		Sections = PropertyModule.FindSectionsForCategory(container->GetClass(), PropertyCategoryName);
		for(auto section : Sections)
		{
			FString tag = section->GetDisplayName().ToString();
			auto& tagMetadataEntry = nosMetaDataMap.FindOrAdd("Tags");
			tagMetadataEntry += tag + ",";
			// UE_LOG(LogTemp, Warning, TEXT("The property %s is in section %s"), *DisplayName, *section->GetDisplayName().ToString());
		}
		if(!Sections.IsEmpty())
		{
			nosMetaDataMap.FindOrAdd("Tags").LeftChopInline(1);
		}
	}

}



std::vector<uint8> NOSProperty::UpdatePinValue(uint8* customContainer)
{
	void* container = nullptr;
	if (customContainer) container = customContainer;
	else if (ComponentContainer) container = ComponentContainer.Get();
	else if (ActorContainer) container = ActorContainer.Get();
	else if (ObjectPtr && IsValid(ObjectPtr)) container = ObjectPtr;
	else if (StructPtr) container = StructPtr;

	if (container)
	{
		void* val = Property->ContainerPtrToValuePtr<void>(container);
		memcpy(data.data(), val, data.size());
	}
	return data;
}

void NOSProperty::MarkState()
{
	if (BoundComponent)
	{
		BoundComponent->MarkRenderStateDirty();
		BoundComponent->UpdateComponentToWorld();
	}

}

void NOSProperty::SetPropValue(void* val, size_t size, uint8* customContainer)
{
	SetPropValue_Internal(val, size, customContainer);
	CallOnChangedFunction();
}

void NOSProperty::SetPropValue_Internal(void* val, size_t size, uint8* customContainer)
{
	IsChanged = true;
	CHECK_PROP_SIZE();

	void* container = nullptr;
	if (customContainer) container = customContainer;
	else if (ComponentContainer) container = ComponentContainer.Get();
	else if (ActorContainer) container = ActorContainer.Get();
	else if (ObjectPtr && IsValid(ObjectPtr)) container = ObjectPtr;
	else if (StructPtr) container = StructPtr;

	if (!container)
	{
		UE_LOG(LogTemp, Warning, TEXT("The property %s has null container!"), *(DisplayName));
		return; //TODO investigate why container is null
	}
	
	SetProperty_InCont(container, val);
	
	MarkState();
}

void* NOSProperty::GetRawContainer()
{
	if(auto Object = GetRawObjectContainer())
	{
		return Object;
	}
	return StructPtr;
}

UObject* NOSProperty::GetRawObjectContainer()
{
	if (ActorContainer)
	{
		return ActorContainer.Get();
	}
	else if (ComponentContainer)
	{
		return ComponentContainer.Get();
	}
	else if (ObjectPtr && IsValid(ObjectPtr))
	{
		return ObjectPtr;
	}

	return nullptr;
}

void NOSProperty::SetProperty_InCont(void* container, void* val)
{
	return;
}

void NOSProperty::CallOnChangedFunction()
{
	if (!Property)
		return;

	UClass* OwnerClass = Property->GetOwnerClass();
	if (!OwnerClass)
		return;

	UObject* objectPtr = nullptr;
	if (ComponentContainer) objectPtr = ComponentContainer.Get();
	else if (ActorContainer) objectPtr = ActorContainer.Get();
	else objectPtr = ObjectPtr;

	if (objectPtr)
	{
		const FString OnChangedFunctionName = TEXT("OnChanged_") + Property->GetName();
		UFunction* OnChanged = OwnerClass->FindFunctionByName(*OnChangedFunctionName);
		if (OnChanged)
		{
			objectPtr->Modify();
			objectPtr->ProcessEvent(OnChanged, nullptr);
		}
	}
}

void NOSTrackProperty::SetPropValue_Internal(void* val, size_t size, uint8* customContainer)
{
	IsChanged = true;

	void* container = nullptr;
	if (customContainer) container = customContainer;
	else if (ComponentContainer) container = ComponentContainer.Get();
	else if (ActorContainer) container = ActorContainer.Get();
	else if (ObjectPtr && IsValid(ObjectPtr)) container = ObjectPtr;
	else if (StructPtr) container = StructPtr;
	if (!container)
	{
		UE_LOG(LogTemp, Warning, TEXT("The property %s has null container!"), *(DisplayName));
		return; //TODO investigate why container is null
	}
	
	SetProperty_InCont(container, val);

	MarkState();
}

bool NOSTrackProperty::CreateFbArray(flatbuffers::FlatBufferBuilder& fb, FScriptArrayHelper_InContainer& ArrayHelper)
{
	std::vector<flatbuffers::Offset<nos::fb::Track>> TrackArray;
	for(int i = 0; i < ArrayHelper.Num(); i++)
	{
		if(auto ElementPtr = ArrayHelper.GetRawPtr(i))
		{
			FNOSTrack TrackData = *(FNOSTrack*)ElementPtr;
			
			nos::fb::TTrack TempTrack = {};
			TempTrack.location = nos::fb::vec3(TrackData.location.X, TrackData.location.Y, TrackData.location.Z);
			TempTrack.rotation = nos::fb::vec3(TrackData.rotation.X, TrackData.rotation.Y, TrackData.rotation.Z);
			TempTrack.fov = TrackData.fov;
			TempTrack.focus = TrackData.focus_distance;
			TempTrack.zoom = TrackData.zoom;
			TempTrack.render_ratio = TrackData.render_ratio;
			TempTrack.sensor_size = nos::fb::vec2(TrackData.sensor_size.X, TrackData.sensor_size.Y);
			TempTrack.pixel_aspect_ratio = TrackData.pixel_aspect_ratio;
			TempTrack.nodal_offset = TrackData.nodal_offset;
			auto& Distortion = TempTrack.lens_distortion;
			Distortion.mutable_k1k2() = nos::fb::vec2(TrackData.k1, TrackData.k2);
			Distortion.mutable_center_shift() = nos::fb::vec2(TrackData.center_shift.X, TrackData.center_shift.Y);
			Distortion.mutate_distortion_scale(TrackData.distortion_scale);
			auto offset = nos::fb::CreateTrack(fb, &TempTrack);
			TrackArray.push_back(offset);
		}
	}
	auto offset = fb.CreateVector(TrackArray).o;
	//auto offset = fb.CreateVectorOfSortedTables(&TrackArray).o;
	fb.Finish(flatbuffers::Offset<flatbuffers::Vector<nos::fb::Track>>(offset));
	
	return true;
}

void NOSTrackProperty::SetArrayPropValues(void* val, size_t size, FScriptArrayHelper_InContainer& ArrayHelper)
{
	auto vec = (flatbuffers::Vector<flatbuffers::Offset<nos::fb::Track>>*)val; 
	int ct = vec->size();
	ArrayHelper.Resize(ct);
	for(int i = 0; i < ct; i++)
	{
		ArrayHelper.ExpandForIndex(i);
		auto track = vec->Get(i);
		// auto track = flatbuffers::GetRoot<nos::fb::Track>(val);
		FNOSTrack* TrackData = (FNOSTrack*)ArrayHelper.GetRawPtr(i);
		*TrackData = {};
		nos::fb::TTrack TTrack = {};
		track->UnPackTo(&TTrack);

		TrackData->location = FVector(TTrack.location.x(), TTrack.location.y(), TTrack.location.z());
		TrackData->rotation = FVector(TTrack.rotation.x(), TTrack.rotation.y(), TTrack.rotation.z());
		TrackData->fov = TTrack.fov;
		TrackData->focus_distance = TTrack.focus_distance;
		TrackData->zoom = TTrack.zoom;
		TrackData->render_ratio = TTrack.render_ratio;
		TrackData->sensor_size = FVector2D(TTrack.sensor_size.x(), TTrack.sensor_size.y());
		TrackData->pixel_aspect_ratio = TTrack.pixel_aspect_ratio;
		TrackData->nodal_offset = TTrack.nodal_offset;
		auto distortion = TTrack.lens_distortion;
		TrackData->distortion_scale = distortion.distortion_scale();
		auto& k1k2 = distortion.k1k2();
		TrackData->k1 = k1k2.x();
		TrackData->k2 = k1k2.y();
		TrackData->center_shift = FVector2D(distortion.center_shift().x(), distortion.center_shift().y());
	}
}

void NOSTrackProperty::SetProperty_InCont(void* container, void* val)
{
	auto track = flatbuffers::GetRoot<nos::fb::Track>(val);
	FNOSTrack* TrackData = structprop->ContainerPtrToValuePtr<FNOSTrack>(container);
	// TrackData.location = FVector(0);
	// TrackData.rotation = FVector(0);
	// TrackData.center_shift = FVector2d(0);
	// TrackData.sensor_size = FVector2d(0);

	if (flatbuffers::IsFieldPresent(track, nos::fb::Track::VT_LOCATION))
	{
		TrackData->location = FVector(track->location()->x(), track->location()->y(), track->location()->z());
	}
	if (flatbuffers::IsFieldPresent(track, nos::fb::Track::VT_ROTATION))
	{
		TrackData->rotation = FVector(track->rotation()->x(), track->rotation()->y(), track->rotation()->z());
	}
	if (flatbuffers::IsFieldPresent(track, nos::fb::Track::VT_FOV))
	{
		TrackData->fov = track->fov();
	}
	if (flatbuffers::IsFieldPresent(track, nos::fb::Track::VT_FOCUS))
	{
		TrackData->focus_distance = track->focus_distance();
	}
	if (flatbuffers::IsFieldPresent(track, nos::fb::Track::VT_ZOOM))
	{
		TrackData->zoom = track->zoom();
	}
	if (flatbuffers::IsFieldPresent(track, nos::fb::Track::VT_RENDER_RATIO))
	{
		TrackData->render_ratio = track->render_ratio();
	}
	if (flatbuffers::IsFieldPresent(track, nos::fb::Track::VT_SENSOR_SIZE))
	{
		TrackData->sensor_size = FVector2D(track->sensor_size()->x(), track->sensor_size()->y());
	}
	if (flatbuffers::IsFieldPresent(track, nos::fb::Track::VT_PIXEL_ASPECT_RATIO))
	{
		TrackData->pixel_aspect_ratio = track->pixel_aspect_ratio();
	}
	if (flatbuffers::IsFieldPresent(track, nos::fb::Track::VT_NODAL_OFFSET))
	{
		TrackData->nodal_offset = track->nodal_offset();
	}
	if (flatbuffers::IsFieldPresent(track, nos::fb::Track::VT_LENS_DISTORTION))
	{
		auto distortion = track->lens_distortion();
		TrackData->distortion_scale = distortion->distortion_scale();
		auto& k1k2 = distortion->k1k2();
		TrackData->k1 = k1k2.x();
		TrackData->k2 = k1k2.y();
		TrackData->center_shift = FVector2D(distortion->center_shift().x(), distortion->center_shift().y());
	}
	//structprop->CopyCompleteValue(structprop->ContainerPtrToValuePtr<void>(container), &TrackData); 
	//FRealityTrack newTrack = *(FRealityTrack*)val;

	//if (ActorContainer.Get())
	//{
	//	//ActorContainer->SetActorRelativeLocation(newTrack.location);
	//	//actor->SetActorRelativeRotation(newTrack.rotation.Rotation());
	//	//actor->SetActorRelativeRotation(newTrack.rotation.Rotation());
	//}
}

std::vector<uint8> NOSCustomTransformProperty::UpdatePinValue(uint8* customContainer)
{
	if (!ActorRef)
		return data;

	auto rootComponent = ActorRef->GetRootComponent();

	if (!rootComponent)
		return data;

	FTransform TransformData = rootComponent->GetRelativeTransform();
	flatbuffers::FlatBufferBuilder fb;
	nos::fb::Transform TempTransform;
	TempTransform.mutable_position() = nos::fb::vec3d(TransformData.GetLocation().X, TransformData.GetLocation().Y, TransformData.GetLocation().Z);
	TempTransform.mutable_scale() = nos::fb::vec3d(TransformData.GetScale3D().X, TransformData.GetScale3D().Y, TransformData.GetScale3D().Z);
	TempTransform.mutable_rotation() = nos::fb::vec3d(TransformData.GetRotation().Rotator().Roll, TransformData.GetRotation().Rotator().Pitch, TransformData.GetRotation().Rotator().Yaw);
		
	nos::Buffer buffer = nos::Buffer::From(TempTransform);
	data = buffer;

	return data;
}

void NOSCustomTransformProperty::SetPropValue_Internal(void* val, size_t size, uint8* customContainer)
{
	IsChanged = true;

	if (ActorRef)
	{
		SetProperty_InCont(nullptr, val);
		BoundComponent = NOSComponentReference(ActorRef->GetRootComponent());
	}

	MarkState();
}

void NOSCustomTransformProperty::SetProperty_InCont(void* container, void* val)
{
	auto transform = *(nos::fb::Transform*)val;

	if (ActorRef->GetRootComponent() && ActorRef->GetRootComponent()->Mobility != EComponentMobility::Movable)
	{
		ActorRef->GetRootComponent()->SetMobility(EComponentMobility::Movable);
	}

	ActorRef->SetActorRelativeLocation(FVector(transform.position().x(), transform.position().y(), transform.position().z()));
	ActorRef->SetActorRelativeScale3D(FVector(transform.scale().x(), transform.scale().y(), transform.scale().z()));
	
	FRotator rot(transform.rotation().y(), transform.rotation().z(), transform.rotation().x());
	ActorRef->SetActorRelativeRotation(rot.Quaternion());
}


std::vector<uint8> NOSTransformProperty::UpdatePinValue(uint8* customContainer)
{
	void* container = nullptr;
	if (customContainer) container = customContainer;
	else if (ComponentContainer) container = ComponentContainer.Get();
	else if (ActorContainer) container = ActorContainer.Get();
	else if (ObjectPtr && IsValid(ObjectPtr)) container = ObjectPtr;
	else if (StructPtr) container = StructPtr;

	if (container)
	{
		FTransform TransformData = *Property->ContainerPtrToValuePtr<FTransform>(container);
		flatbuffers::FlatBufferBuilder fb;
		nos::fb::Transform TempTransform;
		TempTransform.mutable_position() = nos::fb::vec3d(TransformData.GetLocation().X, TransformData.GetLocation().Y, TransformData.GetLocation().Z);
		TempTransform.mutable_scale() = nos::fb::vec3d(TransformData.GetScale3D().X, TransformData.GetScale3D().Y, TransformData.GetScale3D().Z);
		TempTransform.mutable_rotation() = nos::fb::vec3d(TransformData.GetRotation().ToRotationVector().X, TransformData.GetRotation().ToRotationVector().Y, TransformData.GetRotation().ToRotationVector().Z);
		
		nos::Buffer buffer = nos::Buffer::From(TempTransform);
		data = buffer;
	}
	return data;

}

void NOSTransformProperty::SetPropValue_Internal(void* val, size_t size, uint8* customContainer)
{
	IsChanged = true;

	void* container = nullptr;
	if (customContainer) container = customContainer;
	else if (ComponentContainer) container = ComponentContainer.Get();
	else if (ActorContainer) container = ActorContainer.Get();
	else if (ObjectPtr && IsValid(ObjectPtr)) container = ObjectPtr;
	else if (StructPtr) container = StructPtr;
	if (!container)
	{
		UE_LOG(LogTemp, Warning, TEXT("The property %s has null container!"), *(DisplayName));
		return; //TODO investigate why container is null
	}
	
	SetProperty_InCont(container, val);

	MarkState();
}

void NOSTransformProperty::SetProperty_InCont(void* container, void* val)
{
	auto transform = *(nos::fb::Transform*)val;
	FTransform* TransformData = structprop->ContainerPtrToValuePtr<FTransform>(container);

	TransformData->SetLocation(FVector(transform.position().x(), transform.position().y(), transform.position().z()));
	TransformData->SetScale3D(FVector(transform.scale().x(), transform.scale().y(), transform.scale().z()));
	
	FRotator rot(transform.rotation().y(), transform.rotation().z(), transform.rotation().x());
	TransformData->SetRotation(rot.Quaternion());
}


std::vector<uint8> NOSTrackProperty::UpdatePinValue(uint8* customContainer)
{
	void* container = nullptr;
	if (customContainer) container = customContainer;
	else if (ComponentContainer) container = ComponentContainer.Get();
	else if (ActorContainer) container = ActorContainer.Get();
	else if (ObjectPtr && IsValid(ObjectPtr)) container = ObjectPtr;
	else if (StructPtr) container = StructPtr;

	if (container)
	{
		FNOSTrack TrackData = *Property->ContainerPtrToValuePtr<FNOSTrack>(container);
		
		flatbuffers::FlatBufferBuilder fb;
		nos::fb::TTrack TempTrack;
		TempTrack.location = nos::fb::vec3(TrackData.location.X, TrackData.location.Y, TrackData.location.Z);
		TempTrack.rotation = nos::fb::vec3(TrackData.rotation.X, TrackData.rotation.Y, TrackData.rotation.Z);
		TempTrack.fov = TrackData.fov;
		TempTrack.focus = TrackData.focus_distance;
		TempTrack.zoom = TrackData.zoom;
		TempTrack.render_ratio = TrackData.render_ratio;
		TempTrack.sensor_size = nos::fb::vec2(TrackData.sensor_size.X, TrackData.sensor_size.Y);
		TempTrack.pixel_aspect_ratio = TrackData.pixel_aspect_ratio;
		TempTrack.nodal_offset = TrackData.nodal_offset;
		auto& Distortion = TempTrack.lens_distortion;
		Distortion.mutable_k1k2() = nos::fb::vec2(TrackData.k1, TrackData.k2);
		Distortion.mutable_center_shift() = nos::fb::vec2(TrackData.center_shift.X, TrackData.center_shift.Y);
		Distortion.mutate_distortion_scale(TrackData.distortion_scale);
		
		auto offset = nos::fb::CreateTrack(fb, &TempTrack);
		fb.Finish(offset);
		nos::Buffer buffer = fb.Release();
		data = buffer;
	}
	return data;
}


void NOSRotatorProperty::SetProperty_InCont(void* container, void* val)
{
	double x = ((double*)val)[0];
	double y = ((double*)val)[1];
	double z = ((double*)val)[2];
	FRotator rotator = FRotator(y,z,x);
	structprop->CopyCompleteValue(structprop->ContainerPtrToValuePtr<void>(container), &rotator);
}

std::vector<uint8> NOSColorProperty::UpdatePinValue(uint8* customContainer)
{
	void* container = nullptr;
	if (customContainer) container = customContainer;
	else if (ComponentContainer) container = ComponentContainer.Get();
	else if (ActorContainer) container = ActorContainer.Get();
	else if (ObjectPtr && IsValid(ObjectPtr)) container = ObjectPtr;
	else if (StructPtr) container = StructPtr;

	if (container)
	{
		FColor* val = (FColor*)Property->ContainerPtrToValuePtr<void>(container);
		nos::fb::vec4u8 cl = {val->R, val->G, val->B, val->A};
		memcpy(data.data(), &cl, data.size());
	}
	return data;

}

bool NOSColorProperty::CreateFbArray(flatbuffers::FlatBufferBuilder& fb, FScriptArrayHelper_InContainer& ArrayHelper)
{
	fb.StartVector(ArrayHelper.Num(), sizeof(FColor), 1);
	for(int i = ArrayHelper.Num()-1; i >= 0; i--)
	{
		if(auto ElementPtr = ArrayHelper.GetRawPtr(i))
		{
			FColor* val = (FColor*)ElementPtr;
			nos::fb::vec4u8 cl = {val->R, val->G, val->B, val->A};
			fb.PushBytes((uint8_t*)&cl, sizeof(FColor));
		}
	}
	auto offset = fb.EndVector(ArrayHelper.Num());
	fb.Finish(flatbuffers::Offset<flatbuffers::Vector<uint8_t>>(offset));
	return true;
}

void NOSColorProperty::SetArrayPropValues(void* val, size_t size, FScriptArrayHelper_InContainer& ArrayHelper)
{
	auto vec = (flatbuffers::Vector<u8>*)val; 
	int ct = vec->size();
	ArrayHelper.Resize(ct);
	for(int i = 0; i < ct; i++)
	{
		ArrayHelper.ExpandForIndex(i);
		uint8_t* el = vec->data() + (i * Property->ElementSize);
		uint8_t r = ((uint8_t*)el)[0];
		uint8_t g = ((uint8_t*)el)[1];
		uint8_t b = ((uint8_t*)el)[2];
		uint8_t a = ((uint8_t*)el)[3];
		FColor col;
		col.R = r;
		col.G = g;
		col.B = b;
		col.A = a;
		structprop->CopyCompleteValue(ArrayHelper.GetRawPtr(i), &col);
	}
}

void NOSColorProperty::SetProperty_InCont(void* container, void* val)
{
	uint8_t r = ((uint8_t*)val)[0];
	uint8_t g = ((uint8_t*)val)[1];
	uint8_t b = ((uint8_t*)val)[2];
	uint8_t a = ((uint8_t*)val)[3];
	FColor col;
	col.R = r;
	col.G = g;
	col.B = b;
	col.A = a;
	structprop->CopyCompleteValue(structprop->ContainerPtrToValuePtr<void>(container), &col);
}

std::vector<uint8> NOSRotatorProperty::UpdatePinValue(uint8* customContainer)
{
	void* container = nullptr;
	if (customContainer) container = customContainer;
	else if (ComponentContainer) container = ComponentContainer.Get();
	else if (ActorContainer) container = ActorContainer.Get();
	else if (ObjectPtr && IsValid(ObjectPtr)) container = ObjectPtr;
	else if (StructPtr) container = StructPtr;

	if (container)
	{
		void* val = Property->ContainerPtrToValuePtr<void>(container);
		double x = ((double*)val)[0];
		double y = ((double*)val)[1];
		double z = ((double*)val)[2];
		FRotator rotator = FRotator(z, x, y);


		memcpy(data.data(), &rotator, data.size());
	}
	return data;
}

FString ValidateName(FString& name)
{
	//add escape char before invalid characters
	name = name.Replace(TEXT("\\"), TEXT("\\\\"));
	name = name.Replace(TEXT("\""), TEXT("\\\""));
	name = name.Replace(TEXT("\n"), TEXT("\\n"));
	name = name.Replace(TEXT("\r"), TEXT("\\r"));
	name = name.Replace(TEXT("\t"), TEXT("\\t"));
	name = name.Replace(TEXT("#"), TEXT("\\#"));
	name = name.Replace(TEXT("."), TEXT("\\."));
	name = name.Replace(TEXT("/"), TEXT("\\/"));
	return name;
}

flatbuffers::Offset<nos::fb::Pin> NOSProperty::Serialize(flatbuffers::FlatBufferBuilder& fbb)
{

	std::vector<flatbuffers::Offset<nos::fb::MetaDataEntry>> metadata = SerializeMetaData(fbb);
	auto displayName = Property ? Property->GetDisplayNameText().ToString() : DisplayName;
	DisplayName = ValidateName(DisplayName);							    
	if (TypeName == "nos.fb.Void" || TypeName.size() < 1)
	{
		return nos::fb::CreatePinDirect(fbb, (nos::fb::UUID*)&Id, TCHAR_TO_UTF8(*DisplayName), "nos.fb.Void", nos::fb::ShowAs::NONE, PinCanShowAs, TCHAR_TO_UTF8(*CategoryName), 0, &data, 0, 0, 0, &default_val, 0, ReadOnly, IsAdvanced, transient, &metadata, 0, nos::fb::PinContents::JobPin, 0, nos::fb::CreateOrphanStateDirect(fbb, true, TCHAR_TO_UTF8(TEXT("Unknown type!"))), nos::fb::PinValueDisconnectBehavior::KEEP_LAST_VALUE, TCHAR_TO_UTF8(*ToolTipText), TCHAR_TO_UTF8(*displayName));
	}
	return nos::fb::CreatePinDirect(fbb, (nos::fb::UUID*)&Id, TCHAR_TO_UTF8(*DisplayName), TypeName.c_str(),  PinShowAs, PinCanShowAs, TCHAR_TO_UTF8(*CategoryName), 0, &data, 0, &min_val, &max_val, &default_val, 0, ReadOnly, IsAdvanced, transient, &metadata, 0, nos::fb::PinContents::JobPin, 0, nos::fb::CreateOrphanStateDirect(fbb, IsOrphan, TCHAR_TO_UTF8(*OrphanMessage)), nos::fb::PinValueDisconnectBehavior::KEEP_LAST_VALUE, TCHAR_TO_UTF8(*ToolTipText), TCHAR_TO_UTF8(*displayName));
}

std::vector<flatbuffers::Offset<nos::fb::MetaDataEntry>> NOSProperty::SerializeMetaData(flatbuffers::FlatBufferBuilder& fbb)
{
	std::vector<flatbuffers::Offset<nos::fb::MetaDataEntry>> metadata;
	for (auto [key, value] : nosMetaDataMap)
	{
		metadata.push_back(nos::fb::CreateMetaDataEntryDirect(fbb, TCHAR_TO_UTF8(*key), TCHAR_TO_UTF8(*value)));
	}
	return metadata;
}

NOSStructProperty::NOSStructProperty(UObject* container, FStructProperty* uproperty, FString parentCategory, uint8* StructPtr, NOSStructProperty* parentProperty)
	: NOSProperty(container, uproperty, parentCategory, StructPtr, parentProperty), structprop(uproperty)
{
	uint8* StructInst = nullptr;
	UClass* Class = nullptr;
	if (UObject* Container = GetRawObjectContainer())
	{
		StructInst = structprop->ContainerPtrToValuePtr<uint8>(Container);
		Class = Container->GetClass();
	}
	else if (StructPtr)
	{
		StructInst = structprop->ContainerPtrToValuePtr<uint8>(StructPtr);
		Class = structprop->Struct->GetClass();
	}
	class FProperty* AProperty = structprop->Struct->PropertyLink;
	while (AProperty != nullptr)
	{
		FName CategoryNamek = FObjectEditorUtils::GetCategoryFName(AProperty);

		if (Class && FEditorCategoryUtils::IsCategoryHiddenFromClass(Class, CategoryNamek.ToString()) || !PropertyVisibleExp(AProperty))
		{
			AProperty = AProperty->PropertyLinkNext;
			continue;
		}

		//UE_LOG(LogTemp, Warning, TEXT("The property name in struct: %s"), *(AProperty->GetAuthoredName()));
		auto nosprop = NOSPropertyFactory::CreateProperty(nullptr, AProperty, CategoryName + "|" + DisplayName, StructInst, this);
		if (nosprop)
		{
			if(nosprop->nosMetaDataMap.Contains(NosMetadataKeys::ContainerPath))
			{
				auto ContainerPath = nosprop->nosMetaDataMap.Find(NosMetadataKeys::ContainerPath);
				ContainerPath->InsertAt(0, structprop->GetNameCPP() + FString("/") );
			}
			else
			{
				nosprop->nosMetaDataMap.Add(NosMetadataKeys::ContainerPath, structprop->GetNameCPP());
			}
			
			nosprop->nosMetaDataMap.Remove(NosMetadataKeys::component);
			nosprop->nosMetaDataMap.Remove(NosMetadataKeys::actorId);
			FString ActorUniqueName;
			if (auto component = Cast<USceneComponent>(container))
			{
				nosprop->nosMetaDataMap.Add(NosMetadataKeys::component, component->GetFName().ToString());
				if (auto actor = component->GetOwner())
				{
					ActorUniqueName = actor->GetFName().ToString();
					nosprop->nosMetaDataMap.Add(NosMetadataKeys::actorId, actor->GetActorGuid().ToString());
				}
			}
			else if (auto actor = Cast<AActor>(container))
			{
				ActorUniqueName = actor->GetFName().ToString();
				nosprop->nosMetaDataMap.Add(NosMetadataKeys::actorId, actor->GetActorGuid().ToString());
			}

			
			FString PropertyPath = nosprop->nosMetaDataMap.FindRef(NosMetadataKeys::PropertyPath);
			FString ComponentPath = nosprop->nosMetaDataMap.FindRef(NosMetadataKeys::component);
			FString IdStringKey = ActorUniqueName + ComponentPath + PropertyPath + nosprop->DisplayName;
			nosprop->Id = StringToFGuid(IdStringKey);
			childProperties.push_back(nosprop);
			
			for (auto it : nosprop->childProperties)
			{
				if(it->nosMetaDataMap.Contains(NosMetadataKeys::ContainerPath))
				{
					auto ContainerPath = it->nosMetaDataMap.Find(NosMetadataKeys::ContainerPath);
					ContainerPath->InsertAt(0, structprop->GetNameCPP() + FString("/") );
				}
				else
				{
					it->nosMetaDataMap.Add(NosMetadataKeys::ContainerPath, structprop->GetNameCPP());
				}
				it->nosMetaDataMap.Remove(NosMetadataKeys::component);
				it->nosMetaDataMap.Remove(NosMetadataKeys::actorId);
				
				FString ActorUniqueNameChild;
				if (auto component = Cast<USceneComponent>(container))
				{
					it->nosMetaDataMap.Add(NosMetadataKeys::component, component->GetFName().ToString());
					if (auto actor = component->GetOwner())
					{
						ActorUniqueNameChild = actor->GetFName().ToString();
						it->nosMetaDataMap.Add(NosMetadataKeys::actorId, actor->GetActorGuid().ToString());
					}
				}
				else if (auto actor = Cast<AActor>(container))
				{
					ActorUniqueNameChild = actor->GetFName().ToString();
					it->nosMetaDataMap.Add(NosMetadataKeys::actorId, actor->GetActorGuid().ToString());
				}
				
				FString PropertyPathChild = it->nosMetaDataMap.FindRef(NosMetadataKeys::PropertyPath);
				FString ComponentPathChild = it->nosMetaDataMap.FindRef(NosMetadataKeys::component);
				FString IdStringKeyChild = ActorUniqueNameChild + ComponentPathChild + PropertyPathChild;
				it->Id = StringToFGuid(IdStringKeyChild);
				
				childProperties.push_back(it);
			}
		}

		AProperty = AProperty->PropertyLinkNext;
	}

	data = std::vector<uint8_t>(1, 0);
	TypeName = "nos.fb.Void";
}

void NOSStructProperty::SetPropValue_Internal(void* val, size_t size, uint8* customContainer)
{
	//empty
}

NOSArrayProperty::NOSArrayProperty(UObject* container, FArrayProperty* ArrayProperty, FProperty* InnerProperty,
	FString parentCategory, uint8* StructPtr, NOSStructProperty* parentProperty)
	: NOSProperty(container, ArrayProperty, parentCategory, StructPtr, parentProperty), ArrayProperty(ArrayProperty), InnerProperty(InnerProperty)
{
	auto nosprop = NOSPropertyFactory::CreateProperty(nullptr, InnerProperty, "", nullptr, nullptr);
	if(nosprop)
		TypeName = "[" + nosprop->TypeName + "]";
}

void NOSArrayProperty::SetPropValue_Internal(void* val, size_t size, uint8* customContainer)
{
	void* container = nullptr;
	if (customContainer) container = customContainer;
	else if (ComponentContainer) container = ComponentContainer.Get();
	else if (ActorContainer) container = ActorContainer.Get();
	else if (ObjectPtr && IsValid(ObjectPtr)) container = ObjectPtr;
	else if (StructPtr) container = StructPtr;

	if (container)
	{
		auto nosprop = NOSPropertyFactory::CreateProperty(nullptr, InnerProperty, "", nullptr, nullptr);
		if(!nosprop)
			return;
		
		FScriptArrayHelper_InContainer ArrayHelper(ArrayProperty, container);
		nosprop->SetArrayPropValues(val, size, ArrayHelper);
	}
	
	MarkState();
	return;
}

std::vector<uint8> NOSArrayProperty::UpdatePinValue(uint8* customContainer)
{
	void* container = nullptr;
	if (customContainer) container = customContainer;
	else if (ComponentContainer) container = ComponentContainer.Get();
	else if (ActorContainer) container = ActorContainer.Get();
	else if (ObjectPtr && IsValid(ObjectPtr)) container = ObjectPtr;
	else if (StructPtr) container = StructPtr;

	if (container)
	{
		auto nosprop = NOSPropertyFactory::CreateProperty(nullptr, InnerProperty, "", nullptr, nullptr);
		if(!nosprop)
			return data;
		
		FScriptArrayHelper_InContainer ArrayHelper(ArrayProperty, container);
		flatbuffers::FlatBufferBuilder fb;
		if(nosprop->CreateFbArray(fb, ArrayHelper))
		{
			auto buf = fb.Release();
			data = std::vector<uint8_t>{flatbuffers::GetMutableRoot<u8>(buf.data()), buf.data()+buf.size()};
		}
	}
	return data;
}

bool PropertyVisible(FProperty* ueproperty);

NOSObjectProperty::NOSObjectProperty(UObject* container, FObjectProperty* uproperty, FString parentCategory, uint8* StructPtr, NOSStructProperty* parentProperty)
	: NOSProperty(container, uproperty, parentCategory, StructPtr, parentProperty), objectprop(uproperty)
{
	if (objectprop->PropertyClass->IsChildOf<UTextureRenderTarget2D>()) // We only support texturetarget2d from object properties
	{
		TypeName = "nos.sys.vulkan.Texture";
		ReadOnly = true;
		auto tex = NOSTextureShareManager::GetInstance()->AddTexturePin(this);
		data = nos::Buffer::From(tex);
	}
	else if (objectprop->PropertyClass->IsChildOf<UUserWidget>())
	{
		UObject* Container = ActorContainer.Get();
		if (!Container)
		{
			Container = ComponentContainer.Get();
		}
		auto Widget = Cast<UObject>(objectprop->GetObjectPropertyValue(objectprop->ContainerPtrToValuePtr<UUserWidget>(Container)));
		if(!Widget)
		{
			data = std::vector<uint8_t>(1, 0);
			TypeName = "nos.fb.Void";
			return;
		}
	
		auto WidgetClass = Widget->GetClass();
		FunctionContainerClass = WidgetClass;

		
		FProperty* WProperty = WidgetClass->PropertyLink;
		parentCategory = parentCategory + "|" + Widget->GetFName().ToString();
		while (WProperty != nullptr)
		{
			FName CCategoryName = FObjectEditorUtils::GetCategoryFName(WProperty);

			UClass* Class = WidgetClass;

			if (FEditorCategoryUtils::IsCategoryHiddenFromClass(Class, CCategoryName.ToString()) || !PropertyVisible(WProperty))
			{
				WProperty = WProperty->PropertyLinkNext;
				continue;
			}
			TSharedPtr<NOSProperty> nosprop = NOSPropertyFactory::CreateProperty(Widget, WProperty, parentCategory);

			if(nosprop->nosMetaDataMap.Contains(NosMetadataKeys::ContainerPath))
			{
				auto propPath = nosprop->nosMetaDataMap.Find(NosMetadataKeys::ContainerPath);
				propPath->InsertAt(0, objectprop->GetFName().ToString() + FString("/") );
			}
			else
			{
				nosprop->nosMetaDataMap.Add(NosMetadataKeys::ContainerPath, objectprop->GetFName().ToString());
			}
			
			
			nosprop->nosMetaDataMap.Remove(NosMetadataKeys::component);
			nosprop->nosMetaDataMap.Remove(NosMetadataKeys::actorId);
			if (auto component = Cast<USceneComponent>(container))
			{
				nosprop->nosMetaDataMap.Add(NosMetadataKeys::component, component->GetFName().ToString());
				if (auto actor = component->GetOwner())
				{
					nosprop->nosMetaDataMap.Add(NosMetadataKeys::actorId, actor->GetActorGuid().ToString());
				}
			}
			else if (auto actor = Cast<AActor>(container))
			{
				nosprop->nosMetaDataMap.Add(NosMetadataKeys::actorId, actor->GetActorGuid().ToString());
			}

			
			if (!nosprop)
			{
				WProperty = WProperty->PropertyLinkNext;
				continue;
			}
			//RegisteredProperties.Add(nosprop->Id, nosprop);
			childProperties.push_back(nosprop);

			for (auto It : nosprop->childProperties)
			{
				
				if(It->nosMetaDataMap.Contains(NosMetadataKeys::ContainerPath))
				{
					auto propPath = It->nosMetaDataMap.Find(NosMetadataKeys::ContainerPath);
					propPath->InsertAt(0, objectprop->GetFName().ToString() + FString("/") );
				}
				else
				{
					It->nosMetaDataMap.Add(NosMetadataKeys::ContainerPath, objectprop->GetFName().ToString());
				}
				//RegisteredProperties.Add(it->Id, it);
				It->nosMetaDataMap.Remove(NosMetadataKeys::component);
				It->nosMetaDataMap.Remove(NosMetadataKeys::actorId);
				if (auto component = Cast<USceneComponent>(container))
				{
					It->nosMetaDataMap.Add(NosMetadataKeys::component, component->GetFName().ToString());
					if (auto actor = component->GetOwner())
					{
						It->nosMetaDataMap.Add(NosMetadataKeys::actorId, actor->GetActorGuid().ToString());
					}
				}
				else if (auto actor = Cast<AActor>(container))
				{
					It->nosMetaDataMap.Add(NosMetadataKeys::actorId, actor->GetActorGuid().ToString());
				}
				childProperties.push_back(It);
			}

			WProperty = WProperty->PropertyLinkNext;
		}

		data = std::vector<uint8_t>(1, 0);
		TypeName = "nos.fb.Void";
	}
	else
	{
		data = std::vector<uint8_t>(1, 0);
		TypeName = "nos.fb.Void";
	}
}

void NOSObjectProperty::SetPropValue_Internal(void* val, size_t size, uint8* customContainer)
{
}

std::vector<uint8> NOSObjectProperty::UpdatePinValue(uint8* customContainer) 
{ 
	UObject* container = GetRawObjectContainer();

	if (objectprop->PropertyClass->IsChildOf<UTextureRenderTarget2D>()) // We only support texturetarget2d from object properties
		{
		const nos::sys::vulkan::Texture* tex = flatbuffers::GetRoot<nos::sys::vulkan::Texture>(data.data());
		nos::sys::vulkan::TTexture texture;
		tex->UnPackTo(&texture);

		if (NOSTextureShareManager::GetInstance()->UpdateTexturePin(this, texture))
			{
			// data = nos::Buffer::From(texture);
			flatbuffers::FlatBufferBuilder fb;
			auto offset = nos::sys::vulkan::CreateTexture(fb, &texture);
			fb.Finish(offset);
			nos::Buffer buffer = fb.Release();
			data = buffer;
			}
		}

	return std::vector<uint8>(); 
}

void NOSStringProperty::SetPropValue_Internal(void* val, size_t size, uint8* customContainer)
{
	IsChanged = true;

	void* container = nullptr;
	if (customContainer) container = customContainer;
	else if (ComponentContainer) container = ComponentContainer.Get();
	else if (ActorContainer) container = ActorContainer.Get();
	else if (ObjectPtr && IsValid(ObjectPtr)) container = ObjectPtr;
	else if (StructPtr) container = StructPtr;

	if (!container)
	{
		UE_LOG(LogTemp, Warning, TEXT("The property %s has null container!"), *(DisplayName));
		return; //TODO investigate why container is null
	}
	FString newval(UTF8_TO_TCHAR((char*)val));
	stringprop->SetPropertyValue_InContainer(container, newval);
	MarkState();
	return;
}

std::vector<uint8> NOSStringProperty::UpdatePinValue(uint8* customContainer)
{
	void* container = nullptr;
	if (customContainer) container = customContainer;
	else if (ComponentContainer) container = ComponentContainer.Get();
	else if (ActorContainer) container = ActorContainer.Get();
	else if (ObjectPtr && IsValid(ObjectPtr)) container = ObjectPtr;
	else if (StructPtr) container = StructPtr;

	FString val(" ");
	if (container)
	{
		val = stringprop->GetPropertyValue_InContainer(container);
	}
	auto s = StringCast<UTF8CHAR>(*val);
	data = std::vector<uint8_t>(s.Length() + 1, 0);
	memcpy(data.data(), s.Get(), s.Length());

	return data;
}

bool NOSStringProperty::CreateFbArray(flatbuffers::FlatBufferBuilder& fb, FScriptArrayHelper_InContainer& ArrayHelper)
{
	std::vector<flatbuffers::Offset<flatbuffers::String>> StringArray;
	for(int i = 0; i < ArrayHelper.Num(); i++)
	{
		if(auto ElementPtr = ArrayHelper.GetRawPtr(i))
		{
			FString val = *(FString*)ElementPtr;
			char* result = TCHAR_TO_UTF8(*val);
			auto offset = fb.CreateString(result);
			StringArray.push_back(offset);
		}
	}
	auto offset = fb.CreateVector(StringArray).o;
	fb.Finish(flatbuffers::Offset<flatbuffers::Vector<nos::fb::Track>>(offset));
	return true;
}

void NOSStringProperty::SetArrayPropValues(void* val, size_t size, FScriptArrayHelper_InContainer& ArrayHelper)
{
	auto vec = (flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>*)val; 
	int ct = vec->size();
	ArrayHelper.Resize(ct);
	for(int i = 0; i < ct; i++)
	{
		ArrayHelper.ExpandForIndex(i);
		auto string = vec->Get(i);
		FString newString(UTF8_TO_TCHAR(string->c_str()));
		FString* String = (FString*)ArrayHelper.GetRawPtr(i);
		*String = newString;
	}
}

void NOSNameProperty::SetPropValue_Internal(void* val, size_t size, uint8* customContainer)
{
	IsChanged = true;

	void* container = nullptr;
	if (customContainer) container = customContainer;
	else if (ComponentContainer) container = ComponentContainer.Get();
	else if (ActorContainer) container = ActorContainer.Get();
	else if (ObjectPtr && IsValid(ObjectPtr)) container = ObjectPtr;
	else if (StructPtr) container = StructPtr;

	if (!container)
	{
		UE_LOG(LogTemp, Warning, TEXT("The property %s has null container!"), *(DisplayName));
		return; //TODO investigate why container is null
	}
	FString newval(UTF8_TO_TCHAR((char*)val));
	nameprop->SetPropertyValue_InContainer(container, FName(newval));
	
	MarkState();
	
	return;
}

std::vector<uint8> NOSNameProperty::UpdatePinValue(uint8* customContainer)
{
	void* container = nullptr;
	if (customContainer) container = customContainer;
	else if (ComponentContainer) container = ComponentContainer.Get();
	else if (ActorContainer) container = ActorContainer.Get();
	else if (ObjectPtr && IsValid(ObjectPtr)) container = ObjectPtr;
	else if (StructPtr) container = StructPtr;

	FString val(" ");
	if (container)
	{
		val = nameprop->GetPropertyValue_InContainer(container).ToString();
	}
	auto s = StringCast<UTF8CHAR>(*val);
	data = std::vector<uint8_t>(s.Length() + 1, 0);
	memcpy(data.data(), s.Get(), s.Length());
	
	return data;
}

void NOSTextProperty::SetPropValue_Internal(void* val, size_t size, uint8* customContainer)
{
	IsChanged = true;

	void* container = nullptr;
	if (customContainer) container = customContainer;
	else if (ComponentContainer) container = ComponentContainer.Get();
	else if (ActorContainer) container = ActorContainer.Get();
	else if (ObjectPtr && IsValid(ObjectPtr)) container = ObjectPtr;
	else if (StructPtr) container = StructPtr;
	
	if (!container)
	{
		UE_LOG(LogTemp, Warning, TEXT("The property %s has null container!"), *(DisplayName));
		return; //TODO investigate why container is null
	}
	FString newval(UTF8_TO_TCHAR((char*)val));
	textprop->SetPropertyValue_InContainer(container, FText::FromString(newval));

	MarkState();
	return;
}

std::vector<uint8> NOSTextProperty::UpdatePinValue(uint8* customContainer)
{
	void* container = nullptr;
	if (customContainer) container = customContainer;
	else if (ComponentContainer) container = ComponentContainer.Get();
	else if (ActorContainer) container = ActorContainer.Get();
	else if (ObjectPtr && IsValid(ObjectPtr)) container = ObjectPtr;
	else if (StructPtr) container = StructPtr;

	FString val(" ");
	if (container)
	{
		val = textprop->GetPropertyValue_InContainer(container).ToString();
	}

	auto s = StringCast<UTF8CHAR>(*val);
	data = std::vector<uint8_t>(s.Length() + 1, 0);
	memcpy(data.data(), s.Get(), s.Length());

	return data;
}

bool NOSTextProperty::CreateFbArray(flatbuffers::FlatBufferBuilder& fb, FScriptArrayHelper_InContainer& ArrayHelper)
{
	std::vector<flatbuffers::Offset<flatbuffers::String>> StringArray;
	for(int i = 0; i < ArrayHelper.Num(); i++)
	{
		if(auto ElementPtr = ArrayHelper.GetRawPtr(i))
		{
			FString val = (*(FText*)ElementPtr).ToString();
			char* result = TCHAR_TO_UTF8(*val);
			auto offset = fb.CreateString(result);
			StringArray.push_back(offset);
		}
	}
	auto offset = fb.CreateVector(StringArray).o;
	fb.Finish(flatbuffers::Offset<flatbuffers::Vector<nos::fb::Track>>(offset));
	return true;
}

void NOSTextProperty::SetArrayPropValues(void* val, size_t size, FScriptArrayHelper_InContainer& ArrayHelper)
{
	auto vec = (flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>*)val; 
	int ct = vec->size();
	ArrayHelper.Resize(ct);
	for(int i = 0; i < ct; i++)
	{
		ArrayHelper.ExpandForIndex(i);
		auto string = vec->Get(i);
		FString newString(UTF8_TO_TCHAR(string->c_str()));
		FText* Text = (FText*)ArrayHelper.GetRawPtr(i);
		*Text = FText::FromString(newString);
	}
}

flatbuffers::Offset<nos::fb::Visualizer> NOSEnumProperty::SerializeVisualizer(flatbuffers::FlatBufferBuilder& fbb)
{
	return nos::fb::CreateVisualizerDirect(fbb, nos::fb::VisualizerType::COMBO_BOX, TCHAR_TO_UTF8(*PrefixStringList(Enum->GetFName().ToString())));
}

flatbuffers::Offset<nos::fb::Pin> NOSEnumProperty::Serialize(flatbuffers::FlatBufferBuilder& fbb)
{
	std::vector<flatbuffers::Offset<nos::fb::MetaDataEntry>> metadata = SerializeMetaData(fbb);
	DisplayName = ValidateName(DisplayName);
	return nos::fb::CreatePinDirect(fbb, (nos::fb::UUID*)&(NOSProperty::Id), TCHAR_TO_UTF8(*DisplayName), TCHAR_TO_ANSI(TEXT("string")), PinShowAs, PinCanShowAs, TCHAR_TO_UTF8(*CategoryName), SerializeVisualizer(fbb), &data, 0, 0, 0, 0, 0, ReadOnly, IsAdvanced, transient, &metadata, 0,  nos::fb::PinContents::JobPin, 0, 0, nos::fb::PinValueDisconnectBehavior::KEEP_LAST_VALUE, TCHAR_TO_UTF8(*ToolTipText), TCHAR_TO_UTF8(*Property->GetDisplayNameText().ToString()));
}

void NOSEnumProperty::SetPropValue_Internal(void* val, size_t size, uint8* customContainer)
{
	//TODO
	
	IsChanged = true;

	void* container = nullptr;
	if (customContainer) container = customContainer;
	else if (ComponentContainer) container = ComponentContainer.Get();
	else if (ActorContainer) container = ActorContainer.Get();
	else if (ObjectPtr && IsValid(ObjectPtr)) container = ObjectPtr;
	else if (StructPtr) container = StructPtr;

	if (container)
	{
		UEnum* EnumPtr = nullptr;
		FNumericProperty *NumericProperty = nullptr;
		if(const FEnumProperty* PropAsEnum = CastField<FEnumProperty>(Property))
		{
			EnumPtr = PropAsEnum->GetEnum();
			NumericProperty = PropAsEnum->GetUnderlyingProperty();
		}
		else if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			EnumPtr = ByteProperty->GetIntPropertyEnum();
			NumericProperty = CastField<FNumericProperty>(Property);
		}

		if(EnumPtr && NumericProperty)
		{
			FString ValueString((char*)val);
			int64 Index = EnumPtr->GetIndexByNameString(ValueString);
			if (Index != INDEX_NONE)
			{
				int64 Value = EnumPtr->GetValueByIndex(Index);
				uint8* PropData = Property->ContainerPtrToValuePtr<uint8>(container);
				NumericProperty->SetIntPropertyValue(PropData, Value);
			}
		}
	}
	
	MarkState();

	return;
}

std::vector<uint8> NOSEnumProperty::UpdatePinValue(uint8* customContainer)
{
	void* container = nullptr;
	if (customContainer) container = customContainer;
	else if (ComponentContainer) container = ComponentContainer.Get();
	else if (ActorContainer) container = ActorContainer.Get();
	else if (ObjectPtr && IsValid(ObjectPtr)) container = ObjectPtr;
	else if (StructPtr) container = StructPtr;

	FString val(" ");
	
	if (container)
	{
		UEnum* EnumPtr = nullptr;
		FNumericProperty *NumericProperty = nullptr;
		if(const FEnumProperty* PropAsEnum = CastField<FEnumProperty>(Property))
		{
			EnumPtr = PropAsEnum->GetEnum();
			NumericProperty = PropAsEnum->GetUnderlyingProperty();
		}
		else if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			EnumPtr = ByteProperty->GetIntPropertyEnum();
			NumericProperty = CastField<FNumericProperty>(Property);
		}

		if(EnumPtr && NumericProperty)
		{
			uint8* PropData = Property->ContainerPtrToValuePtr<uint8>(container);
			CurrentName = Enum->GetNameByValue(*PropData).ToString();
			val = CurrentName;
		}
	}
	
	auto s = StringCast<ANSICHAR>(*val);
	data = std::vector<uint8_t>(s.Length() + 1, 0);
	memcpy(data.data(), s.Get(), s.Length());

	return data;
}

bool IsArrayPropertySupported(FArrayProperty* ArrayProperty)
{
	auto InnerProperty = ArrayProperty->Inner;
	if(CastField<FNumericProperty>(InnerProperty))
		return true;
	if(CastField<FStrProperty>(InnerProperty) || CastField<FTextProperty>(InnerProperty))
		return true;
	if (FStructProperty* structprop = CastField<FStructProperty>(InnerProperty))
	{
		if (structprop->Struct == FNOSTrack::StaticStruct() ||
			structprop->Struct == TBaseStructure<FVector2D>::Get() ||
			structprop->Struct == TBaseStructure<FVector>::Get() ||
			structprop->Struct == TBaseStructure<FVector4>::Get() ||
			structprop->Struct == TBaseStructure<FLinearColor>::Get() ||
			structprop->Struct == TBaseStructure<FNOSTrack>::Get() ||
			structprop->Struct == TBaseStructure<FColor>::Get())
			return true;
	}
	
	return false;
}


TSharedPtr<NOSProperty> NOSPropertyFactory::CreateProperty(UObject* container,
                                                         FProperty* uproperty, 
                                                         FString parentCategory, 
                                                         uint8* StructPtr, 
                                                         NOSStructProperty* parentProperty)
{
	TSharedPtr<NOSProperty> prop = nullptr;

	//CAST THE PROPERTY ACCORDINGLY
	uproperty->GetClass();
	
	if(CastField<FNumericProperty>(uproperty) && CastField<FNumericProperty>(uproperty)->IsEnum())
	{
		FNumericProperty* numericprop = CastField<FNumericProperty>(uproperty);
		UEnum* uenum = numericprop->GetIntPropertyEnum();
		prop = TSharedPtr<NOSProperty>(new NOSEnumProperty(container, nullptr, numericprop, uenum, parentCategory, StructPtr, parentProperty));
	}
	else if (FFloatProperty* floatprop = CastField<FFloatProperty>(uproperty) ) 
	{
		prop = TSharedPtr<NOSProperty>(new NOSFloatProperty(container, floatprop, parentCategory, StructPtr, parentProperty));
	}
	else if (FDoubleProperty* doubleprop = CastField<FDoubleProperty>(uproperty))
	{
		prop = TSharedPtr<NOSProperty>(new NOSDoubleProperty(container, doubleprop, parentCategory, StructPtr, parentProperty));
	}
	else if (FInt8Property* int8prop = CastField<FInt8Property>(uproperty))
	{
		prop = TSharedPtr<NOSProperty>(new NOSInt8Property(container, int8prop, parentCategory, StructPtr, parentProperty));
	}
	else if (FInt16Property* int16prop = CastField<FInt16Property>(uproperty))
	{
		prop = TSharedPtr<NOSProperty>(new NOSInt16Property(container, int16prop, parentCategory, StructPtr, parentProperty));
	}
	else if (FIntProperty* intprop = CastField<FIntProperty>(uproperty))
	{
		prop = TSharedPtr<NOSProperty>(new NOSIntProperty(container, intprop, parentCategory, StructPtr, parentProperty));
	}
	else if (FInt64Property* int64prop = CastField<FInt64Property>(uproperty))
	{
		prop = TSharedPtr<NOSProperty>(new NOSInt64Property(container, int64prop, parentCategory, StructPtr, parentProperty));
	}
	else if (FByteProperty* byteprop = CastField<FByteProperty>(uproperty))
	{
		prop = TSharedPtr<NOSProperty>(new NOSByteProperty(container, byteprop, parentCategory, StructPtr, parentProperty));
	}
	else if (FUInt16Property* uint16prop = CastField<FUInt16Property>(uproperty))
	{
		prop = TSharedPtr<NOSProperty>(new NOSUInt16Property(container, uint16prop, parentCategory, StructPtr, parentProperty));
	}
	else if (FUInt32Property* uint32prop = CastField<FUInt32Property>(uproperty))
	{
		prop = TSharedPtr<NOSProperty>(new NOSUInt32Property(container, uint32prop, parentCategory, StructPtr, parentProperty));
	}
	else if (FUInt64Property* uint64prop = CastField<FUInt64Property>(uproperty))
	{
		prop = TSharedPtr<NOSProperty>(new NOSUInt64Property(container, uint64prop, parentCategory, StructPtr, parentProperty));
	}
	else if (FBoolProperty* boolprop = CastField<FBoolProperty>(uproperty))
	{
		prop = TSharedPtr<NOSProperty>(new NOSBoolProperty(container, boolprop, parentCategory, StructPtr, parentProperty));
	}
	else if (FEnumProperty* enumprop = CastField<FEnumProperty>(uproperty))
	{
		FNumericProperty* numericprop = enumprop->GetUnderlyingProperty();
		UEnum* uenum = enumprop->GetEnum();
		prop = TSharedPtr<NOSProperty>(new NOSEnumProperty(container, enumprop, numericprop, uenum, parentCategory, StructPtr, parentProperty));
	}
	else if (FTextProperty* textprop = CastField<FTextProperty>(uproperty))
	{
		prop = TSharedPtr<NOSProperty>(new NOSTextProperty(container, textprop, parentCategory, StructPtr, parentProperty));
	}
	else if (FNameProperty* nameprop = CastField<FNameProperty>(uproperty))
	{
		prop = TSharedPtr<NOSProperty>(new NOSNameProperty(container, nameprop, parentCategory, StructPtr, parentProperty));
	}
	else if (FStrProperty* stringProp = CastField<FStrProperty>(uproperty))
	{
		prop = TSharedPtr<NOSProperty>(new NOSStringProperty(container, stringProp, parentCategory, StructPtr, parentProperty));
	}
	else if (FObjectProperty* objectprop = CastField<FObjectProperty>(uproperty))
	{
		if (!container) // TODO: Handle inside NOSObjectProperty
		{
			return nullptr;
		}
		prop = TSharedPtr<NOSProperty>(new NOSObjectProperty(container, objectprop, parentCategory, StructPtr, parentProperty));
	}
	else if (FArrayProperty* arrayprop = CastField<FArrayProperty>(uproperty))
	{
		if(container)
		{
			if(IsArrayPropertySupported(arrayprop))
			{
				prop = TSharedPtr<NOSProperty>(new NOSArrayProperty(container, arrayprop, arrayprop->Inner, parentCategory, StructPtr, parentProperty));
			}
		}
	}
	else if (FStructProperty* structprop = CastField<FStructProperty>(uproperty))
	{
		//TODO ADD SUPPORT FOR FTRANSFORM
		if (structprop->Struct == TBaseStructure<FVector2D>::Get()) //vec2
		{
			prop = TSharedPtr<NOSProperty>(new NOSVec2Property(container, structprop, parentCategory, StructPtr, parentProperty));
		}
		else if (structprop->Struct == TBaseStructure<FVector>::Get()) //vec3
		{
			prop = TSharedPtr<NOSProperty>(new NOSVec3Property(container, structprop, parentCategory, StructPtr, parentProperty));
		}
		else if (structprop->Struct == TBaseStructure<FRotator>::Get())
		{
			prop = TSharedPtr<NOSProperty>(new NOSRotatorProperty(container, structprop, parentCategory, StructPtr, parentProperty));
		}
		else if (structprop->Struct == TBaseStructure<FVector4>::Get() || structprop->Struct == TBaseStructure<FQuat>::Get()) //vec4
		{
			prop = TSharedPtr<NOSProperty>(new NOSVec4Property(container, structprop, parentCategory, StructPtr, parentProperty));
		}
		else if (structprop->Struct == TBaseStructure<FLinearColor>::Get()) //vec4f
		{
			prop = TSharedPtr<NOSProperty>(new NOSVec4FProperty(container, structprop, parentCategory, StructPtr, parentProperty));
		}
		else if (structprop->Struct == FNOSTrack::StaticStruct()) //track
		{
			prop = TSharedPtr<NOSProperty>(new NOSTrackProperty(container, structprop, parentCategory, StructPtr, parentProperty));
		}
		else if (structprop->Struct == TBaseStructure<FTransform>::Get()) //track
		{
			prop = TSharedPtr<NOSProperty>(new NOSTransformProperty(container, structprop, parentCategory, StructPtr, parentProperty));
		}
		else if (structprop->Struct == TBaseStructure<FColor>::Get()) //track
		{
			prop = TSharedPtr<NOSProperty>(new NOSColorProperty(container, structprop, parentCategory, StructPtr, parentProperty));
		}
		else //auto construct
		{
			prop = TSharedPtr<NOSProperty>(new NOSStructProperty(container, structprop, parentCategory, StructPtr, parentProperty));
		}
	}

	if (!prop)
	{
		return nullptr; //for properties that we do not support
	}

	prop->UpdatePinValue();
	prop->default_val = prop->data;
	if (prop->TypeName == "nos.fb.Void")
	{
		prop->data.clear();
		prop->default_val.clear();
	}
#if 0 //default properties from objects
	if (prop->TypeName == "nos.fb.Void")
	{
		prop->data.clear();
		prop->default_val.clear();
	}
	else if (auto actor = Cast<AActor>(container))
	{
		if (prop->TypeName != "bool")
		{
			auto defobj = actor->GetClass()->GetDefaultObject();
			if (defobj)
			{
				auto val = uproperty->ContainerPtrToValuePtr<void>(defobj);
				if (prop->default_val.size() != uproperty->GetSize())
				{
					prop->default_val = std::vector<uint8>(uproperty->GetSize(), 0);	
				}
				memcpy(prop->default_val.data(), val, uproperty->GetSize());
			}

		}
		else
		{
			auto defobj = actor->GetClass()->GetDefaultObject();
			if (defobj)
			{
				auto val = !!( *uproperty->ContainerPtrToValuePtr<bool>(defobj) );
				if (prop->default_val.size() != uproperty->GetSize())
				{
					prop->default_val = std::vector<uint8>(uproperty->GetSize(), 0);
				}
				memcpy(prop->default_val.data(), &val, uproperty->GetSize());
			}

		}
			//uproperty->ContainerPtrToValuePtrForDefaults()
	}
	else if (auto sceneComponent = Cast<USceneComponent>(container))
	{
		if (prop->TypeName != "bool")
		{
			auto defobj = sceneComponent->GetClass()->GetDefaultObject();
			if (defobj)
			{
				auto val = uproperty->ContainerPtrToValuePtr<void>(defobj);
				if (prop->default_val.size() != uproperty->GetSize())
				{
					prop->default_val = std::vector<uint8>(uproperty->GetSize(), 0);
				}
				memcpy(prop->default_val.data(), val, uproperty->GetSize());
			}
		}
		else
		{
			auto defobj = sceneComponent->GetClass()->GetDefaultObject();
			if (defobj)
			{
				auto val = !!( *uproperty->ContainerPtrToValuePtr<bool>(defobj) );

				if (prop->default_val.size() != uproperty->GetSize())
				{
					prop->default_val = std::vector<uint8>(uproperty->GetSize(), 0);
				}
				memcpy(prop->default_val.data(), &val, uproperty->GetSize());
			}

		}
		//uproperty->ContainerPtrToValuePtrForDefaults()
	}
#endif

	FString ActorUniqueName;
	//update metadata
	// prop->nosMetaDataMap.Add("property", uproperty->GetFName().ToString());
	prop->nosMetaDataMap.Add(NosMetadataKeys::PropertyPath, uproperty->GetPathName());
	prop->nosMetaDataMap.Add(NosMetadataKeys::PropertyDisplayName, uproperty->GetDisplayNameText().ToString());
	if (auto component = Cast<USceneComponent>(container))
	{
		prop->nosMetaDataMap.Add(NosMetadataKeys::component, component->GetFName().ToString());
		if (auto actor = component->GetOwner())
		{
			prop->nosMetaDataMap.Add(NosMetadataKeys::actorId, actor->GetActorGuid().ToString());
			prop->nosMetaDataMap.Add(NosMetadataKeys::ActorDisplayName, actor->GetActorLabel());
			ActorUniqueName = actor->GetFName().ToString();
		}
	}
	else if (auto actor = Cast<AActor>(container))
	{
		prop->nosMetaDataMap.Add(NosMetadataKeys::actorId, actor->GetActorGuid().ToString());
		prop->nosMetaDataMap.Add(NosMetadataKeys::ActorDisplayName, actor->GetActorLabel());
		ActorUniqueName = actor->GetFName().ToString();
	}
	
	// FProperty* tryprop = FindFProperty<FProperty>(*uproperty->GetPathName());
	//UE_LOG(LogNOSSceneTreeManager, Warning, TEXT("name of the prop before %s, found property name %s"),*uproperty->GetFName().ToString(),  *tryprop->GetFName().ToString());


	FString PropertyPath = prop->nosMetaDataMap.FindRef(NosMetadataKeys::PropertyPath);
	FString ComponentPath = prop->nosMetaDataMap.FindRef(NosMetadataKeys::component);
	FString IdStringKey = ActorUniqueName + ComponentPath + PropertyPath;
	prop->Id = StringToFGuid(IdStringKey);
	return prop;
}


NOSActorReference::NOSActorReference(TObjectPtr<AActor> actor)
{
	if (actor)
	{
		Actor = TWeakObjectPtr<AActor>(actor);
		ActorGuid = Actor->GetActorGuid();
	}
}

NOSActorReference::NOSActorReference()
{

}

AActor* NOSActorReference::Get()
{
	if (Actor.IsValid())
	{
		return Actor.Get();
	}
	else if (UpdateActualActorPointer())
	{
		return Actor.Get();
	}
	return nullptr;
}

bool NOSActorReference::UpdateActorPointer(UWorld* World)
{
	if (!ActorGuid.IsValid())
	{
		InvalidReference = true;
		return false;
	}

	TMap<FGuid, AActor*> sceneActorMap;
	if (IsValid(World))
	{
		for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
		{
			if (ActorItr->GetActorGuid() == ActorGuid)
			{
				Actor = TWeakObjectPtr<AActor>(*ActorItr);
				return true;
			}
		}
	}
	InvalidReference = true;
	return false;
}

bool NOSActorReference::UpdateActualActorPointer()
{
	UWorld* World;
	if (FNOSSceneTreeManager::daWorld)
	{
		World = FNOSSceneTreeManager::daWorld;
	}
	else
	{
		World = GEngine->GetWorldContextFromGameViewport(GEngine->GameViewport)->World();
	}
	return UpdateActorPointer(World);
}

NOSComponentReference::NOSComponentReference(TObjectPtr<UActorComponent> actorComponent)
	: Actor(actorComponent->GetOwner())
{
	if (actorComponent)
	{
		Component = TWeakObjectPtr<UActorComponent>(actorComponent);
		ComponentProperty = Component->GetFName();
		PathToComponent = Component->GetPathName(Actor.Get());
	}
}

NOSComponentReference::NOSComponentReference()
{

}

UActorComponent* NOSComponentReference::Get()
{
	if (Component.IsValid())
	{
		return Component.Get();
	}
	else if (UpdateActualComponentPointer())
	{
		return Component.Get();
	}

	return nullptr;
}

AActor* NOSComponentReference::GetOwnerActor()
{
	return Actor.Get();
}

bool NOSComponentReference::UpdateActualComponentPointer()
{
	if (!Actor.Get() || Actor.InvalidReference || PathToComponent.IsEmpty())
	{
		InvalidReference = true;
		return false;
	}

	auto comp = FindObject<UActorComponent>(Actor.Get(), *PathToComponent);
	if (comp)
	{
		Component = TWeakObjectPtr<UActorComponent>(comp);
		return true;
	}

	InvalidReference = true;
	return false;
}

void NOSTriggerProperty::SetPropValue(void* val, size_t size, uint8* customContainer)
{

}

void NOSTriggerProperty::SetPropValue_Internal(void* val, size_t size, uint8* customContainer)
{

}

std::vector<uint8> NOSTriggerProperty::UpdatePinValue(uint8* customContainer)
{
	return std::vector<uint8>();
}
