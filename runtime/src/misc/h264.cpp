// Wii U h264.rpl HLE implementation for BotW-Native
// Provides H.264 Annex-B SPS/NAL parsing and hardware/software NV12 frame decoding
// for pre-rendered .mp4 cutscenes (sead::MoviePlayer / MP4DecodeThread).

#include "../hle.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#endif

namespace {

// H264DEC status codes
constexpr uint32_t H264_STATUS_OK                = 0;
constexpr uint32_t H264_STATUS_HAS_NOT_ENOUGH    = 100;
constexpr uint32_t H264_STATUS_OUT_FRAME_FLAG    = 0x80;
constexpr uint32_t H264_ERR_PARAM                = 0xFF000001u;
constexpr uint32_t H264_ERR_OUT_OF_MEMORY        = 0xFF080000u;

// Parameter IDs for H264DECSetParam
constexpr uint32_t H264_PARAM_FPTR_OUTPUT        = 0x00000001u;
constexpr uint32_t H264_PARAM_OUTPUT_PER_FRAME   = 0x20000002u;
constexpr uint32_t H264_PARAM_USER_MEMORY        = 0x70000001u;

struct BitReader {
    const uint8_t* data;
    size_t size;
    size_t bytePos = 0;
    int bitPos = 0; // 0..7, MSB first

    BitReader(const uint8_t* d, size_t s) : data(d), size(s) {}

    bool eof() const { return bytePos >= size; }

    uint32_t readBit() {
        if (bytePos >= size) return 0;
        uint32_t bit = (data[bytePos] >> (7 - bitPos)) & 1u;
        if (++bitPos == 8) {
            bitPos = 0;
            ++bytePos;
        }
        return bit;
    }

    uint32_t readBits(int n) {
        uint32_t val = 0;
        for (int i = 0; i < n; ++i) {
            val = (val << 1) | readBit();
        }
        return val;
    }

    uint32_t readUE() {
        int leadingZeroBits = 0;
        while (readBit() == 0 && !eof() && leadingZeroBits < 31) {
            ++leadingZeroBits;
        }
        if (leadingZeroBits == 0) return 0;
        return ((1u << leadingZeroBits) - 1u) + readBits(leadingZeroBits);
    }

