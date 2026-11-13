#pragma once

#include "Strada/Asset/Asset.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Serialization/JsonSerialization.h"
#include "Strada/Serialization/StructDocument.h"
#include "Strada/Serialization/StructSerialization.h"

#include <concepts>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <vector>

// Data assets: asset types made of one reflected struct, each from one registration (Docs/Architecture.md §6.4). A
// registration names the type, its file extension, its format version and the struct whose field table (StructTraits)
// describes the data; from it come the versioned file, its loader and saver, the AssetType and extension mapping, a
// version for hot reload, the editor's asset editor and commands, and membership in the registry-wide fuzz test.
namespace Strada
{
	class DataAsset;
	template<ReflectedStruct TData>
	class TypedDataAsset;

	// What one registration names. The data struct TData has a field table (StructTraits<TData>) whose field descriptions,
	// ranges and hints are the type's documentation, editor and automation schema; every value in it has an editor and a
	// schema of its own (no FieldKind::Custom value, down through nested structs and array elements, and no array of
	// arrays; checked at compile time). TData lives in the Asset module or below it (Core, Math); its field table, which
	// needs Serialization, lives in the Asset module beside the registration (for a struct of Core or Math, in the type's
	// own header Asset/<Name>Asset.h, which every user of the asset includes).
	template<ReflectedStruct TData>
	struct DataAssetTypeSpecification
	{
		// The type's name: PascalCase ASCII letters and digits, 1 to 64 bytes, not the name of a built-in AssetType or of
		// another registered type. It is the AssetType name (AssetTypeToString, asset registries, the References hint of asset
		// fields), the file header's Type, the key of the file's content object, the managed class's name, and, in kebab case,
		// the commands' domain ("PhysicsMaterial": physics-material.create).
		std::string_view Name;
		// The file extension: a dot, then 2 to 16 lowercase ASCII letters or digits (".scurve"), not the extension of a built-in
		// type or of another registered type. Strada's formats start with ".s".
		std::string_view Extension;
		// The version every save writes, at least 1. Adding a field with a default needs no new version (older files read it
		// as the default); renaming, removing or changing the meaning of a field does, with a migration.
		uint32_t FormatVersion = 1;
		// One sentence for users and agents: the content browser's New menu, the commands' descriptions and the schema.
		std::string_view Description;
		// One migration per version before FormatVersion (FormatVersion - 1 of them): Migrations[i] upgrades version i + 1 to
		// version i + 2. The span's storage must outlive the registration (a static array).
		std::span<DocumentMigration const> Migrations;
		// Rules the field table cannot state (names that must be unique, keys in order, counts that depend on each other);
		// called with every value an asset is about to take (loaded, patched or set). It refuses and never repairs. Null when
		// the field table says all. Pure; any thread.
		Result<void> (*Validate)(TData const& data) = nullptr;
	};

	// One registered data asset type: what its registration named, and what the registry derived from it. Shared by the
	// registry and every asset of the type, so a type and its strings stay valid while either holds it: the engine's types
	// for the whole process, a type registered by Register at least until it is unregistered. Main thread only, like the
	// registry.
	class DataAssetType : public std::enable_shared_from_this<DataAssetType>
	{
	public:
		virtual ~DataAssetType() = default;

		DataAssetType(DataAssetType const&) = delete;
		DataAssetType& operator=(DataAssetType const&) = delete;

		// The AssetType value the registry assigned: DataAssetRegistry::FirstAssetTypeValue and up (the engine's types first,
		// in the order of their list).
		AssetType GetAssetType() const { return m_AssetType; }
		std::string const& GetName() const { return m_Name; }
		// Lowercase, with the dot.
		std::string const& GetExtension() const { return m_Extension; }
		uint32_t GetFormatVersion() const { return m_FormatVersion; }
		std::string const& GetDescription() const { return m_Description; }
		std::span<DocumentMigration const> GetMigrations() const { return m_Migrations; }
		// The format of the type's files (the name, the format version and the migrations).
		StructDocumentFormat GetFormat() const { return {m_Name, m_FormatVersion, m_Migrations}; }
		// The domain of the type's automation commands: the name in kebab case ("PhysicsMaterial": "physics-material",
		// "UIStyle": "ui-style", "Curve2D": "curve2d").
		std::string const& GetCommandDomain() const { return m_CommandDomain; }
		// The name of the commands' asset parameter and of the asset in their results: the name in lower camel case
		// ("PhysicsMaterial": "physicsMaterial", "UIStyle": "uiStyle", "Curve2D": "curve2D").
		std::string const& GetCommandParameter() const { return m_CommandParameter; }
		// The data struct's fields as generic editors and schemas see them (GetFieldDescriptors).
		std::span<FieldDescriptor const> GetFields() const { return m_Fields; }
		// The content object of a value-initialized TData: every field at its default.
		Json const& GetDefaultFields() const { return m_DefaultFields; }
		// The struct's C++ type, which the registry finds the type by (DataAssetRegistry::Find<TData>).
		std::type_index GetDataType() const { return m_DataType; }
		// One of the engine's own types (registered for the whole process), not one registered by Register.
		bool IsEngineType() const { return m_IsEngineType; }

