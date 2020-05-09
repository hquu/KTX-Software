/* -*- tab-width: 4; -*- */
/* vi: set sw=2 ts=4 expandtab: */

/*
 * ©2019 Khronos Group, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @internal
 * @file basis_transcode.cpp
 * @~English
 *
 * @brief Functions for transcoding Basis Universal ETC1S and UASTC textures.
 *
 * Two worlds collide here too. More uglyness!
 *
 * @author Mark Callow, www.edgewise-consulting.com
 */

#include <inttypes.h>
#include <stdio.h>
#include <KHR/khr_df.h>

#include "dfdutils/dfd.h"
#include "ktx.h"
#include "ktxint.h"
#include "texture2.h"
#include "vkformat_enum.h"
#include "vk_format.h"
#include "basis_sgd.h"
#include "basisu_transcoder_config.h"
#include "basisu_image_transcoders.h"
#include "basisu/basisu_comp.h"
#include "basisu/transcoder/basisu_file_headers.h"
#include "basisu/transcoder/basisu_transcoder.h"
#include "basisu/transcoder/basisu_transcoder_internal.h"

#undef DECLARE_PRIVATE
#undef DECLARE_PROTECTED
#define DECLARE_PRIVATE(n,t2) ktxTexture2_private& n = *(t2->_private)
#define DECLARE_PROTECTED(n,t2) ktxTexture_protected& n = *(t2->_protected)

using namespace basisu;
using namespace basist;

// block size calculations
static inline uint32_t getBlockWidth(uint32_t w, uint32_t bw)
{
    return (w + (bw - 1)) / bw;
}

static inline uint32_t getBlockHeight(uint32_t h, uint32_t bh)
{
    return (h + (bh - 1)) / bh;
}

inline bool isPow2(uint32_t x) { return x && ((x & (x - 1U)) == 0U); }

inline bool isPow2(uint64_t x) { return x && ((x & (x - 1U)) == 0U); }

KTX_error_code
ktxTexture2_transcodeEtc1s(ktxTexture2* This,
                           bool hasAlpha,
                           ktxTexture2* prototype,
                           ktx_transcode_fmt_e outputFormat,
                           ktx_transcode_flags transcodeFlags);
KTX_error_code
ktxTexture2_transcodeUastc(ktxTexture2* This,
                           bool hasAlpha,
                           ktxTexture2* prototype,
                           ktx_transcode_fmt_e outputFormat,
                           ktx_transcode_flags transcodeFlags);