    int32_t readSE() {
        uint32_t ue = readUE();
        if (ue == 0) return 0;
        int32_t val = static_cast<int32_t>((ue + 1u) >> 1);
        return (ue & 1u) ? val : -val;
    }
};

struct SpsInfo {
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t cropEnable = 0;
    uint32_t cropTop = 0;
    uint32_t cropBottom = 0;
    uint32_t cropLeft = 0;
    uint32_t cropRight = 0;
    bool valid = false;
};

static void skipScalingList(BitReader& br, int sizeOfScalingList) {
    int lastScale = 8;
    int nextScale = 8;
    for (int j = 0; j < sizeOfScalingList; ++j) {
        if (nextScale != 0) {
            int32_t deltaScale = br.readSE();
            nextScale = (lastScale + deltaScale + 256) % 256;
        }
        lastScale = (nextScale == 0) ? lastScale : nextScale;
    }
}

// Parse H.264 SPS NAL unit (starting at the NAL header byte 0x67 / type 7)
static bool parseSpsNal(const uint8_t* nal, size_t nalLen, SpsInfo& out) {
    if (!nal || nalLen < 4) return false;
    uint8_t nalType = nal[0] & 0x1Fu;
    if (nalType != 7) return false;

    // Remove emulation prevention bytes (0x00 0x00 0x03 -> 0x00 0x00)
    std::vector<uint8_t> rbsp;
    rbsp.reserve(nalLen);
    for (size_t i = 1; i < nalLen; ++i) {
        if (i + 2 < nalLen && nal[i] == 0x00 && nal[i + 1] == 0x00 && nal[i + 2] == 0x03) {
            rbsp.push_back(0x00);
            rbsp.push_back(0x00);
            i += 2;
        } else {
            rbsp.push_back(nal[i]);
        }
    }
    if (rbsp.size() < 3) return false;

    BitReader br(rbsp.data(), rbsp.size());
    uint32_t profileIdc = br.readBits(8);
    br.readBits(8); // constraint_set flags + reserved
    br.readBits(8); // level_idc
    br.readUE();    // seq_parameter_set_id

    uint32_t chromaFormatIdc = 1;
    uint32_t separateColourPlaneFlag = 0;
    if (profileIdc == 100 || profileIdc == 110 || profileIdc == 122 ||
        profileIdc == 244 || profileIdc == 44  || profileIdc == 83  ||
        profileIdc == 86  || profileIdc == 118 || profileIdc == 128 ||
        profileIdc == 138 || profileIdc == 139 || profileIdc == 134 || profileIdc == 135) {
        chromaFormatIdc = br.readUE();
        if (chromaFormatIdc == 3) {
            separateColourPlaneFlag = br.readBit();
        }
        br.readUE(); // bit_depth_luma_minus8
        br.readUE(); // bit_depth_chroma_minus8
        br.readBit(); // qpprime_y_zero_transform_bypass_flag
        uint32_t seqScalingMatrixPresentFlag = br.readBit();
        if (seqScalingMatrixPresentFlag) {
            int count = (chromaFormatIdc != 3) ? 8 : 12;
            for (int i = 0; i < count; ++i) {
                uint32_t seqScalingListPresent = br.readBit();
                if (seqScalingListPresent) {
                    skipScalingList(br, (i < 6) ? 16 : 64);
                }
            }
        }
    }

    br.readUE(); // log2_max_frame_num_minus4
    uint32_t picOrderCntType = br.readUE();
    if (picOrderCntType == 0) {
        br.readUE(); // log2_max_pic_order_cnt_lsb_minus4
    } else if (picOrderCntType == 1) {
        br.readBit(); // delta_pic_order_always_zero_flag
        br.readSE();  // offset_for_non_ref_pic
        br.readSE();  // offset_for_top_to_bottom_field
        uint32_t numRefFramesInPicOrderCntCycle = br.readUE();
        for (uint32_t i = 0; i < numRefFramesInPicOrderCntCycle && !br.eof(); ++i) {
            br.readSE();
        }
    }

    br.readUE();  // max_num_ref_frames
    br.readBit(); // gaps_in_frame_num_value_allowed_flag

    uint32_t picWidthInMbsMinus1 = br.readUE();
    uint32_t picHeightInMapUnitsMinus1 = br.readUE();
    uint32_t frameMbsOnlyFlag = br.readBit();
    if (!frameMbsOnlyFlag) {
        br.readBit(); // mb_adaptive_frame_field_flag
    }
    br.readBit(); // direct_8x8_inference_flag

    uint32_t frameCroppingFlag = br.readBit();
    uint32_t cropLeft = 0, cropRight = 0, cropTop = 0, cropBottom = 0;
    if (frameCroppingFlag) {
        cropLeft   = br.readUE();
        cropRight  = br.readUE();
        cropTop    = br.readUE();
        cropBottom = br.readUE();
    }

    uint32_t width  = (picWidthInMbsMinus1 + 1u) * 16u;
    uint32_t height = (2u - frameMbsOnlyFlag) * (picHeightInMapUnitsMinus1 + 1u) * 16u;

    uint32_t chromaArrayType = separateColourPlaneFlag ? 0u : chromaFormatIdc;
    uint32_t cropUnitX = 1u;
    uint32_t cropUnitY = 2u - frameMbsOnlyFlag;
    if (chromaArrayType != 0) {
        uint32_t subWidthC  = (chromaFormatIdc == 3) ? 1u : 2u;
        uint32_t subHeightC = (chromaFormatIdc == 1) ? 2u : 1u;
        cropUnitX = subWidthC;
        cropUnitY = subHeightC * (2u - frameMbsOnlyFlag);
    }

    uint32_t subW = (cropLeft + cropRight) * cropUnitX;
    uint32_t subH = (cropTop + cropBottom) * cropUnitY;
    if (subW < width)  width  -= subW;
    if (subH < height) height -= subH;

    if (width == 0 || width > 8192 || height == 0 || height > 8192) {
        return false;
    }

    out.width      = width;
    out.height     = height;
    out.cropEnable = frameCroppingFlag ? 1u : 0u;
    out.cropTop    = cropTop * cropUnitY;
    out.cropBottom = cropBottom * cropUnitY;
    out.cropLeft   = cropLeft * cropUnitX;
    out.cropRight  = cropRight * cropUnitX;
    out.valid      = true;
    return true;
}

struct NalSpan {
    const uint8_t* ptr;
    size_t len;
    uint8_t type;
};

// Enumerate Annex-B NAL units in a buffer
static std::vector<NalSpan> scanAnnexBNals(const uint8_t* buf, size_t len) {
    std::vector<NalSpan> nals;
    if (!buf || len < 4) return nals;

    size_t i = 0;
    while (i + 3 <= len) {
        size_t scLen = 0;
        if (buf[i] == 0x00 && buf[i + 1] == 0x00 && buf[i + 2] == 0x01) {
            scLen = 3;
        } else if (i + 4 <= len && buf[i] == 0x00 && buf[i + 1] == 0x00 && buf[i + 2] == 0x00 && buf[i + 3] == 0x01) {
            scLen = 4;
        }
        if (scLen == 0) {
            ++i;
            continue;
        }
        size_t nalStart = i + scLen;
        size_t j = nalStart;
        while (j + 3 <= len) {
            if (buf[j] == 0x00 && buf[j + 1] == 0x00 &&
                (buf[j + 2] == 0x01 || (j + 3 < len && buf[j + 2] == 0x00 && buf[j + 3] == 0x01))) {
                break;
            }
            ++j;
        }
        size_t nalEnd = (j + 3 <= len) ? j : len;
        if (nalEnd > nalStart) {
            nals.push_back(NalSpan{buf + nalStart, nalEnd - nalStart, static_cast<uint8_t>(buf[nalStart] & 0x1Fu)});
        }
        i = nalEnd;
    }
    return nals;
}

#ifdef _WIN32
// CLSID_CMSH264DecoderMFT: {62CE7E72-4C71-4D20-B15D-452831A87D9D}
static const GUID kCLSID_CMSH264DecoderMFT =
    {0x62CE7E72, 0x4C71, 0x4D20, {0xB1, 0x5D, 0x45, 0x28, 0x31, 0xA8, 0x7D, 0x9D}};

// CODECAPI_AVLowLatencyMode: {9C27891A-ED7A-40E1-88E8-B22727A024EE}
static const GUID kCODECAPI_AVLowLatencyMode =
    {0x9C27891A, 0xED7A, 0x40E1, {0x88, 0xE8, 0xB2, 0x27, 0x27, 0xA0, 0x24, 0xEE}};

static std::once_flag g_mfInitOnce;
static bool g_mfInitialized = false;

static void ensureMfInitialized() {
    std::call_once(g_mfInitOnce, []() {
        HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        (void)hrCo;
        HRESULT hrMf = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
        g_mfInitialized = SUCCEEDED(hrMf);
        if (!g_mfInitialized) {
            LOG("h264: MFStartup failed (hr=0x%08X)", static_cast<unsigned>(hrMf));
        }
    });
}
#endif

struct H264DecoderSession {
    uint32_t memPtr = 0;
    uint32_t memSize = 0;
    uint32_t fptrOutput = 0;
    uint32_t outputPerFrame = 0;
    uint32_t userMemory = 0;

