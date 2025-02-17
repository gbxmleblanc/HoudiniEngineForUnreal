#include "HoudiniDataTableTranslator.h"

#include "HoudiniApi.h"
#include "HoudiniEngine.h"
#include "HoudiniEngineString.h"
#include "HoudiniEngineUtils.h"
#include "HoudiniPackageParams.h"

#include "EdGraph/EdGraphPin.h"
#include "Engine/DataTable.h"
#include "Kismet2/StructureEditorUtils.h"
#include "UObject/TextProperty.h"
#include "UObject/UObjectGlobals.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"

namespace
{
	template<typename T, typename P>
	void
	WriteAttributeDataToStruct(const T* AttrData,
		int32 Offset,
		int32 RowSize,
		int32 NumRows,
		int32 TupleSize,
		uint8* PropData)
	{
		for (int32 Idx = 0; Idx < NumRows; ++Idx)
		{
			for (int32 CompIdx = 0; CompIdx < TupleSize; ++CompIdx)
			{
				int32 DataIdx = Idx * RowSize + CompIdx * sizeof(T) + Offset;
				P::SetPropertyValue(&PropData[DataIdx], AttrData[Idx * TupleSize + CompIdx]);
			}
		}
	}

	template<typename T, typename P, typename W>
	void
	WriteAttributeDataToStruct(const void* AttrData,
		int32 Offset,
		int32 RowSize,
		int32 NumRows,
		int32 TupleSize,
		uint8* PropData,
		const TArray<uint32>& Order)
	{
		// Account for type conversions - ie Houdini type is int64 but the found
		// struct property is float32. Needs an explicit conversion.
		const T* CastedData = static_cast<const T*>(AttrData);

		for (int32 Idx = 0; Idx < NumRows; ++Idx)
		{
			for (int32 CompIdx = 0; CompIdx < TupleSize; ++CompIdx)
			{
				int32 DataIdx = Idx * RowSize + Order[CompIdx] * sizeof(P) + Offset;
				W::SetPropertyValue(&PropData[DataIdx], static_cast<const P>(CastedData[Idx * TupleSize + CompIdx]));
			}
		}
	}

	template<typename T, typename P, typename W>
	void
	WriteAttributeDataToStruct(const void* AttrData,
		uint32 Offset,
		uint32 RowSize,
		uint32 NumRows,
		uint32 TupleSize,
		uint8* PropData)
	{
		TArray<uint32> Order;
		Order.Reserve(TupleSize);
		for (uint32 Idx = 0; Idx < TupleSize; ++Idx)
		{
			Order.Add(Idx);
		}
		return WriteAttributeDataToStruct<T, P, W>(AttrData,
			Offset,
			RowSize,
			NumRows,
			TupleSize,
			PropData,
			Order);
	}

