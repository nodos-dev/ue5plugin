/*
 * Copyright MediaZ Teknoloji A.S. All Rights Reserved.
 */

#pragma once
#include "CoreMinimal.h"
#include <vector>
#pragma warning (disable : 4800)
#pragma warning (disable : 4668)
#include "AppEvents_generated.h"
#include "NOSTrack.h"
#include "NOSClient.h"
#include "NOSAssetManager.h"

namespace NosMetadataKeys
{
#define NOS_METADATA_KEY(key) inline const char* key = #key;
		NOS_METADATA_KEY(DoNotAttachToRealityParent);
		NOS_METADATA_KEY(spawnTag);
		NOS_METADATA_KEY(ActorGuid);
		NOS_METADATA_KEY(umgTag);
		NOS_METADATA_KEY(PropertyPath);
		NOS_METADATA_KEY(ContainerPath);
		NOS_METADATA_KEY(component);
		NOS_METADATA_KEY(actorId);
		NOS_METADATA_KEY(EditConditionPropertyId);
		NOS_METADATA_KEY(PinHidden);
		NOS_METADATA_KEY(PinnedCategories);
		NOS_METADATA_KEY(NodeColor);
		NOS_METADATA_KEY(FunctionName);
		NOS_METADATA_KEY(FunctionId);
		NOS_METADATA_KEY(FunctionPropertyName);
		NOS_METADATA_KEY(ActorDisplayName);
		NOS_METADATA_KEY(PropertyDisplayName);
};

#define NODOS_MD5_HASHING

inline FGuid StringToFGuid(const FString& inString)
{
	FGuid id;
#ifdef NODOS_MD5_HASHING
	auto string = FNOSClient::AppKey + inString;
	FString HexHash = FMD5::HashAnsiString(*string);
	TArray<uint8> BinKey;
	BinKey.AddUninitialized(HexHash.Len() / 2);
	HexToBytes(HexHash, BinKey.GetData());
	id.A = *(uint32*)(BinKey.GetData());
	id.B = *(uint32*)(BinKey.GetData() + 4);
	id.C = *(uint32*)(BinKey.GetData() + 8);
	id.D = *(uint32*)(BinKey.GetData() + 12);
#else
	id = FGuid::NewGuid();
#endif
	return id;
}

class NOSStructProperty;

class NOSSCENETREEMANAGER_API NOSActorReference
{
public:
	__declspec(noinline) NOSActorReference(TObjectPtr<AActor> actor);
	NOSActorReference();
	
	AActor* Get();
	
	explicit operator bool() {
		return !!(Get());
	}

	AActor* operator->()
	{
		return Get();
	}

	bool UpdateActorPointer(UWorld* World);
	bool UpdateActualActorPointer();

	bool InvalidReference = false;
private:
	
	UPROPERTY()
	TWeakObjectPtr<AActor> Actor;
	
	FGuid ActorGuid;
	
};

class NOSSCENETREEMANAGER_API NOSComponentReference
{

public:
	NOSComponentReference(TObjectPtr<UActorComponent> actorComponent);
	NOSComponentReference();

	UActorComponent* Get();
	AActor* GetOwnerActor();

	explicit operator bool() {
		return !!(Get());
	}

	UActorComponent* operator->()
	{
		return Get();
	}

	bool UpdateActualComponentPointer();
	NOSActorReference Actor;
	bool InvalidReference = false;

private:
	UPROPERTY()
	TWeakObjectPtr<UActorComponent> Component;
	

	FName ComponentProperty;
	FString PathToComponent;

};

class NOSSCENETREEMANAGER_API NOSProperty : public TSharedFromThis<NOSProperty>
{
public:
	NOSProperty(UObject* Container, FProperty* UProperty, FString ParentCategory = FString(), uint8 * StructPtr = nullptr, NOSStructProperty* parentProperty = nullptr);

	virtual void SetPropValue(void* val, size_t size, uint8* customContainer = nullptr);
	UObject* GetRawObjectContainer();
	void* GetRawContainer();