    uint32_t bitstreamPtr = 0;
    uint32_t bitstreamLen = 0;
    double   timestamp = 0.0;

    SpsInfo  sps{};
    std::vector<uint8_t> lastSpsNal;
    std::vector<uint8_t> lastPpsNal;
    std::vector<uint8_t> lastFrameNV12;
    uint32_t decodedFrameCount = 0;
    bool     sentSpsPpsToDecoder = false;

#ifdef _WIN32
    IMFTransform* mft = nullptr;
    DWORD outSampleSize = 0;
    int32_t outStride = 0;
    uint32_t outPaddedHeight = 0;

    void releaseMft() {
        if (mft) {
            mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            mft->Release();
            mft = nullptr;
        }
        sentSpsPpsToDecoder = false;
    }

    bool configureOutputType(uint32_t width, uint32_t height) {
        if (!mft) return false;
        for (DWORD idx = 0; idx < 64; ++idx) {
            IMFMediaType* outType = nullptr;
            HRESULT hr = mft->GetOutputAvailableType(0, idx, &outType);
            if (FAILED(hr) || !outType) break;
            GUID subtype{};
            outType->GetGUID(MF_MT_SUBTYPE, &subtype);
            if (IsEqualGUID(subtype, MFVideoFormat_NV12)) {
                hr = mft->SetOutputType(0, outType, 0);
                UINT32 w = width, h = height;
                MFGetAttributeSize(outType, MF_MT_FRAME_SIZE, &w, &h);
                UINT32 strideAttr = 0;
                if (SUCCEEDED(outType->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideAttr))) {
                    outStride = static_cast<int32_t>(strideAttr);
                } else {
                    outStride = static_cast<int32_t>(w);
                }
                outPaddedHeight = h;
                outType->Release();
                if (SUCCEEDED(hr)) {
                    MFT_OUTPUT_STREAM_INFO osi{};
                    if (SUCCEEDED(mft->GetOutputStreamInfo(0, &osi))) {
                        outSampleSize = osi.cbSize;
                    }
                    return true;
                }
                break;
            }
            outType->Release();
        }
        return false;
    }

    bool initMft(uint32_t width, uint32_t height) {
        releaseMft();
        ensureMfInitialized();
        if (!g_mfInitialized) return false;

        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        HRESULT hr = CoCreateInstance(
            kCLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&mft));
        if (FAILED(hr) || !mft) {
            LOG("h264: CoCreateInstance(CLSID_CMSH264DecoderMFT) failed (0x%08X)", static_cast<unsigned>(hr));
            mft = nullptr;
            return false;
        }

        // Enable low-latency mode so decoded frames are produced immediately without B-frame delay
        IMFAttributes* attrs = nullptr;
        if (SUCCEEDED(mft->GetAttributes(&attrs)) && attrs) {
            attrs->SetUINT32(kCODECAPI_AVLowLatencyMode, 1);
            attrs->Release();
        }

        IMFMediaType* inType = nullptr;
        if (SUCCEEDED(MFCreateMediaType(&inType)) && inType) {
            inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
            MFSetAttributeSize(inType, MF_MT_FRAME_SIZE, width, height);
            inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            hr = mft->SetInputType(0, inType, 0);
            inType->Release();
            if (FAILED(hr)) {
                LOG("h264: SetInputType failed (0x%08X)", static_cast<unsigned>(hr));
                releaseMft();
                return false;
            }
        }