/**
 * @memberof ktxTexture2
 * @ingroup reader
 * @~English
 * @brief Transcode a KTX2 texture with Basis Universal (ETC1S) or UASTC images.
 *
 * If the texture contains Basis Universal image, Inflates them from
 * supercompression back to ETC1S then transcodes them to the specified
 * block-compressed format. If the texture contains UASTC images, inflates
 * them, if they have been supercompressed with zstd, then transcodes
 * then to the specified format, The transcoded images replace the original
 * images and the texture's fields including the DFD are modified to reflect
 * the new format.
 *
 * These types of textures must be transcoded to a desired target
 * block-compressed format before they can be uploaded to a GPU via a
 * graphics API.
 *
 * The following block compressed transcode targets are available: @c KTX_TTF_ETC1_RGB,
 * @c KTX_TTF_ETC2_RGBA, @c KTX_TTF_BC1_RGB, @c KTX_TTF_BC3_RGBA,
 * @c KTX_TTF_BC4_R, @c KTX_TTF_BC5_RG, @c KTX_TTF_BC7_RGBA,
 * @c @c KTX_TTF_PVRTC1_4_RGB, @c KTX_TTF_PVRTC1_4_RGBA,
 * @c KTX_TTF_PVRTC2_4_RGB, @c KTX_TTF_PVRTC2_4_RGBA, @c KTX_TTF_ASTC_4x4_RGBA,
 * @c KTX_TTF_ETC2_EAC_R11, @c KTX_TTF_ETC2_EAC_RG11, @c KTX_TTF_ETC and
 * @c KTX_TTF_BC1_OR_3.
 *
 * @c KTX_TTF_ETC automatically selects between @c KTX_TTF_ETC1_RGB and
 * @c KTX_TTF_ETC2_RGBA according to whether an alpha channel is available. @c KTX_TTF_BC1_OR_3
 * does likewise between @c KTX_TTF_BC1_RGB and @c KTX_TTF_BC3_RGBA. Note that if
 * @c KTX_TTF_PVRTC1_4_RGBA or @c KTX_TTF_PVRTC2_4_RGBA is specified and there is no alpha
 * channel @c KTX_TTF_PVRTC1_4_RGB or @c KTX_TTF_PVRTC2_4_RGB respectively will be selected.
 *
 * Transcoding to ATC & FXT1 formats is not supported by libktx as there
 * are no equivalent Vulkan formats.
 *
 * The following uncompressed transcode targets are also available: @c KTX_TTF_RGBA32,
 * @c KTX_TTF_RGB565, KTX_TTF_BGR565 and KTX_TTF_RGBA4444.
 *
 * The following @p transcodeFlags are available.
 *
 * @sa ktxtexture2_CompressBasis().
 *
 * @param[in]   This         pointer to the ktxTexture2 object of interest.
 * @param[in]   outputFormat a value from the ktx_texture_transcode_fmt_e enum
 *                                             specifying the target format.
 * @param[in]   transcodeFlags  bitfield of flags modifying the transcode
 *                                                operation. @sa ktx_texture_decode_flags_e.
 *
 * @return      KTX_SUCCESS on success, other KTX_* enum values on error.
 *
 * @exception KTX_FILE_DATA_ERROR
 *                              Supercompression global data is corrupted.
 * @exception KTX_INVALID_OPERATION
 *                              The texture is not supercompressed.
 * @exception KTX_INVALID_OPERATION
 *                              Supercompression global data is missing, i.e.,
 *                              the texture object is invalid.
 * @exception KTX_INVALID_OPERATION
 *                              Image data is missing, i.e., the texture object
 *                              is invalid.
 * @exception KTX_INVALID_OPERATION
 *                              @p outputFormat is PVRTC1 but the texture does
 *                              does not have power-of-two dimensions.
 * @exception KTX_INVALID_VALUE @p outputFormat is invalid.
 * @exception KTX_TRANSCODE_FAILED
 *                              Something went wrong during transcoding. The
 *                              texture object will be corrupted.
 * @exception KTX_UNSUPPORTED_FEATURE
 *                              KTX_TF_PVRTC_DECODE_TO_NEXT_POW2 was requested
 *                              or the specified transcode target has not been
 *                              included in the library being used.
 * @exception KTX_OUT_OF_MEMORY Not enough memory to carry out transcoding.
 */
 KTX_error_code
 ktxTexture2_TranscodeBasis(ktxTexture2* This,
                            ktx_transcode_fmt_e outputFormat,
                            ktx_transcode_flags transcodeFlags)
{
    uint32_t* BDB = This->pDfd + 1;
    khr_df_model_e colorModel = (khr_df_model_e)KHR_DFDVAL(BDB, MODEL);
    if (colorModel != KHR_DF_MODEL_UASTC
         && This->supercompressionScheme != KTX_SS_BASIS_UNIVERSAL)
    return KTX_INVALID_OPERATION; // Not in a transcodable format.

    DECLARE_PRIVATE(priv, This);
    if (This->supercompressionScheme == KTX_SS_BASIS_UNIVERSAL) {
        if (!priv._supercompressionGlobalData || priv._sgdByteLength == 0)
            return KTX_INVALID_OPERATION;
    }

    if (transcodeFlags & KTX_TF_PVRTC_DECODE_TO_NEXT_POW2) {
         debug_printf("ktxTexture_TranscodeBasis: KTX_TF_PVRTC_DECODE_TO_NEXT_POW2 currently unsupported\n");
         return KTX_UNSUPPORTED_FEATURE;
    }

    if (outputFormat == KTX_TTF_PVRTC1_4_RGB
        || outputFormat == KTX_TTF_PVRTC1_4_RGBA) {
         if ((!isPow2(This->baseWidth)) || (!isPow2(This->baseHeight))) {
             debug_printf("ktxTexture_TranscodeBasis: PVRTC1 only supports power of 2 dimensions\n");
             return KTX_INVALID_OPERATION;
        }
    }

    const bool srgb = (KHR_DFDVAL(BDB, TRANSFER) == KHR_DF_TRANSFER_SRGB);
    bool hasAlpha = false;
    if (This->supercompressionScheme == KTX_SS_BASIS_UNIVERSAL) {
        uint32_t numComponents = ktxTexture2_GetNumComponents(This);
        if (numComponents == 2 || numComponents == 4)
            hasAlpha = true;
    } else {
        assert(colorModel == KHR_DF_MODEL_UASTC);
        if (KHR_DFDSVAL(BDB, 0, CHANNELID) == KHR_DF_CHANNEL_UASTC_ALPHAPRESENT)
            hasAlpha = true;
    }

    VkFormat vkFormat;

    // Do some format mapping.
    switch (outputFormat) {
      case KTX_TTF_BC1_OR_3:
        outputFormat = hasAlpha ? KTX_TTF_BC3_RGBA : KTX_TTF_BC1_RGB;
        break;
      case KTX_TTF_ETC:
        outputFormat = hasAlpha ? KTX_TTF_ETC2_RGBA : KTX_TTF_ETC1_RGB;
        break;
      case KTX_TTF_PVRTC1_4_RGBA:
        // This transcoder does not write opaque alpha blocks.
        outputFormat = hasAlpha ? KTX_TTF_PVRTC1_4_RGBA : KTX_TTF_PVRTC1_4_RGB;
        break;
      case KTX_TTF_PVRTC2_4_RGBA:
        // This transcoder does not write opaque alpha blocks.
        outputFormat = hasAlpha ? KTX_TTF_PVRTC2_4_RGBA : KTX_TTF_PVRTC2_4_RGB;
        break;
      default:
        /*NOP*/;
    }

    switch (outputFormat) {
      case KTX_TTF_ETC1_RGB:
        vkFormat = srgb ? VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK
                        : VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
        break;
      case KTX_TTF_ETC2_RGBA:
        vkFormat = srgb ? VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK
                        : VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
        break;
      case KTX_TTF_ETC2_EAC_R11:
        vkFormat = VK_FORMAT_EAC_R11_UNORM_BLOCK;
        break;
      case KTX_TTF_ETC2_EAC_RG11:
        vkFormat = VK_FORMAT_EAC_R11G11_UNORM_BLOCK;
        break;
      case KTX_TTF_BC1_RGB:
        // Transcoding doesn't support BC1 alpha.
        vkFormat = srgb ? VK_FORMAT_BC1_RGB_SRGB_BLOCK
                        : VK_FORMAT_BC1_RGB_UNORM_BLOCK;
        break;
      case KTX_TTF_BC3_RGBA:
        vkFormat = srgb ? VK_FORMAT_BC3_SRGB_BLOCK
                        : VK_FORMAT_BC3_UNORM_BLOCK;
        break;
      case KTX_TTF_BC4_R:
        vkFormat = VK_FORMAT_BC4_UNORM_BLOCK;
        break;
      case KTX_TTF_BC5_RG:
        vkFormat = VK_FORMAT_BC5_UNORM_BLOCK;
        break;
      case KTX_TTF_PVRTC1_4_RGB:
      case KTX_TTF_PVRTC1_4_RGBA:
        vkFormat = srgb ? VK_FORMAT_PVRTC1_4BPP_SRGB_BLOCK_IMG
                        : VK_FORMAT_PVRTC1_4BPP_UNORM_BLOCK_IMG;
        break;
      case KTX_TTF_PVRTC2_4_RGB:
      case KTX_TTF_PVRTC2_4_RGBA:
        vkFormat = srgb ? VK_FORMAT_PVRTC2_4BPP_SRGB_BLOCK_IMG
                        : VK_FORMAT_PVRTC2_4BPP_UNORM_BLOCK_IMG;
        break;
      case KTX_TTF_BC7_RGBA:
        vkFormat = srgb ? VK_FORMAT_BC7_SRGB_BLOCK
                        : VK_FORMAT_BC7_UNORM_BLOCK;
        break;
      case KTX_TTF_ASTC_4x4_RGBA:
        vkFormat = srgb ? VK_FORMAT_ASTC_4x4_SRGB_BLOCK
                        : VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
        break;
      case KTX_TTF_RGB565:
        vkFormat = VK_FORMAT_R5G6B5_UNORM_PACK16;
        break;
      case KTX_TTF_BGR565:
        vkFormat = VK_FORMAT_B5G6R5_UNORM_PACK16;
        break;
      case KTX_TTF_RGBA4444:
        vkFormat = VK_FORMAT_R4G4B4A4_UNORM_PACK16;
        break;
      case KTX_TTF_RGBA32:
        vkFormat = srgb ? VK_FORMAT_R8G8B8A8_SRGB
                        : VK_FORMAT_R8G8B8A8_UNORM;
        break;
      default:
        return KTX_INVALID_VALUE;
    }

    // Create a prototype texture to use for calculating sizes in the target
    // format and, as useful side effects, provide us with a properly sized
    // data allocation and the DFD for the target format.
    ktxTextureCreateInfo createInfo;
    createInfo.glInternalformat = 0;
    createInfo.vkFormat = vkFormat;
    createInfo.baseWidth = This->baseWidth;
    createInfo.baseHeight = This->baseHeight;
    createInfo.baseDepth = This->baseDepth;
    createInfo.generateMipmaps = This->generateMipmaps;
    createInfo.isArray = This->isArray;
    createInfo.numDimensions = This->numDimensions;
    createInfo.numFaces = This->numFaces;
    createInfo.numLayers = This->numLayers;
    createInfo.numLevels = This->numLevels;
    createInfo.pDfd = nullptr;

    KTX_error_code result;
    ktxTexture2* prototype;
    result = ktxTexture2_Create(&createInfo, KTX_TEXTURE_CREATE_ALLOC_STORAGE,
                                &prototype);

    if (result != KTX_SUCCESS) {
        assert(result == KTX_OUT_OF_MEMORY); // The only run time error
        return result;
    }

    if (!This->pData) {
        if (ktxTexture_isActiveStream((ktxTexture*)This)) {
             // Load pending. Complete it.
            result = ktxTexture2_LoadImageData(This, NULL, 0);
            if (result != KTX_SUCCESS)
                return result;
        } else {
            // No data to transcode.
            return KTX_INVALID_OPERATION;
        }
    }

    // Transcoder global initialization. Requires ~9 milliseconds when compiled
    // and executed natively on a Core i7 2.2 GHz. If this is too slow, the
    // tables it computes can easily be moved to be compiled in.
    static bool transcoderInitialized;
    if (!transcoderInitialized)
        basisu_transcoder_init();

    if (This->supercompressionScheme == KTX_SS_BASIS_UNIVERSAL) {
        result = ktxTexture2_transcodeEtc1s(This, hasAlpha, prototype,
                                            outputFormat, transcodeFlags);
    } else {
        result = ktxTexture2_transcodeUastc(This, hasAlpha, prototype,
                                            outputFormat, transcodeFlags);
    }

    if (result == KTX_SUCCESS) {
        // Fix up the current texture
        DECLARE_PROTECTED(thisPrtctd, This);
        DECLARE_PRIVATE(protoPriv, prototype);
        DECLARE_PROTECTED(protoPrtctd, prototype);
        memcpy(&thisPrtctd._formatSize, &protoPrtctd._formatSize,
               sizeof(ktxFormatSize));
        This->vkFormat = vkFormat;
        This->isCompressed = prototype->isCompressed;
        This->supercompressionScheme = KTX_SS_NONE;
        priv._requiredLevelAlignment = protoPriv._requiredLevelAlignment;
        // Copy the levelIndex from the prototype to This.
        memcpy(priv._levelIndex, protoPriv._levelIndex,
               This->numLevels * sizeof(ktxLevelIndexEntry));
        // Move the DFD and data from the prototype to This.
        delete This->pDfd;
        This->pDfd = prototype->pDfd;
        prototype->pDfd = 0;
        delete This->pData;
        This->pData = prototype->pData;
        This->dataSize = prototype->dataSize;
        prototype->pData = 0;
        prototype->dataSize = 0;
    }
    ktxTexture2_Destroy(prototype);
    return result;
 }

