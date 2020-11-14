// Copyright (c) the JPEG XL Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "lib/jxl/dec_modular.h"

#include <stdint.h>

#include <vector>

#include "lib/jxl/frame_header.h"

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "lib/jxl/dec_modular.cc"
#include <hwy/foreach_target.h>
// ^ must come before highway.h and any *-inl.h.

#include <hwy/highway.h>

#include "lib/jxl/alpha.h"
#include "lib/jxl/aux_out.h"
#include "lib/jxl/base/compiler_specific.h"
#include "lib/jxl/base/span.h"
#include "lib/jxl/base/status.h"
#include "lib/jxl/compressed_dc.h"
#include "lib/jxl/modular/encoding/encoding.h"
#include "lib/jxl/modular/image/image.h"
HWY_BEFORE_NAMESPACE();
namespace jxl {
namespace HWY_NAMESPACE {

void MultiplySum(const size_t xsize,
                 const pixel_type* const JXL_RESTRICT row_in,
                 const pixel_type* const JXL_RESTRICT row_in_Y,
                 const float factor, float* const JXL_RESTRICT row_out) {
  const HWY_FULL(float) df;
  const HWY_CAPPED(pixel_type, MaxLanes(df)) di;  // assumes pixel_type <= float
  const auto factor_v = Set(df, factor);
  for (size_t x = 0; x < xsize; x += Lanes(di)) {
    const auto in = Load(di, row_in + x) + Load(di, row_in_Y + x);
    const auto out = ConvertTo(df, in) * factor_v;
    Store(out, df, row_out + x);
  }
}

void RgbFromSingle(const size_t xsize,
                   const pixel_type* const JXL_RESTRICT row_in,
                   const float factor, Image3F* color, size_t /*c*/, size_t y) {
  const HWY_FULL(float) df;
  const HWY_CAPPED(pixel_type, MaxLanes(df)) di;  // assumes pixel_type <= float

  float* const JXL_RESTRICT row_out_r = color->PlaneRow(0, y);
  float* const JXL_RESTRICT row_out_g = color->PlaneRow(1, y);
  float* const JXL_RESTRICT row_out_b = color->PlaneRow(2, y);

  const auto factor_v = Set(df, factor);
  for (size_t x = 0; x < xsize; x += Lanes(di)) {
    const auto in = Load(di, row_in + x);
    const auto out = ConvertTo(df, in) * factor_v;
    Store(out, df, row_out_r + x);
    Store(out, df, row_out_g + x);
    Store(out, df, row_out_b + x);
  }
}

// Same signature as RgbFromSingle so we can assign to the same pointer.
void SingleFromSingle(const size_t xsize,
                      const pixel_type* const JXL_RESTRICT row_in,
                      const float factor, Image3F* color, size_t c, size_t y) {
  const HWY_FULL(float) df;
  const HWY_CAPPED(pixel_type, MaxLanes(df)) di;  // assumes pixel_type <= float

  float* const JXL_RESTRICT row_out = color->PlaneRow(c, y);

  const auto factor_v = Set(df, factor);
  for (size_t x = 0; x < xsize; x += Lanes(di)) {
    const auto in = Load(di, row_in + x);
    const auto out = ConvertTo(df, in) * factor_v;
    Store(out, df, row_out + x);
  }
}
// NOLINTNEXTLINE(google-readability-namespace-comments)
}  // namespace HWY_NAMESPACE
}  // namespace jxl
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace jxl {
HWY_EXPORT(MultiplySum);       // Local function
HWY_EXPORT(RgbFromSingle);     // Local function
HWY_EXPORT(SingleFromSingle);  // Local function

Status ModularFrameDecoder::DecodeGlobalInfo(BitReader* reader,
                                             const FrameHeader& frame_header,
                                             ImageBundle* decoded,
                                             bool decode_color, size_t xsize,
                                             size_t ysize) {
  bool has_tree = reader->ReadBits(1);
  if (has_tree) {
    std::vector<uint8_t> tree_context_map;
    ANSCode tree_code;
    JXL_RETURN_IF_ERROR(DecodeHistograms(reader, kNumTreeContexts, &tree_code,
                                         &tree_context_map));
    ANSSymbolReader ans_reader(&tree_code, reader);
    JXL_RETURN_IF_ERROR(
        DecodeTree(reader, &ans_reader, tree_context_map, &tree));
    if (!ans_reader.CheckANSFinalState()) {
      return JXL_FAILURE("ANS decode final state failed");
    }
    JXL_RETURN_IF_ERROR(
        DecodeHistograms(reader, (tree.size() + 1) / 2, &code, &context_map));
  }
  size_t nb_chans = 3, nb_extra = 0;
  if (decoded->IsGray() &&
      frame_header.color_transform == ColorTransform::kNone) {
    nb_chans = 1;
  }
  do_color = decode_color;
  if (!do_color) nb_chans = 0;
  if (decoded->HasExtraChannels()) {
    nb_extra = decoded->extra_channels().size();
  }

  bool fp = decoded->metadata()->bit_depth.floating_point_sample;

  // bits_per_sample is just metadata for XYB images.
  if (decoded->metadata()->bit_depth.bits_per_sample >= 32 && do_color &&
      frame_header.color_transform != ColorTransform::kXYB) {
    if (decoded->metadata()->bit_depth.bits_per_sample == 32 && fp == false) {
      // TODO(lode): does modular support uint32_t? maxval is signed int so
      // cannot represent 32 bits.
      return JXL_FAILURE("uint32_t not supported in dec_modular");
    } else if (decoded->metadata()->bit_depth.bits_per_sample > 32) {
      return JXL_FAILURE("bits_per_sample > 32 not supported");
    }
  }
  // TODO(lode): must handle decoded->metadata()->floating_point_channel?
  int maxval = (fp ? 1
                   : (1u << static_cast<uint32_t>(
                          decoded->metadata()->bit_depth.bits_per_sample)) -
                         1);

  Image gi(xsize, ysize, maxval, nb_chans + nb_extra);

  for (size_t ec = 0, c = nb_chans; ec < nb_extra; ec++, c++) {
    const ExtraChannelInfo& eci = decoded->metadata()->extra_channel_info[ec];
    gi.channel[c].resize(eci.Size(decoded->xsize()),
                         eci.Size(decoded->ysize()));
    gi.channel[c].hshift = gi.channel[c].vshift = eci.dim_shift;
  }

  ModularOptions options;
  options.max_chan_size = frame_dim.group_dim;
  if (!ModularGenericDecompress(
          reader, gi, &global_header, ModularStreamId::Global().ID(frame_dim),
          &options,
          /*undo_transforms=*/-2, &tree, &code, &context_map)) {
    return JXL_FAILURE("Failed to decode global modular info");
  }

  // ensure all the channel buffers are allocated
  have_something = false;
  for (size_t c = 0; c < gi.channel.size(); c++) {
    Channel& gic = gi.channel[c];
    if (c >= gi.nb_meta_channels && gic.w < frame_dim.group_dim &&
        gic.h < frame_dim.group_dim)
      have_something = true;
    gic.resize();
  }
  full_image = std::move(gi);
  return true;
}

Status ModularFrameDecoder::DecodeGroup(const Rect& rect, BitReader* reader,
                                        AuxOut* aux_out, size_t minShift,
                                        size_t maxShift,
                                        const ModularStreamId& stream) {
  JXL_DASSERT(stream.kind == ModularStreamId::kModularDC ||
              stream.kind == ModularStreamId::kModularAC);
  const size_t xsize = rect.xsize();
  const size_t ysize = rect.ysize();
  int maxval = full_image.maxval;
  Image gi(xsize, ysize, maxval, 0);
  // start at the first bigger-than-groupsize non-metachannel
  size_t c = full_image.nb_meta_channels;
  for (; c < full_image.channel.size(); c++) {
    Channel& fc = full_image.channel[c];
    if (fc.w > frame_dim.group_dim || fc.h > frame_dim.group_dim) break;
  }
  size_t beginc = c;
  for (; c < full_image.channel.size(); c++) {
    Channel& fc = full_image.channel[c];
    size_t shift = std::min(fc.hshift, fc.vshift);
    if (shift > maxShift) continue;
    if (shift < minShift) continue;
    Rect r(rect.x0() >> fc.hshift, rect.y0() >> fc.vshift,
           rect.xsize() >> fc.hshift, rect.ysize() >> fc.vshift, fc.w, fc.h);
    if (r.xsize() == 0 || r.ysize() == 0) continue;
    Channel gc(r.xsize(), r.ysize());
    gc.hshift = fc.hshift;
    gc.vshift = fc.vshift;
    gi.channel.emplace_back(std::move(gc));
  }
  gi.nb_channels = gi.channel.size();
  gi.real_nb_channels = gi.nb_channels;
  ModularOptions options;
  if (!ModularGenericDecompress(
          reader, gi, /*header=*/nullptr, stream.ID(frame_dim), &options,
          /*undo_transforms=*/-1, &tree, &code, &context_map))
    return JXL_FAILURE("Failed to decode modular group");
  int gic = 0;
  for (c = beginc; c < full_image.channel.size(); c++) {
    Channel& fc = full_image.channel[c];
    size_t shift = std::min(fc.hshift, fc.vshift);
    if (shift > maxShift) continue;
    if (shift < minShift) continue;
    Rect r(rect.x0() >> fc.hshift, rect.y0() >> fc.vshift,
           rect.xsize() >> fc.hshift, rect.ysize() >> fc.vshift, fc.w, fc.h);
    if (r.xsize() == 0 || r.ysize() == 0) continue;
    for (size_t y = 0; y < r.ysize(); ++y) {
      pixel_type* const JXL_RESTRICT row_out = r.Row(&fc.plane, y);
      const pixel_type* const JXL_RESTRICT row_in = gi.channel[gic].Row(y);
      for (size_t x = 0; x < r.xsize(); ++x) {
        row_out[x] = row_in[x];
      }
    }
    gic++;
  }
  return true;
}
Status ModularFrameDecoder::DecodeVarDCTDC(size_t group_id, BitReader* reader,
                                           PassesDecoderState* dec_state,
                                           AuxOut* aux_out) {
  const Rect r = dec_state->shared->DCGroupRect(group_id);
  Image image(r.xsize(), r.ysize(), 255, 3);
  size_t stream_id = ModularStreamId::VarDCTDC(group_id).ID(frame_dim);
  reader->Refill();
  size_t extra_precision = reader->ReadFixedBits<2>();
  float mul = 1.0f / (1 << extra_precision);
  ModularOptions options;
  for (size_t c = 0; c < 3; c++) {
    Channel& ch = image.channel[c < 2 ? c ^ 1 : c];
    ch.w >>= dec_state->shared->frame_header.chroma_subsampling.HShift(c);
    ch.h >>= dec_state->shared->frame_header.chroma_subsampling.VShift(c);
    ch.resize();
  }
  if (!ModularGenericDecompress(
          reader, image, /*header=*/nullptr, stream_id, &options,
          /*undo_transforms=*/-1, &tree, &code, &context_map)) {
    return JXL_FAILURE("Failed to decode modular DC group");
  }
  DequantDC(r, &dec_state->shared_storage.dc_storage,
            &dec_state->shared_storage.quant_dc, image,
            dec_state->shared->quantizer.MulDC(), mul,
            dec_state->shared->cmap.DCFactors(),
            dec_state->shared->frame_header.chroma_subsampling,
            dec_state->shared->block_ctx_map);
  return true;
}

Status ModularFrameDecoder::DecodeAcMetadata(size_t group_id, BitReader* reader,
                                             PassesDecoderState* dec_state,
                                             AuxOut* aux_out) {
  const Rect r = dec_state->shared->DCGroupRect(group_id);
  size_t upper_bound = r.xsize() * r.ysize();
  reader->Refill();
  size_t width = reader->ReadBits(CeilLog2Nonzero(upper_bound)) + 1;
  size_t stream_id = ModularStreamId::ACMetadata(group_id).ID(frame_dim);
  // YToX, YToB, ACS + QF, EPF
  Image image(r.xsize(), r.ysize(), 255, 4);
  static_assert(kColorTileDimInBlocks == 8, "Color tile size changed");
  Rect cr(r.x0() >> 3, r.y0() >> 3, (r.xsize() + 7) >> 3, (r.ysize() + 7) >> 3);
  image.channel[0] = Channel(cr.xsize(), cr.ysize(), 3, 3);
  image.channel[1] = Channel(cr.xsize(), cr.ysize(), 3, 3);
  image.channel[2] = Channel(width, 2, 0, 0);
  ModularOptions options;
  if (!ModularGenericDecompress(
          reader, image, /*header=*/nullptr, stream_id, &options,
          /*undo_transforms=*/-1, &tree, &code, &context_map)) {
    return JXL_FAILURE("Failed to decode AC metadata");
  }
  ConvertPlaneAndClamp(Rect(image.channel[0].plane), image.channel[0].plane, cr,
                       &dec_state->shared_storage.cmap.ytox_map);
  ConvertPlaneAndClamp(Rect(image.channel[1].plane), image.channel[1].plane, cr,
                       &dec_state->shared_storage.cmap.ytob_map);
  size_t num = 0;
  bool is444 = dec_state->shared->frame_header.chroma_subsampling.Is444();
  for (size_t y = 0; y < r.ysize(); y++) {
    int* row_qf = r.Row(&dec_state->shared_storage.raw_quant_field, y);
    uint8_t* row_epf = r.Row(&dec_state->shared_storage.epf_sharpness, y);
    int* row_in_1 = image.channel[2].plane.Row(0);
    int* row_in_2 = image.channel[2].plane.Row(1);
    int* row_in_3 = image.channel[3].plane.Row(y);
    for (size_t x = 0; x < r.xsize(); x++) {
      int sharpness = row_in_3[x];
      if (sharpness < 0 || sharpness >= LoopFilter::kEpfSharpEntries) {
        return JXL_FAILURE("Corrupted sharpness field");
      }
      row_epf[x] = sharpness;
      if (dec_state->shared_storage.ac_strategy.IsValid(r.x0() + x,
                                                        r.y0() + y)) {
        continue;
      }

      if (num >= width) return JXL_FAILURE("Corrupted stream");

      if (!AcStrategy::IsRawStrategyValid(row_in_1[num])) {
        return JXL_FAILURE("Invalid AC strategy");
      }
      AcStrategy acs = AcStrategy::FromRawStrategy(row_in_1[num]);
      if ((acs.covered_blocks_x() > 1 || acs.covered_blocks_y() > 1) &&
          !is444) {
        return JXL_FAILURE(
            "AC strategy not compatible with chroma subsampling");
      }
      if (x + acs.covered_blocks_x() > r.xsize()) {
        return JXL_FAILURE("Invalid AC strategy, x overflow");
      }
      if (y + acs.covered_blocks_y() > r.ysize()) {
        return JXL_FAILURE("Invalid AC strategy, y overflow");
      }
      JXL_RETURN_IF_ERROR(
          dec_state->shared_storage.ac_strategy.SetNoBoundsCheck(
              r.x0() + x, r.y0() + y, AcStrategy::Type(row_in_1[num])));
      row_qf[x] = 1 + std::max(0, row_in_2[num]);
      num++;
    }
  }
  return true;
}

Status ModularFrameDecoder::FinalizeDecoding(Image3F* color,
                                             ImageBundle* decoded,
                                             jxl::ThreadPool* pool,
                                             const float* xyb_muls,
                                             const FrameHeader& frame_header) {
  Image& gi = full_image;
  size_t xsize = gi.w;
  size_t ysize = gi.h;

  // Don't use threads if total image size is smaller than a group
  if (xsize * ysize < frame_dim.group_dim * frame_dim.group_dim) pool = nullptr;

  // Undo the global transforms
  gi.undo_transforms(global_header.wp_header, -1, pool);

  int c = 0;
  if (do_color) {
    const bool rgb_from_gray =
        decoded->IsGray() &&
        frame_header.color_transform == ColorTransform::kNone;
    const bool fp = decoded->metadata()->bit_depth.floating_point_sample;

    for (; c < 3; c++) {
      float factor = 255.f / (float)full_image.maxval;
      int c_in = c;
      if (frame_header.color_transform == ColorTransform::kXYB) {
        factor = xyb_muls[c];
        // XYB is encoded as YX(B-Y)
        if (c < 2) c_in = 1 - c;
      } else if (rgb_from_gray) {
        c_in = 0;
      }
      if (frame_header.color_transform == ColorTransform::kXYB && c == 2) {
        RunOnPool(
            pool, 0, ysize, jxl::ThreadPool::SkipInit(),
            [&](const int task, const int thread) {
              const size_t y = task;
              const pixel_type* const JXL_RESTRICT row_in =
                  gi.channel[c_in].Row(y);
              const pixel_type* const JXL_RESTRICT row_in_Y =
                  gi.channel[0].Row(y);
              float* const JXL_RESTRICT row_out = color->PlaneRow(c, y);
              HWY_DYNAMIC_DISPATCH(MultiplySum)
              (xsize, row_in, row_in_Y, factor, row_out);
            },
            "ModularIntToFloat");
      } else if (fp && decoded->metadata()->bit_depth.bits_per_sample < 32) {
        int exp_bits = decoded->metadata()->bit_depth.exponent_bits_per_sample;
        int exp_bias = (1 << (exp_bits - 1)) - 1;
        int sign_shift = (decoded->metadata()->bit_depth.bits_per_sample - 1);
        int mant_bits =
            decoded->metadata()->bit_depth.bits_per_sample - exp_bits - 1;
        int mant_shift = 23 - mant_bits;
        for (size_t y = 0; y < ysize; ++y) {
          float* const JXL_RESTRICT row_out = color->PlaneRow(c, y);
          const pixel_type* const JXL_RESTRICT row_in = gi.channel[c_in].Row(y);
          for (size_t x = 0; x < xsize; ++x) {
            uint32_t f;
            memcpy(&f, &row_in[x], 4);
            int signbit = (f >> sign_shift);
            f &= (1 << sign_shift) - 1;
            int exp = (f >> mant_bits) - exp_bias;
            int mantissa = (f & ((1 << mant_bits) - 1));
            // broke up the arbitrary float into its parts, now reassemble into
            // binary32
            exp += 127;
            mantissa <<= mant_shift;
            f = (signbit ? 0x80000000 : 0);
            f |= (exp << 23);
            f |= mantissa;
            memcpy(&row_out[x], &f, 4);
          }
        }
      } else if (fp) {
        JXL_ASSERT(sizeof(pixel_type) == sizeof(float));
        JXL_ASSERT(SameSize(color->Plane(c), gi.channel[c_in].plane));
        // Convert Plane<int> to Plane<float> using memcpy() for lossless mode.
        for (size_t y = 0; y < color->ysize(); y++) {
          memcpy(color->PlaneRow(c, y), gi.channel[c_in].plane.ConstRow(y),
                 color->xsize() * sizeof(float));
        }
      } else {
        RunOnPool(
            pool, 0, ysize, jxl::ThreadPool::SkipInit(),
            [&](const int task, const int thread) {
              const size_t y = task;
              const pixel_type* const JXL_RESTRICT row_in =
                  gi.channel[c_in].Row(y);
              if (rgb_from_gray) {
                HWY_DYNAMIC_DISPATCH(RgbFromSingle)
                (xsize, row_in, factor, color, c, y);
              } else {
                HWY_DYNAMIC_DISPATCH(SingleFromSingle)
                (xsize, row_in, factor, color, c, y);
              }
            },
            "ModularIntToFloat");
      }
      if (rgb_from_gray) {
        break;
      }
    }
    if (rgb_from_gray) {
      c = 1;
    }
  }
  if (decoded->HasExtraChannels()) {
    for (size_t ec = 0; ec < decoded->extra_channels().size(); ec++, c++) {
      const jxl::ExtraChannelInfo& eci =
          decoded->metadata()->extra_channel_info[ec];
      const pixel_type max_extra = (1u << eci.bit_depth.bits_per_sample) - 1;
      const size_t ec_xsize = eci.Size(xsize);  // includes shift
      const size_t ec_ysize = eci.Size(ysize);
      for (size_t y = 0; y < ec_ysize; ++y) {
        uint16_t* const JXL_RESTRICT row_out =
            decoded->extra_channels()[ec].Row(y);
        const pixel_type* const JXL_RESTRICT row_in = gi.channel[c].Row(y);
        for (size_t x = 0; x < ec_xsize; ++x) {
          row_out[x] = Clamp1(row_in[x], 0, max_extra);
        }
      }
    }
  }
  return true;
}

Status ModularFrameDecoder::DecodeQuantTable(
    size_t required_size_x, size_t required_size_y, BitReader* br,
    QuantEncoding* encoding, size_t idx,
    ModularFrameDecoder* modular_frame_decoder) {
  encoding->qraw.qtable_den_shift = br->ReadFixedBits<3>();
  Image image(required_size_x, required_size_y, 255, 3);
  ModularOptions options;
  if (modular_frame_decoder) {
    JXL_RETURN_IF_ERROR(ModularGenericDecompress(
        br, image, /*header=*/nullptr,
        ModularStreamId::QuantTable(idx).ID(modular_frame_decoder->frame_dim),
        &options, /*undo_transforms=*/-1, &modular_frame_decoder->tree,
        &modular_frame_decoder->code, &modular_frame_decoder->context_map));
  } else {
    JXL_RETURN_IF_ERROR(ModularGenericDecompress(br, image, /*header=*/nullptr,
                                                 0, &options,
                                                 /*undo_transforms=*/-1));
  }
  if (!encoding->qraw.qtable) {
    encoding->qraw.qtable = new std::vector<int>();
  }
  encoding->qraw.qtable->resize(required_size_x * required_size_y * 3);
  for (size_t c = 0; c < 3; c++) {
    for (size_t y = 0; y < required_size_y; y++) {
      int* JXL_RESTRICT row = image.channel[c].Row(y);
      for (size_t x = 0; x < required_size_x; x++) {
        (*encoding->qraw.qtable)[c * required_size_x * required_size_y +
                                 y * required_size_x + x] = row[x];
        if (row[x] <= 0) {
          return JXL_FAILURE("Invalid raw quantization table");
        }
      }
    }
  }
  return true;
}

}  // namespace jxl
#endif  // HWY_ONCE