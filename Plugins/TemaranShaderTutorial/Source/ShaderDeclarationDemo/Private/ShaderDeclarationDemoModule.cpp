// Copyright 2016-2020 Cadic AB. All Rights Reserved.
// @Author	Fredrik Lindh [Temaran] (temaran@gmail.com) {https://github.com/Temaran}
///////////////////////////////////////////////////////////////////////////////////////

#include "ShaderDeclarationDemoModule.h"

#include "ComputeShaderExample.h"
#include "PixelShaderExample.h"
#include "GPUMeshShader.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "RHI.h"
#include "GlobalShader.h"
#include "RHICommandList.h"
#include "RenderGraphBuilder.h"
#include "RenderTargetPool.h"
#include "Runtime/Core/Public/Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "GPUMeshComponent.h"
#include "MLSMPMManager.h"
IMPLEMENT_MODULE(FShaderDeclarationDemoModule, ShaderDeclarationDemo)

// Declare some GPU stats so we can track them later
DECLARE_GPU_STAT_NAMED(ShaderPlugin_Render, TEXT("ShaderPlugin: Root Render"));
DECLARE_GPU_STAT_NAMED(ShaderPlugin_Compute, TEXT("ShaderPlugin: Render Compute Shader"));
DECLARE_GPU_STAT_NAMED(ShaderPlugin_Pixel, TEXT("ShaderPlugin: Render Pixel Shader"));

void FShaderDeclarationDemoModule::StartupModule()
{
	OnPostResolvedSceneColorHandle.Reset();
	bCachedParametersValid = false;

	// Maps virtual shader source directory to the plugin's actual shaders directory.
	FString PluginShaderDir = FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("TemaranShaderTutorial"))->GetBaseDir(), TEXT("Shaders"));
	AddShaderSourceDirectoryMapping(TEXT("/TutorialShaders"), PluginShaderDir);
}

void FShaderDeclarationDemoModule::ShutdownModule()
{
	EndRendering();
}

void FShaderDeclarationDemoModule::BeginRendering()
{
	if (OnPostResolvedSceneColorHandle.IsValid())
	{
		return;
	}

	bCachedParametersValid = false;

	const FName RendererModuleName("Renderer");
	IRendererModule* RendererModule = FModuleManager::GetModulePtr<IRendererModule>(RendererModuleName);
	if (RendererModule)
	{
		OnPostResolvedSceneColorHandle = RendererModule->GetResolvedSceneColorCallbacks().AddRaw(this, &FShaderDeclarationDemoModule::PostResolveSceneColor_RenderThread);
	}
}

void FShaderDeclarationDemoModule::EndRendering()
{
	if (!OnPostResolvedSceneColorHandle.IsValid())
	{
		return;
	}

	const FName RendererModuleName("Renderer");
	IRendererModule* RendererModule = FModuleManager::GetModulePtr<IRendererModule>(RendererModuleName);
	if (RendererModule)
	{
		RendererModule->GetResolvedSceneColorCallbacks().Remove(OnPostResolvedSceneColorHandle);
	}

	OnPostResolvedSceneColorHandle.Reset();
}

void FShaderDeclarationDemoModule::UpdateParameters(FShaderUsageExampleParameters& DrawParameters)
{
	RenderEveryFrameLock.Lock();
	CachedShaderUsageExampleParameters = DrawParameters;
	bCachedParametersValid = true;
	RenderEveryFrameLock.Unlock();
}

void FShaderDeclarationDemoModule::UpdateGPUMeshParameters(FGPUMeshParameters& GPUMeshParameters)
{
	// if (GPUMeshParameters.OutputVertexPositionImage->bCanCreateUAV == false)
	// {
	// 	GPUMeshParameters.OutputVertexPositionImage->bCanCreateUAV = true;
	// }
	RenderEveryFrameLock.Lock();
	if (GPUMeshParameters.OutputVertexPositionImage!= nullptr)
	{
		CachedGPUMeshParameters.OutputVertexPositionImage = GPUMeshParameters.OutputVertexPositionImage;
	}
	CachedGPUMeshParameters.BrushList = GPUMeshParameters.BrushList;
	CachedGPUMeshParameters.SDFTexture = GPUMeshParameters.SDFTexture;
	bCachedParametersValid = true;
	RenderEveryFrameLock.Unlock();
}


void FShaderDeclarationDemoModule::UpdateGPUMesh(FTexture3DRHIRef SDFTexture, const FGPUMeshControlParams& ControlParams,
                                                 FGPUMeshVertexBuffers* VertexBuffers)
{
	UpdateMeshRequests=
		[=]()
		{
			FRHICommandListImmediate& RHICmdList = GRHICommandList.GetImmediateCommandList();
	
			FGPUMeshShader::UpdateMeshBySDFGPU(
				RHICmdList,
				SDFTexture,
				VertexBuffers,
				ControlParams
			);
		}
		;
}