	virtual std::vector<uint8> UpdatePinValue(uint8* customContainer = nullptr);
	//std::vector<uint8> GetValue(uint8* customContainer = nullptr);
	void MarkState();
	virtual flatbuffers::Offset<nos::fb::Pin> Serialize(flatbuffers::FlatBufferBuilder& fbb);
	std::vector<flatbuffers::Offset<nos::fb::MetaDataEntry>> SerializeMetaData(flatbuffers::FlatBufferBuilder& fbb);
	virtual flatbuffers::Offset<nos::fb::Visualizer> SerializeVisualizer(flatbuffers::FlatBufferBuilder& fbb) {return 0;}
	
	virtual bool CreateFbArray(flatbuffers::FlatBufferBuilder& fb, FScriptArrayHelper_InContainer& ArrayHelper) {return false;}
	virtual void SetArrayPropValues(void* val, size_t size, FScriptArrayHelper_InContainer& ArrayHelper) {}
	
	FProperty* Property = nullptr;

	NOSActorReference ActorContainer;
	NOSComponentReference ComponentContainer;
	UObject* ObjectPtr = nullptr;
	uint8* StructPtr = nullptr;
	
	NOSComponentReference BoundComponent;

	FString PropertyName;
	FString DisplayName;
	FString CategoryName;
	FString ToolTipText;
	FString UIMaxString;
	FString UIMinString;
	FString EditConditionPropertyName;
	FProperty* EditConditionProperty = nullptr;
	bool IsAdvanced = false;
	bool ReadOnly = false;
	bool IsOrphan = false;
	FString OrphanMessage = " ";
	std::string TypeName;
	FGuid Id;
	std::vector<uint8_t> data; //wrt Nodos standarts
	std::vector<uint8_t> default_val; //wrt Nodos standarts
	std::vector<uint8_t> min_val; //wrt Nodos standarts
	std::vector<uint8_t> max_val; //wrt Nodos standarts
	nos::fb::ShowAs PinShowAs = nos::fb::ShowAs::PROPERTY;
	nos::fb::CanShowAs PinCanShowAs = nos::fb::CanShowAs::INPUT_OUTPUT_PROPERTY;
	std::vector<TSharedPtr<NOSProperty>> childProperties;
	TMap<FString, FString> nosMetaDataMap;
	bool transient = true;
	bool IsChanged = false;
	bool IsFunctionProp = false;
	FGuid FunctionId;

	UClass* FunctionContainerClass = nullptr;

	virtual ~NOSProperty() {}
protected:
	virtual void SetPropValue_Internal(void* val, size_t size, uint8* customContainer = nullptr);
	virtual void SetProperty_InCont(void* container, void* val);

private:
	void CallOnChangedFunction();

};

class NOSTriggerProperty : public NOSProperty
{
public:
	NOSTriggerProperty() : NOSProperty(nullptr, nullptr)
	{
		TypeName = "nos.exe";
		data = std::vector<uint8_t>(1, 0);
		DisplayName = "Trigger";
		CategoryName = "Default";
	}
	virtual flatbuffers::Offset<nos::fb::Pin> Serialize(flatbuffers::FlatBufferBuilder& fbb) override
	{
		return nos::fb::CreatePinDirect(fbb, (nos::fb::UUID*)&Id, TCHAR_TO_UTF8(*PropertyName), "nos.exe", nos::fb::ShowAs::PROPERTY, nos::fb::CanShowAs::INPUT_PIN_OR_PROPERTY, TCHAR_TO_UTF8(*CategoryName), 0, 0, 0, 0, 0, 0, 0, false, false, true, 0, 0, nos::fb::PinContents::JobPin, 0, 0, nos::fb::PinValueDisconnectBehavior::KEEP_LAST_VALUE, 0, TCHAR_TO_UTF8(*DisplayName));
	};
	virtual void SetPropValue(void* val, size_t size, uint8* customContainer = nullptr) override;
	virtual void SetPropValue_Internal(void* val, size_t size, uint8* customContainer = nullptr) override;
	virtual std::vector<uint8> UpdatePinValue(uint8* customContainer = nullptr) override; 

};