        configureOutputType(width, height);
        mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    bool drainMftOutputs(uint32_t width, uint32_t height) {
        if (!mft) return false;
        bool gotFrame = false;
        uint32_t dstPitch = (width + 255u) & ~255u;
        size_t nv12Size = static_cast<size_t>(dstPitch) * height * 3u / 2u;

        while (true) {
            MFT_OUTPUT_STREAM_INFO osi{};
            mft->GetOutputStreamInfo(0, &osi);
            bool mftAllocates = (osi.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

            IMFSample* outSample = nullptr;
            IMFMediaBuffer* outBuf = nullptr;
            if (!mftAllocates) {
                DWORD allocSize = std::max<DWORD>(osi.cbSize, static_cast<DWORD>(dstPitch * ((height + 31u) & ~31u) * 2u));
                if (FAILED(MFCreateSample(&outSample)) || !outSample) break;
                if (FAILED(MFCreateMemoryBuffer(allocSize, &outBuf)) || !outBuf) {
                    outSample->Release();
                    break;
                }
                outSample->AddBuffer(outBuf);
            }

            MFT_OUTPUT_DATA_BUFFER odb{};
            odb.dwStreamID = 0;
            odb.pSample = outSample;
            DWORD status = 0;
            HRESULT hr = mft->ProcessOutput(0, 1, &odb, &status);
            if (odb.pEvents) {
                odb.pEvents->Release();
                odb.pEvents = nullptr;
            }

            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (outBuf) outBuf->Release();
                if (outSample) outSample->Release();
                configureOutputType(width, height);
                continue;
            }
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT || FAILED(hr)) {
                if (outBuf) outBuf->Release();
                if (outSample) outSample->Release();
                break;
            }

            IMFSample* sampleToRead = odb.pSample ? odb.pSample : outSample;
            if (sampleToRead) {
                IMFMediaBuffer* contigBuf = nullptr;
                if (SUCCEEDED(sampleToRead->ConvertToContiguousBuffer(&contigBuf)) && contigBuf) {
                    BYTE* rawPtr = nullptr;
                    DWORD maxLen = 0, curLen = 0;
                    if (SUCCEEDED(contigBuf->Lock(&rawPtr, &maxLen, &curLen)) && rawPtr && curLen > 0) {
                        uint32_t srcStride = (outStride > 0) ? static_cast<uint32_t>(outStride) : width;
                        uint32_t totalRows = curLen / std::max(1u, srcStride);
                        uint32_t paddedH = (totalRows >= height * 3u / 2u) ? (totalRows * 2u / 3u) : height;
                        if (outPaddedHeight >= height && srcStride * outPaddedHeight * 3u / 2u <= curLen) {
                            paddedH = outPaddedHeight;
                        }

                        lastFrameNV12.assign(nv12Size, 0);
                        uint8_t* dstY  = lastFrameNV12.data();
                        uint8_t* dstUV = dstY + static_cast<size_t>(dstPitch) * height;
                        const uint8_t* srcY  = rawPtr;
                        const uint8_t* srcUV = rawPtr + static_cast<size_t>(srcStride) * paddedH;

                        uint32_t copyW = std::min(width, srcStride);
                        for (uint32_t y = 0; y < height; ++y) {
                            if (static_cast<size_t>((y + 1) * srcStride) <= curLen) {
                                std::memcpy(dstY + static_cast<size_t>(y) * dstPitch,
                                            srcY + static_cast<size_t>(y) * srcStride,
                                            copyW);
                            }
                        }
                        uint32_t uvRows = height / 2u;
                        for (uint32_t y = 0; y < uvRows; ++y) {
                            size_t uvOffset = static_cast<size_t>(srcStride) * paddedH + static_cast<size_t>(y) * srcStride;
                            if (uvOffset + copyW <= curLen) {
                                std::memcpy(dstUV + static_cast<size_t>(y) * dstPitch,
                                            srcUV + static_cast<size_t>(y) * srcStride,
                                            copyW);
                            }
                        }
                        gotFrame = true;
                        contigBuf->Unlock();
                    }
                    contigBuf->Release();
                }
            }

            if (mftAllocates && odb.pSample) {
                odb.pSample->Release();
            }
            if (outBuf) outBuf->Release();
            if (outSample) outSample->Release();
        }
        return gotFrame;
    }

    bool feedAnnexB(const uint8_t* data, size_t len, uint64_t sampleTime100ns, uint32_t width, uint32_t height) {
        if (!mft && !initMft(width, height)) return false;

        IMFSample* inSample = nullptr;
        IMFMediaBuffer* inBuf = nullptr;
        if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(len), &inBuf)) || !inBuf) {
            return false;
        }
        BYTE* dst = nullptr;
        if (SUCCEEDED(inBuf->Lock(&dst, nullptr, nullptr)) && dst) {
            std::memcpy(dst, data, len);
            inBuf->Unlock();
            inBuf->SetCurrentLength(static_cast<DWORD>(len));
        }
        if (FAILED(MFCreateSample(&inSample)) || !inSample) {
            inBuf->Release();
            return false;
        }
        inSample->AddBuffer(inBuf);
        inSample->SetSampleTime(static_cast<LONGLONG>(sampleTime100ns));
        inSample->SetSampleDuration(333333);

        HRESULT hr = mft->ProcessInput(0, inSample, 0);
        if (hr == MF_E_NOTACCEPTING) {
            drainMftOutputs(width, height);
            hr = mft->ProcessInput(0, inSample, 0);
        }
        inBuf->Release();
        inSample->Release();

        return drainMftOutputs(width, height);
    }
