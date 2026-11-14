#include "Asset/TestDataAsset.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/AssetRegistry.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <typeindex>
#include <utility>
#include <vector>

using namespace Strada;
using Testing::TestData;
using Testing::TestDataEntry;
using Testing::TestDataShape;
using Testing::TestDataTypeScope;

namespace Strada::Testing
{
	// A second data struct, for registrations that must not collide with TestData's struct.
	struct OtherData
	{
		float Value = 0.0f;
	};

	// A struct whose default lies outside its field's range.
	struct OutOfRangeData
	{
		float Value = -1.0f;
	};

	// Distinct data structs for filling the registry.
	template<size_t TIndex>
	struct NumberedData
	{
		int32_t Value = 0;
	};
}

namespace Strada
{
	template<>
	struct StructTraits<Testing::OtherData>
	{
		static constexpr std::string_view Name = "OtherData";
		static constexpr auto Fields = std::make_tuple(Field("Value", &Testing::OtherData::Value).Doc("A number."));
	};

	template<>
	struct StructTraits<Testing::OutOfRangeData>
	{
		static constexpr std::string_view Name = "OutOfRangeData";
		static constexpr auto Fields = std::make_tuple(Field("Value", &Testing::OutOfRangeData::Value).AtLeast(0.0).Doc("A number."));
	};

	template<size_t TIndex>
	struct StructTraits<Testing::NumberedData<TIndex>>
	{
		static constexpr std::string_view Name = "NumberedData";
		static constexpr auto Fields = std::make_tuple(Field("Value", &Testing::NumberedData<TIndex>::Value).Doc("A number."));
	};
}

namespace
{
	using Testing::OtherData;

	// The fields every value of a data asset has, through every nested struct and element, are described: no custom kinds
	// and no arrays of arrays.
	static_assert(Detail::IsDescribedFieldType<TestData>());
	static_assert(Detail::IsDescribedFieldType<std::vector<TestDataEntry>>());
	static_assert(!Detail::IsDescribedFieldType<std::vector<std::vector<float>>>());
	static_assert(!Detail::IsDescribedFieldType<Json>());
	static_assert(!Detail::IsDescribedFieldType<std::vector<Json>>());

	DataAssetTypeSpecification<OtherData> MakeOtherSpecification(std::string_view name, std::string_view extension)
	{
		DataAssetTypeSpecification<OtherData> specification;
		specification.Name = name;
		specification.Extension = extension;
		specification.Description = "A number (a type of the tests).";
		return specification;
	}

	// Registers OtherData under a name for the scope's lifetime.
	class OtherTypeScope
	{
	public:
		explicit OtherTypeScope(DataAssetTypeSpecification<OtherData> const& specification)
		{
			Result<AssetType> registered = DataAssetRegistry::Register(specification);
			REQUIRE_MESSAGE(registered.IsOk(), (registered ? std::string() : registered.GetError()));
			m_Type = registered.GetValue();
		}

		~OtherTypeScope() { CHECK(DataAssetRegistry::Unregister(m_Type).IsOk()); }

		OtherTypeScope(OtherTypeScope const&) = delete;
		OtherTypeScope& operator=(OtherTypeScope const&) = delete;

		AssetType GetType() const { return m_Type; }

	private:
		AssetType m_Type = AssetType::None;
	};

	size_t CountEngineTypes()
	{
		std::vector<DataAssetType const*> const types = DataAssetRegistry::GetTypes();
		return static_cast<size_t>(std::ranges::count_if(types,
		                                                 [](DataAssetType const* type)
		                                                 {
															 return type->IsEngineType();
														 }));
	}

	std::filesystem::path GetFixturePath(std::string_view name)
	{
		return FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "DataAssets" / "TestData" / FileSystem::PathFromUtf8(name);
	}

	std::string ReadFixture(std::string_view name)
	{
		Result<std::string> text = FileSystem::ReadTextFile(GetFixturePath(name));
		REQUIRE(text.IsOk());
		return text.TakeValue();
	}

	Json ParseFixture(std::string_view name)
	{
		Result<Json> document = ParseJson(ReadFixture(name));
		REQUIRE(document.IsOk());
		return document.TakeValue();
	}

	Json MakeTestDataDocument(Json content, int version = static_cast<int>(Testing::TestDataFormatVersion))
	{
		Json document = Json::object();
		document["Strada"] = MakeFileHeader("TestData", version);
		document["TestData"] = std::move(content);
		return document;
	}

	DataAssetType const& GetTestDataType()
	{
		DataAssetType const* type = DataAssetRegistry::FindByName("TestData");
		REQUIRE(type != nullptr);
		return *type;
	}

	TestData const& GetData(Ref<DataAsset> const& asset)
	{
		REQUIRE(asset != nullptr);
		return std::static_pointer_cast<TypedDataAsset<TestData>>(asset)->GetData();
	}

	Result<Ref<DataAsset>> Deserialize(Json const& document, DeserializationContext const& context = DeserializationContext{})
	{
		return DataAssetSerializer::Deserialize(GetTestDataType(), document, context);
	}

	std::string GetErrorOf(Result<Ref<DataAsset>> const& result)
	{
		REQUIRE(result.IsError());
		return result.GetError();
	}

	std::vector<std::string> GetTypeNames()
	{
		std::vector<std::string> names;
		for (DataAssetType const* type : DataAssetRegistry::GetTypes())
		{
			names.push_back(type->GetName());
		}
		return names;
	}

	template<size_t... TIndices>
	std::vector<Result<AssetType>> RegisterNumbered(std::index_sequence<TIndices...>)
	{
		std::vector<Result<AssetType>> results;
		(
			[&results]
			{
				DataAssetTypeSpecification<Testing::NumberedData<TIndices>> specification;
				std::string const name = fmt::format("Numbered{}", TIndices);
				std::string const extension = fmt::format(".snumbered{}", TIndices);
				specification.Name = name;
				specification.Extension = extension;
				specification.Description = "A number (a type of the tests).";
				results.push_back(DataAssetRegistry::Register(specification));
			}(),
			...);
		return results;
	}
}

