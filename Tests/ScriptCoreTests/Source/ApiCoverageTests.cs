using System;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Reflection.Metadata;
using System.Reflection.PortableExecutable;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The scripts of Projects/FeatureTest, which CI runs through the game player, use every public member of the scripting
// API: the API cannot grow without being exercised. Usage is read from the scripts' compiled metadata: references to the
// API's types, methods (property accessors and operators included) and fields, matched by signature so that each
// overload counts on its own; constructors and Script's callbacks count when overridden or called by derived types.
// Constants and enumerators compile to literals and overrides of System.Object methods are called through their base
// declarations, so neither leaves a trace: their types must be used instead. The API has no generic types: references
// to their members would name instantiations, which this check does not map back (it would list the members as unused).
public sealed class ApiCoverageTests
{
	private const string FeatureTestAssemblyName = "FeatureTest.dll";

	[Fact]
	public void TheFeatureTestScriptsUseEveryPublicMember()
	{
		string path = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "FeatureTest", FeatureTestAssemblyName));
		Assert.True(File.Exists(path), $"{path} is missing: build the feature-test scripts (CMake target StradaFeatureTestScripts)");
		Usage usage = Usage.Read(path);

		List<string> unused = [];
		foreach (Type type in typeof(Script).Assembly.GetExportedTypes())
		{
			if (!usage.Types.Contains(Names.Of(type)) && !usage.DerivedFrom.Contains(Names.Of(type)))
			{
				unused.Add(Names.Of(type));
			}
			if (type.IsEnum)
			{
				continue;
			}
			foreach (MemberInfo member in GetCoverableMembers(type))
			{
				if (!IsUsed(member, usage))
				{
					unused.Add(Names.Describe(member));
				}
			}
		}
		unused.Sort(StringComparer.Ordinal);
		Assert.True(unused.Count == 0,
			$"{unused.Count} public members of the scripting API are not used by Projects/FeatureTest's scripts:\n{string.Join("\n", unused)}");
	}

	private static IEnumerable<MemberInfo> GetCoverableMembers(Type type)
	{
		const BindingFlags declared = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static |
									  BindingFlags.DeclaredOnly;
		foreach (MethodInfo method in type.GetMethods(declared))
		{
			bool overridesObject = method.GetBaseDefinition().DeclaringType == typeof(object);
			if (IsVisible(method, type) && !overridesObject)
			{
				yield return method;
			}
		}
		foreach (ConstructorInfo constructor in type.GetConstructors(declared))
		{
			if (IsVisible(constructor, type))
			{
				yield return constructor;
			}
		}
		foreach (FieldInfo field in type.GetFields(declared))
		{
			bool visible = field.IsPublic || ((field.IsFamily || field.IsFamilyOrAssembly) && !type.IsSealed);
			if (visible && !field.IsLiteral && !field.IsSpecialName)
			{
				yield return field;
			}
		}
	}

	private static bool IsVisible(MethodBase method, Type type) =>
		method.IsPublic || ((method.IsFamily || method.IsFamilyOrAssembly) && !type.IsSealed);

	private static bool IsUsed(MemberInfo member, Usage usage)
	{
		string declaringType = Names.Of(member.DeclaringType!);
		switch (member)
		{
			case ConstructorInfo constructor:
				// Derived types call protected constructors from their own.
				return usage.Members.Contains(Names.Key(constructor)) || (constructor.IsFamily && usage.DerivedFrom.Contains(declaringType));
			case MethodInfo method:
				// Script's callbacks run when derived types override them.
				return usage.Members.Contains(Names.Key(method)) ||
					   (method.IsVirtual && usage.Overrides.Contains($"{declaringType}::{method.Name}"));
			case FieldInfo field:
				return usage.Members.Contains(Names.Key(field));
			default:
				return false;
		}
	}

	// Canonical names shared by reflection and metadata: "Strada.Vector3", "System.Nullable`1<Strada.Matrix4>",
	// "Strada.RaycastHit&", "!!0" for a generic method's first parameter.
	private static class Names
	{
		public static string Of(Type type)
		{
			if (type.IsByRef)
			{
				return Of(type.GetElementType()!) + "&";
			}
			if (type.IsPointer)
			{
				return Of(type.GetElementType()!) + "*";
			}
			if (type.IsSZArray)
			{
				return Of(type.GetElementType()!) + "[]";
			}
			if (type.IsGenericMethodParameter)
			{
				return "!!" + type.GenericParameterPosition;
			}
			if (type.IsGenericTypeParameter)
			{
				return "!" + type.GenericParameterPosition;
			}
			if (type.IsConstructedGenericType)
			{
				return $"{Of(type.GetGenericTypeDefinition())}<{string.Join(",", type.GetGenericArguments().Select(Of))}>";
			}
			return type.FullName!;
		}

		public static string Key(MethodBase method) =>
			$"{Of(method.DeclaringType!)}::{method.Name}({string.Join(",", method.GetParameters().Select(parameter => Of(parameter.ParameterType)))})";

		public static string Key(FieldInfo field) => $"{Of(field.DeclaringType!)}::{field.Name}";

		public static string Describe(MemberInfo member) => member switch
		{
			MethodBase method => Key(method),
			FieldInfo field => Key(field),
			_ => member.ToString() ?? member.Name,
		};
	}

	// What the feature-test assembly uses from the scripting API.
	private sealed class Usage
	{
		private const string ApiAssemblyName = "Strada.ScriptCore";

		public HashSet<string> Types { get; } = [];
		public HashSet<string> Members { get; } = [];
		// API types the assembly's types derive from, directly or through other types of the assembly.
		public HashSet<string> DerivedFrom { get; } = [];
		// "<API type>::<method>" for methods declared by types deriving from that API type.
		public HashSet<string> Overrides { get; } = [];

		public static Usage Read(string path)
		{
			using FileStream stream = File.OpenRead(path);
			using PEReader peReader = new(stream);
			MetadataReader reader = peReader.GetMetadataReader();
			Usage usage = new();
			SignatureNames provider = new();

			foreach (TypeReferenceHandle handle in reader.TypeReferences)
			{
				if (IsApiType(reader, handle))
				{
					usage.Types.Add(provider.GetTypeFromReference(reader, handle, 0));
				}
			}
			foreach (MemberReferenceHandle handle in reader.MemberReferences)
			{
				MemberReference member = reader.GetMemberReference(handle);
				if (member.Parent.Kind != HandleKind.TypeReference || !IsApiType(reader, (TypeReferenceHandle)member.Parent))
				{
					continue;
				}
				string type = provider.GetTypeFromReference(reader, (TypeReferenceHandle)member.Parent, 0);
				string name = reader.GetString(member.Name);
				if (member.GetKind() == MemberReferenceKind.Field)
				{
					usage.Members.Add($"{type}::{name}");
					continue;
				}
				MethodSignature<string> signature = member.DecodeMethodSignature(provider, null);
				usage.Members.Add($"{type}::{name}({string.Join(",", signature.ParameterTypes)})");
			}
			foreach (TypeDefinitionHandle handle in reader.TypeDefinitions)
			{
				List<string> ancestors = GetApiAncestors(reader, handle, provider);
				TypeDefinition definition = reader.GetTypeDefinition(handle);
				foreach (string ancestor in ancestors)
				{
					usage.DerivedFrom.Add(ancestor);
					foreach (MethodDefinitionHandle method in definition.GetMethods())
					{
						usage.Overrides.Add($"{ancestor}::{reader.GetString(reader.GetMethodDefinition(method).Name)}");
					}
				}
			}
			return usage;
		}

		private static bool IsApiType(MetadataReader reader, TypeReferenceHandle handle)
		{
			EntityHandle scope = reader.GetTypeReference(handle).ResolutionScope;
			return scope.Kind switch
			{
				HandleKind.AssemblyReference => reader.StringComparer.Equals(reader.GetAssemblyReference((AssemblyReferenceHandle)scope).Name,
					ApiAssemblyName),
				HandleKind.TypeReference => IsApiType(reader, (TypeReferenceHandle)scope),
				_ => false,
			};
		}

		// The API types a type of the assembly derives from (all of them along the chain).
		private static List<string> GetApiAncestors(MetadataReader reader, TypeDefinitionHandle handle, SignatureNames provider)
		{
			List<string> ancestors = [];
			EntityHandle baseType = reader.GetTypeDefinition(handle).BaseType;
			while (!baseType.IsNil)
			{
				if (baseType.Kind == HandleKind.TypeDefinition)
				{
					baseType = reader.GetTypeDefinition((TypeDefinitionHandle)baseType).BaseType;
					continue;
				}
				if (baseType.Kind == HandleKind.TypeReference && IsApiType(reader, (TypeReferenceHandle)baseType))
				{
					string name = provider.GetTypeFromReference(reader, (TypeReferenceHandle)baseType, 0);
					// The rest of the chain lives in the API assembly.
					for (Type? type = typeof(Script).Assembly.GetType(name); type != null && type.Assembly == typeof(Script).Assembly;
						 type = type.BaseType)
					{
						ancestors.Add(Names.Of(type));
					}
				}
				break;
			}
			return ancestors;
		}
	}

	// Metadata signatures decoded into the canonical names of Names.Of.
	private sealed class SignatureNames : ISignatureTypeProvider<string, object?>
	{
		public string GetPrimitiveType(PrimitiveTypeCode typeCode) => typeCode switch
		{
			PrimitiveTypeCode.Void => typeof(void).FullName!,
			PrimitiveTypeCode.Boolean => typeof(bool).FullName!,
			PrimitiveTypeCode.Char => typeof(char).FullName!,
			PrimitiveTypeCode.SByte => typeof(sbyte).FullName!,
			PrimitiveTypeCode.Byte => typeof(byte).FullName!,
			PrimitiveTypeCode.Int16 => typeof(short).FullName!,
			PrimitiveTypeCode.UInt16 => typeof(ushort).FullName!,
			PrimitiveTypeCode.Int32 => typeof(int).FullName!,
			PrimitiveTypeCode.UInt32 => typeof(uint).FullName!,
			PrimitiveTypeCode.Int64 => typeof(long).FullName!,
			PrimitiveTypeCode.UInt64 => typeof(ulong).FullName!,
			PrimitiveTypeCode.Single => typeof(float).FullName!,
			PrimitiveTypeCode.Double => typeof(double).FullName!,
			PrimitiveTypeCode.String => typeof(string).FullName!,
			PrimitiveTypeCode.Object => typeof(object).FullName!,
			PrimitiveTypeCode.IntPtr => typeof(IntPtr).FullName!,
			PrimitiveTypeCode.UIntPtr => typeof(UIntPtr).FullName!,
			PrimitiveTypeCode.TypedReference => typeof(TypedReference).FullName!,
			_ => typeCode.ToString(),
		};

		public string GetTypeFromDefinition(MetadataReader reader, TypeDefinitionHandle handle, byte rawTypeKind)
		{
			TypeDefinition definition = reader.GetTypeDefinition(handle);
			string name = reader.GetString(definition.Name);
			TypeDefinitionHandle declaring = definition.GetDeclaringType();
			return !declaring.IsNil
				? $"{GetTypeFromDefinition(reader, declaring, 0)}+{name}"
				: Join(reader.GetString(definition.Namespace), name);
		}

		public string GetTypeFromReference(MetadataReader reader, TypeReferenceHandle handle, byte rawTypeKind)
		{
			TypeReference reference = reader.GetTypeReference(handle);
			string name = reader.GetString(reference.Name);
			return reference.ResolutionScope.Kind == HandleKind.TypeReference
				? $"{GetTypeFromReference(reader, (TypeReferenceHandle)reference.ResolutionScope, 0)}+{name}"
				: Join(reader.GetString(reference.Namespace), name);
		}

		public string GetTypeFromSpecification(MetadataReader reader, object? genericContext, TypeSpecificationHandle handle, byte rawTypeKind) =>
			reader.GetTypeSpecification(handle).DecodeSignature(this, genericContext);

		public string GetSZArrayType(string elementType) => elementType + "[]";

		public string GetArrayType(string elementType, ArrayShape shape) => $"{elementType}[{new string(',', shape.Rank - 1)}]";

		public string GetByReferenceType(string elementType) => elementType + "&";

		public string GetPointerType(string elementType) => elementType + "*";

		public string GetPinnedType(string elementType) => elementType;

		public string GetGenericInstantiation(string genericType, ImmutableArray<string> typeArguments) =>
			$"{genericType}<{string.Join(",", typeArguments)}>";

		public string GetGenericMethodParameter(object? genericContext, int index) => "!!" + index;

		public string GetGenericTypeParameter(object? genericContext, int index) => "!" + index;

		public string GetModifiedType(string modifier, string unmodifiedType, bool isRequired) => unmodifiedType;

		public string GetFunctionPointerType(MethodSignature<string> signature) => "method*";

		private static string Join(string ns, string name) => ns.Length == 0 ? name : $"{ns}.{name}";
	}
}