	template<typename T>
	bool
	WriteAttributeDataToStruct(const void* AttrData,
		uint32 RowSize,
		const HAPI_AttributeInfo& AttribInfo,
		uint8* PropData,
		FProperty* Prop,
		// Take in the attrib name for the transforms special case
		const FString& AttribName)
	{
		uint32 NumRows = AttribInfo.count;
		uint32 TupleSize = AttribInfo.tupleSize;
		uint32 Offset = Prop->GetOffset_ForInternal();
		if (Prop->IsA<FBoolProperty>())
		{
			const T* CastedData = static_cast<const T*>(AttrData);
			FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop);

			for (uint32 Idx = 0; Idx < NumRows; ++Idx)
			{
				for (uint32 CompIdx = 0; CompIdx < TupleSize; ++CompIdx)
				{
					uint32 DataIdx = Idx * RowSize + CompIdx * BoolProp->GetSize() + Offset;
					BoolProp->SetPropertyValue(&PropData[DataIdx], static_cast<const bool>(CastedData[Idx * TupleSize + CompIdx]));
				}
			}
		}
		else if (Prop->IsA<FByteProperty>())
		{
			WriteAttributeDataToStruct<T, uint8, FByteProperty>(AttrData,
				Offset,
				RowSize,
				NumRows,
				TupleSize,
				PropData);
		}
		else if (Prop->IsA<FDoubleProperty>())
		{
			WriteAttributeDataToStruct<T, double, FDoubleProperty>(AttrData,
				Offset,
				RowSize,
				NumRows,
				TupleSize,
				PropData);
		}
		else if (Prop->IsA<FFloatProperty>())
		{
			WriteAttributeDataToStruct<T, float, FFloatProperty>(AttrData,
				Offset,
				RowSize,
				NumRows,
				TupleSize,
				PropData);
		}
		else if (Prop->IsA<FInt16Property>())
		{
			WriteAttributeDataToStruct<T, int16, FInt16Property>(AttrData,
				Offset,
				RowSize,
				NumRows,
				TupleSize,
				PropData);
		}
		else if (Prop->IsA<FInt64Property>())
		{
			WriteAttributeDataToStruct<T, int64, FInt64Property>(AttrData,
				Offset,
				RowSize,
				NumRows,
				TupleSize,
				PropData);
		}
		else if (Prop->IsA<FInt8Property>())
		{
			WriteAttributeDataToStruct<T, int8, FInt8Property>(AttrData,
				Offset,
				RowSize,
				NumRows,
				TupleSize,
				PropData);
		}
		else if (Prop->IsA<FIntProperty>())
		{
			WriteAttributeDataToStruct<T, int32, FIntProperty>(AttrData,
				Offset,
				RowSize,
				NumRows,
				TupleSize,
				PropData);
		}
		else if (Prop->IsA<FUInt16Property>())
		{
			WriteAttributeDataToStruct<T, uint16, FUInt16Property>(AttrData,
				Offset,
				RowSize,
				NumRows,
				TupleSize,
				PropData);
		}
		else if (Prop->IsA<FUInt32Property>())
		{
			WriteAttributeDataToStruct<T, uint32, FUInt32Property>(AttrData,
				Offset,
				RowSize,
				NumRows,
				TupleSize,
				PropData);
		}
		else if (Prop->IsA<FUInt64Property>())
		{
			WriteAttributeDataToStruct<T, uint64, FUInt64Property>(AttrData,
				Offset,
				RowSize,
				NumRows,
				TupleSize,
				PropData);
		}
		else if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			FName StructName = StructProp->Struct->GetFName();
			// If the struct prop is a tuple of floats
			if (StructName == NAME_Vector4f || StructName == NAME_LinearColor || StructName == NAME_Vector3f || StructName == NAME_Vector2f)
			{
				WriteAttributeDataToStruct<T, float, FFloatProperty>(AttrData,
					Offset,
					RowSize,
					NumRows,
					TupleSize,
					PropData);
			}
			else if (StructName == NAME_Rotator3f)
			{
				WriteAttributeDataToStruct<T, float, FFloatProperty>(AttrData,
					Offset,
					RowSize,
					NumRows,
					TupleSize,
					PropData,
					{ 1, 0, 2 });
			}
			// Tuple of doubles
			else if (StructName == NAME_Vector4d || StructName == NAME_Vector4 || StructName == NAME_Vector || StructName == NAME_Vector3d || StructName == NAME_Vector2d || StructName == NAME_Vector2D)
			{
				WriteAttributeDataToStruct<T, double, FDoubleProperty>(AttrData,
					Offset,
					RowSize,
					NumRows,
					TupleSize,
					PropData);
			}
			else if (StructName == NAME_Rotator || StructName == NAME_Rotator3d)
			{
				WriteAttributeDataToStruct<T, double, FDoubleProperty>(AttrData,
					Offset,
					RowSize,
					NumRows,
					TupleSize,
					PropData,
					{ 1, 0, 2 });
			}
			else if (StructName == NAME_Color)
			{
				WriteAttributeDataToStruct<T, uint8, FByteProperty>(AttrData,
					Offset,
					RowSize,
					NumRows,
					TupleSize,
					PropData,
					{ 2, 1, 0, 3 });
			}
			else if (StructName == NAME_Transform3d || StructName == NAME_Transform)
			{
				if (AttribName.EndsWith("_rotation"))
				{
					WriteAttributeDataToStruct<T, double, FDoubleProperty>(AttrData,
						Offset,
						RowSize,
						NumRows,
						TupleSize,
						PropData);
				}
				else if (AttribName.EndsWith("_translate"))
				{
					WriteAttributeDataToStruct<T, double, FDoubleProperty>(AttrData,
						Offset + 32,
						RowSize,
						NumRows,
						TupleSize,
						PropData);
				}
				else
				{
					WriteAttributeDataToStruct<T, double, FDoubleProperty>(AttrData,
						Offset + 64,
						RowSize,
						NumRows,
						TupleSize,
						PropData);
				}
			}
			else if (StructName == NAME_Transform3f)
			{
				if (AttribName.EndsWith("_rotation"))
				{
					WriteAttributeDataToStruct<T, float, FFloatProperty>(AttrData,
						Offset,
						RowSize,
						NumRows,
						TupleSize,
						PropData);
				}
				else if (AttribName.EndsWith("_translate"))
				{
					WriteAttributeDataToStruct<T, float, FFloatProperty>(AttrData,
						Offset + 16,
						RowSize,
						NumRows,
						TupleSize,
						PropData);
				}
				else
				{
					WriteAttributeDataToStruct<T, float, FFloatProperty>(AttrData,
						Offset + 32,
						RowSize,
						NumRows,
						TupleSize,
						PropData);
				}
			}
		}
		else
		{
			bool IsFloat = TIsSame<T, float>::Value || TIsSame<T, double>::Value;
			const T* CastedData = static_cast<const T*>(AttrData);
			if (Prop->IsA<FStrProperty>() || Prop->IsA<FTextProperty>() || Prop->IsA<FNameProperty>())
			{
				for (uint32 Idx = 0; Idx < NumRows; ++Idx)
				{
					if (TupleSize == 1)
					{
						FString Str = IsFloat ? FString::SanitizeFloat(CastedData[Idx]) : FString::FromInt(CastedData[Idx]);
						Prop->ImportText(*Str, &PropData[Idx * RowSize + Offset], PPF_ExternalEditor, nullptr);
						continue;
					}
					FString Str = "(";
					for (uint32 CompIdx = 0; CompIdx < TupleSize; ++CompIdx)
					{
						Str.Append(IsFloat ? FString::SanitizeFloat(CastedData[Idx * TupleSize + CompIdx]) : FString::FromInt(CastedData[Idx * TupleSize + CompIdx]));
						if (CompIdx != TupleSize - 1)
						{
							Str.Append(",");
						}
					}
					Str.Append(")");
					Prop->ImportText(*Str, &PropData[Idx * RowSize + Offset], PPF_ExternalEditor, nullptr);
				}
			}
			else
			{
				// Error
				return false;
			}
		}
		return true;
	}

};

bool
FHoudiniDataTableTranslator::GenerateRowNames(int32 GeoId, int32 PartId, int32 NumRows, TArray<FString>& RowNames)
{
	HAPI_AttributeInfo AttribInfo;

	RowNames.Reserve(NumRows);
	bool Status = FHoudiniEngineUtils::HapiGetAttributeDataAsString(GeoId, PartId, HAPI_UNREAL_ATTRIB_DATA_TABLE_ROWNAME, AttribInfo, RowNames);
	if (!Status || RowNames.Num() == 0)
	{
		RowNames.Add("NewRow");
		for (int32 Idx = 0; Idx < NumRows - 1; ++Idx)
		{
			RowNames.Add(FString("NewRow_") + FString::FromInt(Idx));
		}
	}
	return Status;
}

