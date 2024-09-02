/*
 * Copyright MediaZ AS. All Rights Reserved.
 */

#pragma once
#include "CoreMinimal.h"
#include "NOSActorProperties.h"
#include "NOSActorFunctions.h"
#include <vector>
#pragma warning (disable : 4800)
#pragma warning (disable : 4668)

struct ActorNode;
struct FolderNode;
struct SceneComponentNode;

static const FName NAME_Reality_FolderName(TEXT("Reality Actors"));
static const FString HEXCOLOR_Reality_Node(TEXT("0xFE5000"));

struct NOSSCENETREEMANAGER_API  TreeNode : public TSharedFromThis<TreeNode> {

	virtual ActorNode* GetAsActorNode() { return nullptr; };
	virtual FolderNode* GetAsFolderNode() { return nullptr; };
	virtual SceneComponentNode* GetAsSceneComponentNode() { return nullptr; };
	virtual FString GetClassDisplayName() = 0;
	
	FString Name;
	TreeNode* Parent;
	FGuid Id;
	bool NeedsReload = true;
	std::vector<TSharedPtr<TreeNode>> Children;
	TMap<FString, FString> nosMetaData;

	
	virtual flatbuffers::Offset<nos::fb::Node> Serialize(flatbuffers::FlatBufferBuilder& fbb);
	std::vector<flatbuffers::Offset<nos::fb::Node>> SerializeChildren(flatbuffers::FlatBufferBuilder& fbb);
	std::vector<flatbuffers::Offset<nos::fb::MetaDataEntry>> SerializeMetaData(flatbuffers::FlatBufferBuilder& fbb);

	virtual ~TreeNode();
};

struct NOSSCENETREEMANAGER_API  ActorNode : TreeNode
{
	NOSActorReference actor;
	std::vector<TSharedPtr<NOSProperty>> Properties;
	std::vector<TSharedPtr<NOSFunction>> Functions;
	virtual FString GetClassDisplayName() override { return actor ? actor->GetClass()->GetFName().ToString() : "Actor"; };
	virtual ActorNode* GetAsActorNode() override { return this; };
	virtual flatbuffers::Offset<nos::fb::Node> Serialize(flatbuffers::FlatBufferBuilder& fbb) override;
	std::vector<flatbuffers::Offset<nos::fb::Pin>> SerializePins(flatbuffers::FlatBufferBuilder& fbb);
	
	virtual ~ActorNode();
};

struct NOSSCENETREEMANAGER_API  SceneComponentNode : TreeNode
{
	NOSComponentReference sceneComponent;
	std::vector<TSharedPtr<NOSProperty>> Properties;
	virtual FString GetClassDisplayName() override { return sceneComponent ? sceneComponent->GetClass()->GetFName().ToString() : FString("ActorComponent"); };
	virtual SceneComponentNode* GetAsSceneComponentNode() override { return this; };
	virtual flatbuffers::Offset<nos::fb::Node> Serialize(flatbuffers::FlatBufferBuilder& fbb) override;
	std::vector<flatbuffers::Offset<nos::fb::Pin>> SerializePins(flatbuffers::FlatBufferBuilder& fbb);

	virtual ~SceneComponentNode();
};

struct NOSSCENETREEMANAGER_API  FolderNode : TreeNode
{
	virtual FString GetClassDisplayName() override { return FString("Folder"); };
	virtual FolderNode* GetAsFolderNode() override { return this; };

	virtual ~FolderNode();
};

class NOSSCENETREEMANAGER_API NOSSceneTree
{
public:
	NOSSceneTree();

	TSharedPtr<TreeNode> Root;
	bool IsSorted = false;

	TSharedPtr<FolderNode> FindOrAddChildFolder(TSharedPtr<TreeNode> node, FString name, TSharedPtr<TreeNode>& mostRecentParent);
	TSharedPtr<ActorNode> AddActor(FString folderPath, AActor* actor);
	TSharedPtr<ActorNode> AddActor(::FString folderPath, ::AActor* actor, TSharedPtr<TreeNode>& mostRecentParent);
	TSharedPtr<ActorNode> AddActor(TreeNode* parent, AActor* actor);
	TSharedPtr<SceneComponentNode> AddSceneComponent(ActorNode* parent, USceneComponent* sceneComponent);
	TSharedPtr<SceneComponentNode> AddSceneComponent(TSharedPtr<SceneComponentNode> parent, USceneComponent* sceneComponent);
	ActorNode* GetNode(AActor* Actor);
	ActorNode* GetNodeFromActorId(FGuid ActorId);
	FGuid      GetNodeIdActorId(FGuid ActorId);
	TreeNode* GetNode(FGuid NodeId);
	void RemoveNode(FGuid NodeId);
	TreeNode* GetFolderOrRoot(TreeNode* node);

	void Clear();

private:
	TMap<FGuid, TSharedPtr<TreeNode>> NodeMap;
	TMap<FGuid, FGuid> ActorIdToNodeId;
	void ClearRecursive(TSharedPtr<TreeNode> node);
};
