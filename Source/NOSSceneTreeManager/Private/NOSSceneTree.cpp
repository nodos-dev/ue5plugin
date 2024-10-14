// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include "NOSSceneTree.h"
#include "NOSSceneTreeManager.h"

NOSSceneTree::NOSSceneTree()
{
	Root = TSharedPtr<FolderNode>(new FolderNode);
	Root->nosMetaData.Add("PinnedCategories", "Control");
	Root->nosMetaData.Add("PinnedNodes", "Reality Actors");
	Root->Name = FNOSClient::AppKey;
	Root->Parent = nullptr;
}

TSharedPtr<FolderNode> NOSSceneTree::FindOrAddChildFolder(TSharedPtr<TreeNode> node, FString name, TSharedPtr<TreeNode>& mostRecentParent)
{
	for (auto child : node->Children)
	{
		if (child->Name == name && child->GetAsFolderNode())
		{
			return StaticCastSharedPtr<FolderNode>(child);
		}
	}
	TSharedPtr<FolderNode> newChild(new FolderNode);
	newChild->Parent = node.Get();
	newChild->Name = name;
	if(name == NAME_Reality_FolderName.ToString())
	{
		newChild->nosMetaData.Add("NodeColor", HEXCOLOR_Reality_Node);	
	}
	newChild->Id = FGuid::NewGuid();
	node->Children.push_back(newChild);
	NodeMap.Add(newChild->Id, newChild);
	if (!mostRecentParent)
	{
		mostRecentParent = newChild;
	}
	return newChild;
}



void NOSSceneTree::Clear()
{
	ClearRecursive(Root);
	NodeMap.Empty();
	ActorIdToNodeId.Empty();
	NodeMap.Add(Root->Id, Root);
}

void NOSSceneTree::ClearRecursive(TSharedPtr<TreeNode> node)
{
	for (auto child : node->Children)
	{
		ClearRecursive(child);
	}
	if (node == Root)
	{
		Root->Children.clear();
		return;
	}
}

TSharedPtr<ActorNode> NOSSceneTree::AddActor(FString folderPath, AActor* actor)
{
	TSharedPtr<TreeNode> mostRecentParent;
	return AddActor(folderPath, actor, mostRecentParent);
}

TSharedPtr<ActorNode> NOSSceneTree::AddActor(FString folderPath, AActor* actor, TSharedPtr<TreeNode>& mostRecentParent)
{
	if (!actor)
	{
		return nullptr;
	}

	folderPath.RemoveFromStart(FString("None"));
	folderPath.RemoveFromStart(FString("/"));
	TArray<FString> folders;
	folderPath.ParseIntoArray(folders, TEXT("/"));
	
	TSharedPtr<TreeNode> ptr = Root;
	for (auto item : folders)
	{
		ptr = FindOrAddChildFolder(ptr, item, mostRecentParent);
	}

	auto newChild = AddActor(ptr->GetAsFolderNode(), actor);
	if (!mostRecentParent)
	{
		mostRecentParent = newChild;
	}
	return newChild;
}

TSharedPtr<ActorNode> NOSSceneTree::AddActor(TreeNode* parent, AActor* actor)
{
	if (!actor)
	{
		return nullptr;
	}
	if (!parent)
	{
		parent = Root.Get();
	}

	TSharedPtr<ActorNode> newChild(new ActorNode);
	newChild->Parent = parent;

	if (FNOSSceneTreeManager::daWorld->PersistentLevel == actor->GetLevel())
	{
		// TODO: Shouldn't this be actor->GetFName().ToString()?
		newChild->Name = actor->GetActorLabel();
		newChild->Id = StringToFGuid(actor->GetFName().ToString());
	}
	else
	{
		newChild->Name = actor->GetLevel()->GetOuter()->GetFName().ToString() + actor->GetFName().ToString();
		newChild->Id = StringToFGuid(actor->GetFullName(actor->GetWorld()));
	}
	newChild->actor = NOSActorReference(actor);
	newChild->NeedsReload = true;
	parent->Children.push_back(newChild);
	NodeMap.Add(newChild->Id, newChild);
	ActorIdToNodeId.Add(actor->GetActorGuid(), newChild->Id);
	newChild->nosMetaData.Add(NosMetadataKeys::PinnedCategories, "Transform");
	
	if (actor->GetRootComponent())
	{
		TSharedPtr<SceneComponentNode> loadingChild(new SceneComponentNode);
		loadingChild->Name = "Loading";
		loadingChild->Id = FGuid::NewGuid();
		loadingChild->Parent = newChild.Get();
		newChild->Children.push_back(loadingChild);
	}

	return newChild;
}

