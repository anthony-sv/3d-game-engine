#pragma once

#include "Strada/Asset/Asset.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Serialization/JsonSerialization.h"
#include "Strada/Serialization/StructSerialization.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <typeindex>
#include <vector>

// Data assets: asset types made of one reflected struct, each from one registration (Docs/Architecture.md §6.4). A
// registration names the type, its file extension, its format version and the struct whose field table (StructTraits)
// describes the data; from it come the versioned file, its loader and saver, the AssetType and extension mapping, a
// version for hot reload, the editor's asset editor and commands, and membership in the registry-wide fuzz test.
namespace Strada
{
	class DataAsset;

	// Upgrades the content object of a data asset file (the member named after the type, without the header) from one format
	// version to the next, in place. It may assume only that the content is a JSON object: what it reads it checks first,
	// and it refuses what it cannot convert. The loader also refuses the file when a migration throws (nlohmann throws on a
	// value of the wrong kind), so a missed check refuses instead of crashing. Pure; any thread.
	using DataAssetMigration = Result<void> (*)(Json& content);

	// What one registration names. The data struct TData has a field table (StructTraits<TData>) whose field descriptions,
	// ranges and hints are the type's documentation, editor and automation schema. TData lives in the Asset module or below
	// it (Core, Math, Serialization), so that every host registers the same types (DataAssetRegistry).
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
		std::span<DataAssetMigration const> Migrations;
		// Rules the field table cannot state (names that must be unique, keys in order, counts that depend on each other);
		// called with every value an asset is about to take (loaded, patched or set). Null when the field table says all.
		// Pure; any thread.
		Result<void> (*Validate)(TData const& data) = nullptr;
	};

	// One registered data asset type: what its registration named, and what the registry derived from it. Owned by the
	// DataAssetRegistry, which keeps it at a stable address until the registry shuts down. Main thread only, like the
	// registry.
	class DataAssetType
	{
	public:
		virtual ~DataAssetType() = default;

		DataAssetType(DataAssetType const&) = delete;
		DataAssetType& operator=(DataAssetType const&) = delete;

		// The AssetType value the registry assigned: DataAssetRegistry::FirstAssetTypeValue and up, in registration order.
		AssetType GetAssetType() const;
		std::string const& GetName() const;
		// Lowercase, with the dot.
		std::string const& GetExtension() const;
		uint32_t GetFormatVersion() const;
		std::string const& GetDescription() const;
		std::span<DataAssetMigration const> GetMigrations() const;
		// The domain of the type's automation commands: the name in kebab case ("PhysicsMaterial": "physics-material").
		std::string const& GetCommandDomain() const;
		// The name of the commands' asset parameter and of the asset in their results: the name in lower camel case
		// ("PhysicsMaterial": "physicsMaterial").
		std::string const& GetCommandParameter() const;
		// The data struct's fields as generic editors and schemas see them (GetFieldDescriptors).
		std::span<FieldDescriptor const> GetFields() const;
		// The content object of a value-initialized TData: every field at its default.
		Json const& GetDefaultFields() const;
		// The struct's C++ type, which the registry finds the type by (DataAssetRegistry::Find<TData>).
		std::type_index GetDataType() const;

		// A new asset of the type with the default data, not known to the asset manager (its Handle is invalid).
		[[nodiscard]] virtual Ref<DataAsset> CreateAsset() const = 0;

	protected:
		DataAssetType(std::type_index dataType, std::string name, std::string extension, uint32_t formatVersion, std::string description,
		              std::vector<DataAssetMigration> migrations, std::vector<FieldDescriptor> fields, Json defaultFields);

	private:
		friend class DataAssetRegistry;

		AssetType m_AssetType = AssetType::None;
		std::type_index m_DataType;
		std::string m_Name;
		std::string m_Extension;
		uint32_t m_FormatVersion = 1;
		std::string m_Description;
		std::vector<DataAssetMigration> m_Migrations;
		std::string m_CommandDomain;
		std::string m_CommandParameter;
		std::vector<FieldDescriptor> m_Fields;
		Json m_DefaultFields;
	};

	// The registration of a data struct TData: the typed half of DataAssetType, which keeps the specification's validation.
	template<ReflectedStruct TData>
	class TypedDataAssetType final : public DataAssetType
	{
	public:
		explicit TypedDataAssetType(DataAssetTypeSpecification<TData> const& specification);

		[[nodiscard]] Ref<DataAsset> CreateAsset() const override;
		// The specification's Validate, or success without one.
		[[nodiscard]] Result<void> Validate(TData const& data) const;

	private:
		Result<void> (*m_Validate)(TData const& data) = nullptr;
	};

	// Base class of every loaded data asset: its type, its data as JSON, and the version that tells users it changed. The
	// typed data is TypedDataAsset<TData>'s. Main thread only.
	class DataAsset : public Asset
	{
	public:
		AssetType GetAssetType() const final;
		DataAssetType const& GetDataAssetType() const;

		// A number no other data asset object had or will have within the process: drawn when the object is created and again
		// by every change of its data. Users keep the version of what they derived from the asset and compare it at their
		// next use; a refresh that reloads the file creates another object, whose version differs as well.
		uint64_t GetVersion() const;

		// The data as the content object of its file: every field, in field table order.
		[[nodiscard]] virtual Json SerializeFields() const = 0;
		// Applies a partial patch (fields missing from it keep their values; unknown fields follow the context's policy). The
		// data changes only when every field reads, lies in its range and the whole value passes the type's Validate;
		// nothing changes otherwise. A change draws a new version; a patch that leaves the data as it was draws none.
		[[nodiscard]] virtual Result<void> ApplyFields(Json const& patch, DeserializationContext const& context) = 0;

	protected:
		explicit DataAsset(DataAssetType const& type);

		// Draws the version of a change (DataAssetRegistry::DrawVersion).
		void MarkChanged();

	private:
		DataAssetType const* m_Type = nullptr;
		uint64_t m_Version = 0;
	};

	// A loaded data asset of the registered struct TData: what C++ users read (AssetManager::GetAsset<TypedDataAsset<TData>>).
	template<ReflectedStruct TData>
	class TypedDataAsset final : public DataAsset
	{
	public:
		explicit TypedDataAsset(TypedDataAssetType<TData> const& type);

		// The registered type of TData; AssetType::None when TData is not registered (then no asset is of this class).
		static AssetType GetStaticType();

		// Valid until the next change of the data.
		TData const& GetData() const;
		// Takes the data when it would serialize into JSON that reads back (every number within its field's range) and it
		// passes the type's Validate; nothing changes otherwise. Draws a new version when the data differs.
		[[nodiscard]] Result<void> SetData(TData data);

		[[nodiscard]] Json SerializeFields() const override;
		[[nodiscard]] Result<void> ApplyFields(Json const& patch, DeserializationContext const& context) override;

	private:
		TData m_Data{};
	};

	// The registry of data asset types: the engine's own types, registered when it initializes, and types tests register.
	// Its lifetime is the AssetManager's (AssetManager::Init initializes it first, Shutdown shuts it down last), so every host
	// that opens a project (the editor, the player, tests) knows the same types: an asset registry keeps no entry of a type
	// it does not know, and a scan does not register files of an unknown extension, so a host that missed a type would drop
	// the handles of its files when it saves the registry.
	//
	// AssetTypeToString, AssetTypeFromString, GetAssetTypeForExtension and GetSupportedAssetExtensions (Asset.h) consult it
	// after the built-in types. Main thread only.
	class DataAssetRegistry
	{
	public:
		// Data asset types take the AssetType values from here up, in registration order; the built-in AssetType enumerators
		// stay below.
		static constexpr uint32_t FirstAssetTypeValue = 128;
		// The most types registered at once (the values left in AssetType's uint8_t).
		static constexpr uint32_t MaxTypes = 256 - FirstAssetTypeValue;

		// Registers the engine's types (one list, in DataAsset.cpp), asserting each succeeds. Called by AssetManager::Init.
		static void Init();
		// Forgets every type. Called by AssetManager::Shutdown, after every asset was dropped.
		static void Shutdown();
		static bool IsInitialized();

		// Registers TData as a data asset type and returns its AssetType. Fails, registering nothing, when the name, the
		// extension, the version or the migrations break DataAssetTypeSpecification's rules, when they collide with a built-in
		// or registered type, when a value-initialized TData is outside its field ranges or fails Validate, when TData or
		// MaxTypes types are registered already, or when the asset manager has an asset directory open (its files were scanned
		// without the type). Types stay registered until the registry shuts down. The editor's commands of a type exist only
		// when it was registered before RegisterEditorCommands ran.
		template<ReflectedStruct TData>
		[[nodiscard]] static Result<AssetType> Register(DataAssetTypeSpecification<TData> const& specification);

		// Null when the value is no registered data asset type (built-in types included).
		static DataAssetType const* Find(AssetType type);
		// By name, matched exactly.
		static DataAssetType const* FindByName(std::string_view name);
		// By extension with the dot, in any letter case.
		static DataAssetType const* FindByExtension(std::string_view extension);
		// By data struct.
		template<ReflectedStruct TData>
		static TypedDataAssetType<TData> const* Find();
		// Every registered type, in registration order.
		static std::vector<DataAssetType const*> GetTypes();

		// Changes whenever what a data asset handle resolves to may have changed: a data asset object was created (loaded,
		// reloaded or made in memory), changed its data, or was unloaded by the asset manager. Never repeats within the
		// process (it outlives the registry's shutdown). The scripting runtime compares it before running scripts to tell
		// managed caches of data assets to read again.
		static uint64_t GetEpoch();
		// The asset manager unloaded a data asset: moves the epoch.
		static void NotifyUnloaded();

	private:
		friend class DataAsset;

		// A new version for a data asset object: the epoch, moved first.
		static uint64_t DrawVersion();
		// Checks the type's specification and the registry's state, then assigns the AssetType value and keeps the type.
		[[nodiscard]] static Result<AssetType> Add(Scope<DataAssetType> type);
		static DataAssetType const* FindByDataType(std::type_index dataType);
	};

	// Files of data assets:
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
}
