#include "RendererPrivate.h"
#include "GlobalShader.h"
#include "DeferredShadingRenderer.h"
#include "SceneTextureParameters.h"

#if RHI_RAYTRACING

#include "ClearQuad.h"
#include "SceneRendering.h"
#include "SceneRenderTargets.h"
#include "RHIResources.h"
#include "PostProcess/PostProcessing.h"
#include "RayTracing/RaytracingOptions.h"
#include "Raytracing/RaytracingLighting.h"

DECLARE_GPU_STAT(RayTracingCaustics);

class FRayTracingCausticsRGS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRayTracingCausticsRGS)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FRayTracingCausticsRGS, FGlobalShader)

		class FDenoiserOutput : SHADER_PERMUTATION_BOOL("DIM_DENOISER_OUTPUT");
	class FEnableTwoSidedGeometryForShadowDim : SHADER_PERMUTATION_BOOL("ENABLE_TWO_SIDED_GEOMETRY");
	using FPermutationDomain = TShaderPermutationDomain<FDenoiserOutput, FEnableTwoSidedGeometryForShadowDim>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(int32, SamplesPerPixel)
		SHADER_PARAMETER(int32, MaxRefractionRays)
		SHADER_PARAMETER(int32, HeightFog)
		SHADER_PARAMETER(int32, ShouldDoDirectLighting)
		SHADER_PARAMETER(int32, ReflectedShadowsType)
		SHADER_PARAMETER(int32, ShouldDoEmissiveAndIndirectLighting)
		SHADER_PARAMETER(int32, UpscaleFactor)
		SHADER_PARAMETER(int32, ShouldUsePreExposure)
		SHADER_PARAMETER(uint32, PrimaryRayFlags)
		SHADER_PARAMETER(float, TransmissionMinRayDistance)
		SHADER_PARAMETER(float, TransmissionMaxRayDistance)
		SHADER_PARAMETER(float, TransmissionMaxRoughness)
		SHADER_PARAMETER(int32, TransmissionRefraction)
		SHADER_PARAMETER(float, MaxNormalBias)

		SHADER_PARAMETER_SRV(RaytracingAccelerationStructure, TLAS)
		SHADER_PARAMETER_SRV(StructuredBuffer<FRTLightingData>, LightDataBuffer)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSProfilesTexture)

		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		SHADER_PARAMETER_STRUCT_REF(FRaytracingLightDataPacked, LightDataPacked)
		SHADER_PARAMETER_STRUCT_REF(FReflectionUniformParameters, ReflectionStruct)
		SHADER_PARAMETER_STRUCT_REF(FFogUniformParameters, FogUniformParameters)

		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureSamplerParameters, SceneTextureSamplers)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, ColorOutput)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RayHitDistanceOutput)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RayImaginaryDepthOutput)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}
};

IMPLEMENT_GLOBAL_SHADER(FRayTracingCausticsRGS, "/Engine/Private/RayTracing/RayTracingCaustics.usf", "RayTracingCausticsRGS", SF_RayGen);

void FDeferredShadingSceneRenderer::PrepareRayTracingCaustics(const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders)
{
	// Declare all RayGen shaders that require material closest hit shaders to be bound
	FRayTracingCausticsRGS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FRayTracingCausticsRGS::FEnableTwoSidedGeometryForShadowDim>(EnableRayTracingShadowTwoSidedGeometry());
	auto RayGenShader = View.ShaderMap->GetShader<FRayTracingCausticsRGS>(PermutationVector);
	OutRayGenShaders.Add(RayGenShader.GetRayTracingShader());
}


