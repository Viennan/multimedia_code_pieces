#include <string>
#include <iostream>
#include <vector>
#include <memory>
#include <functional>
#include <mutex>
#include <map>
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

inline std::string av_err2stdstring(int errnum) {
    std::string buf(AV_ERROR_MAX_STRING_SIZE, '\0');
    av_make_error_string(buf.data(), AV_ERROR_MAX_STRING_SIZE, errnum);
    return buf;
};

static int open_codec(AVCodecContext** p_ctx, AVCodecParameters* params = nullptr, const AVCodec* codec = nullptr, AVDictionary** options = nullptr) {
    int ret;
    if (*p_ctx == nullptr) {
        *p_ctx = avcodec_alloc_context3(codec);
    }
    if (!*p_ctx) {
        return AVERROR(ENOMEM);
    }
    if (params != nullptr) {
        if (codec != nullptr && codec->id != params->codec_id) {
            std::cerr << "codec id mismatch between codec params and codec" << std::endl;
            return AVERROR_UNKNOWN;
        }
        if ((ret = avcodec_parameters_to_context(*p_ctx, params)) < 0) {
            std::cerr << "failed copy codec params: " << av_err2stdstring(ret) << std::endl;
            return ret;
        }
    }
    if ((ret = avcodec_open2(*p_ctx, codec, options)) < 0) {
        std::cerr << "failed open codec: " << av_err2stdstring(ret) << std::endl;
        return ret;
    }
    return 0;
};

class Encoder {
public:

    Encoder(): m_ctx(nullptr), m_pkt(nullptr), m_last_pkt(nullptr), m_packet_callback(), m_enable_auto_pts_setting(true) {}

    void release() {
        avcodec_free_context(&m_ctx);
        av_packet_free(&m_pkt);
    }

    // Encoder will take over the lifecycle of the input AVCodecContext
    int init(AVCodecContext* ctx, AVCodecParameters* params = nullptr, const AVCodec* codec = nullptr, AVDictionary** options = nullptr) {
        release();
        int ret;
        m_ctx = ctx;
        if (params != nullptr && codec == nullptr) {
            codec = avcodec_find_encoder(params->codec_id);
        }
        if ((ret = open_codec(&m_ctx, params, codec, options)) < 0) {
            return ret;
        }
        m_pkt = av_packet_alloc();
        m_last_pkt = av_packet_alloc();
        m_max_pts = std::numeric_limits<int64_t>::min();
        m_pts_map.clear();
        m_start_pts = AV_NOPTS_VALUE;
        m_end_pts = AV_NOPTS_VALUE;
        m_frame_cnt = 0;
        return 0;
    }

    void set_packet_callback(std::function<int(AVPacket*)> cb) {
        m_packet_callback = cb;
    }

    void set_auto_duration_setting(bool enable) {
        m_enable_auto_pts_setting = enable;
    }

    // frame should in the same timebase with encoder context
    int encode(AVFrame* frame) {
        if (frame != nullptr) {
            if (frame->pts <= m_max_pts) {
                // equal pts make no sense
                return 0;
            }
            m_max_pts = frame->pts;
            update_statistic_info(frame);
        }

        int ret = avcodec_send_frame(m_ctx, frame);
        if (ret < 0) {
            std::cerr << "failed send frame to encoder: " << av_err2stdstring(ret) << std::endl;
            return ret;
        }
        while(ret >= 0) {
            ret = avcodec_receive_packet(m_ctx, m_pkt);
            if (ret == AVERROR(EAGAIN)) 
                return 0;
            else if (ret == AVERROR_EOF) {
                if ((ret = flush_packets()) != 0) {
                    return ret;
                }
                return AVERROR_EOF;
            } else if (ret < 0) {
                std::cerr << "failed receive packet from encoder: " << av_err2stdstring(ret) << std::endl;
                return ret;
            }
            if ((ret = emit_last_packet()) != 0) {
                return ret;
            }
        }
        return 0;
    }