#if 0
static basist::etc1_global_selector_codebook
            global_codebook(g_global_selector_cb_size, g_global_selector_cb);
#endif

/**
 * @memberof ktxTexture2 @private
 * @ingroup reader
 * @~English
 * @brief Transcode a KTX2 texture with Basis supercompressed images.
 *
 * Inflates the images from Basis Universal supercompression back to ETC1S
 * then transcodes them to the specified block-compressed format. The
 * transcoded images replace the original images and the texture's fields
 * including the DFD are modified to reflect the new format.
 *
 * Basis supercompressed textures must be transcoded to a desired target
 * block-compressed format before they can be uploaded to a GPU via a graphics
 * API.
 *
 * The following block compressed transcode targets are available: @c KTX_TTF_ETC1_RGB,
 * @c KTX_TTF_ETC2_RGBA, @c KTX_TTF_BC1_RGB, @c KTX_TTF_BC3_RGBA,
 * @c KTX_TTF_BC4_R, @c KTX_TTF_BC5_RG, @c KTX_TTF_BC7_RGBA,
 * @c @c KTX_TTF_PVRTC1_4_RGB, @c KTX_TTF_PVRTC1_4_RGBA,
 * @c KTX_TTF_PVRTC2_4_RGB, @c KTX_TTF_PVRTC2_4_RGBA, @c KTX_TTF_ASTC_4x4_RGBA,
 * @c KTX_TTF_ETC2_EAC_R11, @c KTX_TTF_ETC2_EAC_RG11, @c KTX_TTF_ETC and
 * @c KTX_TTF_BC1_OR_3.
 *
 * @c KTX_TTF_ETC automatically selects between @c KTX_TTF_ETC1_RGB and
 * @c KTX_TTF_ETC2_RGBA according to whether an alpha channel is available. @c KTX_TTF_BC1_OR_3
 * does likewise between @c KTX_TTF_BC1_RGB and @c KTX_TTF_BC3_RGBA. Note that if
 * @c KTX_TTF_PVRTC1_4_RGBA or @c KTX_TTF_PVRTC2_4_RGBA is specified and there is no alpha
 * channel @c KTX_TTF_PVRTC1_4_RGB or @c KTX_TTF_PVRTC2_4_RGB respectively will be selected.
 *
 * ATC & FXT1 formats are not supported by KTX2 & libktx as there are no equivalent Vulkan formats.
 *
 * The following uncompressed transcode targets are also available: @c KTX_TTF_RGBA32,
 * @c KTX_TTF_RGB565, KTX_TTF_BGR565 and KTX_TTF_RGBA4444.
 *
 * The following @p transcodeFlags are available.
 *
 * @sa ktxtexture2_CompressBasis().
 *
 * @param[in]   This         pointer to the ktxTexture2 object of interest.
 * @param[in]   outputFormat a value from the ktx_texture_transcode_fmt_e enum
 *                           specifying the target format.
 * @param[in]   transcodeFlags  bitfield of flags modifying the transcode
 *                           operation. @sa ktx_texture_decode_flags_e.
 *
 * @return      KTX_SUCCESS on success, other KTX_* enum values on error.
 *
 * @exception KTX_FILE_DATA_ERROR
 *                              Supercompression global data is corrupted.
 * @exception KTX_INVALID_OPERATION
 *                              The texture is not supercompressed.
 * @exception KTX_INVALID_OPERATION
 *                              Supercompression global data is missing, i.e.,
 *                              the texture object is invalid.
 * @exception KTX_INVALID_OPERATION
 *                              Image data is missing, i.e., the texture object
 *                              is invalid.
 * @exception KTX_INVALID_OPERATION
 *                              @p outputFormat is PVRTC1 but the texture does
 *                              does not have power-of-two dimensions.
 * @exception KTX_INVALID_VALUE @p outputFormat is invalid.
 * @exception KTX_TRANSCODE_FAILED
 *                              Something went wrong during transcoding. The
 *                              texture object will be corrupted.
 * @exception KTX_UNSUPPORTED_FEATURE
 *                              KTX_TF_PVRTC_DECODE_TO_NEXT_POW2 was requested
 *                              or the specified transcode target has not been
 *                              included in the library being used.
 * @exception KTX_OUT_OF_MEMORY Not enough memory to carry out transcoding.
 */
