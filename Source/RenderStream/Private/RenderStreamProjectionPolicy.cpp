#include "RenderStreamProjectionPolicy.h"

#include "Misc/DisplayClusterHelpers.h"

#include "IDisplayCluster.h"
#include "Render/Viewport/IDisplayClusterViewport.h"
#include "Render/Viewport/IDisplayClusterViewportProxy.h"
#include "Config/IDisplayClusterConfigManager.h"
#include "DisplayClusterConfigurationTypes.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "CineCameraComponent.h"

#include "Math/UnitConversion.h"
#include "RenderStream.h"
#include "Kismet/GameplayStatics.h"

#include "RenderStreamSettings.h"
#include "FrameStream.h"

#include "RenderStreamChannelDefinition.h"
#include "RenderStreamProjectionPolicy.h"

#include "Engine/LocalPlayer.h"
#include "GameFramework/PlayerController.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY(LogRenderStreamPolicy);

FString FRenderStreamProjectionPolicy::RenderStreamPolicyType = TEXT("renderstream");

FRenderStreamProjectionPolicy::FRenderStreamProjectionPolicy(const FString& _ProjectionPolicyId, const struct FDisplayClusterConfigurationProjection* InConfigurationProjectionPolicy)
    : ProjectionPolicyId(_ProjectionPolicyId)
    , Parameters(InConfigurationProjectionPolicy->Parameters)
    , NCP(0)
    , FCP(0)
{
}

FRenderStreamProjectionPolicy::~FRenderStreamProjectionPolicy() {}

bool FRenderStreamProjectionPolicy::HandleStartScene(class IDisplayClusterViewport* Viewport)
{
    if (GIsEditor)
        return false;

    const FString ViewportId = Viewport->GetId();
    FRenderStreamModule* Module = FRenderStreamModule::Get();
    check(Module);
    if (!Module->StreamPool)
    {
        UE_LOG(LogRenderStream, Log, TEXT("Abort start scene handler, stream pool not initialized."));
        return false;
    }

    auto Stream = Module->StreamPool->GetStream(ViewportId);
    if (Stream)
    {
        // Reconfigure stream when scene changes
        Module->ConfigureStream(Stream);
    }

    return true;
}

void FRenderStreamProjectionPolicy::HandleEndScene(class IDisplayClusterViewport* Viewport)
{
    FRenderStreamModule* Module = FRenderStreamModule::Get();
    check(Module);

    auto& Info = Module->GetViewportInfo(Viewport->GetId());
    const int32 numLP = GWorld ? GWorld->GetGameInstance()->GetNumLocalPlayers() : -1;
    UE_LOG(LogRenderStreamPolicy, Warning, TEXT("[RS_TRACE] HandleEndScene: viewport='%s' Camera=%s PlayerId=%d numLocalPlayers=%d"),
        *Viewport->GetId(),
        Info.Camera.IsValid() ? *Info.Camera->GetName() : TEXT("INVALID"),
        Info.PlayerId,
        numLP);

    // ── AUDIT: enumerate surviving UObjects once (first viewport only) ──
    {
        static bool s_auditDone = false;
        if (!s_auditDone)
        {
            s_auditDone = true;

            int32 countViewport = 0, countPC = 0, countCam = 0, countLP = 0;
            for (TObjectIterator<UDisplayClusterConfigurationViewport> It; It; ++It)
            {
                ++countViewport;
                UE_LOG(LogRenderStreamPolicy, Warning, TEXT("[AUDIT] LIVE DisplayClusterViewport: %s Outer=%s Flags=0x%x IsPendingKill=%d"),
                    *It->GetName(), It->GetOuter() ? *It->GetOuter()->GetName() : TEXT("null"),
                    static_cast<uint32>(It->GetFlags()), It->IsPendingKillOrUnreachable());
            }
            for (TObjectIterator<APlayerController> It; It; ++It)
            {
                ++countPC;
                UE_LOG(LogRenderStreamPolicy, Warning, TEXT("[AUDIT] LIVE PlayerController: %s Outer=%s Flags=0x%x IsPendingKill=%d"),
                    *It->GetName(), It->GetOuter() ? *It->GetOuter()->GetName() : TEXT("null"),
                    static_cast<uint32>(It->GetFlags()), It->IsPendingKillOrUnreachable());
            }
            for (TObjectIterator<ACameraActor> It; It; ++It)
            {
                ++countCam;
                UE_LOG(LogRenderStreamPolicy, Warning, TEXT("[AUDIT] LIVE CameraActor: %s Outer=%s Flags=0x%x IsPendingKill=%d"),
                    *It->GetName(), It->GetOuter() ? *It->GetOuter()->GetName() : TEXT("null"),
                    static_cast<uint32>(It->GetFlags()), It->IsPendingKillOrUnreachable());
            }
            for (TObjectIterator<ULocalPlayer> It; It; ++It)
            {
                ++countLP;
                UE_LOG(LogRenderStreamPolicy, Warning, TEXT("[AUDIT] LIVE LocalPlayer: %s Outer=%s"),
                    *It->GetName(), It->GetOuter() ? *It->GetOuter()->GetName() : TEXT("null"));
            }
            UE_LOG(LogRenderStreamPolicy, Warning, TEXT("[AUDIT] SUMMARY: Viewports=%d PlayerControllers=%d CameraActors=%d LocalPlayers=%d"),
                countViewport, countPC, countCam, countLP);
        }
    }

    Info.Camera = nullptr;
}