template<typename T, nos::tmp::StrLiteral LitType, typename CppType = T::TCppType>
requires std::is_base_of_v<FProperty, T>
class NOSNumericProperty : public NOSProperty {
public:
	NOSNumericProperty(UObject* container, T* uproperty, FString parentCategory = FString(), uint8* StructPtr = nullptr, NOSStructProperty* parentProperty = nullptr) :
		NOSProperty(container, uproperty, parentCategory, StructPtr, parentProperty), Property(uproperty)
	{
		data = std::vector<uint8_t>(sizeof(CppType), 0);
		TypeName = LitType.val;
	}
	T* Property;
	virtual std::vector<uint8> UpdatePinValue(uint8* customContainer = nullptr) override 
	{
		void* container = nullptr;
		if (customContainer) container = customContainer;
		else if (ComponentContainer) container = ComponentContainer.Get();
		else if (ActorContainer) container = ActorContainer.Get();
		else if (ObjectPtr && IsValid(ObjectPtr)) container = ObjectPtr;
		else if (StructPtr) container = StructPtr;

		if (container)
		{
			CppType val = static_cast<CppType>(Property->GetPropertyValue_InContainer(container));
			memcpy(data.data(), &val, data.size());
		}

		return data;
	}

	virtual bool CreateFbArray(flatbuffers::FlatBufferBuilder& fb, FScriptArrayHelper_InContainer& ArrayHelper) override
	{
		fb.StartVector(ArrayHelper.Num(), sizeof(CppType), 1);
		for(int i = ArrayHelper.Num()-1; i >= 0; i--)
		{
			if(auto ElementPtr = ArrayHelper.GetRawPtr(i))
			{
				fb.PushBytes((uint8_t*)ElementPtr, sizeof(CppType));
			}
		}
		auto offset = fb.EndVector(ArrayHelper.Num());
		fb.Finish(flatbuffers::Offset<flatbuffers::Vector<uint8_t>>(offset));
		return true;
	}

	virtual void SetArrayPropValues(void* val, size_t size, FScriptArrayHelper_InContainer& ArrayHelper) override
	{
		auto vec = (flatbuffers::Vector<u8>*)val; 
		int ct = vec->size();
		ArrayHelper.Resize(ct);
		for(int i = 0; i < ct; i++)
		{
			ArrayHelper.ExpandForIndex(i);
			uint8_t* el = vec->data() + (i * Property->ElementSize);
			Property->SetPropertyValue(ArrayHelper.GetRawPtr(i), (*(CppType*)el));
		}
	}

protected:
	virtual void SetProperty_InCont(void* container, void* val) override 
	{
		Property->SetPropertyValue_InContainer(container, (*(CppType*)val));
	}
};

using NOSBoolProperty = NOSNumericProperty<FBoolProperty, "bool">;
using NOSFloatProperty = NOSNumericProperty<FFloatProperty, "float">;
using NOSDoubleProperty = NOSNumericProperty<FDoubleProperty, "double">;
using NOSInt8Property = NOSNumericProperty<FInt8Property, "byte">;
using NOSInt16Property = NOSNumericProperty<FInt16Property, "short">;
using NOSIntProperty = NOSNumericProperty<FIntProperty, "int">;
using NOSInt64Property = NOSNumericProperty<FInt64Property, "long">;
using NOSByteProperty = NOSNumericProperty<FByteProperty, "ubyte">;
using NOSUInt16Property = NOSNumericProperty<FUInt16Property, "ushort">;
using NOSUInt32Property = NOSNumericProperty<FUInt32Property, "uint">;
using NOSUInt64Property = NOSNumericProperty<FUInt64Property, "ulong">;