KTX_error_code
ktxTexture2_transcodeEtc1s(ktxTexture2* This,
                           bool hasAlpha,
                           ktxTexture2* prototype,
                           ktx_transcode_fmt_e outputFormat,
                           ktx_transcode_flags transcodeFlags)
{
    DECLARE_PRIVATE(priv, This);
    DECLARE_PRIVATE(protoPriv, prototype);
    KTX_error_code result = KTX_SUCCESS;

    assert(This->supercompressionScheme == KTX_SS_BASIS_UNIVERSAL);

    uint8_t* bgd = priv._supercompressionGlobalData;
    ktxBasisGlobalHeader& bgdh = *reinterpret_cast<ktxBasisGlobalHeader*>(bgd);
    if (!(bgdh.endpointsByteLength && bgdh.selectorsByteLength && bgdh.tablesByteLength)) {
            debug_printf("ktxTexture_TranscodeBasis: missing endpoints, selectors or tables");
            return KTX_FILE_DATA_ERROR;
    }

    // Compute some helpful numbers.
    //
    // firstImages contains the indices of the first images for each level to
    // ease finding the correct slice description when iterating from smallest
    // level to largest or when randomly accessing them (t.b.c). The last array
    // entry contains the total number of images, for calculating the offsets
    // of the endpoints, etc.
    uint32_t* firstImages = new uint32_t[This->numLevels+1];

    // Temporary invariant value
    uint32_t layersFaces = This->numLayers * This->numFaces;
    firstImages[0] = 0;
    for (uint32_t level = 1; level <= This->numLevels; level++) {
        // NOTA BENE: numFaces * depth is only reasonable because they can't
        // both be > 1. I.e there are no 3d cubemaps.
        firstImages[level] = firstImages[level - 1]
                           + layersFaces * MAX(This->baseDepth >> (level - 1), 1);
    }
    uint32_t& imageCount = firstImages[This->numLevels];

    if (BGD_TABLES_ADDR(0, bgdh, imageCount) + bgdh.tablesByteLength > priv._sgdByteLength) {
        return KTX_FILE_DATA_ERROR;
    }
    // FIXME: Do more validation.

    // Prepare low-level transcoder for transcoding slices.

    static basist::etc1_global_selector_codebook *global_codebook
        = new basist::etc1_global_selector_codebook(g_global_selector_cb_size,
                                                    g_global_selector_cb);

    ktxBasisImageTranscoder bit(global_codebook);

    bit.decode_palettes(bgdh.endpointCount, BGD_ENDPOINTS_ADDR(bgd, imageCount),
                        bgdh.endpointsByteLength,
                        bgdh.selectorCount, BGD_SELECTORS_ADDR(bgd, bgdh, imageCount),
                        bgdh.selectorsByteLength);

    bit.decode_tables(BGD_TABLES_ADDR(bgd, bgdh, imageCount),
                      bgdh.tablesByteLength);

    // Find matching VkFormat and calculate output sizes.

    assert(hasAlpha == ((bgdh.globalFlags & cBASISHeaderFlagHasAlphaSlices) != 0));
    const bool transcodeAlphaToOpaqueFormats
     = (hasAlpha && (transcodeFlags & KTX_TF_TRANSCODE_ALPHA_DATA_TO_OPAQUE_FORMATS));
    const bool isVideo = This->isVideo;

    ktx_uint8_t* basisData = This->pData;
    ktx_uint8_t* xcodedData = prototype->pData;
    ktxLevelIndexEntry* protoLevelIndex;
    uint64_t levelOffsetWrite;

    // Finally we're ready to transcode the slices.

    // FIXME: Iframe flag needs to be queryable by the application. In Basis
    // the app can query file_info and image_info from the transcoder which
    // returns a structure with lots of info about the image.

    protoLevelIndex = protoPriv._levelIndex;
    levelOffsetWrite = 0;
    for (int32_t level = This->numLevels - 1; level >= 0; level--) {
        uint64_t levelOffset = ktxTexture2_levelDataOffset(This, level);
        uint64_t writeOffset = levelOffsetWrite;
        const ktxBasisImageDesc* imageDescs = BGD_IMAGE_DESCS(bgd);
        uint32_t width = MAX(1, This->baseWidth >> level);
        uint32_t height = MAX(1, This->baseHeight >> level);
        uint32_t depth = MAX(1, This->baseDepth >> level);
        //uint32_t faceSlices = This->numFaces == 1 ? depth : This->numFaces;
        uint32_t faceSlices = This->numFaces * depth;
        uint32_t numImages = This->numLayers * faceSlices;
        uint32_t image = firstImages[level];
        uint32_t endImage = image + numImages;
        ktx_size_t levelImageSizeOut, levelSizeOut;

        // 4x4 is the ETC1S block size.
        const uint32_t num_blocks_x = getBlockWidth(width, 4);
        const uint32_t num_blocks_y = getBlockHeight(height, 4);

        levelSizeOut = 0;
        // FIXME: Figure out a way to get the size out of the transcoder.
        levelImageSizeOut = ktxTexture2_GetImageSize(prototype, level);
        for (; image < endImage; image++) {
            ktx_size_t bufferByteLength = prototype->dataSize - writeOffset;

            if (hasAlpha)
            {
                // The slice descriptions should have alpha information.
                if (imageDescs[image].alphaSliceByteOffset == 0
                    || imageDescs[image].alphaSliceByteLength == 0)
                    return KTX_FILE_DATA_ERROR;
            }

            result = bit.transcode_image(imageDescs[image], outputFormat,
                                xcodedData + writeOffset, bufferByteLength,
                                level,
                                basisData + levelOffset,
                                width, height, num_blocks_x, num_blocks_y,
                                isVideo, transcodeAlphaToOpaqueFormats);
            if (result != KTX_SUCCESS)
                goto cleanup;

            writeOffset += levelImageSizeOut;
            levelSizeOut += levelImageSizeOut;
        } // end images loop
        protoLevelIndex[level].byteOffset = levelOffsetWrite;
        protoLevelIndex[level].byteLength = levelSizeOut;
        protoLevelIndex[level].uncompressedByteLength = levelSizeOut;
        levelOffsetWrite += levelSizeOut;
        assert(levelOffsetWrite == writeOffset);
        // In case of transcoding to uncompressed.
        levelOffsetWrite = _KTX_PADN(protoPriv._requiredLevelAlignment,
                                     levelOffsetWrite);
    } // level loop

    result = KTX_SUCCESS;

cleanup:
    delete[] firstImages;
    return result;
}