TSharedPtr<SceneComponentNode> NOSSceneTree::AddSceneComponent(ActorNode* parent, USceneComponent* sceneComponent)
{
	TSharedPtr<SceneComponentNode>newComponentNode(new SceneComponentNode);
	newComponentNode->nosMetaData.Add(NosMetadataKeys::PinnedCategories, "Transform");
	newComponentNode->sceneComponent = NOSComponentReference(sceneComponent);
	FString ActorUniqueName = parent->actor->GetFName().ToString();
	FString ComponentName = sceneComponent->GetFName().ToString();
	newComponentNode->Name = sceneComponent->GetFName().ToString();
	if (FNOSSceneTreeManager::daWorld->PersistentLevel == sceneComponent->GetComponentLevel())
	{
		newComponentNode->Id = StringToFGuid(ActorUniqueName + ComponentName);
	}
	else
	{
		newComponentNode->Id = StringToFGuid(sceneComponent->GetFullName(sceneComponent->GetWorld()));
	}

	newComponentNode->Parent = parent;
	newComponentNode->NeedsReload = true;
	parent->Children.push_back(newComponentNode);
	NodeMap.Add(newComponentNode->Id, newComponentNode);

	TSharedPtr<SceneComponentNode> loadingChild(new SceneComponentNode);
	loadingChild->Name = "Loading";
	loadingChild->Id = FGuid::NewGuid();
	loadingChild->Parent = newComponentNode.Get();
	newComponentNode->Children.push_back(loadingChild);

	return newComponentNode;
}

TSharedPtr<SceneComponentNode> NOSSceneTree::AddSceneComponent(TSharedPtr<SceneComponentNode> parent, USceneComponent* sceneComponent)
{
	TSharedPtr<SceneComponentNode> newComponentNode(new SceneComponentNode);
	newComponentNode->nosMetaData.Add(NosMetadataKeys::PinnedCategories, "Transform");
	newComponentNode->sceneComponent = NOSComponentReference(sceneComponent);
	newComponentNode->Name = sceneComponent->GetFName().ToString();
	if (FNOSSceneTreeManager::daWorld->PersistentLevel == sceneComponent->GetComponentLevel())
	{
		FString ActorUniqueName;
		if(auto actor = sceneComponent->GetAttachParentActor())
		{
			ActorUniqueName = actor->GetFName().ToString();
		}
		FString ComponentName = sceneComponent->GetFName().ToString();
		newComponentNode->Id = StringToFGuid(ActorUniqueName + ComponentName);
	}
	else
	{
		newComponentNode->Id = StringToFGuid(sceneComponent->GetFullName(sceneComponent->GetWorld()));
	}
	newComponentNode->Parent = parent.Get();
	newComponentNode->NeedsReload = true;
	parent->Children.push_back(newComponentNode);
	NodeMap.Add(newComponentNode->Id, newComponentNode);

	TSharedPtr<SceneComponentNode> loadingChild(new SceneComponentNode);
	loadingChild->Name = "Loading";
	loadingChild->Id = FGuid::NewGuid();
	loadingChild->Parent = newComponentNode.Get();
	newComponentNode->Children.push_back(loadingChild);

	return newComponentNode;
}

ActorNode* NOSSceneTree::GetNode(AActor* Actor)
{
	if(!Actor)
		return  nullptr;
	return GetNodeFromActorId(Actor->GetActorGuid());
}

ActorNode* NOSSceneTree::GetNodeFromActorId(FGuid ActorId)
{
	if(!ActorIdToNodeId.Contains(ActorId))
		return  nullptr;
	
	auto NodeId = ActorIdToNodeId.FindRef(ActorId);
	if(!NodeMap.Contains(NodeId))
		return nullptr;
	
	auto Node = NodeMap.FindRef(NodeId);
	return Node->GetAsActorNode();
}

FGuid NOSSceneTree::GetNodeIdActorId(FGuid ActorId)
{
	if(!ActorIdToNodeId.Contains(ActorId))
		return {};
	
	return ActorIdToNodeId.FindRef(ActorId);
}

TreeNode* NOSSceneTree::GetNode(FGuid NodeId)
{
	if(!NodeMap.Contains(NodeId))
		return nullptr;
	
	auto Node = NodeMap.FindRef(NodeId);
	return Node.Get();
}

void NOSSceneTree::RemoveNode(FGuid NodeId)
{
	NodeMap.Remove(NodeId);
}

TreeNode* NOSSceneTree::GetFolderOrRoot(TreeNode* node)
{
	if (node->GetAsFolderNode())
	{
		return node;
	}
	if (node->Parent)
	{
		return GetFolderOrRoot(node->Parent);
	}
	return Root.Get();
}