		// A new asset of the type with the default data, not known to the asset manager (its Handle is invalid).
		[[nodiscard]] virtual Ref<DataAsset> CreateAsset() const = 0;

	protected:
		DataAssetType(std::type_index dataType, std::string_view name, std::string_view extension, uint32_t formatVersion,
		              std::string_view description, std::span<DocumentMigration const> migrations, std::vector<FieldDescriptor> fields,
		              Json defaultFields);

	private:
		friend class DataAssetRegistry;

		// Whether a value-initialized TData serializes into fields that read back and passes the type's Validate.
		[[nodiscard]] virtual Result<void> CheckDefaultData() const = 0;

		AssetType m_AssetType = AssetType::None;
		bool m_IsEngineType = false;
		std::type_index m_DataType;
		std::string m_Name;
		std::string m_Extension;
		uint32_t m_FormatVersion = 1;
		std::string m_Description;
		std::vector<DocumentMigration> m_Migrations;
		std::string m_CommandDomain;
		std::string m_CommandParameter;
		std::vector<FieldDescriptor> m_Fields;
		Json m_DefaultFields;
	};

	// The registration of a data struct TData: the typed half of DataAssetType, which keeps the specification's validation.
	// Made with CreateRef (assets of the type share it).
	template<ReflectedStruct TData>
	class TypedDataAssetType final : public DataAssetType
	{
	public:
		static_assert(Detail::IsDescribedFieldType<TData>(),
		              "Every value of a data asset needs an editor and a schema: no field of a type with its own JSON format (FieldKind::"
		              "Custom), in nested structs and array elements too, and no array of arrays (Docs/Architecture.md §6.4)");

		explicit TypedDataAssetType(DataAssetTypeSpecification<TData> const& specification)
			: DataAssetType(std::type_index(typeid(TData)), specification.Name, specification.Extension, specification.FormatVersion,
		                    specification.Description, specification.Migrations, GetFieldDescriptors<StructTraits<TData>, TData>(),
		                    SerializeFields<StructTraits<TData>>(TData{})),
			  m_Validate(specification.Validate)
		{
		}

		[[nodiscard]] Ref<DataAsset> CreateAsset() const override;

		// Whether the data can be an asset's: it serializes into fields that read back (every number finite and within its
		// field's range, every array within its MaxCount, in nested structs and elements too) and passes the type's Validate.
		[[nodiscard]] Result<void> Check(TData const& data) const
		{
			TData readBack{};
			if (Result<void> read = DeserializeFields<StructTraits<TData>>(SerializeFields<StructTraits<TData>>(data), readBack,
			                                                               DeserializationContext{}, "data asset");
			    !read)
			{
				return read;
			}
			return Validate(data);
		}

		// The specification's Validate, with the type's name in front of its message, or success without one.
		[[nodiscard]] Result<void> Validate(TData const& data) const
		{
			if (m_Validate == nullptr)
			{
				return {};
			}
			if (Result<void> valid = m_Validate(data); !valid)
			{
				return MakeError("{}: {}", GetName(), valid.GetError());
			}
			return {};
		}

	private:
		[[nodiscard]] Result<void> CheckDefaultData() const override { return Check(TData{}); }

		Result<void> (*m_Validate)(TData const& data) = nullptr;
	};

	// Base class of every loaded data asset: its type, its data as JSON, and the version that tells users it changed. The
	// typed data is TypedDataAsset<TData>'s. Main thread only.
	class DataAsset : public Asset
	{
	public:
		AssetType GetAssetType() const final { return m_Type->GetAssetType(); }
		// Valid as long as the asset (it shares its type).
		DataAssetType const& GetDataAssetType() const { return *m_Type; }

		// A number no other data asset object had or will have within the process: drawn when the object is created and again
		// by every change of its data. Users keep the version of what they derived from the asset and compare it at their
		// next use; a refresh that reloads the file creates another object, whose version differs as well.
		uint64_t GetVersion() const { return m_Version; }

