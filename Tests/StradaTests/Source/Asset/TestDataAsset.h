#pragma once

#include "Strada/Asset/DataAsset.h"
#include "Strada/Core/Result.h"
#include "Strada/Serialization/StructDocument.h"
#include "Strada/Serialization/StructSerialization.h"

#include <doctest/doctest.h>

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The data asset type the tests register (Docs/Architecture.md §6.4, the worked example): an enum, a number with a range,
// references to a texture and to another asset of its own type, and an array of structs with a bound, in its second
// format version. Shared by StradaTests and StradaEditorTests.
namespace Strada::Testing
{
	enum class TestDataShape : uint8_t
	{
		Box = 0,
		Sphere
	};

	struct TestDataEntry
	{
		std::string Name;
		float Weight = 1.0f;

		bool operator==(TestDataEntry const& other) const = default;
	};

	struct TestData
	{
		TestDataShape Shape = TestDataShape::Box;
		float Size = 1.0f;
		AssetHandle Texture;
		AssetHandle Next;
		std::vector<TestDataEntry> Entries;

		bool operator==(TestData const& other) const = default;
	};
}

namespace Strada
{
	template<>
	struct EnumTraits<Testing::TestDataShape>
	{
		static constexpr std::array<std::pair<Testing::TestDataShape, std::string_view>, 2> Values = {{
			{Testing::TestDataShape::Box, "Box"},
			{Testing::TestDataShape::Sphere, "Sphere"},
		}};
	};

	template<>
	struct StructTraits<Testing::TestDataEntry>
	{
		static constexpr std::string_view Name = "TestDataEntry";
		static constexpr auto Fields =
			std::make_tuple(Field("Name", &Testing::TestDataEntry::Name).Doc("Unique within the asset."),
		                    Field("Weight", &Testing::TestDataEntry::Weight).Range(0.0, 10.0).Doc("How much the entry counts."));
	};

	template<>
	struct StructTraits<Testing::TestData>
	{
		static constexpr std::string_view Name = "TestData";
		static constexpr auto Fields =
			std::make_tuple(Field("Shape", &Testing::TestData::Shape).Doc("The shape the size measures."),
		                    Field("Size", &Testing::TestData::Size).AtLeast(0.0).Doc("Edge length of a box, diameter of a sphere."),
		                    Field("Texture", &Testing::TestData::Texture).References("Texture").Doc("A texture the asset refers to."),
		                    Field("Next", &Testing::TestData::Next).References("TestData").Doc("Another asset of the type."),
		                    Field("Entries", &Testing::TestData::Entries).MaxCount(16).Doc("Weighted entries, in order."));
	};
}

namespace Strada::Testing
{
	inline constexpr std::string_view TestDataName = "TestData";
	inline constexpr std::string_view TestDataExtension = ".stestdata";
	inline constexpr uint32_t TestDataFormatVersion = 2;

	// Version 1 called Size "Radius".
	inline Result<void> MigrateTestDataFrom1(Json& content)
	{
		if (auto const radius = content.find("Radius"); radius != content.end())
		{
			if (content.contains("Size"))
			{
				return Error{"version 1 has \"Radius\", not \"Size\""};
			}
			Json size = *radius;
			content.erase(radius);
			content["Size"] = std::move(size);
		}
		return {};
	}

	inline constexpr std::array<DocumentMigration, 1> TestDataMigrations = {&MigrateTestDataFrom1};

	// Entry names are unique.
	inline Result<void> ValidateTestData(TestData const& data)
	{
		for (size_t i = 0; i < data.Entries.size(); i++)
		{
			for (size_t j = i + 1; j < data.Entries.size(); j++)
			{
				if (data.Entries[i].Name == data.Entries[j].Name)
				{
					return MakeError("two entries are named '{}'", data.Entries[i].Name);
				}
			}
		}
		return {};
	}

	inline DataAssetTypeSpecification<TestData> MakeTestDataSpecification()
	{
		DataAssetTypeSpecification<TestData> specification;
		specification.Name = TestDataName;
		specification.Extension = TestDataExtension;
		specification.FormatVersion = TestDataFormatVersion;
		specification.Description = "A shape with a size, references and weighted entries (a type of the tests).";
		specification.Migrations = TestDataMigrations;
		specification.Validate = &ValidateTestData;
		return specification;
	}

	// Registers TestData for the scope's lifetime. Declare it before the asset manager's scope (or before opening an asset
	// directory), so that it unregisters after the directory closed.
	class TestDataTypeScope
	{
	public:
		TestDataTypeScope()
		{
			Result<AssetType> registered = DataAssetRegistry::Register(MakeTestDataSpecification());
			REQUIRE_MESSAGE(registered.IsOk(), (registered ? std::string() : registered.GetError()));
			m_Type = registered.GetValue();
		}

		~TestDataTypeScope()
		{
			Result<void> unregistered = DataAssetRegistry::Unregister(m_Type);
			CHECK_MESSAGE(unregistered.IsOk(), (unregistered ? std::string() : unregistered.GetError()));
		}

		TestDataTypeScope(TestDataTypeScope const&) = delete;
		TestDataTypeScope& operator=(TestDataTypeScope const&) = delete;

		AssetType GetType() const { return m_Type; }

	private:
		AssetType m_Type = AssetType::None;
	};
}