bool FRenderStreamProjectionPolicy::CalculateView(class IDisplayClusterViewport* InViewport, const uint32 InContextNum, FVector& InOutViewLocation, FRotator& InOutViewRotation, const FVector& ViewOffset, const float WorldToMeters, const float InNCP, const float InFCP)
{
    check(IsInGameThread());
    
    FRenderStreamModule* Module = FRenderStreamModule::Get();
    check(Module);

    auto& Info = Module->GetViewportInfo(InViewport->GetId());
    UCameraComponent* AssignedCamera = Info.Camera.IsValid() ? Info.Camera->GetCameraComponent() : nullptr;

    if (!AssignedCamera)
    {
        static int32 s_NoCamCount = 0;
        if (++s_NoCamCount <= 5)
            UE_LOG(LogRenderStreamPolicy, Warning, TEXT("[RS_TRACE] CalculateView: NO camera for viewport '%s', using origin"), *InViewport->GetId());
    }

    InOutViewLocation = (AssignedCamera ? AssignedCamera->GetComponentLocation() : FVector::ZeroVector);
    InOutViewRotation = (AssignedCamera ? AssignedCamera->GetComponentRotation() : FRotator::ZeroRotator);

    // Store culling data
    NCP = InNCP;
    FCP = InFCP;

    return true;
}

