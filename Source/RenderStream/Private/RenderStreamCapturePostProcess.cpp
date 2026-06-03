#pragma once

#include "RenderStreamCapturePostProcess.h"


#include "DisplayClusterConfigurationTypes_Base.h"
#include "FrameStream.h"
#include "IDisplayCluster.h"
#include "RenderStream.h"
#include "RenderStreamProjectionPolicy.h"
#include "Render/Viewport/IDisplayClusterViewportManager.h"
#include "Render/Viewport/IDisplayClusterViewportProxy.h"

#include "Engine/World.h"

class UCameraComponent;
class UWorld;
class FRenderStreamModule;

DEFINE_LOG_CATEGORY(LogRenderStreamPostProcess);

FString FRenderStreamCapturePostProcess::Type = TEXT("renderstream_capture");

FRenderStreamCapturePostProcess::FRenderStreamCapturePostProcess(const FString& PostProcessId, const struct FDisplayClusterConfigurationPostprocess* InConfigurationPostProcess)
    : Id(PostProcessId)
{}

FRenderStreamCapturePostProcess::~FRenderStreamCapturePostProcess() {}

bool FRenderStreamCapturePostProcess::IsConfigurationChanged(const FDisplayClusterConfigurationPostprocess* InConfigurationPostprocess) const
{
    return false;
}

// Once we have access to FDisplayClusterProjectionModule or FDisplayClusterProjectionCameraPolicy
// we can do the work done in FRenderStreamProjectionPolicy HandleStartScene and HandleEndScene here.
bool FRenderStreamCapturePostProcess::HandleStartScene(IDisplayClusterViewportManager* InViewportManager)
{
    if (!IsInCluster()) {
        return false;
    }

    FRenderStreamModule* Module = FRenderStreamModule::Get();
    check(Module);

    Module->LoadSchemas(*GWorld);
    return true;
}

void FRenderStreamCapturePostProcess::HandleEndScene(IDisplayClusterViewportManager* InViewportManager) {}