bool
FHoudiniDataTableTranslator::SetOutputPath(int32 GeoId,
	int32 PartId,
	HAPI_AttributeInfo& AttribInfo,
	FString& DataTableName,
	FString& DataTableFolder)
{
	TArray<FString> DTNameHolder;
	bool Status = FHoudiniEngineUtils::HapiGetAttributeDataAsString(GeoId, PartId, HAPI_UNREAL_ATTRIB_OBJECT_PATH, AttribInfo, DTNameHolder);

	if (Status && AttribInfo.exists && AttribInfo.storage == HAPI_STORAGETYPE_STRING && DTNameHolder.Num())
	{
		const FString Folder = FPaths::GetPath(DTNameHolder[0]);
		if (!FHoudiniEngineUtils::DoesFolderExist(Folder))
		{
			return false;
		}
		DataTableName = FPaths::GetBaseFilename(DTNameHolder[0], true);
		DataTableFolder = Folder;
	}
	return Status;
}

bool
FHoudiniDataTableTranslator::GetAttribNames(int32 GeoId,
	int32 PartId,
	int32 NumAttributes,
	HAPI_AttributeOwner AttribOwner,
	TArray<FString>& AttribNames)
{
	// Get the list of attribute names
	TArray<HAPI_StringHandle> AttribNameHandles;
	AttribNameHandles.SetNum(NumAttributes);
	HAPI_Result Error = FHoudiniApi::GetAttributeNames(
		FHoudiniEngine::Get().GetSession(),
		GeoId, PartId, AttribOwner,
		AttribNameHandles.GetData(), NumAttributes);
	if (Error != HAPI_RESULT_SUCCESS)
	{
		HOUDINI_LOG_WARNING(TEXT("[FHoudiniDataTableTranslator::GetAttribNames]: Error %d when trying to get list of attribute names."), Error);
	}

	FHoudiniEngineString::SHArrayToFStringArray(AttribNameHandles, AttribNames);

	return !Error;
}

bool
FHoudiniDataTableTranslator::GetRowStructAttrib(int32 GeoId,
	int32 PartId,
	HAPI_AttributeInfo& AttribInfo,
	UScriptStruct*& RowStruct,
	FString& RowStructName)
{
	TArray<FString> StructPathHolder;
	bool Status = FHoudiniEngineUtils::HapiGetAttributeDataAsString(GeoId, PartId, HAPI_UNREAL_ATTRIB_DATA_TABLE_ROWSTRUCT, AttribInfo, StructPathHolder);

	if (Status && AttribInfo.exists && AttribInfo.storage == HAPI_STORAGETYPE_STRING && StructPathHolder.Num())
	{
		RowStructName = StructPathHolder[0];
		RowStruct = LoadObject<UScriptStruct>(UScriptStruct::StaticClass(), *(FString("'") + RowStructName + FString("'")));
	}
	return Status;
}

bool
FHoudiniDataTableTranslator::CreateRowStruct(const FHoudiniGeoPartObject& HGPO,
	const FString& RowStructName,
	UUserDefinedStruct*& NewStruct,
	FGuid& DefaultPropId)
{
	FHoudiniPackageParams RSPackageParams;
	RSPackageParams.ObjectId = HGPO.ObjectId;
	RSPackageParams.GeoId = HGPO.GeoId;
	RSPackageParams.PartId = HGPO.PartId;
	// If no name specified, one will be automatically generated.
	if (RowStructName != "")
	{
		FString RowStructFile = FPaths::GetBaseFilename(RowStructName, true);
		FString RowStructPath = FPaths::GetPath(RowStructName);
		RSPackageParams.ObjectName = RowStructFile;
		RSPackageParams.NameOverride = RowStructFile;
		RSPackageParams.FolderOverride = RowStructPath;
		RSPackageParams.OverideEnabled = true;
	}
	else
	{
		RSPackageParams.ObjectName = MakeUniqueObjectName(nullptr, UScriptStruct::StaticClass()).ToString();
	}
	FString PackageName = "";
	UPackage* Pkg = RSPackageParams.CreatePackageForObject(PackageName);
	if (!Pkg)
	{
		HOUDINI_LOG_WARNING(TEXT("[FHoudiniDataTableTranslator::CreateRowStruct]: Failed to create package."));
		return false;
	}
	if (RSPackageParams.OverideEnabled && !FHoudiniEngineUtils::DoesFolderExist(RSPackageParams.FolderOverride))
	{
		HOUDINI_LOG_WARNING(TEXT("[FHoudiniDataTableTranslator::CreateRowStruct]: Invalid package path %s."), *RowStructName);
		return false;
	}

	NewStruct = FStructureEditorUtils::CreateUserDefinedStruct(Pkg, FName(PackageName), RF_Standalone | RF_Public);
	TFieldIterator<FProperty> It(NewStruct);
	DefaultPropId = FStructureEditorUtils::GetGuidForProperty(*It);

	return true;
}

bool
FHoudiniDataTableTranslator::PopulatePropList(const UScriptStruct* RowStruct,
	TMap<FString, FProperty*>& ActualProps,
	TMap<FString, int32>& PropIndices)
{
	ActualProps.Reserve(RowStruct->GetPropertiesSize());
	PropIndices.Reserve(RowStruct->GetPropertiesSize());
	int32 Idx = 1;
	for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
	{
		FString NiceName = It->GetAuthoredName();
		NiceName = NiceName.Replace(TEXT(" "), TEXT("_"));
		NiceName = NiceName.Replace(TEXT(":"), TEXT("_"));
		// These maps only used to match attrib names with struct props when the struct already exists
		ActualProps.Add(NiceName, *It);
		PropIndices.Add(NiceName, Idx++);
	}

	return true;
}