FGPUMeshParameters FShaderDeclarationDemoModule::GetGPUMeshParameters()
{
	return CachedGPUMeshParameters;
}

void FShaderDeclarationDemoModule::RequestTickMPMFluid()
{
	ShouldTickMPMFluid = true;
}

void FShaderDeclarationDemoModule::PostResolveSceneColor_RenderThread(FRHICommandListImmediate& RHICmdList, class FSceneRenderTargets& SceneContext)
{
	if (!bCachedParametersValid)
	{
		return;
	}

	// Depending on your data, you might not have to lock here, just added this code to show how you can do it if you have to.
	RenderEveryFrameLock.Lock();
	FShaderUsageExampleParameters Copy = CachedShaderUsageExampleParameters;
	FGPUMeshParameters MeshParamterCopy = CachedGPUMeshParameters;
	RenderEveryFrameLock.Unlock();

	Draw_RenderThread(Copy, MeshParamterCopy);
}

void FShaderDeclarationDemoModule::Draw_RenderThread(
	const FShaderUsageExampleParameters& DrawParameters,
	const FGPUMeshParameters& GPUMeshDrawParameters
)
{
	check(IsInRenderingThread());

	if (!DrawParameters.RenderTarget)
	{
		return;
	}

	FRHICommandListImmediate& RHICmdList = GRHICommandList.GetImmediateCommandList();

	QUICK_SCOPE_CYCLE_COUNTER(STAT_ShaderPlugin_Render); // Used to gather CPU profiling data for the UE4 session frontend
	SCOPED_DRAW_EVENT(RHICmdList, ShaderPlugin_Render); // Used to profile GPU activity and add metadata to be consumed by for example RenderDoc

	if (!ComputeShaderOutput.IsValid())
	{
		FPooledRenderTargetDesc ComputeShaderOutputDesc(FPooledRenderTargetDesc::Create2DDesc(DrawParameters.GetRenderTargetSize(), PF_R32_UINT, FClearValueBinding::None, TexCreate_None, TexCreate_ShaderResource | TexCreate_UAV, false));
		ComputeShaderOutputDesc.DebugName = TEXT("ShaderPlugin_ComputeShaderOutput");
		GRenderTargetPool.FindFreeElement(RHICmdList, ComputeShaderOutputDesc, ComputeShaderOutput, TEXT("ShaderPlugin_ComputeShaderOutput"));
	}
	if (!SDFRenderTarget.IsValid())
	{
		FPooledRenderTargetDesc ComputeShaderOutputDesc(FPooledRenderTargetDesc::Create2DDesc(DrawParameters.GetRenderTargetSize(), PF_R32_FLOAT, FClearValueBinding::None, TexCreate_None, TexCreate_ShaderResource | TexCreate_UAV, false));
		ComputeShaderOutputDesc.DebugName = TEXT("ShaderPlugin_SDFImageInput");
		GRenderTargetPool.FindFreeElement(RHICmdList, ComputeShaderOutputDesc, SDFRenderTarget, TEXT("ShaderPlugin_SDFImageInput"));
	}
	if (!GPUMeshShaderOutput.IsValid())
	{
		FPooledRenderTargetDesc ComputeShaderOutputDesc(FPooledRenderTargetDesc::Create2DDesc(DrawParameters.GetRenderTargetSize(), PF_FloatRGBA, FClearValueBinding::None, TexCreate_None, TexCreate_ShaderResource | TexCreate_UAV, false));
		ComputeShaderOutputDesc.DebugName = TEXT("ShaderPlugin_GPUMeshOutput");
		GRenderTargetPool.FindFreeElement(RHICmdList, ComputeShaderOutputDesc, GPUMeshShaderOutput, TEXT("ShaderPlugin_GPUMeshOutput"));
	}

	FComputeShaderExample::RunComputeShader_RenderThread(RHICmdList, DrawParameters, ComputeShaderOutput->GetRenderTargetItem().UAV);
	FPixelShaderExample::DrawToRenderTarget_RenderThread(RHICmdList, DrawParameters, ComputeShaderOutput->GetRenderTargetItem().TargetableTexture);
	FGPUMeshShader::RunComputeShader_RenderThread(RHICmdList, GPUMeshDrawParameters, SDFRenderTarget->GetRenderTargetItem().UAV, GPUMeshShaderOutput->GetRenderTargetItem().UAV);
	if (ShouldTickMPMFluid)
	{
		FMLSMPMManager::Update_RenderThread(RHICmdList);
		ShouldTickMPMFluid = false;
	}
	
	UpdateMeshRequests();
}
