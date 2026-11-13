#include "stpch.h"
#include "Strada/Asset/DataAsset.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"

#include <algorithm>
#include <cstddef>
#include <limits>

namespace Strada
{
	namespace
	{
		constexpr size_t MaxNameLength = 64;
		constexpr size_t MinExtensionLetters = 2;
		constexpr size_t MaxExtensionLetters = 16;

		struct RegistryState
		{
			// The engine's types first, in the order of their list, then the others in registration order.
			std::vector<Ref<DataAssetType>> Types;
		};

		// A function's static, so lookups made while other translation units initialize find it constructed.
		RegistryState& GetState()
		{
			static RegistryState state;
			return state;
		}

		// Outside the registry's state: it never repeats within the process.
		uint64_t s_Epoch = 0;

		bool IsUpper(char character)
		{
			return character >= 'A' && character <= 'Z';
		}

		bool IsLower(char character)
		{
			return character >= 'a' && character <= 'z';
		}

		bool IsDigit(char character)
		{
			return character >= '0' && character <= '9';
		}

		char ToLower(char character)
		{
			return IsUpper(character) ? static_cast<char>(character - 'A' + 'a') : character;
		}

		std::string ToLower(std::string_view text)
		{
			std::string lower(text);
			for (char& character : lower)
			{
				character = ToLower(character);
			}
			return lower;
		}

		// Words start at an uppercase letter that follows a lowercase one, and at the last capital of a run of capitals that a
		// lowercase letter follows ("UIStyle": "ui-style"); digits join the word before them ("Curve2D": "curve2d").
		std::string MakeCommandDomain(std::string_view name)
		{
			std::string domain;
			for (size_t i = 0; i < name.size(); i++)
			{
				char const character = name[i];
				if (i > 0 && IsUpper(character))
				{
					bool const afterLower = IsLower(name[i - 1]);
					bool const endsCapitals = IsUpper(name[i - 1]) && i + 1 < name.size() && IsLower(name[i + 1]);
					if (afterLower || endsCapitals)
					{
						domain += '-';
					}
				}
				domain += ToLower(character);
			}
			return domain;
		}

		// The leading capital, or the leading run of capitals but the one that starts the next word, in lowercase
		// ("TestData": "testData", "UIStyle": "uiStyle", "Curve2D": "curve2D").
		std::string MakeCommandParameter(std::string_view name)
		{
			std::string parameter(name);
			for (size_t i = 0; i < parameter.size(); i++)
			{
				bool const startsNextWord = i > 0 && i + 1 < parameter.size() && !IsUpper(parameter[i + 1]);
				if (!IsUpper(parameter[i]) || startsNextWord)
				{
					break;
				}
				parameter[i] = ToLower(parameter[i]);
			}
			return parameter;
		}

		Result<void> CheckName(std::string_view name)
		{
			if (name.empty() || name.size() > MaxNameLength)
			{
				return MakeError("data asset type names have 1 to {} characters, not {}", MaxNameLength, name.size());
			}
			if (!IsUpper(name.front()) || !std::ranges::all_of(name,
			                                                   [](char character)
			                                                   {
																   return IsUpper(character) || IsLower(character) || IsDigit(character);
															   }))
			{
				return MakeError("data asset type name '{}' is not PascalCase ASCII letters and digits", name);
			}
			if (BuiltInAssetTypeFromString(name))
			{
				return MakeError("'{}' is the name of a built-in asset type", name);
			}
			return {};
		}

		Result<void> CheckExtension(std::string_view name, std::string_view extension)
		{
			bool const validLetters = std::ranges::all_of(extension.substr(std::min<size_t>(extension.size(), 1)),
			                                              [](char character)
			                                              {
															  return IsLower(character) || IsDigit(character);
														  });
			if (!extension.starts_with('.') || extension.size() - 1 < MinExtensionLetters || extension.size() - 1 > MaxExtensionLetters ||
			    !validLetters)
			{
				return MakeError("data asset type {}: the extension '{}' is not a dot and {} to {} lowercase ASCII letters or digits", name,
				                 extension, MinExtensionLetters, MaxExtensionLetters);
			}
			if (GetBuiltInAssetTypeForExtension(extension) != AssetType::None)
			{
				return MakeError("data asset type {}: '{}' is the extension of the built-in asset type {}", name, extension,
				                 AssetTypeToString(GetBuiltInAssetTypeForExtension(extension)));
			}
			return {};
		}