    AVCodecContext* context() const {
        return m_ctx;
    }

private:

    void update_statistic_info(AVFrame* frame) {
        ++m_frame_cnt;
        if (m_start_pts == AV_NOPTS_VALUE) {
            m_start_pts = frame->pts;
        }
        m_end_pts = frame->pts + frame->pkt_duration;
        if (m_enable_auto_pts_setting) {
            auto max_pts_iter = m_pts_map.rbegin();
            if (max_pts_iter != m_pts_map.rend() && max_pts_iter->second == 0) {
                max_pts_iter->second = frame->pts - max_pts_iter->first;
            }
            m_pts_map[frame->pts] = frame->pkt_duration;
        }
    }

    void disable_auto_duration_setting() {
        m_enable_auto_pts_setting = false;
        m_pts_map.clear();
    }

    int64_t get_avg_duration() const {
        return (m_end_pts - m_start_pts) / m_frame_cnt;
    }

    int64_t get_best_effort_duration(int64_t pts) {
        if (!m_enable_auto_pts_setting) {
            return get_avg_duration();
        }
        auto iter = m_pts_map.lower_bound(pts);
        if (iter == m_pts_map.end() || iter->first != pts || iter->second == 0) {
            return get_avg_duration();
        }
        auto duration = iter->second;
        m_pts_map.erase(iter);
        return duration;
    }

    int emit_packet(AVPacket* pkt) {
        if (pkt == nullptr) {
            return m_packet_callback(nullptr);
        }
        if (pkt->duration <= 0 && m_enable_auto_pts_setting) {
            pkt->duration = get_best_effort_duration(pkt->pts);
        }
        return m_packet_callback(pkt);
    }

    int emit_last_packet() {
        int ret;
        if (m_last_pkt->pts != AV_NOPTS_VALUE) {
            if ((ret = emit_packet(m_last_pkt)) != 0) {
                return ret;
            }
        }
        if ((ret = av_packet_ref(m_last_pkt, m_pkt)) < 0) {
            std::cerr << "encoder failed transfer data to cache : " << ret << std::endl;
            return ret;
        }
        av_packet_unref(m_pkt);
        return 0;
    }

    int flush_packets() {
        int ret;
        if (m_last_pkt->pts != AV_NOPTS_VALUE) {
            if ((ret = emit_packet(m_last_pkt)) < 0) {
                return ret;
            }
        }
        if (m_pkt->pts != AV_NOPTS_VALUE) {
            if ((ret = emit_packet(m_pkt)) < 0) {
                return ret;
            }
        }
        return emit_packet(nullptr);
    }

    AVCodecContext* m_ctx;
    AVPacket* m_pkt;
    AVPacket* m_last_pkt;
    std::function<int(AVPacket*)> m_packet_callback;
    int64_t m_max_pts;
    bool m_enable_auto_pts_setting;
    std::map<int64_t, int64_t> m_pts_map;
    int64_t m_start_pts;
    int64_t m_end_pts;
    int64_t m_frame_cnt;
};

class Decoder {
public:

    Decoder() : m_ctx(nullptr), m_frame(nullptr), m_frame_callback() {}

    void release() {
        avcodec_free_context(&m_ctx);
        av_frame_free(&m_frame);
    }

    // decoder will take over the lifecycle of the input AVCodecContext
    int init(AVCodecContext* ctx, AVCodecParameters* params = nullptr, const AVCodec* codec = nullptr, AVDictionary** options = nullptr) {
        release();
        m_ctx = ctx;
        if (params != nullptr && codec == nullptr) {
            codec = avcodec_find_decoder(params->codec_id);
        }
        int ret;
        if ((ret = open_codec(&m_ctx, params, codec, options)) < 0) {
            return ret;
        }
        m_frame = av_frame_alloc();
        return 0;
    }

    void set_frame_callback(std::function<int(AVFrame*)> cb) {
        m_frame_callback = cb;
    }