bool
FHoudiniDataTableTranslator::FindTransformAttribs(const TArray<FString>& AttribNames,
	TMap<FString, TransformComponents>& TransformCandidates,
	TSet<FString>& TransformAttribs)
{
	for (const FString& AttribName : AttribNames)
	{
		if (AttribName.StartsWith(HAPI_UNREAL_ATTRIB_DATA_TABLE_PREFIX) &&
			(AttribName.EndsWith("_rotation") ||
				AttribName.EndsWith("_translate") ||
				AttribName.EndsWith("_scale")))
		{
			int32 Start = FString(HAPI_UNREAL_ATTRIB_DATA_TABLE_PREFIX).Len();
			int32 End;
			AttribName.FindLastChar('_', End);
			FString RawName = AttribName.Mid(Start, End - Start);
			FString Component = AttribName.Mid(End + 1);
			FString FullName = AttribName.Mid(Start);
			if (!TransformCandidates.Contains(RawName))
			{
				TransformCandidates.Add(RawName, TransformComponents());
			}
			TransformComponents& Comps = TransformCandidates[RawName];
			if (Component == "rotation")
			{
				Comps.Rotation = AttribName;
			}
			else if (Component == "translate")
			{
				Comps.Translate = AttribName;
			}
			else
			{
				Comps.Scale = AttribName;
			}

			if (Comps.Rotation != "" && Comps.Translate != "" && Comps.Scale != "")
			{
				TransformAttribs.Add(RawName);
			}
		}
	}

	return true;
}

bool
FHoudiniDataTableTranslator::CreateTransformProp(const FString& TAttrib,
	UUserDefinedStruct* NewStruct,
	int32& AssignedIdx,
	TMap<FString, FGuid>& CreatedIds)
{
	bool Status = FStructureEditorUtils::AddVariable(NewStruct, FEdGraphPinType("struct", FName(TEXT("double")), TBaseStructure<FTransform>::Get(), EPinContainerType::None, false, FEdGraphTerminalType()));

	if (!Status)
	{
		HOUDINI_LOG_WARNING(TEXT("[FHoudiniDataTableTranslator::CreateTransformProp]: Failed to create a transform property for attribute %s."), *TAttrib);
		return false;
	}

	FString ExpectedName = DEFAULT_PROP_PREFIX + FString::FromInt(AssignedIdx++);
	FStructVariableDescription* VarDesc = nullptr;
	for (auto&& Desc : FStructureEditorUtils::GetVarDesc(NewStruct))
	{
		if (Desc.FriendlyName == ExpectedName)
		{
			VarDesc = &Desc;
			break;
		}
	}
	if (!VarDesc)
	{
		HOUDINI_LOG_WARNING(TEXT("[FHoudiniDataTableTranslator::CreateTransformProp]: Could not find newly added transform prop by name %s."), *ExpectedName);
		--AssignedIdx;
		return false;
	}

	Status = FStructureEditorUtils::RenameVariable(NewStruct, VarDesc->VarGuid, TAttrib);
	if (!Status)
	{
		HOUDINI_LOG_WARNING(TEXT("[FHoudiniDataTableTranslator::CreateTransformProp]: Failed to rename the transform property %s to %s."), *ExpectedName, *TAttrib);
		return false;
	}
	++AssignedIdx;
	CreatedIds.Add(TAttrib, VarDesc->VarGuid);

	return true;
}

bool
FHoudiniDataTableTranslator::ApplyTransformProp(const FString& TAttrib,
	const TransformComponents& Comps,
	const TMap<FString, int32>& PropIndices,
	TMap<FString, FProperty*>& ActualProps,
	TMap<FString, FProperty*>& FoundProps)
{
	int32 AttrIdx = 0;
	int32 Slice = 0;
	ExtractIndex(TAttrib, AttrIdx, Slice);

	// Slice off index, does nothing if no index specified.
	FString Fragment = TAttrib.Mid(Slice);

	FProperty** FProp = ActualProps.Find(Fragment);
	if (!FProp)
	{
		return false;
	}
	FStructProperty* SProp = CastField<FStructProperty>(*FProp);
	if (!SProp)
	{
		return false;
	}
	const FName& PropName = SProp->Struct->GetFName();
	if (PropName != NAME_Transform3f && PropName != NAME_Transform3d && PropName != NAME_Transform)
	{
		return false;
	}
	// Make sure there is a corresponding prop in the actual struct and the index matches if present.
	if (AttrIdx && AttrIdx != PropIndices[Fragment])
	{
		return false;
	}

	FoundProps.Add(Comps.Rotation, SProp);
	FoundProps.Add(Comps.Translate, SProp);
	FoundProps.Add(Comps.Scale, SProp);

	ActualProps.Remove(Fragment);
	ActualProps.Add(Comps.Rotation, SProp);
	ActualProps.Add(Comps.Translate, SProp);
	ActualProps.Add(Comps.Scale, SProp);

	return true;
}