#endif

    ~H264DecoderSession() {
#ifdef _WIN32
        releaseMft();
#endif
    }
};

static std::mutex g_h264Mutex;
static std::unordered_map<uint32_t, H264DecoderSession> g_sessions;
static SpsInfo g_lastParsedSps{};

static H264DecoderSession& getSessionLocked(uint32_t memPtr) {
    auto& sess = g_sessions[memPtr];
    sess.memPtr = memPtr;
    if (!sess.sps.valid && g_lastParsedSps.valid) {
        sess.sps = g_lastParsedSps;
    }
    return sess;
}

static void resolveMemAndSize(uint32_t a0, uint32_t a1, uint32_t& outMem, uint32_t& outSize) {
    bool a0IsPtr = (a0 >= 0x10000000u && a0 < 0x50000000u);
    bool a1IsPtr = (a1 >= 0x10000000u && a1 < 0x50000000u);
    if (a1IsPtr && !a0IsPtr) {
        outSize = a0;
        outMem  = a1;
    } else {
        outMem  = a0;
        outSize = a1;
    }
}

} // namespace

// H264DECMemoryRequirement(profile, level, maxWidth, maxHeight, outMemoryRequirement)
HLE_EXPORT(h264, H264DECMemoryRequirement) {
    uint32_t maxWidth  = ARG(2);
    uint32_t maxHeight = ARG(3);
    uint32_t outReqPtr = ARG(4);

    if (outReqPtr == 0) {
        RET(H264_ERR_PARAM);
        return;
    }
    if (maxWidth == 0 || maxWidth > 4096)   maxWidth  = 1280;
    if (maxHeight == 0 || maxHeight > 4096) maxHeight = 720;

    uint32_t pitch = (maxWidth + 255u) & ~255u;
    uint32_t frameBytes = pitch * maxHeight * 3u / 2u;
    uint32_t req = (frameBytes * 6u + 0x80000u) & ~0x1FFu;
    req = std::clamp(req, 0x400000u, 0x2000000u);

    mem::w32(outReqPtr, req);
    RET(H264_STATUS_OK);
}

// H264DECCheckMemSegmentation(memory, size)
HLE_EXPORT(h264, H264DECCheckMemSegmentation) {
    RET(H264_STATUS_OK);
}

// H264DECInitParam(memorySize, memory)
HLE_EXPORT(h264, H264DECInitParam) {
    uint32_t memPtr = 0, memSize = 0;
    resolveMemAndSize(ARG(0), ARG(1), memPtr, memSize);
    if (memPtr == 0) {
        RET(H264_ERR_PARAM);
        return;
    }

    std::lock_guard<std::mutex> lock(g_h264Mutex);
    auto& sess = getSessionLocked(memPtr);
    sess.memSize = memSize;
    sess.fptrOutput = 0;
    sess.outputPerFrame = 0;
    sess.userMemory = 0;
    sess.bitstreamPtr = 0;
    sess.bitstreamLen = 0;
    sess.timestamp = 0.0;
    sess.decodedFrameCount = 0;

    if (memSize >= 0x200) {
        mem::commit(memPtr, 0x200, true);
        std::memset(mem::host(memPtr), 0, 0x200);
    }
    RET(H264_STATUS_OK);
}

// H264DECSetParam_FPTR_OUTPUT(memory, funcPtr)
HLE_EXPORT(h264, H264DECSetParam_FPTR_OUTPUT) {
    uint32_t memPtr  = ARG(0);
    uint32_t funcPtr = ARG(1);
    if (memPtr == 0) {
        RET(H264_ERR_PARAM);
        return;
    }
    std::lock_guard<std::mutex> lock(g_h264Mutex);
    auto& sess = getSessionLocked(memPtr);
    sess.fptrOutput = funcPtr;
    RET(H264_STATUS_OK);
}

// H264DECSetParam_OUTPUT_PER_FRAME(memory, value)
HLE_EXPORT(h264, H264DECSetParam_OUTPUT_PER_FRAME) {
    uint32_t memPtr = ARG(0);
    uint32_t value  = ARG(1);
    if (memPtr == 0) {
        RET(H264_ERR_PARAM);
        return;
    }
    std::lock_guard<std::mutex> lock(g_h264Mutex);
    auto& sess = getSessionLocked(memPtr);
    sess.outputPerFrame = value;
    RET(H264_STATUS_OK);
}

