/*
 * The part of NVIDIA's NGX ABI that SteamARM's nvngx.dll answers: values and
 * layouts from the public DLSS SDK headers (github.com/NVIDIA/DLSS,
 * include/nvsdk_ngx_defs.h, nvsdk_ngx_params.h, nvsdk_ngx_defs_vk.h), written
 * out here so that the SDK headers themselves are not needed to build it.
 * Only what the DLL reads or writes; the names keep NVIDIA's spelling.
 */
#ifndef SA_NGX_ABI_H
#define SA_NGX_ABI_H

#include <stdint.h>

/* NVSDK_NGX_Result */
#define NGX_OK                            0x1u
#define NGX_FAIL                          0xBAD00000u
#define NGX_FAIL_FeatureNotSupported      (NGX_FAIL | 1)
#define NGX_FAIL_PlatformError            (NGX_FAIL | 2)
#define NGX_FAIL_FeatureNotFound          (NGX_FAIL | 4)
#define NGX_FAIL_InvalidParameter         (NGX_FAIL | 5)
#define NGX_FAIL_NotInitialized           (NGX_FAIL | 7)
#define NGX_FAIL_MissingInput             (NGX_FAIL | 10)
#define NGX_FAIL_UnableToInitializeFeature (NGX_FAIL | 11)
#define NGX_FAIL_UnsupportedParameter     (NGX_FAIL | 16)
#define NGX_FAIL_NotImplemented           (NGX_FAIL | 18)

/* NVSDK_NGX_Feature */
#define NGX_Feature_SuperSampling     1
#define NGX_Feature_FrameGeneration   11
#define NGX_Feature_RayReconstruction 13

/* NVSDK_NGX_PerfQuality_Value */
#define NGX_PerfQuality_MaxPerf          0
#define NGX_PerfQuality_Balanced         1
#define NGX_PerfQuality_MaxQuality       2
#define NGX_PerfQuality_UltraPerformance 3
#define NGX_PerfQuality_UltraQuality     4
#define NGX_PerfQuality_DLAA             5

/* NVSDK_NGX_DLSS_Feature_Flags */
#define NGX_DLSS_IsHDR          (1 << 0)
#define NGX_DLSS_MVLowRes       (1 << 1)
#define NGX_DLSS_MVJittered     (1 << 2)
#define NGX_DLSS_DepthInverted  (1 << 3)
#define NGX_DLSS_DoSharpening   (1 << 5)
#define NGX_DLSS_AutoExposure   (1 << 6)
#define NGX_DLSS_AlphaUpscaling (1 << 7)

/* NVSDK_NGX_Feature_Support_Result */
#define NGX_FeatureSupportResult_Supported          0
#define NGX_FeatureSupportResult_AdapterUnsupported 4

/* NVSDK_NGX_Version: NVSDK_NGX_VERSION_API_MACRO */
#define NGX_VERSION_API 0x15

/* NVSDK_NGX_FeatureRequirement */
struct ngx_feature_requirement {
    int FeatureSupported;
    unsigned MinHWArchitecture;
    char MinOSVersion[255];
};

/* NVSDK_NGX_FeatureDiscoveryInfo: only the fields read here. */
struct ngx_feature_discovery_info {
    int SDKVersion;
    int FeatureID;
    /* NVSDK_NGX_Application_Identifier, path, feature info follow */
};

/* NVSDK_NGX_Application_Identifier (NVSDK_NGX_UpdateFeature) */
struct ngx_application_identifier {
    int IdentifierType;   /* 0 application id, 1 project id */
    union {
        struct { const char *ProjectId; int EngineType; const char *EngineVersion; } ProjectDesc;
        unsigned long long ApplicationId;
    } v;
};