bool
FHoudiniDataTableTranslator::CreateRowStructProp(int32 GeoId,
	int32 PartId,
	const FString& AttribName,
	const FString& Fragment,
	HAPI_AttributeOwner AttribOwner,
	HAPI_AttributeInfo& AttribInfo,
	UUserDefinedStruct* NewStruct,
	int32& AssignedIdx,
	TMap<FString, FGuid>& CreatedIds,
	TMap<FString, HAPI_AttributeInfo>& FoundInfos)
{
	HAPI_Result Error = FHoudiniApi::GetAttributeInfo(FHoudiniEngine::Get().GetSession(),
		GeoId, PartId, TCHAR_TO_ANSI(*AttribName), AttribOwner, &AttribInfo);
	if (!AttribInfo.exists || Error != HAPI_RESULT_SUCCESS)
	{
		HOUDINI_LOG_WARNING(TEXT("[FHoudiniDataTableTranslator::CreateRowStructProp]: Error %d when trying to get info for attribute %s."), Error, *AttribName);
		return false;
	}
	if (AttribInfo.tupleSize > 4)
	{
		HOUDINI_LOG_WARNING(TEXT("[FHoudiniDataTableTranslator::CreateRowStructProp]: Cannot convert tuple attribute %s of size %d (>4) to struct property."), *AttribName, AttribInfo.tupleSize);
		return false;
	}

	if (!UE_TYPE_NAMES.Contains(AttribInfo.storage))
	{
		HOUDINI_LOG_WARNING(TEXT("[FHoudiniDataTableTranslator::CreateRowStructProp]: Attribute %s is of an unsupported type %d."), *AttribName, AttribInfo.storage);
		return false;
	}

	FName SubCat = NAME_None;
	if (AttribInfo.storage == HAPI_STORAGETYPE_FLOAT)
	{
		SubCat = FName(TEXT("float"));
	}
	else if (AttribInfo.storage == HAPI_STORAGETYPE_FLOAT64)
	{
		SubCat = FName(TEXT("double"));
	}

	bool Status = FStructureEditorUtils::AddVariable(NewStruct,
		FEdGraphPinType(UE_TYPE_NAMES[AttribInfo.storage][AttribInfo.tupleSize - 1],
			SubCat,
			VECTOR_STRUCTS[AttribInfo.storage][AttribInfo.tupleSize - 1],
			EPinContainerType::None,
			false,
			FEdGraphTerminalType()));
	if (!Status)
	{
		HOUDINI_LOG_WARNING(TEXT("[FHoudiniDataTableTranslator::CreateRowStructProp]: Failed to create a struct property for attribute %s."), *AttribName);
		return false;
	}

	// AddVariable doesn't return a handle to the created prop so we have to search it by name
	FString ExpectedName = DEFAULT_PROP_PREFIX + FString::FromInt(AssignedIdx++);
	FStructVariableDescription* VarDesc = nullptr;
	for (auto&& Desc : FStructureEditorUtils::GetVarDesc(NewStruct))
	{
		if (Desc.FriendlyName == ExpectedName)
		{
			VarDesc = &Desc;
			break;
		}
	}
	if (!VarDesc)
	{
		// Somehow the variable was added but couldn't be found afterwards
		HOUDINI_LOG_WARNING(TEXT("[FHoudiniDataTableTranslator::CreateRowStructProp]: Could not find newly added prop by name %s."), *ExpectedName);
		--AssignedIdx;
		return false;
	}

	Status = FStructureEditorUtils::RenameVariable(NewStruct, VarDesc->VarGuid, Fragment);
	if (!Status)
	{
		HOUDINI_LOG_WARNING(TEXT("[FHoudiniDataTableTranslator::CreateRowStructProp]: Failed to rename the property %s to %s."), *ExpectedName, *Fragment);
		return false;
	}

	// Renaming the variable increments the counter for the next created prop
	++AssignedIdx;
	CreatedIds.Add(AttribName, VarDesc->VarGuid);
	FoundInfos.Add(AttribName, AttribInfo);

	return true;
}

bool
FHoudiniDataTableTranslator::ApplyRowStructProp(int32 GeoId,
	int32 PartId,
	const FString& AttribName,
	const FString& Fragment,
	HAPI_AttributeOwner AttribOwner,
	HAPI_AttributeInfo& AttribInfo,
	TMap<FString, FProperty*>& ActualProps,
	TMap<FString, FProperty*>& FoundProps,
	TMap<FString, int32>& PropIndices,
	TMap<FString, HAPI_AttributeInfo>& FoundInfos)
{
	int32 AttrIdx = 0;
	int32 Slice = 0;
	ExtractIndex(Fragment, AttrIdx, Slice);
	// Slice off index, does nothing if no index specified.
	FString SlicedFragment = Fragment.Mid(Slice);

	FProperty** FProp = ActualProps.Find(SlicedFragment);
	// Make sure there is a corresponding prop in the actual struct and the index matches if present.
	if (FProp && (!AttrIdx || AttrIdx == PropIndices[SlicedFragment]))
	{
		HAPI_Result Error = FHoudiniApi::GetAttributeInfo(FHoudiniEngine::Get().GetSession(),
			GeoId, PartId, TCHAR_TO_ANSI(*AttribName), AttribOwner, &AttribInfo);
		if (!AttribInfo.exists || Error != HAPI_RESULT_SUCCESS)
		{
			HOUDINI_LOG_WARNING(TEXT("[FHoudiniDataTableTranslator::ApplyRowStructProp]: Error %d when trying to get info for attribute %s."), Error, *AttribName);
			return false;
		}
		FoundProps.Add(AttribName, *FProp);
		FoundInfos.Add(AttribName, AttribInfo);
	}
	else
	{
		HOUDINI_LOG_WARNING(TEXT("[FHoudiniDataTableTranslator::ApplyRowStructProp]: Skipping attribute %s, could not find struct prop or index mismatch."), *AttribName);
	}

	return true;
}