bool FRenderStreamProjectionPolicy::GetProjectionMatrix(class IDisplayClusterViewport* InViewport, const uint32 InContextNum, FMatrix& OutPrjMatrix)
{
    check(IsInGameThread());

    FRenderStreamModule* Module = FRenderStreamModule::Get();
    check(Module);

    auto const& ViewportId = InViewport->GetId();
    auto& Info = Module->GetViewportInfo(ViewportId);
    UCameraComponent* AssignedCamera = Info.Camera.IsValid() ? Info.Camera->GetCameraComponent() : nullptr;

    if (!AssignedCamera)
    {
        static int32 s_NoCamCount = 0;
        if (++s_NoCamCount <= 5)
            UE_LOG(LogRenderStreamPolicy, Error, TEXT("[RS_TRACE] GetProjectionMatrix: NO camera assigned to viewport '%s' — returning false"), *ViewportId);
        return false;
    }

    FMatrix PrjMatrix;
    if (AssignedCamera->ProjectionMode == ECameraProjectionMode::Orthographic)
    {
        const float OrthoWidth = 0.5f * AssignedCamera->OrthoWidth;
        const float OrthoHeight = OrthoWidth / AssignedCamera->AspectRatio;
        const float ZScale = 1.f / (AssignedCamera->OrthoFarClipPlane - AssignedCamera->OrthoNearClipPlane);
        const float ZOffset = -AssignedCamera->OrthoNearClipPlane;
        PrjMatrix = FReversedZOrthoMatrix(OrthoWidth, OrthoHeight, ZScale, ZOffset);
    }
    else
    {
        const float FieldOfViewH = FMath::DegreesToRadians(AssignedCamera->FieldOfView);
        const float FieldOfViewV = 2 * FMath::Atan(FMath::Tan((FieldOfViewH / 2.0f)) * (1 / AssignedCamera->AspectRatio));

        const float l = -FMath::Tan(0.5f * FieldOfViewH);
        const float r = FMath::Tan(0.5f * FieldOfViewH);
        const float t = FMath::Tan(0.5f * FieldOfViewV);
        const float b = -FMath::Tan(0.5f * FieldOfViewV);

        InViewport->CalculateProjectionMatrix(InContextNum, NCP * l, NCP * r, NCP * t, NCP * b, NCP, FCP, false);
        PrjMatrix = InViewport->GetContexts()[InContextNum].ProjectionMatrix;
    }

    // Center shift
    FVector centerShift = { 0.f, 0.f, 0.f };
    {
        std::lock_guard<std::mutex> guard(Info.m_frameResponsesLock);
        uint64 frameCounter = GFrameCounter;
        if (Info.m_frameResponsesMap.count(frameCounter)) // Check current frame data exists
        {
            // first frame can have no frame response.
            const RenderStreamLink::CameraResponseData& thisFrameResponse = Info.m_frameResponsesMap[frameCounter];
            centerShift = { thisFrameResponse.camera.cx, thisFrameResponse.camera.cy, 0.f };
        }
    }

    auto Stream = Module->StreamPool->GetStream(ViewportId);
    // Clipping
    FTransform clippingTransform;
    RenderStreamLink::ProjectionClipping Clipping = { 0.f, 1.f, 0.f, 1.f };  // Default clipping in case no streams
    if (Stream)
        Clipping = Stream->Clipping();
    FVector clippingScale = { 1.f / (Clipping.right - Clipping.left), -1.f / (Clipping.top - Clipping.bottom), 1.f };
    FVector clippingOffset = (FVector(1.f - (Clipping.right + Clipping.left), -1.f + (Clipping.top + Clipping.bottom), 0.f) + centerShift) * clippingScale;
    clippingTransform.SetTranslationAndScale3D(clippingOffset, clippingScale);
    FMatrix clippingMatrix = clippingTransform.ToMatrixWithScale();

    OutPrjMatrix = PrjMatrix * clippingMatrix;

    {
        static uint32 s_SuccessCount = 0;
        if (++s_SuccessCount <= 5 || s_SuccessCount % 120 == 0)
            UE_LOG(LogRenderStreamPolicy, Warning, TEXT("[RS_TRACE] GetProjectionMatrix: SUCCESS viewport='%s' cam='%s' pos=(%.1f,%.1f,%.1f)"),
                *ViewportId, *AssignedCamera->GetOwner()->GetName(),
                AssignedCamera->GetComponentLocation().X, AssignedCamera->GetComponentLocation().Y, AssignedCamera->GetComponentLocation().Z);
    }

    return true;
}

//////////////////////////////////////////////////////////////////////////////////////////////
// FRenderStreamProjectionPolicyFactory
//////////////////////////////////////////////////////////////////////////////////////////////
FRenderStreamProjectionPolicyFactory::FRenderStreamProjectionPolicyFactory()
{
}

FRenderStreamProjectionPolicyFactory::~FRenderStreamProjectionPolicyFactory()
{
}

FRenderStreamProjectionPolicyFactory::BasePolicyPtr FRenderStreamProjectionPolicyFactory::Create(const FString& ProjectionPolicyId, const struct FDisplayClusterConfigurationProjection* InConfigurationProjectionPolicy)
{
    UE_LOG(LogRenderStreamPolicy, Log, TEXT("Instantiating projection policy <%s>..."), *InConfigurationProjectionPolicy->Type);

    if (!InConfigurationProjectionPolicy->Type.Compare(FRenderStreamProjectionPolicy::RenderStreamPolicyType, ESearchCase::IgnoreCase))
    {
        PolicyPtr Result = MakeShareable(new FRenderStreamProjectionPolicy(ProjectionPolicyId, InConfigurationProjectionPolicy));
        return StaticCastSharedPtr<IDisplayClusterProjectionPolicy>(Result);
    }

    return nullptr;
}