KTX_error_code
ktxTexture2_transcodeUastc(ktxTexture2* This,
                           bool hasAlpha,
                           ktxTexture2* prototype,
                           ktx_transcode_fmt_e outputFormat,
                           ktx_transcode_flags transcodeFlags)
{
    assert(This->supercompressionScheme != KTX_SS_BASIS_UNIVERSAL);

    KTX_error_code result = KTX_SUCCESS;
    ktx_uint8_t* writePtr = prototype->pData;
    ktx_size_t bufferByteLength = prototype->dataSize;
    DECLARE_PRIVATE(protoPriv, prototype);
    ktxLevelIndexEntry* protoLevelIndex = protoPriv._levelIndex;
    ktx_size_t levelOffsetWrite = 0;

    ktxUastcImageTranscoder uit;
    for (ktx_int32_t level = This->numLevels - 1; level >= 0; level--)
    {
        ktx_uint32_t width, height, depth;
        uint64_t writeOffset = levelOffsetWrite;
        ktx_size_t levelImageSizeIn, levelImageSizeOut, levelSizeOut;
        ktx_uint32_t levelImageCount;
        const ktx_uint8_t* pDataIn = This->pData;

        /* Array textures have the same number of layers at each mip level. */
        width = MAX(1, This->baseWidth  >> level);
        height = MAX(1, This->baseHeight >> level);
        depth = MAX(1, This->baseDepth  >> level);

        // 4x4 is the UASTC block size.
        const uint32_t num_blocks_x = getBlockWidth(width, 4);
        const uint32_t num_blocks_y = getBlockHeight(height, 4);

        levelImageCount = This->numLayers * This->numFaces * depth;
        // Note: this is the inflated size of an image.
        levelImageSizeIn = ktxTexture_calcImageSize(ktxTexture(This), level,
                                                    KTX_FORMAT_VERSION_TWO);
        levelImageSizeOut = ktxTexture_calcImageSize(ktxTexture(prototype),
                                                     level,
                                                     KTX_FORMAT_VERSION_TWO);

        pDataIn += ktxTexture2_levelDataOffset(This, level);
        levelSizeOut = 0;
        for (uint32_t image = 0; image < levelImageCount; image++) {
            result = uit.transcode_image(outputFormat,
                              writePtr + writeOffset,
                              bufferByteLength,
                              level,
                              pDataIn,
                              (ktx_uint32_t)levelImageSizeIn,
                              width, height, num_blocks_x, num_blocks_y,
                              hasAlpha,
                              transcodeFlags);
            pDataIn += levelImageSizeIn;
            writeOffset += levelImageSizeOut;
            bufferByteLength -= levelImageSizeOut;
            levelSizeOut += levelImageSizeOut;
        }
        protoLevelIndex[level].byteOffset = levelOffsetWrite;
        // writeOffset will be equal to total size of the images in the level.
        protoLevelIndex[level].byteLength = levelSizeOut;
        protoLevelIndex[level].uncompressedByteLength = levelSizeOut;
        levelOffsetWrite += levelSizeOut;
    }
    // In case of transcoding to uncompressed.
    levelOffsetWrite = _KTX_PADN(protoPriv._requiredLevelAlignment,
                                 levelOffsetWrite);
    return KTX_SUCCESS;
}