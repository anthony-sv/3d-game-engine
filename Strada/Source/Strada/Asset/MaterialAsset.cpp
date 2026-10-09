#include "stpch.h"
#include "Strada/Asset/MaterialAsset.h"

#include "Strada/Core/FileSystem.h"

namespace Strada
{
	void MaterialAsset::SetData(MaterialData data)
	{
		m_Data = std::move(data);
		m_Version++;
	}

	Json MaterialSerializer::Serialize(MaterialData const& data)
	{
		Json document = Json::object();
		document["Strada"] = MakeFileHeader("Material", FormatVersion);
		document["Material"] = SerializeFields<StructTraits<MaterialData>>(data);
		return document;
	}

	Result<MaterialData> MaterialSerializer::Deserialize(Json const& json, DeserializationContext const& context)
	{
		if (Result<int> header = ReadFileHeader(json, "Material", FormatVersion); !header)
		{
			return Error{header.GetError()};
		}
		auto const material = json.find("Material");
		if (material == json.end())
		{
			return Error{"material file has no \"Material\" object"};
		}

		MaterialData data;
		if (Result<void> result = DeserializeFields<StructTraits<MaterialData>>(*material, data, context, "material"); !result)
		{
			return Error{result.GetError()};
		}
		return data;
	}

	Result<void> MaterialSerializer::SaveToFile(MaterialData const& data, std::filesystem::path const& path)
	{
		return FileSystem::WriteTextFile(path, DumpJson(Serialize(data)));
	}

	Result<MaterialData> MaterialSerializer::LoadFromFile(std::filesystem::path const& path, DeserializationContext const& context)
	{
		Result<std::string> text = FileSystem::ReadTextFile(path);
		if (!text)
		{
			return Error{text.GetError()};
		}
		Result<Json> json = ParseJson(text.GetValue());
		if (!json)
		{
			return MakeError("'{}': {}", FileSystem::PathToUtf8(path), json.GetError());
		}
		Result<MaterialData> data = Deserialize(json.GetValue(), context);
		if (!data)
		{
			return MakeError("'{}': {}", FileSystem::PathToUtf8(path), data.GetError());
		}
		return data;
	}
}