    int decode(AVPacket* pkt) {
        int ret = avcodec_send_packet(m_ctx, pkt);
        if (ret < 0) {
            std::cerr << "failed send packet to decoder: " << av_err2stdstring(ret) << std::endl;
            return ret;
        }
        while (ret >= 0) {
            ret = avcodec_receive_frame(m_ctx, m_frame);
            if (ret == AVERROR(EAGAIN))
                return 0;
            else if (ret == AVERROR_EOF) {
                if ((ret = m_frame_callback(nullptr)) != 0) {
                    return ret;
                }
                return AVERROR_EOF;
            } else if (ret < 0) {
                std::cerr << "failed receive frame from decoder: " << av_err2stdstring(ret) << std::endl;
                return ret;
            }
            if ((ret = m_frame_callback(m_frame)) != 0) {
                return ret;
            }
        }
        return 0;
    }

    AVCodecContext* context() const {
        return m_ctx;
    }

private:
    AVCodecContext* m_ctx;
    AVFrame* m_frame;
    std::function<int(AVFrame*)> m_frame_callback;
};

class Demuxer {
public:
    Demuxer() : m_fmt_ctx(nullptr), m_stream_cbs() {}

    void release() {
        avformat_close_input(&m_fmt_ctx);
        av_packet_free(&m_pkt);
    }

    int init(const std::string& path, AVInputFormat* ifmt = nullptr, AVDictionary** options = nullptr) {
        release();
        int ret;
        if ((ret = avformat_open_input(&m_fmt_ctx, path.c_str(), ifmt, options)) < 0) {
            std::cerr << "failed open input file: " << av_err2stdstring(ret) << std::endl;
            return ret;
        }
        if ((ret = avformat_find_stream_info(m_fmt_ctx, nullptr)) < 0) {
            std::cerr << "failed find stream info: " << av_err2stdstring(ret) << std::endl;
            return ret;
        }
        m_stream_cbs = std::vector<std::function<int(AVPacket*)>>(m_fmt_ctx->nb_streams, nullptr);
        m_pkt = av_packet_alloc();
        return 0;
    }

    void set_stream_packet_callback(int stream_idx, std::function<int(AVPacket*)> cb) {
        m_stream_cbs[stream_idx] = cb;
    }

    int init_decoder(int stream_idx, Decoder& decoder, const AVCodec* codec = nullptr, AVDictionary** options = nullptr) {
        int ret;
        if ((ret = decoder.init(nullptr, m_fmt_ctx->streams[stream_idx]->codecpar, codec, options)) < 0) {
            std::cerr << "failed init decoder: " << ret << std::endl;
            return ret;
        }
        return 0;
    }

    int demux() {
        av_packet_unref(m_pkt);
        int ret = av_read_frame(m_fmt_ctx, m_pkt);
        if (ret == AVERROR_EOF) {
            for (auto& cb : m_stream_cbs) {
                if (cb != nullptr) {
                    if ((ret = cb(nullptr)) != 0) {
                        return ret;
                    }
                }
            }
            return AVERROR_EOF;
        } else if (ret < 0) {
            std::cerr << "failed read frame: " << av_err2stdstring(ret) << std::endl;
            return ret;
        }
        if (m_stream_cbs[m_pkt->stream_index] != nullptr) {
            if ((ret = m_stream_cbs[m_pkt->stream_index](m_pkt)) != 0) {
                return ret;
            }
        }
        return 0;
    }

    AVFormatContext* context() const {
        return m_fmt_ctx;
    }

private:
    AVFormatContext* m_fmt_ctx;
    AVPacket* m_pkt;
    std::vector<std::function<int(AVPacket*)>> m_stream_cbs;
};

class Muxer {
public:

    Muxer() : m_fmt_ctx(nullptr) {}

    void release() {
        if (m_fmt_ctx != nullptr && !(m_fmt_ctx->flags & AVFMT_NOFILE)) {
            avio_closep(&m_fmt_ctx->pb);
        }
        avformat_free_context(m_fmt_ctx);
    }