TEST_CASE("DataAsset: a registered type takes the next value and is found by its value, name, extension and struct")
{
	size_t const engineTypes = CountEngineTypes();
	CHECK(DataAssetRegistry::GetTypes().size() == engineTypes);
	CHECK(DataAssetRegistry::FindByName("TestData") == nullptr);
	CHECK(DataAssetRegistry::Find<TestData>() == nullptr);
	CHECK(TypedDataAsset<TestData>::GetStaticType() == AssetType::None);

	uint64_t const epoch = DataAssetRegistry::GetEpoch();
	AssetType value = AssetType::None;
	{
		TestDataTypeScope const scope;
		value = scope.GetType();
		CHECK(DataAssetRegistry::GetEpoch() != epoch);
		CHECK(static_cast<uint32_t>(value) == DataAssetRegistry::FirstAssetTypeValue + engineTypes);
		CHECK(DataAssetRegistry::IsDataAssetValue(value));
		CHECK_FALSE(DataAssetRegistry::IsDataAssetValue(AssetType::VisualScript));

		DataAssetType const* type = DataAssetRegistry::Find(value);
		REQUIRE(type != nullptr);
		CHECK(DataAssetRegistry::FindByName("TestData") == type);
		CHECK(DataAssetRegistry::FindByName("testData") == nullptr);
		CHECK(DataAssetRegistry::FindByExtension(".stestdata") == type);
		CHECK(DataAssetRegistry::FindByExtension(".STestData") == type);
		CHECK(DataAssetRegistry::FindByExtension("stestdata") == nullptr);
		CHECK(DataAssetRegistry::Find<TestData>() == type);
		CHECK(DataAssetRegistry::Find<OtherData>() == nullptr);
		CHECK(DataAssetRegistry::Find(AssetType::Material) == nullptr);
		CHECK(TypedDataAsset<TestData>::GetStaticType() == value);
		CHECK(DataAssetRegistry::GetTypes().back() == type);
		CHECK(DataAssetRegistry::GetTypes().size() == engineTypes + 1);

		CHECK(type->GetAssetType() == value);
		CHECK(type->GetName() == "TestData");
		CHECK(type->GetExtension() == ".stestdata");
		CHECK(type->GetFormatVersion() == 2);
		CHECK(type->GetDescription() == "A shape with a size, references and weighted entries (a type of the tests).");
		CHECK(type->GetMigrations().size() == 1);
		CHECK(type->GetFormat().Type == "TestData");
		CHECK(type->GetFormat().Version == 2);
		CHECK(type->GetCommandDomain() == "test-data");
		CHECK(type->GetCommandParameter() == "testData");
		CHECK(type->GetDataType() == std::type_index(typeid(TestData)));
		CHECK_FALSE(type->IsEngineType());
		std::vector<std::string_view> fieldNames;
		for (FieldDescriptor const& field : type->GetFields())
		{
			fieldNames.push_back(field.Name);
		}
		CHECK(fieldNames == std::vector<std::string_view>{"Shape", "Size", "Texture", "Next", "Entries"});
		CHECK(type->GetFields()[4].Kind == FieldKind::Array);
		CHECK(type->GetFields()[4].ElementKind == FieldKind::Struct);
		CHECK(type->GetFields()[4].Hints.MaxCount == 16);
		CHECK(type->GetDefaultFields() == Json::parse(R"({ "Shape": "Box", "Size": 1.0, "Texture": "0", "Next": "0", "Entries": [] })"));

		// A new asset of the type has the default data and a version of its own.
		Ref<DataAsset> const first = type->CreateAsset();
		Ref<DataAsset> const second = type->CreateAsset();
		REQUIRE(first != nullptr);
		CHECK(first->GetAssetType() == value);
		CHECK(&first->GetDataAssetType() == type);
		CHECK_FALSE(first->Handle.IsValid());
		CHECK(GetData(first) == TestData{});
		CHECK(first->SerializeFields() == type->GetDefaultFields());
		CHECK(first->GetVersion() != second->GetVersion());
	}
	CHECK(DataAssetRegistry::Find(value) == nullptr);
	CHECK(DataAssetRegistry::FindByName("TestData") == nullptr);
	CHECK(DataAssetRegistry::FindByExtension(".stestdata") == nullptr);
	CHECK(DataAssetRegistry::Find<TestData>() == nullptr);
	CHECK(DataAssetRegistry::GetTypes().size() == engineTypes);

	// The value is free again, and the next registration takes it.
	TestDataTypeScope const again;
	CHECK(again.GetType() == value);
}

TEST_CASE("DataAsset: assets keep their type after it is unregistered")
{
	Ref<DataAsset> asset;
	{
		TestDataTypeScope const scope;
		asset = DataAssetRegistry::Find(scope.GetType())->CreateAsset();
	}
	REQUIRE(asset != nullptr);
	CHECK(asset->GetDataAssetType().GetName() == "TestData");
	CHECK(asset->SerializeFields().at("Size") == 1.0);
	// With no value: the type registered next takes the value, and the object is not one of its.
	CHECK(asset->GetAssetType() == AssetType::None);
	TestDataTypeScope const next;
	CHECK(asset->GetAssetType() != next.GetType());
}

