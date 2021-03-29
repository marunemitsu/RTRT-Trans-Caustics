// Copyright Epic Games, Inc. All Rights Reserved.

#include "RendererPrivate.h"
#include "GlobalShader.h"
#include "DeferredShadingRenderer.h"
#include "SceneTextureParameters.h"

#if RHI_RAYTRACING

#include "ClearQuad.h"
#include "SceneRendering.h"
#include "SceneRenderTargets.h"
#include "RHIResources.h"
#include "SystemTextures.h"
#include "ScreenSpaceDenoise.h"
#include "PostProcess/PostProcessing.h"
#include "PostProcess/SceneFilterRendering.h"
#include "PipelineStateCache.h"
#include "RayTracing/RaytracingOptions.h"
#include "Raytracing/RaytracingLighting.h"


static TAutoConsoleVariable<int32> CVarRayTracingTranslucency(
	TEXT("r.RayTracing.Translucency"),
	-1,
	TEXT("-1: Value driven by postprocess volume (default) \n")
	TEXT(" 0: ray tracing translucency off (use raster) \n")
	TEXT(" 1: ray tracing translucency enabled"),
	ECVF_RenderThreadSafe);

static float GRayTracingTranslucencyMaxRoughness = -1;
static FAutoConsoleVariableRef CVarRayTracingTranslucencyMaxRoughness(
	TEXT("r.RayTracing.Translucency.MaxRoughness"),
	GRayTracingTranslucencyMaxRoughness,
	TEXT("Sets the maximum roughness until which ray tracing reflections will be visible (default = -1 (max roughness driven by postprocessing volume))")
);

static int32 GRayTracingTranslucencyMaxRefractionRays = -1;
static FAutoConsoleVariableRef CVarRayTracingTranslucencyMaxRefractionRays(
	TEXT("r.RayTracing.Translucency.MaxRefractionRays"),
	GRayTracingTranslucencyMaxRefractionRays,
	TEXT("Sets the maximum number of refraction rays for ray traced translucency (default = -1 (max bounces driven by postprocessing volume)"));

static int32 GRayTracingTranslucencyEmissiveAndIndirectLighting = 1;
static FAutoConsoleVariableRef CVarRayTracingTranslucencyEmissiveAndIndirectLighting(
	TEXT("r.RayTracing.Translucency.EmissiveAndIndirectLighting"),
	GRayTracingTranslucencyEmissiveAndIndirectLighting,
	TEXT("Enables ray tracing translucency emissive and indirect lighting (default = 1)")
);

static int32 GRayTracingTranslucencyDirectLighting = 1;
static FAutoConsoleVariableRef CVarRayTracingTranslucencyDirectLighting(
	TEXT("r.RayTracing.Translucency.DirectLighting"),
	GRayTracingTranslucencyDirectLighting,
	TEXT("Enables ray tracing translucency direct lighting (default = 1)")
);

static int32 GRayTracingTranslucencyShadows = -1;
static FAutoConsoleVariableRef CVarRayTracingTranslucencyShadows(
	TEXT("r.RayTracing.Translucency.Shadows"),
	GRayTracingTranslucencyShadows,
	TEXT("Enables shadows in ray tracing translucency)")
	TEXT(" -1: Shadows driven by postprocessing volume (default)")
	TEXT(" 0: Shadows disabled ")
	TEXT(" 1: Hard shadows")
	TEXT(" 2: Soft area shadows")
);

static float GRayTracingTranslucencyMinRayDistance = -1;
static FAutoConsoleVariableRef CVarRayTracingTranslucencyMinRayDistance(
	TEXT("r.RayTracing.Translucency.MinRayDistance"),
	GRayTracingTranslucencyMinRayDistance,
	TEXT("Sets the minimum ray distance for ray traced translucency rays. Actual translucency ray length is computed as Lerp(MaxRayDistance, MinRayDistance, Roughness), i.e. translucency rays become shorter when traced from rougher surfaces. (default = -1 (infinite rays))")
);