    int init(const std::string& path, AVOutputFormat* ofmt = nullptr, const char* format_name = nullptr,  AVDictionary** options = nullptr) {
        release();
        int ret;
        if ((ret = avformat_alloc_output_context2(&m_fmt_ctx, ofmt, format_name, path.c_str())) < 0) {
            std::cerr << "failed alloc output context: " << av_err2stdstring(ret) << std::endl;
            return ret;
        }
        if (!(m_fmt_ctx->flags & AVFMT_NOFILE)) {
            if ((ret = avio_open2(&m_fmt_ctx->pb, path.c_str(), AVIO_FLAG_WRITE, nullptr, options)) < 0) {
                std::cerr << "failed open output file: " << av_err2stdstring(ret) << std::endl;
                return ret;
            }
        }
        return 0;
    }

    int add_stream(const Encoder& encoder, std::function<void(AVFormatContext*, AVStream*)> initializer = nullptr) {
        auto encode_ctx = encoder.context();
        AVStream* stream = avformat_new_stream(m_fmt_ctx, encode_ctx->codec);
        if (!stream) {
            std::cerr << "failed alloc stream" << std::endl;
            return AVERROR(ENOMEM);
        }
        stream->id = m_fmt_ctx->nb_streams - 1;
        stream->time_base = encode_ctx->time_base;
        int ret;
        if ((ret = avcodec_parameters_from_context(stream->codecpar, encode_ctx)) < 0 ) {
            std::cerr << "failed copy codec params: " << av_err2stdstring(ret) << std::endl;
            return ret;
        };
        if (initializer != nullptr) {
            initializer(m_fmt_ctx, stream);
        }
        return stream->id;
    }

    int add_stream(const AVCodecParameters* params, const AVCodec* codec = nullptr, std::function<void(AVFormatContext*, AVStream*)> initializer = nullptr) {
        if (codec == nullptr && params != nullptr && params->codec_id != AV_CODEC_ID_NONE) {
            codec = avcodec_find_encoder(params->codec_id);
        }
        AVStream* stream = avformat_new_stream(m_fmt_ctx, codec);
        if (!stream) {
            std::cerr << "failed alloc stream" << std::endl;
            return AVERROR(ENOMEM);
        }
        stream->id = m_fmt_ctx->nb_streams - 1;
        int ret;
        if (params != nullptr) {
            if ((ret = avcodec_parameters_copy(stream->codecpar, params)) < 0 ) {
                std::cerr << "failed copy codec params: " << av_err2stdstring(ret) << std::endl;
                return ret;
            }
        };
        if (initializer != nullptr) {
            initializer(m_fmt_ctx, stream);
        }
        return stream->id;
    }

    std::function<int(AVPacket*)> create_stream_writer(int stream_idx, bool thread_safe = false) {
        m_stream_writers.push_back(std::make_shared<StreamWriter>());
        auto& stream_writer = m_stream_writers.back();
        stream_writer->m_stream_idx = stream_idx;
        stream_writer->m_fmt_ctx = m_fmt_ctx;
        stream_writer->m_mutex = &m_mutex;
        return thread_safe ? stream_writer->create_thread_safe_writer() : stream_writer->create_writer();
    }

    int write_header(AVDictionary** options = nullptr) {
        int ret;
        if ((ret = avformat_write_header(m_fmt_ctx, options)) < 0) {
            std::cerr << "failed write header: " << av_err2stdstring(ret) << std::endl;
            return ret;
        }
        return 0;
    }

    int write_trailer() {
        int ret;
        if ((ret = av_interleaved_write_frame(m_fmt_ctx, nullptr)) != 0) {
            std::cerr << "failed flush mux frame queue : " << av_err2stdstring(ret) << std::endl;
            return ret;
        }
        if ((ret = av_write_trailer(m_fmt_ctx)) < 0) {
            std::cerr << "failed write trailer: " << av_err2stdstring(ret) << std::endl;
            return ret;
        }
        return 0;
    }

