#include <iostream>
#include "atum/codec/video/h264/h264_decoder.hpp"
#include <algorithm>
#include <cstring>
#include "atum/hwaccel/vaapi_device.hpp"
#include "atum/hwaccel/vaapi_param_builder.hpp"

namespace atum {

H264Decoder::H264Decoder() {
    params_.codec_id = CodecId::H264;
    params_.media_type = MediaType::Video;
}

H264Decoder::~H264Decoder() {
    release_hardware_pipeline();
}

void H264Decoder::init_hardware_pipeline(uint32_t width, uint32_t height) {
    if (hw_pipeline_initialized_ || !is_hw_supported_ || !hw_device_) return;
    if (hw_device_->type() == HwDeviceType::Vaapi) {
        auto* va = static_cast<VaapiDevice*>(hw_device_.get());
        auto cfg_res = va->create_config(CodecId::H264);
        if (cfg_res.is_ok()) {
            hw_config_id_ = *cfg_res;
            if (surface_pool_.init(va, width, height, PixelFormat::NV12, 28).is_ok()) {
                auto ctx_res = va->create_context(hw_config_id_, width, height, surface_pool_.all_surfaces());
                if (ctx_res.is_ok()) {
                    hw_context_id_ = *ctx_res;
                    hw_pipeline_initialized_ = true;
                } else {
                    surface_pool_.release();
                }
            }
        }
    }
}

void H264Decoder::release_hardware_pipeline() {
    reorder_queue_.clear();
    dpb_.clear();
    if (!hw_pipeline_initialized_ || !hw_device_) return;
    if (hw_device_->type() == HwDeviceType::Vaapi) {
        auto* va = static_cast<VaapiDevice*>(hw_device_.get());
        if (hw_context_id_ != 0) {
            va->destroy_context(hw_context_id_);
            hw_context_id_ = 0;
        }
        surface_pool_.release();
        if (hw_config_id_ != 0) {
            va->destroy_config(hw_config_id_);
            hw_config_id_ = 0;
        }
    }
    hw_pipeline_initialized_ = false;
}

MediaResult<void> H264Decoder::init(const CodecParameters& params) {
    release_hardware_pipeline();
    params_ = params;
    params_.codec_id = CodecId::H264;
    params_.media_type = MediaType::Video;
    frame_count_ = 0;
    while (!ready_frames_.empty()) ready_frames_.pop();
    reorder_queue_.clear();
    dpb_.clear();

    if (!params.extradata.empty()) {
        if (params.extradata[0] == 1 && params.extradata.size() >= 7) {
            is_avcc_ = true;
            nal_length_size_ = (params.extradata[4] & 0x03) + 1;
        }
        auto res = parser_.parse_extradata(params.extradata);
        if (res) {
            if (parser_.width() > 0) params_.width = parser_.width();
            if (parser_.height() > 0) params_.height = parser_.height();
        }
    }

    hw_device_ = HwDevice::create_default();
    if (hw_device_ && hw_device_->type() == HwDeviceType::Vaapi) {
        auto* va = static_cast<VaapiDevice*>(hw_device_.get());
        is_hw_supported_ = va->supports_codec(CodecId::H264);
    }

    if (params_.width > 0 && params_.height > 0) {
        init_hardware_pipeline(params_.width, params_.height);
    }

    return {};
}

MediaResult<void> H264Decoder::send_packet(const Packet& packet) {
    if (packet.empty()) return MediaError::NeedMoreData;

    auto data = packet.as_span();
    struct ParsedSlice {
        std::span<const uint8_t> nal_span;
        uint32_t header_bit_size{0};
    };
    std::vector<ParsedSlice> slice_nals;
    bool is_ref_picture = false;
    bool has_idr_nal = false;
    bool has_b_slice = false;

    auto record_nal = [&](const NalUnit& nal, std::span<const uint8_t> full_span) {
        (void)parser_.parse_nal(nal);
        if (nal.type == 1 || nal.type == 5) {
            ParsedSlice ps{};
            ps.nal_span = full_span;
            if (const auto& sh = parser_.last_slice_header(); sh.has_value()) {
                ps.header_bit_size = sh->header_bit_size;
                if (sh->slice_type == H264SliceType::B) {
                    has_b_slice = true;
                }
            }
            slice_nals.push_back(ps);
            if (nal.type == 5) has_idr_nal = true;
            if (nal.ref_idc != 0) is_ref_picture = true;
        }
    };

    if (is_avcc_ && nal_length_size_ > 0) {
        size_t offset = 0;
        while (offset + nal_length_size_ <= data.size()) {
            uint32_t nal_len = 0;
            for (size_t i = 0; i < nal_length_size_; ++i) {
                nal_len = (nal_len << 8) | data[offset + i];
            }
            offset += nal_length_size_;
            if (offset + nal_len > data.size()) break;

            auto nal_span = data.subspan(offset, nal_len);
            if (!nal_span.empty()) {
                NalUnit nal{};
                nal.forbidden_zero_bit = (nal_span[0] >> 7) & 1;
                nal.ref_idc = (nal_span[0] >> 5) & 3;
                nal.type = nal_span[0] & 0x1F;
                nal.payload = nal_span;
                record_nal(nal, nal_span);
            }
            offset += nal_len;
        }
    } else {
        auto nals = resilient_parser_.parse_and_recover(data);
        for (const auto& nal : nals) {
            record_nal(nal, nal.payload);
        }
    }

    uint32_t w = parser_.width();
    uint32_t h = parser_.height();
    if (w == 0) w = params_.width;
    if (h == 0) h = params_.height;

    if (w > 0 && h > 0) {
        params_.width = w;
        params_.height = h;

        if (!hw_pipeline_initialized_) {
            init_hardware_pipeline(w, h);
        }

        const auto* sps = parser_.active_sps();
        if (sps) {
            dpb_.set_max_ref_frames(sps->max_num_ref_frames);
            if (sps->vui.bitstream_restriction_flag && sps->vui.max_num_reorder_frames > 0) {
                reorder_queue_.set_sps_reorder_depth(sps->vui.max_num_reorder_frames);
            }
        }
        if (has_b_slice || (packet.pts() != NOPTS && packet.dts() != NOPTS && packet.pts() > packet.dts())) {
            reorder_queue_.note_b_frame_or_dts_lag();
        }

        const bool is_idr = has_idr_nal || (slice_nals.empty() && packet.is_keyframe());
        uint32_t frame_num = parser_.last_slice_header().has_value()
            ? parser_.last_slice_header()->frame_num
            : frame_count_;

        int64_t full_poc = static_cast<int64_t>(frame_count_) * 2;
        if (sps && parser_.last_slice_header().has_value()) {
            const auto& sh = *parser_.last_slice_header();
            if (sps->pic_order_cnt_type == 0) {
                uint32_t max_poc_lsb = 1u << (sps->log2_max_pic_order_cnt_lsb_minus4 + 4);
                full_poc = reorder_queue_.unwrap_poc(sh.pic_order_cnt_lsb, max_poc_lsb, is_idr, is_ref_picture);
            } else if (sps->pic_order_cnt_type == 1) {
                full_poc = static_cast<int64_t>(sh.pic_order_cnt_lsb);
            } else {
                full_poc = static_cast<int64_t>(frame_count_) * 2;
            }
        }
        int32_t poc32 = static_cast<int32_t>(full_poc);

        VideoFrame frame;
        if (hw_pipeline_initialized_ && surface_pool_.is_valid() && hw_device_->type() == HwDeviceType::Vaapi) {
            auto* va = static_cast<VaapiDevice*>(hw_device_.get());

            auto ms = surface_pool_.acquire_lease([this](uint32_t cand) {
                for (const auto& entry : dpb_.entries()) {
                    if (entry.frame.hw_surface_id() == cand) return true;
                }
                return reorder_queue_.contains_surface(cand);
            });

            if (ms.surface_id != HW_INVALID_SURFACE_ID) {
                std::vector<uint32_t> buf_ids;
                auto* pps = parser_.active_pps();

#if defined(ATUM_HAS_VAAPI_HEADERS)
                if (sps && pps) {
                    size_t first_slice_size = slice_nals.empty() ? packet.size() : slice_nals[0].nal_span.size();
                    auto params = VaapiH264DecodeParams::build(
                        *sps,
                        *pps,
                        parser_.last_slice_header().has_value() ? &parser_.last_slice_header().value() : nullptr,
                        ms.surface_id,
                        frame_num,
                        poc32,
                        dpb_,
                        first_slice_size,
                        is_ref_picture
                    );

                    auto pic_buf = va->create_buffer(hw_context_id_, 0 /* VAPictureParameterBufferType */, sizeof(params.pic_param), 1, &params.pic_param);
                    if (pic_buf.is_ok()) buf_ids.push_back(*pic_buf);

                    auto iq_buf = va->create_buffer(hw_context_id_, 1 /* VAIQMatrixBufferType */, sizeof(params.iq_matrix), 1, &params.iq_matrix);
                    if (iq_buf.is_ok()) buf_ids.push_back(*iq_buf);

                    if (!slice_nals.empty()) {
                        for (const auto& s_info : slice_nals) {
                            params.slice_param.slice_data_size = static_cast<uint32_t>(s_info.nal_span.size());
                            params.slice_param.slice_data_offset = 0;
                            params.slice_param.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
                            params.slice_param.slice_data_bit_offset = static_cast<uint16_t>(8 + s_info.header_bit_size);

                            auto sl_buf = va->create_buffer(hw_context_id_, 4 /* VASliceParameterBufferType */, sizeof(params.slice_param), 1, &params.slice_param);
                            if (sl_buf.is_ok()) buf_ids.push_back(*sl_buf);

                            auto data_buf = va->create_buffer(hw_context_id_, 5 /* VASliceDataBufferType */, static_cast<uint32_t>(s_info.nal_span.size()), 1, s_info.nal_span.data());
                            if (data_buf.is_ok()) buf_ids.push_back(*data_buf);
                        }
                    } else if (params.has_slice) {
                        auto sl_buf = va->create_buffer(hw_context_id_, 4 /* VASliceParameterBufferType */, sizeof(params.slice_param), 1, &params.slice_param);
                        if (sl_buf.is_ok()) buf_ids.push_back(*sl_buf);

                        auto data_buf = va->create_buffer(hw_context_id_, 5 /* VASliceDataBufferType */, static_cast<uint32_t>(packet.size()), 1, packet.data());
                        if (data_buf.is_ok()) buf_ids.push_back(*data_buf);
                    }
                }
#endif
                // Hardware Command Submission (zero CPU sync stall; readback/DMA-BUF export is lazy)
                auto b_res = va->begin_picture(hw_context_id_, ms.surface_id);
                MediaResult<void> r_res = MediaResult<void>::ok();
                if (!buf_ids.empty()) {
                    r_res = va->render_picture(hw_context_id_, buf_ids.data(), static_cast<uint32_t>(buf_ids.size()));
                }
                auto e_res = va->end_picture(hw_context_id_);

                if (!b_res.is_ok() || !r_res.is_ok() || !e_res.is_ok()) {
                    std::cerr << "[VA-API HARDWARE EXECUTION FAILED] frame=" << frame_count_
                              << " begin=" << (b_res.is_ok() ? "ok" : media_error_to_string(b_res.error()))
                              << " render=" << (r_res.is_ok() ? "ok" : media_error_to_string(r_res.error()))
                              << " end=" << (e_res.is_ok() ? "ok" : media_error_to_string(e_res.error()))
                              << " bufs=" << buf_ids.size() << std::endl;
                }

                for (uint32_t b : buf_ids) {
                    va->destroy_buffer(b);
                }

                auto lazy = va->create_lazy_hw_frame(ms.surface_id, w, h, PixelFormat::NV12, ms.lease);
                if (lazy.is_ok()) {
                    frame = std::move(*lazy);
                    surface_pool_.bind_backing(ms.surface_id, frame.hw_backing());
                } else {
                    frame = VideoFrame::allocate(w, h, PixelFormat::NV12);
                    frame.set_hw_frame(true);
                    frame.set_hw_surface_id(ms.surface_id);
                }
            }
        }

        if (!frame.is_valid()) {
            frame = VideoFrame::allocate(w, h, PixelFormat::YUV420P);
        }

        frame.set_pts(packet.pts());
        frame.set_dts(packet.dts());
        frame.set_duration(packet.duration());

        bool is_key = is_idr;
        if (!is_key && parser_.last_slice_header().has_value() && parser_.last_slice_header()->is_keyframe()) {
            is_key = true;
        } else if (packet.is_keyframe()) {
            is_key = true;
        }
        frame.set_keyframe(is_key);

        if (sps && sps->vui_parameters_present_flag) {
            frame.set_color_range(sps->vui.video_full_range_flag ? ColorRange::Full : ColorRange::Studio);
            if (sps->vui.colour_description_present_flag) {
                if (sps->vui.matrix_coefficients == 1) {
                    frame.set_color_space(ColorSpace::BT709);
                } else if (sps->vui.matrix_coefficients == 6 || sps->vui.matrix_coefficients == 5) {
                    frame.set_color_space(ColorSpace::BT601);
                } else if (sps->vui.matrix_coefficients == 9) {
                    frame.set_color_space(ColorSpace::BT2020);
                }
            }
        }

        if (error_concealment_enabled_ && resilient_parser_.stats().corrupted_bytes_skipped > 0) {
            damage_mask_ = ErrorConcealmentMask(w, h, 16);
            damage_mask_.mark_damaged(0, 0);
            (void)error_concealer_.conceal(frame, damage_mask_, &dpb_);
        }

        if (is_idr) {
            dpb_.clear();
        }
        if (is_ref_picture) {
            dpb_.insert(frame.shallow_hw_ref(), poc32, frame_num, is_key);
        }

        reorder_queue_.push_and_emit(std::move(frame), full_poc, is_idr, ready_frames_);
        frame_count_++;
    }

    return {};
}

MediaResult<void> H264Decoder::receive_frame(VideoFrame& frame) {
    if (ready_frames_.empty()) return MediaError::NeedMoreData;
    frame = std::move(ready_frames_.front());
    ready_frames_.pop();
    return {};
}

MediaResult<void> H264Decoder::flush() {
    reorder_queue_.drain_to(ready_frames_);
    return {};
}

MediaResult<void> H264Decoder::reset() {
    reorder_queue_.clear();
    while (!ready_frames_.empty()) ready_frames_.pop();
    dpb_.clear();
    frame_count_ = 0;
    return {};
}

} // namespace atum