		// What the type repeats of a registered type: its name (in any letter case), its extension, its command domain or its
		// command parameter, or its data struct.
		Result<void> CheckUnique(DataAssetType const& type, RegistryState const& state)
		{
			std::string const lowerName = ToLower(type.GetName());
			for (Ref<DataAssetType> const& other : state.Types)
			{
				if (ToLower(other->GetName()) == lowerName)
				{
					return MakeError("data asset type {} is registered already ({})", type.GetName(), other->GetName());
				}
				if (other->GetExtension() == type.GetExtension())
				{
					return MakeError("data asset type {}: the extension '{}' is {}'s", type.GetName(), type.GetExtension(),
					                 other->GetName());
				}
				if (other->GetCommandDomain() == type.GetCommandDomain() || other->GetCommandParameter() == type.GetCommandParameter())
				{
					return MakeError("data asset type {}: its commands ({}.*, \"{}\") would be {}'s", type.GetName(),
					                 type.GetCommandDomain(), type.GetCommandParameter(), other->GetName());
				}
				if (other->GetDataType() == type.GetDataType())
				{
					return MakeError("data asset type {}: its data struct is registered already, as {}", type.GetName(), other->GetName());
				}
			}
			return {};
		}

		Result<void> CheckFormat(DataAssetType const& type)
		{
			uint32_t const version = type.GetFormatVersion();
			if (version < 1 || version > static_cast<uint32_t>(std::numeric_limits<int>::max()))
			{
				return MakeError("data asset type {}: the format version {} is not a positive version", type.GetName(), version);
			}
			std::span<DocumentMigration const> const migrations = type.GetMigrations();
			if (migrations.size() != version - 1)
			{
				return MakeError("data asset type {}: version {} needs {} migrations, not {}", type.GetName(), version, version - 1,
				                 migrations.size());
			}
			if (std::ranges::find(migrations, nullptr) != migrations.end())
			{
				return MakeError("data asset type {}: a migration is missing", type.GetName());
			}
			return {};
		}
	}

	DataAssetType::DataAssetType(std::type_index dataType, std::string_view name, std::string_view extension, uint32_t formatVersion,
	                             std::string_view description, std::span<DocumentMigration const> migrations,
	                             std::vector<FieldDescriptor> fields, Json defaultFields)
		: m_DataType(dataType),
		  m_Name(name),
		  m_Extension(extension),
		  m_FormatVersion(formatVersion),
		  m_Description(description),
		  m_Migrations(migrations.begin(), migrations.end()),
		  m_CommandDomain(MakeCommandDomain(name)),
		  m_CommandParameter(MakeCommandParameter(name)),
		  m_Fields(std::move(fields)),
		  m_DefaultFields(std::move(defaultFields))
	{
	}

	DataAsset::DataAsset(Ref<DataAssetType const> type)
		: m_Type(std::move(type)),
		  m_Version(DataAssetRegistry::DrawVersion())
	{
	}

	void DataAsset::MarkChanged()
	{
		m_Version = DataAssetRegistry::DrawVersion();
	}

	template<ReflectedStruct TData>
	void DataAssetRegistry::AddEngineType(DataAssetTypeSpecification<TData> const& specification)
	{
		Result<AssetType> const type = Add(CreateRef<TypedDataAssetType<TData>>(specification), true);
		if (!type)
		{
			ST_CORE_ERROR("The engine's data asset type {} cannot be registered: {}", specification.Name, type.GetError());
		}
		ST_CORE_ASSERT(type.IsOk(), "An engine data asset type failed to register");
	}

	bool DataAssetRegistry::AddEngineTypes()
	{
		// The engine's data asset types, one line each, sorted by name (it merges as a union), for example:
		//   AddEngineType<Curve>(CurveAssetSpecification);
		return true;
	}

	void DataAssetRegistry::EnsureEngineTypes()
	{
		// Thread-safe, and only once: registering looks the registry up through GetState, not through this function.
		[[maybe_unused]] static bool const registered = AddEngineTypes();
	}

	Result<AssetType> DataAssetRegistry::Add(Ref<DataAssetType> type, bool engineType)
	{
		if (!engineType)
		{
			EnsureEngineTypes();
			if (AssetManager::IsInitialized() && AssetManager::HasAssetDirectory())
			{
				return MakeError("data asset type {} cannot be registered while an asset directory is open", type->GetName());
			}
		}
		RegistryState& state = GetState();
		if (Result<void> result = CheckName(type->GetName()); !result)
		{
			return Error{result.GetError()};
		}
		if (Result<void> result = CheckExtension(type->GetName(), type->GetExtension()); !result)
		{
			return Error{result.GetError()};
		}
		if (type->GetDescription().empty())
		{
			return MakeError("data asset type {} has no description", type->GetName());
		}
		if (Result<void> result = CheckFormat(*type); !result)
		{
			return Error{result.GetError()};
		}
		if (Result<void> result = CheckUnique(*type, state); !result)
		{
			return Error{result.GetError()};
		}
		if (state.Types.size() >= MaxTypes)
		{
			return MakeError("data asset type {}: {} types are registered already", type->GetName(), MaxTypes);
		}
		if (Result<void> result = type->CheckDefaultData(); !result)
		{
			return MakeError("data asset type {}: the default value cannot be an asset's: {}", type->GetName(), result.GetError());
		}

		// The lowest value no registered type has.
		uint32_t value = FirstAssetTypeValue;
		while (std::ranges::any_of(state.Types,
		                           [value](Ref<DataAssetType> const& other)
		                           {
									   return static_cast<uint32_t>(other->GetAssetType()) == value;
								   }))
		{
			value++;
		}
		type->m_AssetType = static_cast<AssetType>(value);
		type->m_IsEngineType = engineType;
		state.Types.push_back(std::move(type));
		s_Epoch++;
		return static_cast<AssetType>(value);
	}