// H264DECSetParam_USER_MEMORY(memory, valuePtr)
HLE_EXPORT(h264, H264DECSetParam_USER_MEMORY) {
    uint32_t memPtr   = ARG(0);
    uint32_t valuePtr = ARG(1);
    if (memPtr == 0 || valuePtr == 0) {
        RET(H264_ERR_PARAM);
        return;
    }
    uint32_t userMem = mem::r32(valuePtr);
    std::lock_guard<std::mutex> lock(g_h264Mutex);
    auto& sess = getSessionLocked(memPtr);
    sess.userMemory = userMem;
    RET(H264_STATUS_OK);
}

// H264DECSetParam(memory, paramId, valuePtr)
HLE_EXPORT(h264, H264DECSetParam) {
    uint32_t memPtr   = ARG(0);
    uint32_t paramId  = ARG(1);
    uint32_t valuePtr = ARG(2);
    if (memPtr == 0) {
        RET(H264_ERR_PARAM);
        return;
    }
    std::lock_guard<std::mutex> lock(g_h264Mutex);
    auto& sess = getSessionLocked(memPtr);
    switch (paramId) {
        case H264_PARAM_FPTR_OUTPUT:
            sess.fptrOutput = valuePtr;
            break;
        case H264_PARAM_OUTPUT_PER_FRAME:
            sess.outputPerFrame = valuePtr;
            break;
        case H264_PARAM_USER_MEMORY:
            // value is void** (same as SetParam_USER_MEMORY): store the pointed-to pointer
            sess.userMemory = valuePtr ? mem::r32(valuePtr) : 0;
            break;
        default:
            break;
    }
    RET(H264_STATUS_OK);
}

// H264DECOpen(memory)
HLE_EXPORT(h264, H264DECOpen) {
    uint32_t memPtr = ARG(0);
    if (memPtr == 0) {
        RET(H264_ERR_PARAM);
        return;
    }
    std::lock_guard<std::mutex> lock(g_h264Mutex);
    getSessionLocked(memPtr);
    RET(H264_STATUS_OK);
}

// H264DECBegin(memory)
HLE_EXPORT(h264, H264DECBegin) {
    uint32_t memPtr = ARG(0);
    if (memPtr == 0) {
        RET(H264_ERR_PARAM);
        return;
    }
    std::lock_guard<std::mutex> lock(g_h264Mutex);
    auto& sess = getSessionLocked(memPtr);
    sess.decodedFrameCount = 0;
    sess.lastFrameNV12.clear();
#ifdef _WIN32
    sess.releaseMft();
#endif
    RET(H264_STATUS_OK);
}

// H264DECGetImageSize(bitstream, length, offset, outWidth, outHeight)
HLE_EXPORT(h264, H264DECGetImageSize) {
    uint32_t bsPtr     = ARG(0);
    uint32_t bsLen     = ARG(1);
    uint32_t offset    = ARG(2);
    uint32_t outWPtr   = ARG(3);
    uint32_t outHPtr   = ARG(4);

    if (bsPtr == 0 || outWPtr == 0 || outHPtr == 0 || offset >= bsLen) {
        RET(H264_ERR_PARAM);
        return;
    }

    size_t len = bsLen - offset;
    mem::commit(bsPtr + offset, len, false);
    const uint8_t* data = mem::host(bsPtr + offset);

    SpsInfo sps{};
    auto nals = scanAnnexBNals(data, len);
    for (const auto& nal : nals) {
        if (nal.type == 7 && parseSpsNal(nal.ptr, nal.len, sps)) {
            break;
        }
    }

    std::lock_guard<std::mutex> lock(g_h264Mutex);
    if (sps.valid) {
        g_lastParsedSps = sps;
    } else if (g_lastParsedSps.valid) {
        sps = g_lastParsedSps;
    } else {
        sps.width  = 1280;
        sps.height = 720;
        sps.valid  = true;
    }

    mem::w32(outWPtr, sps.width);
    mem::w32(outHPtr, sps.height);
    RET(H264_STATUS_OK);
}

// H264DECSetBitstream(memory, bitstream, length, timestamp)
HLE_EXPORT(h264, H264DECSetBitstream) {
    uint32_t memPtr = ARG(0);
    uint32_t bsPtr  = ARG(1);
    uint32_t bsLen  = ARG(2);
    double   ts     = ARGF(0);

    if (memPtr == 0 || bsPtr == 0) {
        RET(H264_ERR_PARAM);
        return;
    }

    std::lock_guard<std::mutex> lock(g_h264Mutex);
    auto& sess = getSessionLocked(memPtr);
    sess.bitstreamPtr = bsPtr;
    sess.bitstreamLen = bsLen;
    sess.timestamp    = ts;
    RET(H264_STATUS_OK);
}