TEST_CASE("DataAsset: a type is not unregistered while the asset manager holds an asset of it")
{
	Testing::AssetManagerScope const assets;
	Result<AssetType> const registered = DataAssetRegistry::Register(MakeOtherSpecification("Other", ".sother"));
	REQUIRE(registered.IsOk());
	AssetType const type = registered.GetValue();
	Ref<DataAsset> const kept = DataAssetRegistry::Find(type)->CreateAsset();
	AssetHandle const handle = AssetManager::AddMemoryAsset(kept, "Kept");

	// A memory asset needs no asset directory.
	REQUIRE_FALSE(AssetManager::HasAssetDirectory());
	CHECK(DataAssetRegistry::Unregister(type).GetError() ==
	      "data asset type Other cannot be unregistered while the asset manager holds assets of it");
	CHECK(DataAssetRegistry::Find(type) != nullptr);
	CHECK(kept->GetAssetType() == type);

	AssetManager::RemoveMemoryAsset(handle);
	REQUIRE(DataAssetRegistry::Unregister(type).IsOk());
	CHECK(kept->GetAssetType() == AssetType::None);
	CHECK(kept->GetDataAssetType().GetName() == "Other");

	// The next type takes the value; the object kept, added to the asset manager again, does not pass as one of its.
	TestDataTypeScope const next;
	CHECK(next.GetType() == type);
	AssetHandle const again = AssetManager::AddMemoryAsset(kept, "Again");
	CHECK(AssetManager::GetAsset<TypedDataAsset<TestData>>(again) == nullptr);
	CHECK(AssetManager::TryGetAsset<TypedDataAsset<TestData>>(again).IsError());
	// Nor as an asset of a class whose type is not registered, whose static type is None as well.
	CHECK(TypedDataAsset<Testing::OutOfRangeData>::GetStaticType() == AssetType::None);
	CHECK(AssetManager::GetAsset<TypedDataAsset<Testing::OutOfRangeData>>(again) == nullptr);
	AssetManager::RemoveMemoryAsset(again);
}

TEST_CASE("DataAsset: registrations that break a rule register nothing")
{
	TestDataTypeScope const testData;
	std::vector<std::string> const before = GetTypeNames();
	uint64_t const epoch = DataAssetRegistry::GetEpoch();

	struct Case
	{
		std::string_view What;
		DataAssetTypeSpecification<OtherData> Specification;
		std::string_view Error;
	};
	std::string const longName(65, 'A');
	std::array<DocumentMigration, 1> const migration = {&Testing::MigrateTestDataFrom1};
	std::array<DocumentMigration, 1> const noMigration = {nullptr};
	std::vector<Case> cases = {
		{"an empty name", MakeOtherSpecification("", ".sother"), "data asset type names have 1 to 64 characters, not 0"},
		{"a name of 65 characters", MakeOtherSpecification(longName, ".sother"), "data asset type names have 1 to 64 characters, not 65"},
		{"a name in lower camel case", MakeOtherSpecification("otherData", ".sother"),
	     "data asset type name 'otherData' is not PascalCase ASCII letters and digits"},
		{"a name with an underscore", MakeOtherSpecification("Other_Data", ".sother"),
	     "data asset type name 'Other_Data' is not PascalCase ASCII letters and digits"},
		{"a name with a letter outside ASCII", MakeOtherSpecification("Caf\xC3\xA9", ".sother"),
	     "data asset type name 'Caf\xC3\xA9' is not PascalCase ASCII letters and digits"},
		{"a name starting with a digit", MakeOtherSpecification("2D", ".sother"), "data asset type name '2D' is not PascalCase"},
		{"a built-in type's name", MakeOtherSpecification("Material", ".sother"), "'Material' is the name of a built-in asset type"},
		{"another built-in type's name", MakeOtherSpecification("VisualScript", ".sother"),
	     "'VisualScript' is the name of a built-in asset type"},
		{"the name of no type", MakeOtherSpecification("None", ".sother"), "'None' is the name of a built-in asset type"},
		{"a registered type's name", MakeOtherSpecification("TestData", ".sother"),
	     "data asset type TestData is registered already (TestData)"},
		{"a registered type's name in other letter case", MakeOtherSpecification("Testdata", ".sother"),
	     "data asset type Testdata is registered already (TestData)"},
		{"an extension without a dot", MakeOtherSpecification("Other", "sother"),
	     "data asset type Other: the extension 'sother' is not a dot and 2 to 16 lowercase ASCII letters or digits"},
		{"an extension in capitals", MakeOtherSpecification("Other", ".SOTHER"), "data asset type Other: the extension '.SOTHER' is not"},
		{"an extension of one letter", MakeOtherSpecification("Other", ".s"), "data asset type Other: the extension '.s' is not"},
		{"an extension of 17 letters", MakeOtherSpecification("Other", ".sabcdefghijklmnop"),
	     "data asset type Other: the extension '.sabcdefghijklmnop' is not"},
		{"an extension with a second dot", MakeOtherSpecification("Other", ".s.other"),
	     "data asset type Other: the extension '.s.other' is not"},
		{"a built-in type's extension", MakeOtherSpecification("Other", ".smat"),
	     "data asset type Other: '.smat' is the extension of the built-in asset type Material"},
		{"a font's extension", MakeOtherSpecification("Other", ".ttf"),
	     "data asset type Other: '.ttf' is the extension of the built-in asset type Font"},
		{"a registered type's extension", MakeOtherSpecification("Other", ".stestdata"),
	     "data asset type Other: the extension '.stestdata' is TestData's"},
		{"no description", MakeOtherSpecification("Other", ".sother"), "data asset type Other has no description"},
		{"version 0", MakeOtherSpecification("Other", ".sother"), "data asset type Other: the format version 0 is not a positive version"},
		{"version 2 without a migration", MakeOtherSpecification("Other", ".sother"),
	     "data asset type Other: version 2 needs 1 migrations, not 0"},
		{"version 1 with a migration", MakeOtherSpecification("Other", ".sother"),
	     "data asset type Other: version 1 needs 0 migrations, not 1"},
		{"a missing migration", MakeOtherSpecification("Other", ".sother"), "data asset type Other: a migration is missing"},
		{"a default value Validate refuses", MakeOtherSpecification("Other", ".sother"),
	     "data asset type Other: the default value cannot be an asset's: Other: never valid"},
		{"a built-in type's name in capitals", MakeOtherSpecification("MATERIAL", ".sother"),
	     "'MATERIAL' is the name of a built-in asset type (Material)"},
		{"a built-in type's name in other letter case", MakeOtherSpecification("Visualscript", ".sother"),
	     "'Visualscript' is the name of a built-in asset type (VisualScript)"},
		{"a command parameter that is the generated commands' fields", MakeOtherSpecification("Fields", ".sother"),
	     "data asset type Fields: its command parameter \"fields\" is a member of the generated commands"},
		{"a command parameter that is create's path", MakeOtherSpecification("Path", ".sother"),
	     "data asset type Path: its command parameter \"path\" is a member of the generated commands"},
	};
	cases[19].Specification.Description = {};
	cases[20].Specification.FormatVersion = 0;
	cases[21].Specification.FormatVersion = 2;
	cases[22].Specification.Migrations = migration;
	cases[23].Specification.FormatVersion = 2;
	cases[23].Specification.Migrations = noMigration;
	cases[24].Specification.Validate = [](OtherData const&) -> Result<void>
	{
		return Error{"never valid"};
	};

	for (Case const& refused : cases)
	{
		CAPTURE(refused.What);
		Result<AssetType> result = DataAssetRegistry::Register(refused.Specification);
		REQUIRE(result.IsError());
		CHECK_MESSAGE(result.GetError().starts_with(refused.Error), result.GetError());
		CHECK(GetTypeNames() == before);
		CHECK(DataAssetRegistry::GetEpoch() == epoch);
	}

	// A default value outside its field's range.
	DataAssetTypeSpecification<Testing::OutOfRangeData> outOfRange;
	outOfRange.Name = "OutOfRange";
	outOfRange.Extension = ".soutofrange";
	outOfRange.Description = "A number below its range (a type of the tests).";
	Result<AssetType> const outside = DataAssetRegistry::Register(outOfRange);
	REQUIRE(outside.IsError());
	CHECK(outside.GetError() ==
	      "data asset type OutOfRange: the default value cannot be an asset's: OutOfRangeData.Value: must be at least 0");

	// The same struct twice, and the same specification twice.
	{
		OtherTypeScope const other(MakeOtherSpecification("Other", ".sother"));
		Result<AssetType> const twice = DataAssetRegistry::Register(MakeOtherSpecification("Another", ".sanother"));
		REQUIRE(twice.IsError());
		CHECK(twice.GetError() == "data asset type Another: its data struct is registered already, as Other");
		CHECK(DataAssetRegistry::Register(Testing::MakeTestDataSpecification()).GetError() ==
		      "data asset type TestData is registered already (TestData)");
	}
	CHECK(GetTypeNames() == before);

	// Values no type has, and built-in values, cannot be unregistered.
	CHECK(DataAssetRegistry::Unregister(AssetType::Material).GetError() == "no data asset type has the value 4");
	CHECK(DataAssetRegistry::Unregister(static_cast<AssetType>(255)).IsError());
	for (DataAssetType const* type : DataAssetRegistry::GetTypes())
	{
		if (type->IsEngineType())
		{
			CHECK(DataAssetRegistry::Unregister(type->GetAssetType()).IsError());
		}
	}
	CHECK(GetTypeNames() == before);
}