static float GRayTracingTranslucencyMaxRayDistance = -1;
static FAutoConsoleVariableRef CVarRayTracingTranslucencyMaxRayDistance(
	TEXT("r.RayTracing.Translucency.MaxRayDistance"),
	GRayTracingTranslucencyMaxRayDistance,
	TEXT("Sets the maximum ray distance for ray traced translucency rays. When ray shortening is used, skybox will not be sampled in RT translucency pass and will be composited later, together with local reflection captures. Negative values turn off this optimization. (default = -1 (infinite rays))")
);

static int32 GRayTracingTranslucencySamplesPerPixel = 1;
static FAutoConsoleVariableRef CVarRayTracingTranslucencySamplesPerPixel(
	TEXT("r.RayTracing.Translucency.SamplesPerPixel"),
	GRayTracingTranslucencySamplesPerPixel,
	TEXT("Sets the samples-per-pixel for Translucency (default = 1)"));

static int32 GRayTracingTranslucencyHeightFog = 1;
static FAutoConsoleVariableRef CVarRayTracingTranslucencyHeightFog(
	TEXT("r.RayTracing.Translucency.HeightFog"),
	GRayTracingTranslucencyHeightFog,
	TEXT("Enables height fog in ray traced Translucency (default = 1)"));

static int32 GRayTracingTranslucencyRefraction = -1;
static FAutoConsoleVariableRef CVarRayTracingTranslucencyRefraction(
	TEXT("r.RayTracing.Translucency.Refraction"),
	GRayTracingTranslucencyRefraction,
	TEXT("Enables refraction in ray traced Translucency (default = 1)"));

static float GRayTracingTranslucencyPrimaryRayBias = 1e-5;
static FAutoConsoleVariableRef CVarRayTracingTranslucencyPrimaryRayBias(
	TEXT("r.RayTracing.Translucency.PrimaryRayBias"),
	GRayTracingTranslucencyPrimaryRayBias,
	TEXT("Sets the bias to be subtracted from the primary ray TMax in ray traced Translucency. Larger bias reduces the chance of opaque objects being intersected in ray traversal, saving performance, but at the risk of skipping some thin translucent objects in proximity of opaque objects. (recommended range: 0.00001 - 0.1) (default = 0.00001)"));


DECLARE_GPU_STAT_NAMED(RayTracingTranslucency, TEXT("Ray Tracing Translucency"));

#if RHI_RAYTRACING

FRayTracingPrimaryRaysOptions GetRayTracingTranslucencyOptions()
{
	FRayTracingPrimaryRaysOptions Options;

	Options.IsEnabled = CVarRayTracingTranslucency.GetValueOnRenderThread();
	Options.SamplerPerPixel = GRayTracingTranslucencySamplesPerPixel;
	Options.ApplyHeightFog = GRayTracingTranslucencyHeightFog;
	Options.PrimaryRayBias = GRayTracingTranslucencyPrimaryRayBias;
	Options.MaxRoughness = GRayTracingTranslucencyMaxRoughness;
	Options.MaxRefractionRays = GRayTracingTranslucencyMaxRefractionRays;
	Options.EnableEmmissiveAndIndirectLighting = GRayTracingTranslucencyEmissiveAndIndirectLighting;
	Options.EnableDirectLighting = GRayTracingTranslucencyDirectLighting;
	Options.EnableShadows = GRayTracingTranslucencyShadows;
	Options.MinRayDistance = GRayTracingTranslucencyMinRayDistance;
	Options.MaxRayDistance = GRayTracingTranslucencyMaxRayDistance;
	Options.EnableRefraction = GRayTracingTranslucencyRefraction;

	return Options;
}

class FCompositeTranslucencyPS : public FGlobalShader {