flatbuffers::Offset<nos::fb::Node> TreeNode::Serialize(flatbuffers::FlatBufferBuilder& fbb)
{
	std::vector<flatbuffers::Offset<nos::fb::MetaDataEntry>> metadata = SerializeMetaData(fbb);
	std::vector<flatbuffers::Offset<nos::fb::Node>> childNodes = SerializeChildren(fbb);
	return nos::fb::CreateNodeDirect(fbb, (nos::fb::UUID*)&Id, TCHAR_TO_UTF8(*Name), TCHAR_TO_UTF8(*GetClassDisplayName()), false, true, 0, 0, nos::fb::NodeContents::Graph, nos::fb::CreateGraphDirect(fbb, &childNodes).Union(), TCHAR_TO_ANSI(*FNOSClient::AppKey), 0, 0, 0, 0, &metadata);
}

flatbuffers::Offset<nos::fb::Node> ActorNode::Serialize(flatbuffers::FlatBufferBuilder& fbb)
{
	std::vector<flatbuffers::Offset<nos::fb::MetaDataEntry>> metadata = SerializeMetaData(fbb);
	std::vector<flatbuffers::Offset<nos::fb::Node>> childNodes = SerializeChildren(fbb);
	std::vector<flatbuffers::Offset<nos::fb::Pin>> pins = SerializePins(fbb);
	return nos::fb::CreateNodeDirect(fbb, (nos::fb::UUID*)&Id, TCHAR_TO_UTF8(*Name), TCHAR_TO_UTF8(*GetClassDisplayName()), false, true, &pins, 0, nos::fb::NodeContents::Graph, nos::fb::CreateGraphDirect(fbb, &childNodes).Union(), TCHAR_TO_ANSI(*FNOSClient::AppKey), 0, 0, 0, 0, &metadata, 0, 0, TCHAR_TO_UTF8(*actor->GetActorLabel()));
}

std::vector<flatbuffers::Offset<nos::fb::Pin>> ActorNode::SerializePins(flatbuffers::FlatBufferBuilder& fbb)
{
	std::vector<flatbuffers::Offset<nos::fb::Pin>> pins;
	for (auto nosprop : Properties)
	{
		pins.push_back(nosprop->Serialize(fbb));
	}
	return pins;
}

ActorNode::~ActorNode()
{
}

flatbuffers::Offset<nos::fb::Node> SceneComponentNode::Serialize(flatbuffers::FlatBufferBuilder& fbb)
{
	std::vector<flatbuffers::Offset<nos::fb::MetaDataEntry>> metadata = SerializeMetaData(fbb);
	std::vector<flatbuffers::Offset<nos::fb::Node>> childNodes = SerializeChildren(fbb);
	std::vector<flatbuffers::Offset<nos::fb::Pin>> pins = SerializePins(fbb);
	return nos::fb::CreateNodeDirect(fbb, (nos::fb::UUID*)&Id, TCHAR_TO_UTF8(*Name), TCHAR_TO_UTF8(*GetClassDisplayName()), false, true, &pins, 0, nos::fb::NodeContents::Graph, nos::fb::CreateGraphDirect(fbb, &childNodes).Union(), TCHAR_TO_ANSI(*FNOSClient::AppKey), 0, 0, 0, 0, &metadata);
}

std::vector<flatbuffers::Offset<nos::fb::Pin>> SceneComponentNode::SerializePins(flatbuffers::FlatBufferBuilder& fbb)
{
	std::vector<flatbuffers::Offset<nos::fb::Pin>> pins;
	for (auto nosprop : Properties)
	{
		pins.push_back(nosprop->Serialize(fbb));
	}
	return pins;
}

std::vector<flatbuffers::Offset<nos::fb::Node>> TreeNode::SerializeChildren(flatbuffers::FlatBufferBuilder& fbb)
{
	std::vector<flatbuffers::Offset<nos::fb::Node>> childNodes;

	if (Children.empty())
	{
		return childNodes;
	}

	for (auto child : Children)
	{
		childNodes.push_back(child->Serialize(fbb));
	}

	return childNodes;
}

std::vector<flatbuffers::Offset<nos::fb::MetaDataEntry>> TreeNode::SerializeMetaData(flatbuffers::FlatBufferBuilder& fbb)
{
	std::vector<flatbuffers::Offset<nos::fb::MetaDataEntry>> metadata;
	for (auto [key, value] : nosMetaData)
	{
		metadata.push_back(nos::fb::CreateMetaDataEntryDirect(fbb, TCHAR_TO_UTF8(*key), TCHAR_TO_UTF8(*value)));
	}
	return metadata;
}

TreeNode::~TreeNode()
{
}


SceneComponentNode::~SceneComponentNode()
{
}

FolderNode::~FolderNode()
{
}