		// The data as the content object of its file: every field, in field table order.
		[[nodiscard]] virtual Json SerializeFields() const = 0;
		// Applies a partial patch (fields missing from it keep their values; unknown fields follow the context's policy). The
		// data changes only when every field reads, lies in its range and the whole value passes the type's Validate;
		// nothing changes otherwise. A change draws a new version; a patch that leaves the data as it was draws none.
		[[nodiscard]] virtual Result<void> ApplyFields(Json const& patch, DeserializationContext const& context) = 0;

	protected:
		explicit DataAsset(Ref<DataAssetType const> type);

		// Draws the version of a change.
		void MarkChanged();

	private:
		Ref<DataAssetType const> m_Type;
		uint64_t m_Version = 0;
	};

	// A loaded data asset of the registered struct TData: what C++ users read (AssetManager::GetAsset<TypedDataAsset<TData>>).
	template<ReflectedStruct TData>
	class TypedDataAsset final : public DataAsset
	{
	public:
		explicit TypedDataAsset(Ref<TypedDataAssetType<TData> const> type)
			: DataAsset(type)
		{
		}

		// The registered type of TData; AssetType::None when TData is not registered (then no asset is of this class).
		static AssetType GetStaticType();

		// Valid until the next change of the data.
		TData const& GetData() const { return m_Data; }
		// Takes the data when it can be an asset's (TypedDataAssetType::Check); nothing changes otherwise. Draws a new version
		// when the data differs.
		[[nodiscard]] Result<void> SetData(TData data)
		{
			if (Result<void> valid = GetTypedType().Check(data); !valid)
			{
				return valid;
			}
			if (!IsSameData(data, m_Data))
			{
				m_Data = std::move(data);
				MarkChanged();
			}
			return {};
		}

		[[nodiscard]] Json SerializeFields() const override { return Strada::SerializeFields<StructTraits<TData>>(m_Data); }

		[[nodiscard]] Result<void> ApplyFields(Json const& patch, DeserializationContext const& context) override
		{
			TData updated = m_Data;
			if (Result<void> read = DeserializeFields<StructTraits<TData>>(patch, updated, context, "data asset"); !read)
			{
				return read;
			}
			if (Result<void> valid = GetTypedType().Validate(updated); !valid)
			{
				return valid;
			}
			if (!IsSameData(updated, m_Data))
			{
				m_Data = std::move(updated);
				MarkChanged();
			}
			return {};
		}

	private:
		TypedDataAssetType<TData> const& GetTypedType() const { return static_cast<TypedDataAssetType<TData> const&>(GetDataAssetType()); }

		static bool IsSameData(TData const& first, TData const& second)
		{
			if constexpr (std::equality_comparable<TData>)
			{
				return first == second;
			}
			else
			{
				return Strada::SerializeFields<StructTraits<TData>>(first) == Strada::SerializeFields<StructTraits<TData>>(second);
			}
		}

		TData m_Data{};
	};

	// The registry of data asset types. It has two kinds of types:
	// - The engine's own types, one list in DataAsset.cpp (one line per type, sorted by name). They are registered when the
	//   registry is first used (by any function below or any Asset.h lookup, on whichever host) and stay for the whole
	//   process, with fixed AssetType values from FirstAssetTypeValue in the list's order, so every host, tool and test
	//   knows them by name, whether or not it initializes the asset manager: an asset registry keeps no entry of a type it
	//   does not know and a scan registers no file of an unknown extension, so a host that missed a type would drop the
	//   handles of its files when it saves the registry, and a parser of the visual script library or of script metadata
	//   would refuse the type's name.
	// - Types registered by Register (the tests' types), from the next free value, until Unregister. Neither works while
	//   the asset manager has an asset directory open (its files were scanned with the types it knew).
	//
	// AssetTypeToString, AssetTypeFromString, GetAssetTypeForExtension and GetSupportedAssetExtensions (Asset.h) consult it
	// after the built-in types. Main thread only (the engine's types are registered safely on any thread, but types
	// registered by Register are not guarded).
	class DataAssetRegistry
	{
	public:
		// Data asset types take the AssetType values from here up; the built-in AssetType enumerators stay below.
		static constexpr uint32_t FirstAssetTypeValue = 128;
		// The most types registered at once, the engine's included (the values left in AssetType's uint8_t).
		static constexpr uint32_t MaxTypes = 256 - FirstAssetTypeValue;