/* NVSDK_NGX_Resource_VK (nvsdk_ngx_defs_vk.h) */
#define NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW 0
#define NGX_RESOURCE_VK_TYPE_VK_BUFFER    1
struct ngx_resource_vk {
    union {
        struct {
            uint64_t ImageView;          /* VkImageView */
            uint64_t Image;              /* VkImage */
            uint32_t aspectMask, baseMipLevel, levelCount, baseArrayLayer, layerCount;   /* VkImageSubresourceRange */
            uint32_t Format;             /* VkFormat */
            unsigned Width, Height;
        } ImageViewInfo;
        struct {
            uint64_t Buffer;             /* VkBuffer */
            unsigned SizeInBytes;
        } BufferInfo;
    } Resource;
    int Type;
    unsigned char ReadWrite;             /* bool */
};

/* Parameter keys (nvsdk_ngx_defs.h). The "#\xNN" keys are the enumerated
 * form older games use (NVSDK_NGX_EParameter_*). */
#define K_SuperSampling_Available          "SuperSampling.Available"
#define K_SuperSampling_NeedsUpdatedDriver "SuperSampling.NeedsUpdatedDriver"
#define K_SuperSampling_MinDriverMajor     "SuperSampling.MinDriverVersionMajor"
#define K_SuperSampling_MinDriverMinor     "SuperSampling.MinDriverVersionMinor"
#define K_SuperSampling_FeatureInitResult  "SuperSampling.FeatureInitResult"
#define K_SuperSampling_ScaleFactor        "SuperSampling.ScaleFactor"
#define K_SSD_Available                    "SuperSamplingDenoising.Available"
#define K_SSD_NeedsUpdatedDriver           "SuperSamplingDenoising.NeedsUpdatedDriver"
#define K_SSD_FeatureInitResult            "SuperSamplingDenoising.FeatureInitResult"
#define K_FG_Available                     "FrameGeneration.Available"
#define K_FG_NeedsUpdatedDriver            "FrameGeneration.NeedsUpdatedDriver"
#define K_FG_FeatureInitResult             "FrameGeneration.FeatureInitResult"
#define K_FI_Available                     "FrameInterpolation.Available"
#define K_FI_NeedsUpdatedDriver            "FrameInterpolation.NeedsUpdatedDriver"
#define K_FI_FeatureInitResult             "FrameInterpolation.FeatureInitResult"
#define K_OptLevel                         "Snippet.OptLevel"
#define K_IsDevSnippetBranch               "Snippet.IsDevBranch"
#define K_OptimalSettingsCallback          "DLSSOptimalSettingsCallback"
#define K_GetStatsCallback                 "DLSSGetStatsCallback"
#define K_Width                            "Width"
#define K_Height                           "Height"
#define K_OutWidth                         "OutWidth"
#define K_OutHeight                        "OutHeight"
#define K_Scale                            "Scale"
#define K_Sharpness                        "Sharpness"
#define K_PerfQualityValue                 "PerfQualityValue"
#define K_RTXValue                         "RTXValue"
#define K_DLSSMode                         "DLSSMode"
#define K_SizeInBytes                      "SizeInBytes"
#define K_FreeMemOnReleaseFeature          "FreeMemOnReleaseFeature"
#define K_CreationNodeMask                 "CreationNodeMask"
#define K_VisibilityNodeMask               "VisibilityNodeMask"
#define K_Color                            "Color"
#define K_Output                           "Output"
#define K_Depth                            "Depth"
#define K_MotionVectors                    "MotionVectors"
#define K_ExposureTexture                  "ExposureTexture"
#define K_TransparencyMask                 "TransparencyMask"
#define K_BiasCurrentColorMask             "DLSS.Input.Bias.Current.Color.Mask"
#define K_Reset                            "Reset"
#define K_Jitter_X                         "Jitter.Offset.X"
#define K_Jitter_Y                         "Jitter.Offset.Y"
#define K_MV_Scale_X                       "MV.Scale.X"
#define K_MV_Scale_Y                       "MV.Scale.Y"
#define K_MV_Offset_X                      "MV.Offset.X"
#define K_MV_Offset_Y                      "MV.Offset.Y"
#define K_Pre_Exposure                     "DLSS.Pre.Exposure"
#define K_Exposure_Scale                   "DLSS.Exposure.Scale"
#define K_Create_Flags                     "DLSS.Feature.Create.Flags"
#define K_Enable_Output_Subrects           "DLSS.Enable.Output.Subrects"
#define K_Render_Subrect_W                 "DLSS.Render.Subrect.Dimensions.Width"
#define K_Render_Subrect_H                 "DLSS.Render.Subrect.Dimensions.Height"
#define K_Color_Subrect_X                  "DLSS.Input.Color.Subrect.Base.X"
#define K_Color_Subrect_Y                  "DLSS.Input.Color.Subrect.Base.Y"
#define K_Depth_Subrect_X                  "DLSS.Input.Depth.Subrect.Base.X"
#define K_Depth_Subrect_Y                  "DLSS.Input.Depth.Subrect.Base.Y"
#define K_MV_Subrect_X                     "DLSS.Input.MV.Subrect.Base.X"
#define K_MV_Subrect_Y                     "DLSS.Input.MV.Subrect.Base.Y"
#define K_Bias_Subrect_X                   "DLSS.Input.Bias.Current.Color.Subrect.Base.X"
#define K_Bias_Subrect_Y                   "DLSS.Input.Bias.Current.Color.Subrect.Base.Y"
#define K_Output_Subrect_X                 "DLSS.Output.Subrect.Base.X"
#define K_Output_Subrect_Y                 "DLSS.Output.Subrect.Base.Y"
#define K_Dyn_Max_W                        "DLSS.Get.Dynamic.Max.Render.Width"
#define K_Dyn_Max_H                        "DLSS.Get.Dynamic.Max.Render.Height"
#define K_Dyn_Min_W                        "DLSS.Get.Dynamic.Min.Render.Width"
#define K_Dyn_Min_H                        "DLSS.Get.Dynamic.Min.Render.Height"
#define K_Preset_DLAA                      "DLSS.Hint.Render.Preset.DLAA"
#define K_Preset_Quality                   "DLSS.Hint.Render.Preset.Quality"
#define K_Preset_Balanced                  "DLSS.Hint.Render.Preset.Balanced"
#define K_Preset_Performance               "DLSS.Hint.Render.Preset.Performance"
#define K_Preset_UltraPerformance          "DLSS.Hint.Render.Preset.UltraPerformance"
#define K_Preset_UltraQuality              "DLSS.Hint.Render.Preset.UltraQuality"
/* NVSDK_NGX_EParameter_* */
#define E_SuperSampling_Available          "#\x01"
#define E_Width                            "#\x10"
#define E_Height                           "#\x11"
#define E_OutWidth                         "#\x12"
#define E_OutHeight                        "#\x13"
#define E_Sharpness                        "#\x14"
#define E_Scale                            "#\x0f"
#define E_SizeInBytes                      "#\x30"
#define E_MV_Scale_X                       "#\x2c"
#define E_MV_Scale_Y                       "#\x2d"
#define E_MV_Offset_X                      "#\x38"
#define E_MV_Offset_Y                      "#\x39"
#define E_OptimalSettingsCallback          "#\x3e"
#define E_PerfQualityValue                 "#\x3f"
#define E_DLSSMode                         "#\x41"
#define E_OptLevel                         "#\x44"
#define E_IsDevSnippetBranch               "#\x45"
#define E_Color                            "#\x1e"
#define E_Output                           "#\x22"
#define E_Reset                            "#\x25"
#define E_MotionVectors                    "#\x27"
#define E_Depth                            "#\x3d"

/* NVSDK_NGX_DLSS_Hint_Render_Preset (DLSS 4: J..M are the transformer
 * presets; the hints are read for the log only, MetalFX has no presets). */
#define NGX_Preset_Default 0
#define NGX_Preset_J      10
#define NGX_Preset_K      11
#define NGX_Preset_L      12
#define NGX_Preset_M      13

#endif