TEST_CASE("DataAsset: types are not registered or unregistered while an asset directory is open")
{
	Testing::TemporaryDirectory directory;
	TestDataTypeScope const testData;
	Testing::AssetManagerScope const assets;
	REQUIRE(AssetManager::OpenAssetDirectory(directory.GetPath()).IsOk());

	CHECK(DataAssetRegistry::Register(MakeOtherSpecification("Other", ".sother")).GetError() ==
	      "data asset type Other cannot be registered while an asset directory is open");
	CHECK(DataAssetRegistry::Unregister(testData.GetType()).GetError() ==
	      "data asset type TestData cannot be unregistered while an asset directory is open");
	AssetManager::CloseAssetDirectory();

	// With the directory closed, and without an asset manager, they are.
	{
		OtherTypeScope const other(MakeOtherSpecification("Other", ".sother"));
		CHECK(DataAssetRegistry::FindByName("Other") != nullptr);
	}
	AssetManager::Shutdown();
	OtherTypeScope const other(MakeOtherSpecification("Other", ".sother"));
	CHECK(DataAssetRegistry::FindByName("Other") != nullptr);
}

TEST_CASE("DataAsset: the registry holds at most MaxTypes types")
{
	size_t const free = DataAssetRegistry::MaxTypes - CountEngineTypes();
	REQUIRE(free >= 1);
	REQUIRE(free <= DataAssetRegistry::MaxTypes);
	std::vector<Result<AssetType>> const results = RegisterNumbered(std::make_index_sequence<DataAssetRegistry::MaxTypes + 1>());
	for (size_t i = 0; i < results.size(); i++)
	{
		CAPTURE(i);
		CHECK(results[i].IsOk() == (i < free));
	}
	CHECK(results.back().GetError() == fmt::format("data asset type Numbered{}: 128 types are registered already", results.size() - 1));
	CHECK(DataAssetRegistry::GetTypes().size() == DataAssetRegistry::MaxTypes);
	CHECK(static_cast<uint32_t>(DataAssetRegistry::GetTypes().back()->GetAssetType()) == 255);
	for (Result<AssetType> const& result : results)
	{
		if (result)
		{
			CHECK(DataAssetRegistry::Unregister(result.GetValue()).IsOk());
		}
	}
	CHECK(DataAssetRegistry::GetTypes().size() == CountEngineTypes());
}

TEST_CASE("DataAsset: commands' domains are kebab case and their parameters lower camel case")
{
	struct Case
	{
		std::string_view Name;
		std::string_view Domain;
		std::string_view Parameter;
	};
	std::array<Case, 7> const cases = {{
		{"TestData", "test-data", "testData"},
		{"PhysicsMaterial", "physics-material", "physicsMaterial"},
		{"InputActions", "input-actions", "inputActions"},
		{"UIStyle", "ui-style", "uiStyle"},
		{"Curve2D", "curve2d", "curve2D"},
		{"Gadget", "gadget", "gadget"},
		{"UI", "ui", "ui"},
	}};
	for (Case const& expected : cases)
	{
		CAPTURE(expected.Name);
		OtherTypeScope const other(MakeOtherSpecification(expected.Name, ".sother"));
		DataAssetType const* type = DataAssetRegistry::Find(other.GetType());
		REQUIRE(type != nullptr);
		CHECK(type->GetCommandDomain() == expected.Domain);
		CHECK(type->GetCommandParameter() == expected.Parameter);
	}
}