void FDeferredShadingSceneRenderer::RenderRayTracingCaustics(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	FRDGTextureRef* InOutColorTexture,
	FRDGTextureRef* InOutRayHitDistanceTexture,
	FRDGTextureRef* InOutRayImaginaryDepthTexture,
	int32 SamplePerPixel,
	int32 HeightFog,
	float ResolutionFraction
)
{

	FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(GraphBuilder.RHICmdList);

	FSceneTextureParameters SceneTextures;
	SetupSceneTextureParameters(GraphBuilder, &SceneTextures);
	FSceneTextureSamplerParameters SceneTextureSamplers;
	SetupSceneTextureSamplers(&SceneTextureSamplers);
	int32 UpscaleFactor = int32(1.0f / ResolutionFraction);
	ensure(ResolutionFraction == 1.0 / UpscaleFactor);
	ensureMsgf(FComputeShaderUtils::kGolden2DGroupSize % UpscaleFactor == 0, TEXT("Transmission ray tracing will have uv misalignement."));
	FIntPoint RayTracingResolution = FIntPoint::DivideAndRoundUp(View.ViewRect.Size(), UpscaleFactor);

	FPooledRenderTargetDesc Desc = SceneContext.GetSceneColor()->GetDesc();
	Desc.Format = PF_FloatRGBA;
	Desc.Flags &= ~(TexCreate_FastVRAM | TexCreate_Transient);
	Desc.Extent /= UpscaleFactor;
	Desc.TargetableFlags |= TexCreate_UAV;
	Desc.Format = PF_R16F;

	if (*InOutRayHitDistanceTexture == nullptr)
	{
		*InOutRayHitDistanceTexture = GraphBuilder.CreateTexture(Desc, TEXT("RayTracingCausticsHitDistance"));
	}
	if (*InOutRayImaginaryDepthTexture == nullptr)
	{
		*InOutRayImaginaryDepthTexture = GraphBuilder.CreateTexture(Desc, TEXT("RayTracingCausticsImaginaryDepth"));
	}

	FRayTracingCausticsRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FRayTracingCausticsRGS::FParameters>();

	FRayTracingPrimaryRaysOptions TranslucencyOptions = GetRayTracingTranslucencyOptions();
	PassParameters->SamplesPerPixel = SamplePerPixel;
	PassParameters->MaxRefractionRays = TranslucencyOptions.MaxRefractionRays > -1 ? TranslucencyOptions.MaxRefractionRays : View.FinalPostProcessSettings.RayTracingTranslucencyRefractionRays;
	PassParameters->HeightFog = HeightFog;
	PassParameters->ShouldDoDirectLighting = TranslucencyOptions.EnableDirectLighting;
	PassParameters->ReflectedShadowsType = TranslucencyOptions.EnableShadows > -1 ? TranslucencyOptions.EnableShadows : (int32)View.FinalPostProcessSettings.RayTracingTranslucencyShadows;
	PassParameters->ShouldDoEmissiveAndIndirectLighting = TranslucencyOptions.EnableEmmissiveAndIndirectLighting;
	PassParameters->UpscaleFactor = UpscaleFactor;
	PassParameters->TransmissionMinRayDistance = FMath::Min(TranslucencyOptions.MinRayDistance, TranslucencyOptions.MaxRayDistance);
	PassParameters->TransmissionMaxRayDistance = TranslucencyOptions.MaxRayDistance;
	PassParameters->TransmissionMaxRoughness = FMath::Clamp(TranslucencyOptions.MaxRoughness >= 0 ? TranslucencyOptions.MaxRoughness : View.FinalPostProcessSettings.RayTracingTranslucencyMaxRoughness, 0.01f, 1.0f);
	PassParameters->TransmissionRefraction = TranslucencyOptions.EnableRefraction >= 0 ? TranslucencyOptions.EnableRefraction : View.FinalPostProcessSettings.RayTracingTranslucencyRefraction;
	PassParameters->MaxNormalBias = GetRaytracingMaxNormalBias();
	PassParameters->ShouldUsePreExposure = View.Family->EngineShowFlags.Tonemapper;
	PassParameters->TLAS = View.RayTracingScene.RayTracingSceneRHI->GetShaderResourceView();
	PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;

	PassParameters->LightDataPacked = View.RayTracingLightingDataUniformBuffer;
	PassParameters->LightDataBuffer = View.RayTracingLightingDataSRV;

	PassParameters->SceneTextures = SceneTextures;
	PassParameters->SceneTextureSamplers = SceneTextureSamplers;

	FRDGTextureRef SceneColorTexture = GraphBuilder.RegisterExternalTexture(SceneContext.GetSceneColor(), TEXT("SceneColor"));
	PassParameters->SceneColorTexture = SceneColorTexture;

	PassParameters->ReflectionStruct = CreateReflectionUniformBuffer(View, EUniformBufferUsage::UniformBuffer_SingleFrame);
	PassParameters->FogUniformParameters = CreateFogUniformBuffer(View, EUniformBufferUsage::UniformBuffer_SingleFrame);

	PassParameters->ColorOutput = GraphBuilder.CreateUAV(*InOutColorTexture);
	PassParameters->RayHitDistanceOutput = GraphBuilder.CreateUAV(*InOutRayHitDistanceTexture);
	PassParameters->RayImaginaryDepthOutput = GraphBuilder.CreateUAV(*InOutRayImaginaryDepthTexture);

	// TODO: should be converted to RDG
	TRefCountPtr<IPooledRenderTarget> SubsurfaceProfileRT((IPooledRenderTarget*)GetSubsufaceProfileTexture_RT(GraphBuilder.RHICmdList));
	if (!SubsurfaceProfileRT)
	{
		SubsurfaceProfileRT = GSystemTextures.BlackDummy;
	}
	PassParameters->SSProfilesTexture = GraphBuilder.RegisterExternalTexture(SubsurfaceProfileRT);

	FRayTracingCausticsRGS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FRayTracingCausticsRGS::FEnableTwoSidedGeometryForShadowDim>(EnableRayTracingShadowTwoSidedGeometry());
	auto RayGenShader = View.ShaderMap->GetShader<FRayTracingCausticsRGS>(PermutationVector);

	ClearUnusedGraphResources(RayGenShader, PassParameters);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("RayTracingCaustics %dx%d", RayTracingResolution.X, RayTracingResolution.Y),
		PassParameters,
		ERDGPassFlags::Compute,
		[PassParameters, this, &View, RayGenShader, RayTracingResolution](FRHICommandList& RHICmdList)
		{
			SCOPED_GPU_STAT(RHICmdList, RayTracingCaustics);
			FRayTracingPipelineState* Pipeline = View.RayTracingMaterialPipeline;

			FRayTracingShaderBindingsWriter GlobalResources;
			SetShaderParameters(GlobalResources, RayGenShader, *PassParameters);

			FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;
			RHICmdList.RayTraceDispatch(Pipeline, RayGenShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, RayTracingResolution.X, RayTracingResolution.Y);
		});
}

#endif
