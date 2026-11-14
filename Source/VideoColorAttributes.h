#pragma once
#include "pch.h"
#include <cmath>

extern "C"
{
#include <libavutil/mastering_display_metadata.h>
}

#pragma region Desktop Family
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_APP) && !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)

// {47537213-8cfb-4722-aa34-fbc9e24d77b8}   MF_MT_CUSTOM_VIDEO_PRIMARIES    {BLOB (MT_CUSTOM_VIDEO_PRIMARIES)}
DEFINE_GUID(MF_MT_CUSTOM_VIDEO_PRIMARIES,
    0x47537213, 0x8cfb, 0x4722, 0xaa, 0x34, 0xfb, 0xc9, 0xe2, 0x4d, 0x77, 0xb8);

typedef struct _MT_CUSTOM_VIDEO_PRIMARIES {
    float fRx;
    float fRy;
    float fGx;
    float fGy;
    float fBx;
    float fBy;
    float fWx;
    float fWy;
} MT_CUSTOM_VIDEO_PRIMARIES;

#endif /* WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP | WINAPI_PARTITION_GAMES) */
#pragma endregion

// Applies the container's colour description to either compressed or decoded video.
class VideoColorAttributes
{
public:
    // Reports source HDR metadata even when the HDR policy suppresses its attributes.
    static bool Apply(const AVCodecParameters* codecPar, bool applyHdrColorInfo,
        winrt::Windows::Media::MediaProperties::MediaPropertySet const& properties)
    {
        using winrt::Windows::Foundation::PropertyValue;

        bool hasHdrVideoPrimaries = false;
        if (codecPar->color_primaries != AVCOL_PRI_UNSPECIFIED)
        {
            MFVideoPrimaries videoPrimaries{ MFVideoPrimaries_Unknown };
            switch (codecPar->color_primaries)
            {
            case AVCOL_PRI_BT709:
                videoPrimaries = MFVideoPrimaries_BT709;
                break;

            case AVCOL_PRI_BT470M:
                videoPrimaries = MFVideoPrimaries_BT470_2_SysM;
                break;

            case AVCOL_PRI_BT470BG:
                videoPrimaries = MFVideoPrimaries_BT470_2_SysBG;
                break;

            case AVCOL_PRI_SMPTE170M:
                videoPrimaries = MFVideoPrimaries_SMPTE170M;
                break;

            case AVCOL_PRI_SMPTE240M:
                videoPrimaries = MFVideoPrimaries_SMPTE240M;
                break;

            case AVCOL_PRI_FILM:
                // FFmpeg uses SMPTE-C / BT.601-NTSC as best approximation.
                videoPrimaries = MFVideoPrimaries_SMPTE_C;
                break;

            case AVCOL_PRI_BT2020:
                videoPrimaries = MFVideoPrimaries_BT2020;
                break;

            case AVCOL_PRI_EBU3213: // EBU Tech. 3213 (PAL/SECAM Variant)
                videoPrimaries = MFVideoPrimaries_EBU3213;
                break;

            case AVCOL_PRI_SMPTE428: // XYZ (DCI-P3 standard in XYZ-Colorspace)
                videoPrimaries = MFVideoPrimaries_XYZ;
                break;

            case AVCOL_PRI_SMPTE431: // DCI-P3 (cinema standard)
            case AVCOL_PRI_SMPTE432: // Display P3 / Apple P3
                videoPrimaries = MFVideoPrimaries_DCI_P3;
                break;

            default:
                break;
            }

            if (videoPrimaries != MFVideoPrimaries_Unknown)
            {
                hasHdrVideoPrimaries = videoPrimaries >= MFVideoPrimaries_BT2020;
                if (!hasHdrVideoPrimaries || applyHdrColorInfo)
                {
                    properties.Insert(MF_MT_VIDEO_PRIMARIES, PropertyValue::CreateUInt32(videoPrimaries));
                }
            }
        }

        bool hasHdrVideoTransferFunction = false;
        if (codecPar->color_trc != AVCOL_TRC_UNSPECIFIED)
        {
            MFVideoTransferFunction videoTransferFunc{ MFVideoTransFunc_Unknown };
            switch (codecPar->color_trc)
            {
            case AVCOL_TRC_LINEAR:
                videoTransferFunc = MFVideoTransFunc_10; // Gamma 1.0 (Linear)
                break;

            case AVCOL_TRC_GAMMA22:
                videoTransferFunc = MFVideoTransFunc_22; // Pure Gamma 2.2
                break;

            case AVCOL_TRC_GAMMA28:
                videoTransferFunc = MFVideoTransFunc_28; // Pure Gamma 2.8
                break;

            case AVCOL_TRC_SMPTE240M:
                videoTransferFunc = MFVideoTransFunc_240M;
                break;

            case AVCOL_TRC_LOG:
                videoTransferFunc = MFVideoTransFunc_Log_100;
                break;

            case AVCOL_TRC_LOG_SQRT:
                videoTransferFunc = MFVideoTransFunc_Log_316;
                break;

            case AVCOL_TRC_BT1361_ECG:
                videoTransferFunc = MFVideoTransFunc_BT1361_ECG;
                break;

            case AVCOL_TRC_SMPTE428:
                videoTransferFunc = MFVideoTransFunc_SMPTE428; // DCI-Cinema transfer function
                break;

                // True ITU-R / SDR Transfer Functions ---
            case AVCOL_TRC_BT709:
            case AVCOL_TRC_SMPTE170M: // BT.601 uses the same transfer characteristics as BT.709
                videoTransferFunc = MFVideoTransFunc_709;
                break;

                // Standard sRGB (Crucial for web, desktop and screen captures) ---
            case AVCOL_TRC_IEC61966_2_1:
                videoTransferFunc = MFVideoTransFunc_sRGB;
                break;

                // --- HDR STANDARDS ---
            case AVCOL_TRC_BT2020_10:
            case AVCOL_TRC_BT2020_12:
                videoTransferFunc = codecPar->color_space == AVCOL_SPC_BT2020_CL || codecPar->color_space == AVCOL_SPC_CHROMA_DERIVED_CL ?
                    MFVideoTransFunc_2020_const :
                    MFVideoTransFunc_2020;
                break;

            case AVCOL_TRC_SMPTEST2084:
                videoTransferFunc = MFVideoTransFunc_2084; // PQ / HDR10 / Dolby Vision
                break;

            case AVCOL_TRC_ARIB_STD_B67:
                videoTransferFunc = MFVideoTransFunc_HLG; // Hybrid Log-Gamma
                break;

            default:
                break;
            }

            if (videoTransferFunc != MFVideoTransFunc_Unknown)
            {
                hasHdrVideoTransferFunction = videoTransferFunc >= MFVideoTransFunc_2020_const;
                if (!hasHdrVideoTransferFunction || applyHdrColorInfo)
                {
                    properties.Insert(MF_MT_TRANSFER_FUNCTION, PropertyValue::CreateUInt32(videoTransferFunc));
                }
            }
        }

        if (codecPar->color_range != AVCOL_RANGE_UNSPECIFIED)
        {
            MFNominalRange nominalRange{ codecPar->color_range == AVCOL_RANGE_JPEG ? MFNominalRange_0_255 : MFNominalRange_16_235 };
            properties.Insert(MF_MT_VIDEO_NOMINAL_RANGE, PropertyValue::CreateUInt32(nominalRange));
        }

        if (codecPar->color_space != AVCOL_SPC_UNSPECIFIED)
        {
            MFVideoTransferMatrix transferMatrix{ MFVideoTransferMatrix_Unknown };
            switch (codecPar->color_space)
            {
            case AVCOL_SPC_BT709:
                transferMatrix = MFVideoTransferMatrix_BT709;
                break;

            case AVCOL_SPC_FCC:
                transferMatrix = MFVideoTransferMatrix_FCC47;
                break;

            case AVCOL_SPC_BT470BG:
            case AVCOL_SPC_SMPTE170M:
                transferMatrix = MFVideoTransferMatrix_BT601;
                break;

            case AVCOL_SPC_SMPTE240M:
                transferMatrix = MFVideoTransferMatrix_SMPTE240M;
                break;

            case AVCOL_SPC_YCGCO:
            case AVCOL_SPC_YCGCO_RE: // H.273 v3 Reversible Matrix rounded even
            case AVCOL_SPC_YCGCO_RO: // H.273 v3 Reversible Matrix rounded odd
                transferMatrix = MFVideoTransferMatrix_YCgCo;
                break;

            case AVCOL_SPC_SMPTE2085:
                transferMatrix = MFVideoTransferMatrix_SMPTE2085;
                break;

            case AVCOL_SPC_BT2020_NCL:
                transferMatrix = codecPar->color_trc == AVCOL_TRC_BT2020_12 ?
                    MFVideoTransferMatrix_BT2020_12 :
                    MFVideoTransferMatrix_BT2020_10;
                break;

            case AVCOL_SPC_BT2020_CL:
            case AVCOL_SPC_CHROMA_DERIVED_CL:
                transferMatrix = MFVideoTransferMatrix_Chroma_const;
                break;

            case AVCOL_SPC_CHROMA_DERIVED_NCL:
                transferMatrix = MFVideoTransferMatrix_Chroma;
                break;

            case AVCOL_SPC_ICTCP:
                transferMatrix = MFVideoTransferMatrix_ICtCp;
                break;

            default:
                break;
            }

            if (transferMatrix != MFVideoTransferMatrix_Unknown)
            {
                properties.Insert(MF_MT_YUV_MATRIX, PropertyValue::CreateUInt32(transferMatrix));
            }
        }

        // av_packet_side_data_get returns the side data entry; the metadata struct is its payload.
        auto contentLightSideData = av_packet_side_data_get(codecPar->coded_side_data, codecPar->nb_coded_side_data, AV_PKT_DATA_CONTENT_LIGHT_LEVEL);
        const AVContentLightMetadata* contentLightMetadata{ contentLightSideData && contentLightSideData->size >= sizeof(AVContentLightMetadata) ? reinterpret_cast<const AVContentLightMetadata*>(contentLightSideData->data) : nullptr };
        if (contentLightMetadata != nullptr && applyHdrColorInfo)
        {
            properties.Insert(MF_MT_MAX_LUMINANCE_LEVEL, PropertyValue::CreateUInt32(contentLightMetadata->MaxCLL));
            properties.Insert(MF_MT_MAX_FRAME_AVERAGE_LUMINANCE_LEVEL, PropertyValue::CreateUInt32(contentLightMetadata->MaxFALL));
        }

        auto masteringDisplaySideData = av_packet_side_data_get(codecPar->coded_side_data, codecPar->nb_coded_side_data, AV_PKT_DATA_MASTERING_DISPLAY_METADATA);
        const AVMasteringDisplayMetadata* masteringDisplayMetadata{ masteringDisplaySideData && masteringDisplaySideData->size >= sizeof(AVMasteringDisplayMetadata) ? reinterpret_cast<const AVMasteringDisplayMetadata*>(masteringDisplaySideData->data) : nullptr };
        if (masteringDisplayMetadata != nullptr && applyHdrColorInfo)
        {
            if (masteringDisplayMetadata->has_luminance)
            {
                constexpr UINT32 MASTERING_DISP_LUMINANCE_SCALE{ 10000 };
                properties.Insert(MF_MT_MIN_MASTERING_LUMINANCE, PropertyValue::CreateUInt32(static_cast<UINT32>(std::lround(MASTERING_DISP_LUMINANCE_SCALE * av_q2d(masteringDisplayMetadata->min_luminance)))));
                properties.Insert(MF_MT_MAX_MASTERING_LUMINANCE, PropertyValue::CreateUInt32(static_cast<UINT32>(std::lround(av_q2d(masteringDisplayMetadata->max_luminance)))));
            }

            if (masteringDisplayMetadata->has_primaries)
            {
                MT_CUSTOM_VIDEO_PRIMARIES customVideoPrimaries
                {
                    static_cast<float>(av_q2d(masteringDisplayMetadata->display_primaries[0][0])),
                    static_cast<float>(av_q2d(masteringDisplayMetadata->display_primaries[0][1])),
                    static_cast<float>(av_q2d(masteringDisplayMetadata->display_primaries[1][0])),
                    static_cast<float>(av_q2d(masteringDisplayMetadata->display_primaries[1][1])),
                    static_cast<float>(av_q2d(masteringDisplayMetadata->display_primaries[2][0])),
                    static_cast<float>(av_q2d(masteringDisplayMetadata->display_primaries[2][1])),
                    static_cast<float>(av_q2d(masteringDisplayMetadata->white_point[0])),
                    static_cast<float>(av_q2d(masteringDisplayMetadata->white_point[1]))
                };
                auto data = winrt::array_view<UINT8>(reinterpret_cast<UINT8*>(&customVideoPrimaries), sizeof(customVideoPrimaries));
                properties.Insert(MF_MT_CUSTOM_VIDEO_PRIMARIES, PropertyValue::CreateUInt8Array(data));
            }
        }

        return (masteringDisplayMetadata != nullptr && (masteringDisplayMetadata->has_primaries || masteringDisplayMetadata->has_luminance)) || contentLightMetadata != nullptr || hasHdrVideoPrimaries || hasHdrVideoTransferFunction;
    }
};