TEST_CASE("DataAsset: the asset type lookups know registered types by name and extension")
{
	std::vector<std::string_view> const builtIn = GetSupportedAssetExtensions();
	AssetType type = AssetType::None;
	{
		TestDataTypeScope const scope;
		type = scope.GetType();
		CHECK(std::string_view(AssetTypeToString(type)) == "TestData");
		CHECK(AssetTypeFromString("TestData") == type);
		CHECK_FALSE(AssetTypeFromString("testdata").has_value());
		CHECK(GetAssetTypeForExtension(".stestdata") == type);
		CHECK(GetAssetTypeForExtension(".STESTDATA") == type);
		CHECK(GetBuiltInAssetTypeForExtension(".stestdata") == AssetType::None);
		CHECK_FALSE(BuiltInAssetTypeFromString("TestData").has_value());
		CHECK(BuiltInAssetTypeFromString("Material") == AssetType::Material);
		std::vector<std::string_view> const extensions = GetSupportedAssetExtensions();
		CHECK(extensions.size() == builtIn.size() + 1);
		CHECK(extensions.back() == ".stestdata");
	}
	CHECK(std::string_view(AssetTypeToString(type)) == "None");
	CHECK_FALSE(AssetTypeFromString("TestData").has_value());
	CHECK(GetAssetTypeForExtension(".stestdata") == AssetType::None);
	CHECK(GetSupportedAssetExtensions() == builtIn);
}

TEST_CASE("DataAsset: the visual script library and asset registries read a registered type's name")
{
	constexpr std::string_view Library =
		R"({ "Types": [ { "ID": "Strada.TestData", "Title": "Test Data", "Kind": "Asset", "FieldType": "Asset",
		"AssetType": "TestData", "Literal": true, "ImplicitTo": [] } ], "Nodes": [] })";
	Json registry = Json::object();
	registry["Strada"] = MakeFileHeader("AssetRegistry", 1);
	registry["Assets"] = Json::array({Json::object({{"Handle", "70001"}, {"Type", "TestData"}, {"Path", "Data/A.stestdata"}}),
	                                  Json::object({{"Handle", "70002"}, {"Type", "Material"}, {"Path", "Red.smat"}})});

	// Unknown, the type refuses the library, and its registry entries are skipped with a warning.
	Result<Scope<VisualScriptLibrary>> unknown = VisualScriptLibrary::Parse(Library);
	REQUIRE(unknown.IsError());
	CHECK(unknown.GetError() == "Types[0].AssetType: TestData is not an asset type");
	std::vector<std::string> warnings;
	DeserializationContext context;
	context.Warnings = &warnings;
	Result<AssetRegistry> skipped = AssetRegistry::Deserialize(registry, context);
	REQUIRE(skipped.IsOk());
	CHECK(skipped.GetValue().GetAll().size() == 1);
	CHECK(warnings == std::vector<std::string>{"Assets[0]: unknown asset type 'TestData'; entry skipped"});

	TestDataTypeScope const scope;
	Result<Scope<VisualScriptLibrary>> known = VisualScriptLibrary::Parse(Library);
	REQUIRE_MESSAGE(known.IsOk(), (known ? std::string() : known.GetError()));
	VisualScriptTypeInfo const* type = known.GetValue()->FindType("Strada.TestData");
	REQUIRE(type != nullptr);
	CHECK(type->AcceptedAssetType == scope.GetType());

	warnings.clear();
	Result<AssetRegistry> kept = AssetRegistry::Deserialize(registry, context);
	REQUIRE(kept.IsOk());
	CHECK(warnings.empty());
	REQUIRE(kept.GetValue().GetAll().size() == 2);
	AssetMetadata const* entry = kept.GetValue().Find(AssetHandle(UUID(70001)));
	REQUIRE(entry != nullptr);
	CHECK(entry->Type == scope.GetType());
	CHECK(kept.GetValue().Serialize() == registry);
}

TEST_CASE("DataAssetSerializer: files round trip byte for byte, and older versions migrate")
{
	TestDataTypeScope const scope;
	std::string const text = ReadFixture("Version2.stestdata");
	Result<Ref<DataAsset>> asset = DataAssetSerializer::LoadFromFile(GetTestDataType(), GetFixturePath("Version2.stestdata"), {});
	REQUIRE_MESSAGE(asset.IsOk(), (asset ? std::string() : asset.GetError()));
	TestData const& data = GetData(asset.GetValue());
	CHECK(data.Shape == TestDataShape::Sphere);
	CHECK(data.Size == 2.5f);
	CHECK(data.Texture == AssetHandle(UUID(8406934185603345521ull)));
	CHECK_FALSE(data.Next.IsValid());
	CHECK(data.Entries == std::vector<TestDataEntry>{{"First", 1.0f}});
	CHECK(DumpJson(DataAssetSerializer::Serialize(*asset.GetValue())) == text);

	// Version 1 called Size "Radius": it loads as the same data and saves as the current version.
	Result<Ref<DataAsset>> migrated = DataAssetSerializer::LoadFromFile(GetTestDataType(), GetFixturePath("Version1.stestdata"), {});
	REQUIRE_MESSAGE(migrated.IsOk(), (migrated ? std::string() : migrated.GetError()));
	CHECK(GetData(migrated.GetValue()) == data);
	CHECK(DumpJson(DataAssetSerializer::Serialize(*migrated.GetValue())) == text);

	// Saved and loaded again.
	Testing::TemporaryDirectory directory;
	std::filesystem::path const path = directory.GetPath() / "Copy.stestdata";
	REQUIRE(DataAssetSerializer::SaveToFile(*migrated.GetValue(), path).IsOk());
	Result<std::string> written = FileSystem::ReadTextFile(path);
	REQUIRE(written.IsOk());
	CHECK(written.GetValue() == text);

	// A version-1 file that has both names is refused, naming the version.
	Json both = ParseFixture("Version1.stestdata");
	both["TestData"]["Size"] = 1.0;
	CHECK(GetErrorOf(Deserialize(both)) == "TestData version 1 cannot be read: version 1 has \"Radius\", not \"Size\"");
	// What the migration leaves is read like any content.
	Json negative = ParseFixture("Version1.stestdata");
	negative["TestData"]["Radius"] = -1.0;
	CHECK(GetErrorOf(Deserialize(negative)) == "TestData.Size: must be at least 0");

	// Files that cannot be read name the file.
	std::filesystem::path const broken = directory.GetPath() / "Broken.stestdata";
	REQUIRE(FileSystem::WriteTextFile(broken, "{ \"Strada\": ").IsOk());
	Result<Ref<DataAsset>> unreadable = DataAssetSerializer::LoadFromFile(GetTestDataType(), broken, {});
	REQUIRE(unreadable.IsError());
	CHECK(unreadable.GetError().starts_with(fmt::format("'{}': ", FileSystem::PathToUtf8(broken))));
	CHECK(DataAssetSerializer::LoadFromFile(GetTestDataType(), directory.GetPath() / "Missing.stestdata", {}).IsError());
}