    AVFormatContext* context() const {
        return m_fmt_ctx;
    }

private:
    AVFormatContext* m_fmt_ctx;
    std::mutex m_mutex;

    struct StreamWriter {
        int m_stream_idx;
        AVFormatContext* m_fmt_ctx;
        std::mutex* m_mutex;

        StreamWriter(): m_stream_idx(-1), m_fmt_ctx(nullptr), m_mutex(nullptr) {}

        std::function<int(AVPacket*)> create_thread_safe_writer () {
            return [this](AVPacket* pkt)->int{
                std::lock_guard<std::mutex> lock(*m_mutex);
                if (pkt != nullptr) {
                    pkt->stream_index = m_stream_idx;
                }
                int ret;
                if ((ret = av_interleaved_write_frame(m_fmt_ctx, pkt)) < 0) {
                    std::cerr << "failed write packet for stream " << m_stream_idx <<  " : " << av_err2stdstring(ret) << std::endl;
                    return ret;
                }
                return 0;
            };
        }

        std::function<int(AVPacket*)> create_writer() {
            return [this](AVPacket* pkt)->int{
                if (pkt != nullptr) {
                    pkt->stream_index = m_stream_idx;
                }
                int ret;
                if ((ret = av_interleaved_write_frame(m_fmt_ctx, pkt)) < 0) {
                    std::cerr << "failed write packet for stream " << m_stream_idx <<  " : " << av_err2stdstring(ret) << std::endl;
                    return ret;
                }
                return 0;
            };
        }

    };
    std::vector<std::shared_ptr<StreamWriter>> m_stream_writers;
};

// transcode video stream from h.264 to h.265 and just copy audio stream
class ToyTranscoder {
public:
    ToyTranscoder() : m_demuxer(), m_muxer(), m_decoders(), m_encoders() {}

    ~ToyTranscoder() {
        m_muxer.release();
        for (auto& encoder : m_encoders) {
            encoder.release();
        }
        for (auto& decoder : m_decoders) {
            decoder.release();
        }
        m_demuxer.release();
    }