// H264DECExecute(memory, frameBuffer)
HLE_EXPORT(h264, H264DECExecute) {
    uint32_t memPtr      = ARG(0);
    uint32_t frameBuffer = ARG(1);
    if (memPtr == 0 || frameBuffer == 0) {
        RET(H264_ERR_PARAM);
        return;
    }

    uint32_t fptrOutput = 0;
    uint32_t userMemory = 0;
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t pitch = 1280;
    SpsInfo sps{};
    double timestamp = 0.0;
    bool emitFrame = false;

    {
        std::lock_guard<std::mutex> lock(g_h264Mutex);
        auto& sess = getSessionLocked(memPtr);
        fptrOutput = sess.fptrOutput;
        userMemory = sess.userMemory;
        timestamp  = sess.timestamp;

        size_t bsLen = sess.bitstreamLen;
        const uint8_t* bsData = nullptr;
        if (sess.bitstreamPtr != 0 && bsLen > 0) {
            mem::commit(sess.bitstreamPtr, bsLen, false);
            bsData = mem::host(sess.bitstreamPtr);
        }

        bool hasSlice = false;
        if (bsData && bsLen > 0) {
            auto nals = scanAnnexBNals(bsData, bsLen);
            for (const auto& nal : nals) {
                if (nal.type == 7) {
                    SpsInfo parsed{};
                    if (parseSpsNal(nal.ptr, nal.len, parsed)) {
                        sess.sps = parsed;
                        g_lastParsedSps = parsed;
                    }
                    sess.lastSpsNal.assign(nal.ptr, nal.ptr + nal.len);
                } else if (nal.type == 8) {
                    sess.lastPpsNal.assign(nal.ptr, nal.ptr + nal.len);
                } else if (nal.type >= 1 && nal.type <= 5) {
                    hasSlice = true;
                }
            }
        }

        if (sess.sps.valid) {
            sps = sess.sps;
        } else if (g_lastParsedSps.valid) {
            sps = g_lastParsedSps;
            sess.sps = sps;
        } else {
            sps.width = 1280;
            sps.height = 720;
            sps.valid = true;
        }
        width  = sps.width;
        height = sps.height;
        pitch  = (width + 255u) & ~255u;

        bool decodedNew = false;
#ifdef _WIN32
        if (bsData && bsLen > 0) {
            std::vector<uint8_t> combined;
            const uint8_t* feedPtr = bsData;
            size_t feedLen = bsLen;
            if (!sess.sentSpsPpsToDecoder && (!sess.lastSpsNal.empty() || !sess.lastPpsNal.empty())) {
                static const uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};
                if (!sess.lastSpsNal.empty()) {
                    combined.insert(combined.end(), kStartCode, kStartCode + 4);
                    combined.insert(combined.end(), sess.lastSpsNal.begin(), sess.lastSpsNal.end());
                }
                if (!sess.lastPpsNal.empty()) {
                    combined.insert(combined.end(), kStartCode, kStartCode + 4);
                    combined.insert(combined.end(), sess.lastPpsNal.begin(), sess.lastPpsNal.end());
                }
                combined.insert(combined.end(), bsData, bsData + bsLen);
                feedPtr = combined.data();
                feedLen = combined.size();
                sess.sentSpsPpsToDecoder = true;
            }
            uint64_t pts100ns = static_cast<uint64_t>(sess.decodedFrameCount) * 333333ULL;
            decodedNew = sess.feedAnnexB(feedPtr, feedLen, pts100ns, width, height);
        }
#endif

        emitFrame = hasSlice || decodedNew;
        if (emitFrame) {
            size_t ySize  = static_cast<size_t>(pitch) * height;
            size_t uvSize = static_cast<size_t>(pitch) * (height / 2u);
            mem::commit(frameBuffer, ySize + uvSize, true);
            uint8_t* dst  = mem::host(frameBuffer);

            if (sess.lastFrameNV12.size() == ySize + uvSize) {
                std::memcpy(dst, sess.lastFrameNV12.data(), ySize + uvSize);
            } else {
                std::memset(dst, 16, ySize);
                std::memset(dst + ySize, 128, uvSize);
            }
            ++sess.decodedFrameCount;
        }
    }

    if (!emitFrame) {
        RET(H264_STATUS_HAS_NOT_ENOUGH);
        return;
    }

    if (fptrOutput != 0) {
        uint32_t outputAddr   = memPtr + 0x100u;
        uint32_t ptrArrayAddr = memPtr + 0x110u;
        uint32_t resultAddr   = memPtr + 0x140u;

        mem::commit(outputAddr, 0xC0, true);
        std::memset(mem::host(outputAddr), 0, 0xC0);

        uint64_t tsBits = 0;
        std::memcpy(&tsBits, &timestamp, sizeof(double));

        // H264DecodeResult (0x78 bytes)
        mem::w32(resultAddr + 0x00, 100u);        // status (100 = decoded frame ready)
        mem::w64(resultAddr + 0x08, tsBits);      // timestamp (double BE)
        mem::w32(resultAddr + 0x10, width);       // inputWidth
        mem::w32(resultAddr + 0x14, height);      // inputHeight
        mem::w32(resultAddr + 0x18, pitch);       // nextLine (row pitch)
        mem::w32(resultAddr + 0x1C, sps.cropEnable);
        mem::w32(resultAddr + 0x20, sps.cropTop);
        mem::w32(resultAddr + 0x24, sps.cropBottom);
        mem::w32(resultAddr + 0x28, sps.cropLeft);
        mem::w32(resultAddr + 0x2C, sps.cropRight);
        mem::w32(resultAddr + 0x44, frameBuffer); // framebuffer pointer at +68 (0x44)

        // H264DecodeResult* array[1]
        mem::w32(ptrArrayAddr, resultAddr);

        // H264DecodeOutput (0x0C bytes)
        mem::w32(outputAddr + 0x00, 1u);          // frameCount = 1
        mem::w32(outputAddr + 0x04, ptrArrayAddr);// decodeResults = &resultPtr
        mem::w32(outputAddr + 0x08, userMemory);  // userMemory

        guest_call(c, fptrOutput, {outputAddr});
    }

    RET(H264_STATUS_OUT_FRAME_FLAG | H264_STATUS_HAS_NOT_ENOUGH);
}