TEST_CASE("DataAssetSerializer: missing fields keep their defaults, and damaged content is refused with a reason")
{
	TestDataTypeScope const scope;

	Ref<DataAsset> const defaults = GetTestDataType().CreateAsset();
	Json const defaultDocument = DataAssetSerializer::Serialize(*defaults);
	CHECK(defaultDocument == MakeTestDataDocument(GetTestDataType().GetDefaultFields()));
	CHECK(defaultDocument.at("Strada") == Json::parse(R"({ "Version": 2, "Type": "TestData" })"));

	// Missing fields keep their defaults.
	Result<Ref<DataAsset>> partial = Deserialize(MakeTestDataDocument(Json::object({{"Size", 3.0}})));
	REQUIRE(partial.IsOk());
	TestData expected;
	expected.Size = 3.0f;
	CHECK(GetData(partial.GetValue()) == expected);
	Result<Ref<DataAsset>> empty = Deserialize(MakeTestDataDocument(Json::object()));
	REQUIRE(empty.IsOk());
	CHECK(GetData(empty.GetValue()) == TestData{});

	// Unknown fields warn under Warn and refuse under Error.
	Json const unknown = MakeTestDataDocument(Json::object({{"Size", 3.0}, {"Colour", "Red"}}));
	std::vector<std::string> warnings;
	DeserializationContext warn;
	warn.UnknownFields = UnknownFieldPolicy::Warn;
	warn.Warnings = &warnings;
	Result<Ref<DataAsset>> warned = Deserialize(unknown, warn);
	REQUIRE(warned.IsOk());
	CHECK(GetData(warned.GetValue()) == expected);
	CHECK(warnings == std::vector<std::string>{"ignored unknown field 'Colour' in data asset 'TestData'"});
	CHECK(GetErrorOf(Deserialize(unknown)) == "data asset 'TestData' has no field 'Colour' (fields: Shape, Size, Texture, Next, Entries)");

	// References resolve through the context; one to the asset itself or to nothing loads.
	DeserializationContext resolving;
	resolving.ResolveAssetReference = [](std::string_view reference) -> Result<AssetHandle>
	{
		if (reference == "asset://Textures/Bricks.png")
		{
			return AssetHandle(UUID(70002));
		}
		return MakeError("'{}' is not registered", reference);
	};
	Result<Ref<DataAsset>> resolved = Deserialize(
		MakeTestDataDocument(Json::object({{"Texture", "asset://Textures/Bricks.png"}, {"Next", "18446744073709551615"}})), resolving);
	REQUIRE(resolved.IsOk());
	CHECK(GetData(resolved.GetValue()).Texture == AssetHandle(UUID(70002)));
	CHECK(GetData(resolved.GetValue()).Next == AssetHandle(UUID(18446744073709551615ull)));
	CHECK(GetErrorOf(Deserialize(MakeTestDataDocument(Json::object({{"Texture", "asset://Missing.png"}})), resolving)) ==
	      "TestData.Texture: 'asset://Missing.png' is not registered");

	struct Case
	{
		std::string_view What;
		Json Document;
		std::string_view Error;
	};
	Json wrongType = MakeTestDataDocument(Json::object());
	wrongType["Strada"]["Type"] = "Material";
	Json noContent = MakeTestDataDocument(Json::object());
	noContent.erase("TestData");
	std::vector<TestDataEntry> seventeen;
	for (int i = 0; i < 17; i++)
	{
		seventeen.push_back({fmt::format("Entry{}", i), 1.0f});
	}
	std::vector<Case> const cases = {
		{"not an object", Json::array(), "document must be a JSON object"},
		{"no header", Json::object({{"TestData", Json::object()}}), "missing \"Strada\" file header"},
		{"another type's header", wrongType, "expected a TestData file but found 'Material'"},
		{"version 0", MakeTestDataDocument(Json::object(), 0), "invalid file version 0"},
		{"version 3", MakeTestDataDocument(Json::object(), 3), "file version 3 is newer than the supported version 2"},
		{"no content", noContent, "the document has no \"TestData\" object"},
		{"content of another kind", MakeTestDataDocument(Json::array()), "\"TestData\" must be a JSON object"},
		{"a value of another kind", MakeTestDataDocument(Json::object({{"Size", "big"}})), "TestData.Size: expected a number"},
		{"a value outside its range", MakeTestDataDocument(Json::object({{"Size", -0.5}})), "TestData.Size: must be at least 0"},
		{"an unknown enumerator", MakeTestDataDocument(Json::object({{"Shape", "Cone"}})), "TestData.Shape: expected one of Box|Sphere"},
		{"an element outside its range", MakeTestDataDocument(Json::parse(R"({ "Entries": [ { "Name": "A", "Weight": 11 } ] })")),
	     "TestData.Entries: element 0: TestDataEntry.Weight: must be between 0 and 10"},
		{"more elements than MaxCount",
	     MakeTestDataDocument(Json::object({{"Entries", JsonTraits<std::vector<TestDataEntry>>::ToJson(seventeen)}})),
	     "TestData.Entries: must hold at most 16 elements"},
		{"a value Validate refuses",
	     MakeTestDataDocument(Json::parse(R"({ "Entries": [ { "Name": "A" }, { "Name": "B" }, { "Name": "A" } ] })")),
	     "TestData: two entries are named 'A'"},
	};
	for (Case const& refused : cases)
	{
		CAPTURE(refused.What);
		CHECK(GetErrorOf(Deserialize(refused.Document)) == refused.Error);
	}
}