bool
FHoudiniDataTableTranslator::RemoveDefaultProp(UUserDefinedStruct* NewStruct,
	FGuid& DefaultPropId,
	int32& StructSize,
	TMap<FString, FGuid>& CreatedIds,
	const TSet<FString>& TransformAttribs,
	const TMap<FString, TransformComponents>& TransformCandidates,
	TMap<FString, FProperty*>& FoundProps)
{
	bool Status = FStructureEditorUtils::RemoveVariable(NewStruct, DefaultPropId);
	if (!Status)
	{
		HOUDINI_LOG_WARNING(TEXT("[FHoudiniDataTableTranslator::RemoveDefaultProp]: Failed to remove default property."));
		return false;
	}

	StructSize = NewStruct->GetStructureSize();
	NewStruct->MarkPackageDirty();

	for (auto&& KV : CreatedIds)
	{
		FProperty* Prop = FStructureEditorUtils::GetPropertyByGuid(NewStruct, KV.Value);
		// If this prop is a transform
		if (TransformAttribs.Contains(KV.Key))
		{
			const FHoudiniDataTableTranslator::TransformComponents& Comps = TransformCandidates[KV.Key];
			FoundProps.Add(Comps.Rotation, Prop);
			FoundProps.Add(Comps.Translate, Prop);
			FoundProps.Add(Comps.Scale, Prop);
		}
		else
		{
			FoundProps.Add(KV.Key, Prop);
		}
	}

	return true;
}

bool
FHoudiniDataTableTranslator::PopulateRowData(int32 GeoId,
	int32 PartId,
	const TMap<FString, FProperty*>& FoundProps,
	const TMap<FString, HAPI_AttributeInfo>& FoundInfos,
	int32 StructSize,
	int32 NumRows,
	uint8* RowData)
{
	HAPI_AttributeInfo AttribInfo;
	HAPI_Result Error = HAPI_RESULT_FAILURE;
	int32 Offset;
	FProperty* Prop;
	ANSICHAR* AttribName;
	int32 RowIdx = 0;
	for (auto&& KV : FoundProps)
	{
		AttribName = TCHAR_TO_ANSI(*KV.Key);
		Prop = KV.Value;
		Offset = Prop->GetOffset_ForInternal();

		AttribInfo = FoundInfos[KV.Key];
		if (AttribInfo.count < 1)
		{
			HOUDINI_LOG_WARNING(TEXT("[FHoudiniDataTableTranslator::PopulateRowData]: Attribute %s has no values."), *KV.Key);
			return false;
		}

		if (AttribInfo.storage == HAPI_STORAGETYPE_INVALID)
		{
			HOUDINI_LOG_WARNING(TEXT("[FHoudiniDataTableTranslator::PopulateRowData]: Invalid storage type for Houdini attribute %s."), *KV.Key);
			return false;
		}

		if (!IsValidType(AttribInfo, Prop))
		{
			HOUDINI_LOG_WARNING(TEXT("[FHoudiniDataTableTranslator::PopulateRowData]: Incompatible types for column %s and attribute %s."), *Prop->GetAuthoredName(), *KV.Key);
			return false;
		}

		void* Data = nullptr;

		if (AttribInfo.storage == HAPI_STORAGETYPE_INT)
		{
			Data = FMemory::Malloc(AttribInfo.count * AttribInfo.tupleSize * sizeof(int32));
			Error = FHoudiniApi::GetAttributeIntData(FHoudiniEngine::Get().GetSession(), GeoId, PartId, AttribName, &AttribInfo, -1, static_cast<int32*>(Data), 0, AttribInfo.count);

			WriteAttributeDataToStruct<int32>(Data, StructSize, AttribInfo, RowData, Prop, KV.Key);
		}
		else if (AttribInfo.storage == HAPI_STORAGETYPE_INT64)
		{
			Data = FMemory::Malloc(AttribInfo.count * AttribInfo.tupleSize * sizeof(int64));
			Error = FHoudiniApi::GetAttributeInt64Data(FHoudiniEngine::Get().GetSession(), GeoId, PartId, AttribName, &AttribInfo, -1, static_cast<int64*>(Data), 0, AttribInfo.count);

			WriteAttributeDataToStruct<int64>(Data, StructSize, AttribInfo, RowData, Prop, KV.Key);
		}
		else if (AttribInfo.storage == HAPI_STORAGETYPE_FLOAT)
		{
			Data = FMemory::Malloc(AttribInfo.count * AttribInfo.tupleSize * sizeof(float));
			Error = FHoudiniApi::GetAttributeFloatData(FHoudiniEngine::Get().GetSession(), GeoId, PartId, AttribName, &AttribInfo, -1, static_cast<float*>(Data), 0, AttribInfo.count);

			WriteAttributeDataToStruct<float>(Data, StructSize, AttribInfo, RowData, Prop, KV.Key);
		}
		else if (AttribInfo.storage == HAPI_STORAGETYPE_FLOAT64)
		{
			Data = FMemory::Malloc(AttribInfo.count * AttribInfo.tupleSize * sizeof(double));
			Error = FHoudiniApi::GetAttributeFloat64Data(FHoudiniEngine::Get().GetSession(), GeoId, PartId, AttribName, &AttribInfo, -1, static_cast<double*>(Data), 0, AttribInfo.count);

			WriteAttributeDataToStruct<double>(Data, StructSize, AttribInfo, RowData, Prop, KV.Key);
		}
		else if (AttribInfo.storage == HAPI_STORAGETYPE_UINT8)
		{
			Data = FMemory::Malloc(AttribInfo.count * AttribInfo.tupleSize * sizeof(uint8));
			Error = FHoudiniApi::GetAttributeUInt8Data(FHoudiniEngine::Get().GetSession(), GeoId, PartId, AttribName, &AttribInfo, -1, static_cast<uint8*>(Data), 0, AttribInfo.count);

			WriteAttributeDataToStruct<uint8>(Data, StructSize, AttribInfo, RowData, Prop, KV.Key);
		}
		else if (AttribInfo.storage == HAPI_STORAGETYPE_INT8)
		{
			Data = FMemory::Malloc(AttribInfo.count * AttribInfo.tupleSize * sizeof(int8));
			Error = FHoudiniApi::GetAttributeInt8Data(FHoudiniEngine::Get().GetSession(), GeoId, PartId, AttribName, &AttribInfo, -1, static_cast<int8*>(Data), 0, AttribInfo.count);

			WriteAttributeDataToStruct<int8>(Data, StructSize, AttribInfo, RowData, Prop, KV.Key);
		}
		else if (AttribInfo.storage == HAPI_STORAGETYPE_INT16) {
			Data = FMemory::Malloc(AttribInfo.count * AttribInfo.tupleSize * sizeof(int16));
			Error = FHoudiniApi::GetAttributeInt16Data(FHoudiniEngine::Get().GetSession(), GeoId, PartId, AttribName, &AttribInfo, -1, static_cast<int16*>(Data), 0, AttribInfo.count);

			WriteAttributeDataToStruct<int16>(Data, StructSize, AttribInfo, RowData, Prop, KV.Key);
		}
		else if (AttribInfo.storage == HAPI_STORAGETYPE_STRING) {
			TArray<FString> StringData;
			StringData.Reserve(NumRows);
			FHoudiniEngineUtils::HapiGetAttributeDataAsString(GeoId, PartId, AttribName, AttribInfo, StringData, AttribInfo.tupleSize);
			if (AttribInfo.tupleSize != 1)
			{
				HOUDINI_LOG_WARNING(TEXT("[FHoudiniDataTableTranslator::PopulateRowData]: Tuples of strings are not supported, skipping attribute %s."), *KV.Key);
				return false;
			}
			if (!Prop->IsA<FStrProperty>() && !Prop->IsA<FTextProperty>() && !Prop->IsA<FNameProperty>())
			{
				HOUDINI_LOG_WARNING(TEXT("[FHoudiniDataTableTranslator::PopulateRowData]: Cannot convert Houdini string attribute to non string property, skipping attribute %s."), *KV.Key);
				return false;
			}
			for (int32 Idx = 0; Idx < NumRows; ++Idx)
			{
				Prop->ImportText(*StringData[Idx], &RowData[Idx * StructSize + Offset], PPF_ExternalEditor, nullptr);
			}
		}
		else {
			HOUDINI_LOG_WARNING(TEXT("[FHoudiniDataTableTranslator::PopulateRowData]: Unknown attribute type %d."), AttribInfo.storage);
			return false;
		}
		if (Data)
		{
			FMemory::Free(Data);
			Data = nullptr;
		}
		if (Error)
		{
			HOUDINI_LOG_WARNING(TEXT("[FHoudiniDataTableTranslator::PopulateRowData]: Error %d when trying to get values for attribute %s."), Error, *KV.Key);
			return false;
		}

		++RowIdx;
	}

	return true;
}

