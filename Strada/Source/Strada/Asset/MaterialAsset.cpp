#include "stpch.h"
#include "Strada/Asset/MaterialAsset.h"

#include "Strada/Core/FileSystem.h"

#include <algorithm>
#include <cmath>

namespace Strada
{
	std::vector<std::string_view> FitMaterialToFieldRanges(MaterialData& data)
	{
		// The brightest channel becomes 1 and the intensity carries the rest. Non-finite numbers are left to
		// FitToFieldRanges, which also gives an intensity that overflows its default.
		float brightest = 0.0f;
		for (glm::length_t i = 0; i < 3; i++)
		{
			if (std::isfinite(data.EmissiveColor[i]))
			{
				brightest = std::max(brightest, data.EmissiveColor[i]);
			}
		}
		if (brightest > 1.0f && std::isfinite(data.EmissiveIntensity))
		{
			data.EmissiveColor /= brightest;
			data.EmissiveIntensity *= brightest;
		}
		return FitToFieldRanges<StructTraits<MaterialData>>(data);
	}

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