TEST_CASE("DataAsset: changes are all or nothing, and only changes draw a version")
{
	TestDataTypeScope const scope;
	Ref<DataAsset> const asset = GetTestDataType().CreateAsset();
	auto const typed = std::static_pointer_cast<TypedDataAsset<TestData>>(asset);
	uint64_t const created = asset->GetVersion();
	uint64_t epoch = DataAssetRegistry::GetEpoch();
	CHECK(epoch >= created);

	// A patch that changes the data draws a version and moves the epoch.
	REQUIRE(asset->ApplyFields(Json::object({{"Size", 4.0}, {"Shape", "Sphere"}}), {}).IsOk());
	uint64_t const patched = asset->GetVersion();
	CHECK(patched != created);
	CHECK(DataAssetRegistry::GetEpoch() != epoch);
	CHECK(typed->GetData().Size == 4.0f);
	CHECK(typed->GetData().Shape == TestDataShape::Sphere);

	// One that leaves it as it was draws none.
	epoch = DataAssetRegistry::GetEpoch();
	REQUIRE(asset->ApplyFields(Json::object({{"Size", 4.0}}), {}).IsOk());
	REQUIRE(asset->ApplyFields(Json::object(), {}).IsOk());
	CHECK(asset->GetVersion() == patched);
	CHECK(DataAssetRegistry::GetEpoch() == epoch);

	// Refused patches change nothing: a later field outside its range, Validate, an unknown field.
	TestData const before = typed->GetData();
	std::vector<Json> const refused = {
		Json::object({{"Shape", "Box"}, {"Size", -1.0}}),
		Json::parse(R"({ "Size": 2.0, "Entries": [ { "Name": "A" }, { "Name": "A" } ] })"),
		Json::object({{"Size", 2.0}, {"Radius", 1.0}}),
		Json::array(),
	};
	for (Json const& patch : refused)
	{
		CAPTURE(patch.dump());
		CHECK(asset->ApplyFields(patch, {}).IsError());
		CHECK(typed->GetData() == before);
		CHECK(asset->GetVersion() == patched);
	}
	CHECK(DataAssetRegistry::GetEpoch() == epoch);

	// SetData checks what reading would: ranges in elements, MaxCount, numbers JSON cannot hold, Validate.
	TestData invalid = before;
	invalid.Entries = {{"A", 12.0f}};
	CHECK(typed->SetData(invalid).GetError() == "TestData.Entries: element 0: TestDataEntry.Weight: must be between 0 and 10");
	invalid.Entries.assign(17, {"A", 1.0f});
	CHECK(typed->SetData(invalid).GetError() == "TestData.Entries: must hold at most 16 elements");
	invalid = before;
	invalid.Size = std::numeric_limits<float>::infinity();
	CHECK(typed->SetData(invalid).IsError());
	invalid.Size = std::numeric_limits<float>::quiet_NaN();
	CHECK(typed->SetData(invalid).IsError());
	invalid = before;
	invalid.Entries = {{"A", 1.0f}, {"A", 2.0f}};
	CHECK(typed->SetData(invalid).GetError() == "TestData: two entries are named 'A'");
	CHECK(typed->GetData() == before);
	CHECK(asset->GetVersion() == patched);

	REQUIRE(typed->SetData(before).IsOk());
	CHECK(asset->GetVersion() == patched);
	TestData changed = before;
	changed.Entries = {{"A", 1.0f}, {"B", 2.0f}};
	REQUIRE(typed->SetData(changed).IsOk());
	CHECK(typed->GetData() == changed);
	CHECK(asset->GetVersion() != patched);
	CHECK(asset->SerializeFields().at("Entries").size() == 2);
}

TEST_CASE("DataAsset: the asset manager scans, loads and reloads data asset files")
{
	TestDataTypeScope const scope;
	Testing::TemporaryDirectory directory;
	Testing::AssetManagerScope const assets;
	std::filesystem::path const data = directory.GetPath() / "Data";
	REQUIRE(FileSystem::CreateDirectories(data).IsOk());
	REQUIRE(FileSystem::WriteTextFile(data / "Ball.stestdata", ReadFixture("Version2.stestdata")).IsOk());
	REQUIRE(FileSystem::WriteTextFile(data / "Old.STESTDATA", ReadFixture("Version1.stestdata")).IsOk());
	REQUIRE(FileSystem::WriteTextFile(data / "Broken.stestdata", "{}").IsOk());
	REQUIRE(AssetManager::OpenAssetDirectory(directory.GetPath()).IsOk());

	AssetHandle const ball = AssetManager::FindByPath("Data/Ball.stestdata");
	AssetHandle const old = AssetManager::FindByPath("Data/Old.STESTDATA");
	AssetHandle const broken = AssetManager::FindByPath("Data/Broken.stestdata");
	REQUIRE(ball.IsValid());
	REQUIRE(old.IsValid());
	REQUIRE(broken.IsValid());
	CHECK(AssetManager::GetAssetType(ball) == scope.GetType());
	CHECK(AssetManager::GetAssets(scope.GetType()).size() == 3);

	// The registry saved the type by name.
	Result<std::string> registry = FileSystem::ReadTextFile(directory.GetPath() / AssetRegistry::FileName);
	REQUIRE(registry.IsOk());
	CHECK(registry.GetValue().find("\"Type\": \"TestData\"") != std::string::npos);

	Ref<TypedDataAsset<TestData>> const loaded = AssetManager::GetAsset<TypedDataAsset<TestData>>(ball);
	REQUIRE(loaded != nullptr);
	CHECK(loaded->Handle == ball);
	CHECK(loaded->GetData().Size == 2.5f);
	CHECK(AssetManager::GetAsset<MaterialAsset>(ball) == nullptr);
	CHECK(AssetManager::TryGetAsset<MaterialAsset>(ball).GetError() == fmt::format("asset {} is a TestData, not a Material", ball));
	Ref<TypedDataAsset<TestData>> const migrated = AssetManager::GetAsset<TypedDataAsset<TestData>>(old);
	REQUIRE(migrated != nullptr);
	CHECK(migrated->GetData() == loaded->GetData());

	// A file that does not load is remembered.
	Result<Ref<Asset>> failed = AssetManager::LoadAsset(broken);
	REQUIRE(failed.IsError());
	CHECK(failed.GetError() == fmt::format("failed to load 'Data/Broken.stestdata': '{}': missing \"Strada\" file header",
	                                       FileSystem::PathToUtf8(data / "Broken.stestdata")));
	CHECK(AssetManager::HasLoadFailure(broken));

	// Loading never loads what the asset refers to: a reference to itself, or to another asset.
	REQUIRE(FileSystem::WriteTextFile(data / "Loop.stestdata",
	                                  DumpJson(MakeTestDataDocument(Json::object(
										  {{"Next", "asset://Data/Loop.stestdata"}, {"Texture", "asset://Data/Ball.stestdata"}}))))
	            .IsOk());
	Result<AssetRefreshResult> refreshed = AssetManager::Refresh();
	REQUIRE(refreshed.IsOk());
	AssetHandle const loop = AssetManager::FindByPath("Data/Loop.stestdata");
	AssetManager::UnloadAsset(ball);
	Ref<TypedDataAsset<TestData>> const looping = AssetManager::GetAsset<TypedDataAsset<TestData>>(loop);
	REQUIRE(looping != nullptr);
	CHECK(looping->GetData().Next == loop);
	CHECK(looping->GetData().Texture == ball);
	CHECK_FALSE(AssetManager::IsLoaded(ball));
}

