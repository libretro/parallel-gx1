// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControllerEmu/StickGate.h"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <type_traits>

#include "Common/MathUtil.h"
#include "Common/Matrix.h"
#include "Common/StringUtil.h"
#include "InputCommon/ControllerEmu/Control/Control.h"
#include "InputCommon/ControllerEmu/ControlGroup/AnalogStick.h"
#include "InputCommon/ControllerEmu/ControlGroup/Cursor.h"
#include "InputCommon/ControllerEmu/ControlGroup/Force.h"
#include "InputCommon/ControllerEmu/ControlGroup/Tilt.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"

namespace
{
constexpr auto CALIBRATION_CONFIG_NAME = "Calibration";
constexpr auto CALIBRATION_DEFAULT_VALUE = 1.0;
constexpr auto CALIBRATION_CONFIG_SCALE = 100;

constexpr auto CENTER_CONFIG_NAME = "Center";
constexpr auto CENTER_CONFIG_SCALE = 100;

// Calculate distance to intersection of a ray with a line segment defined by two points.
// Returns false, leaving distance as it was, when there is none.
static bool GetRayLineIntersection(Common::DVec2 ray, Common::DVec2 point1, Common::DVec2 point2,
                                   double* distance)
{
  const auto diff = point2 - point1;

  const auto dot = diff.Dot({-ray.y, ray.x});
  if (std::abs(dot) < 0.00001)
  {
    // Both points are on top of each other.
    return false;
  }

  const auto segment_position = point1.Dot({ray.y, -ray.x}) / dot;
  if (segment_position < -0.00001 || segment_position > 1.00001)
  {
    // Ray does not pass through segment.
    return false;
  }

  *distance = diff.Cross(-point1) / dot;
  return true;
}

double GetNearestNotch(double angle, double virtual_notch_angle)
{
  constexpr auto sides = 8;
  constexpr auto rounding = MathUtil::TAU / sides;
  const auto closest_notch = std::round(angle / rounding) * rounding;
  const auto angle_diff =
      std::fmod(angle - closest_notch + MathUtil::PI, MathUtil::TAU) - MathUtil::PI;
  return std::abs(angle_diff) < virtual_notch_angle / 2 ? closest_notch : angle;
}

Common::DVec2 GetPointFromAngleAndLength(double angle, double length)
{
  return Common::DVec2{std::cos(angle), std::sin(angle)} * length;
}
}  // namespace