    int init(const std::string& input_file, const std::string& output_file) {
        int ret;
        if ((ret = m_demuxer.init(input_file, nullptr, nullptr)) < 0) {
            return ret;
        }
        if ((ret = m_muxer.init(output_file, nullptr, nullptr, nullptr)) < 0) {
            return ret;
        }

        // init video transcode pipeline
        auto ifmt = m_demuxer.context();
        auto vst_id = av_find_best_stream(ifmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vst_id < 0) {
            std::cerr << "failed find video stream" << std::endl;
            return AVERROR_STREAM_NOT_FOUND;
        }
        auto vst = ifmt->streams[vst_id];
        if (vst->codecpar->codec_id != AV_CODEC_ID_H264) {
            std::cerr << "video stream is not encoded in h.264" << std::endl;
            return AVERROR_DECODER_NOT_FOUND;
        }
        m_decoders.emplace_back();
        auto& v_decoder = m_decoders.back();
        if ((ret = m_demuxer.init_decoder(vst_id, v_decoder, nullptr)) < 0) {
            return ret;
        }
        auto x265 = avcodec_find_encoder_by_name("libx265");
        if (x265 == nullptr) {
            std::cerr << "failed find encoder libx265" << std::endl;
            return AVERROR_ENCODER_NOT_FOUND;
        }
        auto v_enc_ctx = avcodec_alloc_context3(x265);
        // copy codec params from decoder
        v_enc_ctx->width = vst->codecpar->width;
        v_enc_ctx->height = vst->codecpar->height;
        v_enc_ctx->pix_fmt = AVPixelFormat(vst->codecpar->format);
        v_enc_ctx->field_order = vst->codecpar->field_order;
        v_enc_ctx->sample_aspect_ratio = vst->codecpar->sample_aspect_ratio;
        v_enc_ctx->time_base = vst->time_base;
        v_enc_ctx->color_range = vst->codecpar->color_range;
        v_enc_ctx->color_primaries = vst->codecpar->color_primaries;
        v_enc_ctx->color_trc = vst->codecpar->color_trc;
        v_enc_ctx->colorspace = v_decoder.context()->colorspace;
        v_enc_ctx->chroma_sample_location = vst->codecpar->chroma_location;
        // set encoder options
        if ((ret = av_opt_set(v_enc_ctx->priv_data, "preset", "slow", 0)) < 0) {
            std::cerr << "failed set preset for x265 : " << av_err2stdstring(ret) << std::endl;
            return ret;
        };
        if ((ret = av_opt_set_int(v_enc_ctx->priv_data, "crf", 23, 0)) < 0) {
            std::cerr << "failed set crf for x265 : " << av_err2stdstring(ret) << std::endl;
            return ret;
        }
        m_encoders.emplace_back();
        auto& v_encoder = m_encoders.back();
        if ((ret = v_encoder.init(v_enc_ctx, nullptr, x265, nullptr)) != 0) {
            return ret;
        }
        int vst_enc_id;
        if ((vst_enc_id = m_muxer.add_stream(v_encoder)) < 0) {
            return vst_enc_id;
        }
        m_demuxer.set_stream_packet_callback(vst_id, [this](AVPacket* pkt)->int{
            return this->m_decoders[0].decode(pkt);
        });
        v_decoder.set_frame_callback([this](AVFrame* frame)->int{
            if (frame != nullptr) {
                frame->pkt_dts = AV_NOPTS_VALUE;
                frame->key_frame = 0;
                frame->pict_type = AV_PICTURE_TYPE_NONE;
                frame->pkt_pos = -1;
                frame->pkt_size = -1;
            }
            return this->m_encoders[0].encode(frame);
        });
        auto v_writer = m_muxer.create_stream_writer(vst_enc_id, false);
        v_encoder.set_packet_callback([v_writer](AVPacket* pkt)->int{
            if (pkt == nullptr) {
                return 0; // we do not want to flush av_interleaved_write_frame immediately when video stream done.
            }
            std::cout << "write video packet at pts " << pkt->pts << " duration " << pkt->duration << std::endl;
            return v_writer(pkt);
        });

        // init audio copy pipeline
        auto ast_id = av_find_best_stream(ifmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (ast_id < 0) {
            std::cerr << "failed find audio stream" << std::endl;
            return AVERROR_STREAM_NOT_FOUND;
        }
        auto ast = ifmt->streams[ast_id];
        int ast_enc_id;
        if ((ast_enc_id = m_muxer.add_stream(ifmt->streams[ast_id]->codecpar)) < 0) {
            return ast_enc_id;
        }
        auto a_writer = m_muxer.create_stream_writer(ast_enc_id);
        m_demuxer.set_stream_packet_callback(ast_id, [a_writer](AVPacket* pkt)->int{
            if (pkt == nullptr) {
                return 0; // we also do not want to flush av_interleaved_write_frame immediately when audio stream done.
            }
            return a_writer(pkt);
        });
        return 0;
    }

    int transcode() {
        int ret = 0;
        if ((ret = m_muxer.write_header()) < 0) {
            return ret;
        };
        while((ret = m_demuxer.demux()) >= 0);
        if (ret == AVERROR_EOF) {
            return m_muxer.write_trailer();
        }
        return ret;
    }

private:
    Demuxer m_demuxer;
    Muxer m_muxer;
    std::vector<Decoder> m_decoders;
    std::vector<Encoder> m_encoders;
};

int main(int argc, char* argv[]) {
    ToyTranscoder transcoder;
    int ret;
    if ((ret = transcoder.init(argv[1], argv[2])) != 0) {
        std::cerr << "failed init transcode pipeline: " << ret << std::endl;
        return ret;
    }
    if ((ret = transcoder.transcode()) != 0) {
        std::cerr << "failed transcode: " << ret << std::endl;
        return ret;
    }
    return 0;
};