class NOSEnumProperty : public NOSProperty
{
public:
	NOSEnumProperty(UObject* container, FEnumProperty* enumprop, FNumericProperty* numericprop,  UEnum* uenum, FString parentCategory = FString(), uint8* StructPtr = nullptr, NOSStructProperty* parentProperty = nullptr)
		: NOSProperty(container, (FProperty*)(enumprop ? (FProperty*)enumprop : (FProperty*)numericprop), parentCategory, StructPtr, parentProperty), Enum(uenum), IndexProp(numericprop), EnumProperty(enumprop)
	{
		data = std::vector<uint8_t>(1, 0); 
		TypeName = "string";

		int EnumSize = Enum->NumEnums();
		for(int i = 0; i < EnumSize; i++)
		{
			NameMap.Add(Enum->GetDisplayNameTextByIndex(i).ToString(), Enum->GetValueByIndex(i));
		}
	
		flatbuffers::FlatBufferBuilder mb;

		std::vector<std::string> NameList;
		for (const auto& [name, _] : NameMap)
		{
			NameList.push_back(TCHAR_TO_UTF8(*name));
		}

		auto offset = nos::app::CreateUpdateStringList(mb, nos::fb::CreateStringList(mb, mb.CreateString(TCHAR_TO_UTF8(*PrefixStringList(Enum->GetFName().ToString()))), mb.CreateVectorOfStrings(NameList)));
		mb.Finish(offset);

		auto buf = mb.Release();
		auto root = flatbuffers::GetRoot<nos::app::UpdateStringList>(buf.data());
		auto NOSClient = &FModuleManager::LoadModuleChecked<FNOSClient>("NOSClient");
		NOSClient->AppServiceClient->UpdateStringList(*root);
	}

	FString NodosListName;
	TMap<FString, int64> NameMap;
	FString CurrentName;
	int64 CurrentValue;
	UEnum* Enum;
	FNumericProperty* IndexProp;
	FEnumProperty* EnumProperty;
	virtual flatbuffers::Offset<nos::fb::Pin> Serialize(flatbuffers::FlatBufferBuilder& fbb) override;
	virtual flatbuffers::Offset<nos::fb::Visualizer> SerializeVisualizer(flatbuffers::FlatBufferBuilder& fbb) override;
	virtual void SetPropValue_Internal(void* val, size_t size, uint8* customContainer = nullptr) override;
	virtual std::vector<uint8> UpdatePinValue(uint8* customContainer = nullptr) override; 

};

class NOSTextProperty : public NOSProperty
{
public:
	NOSTextProperty(UObject* container, FTextProperty* uproperty, FString parentCategory = FString(), uint8* StructPtr = nullptr, NOSStructProperty* parentProperty = nullptr)
		: NOSProperty(container, uproperty, parentCategory, StructPtr, parentProperty), textprop(uproperty) 
	{
		data = std::vector<uint8_t>(1, 0);
		TypeName = "string";
	}

	FTextProperty* textprop;
	virtual void SetPropValue_Internal(void* val, size_t size, uint8* customContainer = nullptr) override;
	virtual std::vector<uint8> UpdatePinValue(uint8* customContainer = nullptr) override;

	virtual bool CreateFbArray(flatbuffers::FlatBufferBuilder& fb, FScriptArrayHelper_InContainer& ArrayHelper) override;
	virtual void SetArrayPropValues(void* val, size_t size, FScriptArrayHelper_InContainer& ArrayHelper) override;
};

class NOSNameProperty : public NOSProperty
{
public:
	NOSNameProperty(UObject* container, FNameProperty* uproperty, FString parentCategory = FString(), uint8* StructPtr = nullptr, NOSStructProperty* parentProperty = nullptr)
		: NOSProperty(container, uproperty, parentCategory, StructPtr, parentProperty), nameprop(uproperty) 
	{
		data = std::vector<uint8_t>(1, 0);
		TypeName = "string";
	}

	FNameProperty* nameprop;
	virtual void SetPropValue_Internal(void* val, size_t size, uint8* customContainer = nullptr) override;
	virtual std::vector<uint8> UpdatePinValue(uint8* customContainer = nullptr) override;
};

class NOSStringProperty : public NOSProperty
{
public:
	NOSStringProperty(UObject* container, FStrProperty* uproperty, FString parentCategory = FString(), uint8* StructPtr = nullptr, NOSStructProperty* parentProperty = nullptr)
		: NOSProperty(container, uproperty, parentCategory, StructPtr, parentProperty), stringprop(uproperty)
	{
		data = std::vector<uint8_t>(1, 0);
		TypeName = "string";
	}

	FStrProperty* stringprop;
	virtual void SetPropValue_Internal(void* val, size_t size, uint8* customContainer = nullptr) override;
	virtual std::vector<uint8> UpdatePinValue(uint8* customContainer = nullptr) override;