namespace ControllerEmu
{
constexpr int ReshapableInput::CALIBRATION_SAMPLE_COUNT;

ControlState StickGate::GetRadiusAtAngle(double angle) const
{
  switch (m_shape)
  {
  case Shape::Octagon:
  {
    constexpr int sides = 8;
    constexpr double sum_int_angles = (sides - 2) * MathUtil::PI;
    constexpr double half_int_angle = sum_int_angles / sides / 2;

    angle = std::fmod(angle, MathUtil::TAU / sides);
    // Solve ASA triangle using The Law of Sines:
    return m_size / std::sin(MathUtil::PI - angle - half_int_angle) * std::sin(half_int_angle);
  }
  case Shape::Round:
    return m_size;
  case Shape::Square:
  default:
  {
    constexpr double section_angle = MathUtil::TAU / 4;
    return m_size /
           std::cos(std::fmod(angle + section_angle / 2, section_angle) - section_angle / 2);
  }
  }
}

std::optional<u32> StickGate::GetIdealCalibrationSampleCount() const
{
  switch (m_shape)
  {
  case Shape::Octagon:
    return 8;
  case Shape::Round:
    // The "radius" is the same at every angle so a single sample is enough.
    return 1;
  case Shape::Square:
  default:
    // Because angle:0 points to the right we must use 8 samples for our square.
    return 8;
  }
}

ReshapableInput::ReshapableInput(std::string name_, std::string ui_name_, GroupType type_,
                                 ReshapeKind kind)
    : ControlGroup(std::move(name_), std::move(ui_name_), type_), m_reshape_kind(kind)
{
  // 50 is not always enough but users can set it to more with an expression
  AddDeadzoneSetting(&m_deadzone_setting, 50);
}

ReshapableInput::~ReshapableInput()
{
  delete static_cast<Shape*>(retro_atomic_exchange_ptr(&m_shape, nullptr));
}

void ReshapableInput::FreeShape(quiesce_node_t* node)
{
  delete reinterpret_cast<Shape*>(node);
}

const ReshapableInput::Shape& ReshapableInput::PeekShape() const
{
  static const Shape s_default{};
  const auto* shape = static_cast<const Shape*>(retro_atomic_load_acquire_ptr(&m_shape));
  return shape ? *shape : s_default;
}

template <typename Edit>
void ReshapableInput::EditShape(Edit&& edit)
{
  ciface::InputReadScope scope;
  for (;;)
  {
    void* const cur = retro_atomic_load_acquire_ptr(&m_shape);
    const Shape& from = cur ? *static_cast<const Shape*>(cur) : PeekShape();
    auto* next = new Shape{{}, from.calibration, from.center};
    edit(*next);
    if (retro_atomic_cas_ptr(&m_shape, cur, next))
    {
      if (cur)
        quiesce_retire(ciface::InputQuiesce(), &static_cast<Shape*>(cur)->node, FreeShape);
      return;
    }
    delete next;
  }
}

ControlState ReshapableInput::GetInputRadiusAtAngle(double angle) const
{
  ciface::InputReadScope scope;
  const CalibrationData& calibration = PeekShape().calibration;

  // Handle the "default" state.
  if (calibration.empty())
  {
    return GetDefaultInputRadiusAtAngle(angle);
  }

  return GetCalibrationDataRadiusAtAngle(calibration, angle);
}

ControlState ReshapableInput::GetDeadzonePercentage() const
{
  return m_deadzone_setting.GetValue() / 100;
}

ControlState ReshapableInput::GetCalibrationDataRadiusAtAngle(const CalibrationData& data,
                                                              double angle)
{
  const auto sample_pos = angle / MathUtil::TAU * data.size();
  // Interpolate the radius between 2 calibration samples.
  const u32 sample1_index = u32(sample_pos) % data.size();
  const u32 sample2_index = (sample1_index + 1) % data.size();
  const double sample1_angle = sample1_index * MathUtil::TAU / data.size();
  const double sample2_angle = sample2_index * MathUtil::TAU / data.size();

  // There is no intersection when the points are on top of each other.
  double radius = data[sample1_index];
  GetRayLineIntersection(GetPointFromAngleAndLength(angle, 1.0),
                         GetPointFromAngleAndLength(sample1_angle, data[sample1_index]),
                         GetPointFromAngleAndLength(sample2_angle, data[sample2_index]), &radius);
  return radius;
}

/* The call on the group this is: its own, an analog stick's for the octagonal one when it has
 * none of its own, or, when neither has one, the base's default (named Fn##Default). */
#define RESHAPE_HAS_OWN(D, Fn) (!std::is_same_v<decltype(&D::Fn), decltype(&ReshapableInput::Fn)>)

#define RESHAPE_CALL(Fn, ...)                                                                      \
  const auto call = [&](const auto* group) {                                                       \
    using D = std::remove_cv_t<std::remove_pointer_t<decltype(group)>>;                            \
    if constexpr (RESHAPE_HAS_OWN(D, Fn))                                                          \
      return group->Fn(__VA_ARGS__);                                                               \
    else                                                                                           \
      return Fn##Default(__VA_ARGS__);                                                             \
  };                                                                                               \
  switch (m_reshape_kind)                                                                          \
  {                                                                                                \
  case ReshapeKind::OctagonAnalogStick:                                                            \
    return call(static_cast<const OctagonAnalogStick*>(this));                                     \
  case ReshapeKind::Force:                                                                         \
    return call(static_cast<const Force*>(this));                                                  \
  case ReshapeKind::Tilt:                                                                          \
    return call(static_cast<const Tilt*>(this));                                                   \
  case ReshapeKind::Cursor:                                                                        \
    return call(static_cast<const Cursor*>(this));                                                 \
  case ReshapeKind::AnalogStick:                                                                   \
  default:                                                                                         \
    return call(static_cast<const AnalogStick*>(this));                                            \
  }

ControlState ReshapableInput::GetVirtualNotchSize() const {RESHAPE_CALL(GetVirtualNotchSize)}

ControlState ReshapableInput::GetGateRadiusAtAngle(double angle) const
{
  const auto call = [&](const auto* group) { return group->GetGateRadiusAtAngle(angle); };
  switch (m_reshape_kind)
  {
  case ReshapeKind::OctagonAnalogStick:
    return call(static_cast<const OctagonAnalogStick*>(this));
  case ReshapeKind::Force:
    return call(static_cast<const Force*>(this));
  case ReshapeKind::Tilt:
    return call(static_cast<const Tilt*>(this));
  case ReshapeKind::Cursor:
    return call(static_cast<const Cursor*>(this));
  case ReshapeKind::AnalogStick:
  default:
    return call(static_cast<const AnalogStick*>(this));
  }
}

ReshapableInput::ReshapeData ReshapableInput::GetReshapableState(bool adjusted) const
{
  const auto call = [&](const auto* group) { return group->GetReshapableState(adjusted); };
  switch (m_reshape_kind)
  {
  case ReshapeKind::OctagonAnalogStick:
    return call(static_cast<const OctagonAnalogStick*>(this));
  case ReshapeKind::Force:
    return call(static_cast<const Force*>(this));
  case ReshapeKind::Tilt:
    return call(static_cast<const Tilt*>(this));
  case ReshapeKind::Cursor:
    return call(static_cast<const Cursor*>(this));
  case ReshapeKind::AnalogStick:
  default:
    return call(static_cast<const AnalogStick*>(this));
  }
}

ControlState ReshapableInput::GetDefaultInputRadiusAtAngle(double angle) const
{
  RESHAPE_CALL(GetDefaultInputRadiusAtAngle, angle)
}

void ReshapableInput::SetCalibrationFromGate(const StickGate& gate)
{
  CalibrationData calibration(
      gate.GetIdealCalibrationSampleCount().value_or(CALIBRATION_SAMPLE_COUNT));

  u32 i = 0;
  for (auto& val : calibration)
    val = gate.GetRadiusAtAngle(MathUtil::TAU * i++ / calibration.size());

  SetCalibrationData(std::move(calibration));
}

ReshapableInput::CalibrationData ReshapableInput::GetCalibrationData() const
{
  ciface::InputReadScope scope;
  return PeekShape().calibration;
}

void ReshapableInput::SetCalibrationData(CalibrationData data)
{
  EditShape([&data](Shape& shape) { shape.calibration = data; });
}

ReshapableInput::ReshapeData ReshapableInput::GetCenter() const
{
  ciface::InputReadScope scope;
  return PeekShape().center;
}

void ReshapableInput::LoadConfig(Common::IniFile::Section* section, const char* base_name)
{
  char group[INI_KEY_SIZE];
  char key[INI_KEY_SIZE];
  LoadConfigDefault(section, base_name);

  Common::JoinIniKey(group, sizeof(group), base_name, name.c_str(), "/");

  // Special handling for "Modifier" button "Range" settings which default to 50% instead of 100%.
  if (const auto* modifier_input = GetModifierInput())
  {
    double range = 50.0;
    Common::JoinIniKey(key, sizeof(key), group, modifier_input->name.c_str(), "/Range");
    section->Get(key, &range, 50.0);
    modifier_input->control_ref->SetRange(range / 100);
  }

  std::string load_str;
  Common::JoinIniKey(key, sizeof(key), group, CALIBRATION_CONFIG_NAME);
  section->Get(key, &load_str, "");
  const auto load_data = SplitString(load_str, ' ');

  CalibrationData calibration(load_data.size(), CALIBRATION_DEFAULT_VALUE);

  auto it = load_data.begin();
  for (auto& sample : calibration)
  {
    if (TryParse(*(it++), &sample))
      sample /= CALIBRATION_CONFIG_SCALE;
  }

  Common::JoinIniKey(key, sizeof(key), group, CENTER_CONFIG_NAME);
  section->Get(key, &load_str, "");
  const auto center_data = SplitString(load_str, ' ');

  ReshapeData center = Common::DVec2();

  if (center_data.size() == 2)
  {
    if (TryParse(center_data[0], &center.x))
      center.x /= CENTER_CONFIG_SCALE;

    if (TryParse(center_data[1], &center.y))
      center.y /= CENTER_CONFIG_SCALE;
  }

  EditShape([&calibration, &center](Shape& shape) {
    shape.calibration = calibration;
    shape.center = center;
  });
}

void ReshapableInput::SaveConfig(Common::IniFile::Section* section, const char* base_name)
{
  char group[INI_KEY_SIZE];
  char key[INI_KEY_SIZE];
  SaveConfigDefault(section, base_name);

  if (!Common::JoinIniKey(group, sizeof(group), base_name, name.c_str(), "/"))
    return;

  // Special handling for "Modifier" button "Range" settings which default to 50% instead of 100%.
  if (const auto* modifier_input = GetModifierInput())
  {
    if (Common::JoinIniKey(key, sizeof(key), group, modifier_input->name.c_str(), "/Range"))
      section->Set(key, modifier_input->control_ref->GetRange() * 100, 50.0);
  }

  const CalibrationData calibration = GetCalibrationData();
  const ReshapeData center = GetCenter();
  // each sample as "%.2f", a space between
  std::string calibration_data;
  calibration_data.reserve(calibration.size() * 8);
  for (const ControlState val : calibration)
  {
    char text[32];
    CharArrayFromFormat(text, "%s%.2f", calibration_data.empty() ? "" : " ",
                        static_cast<double>(val * CALIBRATION_CONFIG_SCALE));
    calibration_data += text;
  }
  if (Common::JoinIniKey(key, sizeof(key), group, CALIBRATION_CONFIG_NAME))
    section->Set(key, calibration_data, "");

  // Save center value.
  char center_data[64];
  CharArrayFromFormat(center_data, "%.2f %.2f", static_cast<double>(center.x * CENTER_CONFIG_SCALE),
                      static_cast<double>(center.y * CENTER_CONFIG_SCALE));
  if (Common::JoinIniKey(key, sizeof(key), group, CENTER_CONFIG_NAME))
    section->Set(key, std::string(center_data), "0.00 0.00");
}

ReshapableInput::ReshapeData ReshapableInput::Reshape(ControlState x, ControlState y,
                                                      ControlState modifier,
                                                      ControlState clamp) const
{
  ReshapeData center;
  {
    ciface::InputReadScope scope;
    center = PeekShape().center;
  }
  x -= center.x;
  y -= center.y;

  // We run this even if both x and y will be zero.
  // In that case, std::atan2(0, 0) returns a valid non-NaN value, but the exact value
  // (which depends on the signs of x and y) does not matter here as dist is zero

  // TODO: make the AtAngle functions work with negative angles:
  ControlState angle = std::atan2(y, x) + MathUtil::TAU;

  const ControlState input_max_dist = GetInputRadiusAtAngle(angle);
  ControlState gate_max_dist = GetGateRadiusAtAngle(angle);

  // If input radius (from calibration) is zero apply no scaling to prevent division by zero.
  const ControlState max_dist = input_max_dist ? input_max_dist : gate_max_dist;

  ControlState dist = Common::DVec2{x, y}.Length() / max_dist;

  const double virtual_notch_size = GetVirtualNotchSize();

  if (virtual_notch_size > 0.0 && dist >= MINIMUM_NOTCH_DISTANCE)
  {
    angle = GetNearestNotch(angle, virtual_notch_size);
    gate_max_dist = GetGateRadiusAtAngle(angle);
  }

  // If the modifier is pressed, scale the distance by the modifier's value.
  // This is affected by the modifier's "range" setting which defaults to 50%.
  if (modifier)
  {
    dist *= modifier;
  }

  // Apply deadzone as a percentage of the user-defined calibration shape/size:
  dist = ApplyDeadzone(dist, GetDeadzonePercentage());

  // Scale to the gate shape/radius:
  dist *= gate_max_dist;

  return {std::clamp(std::cos(angle) * dist, -clamp, clamp),
          std::clamp(std::sin(angle) * dist, -clamp, clamp)};
}

Control* ReshapableInput::GetModifierInput() const
{
  RESHAPE_CALL(GetModifierInput)
}

}  // namespace ControllerEmu
