// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <vector>

#include <retro_atomic.h>

#include "Common/Matrix.h"
#include "Common/Quiesce.h"

#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"

namespace ControllerEmu
{
// Minimum stick distance from the center before virtual notches are applied.
constexpr ControlState MINIMUM_NOTCH_DISTANCE = 0.9;

// The plastic shell that limits an analog stick's movement, by its shape.
class StickGate
{
public:
  enum class Shape : u8
  {
    // Found on most Nintendo GC/Wii analog sticks; size is its circumscribed circle's radius.
    Octagon,
    // Possibly found on 3rd-party accessories; size is its radius.
    Round,
    // e.g. keyboard input; size is its half width.
    Square,
  };

  StickGate(Shape shape, ControlState size) : m_shape(shape), m_size(size) {}

  // Angle is in radians and should be non-negative
  ControlState GetRadiusAtAngle(double ang) const;

  // This is provided purely as an optimization for ReshapableInput to produce a minimal amount of
  // calibration points that are saved in our config.
  std::optional<u32> GetIdealCalibrationSampleCount() const;

private:
  Shape m_shape;
  ControlState m_size;
};

// The gates by shape; they add nothing to StickGate, so each is held as the StickGate it is.
class OctagonStickGate final : public StickGate
{
public:
  explicit OctagonStickGate(ControlState radius) : StickGate(Shape::Octagon, radius) {}
};

class RoundStickGate final : public StickGate
{
public:
  explicit RoundStickGate(ControlState radius) : StickGate(Shape::Round, radius) {}
};

class SquareStickGate final : public StickGate
{
public:
  explicit SquareStickGate(ControlState half_width) : StickGate(Shape::Square, half_width) {}
};

// The reshapable group a ReshapableInput is, which its reshaping calls branch on
enum class ReshapeKind : u8
{
  AnalogStick,
  OctagonAnalogStick,
  Force,
  Tilt,
  Cursor,
};

class ReshapableInput : public ControlGroup
{
public:
  // This is the number of samples we generate but any number could be loaded from config.
  static constexpr int CALIBRATION_SAMPLE_COUNT = 32;

  // Contains input radius maximums at evenly-spaced angles.
  using CalibrationData = std::vector<ControlState>;

  ReshapableInput(std::string name, std::string ui_name, GroupType type, ReshapeKind kind);

  using ReshapeData = Common::DVec2;

  // Angle is in radians and should be non-negative
  ControlState GetInputRadiusAtAngle(double angle) const;

  ControlState GetDeadzonePercentage() const;

  // These branch to the group this is.
  ControlState GetVirtualNotchSize() const;
  ControlState GetGateRadiusAtAngle(double angle) const;
  ReshapeData GetReshapableState(bool adjusted) const;
  ControlState GetDefaultInputRadiusAtAngle(double ang) const;

  void SetCalibrationFromGate(const StickGate& gate);

  static ControlState GetCalibrationDataRadiusAtAngle(const CalibrationData& data, double angle);

  CalibrationData GetCalibrationData() const;
  void SetCalibrationData(CalibrationData data);

  ReshapeData GetCenter() const;

  ~ReshapableInput() override;
  ReshapableInput(const ReshapableInput&) = delete;
  ReshapableInput& operator=(const ReshapableInput&) = delete;

protected:
  ReshapeData Reshape(ControlState x, ControlState y, ControlState modifier = 0.0,
                      ControlState clamp = 1.0) const;

  Control* GetModifierInput() const;

  // What a group that has no call of its own does
  ControlState GetVirtualNotchSizeDefault() const { return 0.0; }
  // Normally the same as the gate radius, unless a group does odd things with it (e.g. Tilt)
  ControlState GetDefaultInputRadiusAtAngleDefault(double angle) const
  {
    return GetGateRadiusAtAngle(angle);
  }
  Control* GetModifierInputDefault() const { return nullptr; }

  const ReshapeKind m_reshape_kind;

private:
  // ControlGroup branches to these, private as they are
  friend class ControlGroup;
  void LoadConfig(Common::IniFile::Section*, const char* base_name);
  void SaveConfig(Common::IniFile::Section*, const char* base_name);

  // Calibration and center, read on every poll and set from the configuration: published
  // whole, so a poll never sees half of a change.
  struct Shape
  {
    quiesce_node_t node;  // first: the free callback casts it back
    CalibrationData calibration;
    ReshapeData center;
  };
  static void FreeShape(quiesce_node_t* node);
  // Only inside an InputReadScope.
  const Shape& PeekShape() const;
  // Swaps in edit's result, made from a copy of the current shape.
  template <typename Edit>
  void EditShape(Edit&& edit);

  mutable retro_atomic_ptr_t m_shape{};
  SettingValue<double> m_deadzone_setting;
};

}  // namespace ControllerEmu