	Result<void> DataAssetRegistry::Unregister(AssetType type)
	{
		EnsureEngineTypes();
		RegistryState& state = GetState();
		auto const found = std::ranges::find_if(state.Types,
		                                        [type](Ref<DataAssetType> const& registered)
		                                        {
													return registered->GetAssetType() == type;
												});
		if (found == state.Types.end())
		{
			return MakeError("no data asset type has the value {}", static_cast<uint32_t>(type));
		}
		if ((*found)->IsEngineType())
		{
			return MakeError("{} is one of the engine's data asset types, which stay registered", (*found)->GetName());
		}
		if (AssetManager::IsInitialized() && AssetManager::HasAssetDirectory())
		{
			return MakeError("data asset type {} cannot be unregistered while an asset directory is open", (*found)->GetName());
		}
		state.Types.erase(found);
		s_Epoch++;
		return {};
	}

	DataAssetType const* DataAssetRegistry::Find(AssetType type)
	{
		if (!IsDataAssetValue(type))
		{
			return nullptr;
		}
		EnsureEngineTypes();
		for (Ref<DataAssetType> const& registered : GetState().Types)
		{
			if (registered->GetAssetType() == type)
			{
				return registered.get();
			}
		}
		return nullptr;
	}

	DataAssetType const* DataAssetRegistry::FindByName(std::string_view name)
	{
		EnsureEngineTypes();
		for (Ref<DataAssetType> const& registered : GetState().Types)
		{
			if (registered->GetName() == name)
			{
				return registered.get();
			}
		}
		return nullptr;
	}

	DataAssetType const* DataAssetRegistry::FindByExtension(std::string_view extension)
	{
		EnsureEngineTypes();
		std::string const lower = ToLower(extension);
		for (Ref<DataAssetType> const& registered : GetState().Types)
		{
			if (registered->GetExtension() == lower)
			{
				return registered.get();
			}
		}
		return nullptr;
	}

	DataAssetType const* DataAssetRegistry::FindByDataType(std::type_index dataType)
	{
		EnsureEngineTypes();
		for (Ref<DataAssetType> const& registered : GetState().Types)
		{
			if (registered->GetDataType() == dataType)
			{
				return registered.get();
			}
		}
		return nullptr;
	}

	std::vector<DataAssetType const*> DataAssetRegistry::GetTypes()
	{
		EnsureEngineTypes();
		std::vector<DataAssetType const*> types;
		types.reserve(GetState().Types.size());
		for (Ref<DataAssetType> const& registered : GetState().Types)
		{
			types.push_back(registered.get());
		}
		return types;
	}

	uint64_t DataAssetRegistry::GetEpoch()
	{
		return s_Epoch;
	}

	void DataAssetRegistry::NotifyUnloaded()
	{
		s_Epoch++;
	}

	uint64_t DataAssetRegistry::DrawVersion()
	{
		return ++s_Epoch;
	}

	Json DataAssetSerializer::Serialize(DataAsset const& asset)
	{
		return MakeStructDocument(asset.GetDataAssetType().GetFormat(), asset.SerializeFields());
	}

	Result<Ref<DataAsset>> DataAssetSerializer::Deserialize(DataAssetType const& type, Json const& document,
	                                                        DeserializationContext const& context)
	{
		Result<Json> content = ReadDocumentContent(document, type.GetFormat());
		if (!content)
		{
			return Error{content.GetError()};
		}
		Ref<DataAsset> asset = type.CreateAsset();
		if (Result<void> applied = asset->ApplyFields(content.GetValue(), context); !applied)
		{
			return Error{applied.GetError()};
		}
		return asset;
	}

	Result<void> DataAssetSerializer::SaveToFile(DataAsset const& asset, std::filesystem::path const& path)
	{
		return FileSystem::WriteTextFile(path, DumpJson(Serialize(asset)));
	}

	Result<Ref<DataAsset>> DataAssetSerializer::LoadFromFile(DataAssetType const& type, std::filesystem::path const& path,
	                                                         DeserializationContext const& context)
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
		Result<Ref<DataAsset>> asset = Deserialize(type, json.GetValue(), context);
		if (!asset)
		{
			return MakeError("'{}': {}", FileSystem::PathToUtf8(path), asset.GetError());
		}
		return asset;
	}
}
