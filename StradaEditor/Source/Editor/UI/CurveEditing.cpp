#include "Editor/UI/CurveEditing.h"

#include "Editor/UI/CurveViewImpl.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>

namespace Strada
{
	namespace
	{
		Result<void> CheckKeys(Curve const& curve, std::span<CurveKeyRef const> keys)
		{
			for (auto const& [track, key] : keys)
			{
				if (track != 0 || key >= curve.Keys.size())
				{
					return MakeError("the curve has no key {} of track {}", key, track);
				}
			}
			return {};
		}

		template<typename TEdit>
		Result<void> EditKeys(Curve& curve, std::span<CurveKeyRef const> keys, TEdit&& edit)
		{
			if (Result<void> checked = CheckKeys(curve, keys); !checked)
			{
				return checked;
			}
			if (keys.empty())
			{
				return Error{"no key is selected"};
			}
			for (CurveKeyRef const& key : keys)
			{
				edit(curve.Keys[key.second]);
			}
			return {};
		}
	}

	Result<void> MoveCurveKeys(Curve& curve, std::span<CurveKeyRef const> keys, float timeOffset, float valueOffset)
	{
		if (Result<void> checked = CheckKeys(curve, keys); !checked)
		{
			return checked;
		}
		if (!std::isfinite(timeOffset) || !std::isfinite(valueOffset))
		{
			return Error{"the offsets must be finite numbers"};
		}
		std::set<uint32_t> selected;
		for (CurveKeyRef const& key : keys)
		{
			selected.insert(key.second);
		}
		for (uint32_t const key : selected)
		{
			if (!std::isfinite(curve.Keys[key].Value + valueOffset))
			{
				return Error{"a key's value is finite: the move takes it past the largest float"};
			}
		}
		// The time offsets the block may take: no selected key leaves the finite floats or passes an unselected neighbour.
		double lowest = -std::numeric_limits<double>::infinity();
		double highest = std::numeric_limits<double>::infinity();
		for (uint32_t const key : selected)
		{
			double const time = curve.Keys[key].Time;
			double lower = -static_cast<double>(std::numeric_limits<float>::max());
			double upper = static_cast<double>(std::numeric_limits<float>::max());
			for (uint32_t before = key; before-- > 0;)
			{
				if (!selected.contains(before))
				{
					lower = std::max(lower, static_cast<double>(curve.Keys[before].Time));
					break;
				}
			}
			for (uint32_t after = key + 1; after < curve.Keys.size(); after++)
			{
				if (!selected.contains(after))
				{
					upper = std::min(upper, static_cast<double>(curve.Keys[after].Time));
					break;
				}
			}
			lowest = std::max(lowest, lower - time);
			highest = std::min(highest, upper - time);
		}
		double const offset =
			selected.empty() ? 0.0 : std::clamp(static_cast<double>(timeOffset), std::min(lowest, 0.0), std::max(highest, 0.0));
		for (uint32_t const key : selected)
		{
			CurveKey& moved = curve.Keys[key];
			// The offset keeps the sum within the float bounds of the block in double, so the float it rounds to stays within
			// them too (rounding is monotonic and the bounds are floats).
			moved.Time = static_cast<float>(static_cast<double>(moved.Time) + offset);
			moved.Value += valueOffset;
		}
		return {};
	}

	Result<uint32_t> AddCurveKey(Curve& curve, float time, float value)
	{
		if (!std::isfinite(time))
		{
			return Error{"a key's time is a finite number"};
		}
		if (!std::isfinite(value))
		{
			return Error{"a key's value is finite"};
		}
		if (curve.Keys.size() >= MaxCurveKeys)
		{
			return MakeError("a curve has at most {} keys", MaxCurveKeys);
		}
		CurveKey key;
		key.Time = time;
		key.Value = value;
		auto const position = std::upper_bound(curve.Keys.begin(), curve.Keys.end(), time,
		                                       [](float candidate, CurveKey const& stored)
		                                       {
												   return candidate < stored.Time;
											   });
		auto const index = static_cast<uint32_t>(std::distance(curve.Keys.begin(), position));
		curve.Keys.insert(position, key);
		return index;
	}

