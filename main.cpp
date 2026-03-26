#include <iostream>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
}

using namespace std;

int main(int argc, char **argv) {
    if (argc < 3) {
        cerr << "Usage: " << argv[0] << " <input_h264.mp4> <output_h265.mp4>" << endl;
        return -1;
    }

    const char* in_filename = argv[1];
    const char* out_filename = argv[2];

    int ret;
    AVFormatContext* ifmt_ctx = nullptr;
    AVFormatContext* ofmt_ctx = nullptr;
    AVCodecContext* dec_ctx = nullptr;
    AVCodecContext* enc_ctx = nullptr;

    // 1. Open input file
    if ((ret = avformat_open_input(&ifmt_ctx, in_filename, nullptr, nullptr)) < 0) {
        cerr << "Could not open input file: " << in_filename << endl;
        return -1;
    }

    if ((ret = avformat_find_stream_info(ifmt_ctx, nullptr)) < 0) {
        cerr << "Failed to retrieve input stream information." << endl;
        return -1;
    }

    // 2. Find video stream and setup decoder
    int video_stream_index = -1;
    const AVCodec* decoder = nullptr;
    // 强制类型转换 (AVCodec**)，用来兼容 Ubuntu 22.04 自带的 FFmpeg 4.x 旧版本 API
    video_stream_index = av_find_best_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, (AVCodec **)&decoder, 0);
    if (video_stream_index < 0) {
        cerr << "Could not find video stream in input file." << endl;
        return -1;
    }

    AVStream* in_stream = ifmt_ctx->streams[video_stream_index];
    dec_ctx = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(dec_ctx, in_stream->codecpar);

    // Open decoder
    if ((ret = avcodec_open2(dec_ctx, decoder, nullptr)) < 0) {
        cerr << "Failed to open decoder." << endl;
        return -1;
    }

    // 3. Setup output file and encoder
    avformat_alloc_output_context2(&ofmt_ctx, nullptr, nullptr, out_filename);
    if (!ofmt_ctx) {
        cerr << "Could not create output context." << endl;
        return -1;
    }

    const AVCodec* encoder = avcodec_find_encoder_by_name("libx265");
    if (!encoder) {
        cerr << "Necessary encoder (libx265) not found. Make sure ffmpeg is compiled with libx265." << endl;
        return -1;
    }

    AVStream* out_stream = avformat_new_stream(ofmt_ctx, nullptr);
    if (!out_stream) {
        cerr << "Failed allocating output stream." << endl;
        return -1;
    }

    enc_ctx = avcodec_alloc_context3(encoder);
    enc_ctx->height = dec_ctx->height;
    enc_ctx->width = dec_ctx->width;
    enc_ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;

    // Set appropriate pixel format
    if (encoder->pix_fmts)
        enc_ctx->pix_fmt = encoder->pix_fmts[0];
    else
        enc_ctx->pix_fmt = dec_ctx->pix_fmt;

    // 猜测正确的帧率，部分输入文件里 decoder 默认的 framerate 未正确初始化
    AVRational framerate = av_guess_frame_rate(ifmt_ctx, in_stream, nullptr);
    if (framerate.num == 0 || framerate.den == 0) {
        framerate = {25, 1}; // 如果解析不到帧率，兜底设置 25 fps
    }
    enc_ctx->framerate = framerate;
    enc_ctx->time_base = av_inv_q(framerate);
    
    out_stream->time_base = enc_ctx->time_base;

    // Optional: Pass x265 specific parameters
    av_opt_set(enc_ctx->priv_data, "preset", "medium", 0);
    av_opt_set(enc_ctx->priv_data, "crf", "28", 0);

    // Some formats require global headers (like mp4)
    if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // Open encoder
    if ((ret = avcodec_open2(enc_ctx, encoder, nullptr)) < 0) {
        cerr << "Cannot open video encoder." << endl;
        return -1;
    }

    avcodec_parameters_from_context(out_stream->codecpar, enc_ctx);

    // Open output file IO
    if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if ((ret = avio_open(&ofmt_ctx->pb, out_filename, AVIO_FLAG_WRITE)) < 0) {
            cerr << "Could not open output file: " << out_filename << endl;
            return -1;
        }
    }

    // Write file header
    if ((ret = avformat_write_header(ofmt_ctx, nullptr)) < 0) {
        cerr << "Error occurred when writing output file header." << endl;
        return -1;
    }

    // 4. Processing Loop (Read, Decode, Encode, Write)
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVPacket* enc_pkt = av_packet_alloc();

    cout << "Starting transcoding processing loop..." << endl;

    while (av_read_frame(ifmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == video_stream_index) {
            
            // Send packet to decoder
            ret = avcodec_send_packet(dec_ctx, pkt);
            if (ret < 0) {
                cerr << "Error sending a packet for decoding" << endl;
                break;
            }

            while (ret >= 0) {
                ret = avcodec_receive_frame(dec_ctx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    cerr << "Error during decoding" << endl;
                    break;
                }

                // Give the decoded frame a presentation timestamp and rescale it to the encoder's scale
                // 致命 Bug 修复：解码出来的 timestamp 单位原来是针对 mp4 输入流的 (如 1/90000)
                // 必须严格转换到 x265 编码器的刻度 (如 1/25)，否则输入进去的数字会让由于刻度不同被放大成千上万倍，导致画面疯跑！
                frame->pts = av_rescale_q(frame->best_effort_timestamp, in_stream->time_base, enc_ctx->time_base);
                cout << "[Decode] in_stream best_effort_ts: " << frame->best_effort_timestamp 
                     << " -> rescaled enc_ctx frame->pts: " << frame->pts << endl;

                // Send frame to encoder
                ret = avcodec_send_frame(enc_ctx, frame);
                if (ret < 0) {
                    cerr << "Error sending a frame for encoding" << endl;
                    break;
                }

                while (ret >= 0) {
                    ret = avcodec_receive_packet(enc_ctx, enc_pkt);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                        break;
                    } else if (ret < 0) {
                        cerr << "Error during encoding" << endl;
                        break;
                    }

                    // Rescale timestamps from encoder timebase to output stream timebase
                    av_packet_rescale_ts(enc_pkt, enc_ctx->time_base, out_stream->time_base);
                    enc_pkt->stream_index = out_stream->index;
                    cout << "  [Encode Return] enc_pkt rescaled to output container pts: " << enc_pkt->pts << endl;

                    // Write to output file
                    av_interleaved_write_frame(ofmt_ctx, enc_pkt);
                    av_packet_unref(enc_pkt);
                }
                av_frame_unref(frame);
            }
        }
        av_packet_unref(pkt);
    }

    cout << "Flushing buffers..." << endl;

    // 5. Flushing encoders and decoders
    // Flush decoder
    avcodec_send_packet(dec_ctx, nullptr);
    while (avcodec_receive_frame(dec_ctx, frame) >= 0) {
        frame->pts = av_rescale_q(frame->best_effort_timestamp, in_stream->time_base, enc_ctx->time_base);
        cout << "[Flush Decode] best_effort_ts: " << frame->best_effort_timestamp 
             << " -> rescaled frame->pts: " << frame->pts << endl;
             
        avcodec_send_frame(enc_ctx, frame);
        while (avcodec_receive_packet(enc_ctx, enc_pkt) >= 0) {
            av_packet_rescale_ts(enc_pkt, enc_ctx->time_base, out_stream->time_base);
            enc_pkt->stream_index = out_stream->index;
            cout << "  [Flush Encode Return] enc_pkt->pts rescaled: " << enc_pkt->pts << endl;
            av_interleaved_write_frame(ofmt_ctx, enc_pkt);
            av_packet_unref(enc_pkt);
        }
        av_frame_unref(frame);
    }

    // Flush encoder
    avcodec_send_frame(enc_ctx, nullptr);
    while (avcodec_receive_packet(enc_ctx, enc_pkt) >= 0) {
        av_packet_rescale_ts(enc_pkt, enc_ctx->time_base, out_stream->time_base);
        enc_pkt->stream_index = out_stream->index;
        av_interleaved_write_frame(ofmt_ctx, enc_pkt);
        av_packet_unref(enc_pkt);
    }

    // Write file trailer
    av_write_trailer(ofmt_ctx);

    // Cleanup
    av_packet_free(&pkt);
    av_frame_free(&frame);
    av_packet_free(&enc_pkt);

    avcodec_free_context(&dec_ctx);
    avcodec_free_context(&enc_ctx);
    if (ofmt_ctx && !(ofmt_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&ofmt_ctx->pb);
    avformat_free_context(ofmt_ctx);
    avformat_close_input(&ifmt_ctx);

    cout << "Transcoding finished successfully!" << endl;
    return 0;
}