	virtual bool CreateFbArray(flatbuffers::FlatBufferBuilder& fb, FScriptArrayHelper_InContainer& ArrayHelper) override;
	virtual void SetArrayPropValues(void* val, size_t size, FScriptArrayHelper_InContainer& ArrayHelper) override;

};

class NOSObjectProperty : public NOSProperty
{
public:
	NOSObjectProperty(UObject* container, FObjectProperty* uproperty, FString parentCategory = FString(), uint8* StructPtr = nullptr, NOSStructProperty* parentProperty = nullptr);
	

	FObjectProperty* objectprop;
	virtual void SetPropValue_Internal(void* val, size_t size, uint8* customContainer = nullptr) override;
	virtual std::vector<uint8> UpdatePinValue(uint8* customContainer = nullptr) override;

};

class NOSStructProperty : public NOSProperty
{
public:
	NOSStructProperty(UObject* container, FStructProperty* uproperty, FString parentCategory = FString(), uint8* StructPtr = nullptr, NOSStructProperty* parentProperty = nullptr);

	FStructProperty* structprop;
	virtual void SetPropValue_Internal(void* val, size_t size, uint8* customContainer = nullptr) override;
	virtual std::vector<uint8> UpdatePinValue(uint8* customContainer = nullptr) override { return std::vector<uint8>(); }
};


class NOSArrayProperty : public NOSProperty
{
public:
	NOSArrayProperty(UObject* container, FArrayProperty* ArrayProperty, FProperty* InnerProperty, FString parentCategory = FString(), uint8* StructPtr = nullptr, NOSStructProperty* parentProperty = nullptr);
	
	FArrayProperty* ArrayProperty;
	FProperty* InnerProperty;
	
	virtual void SetPropValue_Internal(void* val, size_t size, uint8* customContainer = nullptr) override;
	virtual std::vector<uint8> UpdatePinValue(uint8* customContainer = nullptr) override;
};

template<typename T, nos::tmp::StrLiteral LitType>
class NOSCustomStructProperty : public NOSProperty 
{
public:
	NOSCustomStructProperty(UObject* container, FStructProperty* uproperty, FString parentCategory = FString(), uint8* StructPtr = nullptr, NOSStructProperty* parentProperty = nullptr)
		: NOSProperty(container, uproperty, parentCategory, StructPtr, parentProperty), structprop(uproperty)
	{
		data = std::vector<uint8_t>(sizeof(T), 0);
		TypeName = LitType.val;
	}

	FStructProperty* structprop;

	virtual bool CreateFbArray(flatbuffers::FlatBufferBuilder& fb, FScriptArrayHelper_InContainer& ArrayHelper) override
	{
		fb.StartVector(ArrayHelper.Num(), sizeof(T), 1);
		for(int i = ArrayHelper.Num()-1; i >= 0; i--)
		{
			if(auto ElementPtr = ArrayHelper.GetRawPtr(i))
			{
				fb.PushBytes((uint8_t*)ElementPtr, sizeof(T));
			}
		}
		auto offset = fb.EndVector(ArrayHelper.Num());
		fb.Finish(flatbuffers::Offset<flatbuffers::Vector<uint8_t>>(offset));
		return true;
	}

	virtual void SetArrayPropValues(void* val, size_t size, FScriptArrayHelper_InContainer& ArrayHelper) override
	{
		auto vec = (flatbuffers::Vector<u8>*)val; 
		int ct = vec->size();
		ArrayHelper.Resize(ct);
		for(int i = 0; i < ct; i++)
		{
			ArrayHelper.ExpandForIndex(i);
			uint8_t* el = vec->data() + (i * Property->ElementSize);
			structprop->CopyCompleteValue(ArrayHelper.GetRawPtr(i), (T*)el);
		}
	}


protected:
	virtual void SetProperty_InCont(void* container, void* val) override 
	{
		structprop->CopyCompleteValue(structprop->ContainerPtrToValuePtr<void>(container), (T*)val);
	}
	
};

using NOSVec2Property = NOSCustomStructProperty<FVector2D, "nos.fb.vec2d">;
using NOSVec3Property = NOSCustomStructProperty<FVector, "nos.fb.vec3d">;
using NOSVec4Property = NOSCustomStructProperty < FVector4, "nos.fb.vec4d">;
using NOSVec4FProperty = NOSCustomStructProperty < FVector4f, "nos.fb.vec4">;