	Result<void> RemoveCurveKeys(Curve& curve, std::span<CurveKeyRef const> keys)
	{
		if (Result<void> checked = CheckKeys(curve, keys); !checked)
		{
			return checked;
		}
		std::set<uint32_t> removed;
		for (CurveKeyRef const& key : keys)
		{
			removed.insert(key.second);
		}
		// remove_if reads each element before anything is moved onto it, so its address still says its index.
		CurveKey const* const first = curve.Keys.data();
		std::erase_if(curve.Keys,
		              [&removed, first](CurveKey const& key)
		              {
						  return removed.contains(static_cast<uint32_t>(&key - first));
					  });
		return {};
	}

	Result<void> SetCurveKeyValue(Curve& curve, uint32_t key, float value)
	{
		if (!std::isfinite(value))
		{
			return Error{"a key's value is finite"};
		}
		std::array<CurveKeyRef, 1> const keys = {CurveKeyRef(0, key)};
		return EditKeys(curve, keys,
		                [value](CurveKey& edited)
		                {
							edited.Value = value;
						});
	}

	Result<void> SetCurveKeyInterpolation(Curve& curve, std::span<CurveKeyRef const> keys, CurveInterpolation interpolation)
	{
		return EditKeys(curve, keys,
		                [interpolation](CurveKey& edited)
		                {
							edited.Interpolation = interpolation;
						});
	}

	Result<void> SetCurveKeyTangents(Curve& curve, uint32_t key, std::optional<float> in, std::optional<float> out)
	{
		if ((in && !std::isfinite(*in)) || (out && !std::isfinite(*out)))
		{
			return Error{"a tangent is a finite slope"};
		}
		std::array<CurveKeyRef, 1> const keys = {CurveKeyRef(0, key)};
		return EditKeys(curve, keys,
		                [&in, &out](CurveKey& edited)
		                {
							edited.HasIn = in.has_value();
							edited.In = in.value_or(edited.In);
							edited.HasOut = out.has_value();
							edited.Out = out.value_or(edited.Out);
						});
	}

	Result<void> FlattenCurveTangents(Curve& curve, std::span<CurveKeyRef const> keys)
	{
		return EditKeys(curve, keys,
		                [](CurveKey& edited)
		                {
							edited.HasIn = true;
							edited.In = 0.0f;
							edited.HasOut = true;
							edited.Out = 0.0f;
						});
	}

	Result<void> ResetCurveTangents(Curve& curve, std::span<CurveKeyRef const> keys)
	{
		return EditKeys(curve, keys,
		                [](CurveKey& edited)
		                {
							edited.HasIn = false;
							edited.HasOut = false;
						});
	}

	Result<uint32_t> CurveDocumentTraits<Curve>::AddKey(Curve& curve, uint32_t track, float time, glm::vec4 value)
	{
		if (track != 0)
		{
			return MakeError("the curve has no track {}", track);
		}
		return AddCurveKey(curve, time, value.x);
	}

	Result<void> CurveDocumentTraits<Curve>::SetKeyValue(Curve& curve, CurveKeyRef key, glm::vec4 value)
	{
		if (key.first != 0)
		{
			return MakeError("the curve has no track {}", key.first);
		}
		return SetCurveKeyValue(curve, key.second, value.x);
	}

	Result<void> CurveDocumentTraits<Curve>::SetKeyTangents(Curve& curve, CurveKeyRef key, std::optional<glm::vec4> in,
	                                                        std::optional<glm::vec4> out)
	{
		if (key.first != 0)
		{
			return MakeError("the curve has no track {}", key.first);
		}
		return SetCurveKeyTangents(curve, key.second, in ? std::optional<float>(in->x) : std::nullopt,
		                           out ? std::optional<float>(out->x) : std::nullopt);
	}

	template class CurveView<Curve>;
}