	DECLARE_GLOBAL_SHADER(FCompositeTranslucencyPS)
	SHADER_USE_PARAMETER_STRUCT(FCompositeTranslucencyPS, FGlobalShader)

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, TranslucencyTexture)
		//SHADER_PARAMETER_RDG_TEXTURE(Texture2D, BaseTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CausticsTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, TranslucencyTextureSampler)
		//SHADER_PARAMETER_SAMPLER(SamplerState, BaseTextureSampler)
		SHADER_PARAMETER_SAMPLER(SamplerState, CausticsTextureSampler)

		RENDER_TARGET_BINDING_SLOTS()
		END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FCompositeTranslucencyPS, "/Engine/Private/RayTracing/CompositeTranslucencyPS.usf", "CompositeTranslucencyPS", SF_Pixel)

bool ShouldRenderRayTracingTranslucency(const FViewInfo& View)
{
	bool bViewWithRaytracingTranslucency = View.FinalPostProcessSettings.TranslucencyType == ETranslucencyType::RayTracing;

	const int32 GRayTracingTranslucency = CVarRayTracingTranslucency.GetValueOnRenderThread();
	const bool bTranslucencyCvarEnabled = GRayTracingTranslucency < 0 ? bViewWithRaytracingTranslucency : (GRayTracingTranslucency != 0);
	const int32 ForceAllRayTracingEffects = GetForceRayTracingEffectsCVarValue();
	const bool bRayTracingTranslucencyEnabled = (ForceAllRayTracingEffects > 0 || (bTranslucencyCvarEnabled && ForceAllRayTracingEffects < 0));

	return IsRayTracingEnabled() && bRayTracingTranslucencyEnabled;
}
#endif // RHI_RAYTRACING

void AddCompositeTexturePass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	//FScreenPassTexture BaseInput,
	FScreenPassTexture TranslucencyInput,
	FScreenPassTexture AdditiveInput,
	FScreenPassRenderTarget Output)
{
	const FScreenPassTextureViewport InputViewport(TranslucencyInput);
	const FScreenPassTextureViewport OutputViewport(Output);

	TShaderMapRef<FCompositeTranslucencyPS> PixelShader(View.ShaderMap);

	FCompositeTranslucencyPS::FParameters* Parameters = GraphBuilder.AllocParameters<FCompositeTranslucencyPS::FParameters>();

	Parameters->CausticsTexture = AdditiveInput.Texture;
	Parameters->CausticsTextureSampler = TStaticSamplerState<>::GetRHI();
	//Parameters->BaseTexture = BaseInput.Texture;
	//Parameters->BaseTextureSampler = TStaticSamplerState<>::GetRHI();
	Parameters->TranslucencyTexture = TranslucencyInput.Texture;
	Parameters->TranslucencyTextureSampler = TStaticSamplerState<>::GetRHI();
	Parameters->RenderTargets[0] = Output.GetRenderTargetBinding();

	AddDrawScreenPass(GraphBuilder, RDG_EVENT_NAME("CompositeTranslucent"), View, OutputViewport, InputViewport, PixelShader, Parameters);
}