TEST_CASE("DataAsset: hot reload keeps an object the editor wrote and replaces one changed outside")
{
	TestDataTypeScope const scope;
	Testing::TemporaryDirectory directory;
	Testing::AssetManagerScope const assets;
	std::filesystem::path const path = directory.GetPath() / "Ball.stestdata";
	REQUIRE(FileSystem::WriteTextFile(path, ReadFixture("Version2.stestdata")).IsOk());
	REQUIRE(AssetManager::OpenAssetDirectory(directory.GetPath()).IsOk());
	AssetHandle const ball = AssetManager::FindByPath("Ball.stestdata");

	Ref<TypedDataAsset<TestData>> const asset = AssetManager::GetAsset<TypedDataAsset<TestData>>(ball);
	REQUIRE(asset != nullptr);
	uint64_t const loadedVersion = asset->GetVersion();

	// An edit the editor makes: the data changes, the file is written and the write recorded. The object stays.
	REQUIRE(asset->ApplyFields(Json::object({{"Size", 6.0}}), {}).IsOk());
	uint64_t const editedVersion = asset->GetVersion();
	CHECK(editedVersion != loadedVersion);
	REQUIRE(DataAssetSerializer::SaveToFile(*asset, path).IsOk());
	AssetManager::RecordFileWrite(ball);
	Result<AssetRefreshResult> kept = AssetManager::Refresh();
	REQUIRE(kept.IsOk());
	CHECK(kept.GetValue().Modified.empty());
	CHECK(AssetManager::GetAsset<TypedDataAsset<TestData>>(ball) == asset);
	CHECK(asset->GetVersion() == editedVersion);

	// A change made outside: the next refresh unloads the asset (the epoch moves), and the next lookup loads another object
	// with another version.
	Json changed = ParseFixture("Version2.stestdata");
	changed["TestData"]["Size"] = 9.0;
	REQUIRE(FileSystem::WriteTextFile(path, DumpJson(changed)).IsOk());
	std::error_code errorCode;
	std::filesystem::last_write_time(path, std::filesystem::last_write_time(path) + std::chrono::seconds(5), errorCode);
	REQUIRE_FALSE(errorCode);
	uint64_t const epoch = DataAssetRegistry::GetEpoch();
	Result<AssetRefreshResult> modified = AssetManager::Refresh();
	REQUIRE(modified.IsOk());
	CHECK(modified.GetValue().Modified == std::vector<AssetHandle>{ball});
	CHECK(DataAssetRegistry::GetEpoch() != epoch);
	CHECK_FALSE(AssetManager::IsLoaded(ball));
	Ref<TypedDataAsset<TestData>> const reloaded = AssetManager::GetAsset<TypedDataAsset<TestData>>(ball);
	REQUIRE(reloaded != nullptr);
	CHECK(reloaded != asset);
	CHECK(reloaded->GetData().Size == 9.0f);
	CHECK(reloaded->GetVersion() != editedVersion);
	CHECK(reloaded->GetVersion() != loadedVersion);
	CHECK(asset->GetData().Size == 6.0f);

	// Dropping data assets moves the epoch: unloading one, removing a memory asset, closing the directory, shutting down.
	uint64_t before = DataAssetRegistry::GetEpoch();
	AssetManager::UnloadAsset(ball);
	CHECK(DataAssetRegistry::GetEpoch() != before);
	before = DataAssetRegistry::GetEpoch();
	AssetManager::UnloadAsset(ball);
	CHECK(DataAssetRegistry::GetEpoch() == before);
	AssetHandle const memory = AssetManager::AddMemoryAsset(GetTestDataType().CreateAsset(), "Memory");
	CHECK(AssetManager::GetAssetType(memory) == scope.GetType());
	before = DataAssetRegistry::GetEpoch();
	AssetManager::RemoveMemoryAsset(memory);
	CHECK(DataAssetRegistry::GetEpoch() != before);
	before = DataAssetRegistry::GetEpoch();
	AssetManager::CloseAssetDirectory();
	CHECK(DataAssetRegistry::GetEpoch() != before);
	before = DataAssetRegistry::GetEpoch();
	AssetManager::Shutdown();
	CHECK(DataAssetRegistry::GetEpoch() != before);
}