bool
FHoudiniDataTableTranslator::CreateAndSaveDataTable(const FHoudiniGeoPartObject& HGPO,
	int32 NumRows,
	const TArray<FString>& RowNames,
	int32 StructSize,
	const FString& DataTableFolder,
	const FString& DataTableName,
	UScriptStruct* RowStruct,
	uint8* RowData)
{
	TMap<FName, const uint8*> TableData;
	UDataTable* CreatedDataTable;
	TableData.Reserve(NumRows);
	for (int32 Idx = 0; Idx < NumRows; ++Idx)
	{
		TableData.Add(FName(RowNames[Idx]), &RowData[Idx * StructSize]);
	}

	FHoudiniPackageParams DTPackageParams;
	DTPackageParams.ObjectId = HGPO.ObjectId;
	DTPackageParams.GeoId = HGPO.GeoId;
	DTPackageParams.PartId = HGPO.PartId;
	// If no name specified, one will be automatically generated.
	if (DataTableName != "")
	{
		DTPackageParams.ObjectName = DataTableName;
		if (DataTableFolder != "")
		{
			DTPackageParams.FolderOverride = DataTableFolder;
			DTPackageParams.NameOverride = DataTableName;
			DTPackageParams.OverideEnabled = true;
		}
	}

	CreatedDataTable = DTPackageParams.CreateObjectAndPackage<UDataTable>();
	CreatedDataTable->PreEditChange(nullptr);

	// CreateTableFromRawData not available on UE4.
#if ENGINE_MAJOR_VERSION < 5
	CreatedDataTable->RowStruct = RowStruct;
	for (auto RowIt = TableData.CreateIterator(); RowIt; ++RowIt)
	{
		uint8* RowBuf = FDataTableEditorUtils::AddRow(CreatedDataTable, RowIt.Key());
		FMemory::Memcpy(RowBuf, RowIt.Value(), StructSize);
	}
#else
	CreatedDataTable->CreateTableFromRawData(TableData, RowStruct);
#endif

	CreatedDataTable->MarkPackageDirty();

	return true;
}

const FString FHoudiniDataTableTranslator::DEFAULT_PROP_PREFIX = "MemberVar_";