void FRenderStreamCapturePostProcess::PerformPostProcessViewAfterWarpBlend_RenderThread(FRHICommandListImmediate& RHICmdList, const IDisplayClusterViewportProxy* ViewportProxy) const
{
    static int32 s_CaptureCallCount = 0;
    ++s_CaptureCallCount;
    const int32 CallN = s_CaptureCallCount;

    if (!IsInCluster() || ViewportProxy == nullptr)
    {
        return;
    }

    auto ViewportId = ViewportProxy->GetId();
    FRenderStreamModule* Module = FRenderStreamModule::Get();
    check(Module);

    auto Stream = Module->StreamPool->GetStream(ViewportId);
    // We can't create a stream on the render thread, so our only option is to not do anything if the stream doesn't exist here.
    if (!Stream)
    {
        if (CallN <= 30)
            UE_LOG(LogRenderStreamPostProcess, Warning, TEXT("[RS_TRACE] Capture #%d: viewport '%s' has NO stream in pool — skipping send"), CallN, *ViewportId);
        return;
    }

    if (Stream)
    {
        if (CallN <= 30 || CallN % 120 == 0)
            UE_LOG(LogRenderStreamPostProcess, Warning, TEXT("[RS_TRACE] Capture #%d: viewport='%s' stream='%s' channel='%s' resolution=%dx%d"),
                CallN, *ViewportId, *Stream->Name(), *Stream->Channel(), Stream->Resolution().X, Stream->Resolution().Y);

        auto Size = ViewportProxy->GetRenderSettings_RenderThread().Rect.Size();
        if (Size.GetMin() <= 0)
        {
            auto Resolution = Stream->Resolution();
            UE_LOG(LogRenderStream, Error, TEXT("[RS_TRACE] Capture #%d: Viewport '%s' has ZERO size %dx%d, expected %dx%d"),
                CallN, *ViewportId, Size.X, Size.Y, Resolution.X, Resolution.Y);
            return;
        }

        auto& Info = Module->GetViewportInfo(ViewportId);
        RenderStreamLink::CameraResponseData frameResponse;
        {
            std::lock_guard<std::mutex> guard(Info.m_frameResponsesLock);
            if (Info.m_frameResponsesMap.count(GFrameCounterRenderThread)) // Check current frame data exists
            {
                frameResponse = Info.m_frameResponsesMap[GFrameCounterRenderThread];
                Info.m_frameResponsesMap.erase(GFrameCounterRenderThread);
                if (CallN <= 30)
                    UE_LOG(LogRenderStreamPostProcess, Warning, TEXT("[RS_TRACE] Capture #%d: found frameResponse tTracked=%.4f camPos=(%.2f,%.2f,%.2f)"),
                        CallN, frameResponse.tTracked, frameResponse.camera.x, frameResponse.camera.y, frameResponse.camera.z);
            }
            else
            {
                return; // No frame requested — skip GPU blit, RHI flush, and rs_sendFrame2
            }
        }

        TArray<FRHITexture*> Resources;
        TArray<FIntRect> Rects;
        // NOTE: If you get a black screen on the stream when updating the plugin to a new unreal version try changing the EDisplayClusterViewportResourceType enum.
        EDisplayClusterViewportResourceType resourceType = EDisplayClusterViewportResourceType::InputShaderResource;
        const FString policyType = ViewportProxy->GetProjectionPolicy_RenderThread()->GetType();
        if (policyType != FRenderStreamProjectionPolicy::RenderStreamPolicyType)
        {
            resourceType = EDisplayClusterViewportResourceType::AdditionalTargetableResource;
            if (CallN <= 30)
                UE_LOG(LogRenderStreamPostProcess, Warning, TEXT("[RS_TRACE] Capture #%d: policy type '%s' != renderstream, using AdditionalTargetableResource"),
                    CallN, *policyType);
        }
        ViewportProxy->GetResourcesWithRects_RenderThread(resourceType, Resources, Rects);
        if (Resources.Num() != 1 || Rects.Num() != 1)
        {
            UE_LOG(LogRenderStream, Error, TEXT("[RS_TRACE] Capture #%d: Missing viewport output for '%s' — Resources=%d Rects=%d"),
                CallN, *ViewportId, Resources.Num(), Rects.Num());
            return;
        }

        if (CallN <= 30)
            UE_LOG(LogRenderStreamPostProcess, Warning, TEXT("[RS_TRACE] Capture #%d: sending frame for '%s' resource=%p rect=(%d,%d,%d,%d)"),
                CallN, *ViewportId, Resources[0], Rects[0].Min.X, Rects[0].Min.Y, Rects[0].Max.X, Rects[0].Max.Y);

        Stream->SendFrame_RenderingThread(RHICmdList, frameResponse, Resources[0], Rects[0]);
    }

    // Uncomment this to restore client display
    // InViewportProxy->ResolveResources(RHICmdList, EDisplayClusterViewportResourceType::InputShaderResource, InViewportProxy->GetOutputResourceType());

    // Diagnostic: log render settings and output state for both viewports
    {
        const auto& Settings = ViewportProxy->GetRenderSettings_RenderThread();
        if (CallN <= 30)
            UE_LOG(LogRenderStreamPostProcess, Warning, TEXT("[RS_TRACE] Capture #%d: POST-CAPTURE viewport='%s' rect=(%d,%d,%d,%d)"),
                CallN, *ViewportId, Settings.Rect.Min.X, Settings.Rect.Min.Y, Settings.Rect.Max.X, Settings.Rect.Max.Y);
    }
}

FRenderStreamPostProcessFactory::BasePostProcessPtr FRenderStreamPostProcessFactory::Create(
    const FString& PostProcessId, const struct FDisplayClusterConfigurationPostprocess* InConfigurationPostProcess)
{
    UE_LOG(LogRenderStreamPostProcess, Log, TEXT("Instantiating post process <%s>..."), *InConfigurationPostProcess->Type);

    if (!InConfigurationPostProcess->Type.Compare(FRenderStreamPostProcessFactory::RenderStreamPostProcessType, ESearchCase::IgnoreCase))
    {
        PostProcessPtr Result = MakeShareable(new FRenderStreamCapturePostProcess(PostProcessId, InConfigurationPostProcess));
        return StaticCastSharedPtr<IDisplayClusterPostProcess>(Result);
    }

    return nullptr;
}