class NOSRotatorProperty : public NOSProperty
{
public:
	NOSRotatorProperty(UObject* container, FStructProperty* uproperty, FString parentCategory = FString(), uint8* StructPtr = nullptr, NOSStructProperty* parentProperty = nullptr)
		: NOSProperty(container, uproperty, parentCategory, StructPtr, parentProperty), structprop(uproperty)
	{
		data = std::vector<uint8_t>(sizeof(FVector), 0);
		TypeName = "nos.fb.vec3d";
	}
	virtual std::vector<uint8> UpdatePinValue(uint8* customContainer = nullptr) override;

	FStructProperty* structprop;
	
protected:
	virtual void SetProperty_InCont(void* container, void* val) override;
};


class NOSColorProperty : public NOSProperty
{
public:
	NOSColorProperty(UObject* container, FStructProperty* uproperty, FString parentCategory = FString(), uint8* StructPtr = nullptr, NOSStructProperty* parentProperty = nullptr)
		: NOSProperty(container, uproperty, parentCategory, StructPtr, parentProperty), structprop(uproperty)
	{
		data = std::vector<uint8_t>(sizeof(FColor), 0);
		TypeName = "nos.fb.vec4u8";
	}
	virtual std::vector<uint8> UpdatePinValue(uint8* customContainer = nullptr) override;

	FStructProperty* structprop;

	virtual bool CreateFbArray(flatbuffers::FlatBufferBuilder& fb, FScriptArrayHelper_InContainer& ArrayHelper) override;
	virtual void SetArrayPropValues(void* val, size_t size, FScriptArrayHelper_InContainer& ArrayHelper) override;
	
protected:
	virtual void SetProperty_InCont(void* container, void* val) override;
};



class NOSTrackProperty : public NOSProperty
{
public:
	NOSTrackProperty(UObject* container, FStructProperty* uproperty, FString parentCategory = FString(), uint8* StructPtr = nullptr, NOSStructProperty* parentProperty = nullptr)
		: NOSProperty(container, uproperty, parentCategory, StructPtr, parentProperty), structprop(uproperty)
	{
		
		data = std::vector<uint8_t>(1, 0);
		TypeName = "nos.fb.Track";
	}
	virtual std::vector<uint8> UpdatePinValue(uint8* customContainer = nullptr) override;

	//virtual flatbuffers::Offset<nos::fb::Pin> Serialize(flatbuffers::FlatBufferBuilder& fbb) override;
	virtual void SetPropValue_Internal(void* val, size_t size, uint8* customContainer = nullptr) override;

	FStructProperty* structprop;

	virtual bool CreateFbArray(flatbuffers::FlatBufferBuilder& fb, FScriptArrayHelper_InContainer& ArrayHelper) override;
	virtual void SetArrayPropValues(void* val, size_t size, FScriptArrayHelper_InContainer& ArrayHelper) override;
	
protected:
	virtual void SetProperty_InCont(void* container, void* val) override;
};


class NOSTransformProperty : public NOSProperty
{
public:
	NOSTransformProperty(UObject* container, FStructProperty* uproperty, FString parentCategory = FString(), uint8* StructPtr = nullptr, NOSStructProperty* parentProperty = nullptr)
		: NOSProperty(container, uproperty, parentCategory, StructPtr, parentProperty), structprop(uproperty)
	{
		data = std::vector<uint8_t>(72, 0);
		TypeName = "nos.fb.Transform";
	}
	virtual std::vector<uint8> UpdatePinValue(uint8* customContainer = nullptr) override;

	virtual void SetPropValue_Internal(void* val, size_t size, uint8* customContainer = nullptr) override;

	FStructProperty* structprop;
protected:
	virtual void SetProperty_InCont(void* container, void* val) override;
};


class  NOSSCENETREEMANAGER_API  NOSPropertyFactory
{
public:
	static TSharedPtr<NOSProperty> CreateProperty(UObject* Container, 
		FProperty* UProperty, 
		FString ParentCategory = FString(), 
		uint8* StructPtr = nullptr, 
		NOSStructProperty* ParentProperty = nullptr);
};
