// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3D12/DX12Shader.h"

#include "Common/Assert.h"
#include "Common/StringUtil.h"

#include "VideoBackends/D3D12/Common.h"
#include "VideoBackends/D3D12/DX12Context.h"

namespace DX12
{
DXShader::DXShader(ShaderStage stage, ShaderBinary bytecode)
    : D3DCommon::Shader(stage, bytecode)
{
}

DXShader::~DXShader() = default;

DXShader* DXShader::CreateFromBytecode(ShaderStage stage, ShaderBinary bytecode,
                                                       std::string_view name)
{
  DXShader* shader = new DXShader(stage, bytecode);
  if (stage == ShaderStage::Compute && !shader->CreateComputePipeline(name))
  {
    delete shader;
    return nullptr;
  }

  return shader;
}

DXShader* DXShader::CreateFromSource(ShaderStage stage, ShaderCode* source,
                                                     std::string_view name)
{
  ShaderBinary bytecode;
  if (!CompileShader(g_dx_context->GetFeatureLevel(), stage, source, &bytecode))
    return nullptr;

  return CreateFromBytecode(stage, bytecode, name);
}

DXShader* DXShader::CreateFromBuild(ShaderStage stage, shader_build_fn fn, const void* arg,
                                    std::string_view name)
{
  ShaderBinary bytecode;
  if (!CompileBuild(g_dx_context->GetFeatureLevel(), stage, fn, arg, &bytecode))
    return nullptr;

  return CreateFromBytecode(stage, bytecode, name);
}

D3D12_SHADER_BYTECODE DXShader::GetD3DByteCode() const
{
  return D3D12_SHADER_BYTECODE{m_bytecode.code, m_bytecode.size};
}

bool DXShader::CreateComputePipeline(std::string_view name)
{
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
  desc.pRootSignature = g_dx_context->GetComputeRootSignature();
  desc.CS = GetD3DByteCode();
  desc.NodeMask = 1;

  HRESULT hr = g_dx_context->GetDevice()->CreateComputePipelineState(
      &desc, IID_PPV_ARGS(&m_compute_pipeline));
  ASSERT_MSG(VIDEO, SUCCEEDED(hr), "Creating compute pipeline failed: {}", DX12HRWrap(hr));

  /* only the compute pipeline is named: the name is widened for it alone */
  if (m_compute_pipeline && !name.empty())
    m_compute_pipeline->SetName(UTF8ToWString(name).c_str());

  return SUCCEEDED(hr);
}

}  // namespace DX12