void FDeferredShadingSceneRenderer::RenderRayTracingTranslucency(FRHICommandListImmediate& RHICmdList)
{
	if (!ShouldRenderTranslucency(ETranslucencyPass::TPT_StandardTranslucency)
		&& !ShouldRenderTranslucency(ETranslucencyPass::TPT_TranslucencyAfterDOF)
		&& !ShouldRenderTranslucency(ETranslucencyPass::TPT_TranslucencyAfterDOFModulate)
		&& !ShouldRenderTranslucency(ETranslucencyPass::TPT_AllTranslucency)
		)
	{
		return; // Early exit if nothing needs to be done.
	}

	FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);

	FRDGBuilder GraphBuilder(RHICmdList);

	FRDGTextureRef SceneColorTexture = GraphBuilder.RegisterExternalTexture(SceneContext.GetSceneColor(), TEXT("SceneColor"));

	FSceneTextureParameters SceneTextures;
	SetupSceneTextureParameters(GraphBuilder, &SceneTextures);

	{
		RDG_EVENT_SCOPE(GraphBuilder, "RayTracingTranslucency");
		RDG_GPU_STAT_SCOPE(GraphBuilder, RayTracingTranslucency)

			for (int32 ViewIndex = 0, Num = Views.Num(); ViewIndex < Num; ViewIndex++)
			{
				FViewInfo& View = Views[ViewIndex];

				const FScreenPassRenderTarget Output(SceneColorTexture, View.ViewRect, ERenderTargetLoadAction::ELoad);

				//#dxr_todo: UE-72581 do not use reflections denoiser structs but separated ones
				IScreenSpaceDenoiser::FReflectionsRayTracingConfig RayTracingConfig;
				IScreenSpaceDenoiser::FReflectionsInputs DenoiserInputs;
				IScreenSpaceDenoiser::FReflectionsInputs CausticsInputs;
				FRDGTextureRef PrimaryRayColorTexture = nullptr;

				float ResolutionFraction = 1.0f;
				int32 TranslucencySPP = GRayTracingTranslucencySamplesPerPixel > 1 ? GRayTracingTranslucencySamplesPerPixel : View.FinalPostProcessSettings.RayTracingTranslucencySamplesPerPixel;

				RayTracingConfig.RayCountPerPixel = TranslucencySPP;
				RayTracingConfig.ResolutionFraction = ResolutionFraction;

				RenderRayTracingPrimaryRaysView(
					GraphBuilder,
					View, &DenoiserInputs.Color, &DenoiserInputs.RayHitDistance, &DenoiserInputs.RayImaginaryDepth,
					&CausticsInputs.Color,
					TranslucencySPP, GRayTracingTranslucencyHeightFog, ResolutionFraction,
					ERayTracingPrimaryRaysFlag::AllowSkipSkySample | ERayTracingPrimaryRaysFlag::UseGBufferForMaxDistance);

				const IScreenSpaceDenoiser* DefaultDenoiser = IScreenSpaceDenoiser::GetDefaultDenoiser();
				const IScreenSpaceDenoiser* DenoiserToUse = DefaultDenoiser;

				ResolveSceneColor(RHICmdList);

				RenderRayTracingCaustics(
					GraphBuilder,
					View,
					&CausticsInputs.Color, &CausticsInputs.RayHitDistance, &CausticsInputs.RayImaginaryDepth,
					TranslucencySPP, GRayTracingTranslucencyHeightFog, ResolutionFraction
				);


				RDG_EVENT_SCOPE(GraphBuilder, "%s%s(Transluency) %dx%d",
					DenoiserToUse != DefaultDenoiser ? TEXT("ThirdParty ") : TEXT(""),
					DenoiserToUse->GetDebugName(),
					View.ViewRect.Width(), View.ViewRect.Height());

				/*IScreenSpaceDenoiser::FReflectionsOutputs TranslucentDenoiserOutputs = DenoiserToUse->DenoiseReflections(
					GraphBuilder,
					View,
					&View.PrevViewInfo,
					SceneTextures,
					DenoiserInputs,
					RayTracingConfig);*/

				IScreenSpaceDenoiser::FReflectionsOutputs CausticsDenoiserOutputs = DenoiserToUse->DenoiseReflections(
					GraphBuilder,
					View,
					&View.PrevViewInfo,
					SceneTextures,
					CausticsInputs,
					RayTracingConfig);

				const FScreenPassTexture AdditiveColor(CausticsDenoiserOutputs.Color, View.ViewRect);
				const FScreenPassTexture Transparency(DenoiserInputs.Color, View.ViewRect);
				//const FScreenPassTexture BaseColor(PrimaryRayColorTexture, View.ViewRect);

				AddCompositeTexturePass(GraphBuilder, View, Transparency, AdditiveColor, Output);
				//AddDrawTexturePass(GraphBuilder, View, AdditiveColor, Output);
			}
	}

	GraphBuilder.Execute();

	ResolveSceneColor(RHICmdList);
}

#endif // RHI_RAYTRACING
