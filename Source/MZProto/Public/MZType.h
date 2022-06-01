#pragma once

#include "IMZProto.h"
#include "CoreMinimal.h"
#include "RemoteControlField.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "UObject/Field.h"

#include <memory>
#include <string>

namespace mz::app
{
	class AddPinRequest;
	class NodeUpdateRequest;
}

namespace mz::proto
{
	class Dynamic;
}

struct MZPROTO_API MZType
{
	enum
	{
		BOOL,
		STRING,
		INT,
		FLOAT,
		ARRAY,
		STRUCT,
	} Tag;

	std::string TypeName;

	//Scalar
	uint32_t Width = 0;

	// Array
	MZType* ElementType = 0;
	uint32_t ElementCount = 0;

	//Struct
	TMap<FString, MZType*> StructFields;

	static MZType* GetType(FField*);
	void SerializeToProto(mz::proto::Dynamic* dyn, IRemoteControlPropertyHandle* p);
private:
	MZType() = default;
	bool Init(FField*);
};

struct MZPROTO_API MZEntity
{
	MZType* Type;
	FRemoteControlEntity* Entity;
	TSharedPtr<IRemoteControlPropertyHandle> Property;
	//void SerializeToProto(google::protobuf::Any* value);
	//void SerializeToProto(mz::proto::DynamicField* field);
	void SerializeToProto(mz::proto::Dynamic* req);
};