		// Registers TData as a data asset type and returns its AssetType, the lowest value no registered type has. Fails,
		// registering nothing, when the name, the extension, the version or the migrations break DataAssetTypeSpecification's
		// rules, when the name, the extension, the command domain or the command parameter repeat a built-in or registered
		// type's, when a value-initialized TData does not read back from its fields or fails Validate, when TData or MaxTypes
		// types are registered already, or while the asset manager has an asset directory open. The editor's commands of a
		// type exist only when it was registered before RegisterEditorCommands ran.
		template<ReflectedStruct TData>
		[[nodiscard]] static Result<AssetType> Register(DataAssetTypeSpecification<TData> const& specification)
		{
			return Add(CreateRef<TypedDataAssetType<TData>>(specification), false);
		}
		// Forgets a type registered by Register. Fails for the engine's types, for values no type has, and while the asset
		// manager has an asset directory open. Assets of the type that are still held keep it alive, but no lookup finds it.
		[[nodiscard]] static Result<void> Unregister(AssetType type);

		// Null when the value is no registered data asset type (built-in types included).
		static DataAssetType const* Find(AssetType type);
		// By name, matched exactly.
		static DataAssetType const* FindByName(std::string_view name);
		// By extension with the dot, in any letter case.
		static DataAssetType const* FindByExtension(std::string_view extension);
		// By data struct.
		template<ReflectedStruct TData>
		static TypedDataAssetType<TData> const* Find()
		{
			return static_cast<TypedDataAssetType<TData> const*>(FindByDataType(std::type_index(typeid(TData))));
		}
		// Every registered type: the engine's in the order of their list, then the others in registration order.
		static std::vector<DataAssetType const*> GetTypes();
		// Whether the value lies in the range of data asset types (registered or not).
		static constexpr bool IsDataAssetValue(AssetType type) { return static_cast<uint32_t>(type) >= FirstAssetTypeValue; }

		// Changes whenever what a data asset handle resolves to may have changed: a data asset object was created (loaded,
		// reloaded or made in memory), changed its data, or was dropped by the asset manager, or a type was registered or
		// unregistered. Never repeats within the process. The scripting runtime compares it before running scripts to tell
		// managed caches of data assets to read again.
		static uint64_t GetEpoch();
		// The asset manager dropped loaded data assets (unloaded one, closed the directory, removed a memory asset, shut
		// down): moves the epoch.
		static void NotifyUnloaded();

	private:
		friend class DataAsset;

		// A new version for a data asset object: the epoch, moved first.
		static uint64_t DrawVersion();
		// Checks the type's specification and the registry's state, then assigns the AssetType value and keeps the type.
		[[nodiscard]] static Result<AssetType> Add(Ref<DataAssetType> type, bool engineType);
		static DataAssetType const* FindByDataType(std::type_index dataType);
		// Registers the engine's types (the list in DataAsset.cpp) once per process, before any lookup.
		static void EnsureEngineTypes();
		static bool AddEngineTypes();
		// One line of the engine's list: registers the type, which must succeed.
		template<ReflectedStruct TData>
		static void AddEngineType(DataAssetTypeSpecification<TData> const& specification);
	};

	// Files of data assets, versioned struct documents (StructDocument.h) of the type's format:
	//   { "Strada": { "Version": <format version>, "Type": "<Name>" }, "<Name>": { <the fields> } }
	// Reading checks the header (the type's name, a version from 1 to the type's FormatVersion), takes the content object,
	// runs the migrations from the file's version to the current one, reads the fields (missing fields keep their defaults,
	// unknown ones follow the context's policy, values outside their ranges refuse the file) and validates the result. Saving
	// writes the current version and every field. Main thread only: the types belong to the registry.
	class DataAssetSerializer
	{
	public:
		[[nodiscard]] static Json Serialize(DataAsset const& asset);
		// A new asset of the type, not known to the asset manager.
		[[nodiscard]] static Result<Ref<DataAsset>> Deserialize(DataAssetType const& type, Json const& document,
		                                                        DeserializationContext const& context);
		[[nodiscard]] static Result<void> SaveToFile(DataAsset const& asset, std::filesystem::path const& path);
		[[nodiscard]] static Result<Ref<DataAsset>> LoadFromFile(DataAssetType const& type, std::filesystem::path const& path,
		                                                         DeserializationContext const& context);
	};

	template<ReflectedStruct TData>
	Ref<DataAsset> TypedDataAssetType<TData>::CreateAsset() const
	{
		return CreateRef<TypedDataAsset<TData>>(std::static_pointer_cast<TypedDataAssetType<TData> const>(shared_from_this()));
	}

	template<ReflectedStruct TData>
	AssetType TypedDataAsset<TData>::GetStaticType()
	{
		TypedDataAssetType<TData> const* type = DataAssetRegistry::Find<TData>();
		return type != nullptr ? type->GetAssetType() : AssetType::None;
	}
}
