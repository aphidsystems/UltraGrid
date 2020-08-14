/**
 * @file   video_rxtx/srt.h
 * @author Martin Pulec     <martin.pulec@cesnet.cz>
 */
/*
 * Copyright (c) 2020 CESNET, z. s. p. o.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, is permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of CESNET nor the names of its contributors may be
 *    used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef VIDEO_RXTX_SRT_H_37B7F258_3A70_4EB1_A5F8_BA88B3D5416D
#define VIDEO_RXTX_SRT_H_37B7F258_3A70_4EB1_A5F8_BA88B3D5416D

#include <condition_variable>
#include <memory> // std::unique_ptr
#include <mutex>
#include <srt/srt.h>
#include <string>
#include <thread>

#include "types.h"
#include "video_rxtx.h"

constexpr const char *SRT_LOOPBACK = "127.1.2.3";

struct video_frame;
struct display;
struct srt_decoder;

class srt_video_rxtx: public video_rxtx {
public:
        srt_video_rxtx(std::map<std::string, param_u> const &);
        ~srt_video_rxtx();
private:
        void send_frame(std::shared_ptr<video_frame>) override;
        void *(*get_receiver_thread())(void *arg) override {
                return receiver_thread;
        }
        void listen();
        void connect(std::string receiver, uint16_t tx_port);

        static void *receiver_thread(void *arg) {
                auto *s = static_cast<srt_video_rxtx *>(arg);
                s->receiver_loop();
                return nullptr;
        }
        static void cancel_receiver(void *arg);
        void receiver_loop();

        struct display *m_display_device;

        SRTSOCKET     m_socket_listen{};
        SRTSOCKET     m_socket_rx{};
        SRTSOCKET     m_socket_tx{};

        std::thread m_listener_thread;
        std::thread m_caller_thread;

        std::mutex m_lock;
        std::condition_variable m_connected_cv;

        bool m_should_exit{false};

        std::unique_ptr<srt_decoder> m_decoder;
};

#endif // defined VIDEO_RXTX_SRT_H_37B7F258_3A70_4EB1_A5F8_BA88B3D5416D