const TMap<HAPI_StorageType, TArray<FName>> FHoudiniDataTableTranslator::UE_TYPE_NAMES = {
	// Names are constant presets taken from UEdGraphSchema_K2
	// Mappings found in UEdGraphSchema_K2::GetPropertyCategoryInfo
	{HAPI_STORAGETYPE_INT, {FName(TEXT("int")), FName(TEXT("struct")), FName(TEXT("struct")), FName(TEXT("struct"))}},
	{HAPI_STORAGETYPE_INT64, {FName(TEXT("int64")), FName(TEXT("struct")), FName(TEXT("struct")), FName(TEXT("struct"))}},
	{HAPI_STORAGETYPE_FLOAT, {FName(TEXT("real")), FName(TEXT("struct")), FName(TEXT("struct")), FName(TEXT("struct"))}},
	{HAPI_STORAGETYPE_FLOAT64, {FName(TEXT("real")), FName(TEXT("struct")), FName(TEXT("struct")), FName(TEXT("struct"))}},
	{HAPI_STORAGETYPE_STRING, {FName(TEXT("string"))}},
	{HAPI_STORAGETYPE_UINT8, {FName(TEXT("byte")), FName(TEXT("struct")), FName(TEXT("struct")), FName(TEXT("struct"))}},
	// No presets, just expand to int32
	{HAPI_STORAGETYPE_INT8, {FName(TEXT("int")), FName(TEXT("struct")), FName(TEXT("struct")), FName(TEXT("struct"))}},
	{HAPI_STORAGETYPE_INT16, {FName(TEXT("int")), FName(TEXT("struct")), FName(TEXT("struct")), FName(TEXT("struct"))}}
};

const TMap<HAPI_StorageType, TArray<UScriptStruct*>> FHoudiniDataTableTranslator::VECTOR_STRUCTS = {
		{HAPI_STORAGETYPE_INT, { nullptr, TBaseStructure<FVector2d>::Get(), TBaseStructure<FVector3d>::Get(), TBaseStructure<FVector4>::Get() }},
		{HAPI_STORAGETYPE_INT64, { nullptr, TBaseStructure<FVector2d>::Get(), TBaseStructure<FVector3d>::Get(), TBaseStructure<FVector4>::Get() }},
		{HAPI_STORAGETYPE_FLOAT, { nullptr, TBaseStructure<FVector2d>::Get(), TBaseStructure<FVector3d>::Get(), TBaseStructure<FVector4>::Get() }},
		{HAPI_STORAGETYPE_FLOAT64, { nullptr, TBaseStructure<FVector2d>::Get(), TBaseStructure<FVector3d>::Get(), TBaseStructure<FVector4>::Get() }},
		// cannot have tuple of strings
		{HAPI_STORAGETYPE_STRING, { nullptr }},
		{HAPI_STORAGETYPE_UINT8, { nullptr, TBaseStructure<FVector2d>::Get(), TBaseStructure<FVector3d>::Get(), TBaseStructure<FVector4>::Get() }},
		{HAPI_STORAGETYPE_INT8, { nullptr, TBaseStructure<FVector2d>::Get(), TBaseStructure<FVector3d>::Get(), TBaseStructure<FVector4>::Get() }},
		{HAPI_STORAGETYPE_INT16, { nullptr, TBaseStructure<FVector2d>::Get(), TBaseStructure<FVector3d>::Get(), TBaseStructure<FVector4>::Get() }}
};

void
FHoudiniDataTableTranslator::ExtractIndex(const FString& Fragment,
	int32& AttrIdx,
	int32& Slice)
{
	// Determine if an index was specified in attrib name.
	for (int32 Idx = 0; Idx < Fragment.Len(); ++Idx)
	{
		++Slice;
		const TCHAR& ch = Fragment[Idx];
		if (ch == '_' && AttrIdx != 0)
		{
			break;
		}
		if (ch >= '0' && ch <= '9')
		{
			AttrIdx *= 10;
			AttrIdx += ch - '0';
		}
		// If we encounter a letter before a _, the index is not present.
		else
		{
			AttrIdx = 0;
			Slice = 0;
			break;
		}
	}
}

bool
FHoudiniDataTableTranslator::IsValidType(const HAPI_AttributeInfo& AttribInfo,
	const FProperty* Prop)
{
	// Invalid if attribute is string but property type is not
	// Numeric to numeric is fine regardless of types, will just
	// forcefully convert.
	// Numeric to string is also fine, will convert the numbers to string.
	if (AttribInfo.storage == HAPI_STORAGETYPE_STRING)
	{
		return Prop->IsA<FStrProperty>() ||
			Prop->IsA<FNameProperty>() ||
			Prop->IsA<FTextProperty>();
	}
	// Check the destination prop has the same number of components.
	if (AttribInfo.tupleSize == 1)
	{
		return Prop->IsA<FBoolProperty>() ||
			Prop->IsA<FNumericProperty>() ||
			Prop->IsA<FStrProperty>() ||
			Prop->IsA<FNameProperty>() ||
			Prop->IsA<FTextProperty>();
	}
	const FStructProperty* SProp = CastField<FStructProperty>(Prop);
	if (!SProp)
	{
		return false;
	}
	const FName Name = SProp->Struct->GetFName();
	if (AttribInfo.tupleSize == 2)
	{
		return Name == NAME_Vector2d || Name == NAME_Vector2D;
	}
	else if (AttribInfo.tupleSize == 3)
	{
		return Name == NAME_Vector3f || Name == NAME_Vector3d ||
			Name == NAME_Rotator3f || Name == NAME_Rotator3d ||
			Name == NAME_Vector || Name == NAME_Rotator ||
			Name == NAME_Transform || Name == NAME_Transform3f ||
			Name == NAME_Transform3d;
	}
	else if (AttribInfo.tupleSize == 4)
	{
		return Name == NAME_Vector4f || Name == NAME_Vector4d ||
			Name == NAME_Vector4 || Name == NAME_LinearColor ||
			Name == NAME_Color || Name == NAME_Transform ||
			Name == NAME_Transform3f || Name == NAME_Transform3d;
	}
	return false;
}