// H264DECFlush(memory)
HLE_EXPORT(h264, H264DECFlush) {
    uint32_t memPtr = ARG(0);
    if (memPtr == 0) {
        RET(H264_ERR_PARAM);
        return;
    }
    std::lock_guard<std::mutex> lock(g_h264Mutex);
    auto it = g_sessions.find(memPtr);
    if (it != g_sessions.end()) {
#ifdef _WIN32
        if (it->second.mft) {
            it->second.mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        }
#endif
    }
    RET(H264_STATUS_OK);
}

// H264DECEnd(memory)
HLE_EXPORT(h264, H264DECEnd) {
    uint32_t memPtr = ARG(0);
    if (memPtr == 0) {
        RET(H264_ERR_PARAM);
        return;
    }
    std::lock_guard<std::mutex> lock(g_h264Mutex);
    auto it = g_sessions.find(memPtr);
    if (it != g_sessions.end()) {
#ifdef _WIN32
        it->second.releaseMft();
#endif
        it->second.lastFrameNV12.clear();
    }
    RET(H264_STATUS_OK);
}

// H264DECClose(memory)
HLE_EXPORT(h264, H264DECClose) {
    uint32_t memPtr = ARG(0);
    if (memPtr == 0) {
        RET(H264_ERR_PARAM);
        return;
    }
    std::lock_guard<std::mutex> lock(g_h264Mutex);
    auto it = g_sessions.find(memPtr);
    if (it != g_sessions.end()) {
#ifdef _WIN32
        it->second.releaseMft();
#endif
        g_sessions.erase(it);
    }
    RET(H264_STATUS_OK);
}

// H264DECCheckSkipableFrame(bitstream, length, outSkipFlag)
HLE_EXPORT(h264, H264DECCheckSkipableFrame) {
    uint32_t bsPtr      = ARG(0);
    uint32_t bsLen      = ARG(1);
    uint32_t outSkipPtr = ARG(2);
    if (outSkipPtr != 0) {
        uint32_t skippable = 0;
        if (bsPtr != 0 && bsLen > 0) {
            mem::commit(bsPtr, bsLen, false);
            auto nals = scanAnnexBNals(mem::host(bsPtr), bsLen);
            for (const auto& nal : nals) {
                if (nal.type >= 1 && nal.type <= 5) {
                    uint8_t nalRefIdc = (nal.ptr[0] >> 5) & 0x3u;
                    if (nalRefIdc == 0) skippable = 1;
                }
            }
        }
        mem::w32(outSkipPtr, skippable);
    }
    RET(H264_STATUS_OK);
}

// H264DECFindDecstartpoint(bitstream, length, outOffset)
HLE_EXPORT(h264, H264DECFindDecstartpoint) {
    uint32_t bsPtr     = ARG(0);
    uint32_t bsLen     = ARG(1);
    uint32_t outOffPtr = ARG(2);
    if (outOffPtr != 0) {
        uint32_t foundOffset = 0;
        if (bsPtr != 0 && bsLen > 0) {
            mem::commit(bsPtr, bsLen, false);
            const uint8_t* base = mem::host(bsPtr);
            auto nals = scanAnnexBNals(base, bsLen);
            for (const auto& nal : nals) {
                if (nal.type == 7 || nal.type == 5) {
                    size_t off = static_cast<size_t>(nal.ptr - base);
                    foundOffset = (off >= 4 && base[off - 4] == 0) ? static_cast<uint32_t>(off - 4) :
                                  (off >= 3 ? static_cast<uint32_t>(off - 3) : 0u);
                    break;
                }
            }
        }
        mem::w32(outOffPtr, foundOffset);
    }
    RET(H264_STATUS_OK);
}

// H264DECFindIdrpoint(bitstream, length, outOffset)
HLE_EXPORT(h264, H264DECFindIdrpoint) {
    hle_h264_H264DECFindDecstartpoint(c);
}

// H264DECCheckDecunitLength(memory, bitstream, length, offset, outLength)
HLE_EXPORT(h264, H264DECCheckDecunitLength) {
    uint32_t bsLen     = ARG(2);
    uint32_t offset    = ARG(3);
    uint32_t outLenPtr = ARG(4);
    if (outLenPtr != 0) {
        uint32_t rem = (bsLen > offset) ? (bsLen - offset) : 0u;
        mem::w32(outLenPtr, rem);
    }
    RET(H264_STATUS_OK);
}
